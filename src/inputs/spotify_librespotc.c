/*
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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>
#include <pthread.h>

#include <event2/event.h>

#include "input.h"
#include "misc.h"
#include "logger.h"
#include "conffile.h"
#include "listener.h"
#include "http.h"
#include "db.h"
#include "transcode.h"
#include "spotify.h"
#include "librespot-c/librespot-c.h"

// Haven't actually studied ffmpeg's probe size requirements, this is just a
// guess
#define SPOTIFY_PROBE_SIZE_MIN 16384

// The transcoder will say EOF if too little data is provided to it
#define SPOTIFY_BUF_MIN 4096

// Limits how much of the Spotify Ogg file we fetch and buffer (in read_buf).
// This will also in effect throttle in librespot-c.
#define SPOTIFY_BUF_MAX (512 * 1024)

struct global_ctx
{
  bool is_initialized;
  struct spotify_status status;

  struct sp_session *session;
  enum sp_bitrates bitrate_preferred;
};

struct download_ctx
{
  bool is_started;
  bool is_ended;
  struct transcode_ctx *xcode;

  struct evbuffer *read_buf;
  int read_fd;

  uint32_t len_ms;
  size_t len_bytes;
};

static struct global_ctx spotify_ctx;

// Must be initialized statically since we don't have anywhere to do it at
// runtime. We are in the special situation that multiple threads can result in
// calls to initialize(), e.g. input_init() and library init scan, thus it must
// have the lock ready to use to be thread safe.
static pthread_mutex_t spotify_ctx_lock = PTHREAD_MUTEX_INITIALIZER;

static struct media_quality spotify_quality = { 44100, 16, 2, 0 };


/* ------------------------------ Utility funcs ----------------------------- */

static void
hextobin(uint8_t *data, size_t data_len, const char *hexstr, size_t hexstr_len)
{
  char hex[] = { 0, 0, 0 };
  const char *ptr;
  int i;

  if (2 * data_len < hexstr_len)
    {
      memset(data, 0, data_len);
      return;
    }

  ptr = hexstr;
  for (i = 0; i < data_len; i++, ptr+=2)
    {
      memcpy(hex, ptr, 2);
      data[i] = strtol(hex, NULL, 16);
    }
}

static int
postlogin(struct global_ctx *ctx)
{
  struct sp_credentials credentials;
  char *db_stored_cred;
  char *ptr;
  int i;
  int ret;

  ret = librespotc_credentials_get(&credentials, ctx->session);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Error getting Spotify credentials: %s\n", librespotc_last_errmsg());
      return -1;
    }

  CHECK_NULL(L_SPOTIFY, db_stored_cred = malloc(2 * credentials.stored_cred_len + 1));
  for (i = 0, ptr = db_stored_cred; i < credentials.stored_cred_len; i++)
    ptr += sprintf(ptr, "%02x", credentials.stored_cred[i]);

  db_admin_set("spotify_username", credentials.username);
  db_admin_set("spotify_stored_cred", db_stored_cred);

  free(db_stored_cred);

  ctx->status.logged_in = true;
  snprintf(ctx->status.username, sizeof(ctx->status.username), "%s", credentials.username);

  librespotc_bitrate_set(ctx->session, ctx->bitrate_preferred);

  DPRINTF(E_LOG, L_SPOTIFY, "Logged into Spotify succesfully with username %s\n", credentials.username);

  listener_notify(LISTENER_SPOTIFY);

  return 0;
}

static int
login_stored_cred(struct global_ctx *ctx, const char *username, const char *db_stored_cred)
{
  size_t db_stored_cred_len;
  uint8_t *stored_cred = NULL;
  size_t stored_cred_len;
  int ret;

  db_stored_cred_len = strlen(db_stored_cred);
  stored_cred_len = db_stored_cred_len / 2;

  CHECK_NULL(L_SPOTIFY, stored_cred = malloc(stored_cred_len));
  hextobin(stored_cred, stored_cred_len, db_stored_cred, db_stored_cred_len);

  ctx->session = librespotc_login_stored_cred(username, stored_cred, stored_cred_len);
  if (!ctx->session)
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Error logging into Spotify: %s\n", librespotc_last_errmsg());
      goto error;
    }

  ret = postlogin(ctx);
  if (ret < 0)
    goto error;

  free(stored_cred);
  return 0;

 error:
  free(stored_cred);
  if (ctx->session)
    librespotc_logout(ctx->session);
  ctx->session = NULL;
  return -1;
}

