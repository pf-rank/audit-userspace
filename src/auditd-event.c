/* auditd-event.c --
 * Copyright 2004-08,2011,2013,2015-16,2018,2021 Red Hat Inc.
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Authors:
 *   Steve Grubb <sgrubb@redhat.com>
 *
 */

#include "config.h"
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <fcntl.h>	/* O_NOFOLLOW needs gnu defined */
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/vfs.h>
#include <ctype.h>	/* toupper */
#include <libgen.h>	/* dirname */
#include "auditd-event.h"
#include "auditd-dispatch.h"
#include "auditd-listen.h"
#include "libaudit.h"
#include "private.h"
#include "auparse.h"
#include "auparse-idata.h"
#include "common.h"

/* This is defined in auditd.c */
#ifdef HAVE_ATOMIC
extern ATOMIC_INT stop;
#else
extern volatile ATOMIC_INT stop;
#endif

extern void update_report_timer(unsigned int interval);
/*
 * This function is provided by auditd.c and marked weak so test utilities
 * that don't link auditd.c can still link this file.
 */
extern int event_is_prealloc(struct auditd_event *e) __attribute__((weak));

/* Local function prototypes */
static void send_ack(const struct auditd_event *e, int ack_type,
			const char *msg);
static void write_to_log(const struct auditd_event *e);
static void check_log_file_size(void);
static void check_space_left(void);
static void do_space_left_action(int admin);
static void do_disk_full_action(void);
static void do_disk_error_action(const char *func, int err);
static void fix_disk_permissions(void);
static void check_excess_logs(void);
static void rotate_logs_now(void);
static void rotate_logs(unsigned int num_logs, unsigned int keep_logs);
static void shift_logs(void);
static int  open_audit_log(void);
static pid_t safe_exec(const char *exe);
static void reconfigure(struct auditd_event *e);
static void init_flush_thread(void);

/* Local Data */
static struct daemon_conf *config;
static volatile int log_fd;
static FILE *log_file = NULL;
static unsigned int disk_err_warning = 0;
static int fs_space_warning = 0;
static int fs_admin_space_warning = 0;
static int fs_space_left = 1;
static int logging_suspended = 0;
static unsigned int known_logs = 0;
static pid_t exec_child_pid = -1;
static char *format_buf = NULL;
static off_t log_size = 0;
static pthread_t flush_thread;
static pthread_mutex_t flush_lock;
static pthread_cond_t do_flush;
static volatile int flush;
static auparse_state_t *au = NULL;

/* Local definitions */
#define MIN_SPACE_LEFT 24

static inline int from_network(const struct auditd_event *e)
{ if (e && e->ack_func) return 1; return 0; }

int dispatch_network_events(void)
{
	return config->distribute_network_events;
}

pid_t auditd_get_exec_pid(void)
{
       return exec_child_pid;
}

void auditd_clear_exec_pid(void)
{
       exec_child_pid = -1;
}

void write_logging_state(FILE *f)
{
	fprintf(f, "writing to logs = %s\n", config->write_logs ? "yes" : "no");
	if (config->daemonize == D_BACKGROUND && config->write_logs) {
		int rc;
		struct statfs buf;

		fprintf(f, "current log size = %llu KiB\n",
			(long long unsigned)log_size/1024);
		fprintf(f, "max log size = %lu KiB\n",
				config->max_log_size * (MEGABYTE/1024));
		fprintf(f,"logs detected last rotate/shift = %u\n", known_logs);
		fprintf(f, "space left on partition = %s\n",
					fs_space_left ? "yes" : "no");
		rc = fstatfs(log_fd, &buf);
		if (rc == 0) {
			fprintf(f, "Logging partition free space = %llu MiB\n",
				(long long unsigned)
				(buf.f_bavail * buf.f_bsize)/MEGABYTE);
			fprintf(f, "space_left setting = %lu MiB\n",
				config->space_left);
			fprintf(f, "admin_space_left setting = %lu MiB\n",
				config->admin_space_left);
		}
		fprintf(f, "logging suspended = %s\n",
					logging_suspended ? "yes" : "no");
		fprintf(f, "file system space action performed = %s\n",
					fs_space_warning ? "yes" : "no");
		fprintf(f, "admin space action performed = %s\n",
					fs_admin_space_warning ? "yes" : "no");
		fprintf(f, "disk error detected = %s\n",
					disk_err_warning ? "yes" : "no");
	}
}

void shutdown_events(void)
{
	// We are no longer processing events, sync the disk and close up.
	pthread_cancel(flush_thread);
	free((void *)format_buf);
	auparse_destroy_ext(au, AUPARSE_DESTROY_ALL);
	if (log_fd >= 0)
		fsync(log_fd);
	if (log_file)
		fclose(log_file);
}

int init_event(struct daemon_conf *conf)
{
	/* Store the netlink descriptor and config info away */
	config = conf;
	log_fd = -1;

	/* Now open the log */
	if (config->daemonize == D_BACKGROUND) {
		fix_disk_permissions();
		if (open_audit_log())
			return 1;
		setup_percentages(config, log_fd);
	} else {
		log_fd = 1; // stdout
		log_file = fdopen(log_fd, "a");
		if (log_file == NULL) {
			audit_msg(LOG_ERR,
				"Error setting up stdout descriptor (%s)",
				strerror(errno));
			return 1;
		}
		/* Set it to line buffering */
		setlinebuf(log_file);
	}

	if (config->daemonize == D_BACKGROUND) {
		check_log_file_size();
		check_excess_logs();
		/* At this stage, auditd is not fully initialized and operational.
		 This means we can't notify the parent process that initialization
		 is complete. However, if space_left_action is set to SINGLE, we must
		 avoid switching to that runlevel. Before entering the SINGLE
		 runlevel requires auditd to finish initialization. But auditd will not
		 start properly or signal the init system that it has started, as it is
		 blocked by the attempt to switch to single-user mode, resulting in a
		 deadlock. */
		// check_space_left();
	}
	format_buf = (char *)malloc(FORMAT_BUF_LEN);
	if (format_buf == NULL) {
		audit_msg(LOG_ERR, "No memory for formatting, exiting");
		if (log_file)
			fclose(log_file);
		log_file = NULL;
		return 1;
	}
	init_flush_thread();
	return 0;
}

/* This tells the OS that pending writes need to get going.
 * Its only used when flush == incremental_async. */
