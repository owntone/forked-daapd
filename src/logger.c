/*
 * Copyright (C) 2009-2011 Julien BLACHE <jb@jblache.org>
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
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "logger.h"

#include <stdio.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <ctype.h> // for isprint()

#include <event2/event.h>

#include <libavutil/log.h>

#include "conffile.h"
#include "misc.h"

#define LOGGER_REPEAT_MAX 10

/* We need our own check to avoid nested locking or recursive calls */
#define LOGGER_CHECK_ERR(f) \
  do { int lerr; lerr = f; if (lerr != 0) { \
      vlogger_fatal("%s failed at line %d, err %d (%s)\n", #f, __LINE__, \
                    lerr, strerror(lerr)); \
      abort(); \
    } } while(0)

static pthread_mutex_t logger_lck;
static int logger_initialized;
static int logdomains;
static int threshold;
static int console = 1;
static uint32_t logger_repeat_counter;
static uint32_t logger_last_hash;
static char *logfilename;
static FILE *logfile;
static char *labels[] = { "config", "daap", "db", "httpd", "http", "main", "mdns", "misc", "rsp", "scan", "xcode", "event", "remote", "dacp", "ffmpeg", "artwork", "player", "raop", "laudio", "dmap", "dbperf", "spotify", "scrobble", "cache", "mpd", "stream", "cast", "fifo", "lib", "web", "airplay", "rcp" };
static char *severities[] = { "FATAL", "LOG", "WARN", "INFO", "DEBUG", "SPAM" };
static char *format_labels[] = { "default", "logfmt" };

enum format {
  L_FMT_DEFAULT = 0,
  L_FMT_LOGFMT = 1,
};
static enum format format = L_FMT_DEFAULT;

static int
set_logdomains(char *domains)
{
  char *ptr;
  char *d;
  int i;

  logdomains = 0;

  while ((d = strtok_r(domains, " ,", &ptr)))
    {
      domains = NULL;

      for (i = 0; i < N_LOGDOMAINS; i++)
	{
	  if (strcmp(d, labels[i]) == 0)
	    {
	      logdomains |= (1 << i);
	      break;
	    }
	}

      if (i == N_LOGDOMAINS)
	{
	  fprintf(stderr, "Error: unknown log domain '%s'\n", d);
	  return -1;
	}
    }

  return 0;
}

static int
format_code_get(const char *label)
{
  int i;

  if (!label)
    return 0;

  for (i = 0; i < ARRAY_SIZE(format_labels); i++)
    {
      if (strcmp(label, format_labels[i]) == 0)
	return i;
    }

  return 0;
}

static int
repeat_count(const char *fmt)
{
  uint32_t hash;

  hash = djb_hash(fmt, strlen(fmt));

  if (hash == logger_last_hash)
    logger_repeat_counter++;
  else
    logger_repeat_counter = 0;

  logger_last_hash = hash;

  return logger_repeat_counter;
}

static void
logger_write(const char *fmt, ...)
{
  va_list ap;

  if (logfile)
    {
      va_start(ap, fmt);
      vfprintf(logfile, fmt, ap);
      va_end(ap);

      fflush(logfile);
    }
  if (console)
    {
      va_start(ap, fmt);
      vfprintf(stderr, fmt, ap);
      va_end(ap);
    }
}

static void
logger_write_with_label(int severity, int domain, const char *content)
{
  char stamp[32];
  char thread_nametid[32];
  time_t t;
  struct tm timebuf;
  char logfmt_msg[1024];
  int ret;

  thread_getnametid(thread_nametid, sizeof(thread_nametid));
  t = time(NULL);

  if (format == L_FMT_LOGFMT)
    {
      ret = strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S%z", localtime_r(&t, &timebuf));
      if (ret == 0)
	stamp[0] = '\0';

      strncpy(logfmt_msg, content, sizeof(logfmt_msg));
      logfmt_msg[sizeof(logfmt_msg) - 1] = '\0';
      safe_snreplace(logfmt_msg, sizeof(logfmt_msg), "\n", " ");
      safe_snreplace(logfmt_msg, sizeof(logfmt_msg), "\"", "\\\"");

      logger_write("time=%s level=%s thread=\"%s\" component=%s msg=\"%s\"\n", stamp, severities[severity], thread_nametid,
          labels[domain], logfmt_msg);
    }
  else
    {
      ret = strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", localtime_r(&t, &timebuf));
      if (ret == 0)
	stamp[0] = '\0';

      logger_write("[%s] [%5s] [%16s] %8s: %s", stamp, severities[severity], thread_nametid, labels[domain], content);
    }
}