// If there is evbuf size is below max, reads from a non-blocking fd until error,
// EAGAIN or evbuf full
static int
fd_read(bool *eofptr, struct evbuffer *evbuf, int fd)
{
  size_t len = evbuffer_get_length(evbuf);
  bool eof = false;
  int total = 0;
  int ret = 0;

  while (len + total < SPOTIFY_BUF_MAX && !eof)
    {
      ret = evbuffer_read(evbuf, fd, -1); // Each read is 4096 bytes (EVBUFFER_READ_MAX)

      if (ret == 0)
	eof = true;
      else if (ret < 0)
	break;

      total += ret;
    }

  if (eofptr)
    *eofptr = eof;

  if (ret < 0 && errno != EAGAIN)
    return ret;

  return total;
}

/* ------------------ Callbacks from librespot-c thread --------------------- */

static void
progress_cb(int fd, void *cb_arg, size_t received, size_t len)
{
  DPRINTF(E_SPAM, L_SPOTIFY, "Progress %zu/%zu\n", received, len);
}

static int
tcp_connect(const char *address, unsigned short port)
{
  return net_connect(address, port, SOCK_STREAM, "spotify");
}

static void
tcp_disconnect(int fd)
{
  close(fd);
}

static void
thread_name_set(pthread_t thread)
{
  thread_setname(thread, "spotify");
}

static void
logmsg_cb(const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  DVPRINTF(E_DBG, L_SPOTIFY, fmt, ap);
  va_end(ap);
}

static void
hexdump_cb(const char *msg, uint8_t *data, size_t data_len)
{
//  DHEXDUMP(E_DBG, L_SPOTIFY, data, data_len, msg);
}


/* ------------------------ librespot-c initialization ---------------------- */

struct sp_callbacks callbacks = {
  .tcp_connect    = tcp_connect,
  .tcp_disconnect = tcp_disconnect,

  .thread_name_set = thread_name_set,

  .hexdump  = hexdump_cb,
  .logmsg   = logmsg_cb,
};

// Called from main thread as part of player_init, or from library thread as
// part of relogin. Caller must use mutex for thread safety.
static int
initialize(struct global_ctx *ctx)
{
  struct sp_sysinfo sysinfo = { 0 };
  cfg_t *spotify_cfg;
  int ret;

  spotify_cfg = cfg_getsec(cfg, "spotify");

  if (ctx->is_initialized)
    return 0;

  snprintf(sysinfo.device_id, sizeof(sysinfo.device_id), "%" PRIx64, libhash); // TODO use a UUID instead

  ret = librespotc_init(&sysinfo, &callbacks);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Error initializing Spotify: %s\n", librespotc_last_errmsg());
      goto error;
    }

  switch (cfg_getint(spotify_cfg, "bitrate"))
    {
      case 1:
	ctx->bitrate_preferred = SP_BITRATE_96;
	break;
      case 2:
	ctx->bitrate_preferred = SP_BITRATE_160;
	break;
      case 3:
	ctx->bitrate_preferred = SP_BITRATE_320;
	break;
      default:
	ctx->bitrate_preferred = SP_BITRATE_ANY;
    }

  ctx->is_initialized = true;
  return 0;

 error:
  ctx->is_initialized = false;
  return -1;
}


/* --------------------- Implementation (input thread) ---------------------- */

static int64_t
download_seek(void *arg, int64_t offset, enum transcode_seek_type type)
{
  struct download_ctx *download = arg;
  int64_t out;
  int ret;

  switch (type)
    {
      case XCODE_SEEK_SIZE:
	out = download->len_bytes;
	break;
      case XCODE_SEEK_SET:
	// Flush read buffer
	evbuffer_drain(download->read_buf, -1);

	ret = librespotc_seek(download->read_fd, offset);
	if (ret < 0)
	  goto error;

	fd_read(NULL, download->read_buf, download->read_fd);

	out = offset;
	break;
      default:
	goto error;
    }

  DPRINTF(E_DBG, L_SPOTIFY, "Seek to offset %" PRIi64 " requested, type %d, returning %" PRIi64 "\n", offset, type, out);

  return out;

 error:
  DPRINTF(E_WARN, L_SPOTIFY, "Seek error\n");

  return -1;
}