static void *flush_thread_main(void *arg)
{
	sigset_t sigs;

	/* This is a worker thread. Don't handle signals. */
	sigemptyset(&sigs);
	sigaddset(&sigs, SIGTERM);
	sigaddset(&sigs, SIGHUP);
	sigaddset(&sigs, SIGUSR1);
	sigaddset(&sigs, SIGUSR2);
	sigaddset(&sigs, SIGCHLD);
	sigaddset(&sigs, SIGCONT);
	pthread_sigmask(SIG_SETMASK, &sigs, NULL);

	while (!AUDIT_ATOMIC_LOAD(stop)) {
		pthread_mutex_lock(&flush_lock);

		// In the event that the logging thread requests another
		// flush before the first completes, this simply turns
		// into a loop of fsyncs.
		while (flush == 0) {
			pthread_cond_wait(&do_flush, &flush_lock);
			if (AUDIT_ATOMIC_LOAD(stop)) {
				pthread_mutex_unlock(&flush_lock);
				return NULL;
			}
		}
		flush = 0;
		pthread_mutex_unlock(&flush_lock);

		if (log_fd >= 0)
			fsync(log_fd);
	}
	return NULL;
}

/* We setup the flush thread no matter what. This is incase a reconfig
 * changes from non incremental to incremental or vice versa. */
static void init_flush_thread(void)
{
	pthread_mutex_init(&flush_lock, NULL);
	pthread_cond_init(&do_flush, NULL);
	flush = 0;
	pthread_create(&flush_thread, NULL, flush_thread_main, NULL);
	pthread_detach(flush_thread);
}

static void replace_event_msg(struct auditd_event *e, const char *buf)
{
	if (buf) {
		size_t len = strlen(buf);

		if (e->reply.message != e->reply.msg.data)
			free((void *)e->reply.message);

		if (len < MAX_AUDIT_MESSAGE_LENGTH - 1)
			e->reply.message = strdup(buf);
		else {
			// If too big, we must truncate the event due to API
			e->reply.message = strndup(buf,
						  MAX_AUDIT_MESSAGE_LENGTH-1);
			len = MAX_AUDIT_MESSAGE_LENGTH;
		}
		// For network originating events, len should be used
		if (!from_network(e)) // V1 protocol msg size
			e->reply.msg.nlh.nlmsg_len = e->reply.len;
		e->reply.len = len; // V2 protocol msg size
	}
}

/*
* This function will take an audit structure and return a
* text buffer that's formatted for writing to disk. If there is
* an error the return value is 0 and the format_buf is truncated.
* format_buf will have any '\n' removed on return.
*/
static int format_raw(const struct audit_reply *rep)
{
	char *ptr;
	int nlen;

	format_buf[0] = 0;

	if (rep == NULL) {
		if (config->node_name_format != N_NONE)
			nlen = snprintf(format_buf, FORMAT_BUF_LEN - 32,
		"node=%s type=DAEMON_ERR op=format-raw msg=NULL res=failed",
                                config->node_name);
		else
			nlen = snprintf(format_buf, MAX_AUDIT_MESSAGE_LENGTH,
			  "type=DAEMON_ERR op=format-raw msg=NULL res=failed");

		if (nlen < 1)
			return 0;
	} else {
		int len;
		const char *type, *message;
		char unknown[32];
		type = audit_msg_type_to_name(rep->type);
		if (type == NULL) {
			snprintf(unknown, sizeof(unknown),
				"UNKNOWN[%d]", rep->type);
			type = unknown;
		}
		if (rep->message == NULL) {
			message = "lost";
			len = 4;
		} else {
			message = rep->message;
			len = rep->len;
		}

		// Note: This can truncate messages if
		// MAX_AUDIT_MESSAGE_LENGTH is too small
		if (config->node_name_format != N_NONE)
			nlen = snprintf(format_buf, FORMAT_BUF_LEN - 32,
				"node=%s type=%s msg=%.*s",
                                config->node_name, type, len, message);
		else
		        nlen = snprintf(format_buf,
				MAX_AUDIT_MESSAGE_LENGTH - 32,
				"type=%s msg=%.*s", type, len, message);

		if (nlen < 1)
			return 0;

	        /* Replace \n with space so it looks nicer. */
		ptr = format_buf;
	        while (*ptr) {
			if (*ptr == '\n')
			        *ptr = ' ';
			ptr++;
		}

		/* Trim trailing space off since it wastes space */
		if (format_buf[nlen-1] == ' ') {
			format_buf[nlen-1] = 0;
			nlen--;
		}
	}
        return nlen;
}

static int sep_done = 0;
static int add_separator(unsigned int len_left)
{
	if (sep_done == 0) {
		format_buf[FORMAT_BUF_LEN - len_left] = AUDIT_INTERP_SEPARATOR;
		sep_done++;
		return 1;
	}
	sep_done++;
	return 0;
}

// returns length used, 0 on error
#define NAME_SIZE 64
static int add_simple_field(auparse_state_t *au, size_t len_left, int encode)
{
	const char *value, *nptr;
	char *enc = NULL;
	char *ptr, field_name[NAME_SIZE];
	size_t nlen, vlen, tlen;
	unsigned int i;
	int num;

	// prepare field name
	i = 0;
	nptr = auparse_get_field_name(au);
	while (*nptr && i < (NAME_SIZE - 1)) {
		field_name[i] = toupper(*nptr);
		i++;
		nptr++;
	}
	field_name[i] = 0;
	nlen = i;

	// get the translated value
	value = auparse_interpret_field(au);
	if (value == NULL)
		value = "?";
	vlen = strlen(value);

	if (encode) {
		enc = audit_encode_nv_string(field_name, value, vlen);
		if (enc == NULL)
			return 0;
		vlen = strlen(enc);
		tlen = 1 + vlen + 1;
	} else
		// calculate length to use
		tlen = 1 + nlen + 1 + vlen + 1;

	// If no room, do not truncate - just do nothing
	if (tlen >= len_left) {
		free(enc);
		return 0;
	}

	// Setup pointer
	ptr = &format_buf[FORMAT_BUF_LEN - len_left];
	if (sep_done > 1) {
		*ptr = ' ';
		ptr++;
		num = 1;
	} else
		num = 0;

	// Add the field
	if (encode) {	// encoded: "%s"
		memcpy(ptr, enc, vlen);
		ptr[vlen] = 0;
		num += vlen;
		free(enc);
	} else {	// plain: "%s=%s"
		memcpy(ptr, field_name, nlen);
		ptr += nlen;
		*ptr++ = '=';
		memcpy(ptr, value, vlen);
		ptr[vlen] = 0;
		num += nlen + 1 + vlen;
	}
	return num;
}