static void
vlogger_writer(int severity, int domain, const char *fmt, va_list args)
{
  va_list ap;
  char content[2048];
  int ret;

  va_copy(ap, args);
  ret = vsnprintf(content, sizeof(content), fmt, ap);
  if (ret < 0)
    strcpy(content, "(LOGGING SKIPPED - error printing log message)\n");
  else if (ret >= sizeof(content))
    strcpy(content + sizeof(content) - 8, "...\n");
  va_end(ap);

  ret = repeat_count(content);
  if (ret == LOGGER_REPEAT_MAX)
    strcpy(content, "(LOGGING SKIPPED - above log message is repeating)\n");
  else if (ret > LOGGER_REPEAT_MAX)
    return;

  logger_write_with_label(severity, domain, content);
}

static void
vlogger_fatal(const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  vlogger_writer(E_FATAL, L_MISC, fmt, ap);
  va_end(ap);
}

static void
vlogger(int severity, int domain, const char *fmt, va_list args)
{

  if(! logger_initialized)
    {
      /* lock not initialized, use stderr */
      vlogger_writer(severity, domain, fmt, args);
      return;
    }

  if (!((1 << domain) & logdomains) || (severity > threshold))
    return;

  LOGGER_CHECK_ERR(pthread_mutex_lock(&logger_lck));

  if (!logfile && !console)
    {
      LOGGER_CHECK_ERR(pthread_mutex_unlock(&logger_lck));
      return;
    }

  vlogger_writer(severity, domain, fmt, args);

  LOGGER_CHECK_ERR(pthread_mutex_unlock(&logger_lck));
}

static void
hexdump(int severity, int domain, const unsigned char *data, int len, const char *heading)
{
  int i;
  unsigned char buff[17];
  const unsigned char *pc = data;

  if (len <= 0)
    return;

  LOGGER_CHECK_ERR(pthread_mutex_lock(&logger_lck));

  if (heading)
    logger_write_with_label(severity, domain, heading);

  for (i = 0; i < len; i++)
    {
      if ((i % 16) == 0)
	{
	  if (i != 0)
	    logger_write("  %s\n", buff);

	  logger_write(" %04x ", i);
	}

	logger_write(" %02x", pc[i]);

	if (isprint(pc[i]))
	  buff[i % 16] = pc[i];
	else
	  buff[i % 16] = '.';

	buff[(i % 16) + 1] = '\0';
    }

  while ((i % 16) != 0)
    {
      logger_write("   ");
      i++;
    }

  logger_write("  %s\n", buff);

  LOGGER_CHECK_ERR(pthread_mutex_unlock(&logger_lck));
}

void
DPRINTF(int severity, int domain, const char *fmt, ...)
{
  va_list ap;

  // If domain and severity do not match the current log configuration, return early to
  // save some unnecessary code execution (tiny performance gain)
  if (logger_initialized && (!((1 << domain) & logdomains) || (severity > threshold)))
    return;

  va_start(ap, fmt);
  vlogger(severity, domain, fmt, ap);
  va_end(ap);
}

void
DVPRINTF(int severity, int domain, const char *fmt, va_list ap)
{
  // If domain and severity do not match the current log configuration, return early to
  // safe some unnecessary code execution (tiny performance gain)
  if (logger_initialized && (!((1 << domain) & logdomains) || (severity > threshold)))
    return;

  vlogger(severity, domain, fmt, ap);
}