// Has to be called after we have started receiving data, since ffmpeg needs to
// probe the data to find the audio streams
static int
download_xcode_setup(struct download_ctx *download)
{
  struct transcode_decode_setup_args decode_args = { .profile = XCODE_OGG, .len_ms = download->len_ms };
  struct transcode_encode_setup_args encode_args = { .profile = XCODE_PCM16, };
  struct transcode_evbuf_io xcode_evbuf_io = { 0 };
  struct transcode_ctx *xcode;

  xcode_evbuf_io.evbuf = download->read_buf;
  xcode_evbuf_io.seekfn = download_seek;
  xcode_evbuf_io.seekfn_arg = download;
  decode_args.evbuf_io = &xcode_evbuf_io;

  xcode = transcode_setup(decode_args, encode_args);
  if (!xcode)
    goto error;

  download->xcode = xcode;

  return 0;

 error:
  transcode_cleanup(&xcode);
  return -1;
}

static void
download_free(struct download_ctx *download)
{
  if (!download)
    return;

  if (download->read_fd >= 0)
    librespotc_close(download->read_fd);

  if (download->read_buf)
    evbuffer_free(download->read_buf);

  transcode_cleanup(&download->xcode);
  free(download);
}

static struct download_ctx *
download_new(int fd, uint32_t len_ms, size_t len_bytes)
{
  struct download_ctx *download;

  CHECK_NULL(L_SPOTIFY, download = calloc(1, sizeof(struct download_ctx)));
  CHECK_NULL(L_SPOTIFY, download->read_buf = evbuffer_new());

  download->read_fd = fd;
  download->len_ms = len_ms;
  download->len_bytes = len_bytes;

  return download;
}

static int
stop(struct input_source *source)
{
  struct download_ctx *download = source->input_ctx;

  DPRINTF(E_DBG, L_SPOTIFY, "stop()\n");

  pthread_mutex_lock(&spotify_ctx_lock);

  download_free(download);

  if (source->evbuf)
    evbuffer_free(source->evbuf);

  source->input_ctx = NULL;
  source->evbuf = NULL;

  pthread_mutex_unlock(&spotify_ctx_lock);

  return 0;
}

static int
setup(struct input_source *source)
{
  struct global_ctx *ctx = &spotify_ctx;
  struct download_ctx *download;
  struct sp_metadata metadata;
  int probe_bytes;
  int fd;
  int ret;

  DPRINTF(E_DBG, L_SPOTIFY, "setup()\n");

  pthread_mutex_lock(&spotify_ctx_lock);

  fd = librespotc_open(source->path, ctx->session);
  if (fd < 0)
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Error opening source: %s\n", librespotc_last_errmsg());
      goto error;
    }

  ret = librespotc_metadata_get(&metadata, fd);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Error getting track metadata: %s\n", librespotc_last_errmsg());
      goto error;
    }

  // Seems we have a valid source, now setup a read + decoding context. The
  // closing of the fd is from now on part of closing the download_ctx, which is
  // done in stop().
  download = download_new(fd, source->len_ms, metadata.file_len);

  CHECK_NULL(L_SPOTIFY, source->evbuf = evbuffer_new());
  CHECK_NULL(L_SPOTIFY, source->input_ctx = download);

  source->quality = spotify_quality;

  // At this point enough bytes should be ready for transcode setup (ffmpeg probing)
  probe_bytes = fd_read(NULL, download->read_buf, fd);
  if (probe_bytes < SPOTIFY_PROBE_SIZE_MIN)
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Not enough audio data for ffmpeg probing (%d)\n", probe_bytes);
      goto error;
    }

  ret = download_xcode_setup(download);
  if (ret < 0)
    goto error;

  pthread_mutex_unlock(&spotify_ctx_lock);
  return 0;

 error:
  pthread_mutex_unlock(&spotify_ctx_lock);
  stop(source);

  return -1;
}

static int
play(struct input_source *source)
{
  struct download_ctx *download = source->input_ctx;
  size_t buflen;
  int ret;

  // Starts the download. We don't do that in setup because the player/input
  // might run seek() before starting download.
  if (!download->is_started)
    {
      librespotc_write(download->read_fd, progress_cb, download);
      download->is_started = true;
    }

  if (!download->is_ended)
    {
      ret = fd_read(&download->is_ended, download->read_buf, download->read_fd);
      if (ret < 0)
        goto error;

      buflen = evbuffer_get_length(download->read_buf);
      if (buflen < SPOTIFY_BUF_MIN)
	goto wait;
    }

  // Decode the Ogg Vorbis to PCM in chunks of 16 packets, which is pretty much
  // a randomly chosen chunk size
  ret = transcode(source->evbuf, NULL, download->xcode, 16);
  if (ret == 0)
    {
      input_write(source->evbuf, &source->quality, INPUT_FLAG_EOF);
      stop(source);
      return -1;
    }
  else if (ret < 0)
    goto error;

  ret = input_write(source->evbuf, &source->quality, 0);
  if (ret == EAGAIN)
    goto wait;

  return 0;

 error:
  input_write(NULL, NULL, INPUT_FLAG_ERROR);
  stop(source);
  return -1;

 wait:
  DPRINTF(E_DBG, L_SPOTIFY, "Waiting for data\n");
  input_wait();
  return 0;
}