/*
* This function will take an audit structure and return a text
* buffer that's formatted and enriched. If there is an error the
* return value is the raw formatted buffer (which may be truncated if it
* had an error)or an error message in the format_buffer. The return
* value is never NULL.
*/
static const char *format_enrich(const struct audit_reply *rep)
{
        if (rep == NULL) {
		if (config->node_name_format != N_NONE)
			snprintf(format_buf, FORMAT_BUF_LEN - 32,
	    "node=%s type=DAEMON_ERR op=format-enriched msg=NULL res=failed",
                                config->node_name);
		else
			snprintf(format_buf, MAX_AUDIT_MESSAGE_LENGTH,
		    "type=DAEMON_ERR op=format-enriched msg=NULL res=failed");
	} else {
		int rc, rtype;
		size_t mlen, len;

		// Do raw format to get event started
		mlen = format_raw(rep);

		// How much room is left?
		len = FORMAT_BUF_LEN - mlen;
		if (len <= MIN_SPACE_LEFT)
			return format_buf;

		// Add carriage return so auparse sees it correctly
		format_buf[mlen] = 0x0A;
		format_buf[mlen+1] = 0;
		mlen++;	// Increase the length so auparse copies the '\n'

		// init auparse
		if (au == NULL) {
			au = auparse_init(AUSOURCE_BUFFER, format_buf);
			if (au == NULL) {
				format_buf[mlen-1] = 0; //remove newline
				return format_buf;
			}

			auparse_set_escape_mode(au, AUPARSE_ESC_RAW);
			auparse_set_eoe_timeout(config->end_of_event_timeout);
		} else
			auparse_new_buffer(au, format_buf, mlen);

		sep_done = 0;

		// Loop over all fields while possible to add field
		rc = auparse_first_record(au);
		if (rc != 1)
			format_buf[mlen-1] = 0; //remove newline on failure

		rtype = auparse_get_type(au);
		switch (rtype)
		{	// Flush before adding to pickup new associations
			case AUDIT_ADD_USER:
			case AUDIT_ADD_GROUP:
				_auparse_flush_caches(au);
				break;
			default:
				break;
		}

		while (rc > 0 && len > MIN_SPACE_LEFT) {
			// See what kind of field we have
			size_t vlen;
			int type = auparse_get_field_type(au);
			switch (type)
			{
				case AUPARSE_TYPE_UID:
				case AUPARSE_TYPE_GID:
					if (add_separator(len))
						len--;
					vlen = add_simple_field(au, len, 1);
					len -= vlen;
					break;
				case AUPARSE_TYPE_SYSCALL:
				case AUPARSE_TYPE_ARCH:
				case AUPARSE_TYPE_SOCKADDR:
					if (add_separator(len))
						len--;
					vlen = add_simple_field(au, len, 0);
					len -= vlen;
					break;
				default:
					break;
			}
			rc = auparse_next_field(au);
			//remove newline when nothing added
			if (rc < 1 && sep_done == 0)
				format_buf[mlen-1] = 0;
		}

		switch(rtype)
		{	// Flush after modification to remove stale entries
			case AUDIT_USER_MGMT:
			case AUDIT_DEL_USER:
			case AUDIT_DEL_GROUP:
			case AUDIT_GRP_MGMT:
				_auparse_flush_caches(au);
				break;
			default:
				break;
		}
	}

        return format_buf;
}

void format_event(struct auditd_event *e)
{
	const char *buf;

	switch (config->log_format)
	{
		case LF_RAW:
			format_raw(&e->reply);
			buf = format_buf;
			break;
		case LF_ENRICHED:
			buf = format_enrich(&e->reply);
			break;
		default:
			buf = NULL;
			break;
	}

	replace_event_msg(e, buf);
}

/* This function free's all memory associated with events */
void cleanup_event(struct auditd_event *e)
{
	// Over in send_audit_event we sometimes have message pointing
	// into the middle of the reply allocation. Check for it.
	if (e->reply.message != e->reply.msg.data)
		free((void *)e->reply.message);
	if (!event_is_prealloc || !event_is_prealloc(e))
		free(e);
}

/* This function takes a  reconfig event and sends it to the handler */
void enqueue_event(struct auditd_event *e)
{
	e->ack_func = NULL;
	e->ack_data = NULL;
	e->sequence_id = 0;

        handle_event(e);
	cleanup_event(e);
}

/* This function allocates memory and fills the event fields with
   passed arguments. Caller must free memory. */
struct auditd_event *create_event(const char *msg, ack_func_type ack_func,
	 void *ack_data, uint32_t sequence_id)
{
	struct auditd_event *e;

	e = (struct auditd_event *)calloc(1, sizeof (*e));
	if (e == NULL) {
		audit_msg(LOG_ERR, "Cannot allocate audit reply");
		return NULL;
	}

	e->ack_func = ack_func;
	e->ack_data = ack_data;
	e->sequence_id = sequence_id;

	/* Network originating events need things adjusted to mimic netlink. */
	if (from_network(e))
		replace_event_msg(e, msg);

	return e;
}

/* This function takes the event and handles it. */
static unsigned int count = 0L;
void handle_event(struct auditd_event *e)
{
	if (e->reply.type == AUDIT_DAEMON_RECONFIG && e->ack_func == NULL) {
		reconfigure(e);
		if (config->write_logs == 0 && config->daemonize == D_BACKGROUND)
                        return;
                format_event(e);
	} else if (e->reply.type == AUDIT_DAEMON_ROTATE) {
		rotate_logs_now();
		if (config->write_logs == 0 && config->daemonize == D_BACKGROUND)
			return;
	}
	if (!logging_suspended && (config->write_logs ||
					config->daemonize == D_FOREGROUND)) {
		write_to_log(e);

		/* See if we need to flush to disk manually */
		if (config->flush == FT_INCREMENTAL ||
			config->flush == FT_INCREMENTAL_ASYNC) {
			count++;
			if ((count % config->freq) == 0) {
				int rc;
				errno = 0;
				do {
					rc = fflush_unlocked(log_file);
				} while (rc < 0 && errno == EINTR);
		                if (errno) {
					if (errno == ENOSPC &&
					     fs_space_left == 1) {
					     fs_space_left = 0;
					     do_disk_full_action();
				        } else
					     //EIO is only likely failure mode
					     do_disk_error_action("flush",
						errno);
				}

				if (config->daemonize == D_BACKGROUND) {
					if (config->flush == FT_INCREMENTAL) {
						/* EIO is only likely failure */
						if (log_fd >= 0 &&
							fsync(log_fd) != 0) {
						     do_disk_error_action(
							"fsync",
							errno);
						}
					} else {
						pthread_mutex_lock(&flush_lock);
						flush = 1;
						pthread_cond_signal(&do_flush);
						pthread_mutex_unlock(
								   &flush_lock);
					}
				}
			}
		}
	} else if (!config->write_logs && config->daemonize == D_BACKGROUND)
		send_ack(e, AUDIT_RMW_TYPE_ACK, "");
	else if (logging_suspended)
		send_ack(e,AUDIT_RMW_TYPE_DISKERROR,"remote logging suspended");
}

static void send_ack(const struct auditd_event *e, int ack_type,
			const char *msg)
{
	if (from_network(e)) {
		unsigned char header[AUDIT_RMW_HEADER_SIZE];

		AUDIT_RMW_PACK_HEADER(header, 0, ack_type, strlen(msg),
					e->sequence_id);

		e->ack_func(e->ack_data, header, msg);
	}
}