void
DHEXDUMP(int severity, int domain, const unsigned char *data, int data_len, const char *heading)
{
  // If domain and severity do not match the current log configuration, return early to
  // save some unnecessary code execution (tiny performance gain)
  if (logger_initialized && (!((1 << domain) & logdomains) || (severity > threshold)))
    return;

  hexdump(severity, domain, data, data_len, heading);
}

void
logger_ffmpeg(void *ptr, int level, const char *fmt, va_list ap)
{
  int severity;

  if (level <= AV_LOG_FATAL)
    severity = E_LOG;
  else if (level <= AV_LOG_WARNING)
    severity = E_WARN;
  else if (level <= AV_LOG_VERBOSE)
    severity = E_DBG;
  else if (level <= AV_LOG_DEBUG)
    severity = E_SPAM;
  else
    severity = E_SPAM;

  vlogger(severity, L_FFMPEG, fmt, ap);
}

void
logger_libevent(int severity, const char *msg)
{
  switch (severity)
    {
      case EVENT_LOG_DEBUG:
	severity = E_DBG;
	break;

      case EVENT_LOG_ERR:
	severity = E_LOG;
	break;

      case EVENT_LOG_WARN:
	severity = E_WARN;
	break;

      case EVENT_LOG_MSG:
	severity = E_INFO;
	break;

      default:
	severity = E_LOG;
	break;
    }

  DPRINTF(severity, L_EVENT, "%s\n", msg);
}

#ifdef HAVE_ALSA
void
logger_alsa(const char *file, int line, const char *function, int err, const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  vlogger(E_LOG, L_LAUDIO, fmt, ap);
  va_end(ap);
}
#endif /* HAVE_ALSA */

void
logger_reinit(void)
{
  FILE *fp;

  if (!logfile)
    return;

  LOGGER_CHECK_ERR(pthread_mutex_lock(&logger_lck));

  fp = fopen(logfilename, "a");
  if (!fp)
    {
      fprintf(logfile, "Could not reopen logfile: %s\n", strerror(errno));

      goto out;
    }

  fclose(logfile);
  logfile = fp;

 out:
  LOGGER_CHECK_ERR(pthread_mutex_unlock(&logger_lck));
}


int
logger_severity(void)
{
  return threshold;
}

/* The functions below are used at init time with a single thread running */
void
logger_domains(void)
{
  int i;

  fprintf(stdout, "%s", labels[0]);

  for (i = 1; i < N_LOGDOMAINS; i++)
    fprintf(stdout, ", %s", labels[i]);

  fprintf(stdout, "\n");
}

void
logger_detach(void)
{
  console = 0;
}

int
logger_init(char *file, char *domains, int severity, char *logformat)
{
  int ret;

  if ((sizeof(labels) / sizeof(labels[0])) != N_LOGDOMAINS)
    {
      fprintf(stderr, "WARNING: log domains do not match\n");

      return -1;
    }

  console = 1;
  threshold = severity;
  format = format_code_get(logformat);

  if (domains)
    {
      ret = set_logdomains(domains);
      if (ret < 0)
	return ret;
    }
  else
    logdomains = ~0;

  if (!file)
    return 0;

  logfile = fopen(file, "a");
  if (!logfile)
    {
      fprintf(stderr, "Could not open logfile %s: %s\n", file, strerror(errno));

      return -1;
    }

  ret = fchown(fileno(logfile), runas_uid, 0);
  if (ret < 0)
    fprintf(stderr, "Failed to set ownership on logfile: %s\n", strerror(errno));

  ret = fchmod(fileno(logfile), 0644);
  if (ret < 0)
    fprintf(stderr, "Failed to set permissions on logfile: %s\n", strerror(errno));

  logfilename = file;

  /* logging w/o locks before initialized complete */
  CHECK_ERR(L_MISC, mutex_init(&logger_lck));

  logger_initialized = 1;

  return 0;
}

void
logger_deinit(void)
{
  if (logfile)
    {
      fclose(logfile);
      logfile = NULL;
    }

  if(logger_initialized)
    {
      /* logging w/o locks to stderr now */
      logger_initialized = 0;
      console = 1;
      CHECK_ERR(L_MISC, pthread_mutex_destroy(&logger_lck));
    }
}