static int
seek(struct input_source *source, int seek_ms)
{
  struct download_ctx *download = source->input_ctx;
  int ret;

  pthread_mutex_lock(&spotify_ctx_lock);

  // This will make transcode call back to download_seek(), but with a byte
  // offset instead of a ms position, which is what librespot-c requires
  ret = transcode_seek(download->xcode, seek_ms);

  pthread_mutex_unlock(&spotify_ctx_lock);

  return ret;
}

static int
init(void)
{
  int ret;

  pthread_mutex_lock(&spotify_ctx_lock);

  ret = initialize(&spotify_ctx);

  pthread_mutex_unlock(&spotify_ctx_lock);

  return ret;
}

static void
deinit(void)
{
  pthread_mutex_lock(&spotify_ctx_lock);

  librespotc_deinit();

  pthread_mutex_unlock(&spotify_ctx_lock);
}

struct input_definition input_spotify =
{
  .name = "Spotify",
  .type = INPUT_TYPE_SPOTIFY,
  .disabled = 0,
  .setup = setup,
  .stop = stop,
  .play = play,
  .seek = seek,
  .init = init,
  .deinit = deinit,
};


/* -------------------- Functions exposed via spotify.h --------------------- */
/*             Called from other threads than the input thread                */

static int
login(const char *username, const char *token, const char **errmsg)
{
  struct global_ctx *ctx = &spotify_ctx;
  int ret;

  pthread_mutex_lock(&spotify_ctx_lock);

  ctx->session = librespotc_login_token(username, token);
  if (!ctx->session)
    goto error;

  ret = postlogin(ctx);
  if (ret < 0)
    goto error;

  pthread_mutex_unlock(&spotify_ctx_lock);

  return 0;

 error:
  if (ctx->session)
    librespotc_logout(ctx->session);
  ctx->session = NULL;

  if (errmsg)
    *errmsg = librespotc_last_errmsg();

  pthread_mutex_unlock(&spotify_ctx_lock);

  return -1;
}

static void
logout(void)
{
  struct global_ctx *ctx = &spotify_ctx;

  db_admin_delete("spotify_username");
  db_admin_delete("spotify_stored_cred");

  pthread_mutex_lock(&spotify_ctx_lock);

  librespotc_logout(ctx->session);
  ctx->session = NULL;

  memset(&ctx->status, 0, sizeof(ctx->status));

  pthread_mutex_unlock(&spotify_ctx_lock);

  listener_notify(LISTENER_SPOTIFY);
}

static int
relogin(void)
{
  struct global_ctx *ctx = &spotify_ctx;
  char *username = NULL;
  char *db_stored_cred = NULL;
  int ret;

  pthread_mutex_lock(&spotify_ctx_lock);

  ret = initialize(ctx);
  if (ret < 0)
    goto error;

  // Re-login if we have stored credentials
  db_admin_get(&username, "spotify_username");
  db_admin_get(&db_stored_cred, "spotify_stored_cred");
  if (username && db_stored_cred)
    {
      ret = login_stored_cred(ctx, username, db_stored_cred);
      if (ret < 0)
	goto error;
    }

  free(username);
  free(db_stored_cred);

  pthread_mutex_unlock(&spotify_ctx_lock);
  return 0;

 error:
  free(username);
  free(db_stored_cred);

  pthread_mutex_unlock(&spotify_ctx_lock);
  return -1;
}

static void
status_get(struct spotify_status *status)
{
  struct global_ctx *ctx = &spotify_ctx;

  pthread_mutex_lock(&spotify_ctx_lock);

  memcpy(status->username, ctx->status.username, sizeof(status->username));
  status->logged_in = ctx->status.logged_in;
  status->installed = true;
  status->has_podcast_support = true;

  pthread_mutex_unlock(&spotify_ctx_lock);
}

struct spotify_backend spotify_librespotc =
{
  .login = login,
  .logout = logout,
  .relogin = relogin,
  .status_get = status_get,
};