void resume_logging(void)
{
	audit_msg(LOG_NOTICE, "Audit daemon is attempting to resume logging.");
	logging_suspended = 0;
	fs_space_left = 1;

	// User space action scripts cause fd to close
	// Need to reopen here to recreate the file if the
	// script deleted or moved it.
	if (log_file == NULL) {
		fix_disk_permissions();
		if (open_audit_log()) {
			int saved_errno = errno;
			audit_msg(LOG_WARNING,
				"Could not reopen a log after resume logging");
			logging_suspended = 1;
			do_disk_error_action("resume", saved_errno);
		} else
			check_log_file_size();
		audit_msg(LOG_NOTICE, "Audit daemon resumed logging.");
	}
	disk_err_warning = 0;
	fs_space_warning = 0;
	fs_admin_space_warning = 0;
}

/* This function writes the given buf to the current log file */
static void write_to_log(const struct auditd_event *e)
{
	int rc;
	int ack_type = AUDIT_RMW_TYPE_ACK;
	const char *msg = "";

	/* write it to disk */
	rc = fprintf(log_file, "%s\n", e->reply.message);

	/* error? Handle it */
	if (rc < 0) {
		if (errno == ENOSPC) {
			ack_type = AUDIT_RMW_TYPE_DISKFULL;
			msg = "disk full";
			send_ack(e, ack_type, msg);
			if (fs_space_left == 1) {
				fs_space_left = 0;
				do_disk_full_action();
			}
		} else  {
			int saved_errno = errno;
			ack_type = AUDIT_RMW_TYPE_DISKERROR;
			msg = "disk write error";
			send_ack(e, ack_type, msg);
			do_disk_error_action("write", saved_errno);
		}
	} else {
		/* check log file size & space left on partition */
		if (config->daemonize == D_BACKGROUND) {
			// If either of these fail, I consider it an
			// inconvenience as opposed to something that is
			// actionable. There may be some temporary condition
			// that the system recovers from. The real error
			// occurs on write.
			log_size += rc;
			check_log_file_size();
			// Keep loose tabs on the free space
			if ((log_size % 8) < 3)
				check_space_left();
		}

		if (fs_space_warning)
			ack_type = AUDIT_RMW_TYPE_DISKLOW;
		send_ack(e, ack_type, msg);
		disk_err_warning = 0;
	}
}

static void check_log_file_size(void)
{
	/* did we cross the size limit? */
	off_t sz = log_size / MEGABYTE;

	if (config->write_logs == 0)
		return;

	if (sz >= config->max_log_size && (config->daemonize == D_BACKGROUND)) {
		switch (config->max_log_size_action)
		{
			case SZ_IGNORE:
				break;
			case SZ_SYSLOG:
				audit_msg(LOG_ERR,
			    "Audit daemon log file is larger than max size");
				break;
			case SZ_EXEC:
				if (log_file)
					fclose(log_file);
				log_file = NULL;
				log_fd = -1;
				logging_suspended = 1;
				exec_child_pid =
					safe_exec(config->max_log_file_exe);
				break;
			case SZ_SUSPEND:
				audit_msg(LOG_ERR,
		    "Audit daemon is suspending logging due to logfile size.");
				// We need to close the file so that manual
				// intervention can move or delete the file.
				// We don't want to keep logging to a deleted
				// file.
				if (log_file)
					fclose(log_file);
				log_file = NULL;
				log_fd = -1;
				logging_suspended = 1;
				break;
			case SZ_ROTATE:
				if (config->num_logs > 1) {
					audit_msg(LOG_INFO,
					    "Audit daemon rotating log files");
					rotate_logs(0, 0);
				}
				break;
			case SZ_KEEP_LOGS:
				audit_msg(LOG_INFO,
			    "Audit daemon rotating log files with keep option");
					shift_logs();
				break;
			default:
				audit_msg(LOG_ALERT,
  "Audit daemon log file is larger than max size and unknown action requested");
				break;
		}
	}
}

static void check_space_left(void)
{
	int rc;
	struct statfs buf;

	if (log_fd < 0)
		return;

        rc = fstatfs(log_fd, &buf);
        if (rc == 0) {
		if (buf.f_bavail < 5) {
			/* we won't consume the last 5 blocks */
			fs_space_left = 0;
			do_disk_full_action();
		} else {
			unsigned long blocks;
			unsigned long block_size = buf.f_bsize;
		        blocks = config->space_left * (MEGABYTE/block_size);
			if (buf.f_bavail < blocks) {
				if (fs_space_warning == 0) {
					do_space_left_action(0);
					// Allow unlimited rotation
					if (config->space_left_action !=
								FA_ROTATE)
						fs_space_warning = 1;
				}
			} else if (fs_space_warning &&
					config->space_left_action == FA_SYSLOG){
				// Auto reset only if failure action is syslog
				fs_space_warning = 0;
			}
		        blocks=config->admin_space_left * (MEGABYTE/block_size);
			if (buf.f_bavail < blocks) {
				if (fs_admin_space_warning == 0) {
					do_space_left_action(1);
					// Allow unlimited rotation
					if (config->admin_space_left_action !=
								FA_ROTATE)
						fs_admin_space_warning = 1;
				}
			} else if (fs_admin_space_warning &&
				config->admin_space_left_action == FA_SYSLOG) {
				// Auto reset only if failure action is syslog
				fs_admin_space_warning = 0;
			}
		}
	}
	else audit_msg(LOG_DEBUG, "fstatfs returned:%d, %s", rc,
			strerror(errno));
}

extern int sendmail(const char *subject, const char *content,
	const char *mail_acct);
static void do_space_left_action(int admin)
{
	int action;
	char buffer[256];
	const char *next_actions;

	// Select the appropriate action and generate a meaningful message
	// explaining what happens if disk space reaches a threshold or
	// becomes completely full.
	if (admin) {
		action = config->admin_space_left_action;

		snprintf(buffer, sizeof(buffer),
			"If the disk becomes full, audit will %s.",  failure_action_to_str(config->disk_full_action));
	}
	else {
		action = config->space_left_action;

		snprintf(buffer, sizeof(buffer),
			"If the admin space left threshold is reached, audit will %s. "
			"If the disk becomes full, audit will %s.",
			failure_action_to_str(config->admin_space_left_action),
			failure_action_to_str(config->disk_full_action));
	}
	next_actions = buffer;

	// If space_left is reached and FA_HALT is set in any of these fields
	// we need to inform logged in users.
	if (config->admin_space_left_action == FA_HALT ||
		config->disk_full_action == FA_HALT) {
		wall_message("The audit system is low on disk space and is now halting the system for admin corrective action.");
	}

	switch (action)
	{
		case FA_IGNORE:
			break;
		case FA_SYSLOG:
			audit_msg(LOG_ALERT,
				"Audit daemon is low on disk space for logging. %s", next_actions);
			break;
		case FA_ROTATE:
			if (config->num_logs > 1) {
				audit_msg(LOG_INFO,
					"Audit daemon rotating log files");
				rotate_logs(0, 0);
			}
			break;
		case FA_EMAIL:
		{
			char content[512];
			const char *subject;

			if (admin == 0) {
				subject = "Audit Disk Space Alert";
				snprintf(content, sizeof(content),
					"The audit daemon is low on disk space for logging! Please take action\n"
					"to ensure no loss of service.\n"
					"%s", next_actions);
			} else {
				subject = "Audit Admin Space Alert";
				snprintf(content, sizeof(content),
					"The audit daemon is very low on disk space for logging! Immediate action\n"
					"is required to ensure no loss of service.\n"
					"%s", next_actions);
			}
			sendmail(subject, content, config->action_mail_acct);
			audit_msg(LOG_ALERT, "%s", content);
			break;
		}
		case FA_EXEC:
			// Close the logging file in case the script zips or
			// moves the file. We'll reopen in sigusr2 handler
			if (log_file)
				fclose(log_file);
			log_file = NULL;
			log_fd = -1;
			logging_suspended = 1;
			if (admin)
				safe_exec(config->admin_space_left_exe);
			else
				safe_exec(config->space_left_exe);
			break;
		case FA_SUSPEND:
			audit_msg(LOG_ALERT,
			    "Audit daemon is suspending logging due to low disk space.");
			// We need to close the file so that manual
			// intervention can move or delete the file. We
			// don't want to keep logging to a deleted file.
			if (log_file)
				fclose(log_file);
			log_file = NULL;
			log_fd = -1;
			logging_suspended = 1;
			break;
		case FA_SINGLE:
			audit_msg(LOG_ALERT,
				"The audit daemon is now changing the system to single user mode and exiting due to low disk space");
			change_runlevel(SINGLE);
			AUDIT_ATOMIC_STORE(stop, 1);
			break;
		case FA_HALT:
			// Only available for admin
			audit_msg(LOG_ALERT,
				"The audit daemon is now halting the system and exiting due to low disk space");
			change_runlevel(HALT);
			AUDIT_ATOMIC_STORE(stop, 1);
			break;
		default:
			audit_msg(LOG_ALERT,
			    "Audit daemon is low on disk space for logging and unknown action requested");
			break;
	}
}

static void do_disk_full_action(void)
{
	audit_msg(LOG_ALERT,
			"Audit daemon has no space left on logging partition");
	switch (config->disk_full_action)
	{
		case FA_IGNORE:
		case FA_SYSLOG: /* Message is syslogged above */
			break;
		case FA_ROTATE:
			if (config->num_logs > 1) {
				audit_msg(LOG_INFO,
					"Audit daemon rotating log files");
				rotate_logs(0, 0);
			}
			break;
		case FA_EXEC:
			// Close the logging file in case the script zips or
			// moves the file. We'll reopen in sigusr2 handler
			if (log_file)
				fclose(log_file);
			log_file = NULL;
			log_fd = -1;
			logging_suspended = 1;
			safe_exec(config->disk_full_exe);
			break;
		case FA_SUSPEND:
			audit_msg(LOG_ALERT,
			    "Audit daemon is suspending logging due to no space left on logging partition.");
			// We need to close the file so that manual
			// intervention can move or delete the file. We
			// don't want to keep logging to a deleted file.
			if (log_file)
				fclose(log_file);
			log_file = NULL;
			log_fd = -1;
			logging_suspended = 1;
			break;
		case FA_SINGLE:
			audit_msg(LOG_ALERT,
				"The audit daemon is now changing the system to single user mode and exiting due to no space left on logging partition");
			change_runlevel(SINGLE);
			AUDIT_ATOMIC_STORE(stop, 1);
			break;
		case FA_HALT:
			audit_msg(LOG_ALERT,
				"The audit daemon is now halting the system and exiting due to no space left on logging partition");
			change_runlevel(HALT);
			AUDIT_ATOMIC_STORE(stop, 1);
			break;
		default:
			audit_msg(LOG_ALERT, "Unknown disk full action requested");
			break;
	}
}

static void do_disk_error_action(const char *func, int err)
{
	char text[128];

	switch (config->disk_error_action)
	{
		case FA_IGNORE:
			break;
		case FA_SYSLOG:
			if (disk_err_warning < 5) {
				snprintf(text, sizeof(text),
			    "%s: Audit daemon detected an error writing an event to disk (%s)",
					func, strerror(err));
				audit_msg(LOG_ALERT, "%s", text);
				disk_err_warning++;
			}
			break;
		case FA_EXEC:
			// Close the logging file in case the script zips or
			// moves the file. We'll reopen in sigusr2 handler
			if (log_file)
				fclose(log_file);
			log_file = NULL;
			log_fd = -1;
			logging_suspended = 1;
			safe_exec(config->disk_error_exe);
			break;
		case FA_SUSPEND:
			audit_msg(LOG_ALERT,
			    "Audit daemon is suspending logging due to previously mentioned write error");
			// We need to close the file so that manual
			// intervention can move or delete the file. We
			// don't want to keep logging to a deleted file.
			if (log_file)
				fclose(log_file);
			log_file = NULL;
			log_fd = -1;
			logging_suspended = 1;
			break;
		case FA_SINGLE:
			audit_msg(LOG_ALERT,
				"The audit daemon is now changing the system to single user mode and exiting due to previously mentioned write error");
			change_runlevel(SINGLE);
			AUDIT_ATOMIC_STORE(stop, 1);
			break;
		case FA_HALT:
			audit_msg(LOG_ALERT,
				"The audit daemon is now halting the system and exiting due to previously mentioned write error.");
			change_runlevel(HALT);
			AUDIT_ATOMIC_STORE(stop, 1);
			break;
		default:
			audit_msg(LOG_ALERT,
				"Unknown disk error action requested");
			break;
	}
}

static void rotate_logs_now(void)
{
	/* Don't rotate in debug mode */
	if (config->daemonize == D_FOREGROUND)
		return;
	if (config->max_log_size_action == SZ_KEEP_LOGS)
		shift_logs();
	else
		rotate_logs(0, 0);
}

/* Check for and remove excess logs so that we don't run out of room */
static void check_excess_logs(void)
{
	int rc;
	unsigned int i, len;
	char *name;

	// Only do this if rotate is the log size action
	// and we actually have a limit
	if (config->max_log_size_action != SZ_ROTATE ||
			config->num_logs < 2)
		return;

	len = strlen(config->log_file) + 16;
	name = (char *)malloc(len);
	if (name == NULL) { /* Not fatal - just messy */
		audit_msg(LOG_ERR, "No memory checking excess logs");
		return;
	}

	// We want 1 beyond the normal logs
	i = config->num_logs;
	rc = 0;
	while (rc == 0) {
		snprintf(name, len, "%s.%u", config->log_file, i++);
		rc=unlink(name);
		if (rc == 0)
			audit_msg(LOG_NOTICE,
			    "Log %s removed as it exceeds num_logs parameter",
			     name);
	}
	free(name);
}

static void fix_disk_permissions(void)
{
	char *path, *dir;
	unsigned int i, len;

	if (config == NULL || config->log_file == NULL)
		return;

	len = strlen(config->log_file) + 16;

	path = malloc(len);
	if (path == NULL)
		return;

	// Start with the directory
	strcpy(path, config->log_file);
	dir = dirname(path);
	if (chmod(dir,config->log_group ? S_IRWXU|S_IRGRP|S_IXGRP: S_IRWXU) < 0)
		audit_msg(LOG_WARNING, "Couldn't change access mode of "
			"%s (%s)", dir, strerror(errno));
	if (chown(dir, 0, config->log_group ? config->log_group : 0) < 0)
		audit_msg(LOG_WARNING, "Couldn't change ownership of "
			"%s (%s)", dir, strerror(errno));

	// Now, for each file...
	for (i = 1; i < config->num_logs; i++) {
		int rc;
		snprintf(path, len, "%s.%u", config->log_file, i);
		rc = chmod(path, config->log_group ? S_IRUSR|S_IRGRP : S_IRUSR);
		if (rc && errno == ENOENT)
			break;
	}

	// Now the current file
	chmod(config->log_file, config->log_group ? S_IWUSR|S_IRUSR|S_IRGRP :
			S_IWUSR|S_IRUSR);

	free(path);
}

static void rotate_logs(unsigned int num_logs, unsigned int keep_logs)
{
	int rc, i;
	unsigned int len;
	char *oldname, *newname;

	/* Check that log rotation is enabled in the configuration file. There
	 * is no need to check for max_log_size_action == SZ_ROTATE because
	 * this could be invoked externally by receiving a USR1 signal,
	 * independently on the action parameter. */
	if (config->num_logs < 2 && !keep_logs){
		audit_msg(LOG_NOTICE,
			"Log rotation disabled (num_logs < 2), skipping");
		return;
	}

	/* Close audit file. fchmod and fchown errors are not fatal because we
	 * already adjusted log file permissions and ownership when opening the
	 * log file. */
	if (log_fd >= 0) {
		if (fchmod(log_fd, config->log_group ? S_IRUSR|S_IRGRP :
			  S_IRUSR) < 0){
		    audit_msg(LOG_WARNING, "Couldn't change permissions while "
			"rotating log file (%s)", strerror(errno));
		}
		if (fchown(log_fd, 0, config->log_group) < 0) {
		    audit_msg(LOG_WARNING, "Couldn't change ownership while "
			"rotating log file (%s)", strerror(errno));
		}
	}
	if (log_file) {
		log_fd = -1;
		fclose(log_file);
		log_file = NULL;
	}

	/* Rotate */
	len = strlen(config->log_file) + 16;
	oldname = (char *)malloc(len);
	if (oldname == NULL) { /* Not fatal - just messy */
		audit_msg(LOG_ERR, "No memory rotating logs");
		logging_suspended = 1;
		return;
	}
	newname = (char *)malloc(len);
	if (newname == NULL) { /* Not fatal - just messy */
		audit_msg(LOG_ERR, "No memory rotating logs");
		free(oldname);
		logging_suspended = 1;
		return;
	}

	/* If we are rotating, get number from config */
	if (num_logs == 0)
		num_logs = config->num_logs;

	/* Handle this case first since it will not enter the for loop */
	if (num_logs == 2)
		snprintf(oldname, len, "%s.1", config->log_file);

	known_logs = 0;
	for (i=(int)num_logs - 1; i>1; i--) {
		snprintf(oldname, len, "%s.%d", config->log_file, i-1);
		snprintf(newname, len, "%s.%d", config->log_file, i);
		/* if the old file exists */
		rc = rename(oldname, newname);
		if (rc == -1 && errno != ENOENT) {
			// Likely errors: ENOSPC, ENOMEM, EBUSY
			int saved_errno = errno;
			audit_msg(LOG_ERR,
				"Error rotating logs from %s to %s (%s)",
				oldname, newname, strerror(errno));
			if (saved_errno == ENOSPC && fs_space_left == 1) {
				fs_space_left = 0;
				do_disk_full_action();
			} else
				do_disk_error_action("rotate", saved_errno);
		} else if (rc == 0 && known_logs == 0)
			known_logs = i + 1;
	}
	free(newname);

	/* At this point, oldname should point to lowest number - use it */
	newname = oldname;
	rc = rename(config->log_file, newname);
	if (rc == -1 && errno != ENOENT) {
		// Likely errors: ENOSPC, ENOMEM, EBUSY
		int saved_errno = errno;
		audit_msg(LOG_ERR, "Error rotating logs from %s to %s (%s)",
			config->log_file, newname, strerror(errno));
		if (saved_errno == ENOSPC && fs_space_left == 1) {
			fs_space_left = 0;
			do_disk_full_action();
		} else
			do_disk_error_action("rotate2", saved_errno);

		/* At this point, we've failed to rotate the original log.
		 * So, let's make the old log writable and try again next
		 * time */
		chmod(config->log_file,
			config->log_group ? S_IWUSR|S_IRUSR|S_IRGRP :
			S_IWUSR|S_IRUSR);
	}
	free(newname);

	/* open new audit file */
	if (open_audit_log()) {
		int saved_errno = errno;
		audit_msg(LOG_CRIT,
			"Could not reopen a log after rotating.");
		logging_suspended = 1;
		do_disk_error_action("reopen", saved_errno);
	}
}

static unsigned int last_log = 1;
static void shift_logs(void)
{
	// The way this has to work is to start scanning from .1 up until
	// no file is found. Then do the rotate algorithm using that number
	// instead of log_max.
	unsigned int num_logs, len;
	char *name;

	len = strlen(config->log_file) + 16;
	name = (char *)malloc(len);
	if (name == NULL) { /* Not fatal - just messy */
		audit_msg(LOG_ERR, "No memory shifting logs");
		return;
	}

	// Find last log
	num_logs = last_log;
	while (num_logs) {
		snprintf(name, len, "%s.%u", config->log_file,
						num_logs);
		if (access(name, R_OK) != 0)
			break;
		num_logs++;
	}
	known_logs = num_logs;

	/* Our last known file disappeared, start over... */
	if (num_logs <= last_log && last_log > 1) {
		audit_msg(LOG_WARNING, "Last known log disappeared (%s)", name);
		num_logs = last_log = 1;
		while (num_logs) {
			snprintf(name, len, "%s.%u", config->log_file,
							num_logs);
			if (access(name, R_OK) != 0)
				break;
			num_logs++;
		}
		audit_msg(LOG_INFO, "Next log to use will be %s", name);
	}
	last_log = num_logs;
	rotate_logs(num_logs+1, 1);
	free(name);
}

/*
 * This function handles opening a descriptor for the audit log
 * file and ensuring the correct options are applied to the descriptor.
 * It returns 0 on success and 1 on failure.
 */
static int open_audit_log(void)
{
	int flags, lfd;

	if (config->write_logs == 0)
		return 0;

	flags = O_WRONLY|O_APPEND|O_NOFOLLOW|O_CLOEXEC;
	if (config->flush == FT_DATA)
		flags |= O_DSYNC;
	else if (config->flush == FT_SYNC)
		flags |= O_SYNC;

	// Likely errors for open: Almost anything
	// Likely errors on rotate: ENFILE, ENOMEM, ENOSPC
retry:
	lfd = open(config->log_file, flags);
	if (lfd < 0) {
		if (errno == ENOENT) {
			lfd = create_log_file(config->log_file);
			if (lfd < 0) {
				audit_msg(LOG_CRIT,
					"Couldn't create log file %s (%s)",
					config->log_file,
					strerror(errno));
				return 1;
			}
			close(lfd);
			lfd = open(config->log_file, flags);
			log_size = 0;
		} else if (errno == ENFILE) {
			// All system descriptors used, try again...
			goto retry;
		}
		if (lfd < 0) {
			audit_msg(LOG_CRIT, "Couldn't open log file %s (%s)",
				config->log_file, strerror(errno));
			return 1;
		}
	} else {
		// Get initial size
		struct stat st;

		int rc = fstat(lfd, &st);
		if (rc == 0)
			 log_size = st.st_size;
		else {
			close(lfd);
			return 1;
		}
	}

	if (fchmod(lfd, config->log_group ? S_IRUSR|S_IWUSR|S_IRGRP :
							S_IRUSR|S_IWUSR) < 0) {
		audit_msg(LOG_ERR,
			"Couldn't change permissions of log file (%s)",
			strerror(errno));
		close(lfd);
		return 1;
	}
	if (fchown(lfd, 0, config->log_group) < 0) {
		audit_msg(LOG_ERR, "Couldn't change ownership of log file (%s)",
			strerror(errno));
		close(lfd);
		return 1;
	}

	log_fd = lfd;
	log_file = fdopen(lfd, "a");
	if (log_file == NULL) {
		audit_msg(LOG_CRIT, "Error setting up log descriptor (%s)",
			strerror(errno));
		close(lfd);
		return 1;
	}

	/* Set it to line buffering */
	setlinebuf(log_file);
	return 0;
}

/*
 * This function executes a new process. It returns -1 on failure to fork.
 * It returns a positive number to the parent. This positive number is the
 * pid of the child. If the child fails to exec the new process, it exits
 * with a 1. The exit code can be picked up in the sigchld handler.
 */
static pid_t safe_exec(const char *exe)
{
	char *argv[2];
	pid_t pid;
	struct sigaction sa;

	if (exe == NULL) {
		audit_msg(LOG_ALERT,
			"Safe_exec passed NULL for program to execute");
		return -1;
	}

	pid = fork();
	if (pid < 0) {
		audit_msg(LOG_ALERT,
			"Audit daemon failed to fork doing safe_exec");
		return -1;
	}
	if (pid) /* Parent */
	return pid;
	/* Child */
	sigfillset(&sa.sa_mask);
	sigprocmask(SIG_UNBLOCK, &sa.sa_mask, 0);
#ifdef HAVE_CLOSE_RANGE
	close_range(3, ~0U, 0); /* close all past stderr */
#else
	for (int i=3; i<24; i++)     /* Arbitrary number */
		close(i);
#endif

	argv[0] = (char *)exe;
	argv[1] = NULL;
	execve(exe, argv, NULL);
	audit_msg(LOG_ALERT, "Audit daemon failed to exec %s", exe);
	exit(1); /* FIXME: Maybe this should error instead of exit */
}

static void reconfigure(struct auditd_event *e)
{
	struct daemon_conf *nconf = e->reply.conf;
	struct daemon_conf *oconf = config;
	uid_t uid = nconf->sender_uid;
	pid_t pid = nconf->sender_pid;
	const char *ctx = nconf->sender_ctx;
	struct timeval tv;
	char txt[MAX_AUDIT_MESSAGE_LENGTH];
	char date[40];
	unsigned int seq_num;
	int need_size_check = 0, need_reopen = 0, need_space_check = 0;

	snprintf(txt, sizeof(txt),
		"config change requested by pid=%d auid=%u subj=%s",
		pid, uid, ctx);
	audit_msg(LOG_NOTICE, "%s", txt);

	/* Do the reconfiguring. These are done in a specific
	 * order from least invasive to most invasive. We will
	 * start with general system parameters. */

	// start with disk error action.
	oconf->disk_error_action = nconf->disk_error_action;
	free((char *)oconf->disk_error_exe);
	oconf->disk_error_exe = nconf->disk_error_exe;
	disk_err_warning = 0;

	// number of logs
	oconf->num_logs = nconf->num_logs;

	// flush freq
	oconf->freq = nconf->freq;

	// priority boost
	if (oconf->priority_boost != nconf->priority_boost) {
		oconf->priority_boost = nconf->priority_boost;
		errno = 0;
		if (nice(-oconf->priority_boost))
			; /* Intentionally blank, we have to check errno */
		if (errno)
			audit_msg(LOG_WARNING, "Cannot change priority in "
					"reconfigure (%s)", strerror(errno));
	}

	// log format
	oconf->log_format = nconf->log_format;

	// Only update this if we are in background mode since
	// foreground mode writes to stderr.
	if ((oconf->write_logs != nconf->write_logs) &&
				(oconf->daemonize == D_BACKGROUND)) {
		oconf->write_logs = nconf->write_logs;
		need_reopen = 1;
	}

	// log_group
	if (oconf->log_group != nconf->log_group) {
		oconf->log_group = nconf->log_group;
		need_reopen = 1;
	}

	// action_mail_acct
	if (strcmp(oconf->action_mail_acct, nconf->action_mail_acct)) {
		free((void *)oconf->action_mail_acct);
		oconf->action_mail_acct = nconf->action_mail_acct;
	} else
		free((void *)nconf->action_mail_acct);

	// node_name
	if (oconf->node_name_format != nconf->node_name_format ||
			(oconf->node_name && nconf->node_name &&
			strcmp(oconf->node_name, nconf->node_name) != 0)) {
		oconf->node_name_format = nconf->node_name_format;
		free((char *)oconf->node_name);
		oconf->node_name = nconf->node_name;
	}

	// network listener
	auditd_tcp_listen_reconfigure(nconf, oconf);

	// distribute network events
	oconf->distribute_network_events = nconf->distribute_network_events;

	// Dispatcher items
	oconf->q_depth = nconf->q_depth;
	oconf->overflow_action = nconf->overflow_action;
	oconf->max_restarts = nconf->max_restarts;
	if (oconf->plugin_dir != nconf->plugin_dir ||
		(oconf->plugin_dir && nconf->plugin_dir &&
		strcmp(oconf->plugin_dir, nconf->plugin_dir) != 0)) {
		free(oconf->plugin_dir);
		oconf->plugin_dir = nconf->plugin_dir;
	}

	/* At this point we will work on the items that are related to
	 * a single log file. */

	// max logfile action
	if (oconf->max_log_size_action != nconf->max_log_size_action) {
		oconf->max_log_size_action = nconf->max_log_size_action;
		need_size_check = 1;
	}

	// max log size
	if (oconf->max_log_size != nconf->max_log_size) {
		oconf->max_log_size = nconf->max_log_size;
		need_size_check = 1;
	}

	// max log exe
	if (oconf->max_log_file_exe || nconf->max_log_file_exe) {
		if (nconf->max_log_file_exe == NULL)
                       ;
		else if (oconf->max_log_file_exe == NULL && nconf->max_log_file_exe)
			need_size_check = 1;
		else if (strcmp(oconf->max_log_file_exe,
				nconf->max_log_file_exe))
			need_size_check = 1;
		free((char *)oconf->max_log_file_exe);
		oconf->max_log_file_exe = nconf->max_log_file_exe;
	}

	if (need_size_check) {
		logging_suspended = 0;
		check_log_file_size();
	}

	// flush technique
	if (oconf->flush != nconf->flush) {
		oconf->flush = nconf->flush;
		need_reopen = 1;
	}

	// logfile
	if (strcmp(oconf->log_file, nconf->log_file)) {
		free((void *)oconf->log_file);
		oconf->log_file = nconf->log_file;
		need_reopen = 1;
		need_space_check = 1; // might be on new partition
	} else
		free((void *)nconf->log_file);

	if (need_reopen) {
		if (log_file)
			fclose(log_file);
		log_file = NULL;
		fix_disk_permissions();
		if (open_audit_log()) {
			int saved_errno = errno;
			audit_msg(LOG_ERR,
				"Could not reopen a log after reconfigure");
			logging_suspended = 1;
			// Likely errors: ENOMEM, ENOSPC
			do_disk_error_action("reconfig", saved_errno);
		} else {
			logging_suspended = 0;
			check_log_file_size();
		}
	}

	/* At this point we will start working on items that are
	 * related to the amount of space on the partition. */

	// space left
	if (oconf->space_left != nconf->space_left) {
		oconf->space_left = nconf->space_left;
		need_space_check = 1;
	}

	// space left percent
	if (oconf->space_left_percent != nconf->space_left_percent) {
		oconf->space_left_percent = nconf->space_left_percent;
		need_space_check = 1;
	}

	// space left action
	if (oconf->space_left_action != nconf->space_left_action) {
		oconf->space_left_action = nconf->space_left_action;
		need_space_check = 1;
	}

	// space left exe
	if (oconf->space_left_exe || nconf->space_left_exe) {
		if (nconf->space_left_exe == NULL)
			; /* do nothing if new one is blank */
		else if (oconf->space_left_exe == NULL && nconf->space_left_exe)
			need_space_check = 1;
		else if (strcmp(oconf->space_left_exe, nconf->space_left_exe))
			need_space_check = 1;
		free((char *)oconf->space_left_exe);
		oconf->space_left_exe = nconf->space_left_exe;
	}

	// admin space left
	if (oconf->admin_space_left != nconf->admin_space_left) {
		oconf->admin_space_left = nconf->admin_space_left;
		need_space_check = 1;
	}

	// admin space left percent
	if (oconf->admin_space_left_percent != nconf->admin_space_left_percent){
		oconf->admin_space_left_percent =
					nconf->admin_space_left_percent;
		need_space_check = 1;
	}

	// admin space action
	if (oconf->admin_space_left_action != nconf->admin_space_left_action) {
		oconf->admin_space_left_action = nconf->admin_space_left_action;
		need_space_check = 1;
	}

	// admin space left exe
	if (oconf->admin_space_left_exe || nconf->admin_space_left_exe) {
		if (nconf->admin_space_left_exe == NULL)
			; /* do nothing if new one is blank */
		else if (oconf->admin_space_left_exe == NULL &&
					 nconf->admin_space_left_exe)
			need_space_check = 1;
		else if (strcmp(oconf->admin_space_left_exe,
					nconf->admin_space_left_exe))
			need_space_check = 1;
		free((char *)oconf->admin_space_left_exe);
		oconf->admin_space_left_exe = nconf->admin_space_left_exe;
	}
	// disk full action
	if (oconf->disk_full_action != nconf->disk_full_action) {
		oconf->disk_full_action = nconf->disk_full_action;
		need_space_check = 1;
	}

	// disk full exe
	if (oconf->disk_full_exe || nconf->disk_full_exe) {
		if (nconf->disk_full_exe == NULL)
			; /* do nothing if new one is blank */
		else if (oconf->disk_full_exe == NULL && nconf->disk_full_exe)
			need_space_check = 1;
		else if (strcmp(oconf->disk_full_exe, nconf->disk_full_exe))
			need_space_check = 1;
		free((char *)oconf->disk_full_exe);
		oconf->disk_full_exe = nconf->disk_full_exe;
	}

	// report interval
	if (oconf->report_interval != nconf->report_interval) {
		oconf->report_interval = nconf->report_interval;
		update_report_timer(oconf->report_interval);
	}

	if (need_space_check) {
		/* note save suspended flag, then do space_left. If suspended
		 * is still 0, then copy saved suspended back. This avoids
		 * having to call check_log_file_size to restore it. */
		int saved_suspend = logging_suspended;

		setup_percentages(oconf, log_fd);
		fs_space_warning = 0;
		fs_admin_space_warning = 0;
		fs_space_left = 1;
		logging_suspended = 0;
		check_excess_logs();
		check_space_left();
		if (logging_suspended == 0)
			logging_suspended = saved_suspend;
	}

	reconfigure_dispatcher(oconf);

	// Next document the results
	srand(time(NULL));
	seq_num = rand()%10000;
	if (gettimeofday(&tv, NULL) == 0) {
		snprintf(date, sizeof(date), "audit(%lld.%03u:%u)",
			 (long long int)tv.tv_sec, (unsigned)(tv.tv_usec/1000),
			 seq_num);
	} else {
		snprintf(date, sizeof(date),
			"audit(%lld.%03d:%u)", (long long int)time(NULL),
			 0, seq_num);
        }

	e->reply.type = AUDIT_DAEMON_CONFIG;
	e->reply.len = snprintf(e->reply.msg.data, MAX_AUDIT_MESSAGE_LENGTH-2,
	"%s: op=reconfigure state=changed auid=%u pid=%d subj=%s res=success",
		date, uid, pid, ctx );
	e->reply.message = e->reply.msg.data;
	free((char *)ctx);
}

