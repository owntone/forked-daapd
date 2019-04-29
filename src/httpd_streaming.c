/*
 * Copyright (C) 2015 Espen Jürgensen <espenjurgensen@gmail.com>
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include <uninorm.h>
#include <unistd.h>
#include <pthread.h>

#include <event2/event.h>

#include "httpd_streaming.h"
#include "logger.h"
#include "conffile.h"
#include "transcode.h"
#include "player.h"
#include "listener.h"
#include "db.h"

/* httpd event base, from httpd.c */
extern struct event_base *evbase_httpd;

// Seconds between sending silence when player is idle
// (to prevent client from hanging up)
#define STREAMING_SILENCE_INTERVAL 1
// How many bytes we try to read at a time from the httpd pipe
#define STREAMING_READ_SIZE STOB(352, 16, 2)

#define STREAMING_MP3_SAMPLE_RATE 44100
#define STREAMING_MP3_BPS         16
#define STREAMING_MP3_CHANNELS    2


// Linked list of mp3 streaming requests
struct streaming_session {
  struct evhttp_request *req;
  struct streaming_session *next;

  bool     icy;        // client requested icy meta
  size_t   bytes_sent; // audio bytes sent after metablock
};
static pthread_mutex_t streaming_sessions_lck;
static struct streaming_session *streaming_sessions;

// Means we're not able to encode to mp3
static bool streaming_not_supported;

// Interval for sending silence when playback is paused
static struct timeval streaming_silence_tv = { STREAMING_SILENCE_INTERVAL, 0 };

// Input buffer, output buffer and encoding ctx for transcode
static struct encode_ctx *streaming_encode_ctx;
static struct evbuffer *streaming_encoded_data;
static struct media_quality streaming_quality;

// Used for pushing events and data from the player
static struct event *streamingev;
static struct event *metaev;
static struct player_status streaming_player_status;
static int streaming_player_changed;
static int streaming_pipe[2];
static int streaming_meta[2];

#define STREAMING_ICY_METALEN_MAX 4080  // 255*16
static const short STREAMING_ICY_METAINT = 8192;
static unsigned streaming_icy;
static char *streaming_icy_title;


static void
streaming_close_cb(struct evhttp_connection *evcon, void *arg)
{
  struct streaming_session *this;
  struct streaming_session *session;
  struct streaming_session *prev;
  char *address;
  ev_uint16_t port;

  this = (struct streaming_session *)arg;

  evhttp_connection_get_peer(evcon, &address, &port);
  DPRINTF(E_INFO, L_STREAMING, "stopping mp3 streaming to %s:%d\n", address, (int)port);

  pthread_mutex_lock(&streaming_sessions_lck);
  if (streaming_sessions == NULL)
    {
      // this close comes duing deinit(), dont touch the `this` since
      // is already a dangling ptr (free'd in deinit())
      pthread_mutex_unlock(&streaming_sessions_lck);
      return;
    }

  prev = NULL;
  for (session = streaming_sessions; session; session = session->next)
    {
      if (session->req == this->req)
	break;

      prev = session;
    }

  if (!session)
    {
      DPRINTF(E_LOG, L_STREAMING, "Bug! Got a failure callback for an unknown stream (%s:%d)\n", address, (int)port);
      free(this);
    pthread_mutex_unlock(&streaming_sessions_lck);
      return;
    }

  if (!prev)
    streaming_sessions = session->next;
  else
    prev->next = session->next;

  if (session->icy)
    --streaming_icy;

  free(session);

  if (!streaming_sessions)
    {
      DPRINTF(E_INFO, L_STREAMING, "No more clients, will stop streaming\n");
      event_del(streamingev);
      event_del(metaev);
    }
    pthread_mutex_unlock(&streaming_sessions_lck);
}

static void
streaming_end(void)
{
  struct streaming_session *session;
  struct evhttp_connection *evcon;
  char *address;
  ev_uint16_t port;

  pthread_mutex_lock(&streaming_sessions_lck);
  for (session = streaming_sessions; streaming_sessions; session = streaming_sessions)
    {
      evcon = evhttp_request_get_connection(session->req);
      if (evcon)
	{
	  evhttp_connection_set_closecb(evcon, NULL, NULL);
	  evhttp_connection_get_peer(evcon, &address, &port);
	  DPRINTF(E_INFO, L_STREAMING, "force close stream to %s:%d\n", address, (int)port);
	}
      evhttp_send_reply_end(session->req);

      streaming_sessions = session->next;
      free(session);
    }
  pthread_mutex_unlock(&streaming_sessions_lck);

  event_del(streamingev);
  event_del(metaev);
}

static void
streaming_meta_cb(evutil_socket_t fd, short event, void *arg)
{
  struct media_quality mp3_quality = { STREAMING_MP3_SAMPLE_RATE, STREAMING_MP3_BPS, STREAMING_MP3_CHANNELS };
  struct media_quality quality;
  struct decode_ctx *decode_ctx;
  int ret;

  transcode_encode_cleanup(&streaming_encode_ctx);

  ret = read(fd, &quality, sizeof(struct media_quality));
  if (ret != sizeof(struct media_quality))
    goto error;

  decode_ctx = NULL;
  if (quality.bits_per_sample == 16)
    decode_ctx = transcode_decode_setup_raw(XCODE_PCM16, &quality);
  else if (quality.bits_per_sample == 24)
    decode_ctx = transcode_decode_setup_raw(XCODE_PCM24, &quality);
  else if (quality.bits_per_sample == 32)
    decode_ctx = transcode_decode_setup_raw(XCODE_PCM32, &quality);

  if (!decode_ctx)
    goto error;

  streaming_encode_ctx = transcode_encode_setup(XCODE_MP3, &mp3_quality, decode_ctx, NULL, 0, 0);
  transcode_decode_cleanup(&decode_ctx);
  if (!streaming_encode_ctx)
    {
      DPRINTF(E_LOG, L_STREAMING, "Will not be able to stream MP3, libav does not support MP3 encoding\n");
      streaming_not_supported = 1;
      return;
    }

  streaming_quality = quality;
  streaming_not_supported = 0;

  return;

 error:
  DPRINTF(E_LOG, L_STREAMING, "Unknown or unsupported quality of input data (%d/%d/%d), cannot MP3 encode\n", quality.sample_rate, quality.bits_per_sample, quality.channels);
  streaming_not_supported = 1;
  streaming_end();
}

static int
encode_buffer(uint8_t *buffer, size_t size)
{
  transcode_frame *frame;
  int samples;
  int ret;

  if (streaming_not_supported)
    {
      DPRINTF(E_LOG, L_STREAMING, "Streaming unsupported\n");
      return -1;
    }

  if (streaming_quality.channels == 0)
    {
      DPRINTF(E_LOG, L_STREAMING, "Streaming quality is zero (%d/%d/%d)\n", streaming_quality.sample_rate, streaming_quality.bits_per_sample, streaming_quality.channels);
      return -1;
    }

  samples = BTOS(size, streaming_quality.bits_per_sample, streaming_quality.channels);

  frame = transcode_frame_new(buffer, size, samples, &streaming_quality);
  if (!frame)
    {
      DPRINTF(E_LOG, L_STREAMING, "Could not convert raw PCM to frame\n");
      return -1;
    }

  ret = transcode_encode(streaming_encoded_data, streaming_encode_ctx, frame, 0);
  transcode_frame_free(frame);

  return ret;
}

/* we know that the icymeta is limited to 1+255*16 == 4081 bytes so caller must
 * provide a buf of this size
 *
 * the icy meta block is defined by a single byte indicating how many dbl byte 
 * words used for the actual meta.  unused bytes null padded
 *
 * https://stackoverflow.com/questions/4911062/pulling-track-info-from-an-audio-stream-using-php/4914538#4914538
 * http://www.smackfu.com/stuff/programming/shoutcast.html
 */
static uint8_t *
streaming_icy_meta_create(uint8_t buf[STREAMING_ICY_METALEN_MAX+1], const char *title, unsigned *buflen)
{
  static const char *head = "StreamTitle='";
  static const char *tail = "';";

  unsigned titlelen = 0;
  unsigned metalen = 0;
  uint8_t no16s;

  *buflen = 0;

  if (title == NULL)
    {
      no16s = 0;
      memcpy(buf, &no16s, 1);

      *buflen = 1;
    }
  else
    {
      titlelen = strlen(title);
      if (titlelen > STREAMING_ICY_METALEN_MAX)
	titlelen = STREAMING_ICY_METALEN_MAX;  // dont worry about the null byte

      // 1x byte len following by how many 16 byte words needed
      no16s = (15 + titlelen)/16 +1;
      metalen = 1 + no16s*16;
      memset(buf, 0, metalen);

      memcpy(buf, &no16s, 1);
      memcpy(&buf[1], head, 13);
      memcpy(&buf[14], title, titlelen);
      memcpy(&buf[14+titlelen], tail, 2);

      *buflen = metalen;
    }

  return buf;
}

static uint8_t *
streaming_icy_meta_splice(uint8_t *data, size_t datalen, off_t offset, size_t *len)
{
  uint8_t  meta[STREAMING_ICY_METALEN_MAX+1];  // static buffer, of max sz, for the created icymeta
  unsigned metalen;     // how much of the static buffer is use
  uint8_t *buf;         // buffer to contain the audio data (data) spliced w/meta (meta)

  if (data == NULL || datalen == 0)
    return NULL;

  memset(meta, 0, sizeof(meta));
  streaming_icy_meta_create(meta, streaming_icy_title, &metalen);

  *len = datalen + metalen;
  // DPRINTF(E_DBG, L_STREAMING, "splicing meta, audio block=%d bytes, offset=%d, metalen=%d new buflen=%d\n", datalen, offset, metalen, *len);
  buf = malloc(*len);
  memcpy(buf, data, offset);
  memcpy(&buf[offset], &meta[0], metalen);
  memcpy(&buf[offset+metalen], &data[offset], datalen-offset);

  return buf;
}

static void
streaming_player_status_update()
{
  unsigned x, y;
  struct db_queue_item *queue_item = NULL;
  struct player_status  tmp;

  tmp.id = streaming_player_status.id;
  player_get_status(&streaming_player_status);

  if (tmp.id != streaming_player_status.id && streaming_icy)
    {
      free(streaming_icy_title);
      if ( (queue_item = db_queue_fetch_byfileid(streaming_player_status.id)) == NULL)
	{
	  streaming_icy_title = NULL;
	}
      else
	{
	  x = strlen(queue_item->title);
	  y = strlen(queue_item->artist);
	  if (x && y)
	    {
	      streaming_icy_title = malloc(x+y+4);
	      snprintf(streaming_icy_title, x+y+4, "%s - %s", queue_item->title, queue_item->artist);
	    }
	  else
	    {
	      streaming_icy_title = strdup( x ? queue_item->title : queue_item->artist);
	    }
	  free_queue_item(queue_item, 0);
	}
    }
}

static void
streaming_send_cb(evutil_socket_t fd, short event, void *arg)
{
  struct streaming_session *session;
  struct evbuffer *evbuf;
  uint8_t rawbuf[STREAMING_READ_SIZE];
  uint8_t *buf;
  uint8_t *splice_buf = NULL;
  size_t splice_len;
  size_t count;
  int overflow;
  int len;
  int ret;

  // Player wrote data to the pipe (EV_READ)
  if (event & EV_READ)
    {
      while (1)
	{
	  ret = read(fd, &rawbuf, sizeof(rawbuf));
	  if (ret <= 0)
	    break;

	  if (streaming_player_changed)
	    {
	      streaming_player_changed = 0;
	      streaming_player_status_update();
	    }

	  ret = encode_buffer(rawbuf, ret);
	  if (ret < 0)
	    return;
	}
    }
  // Event timed out, let's see what the player is doing and send silence if it is paused
  else
    {
      if (streaming_player_changed)
	{
	  streaming_player_changed = 0;
	  streaming_player_status_update();
	}

      if (streaming_player_status.status != PLAY_PAUSED)
	return;

      memset(&rawbuf, 0, sizeof(rawbuf));
      ret = encode_buffer(rawbuf, sizeof(rawbuf));
      if (ret < 0)
	return;
    }

  len = evbuffer_get_length(streaming_encoded_data);
  if (len == 0)
    return;

  // Send data
  evbuf = evbuffer_new();
  pthread_mutex_lock(&streaming_sessions_lck);
  for (session = streaming_sessions; session; session = session->next)
    {
      // does this session want ICY and it is time to send..
      count = session->bytes_sent+len;
      if (session->icy && count > STREAMING_ICY_METAINT)
	{
	  overflow = count%STREAMING_ICY_METAINT;
	  buf = evbuffer_pullup(streaming_encoded_data, -1);

	  // DPRINTF(E_DBG, L_STREAMING, "session=%x sent=%ld len=%ld overflow=%ld\n", session, session->bytes_sent, len, overflow);

	  // splice in icy title with encoded audio data
	  splice_len = 0;
	  splice_buf = streaming_icy_meta_splice(buf, len, len-overflow, &splice_len);

	  evbuffer_add(evbuf, splice_buf, splice_len);

	  free(splice_buf);
	  splice_buf = NULL;

	  evhttp_send_reply_chunk(session->req, evbuf);

	  if (session->next == NULL)
	    {
	      // we're the last session, drop the contents of the encoded buffer
	      evbuffer_drain(streaming_encoded_data, len);
	    }
	  session->bytes_sent = overflow;
	}
      else
	{
	  if (session->next)
	    {
	      buf = evbuffer_pullup(streaming_encoded_data, -1);
	      evbuffer_add(evbuf, buf, len);
	      evhttp_send_reply_chunk(session->req, evbuf);
	    }
	  else
	    {
	      evhttp_send_reply_chunk(session->req, streaming_encoded_data);
	    }
	  session->bytes_sent += len;
	}
    }
  pthread_mutex_unlock(&streaming_sessions_lck);

  evbuffer_free(evbuf);
}

// Thread: player (not fully thread safe, but hey...)
static void
player_change_cb(short event_mask)
{
  streaming_player_changed = 1;
}

// Thread: player (also prone to race conditions, mostly during deinit)
void
streaming_write(struct output_buffer *obuf)
{
  int ret;

  // explicit no-lock - let the write to pipes fail if deinit
  if (!streaming_sessions)
    return;

  if (!quality_is_equal(&obuf->data[0].quality, &streaming_quality))
    {
      ret = write(streaming_meta[1], &obuf->data[0].quality, sizeof(struct media_quality));
      if (ret < 0)
	{
	  if (errno == EBADF)
	    DPRINTF(E_LOG, L_STREAMING, "streaming pipe already closed\n");
	  else
	    DPRINTF(E_LOG, L_STREAMING, "Error writing to streaming pipe: %s\n", strerror(errno));
	  return;
	}
    }

  ret = write(streaming_pipe[1], obuf->data[0].buffer, obuf->data[0].bufsize);
  if (ret < 0)
    {
      if (errno == EAGAIN)
	DPRINTF(E_WARN, L_STREAMING, "Streaming pipe full, skipping write\n");
      else
	{
	  if (errno == EBADF)
	    DPRINTF(E_LOG, L_STREAMING, "streaming pipe already closed\n");
	  else
	    DPRINTF(E_LOG, L_STREAMING, "Error writing to streaming pipe: %s\n", strerror(errno));
	}
    }
}

int
streaming_request(struct evhttp_request *req, struct httpd_uri_parsed *uri_parsed)
{
  struct streaming_session *session;
  struct evhttp_connection *evcon;
  struct evkeyvalq *output_headers;
  cfg_t *lib;
  const char *name;
  char *address;
  ev_uint16_t port;
  const char *param;
  bool wanticy = false;
  char buf[9];

  if (streaming_not_supported)
    {
      DPRINTF(E_LOG, L_STREAMING, "Got MP3 streaming request, but cannot encode to MP3\n");

      evhttp_send_error(req, HTTP_NOTFOUND, "Not Found");
      return -1;
    }

  evcon = evhttp_request_get_connection(req);
  evhttp_connection_get_peer(evcon, &address, &port);
  param = evhttp_find_header( evhttp_request_get_input_headers(req), "Icy-MetaData");
  if (param && strcmp(param, "1") == 0)
    wanticy = true;

  DPRINTF(E_INFO, L_STREAMING, "Beginning mp3 streaming (with icy=%d) to %s:%d\n", wanticy, address, (int)port);

  lib = cfg_getsec(cfg, "library");
  name = cfg_getstr(lib, "name");

  output_headers = evhttp_request_get_output_headers(req);
  evhttp_add_header(output_headers, "Content-Type", "audio/mpeg");
  evhttp_add_header(output_headers, "Server", "forked-daapd/" VERSION);
  evhttp_add_header(output_headers, "Cache-Control", "no-cache");
  evhttp_add_header(output_headers, "Pragma", "no-cache");
  evhttp_add_header(output_headers, "Expires", "Mon, 31 Aug 2015 06:00:00 GMT");
  if (wanticy) 
    {
      ++streaming_icy;
      evhttp_add_header(output_headers, "icy-name", name);
      snprintf(buf, sizeof(buf)-1, "%d", STREAMING_ICY_METAINT);
      evhttp_add_header(output_headers, "icy-metaint", buf);
    }
  evhttp_add_header(output_headers, "Access-Control-Allow-Origin", "*");
  evhttp_add_header(output_headers, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");

  evhttp_send_reply_start(req, HTTP_OK, "OK");

  session = calloc(1, sizeof(struct streaming_session));
  if (!session)
    {
      DPRINTF(E_LOG, L_STREAMING, "Out of memory for streaming request\n");

      evhttp_send_error(req, HTTP_SERVUNAVAIL, "Internal Server Error");
      return -1;
    }

  pthread_mutex_lock(&streaming_sessions_lck);

  if (!streaming_sessions)
    {
      event_add(streamingev, &streaming_silence_tv);
      event_add(metaev, NULL);
    }

  session->req = req;
  session->next = streaming_sessions;
  session->icy = wanticy;
  session->bytes_sent = 0;
  streaming_sessions = session;

  pthread_mutex_unlock(&streaming_sessions_lck);

  evhttp_connection_set_closecb(evcon, streaming_close_cb, session);

  return 0;
}

int
streaming_is_request(const char *path)
{
  char *ptr;

  ptr = strrchr(path, '/');
  if (ptr && (strcasecmp(ptr, "/stream.mp3") == 0))
    return 1;

  return 0;
}

int
streaming_init(void)
{
  int ret;

  pthread_mutex_init(&streaming_sessions_lck, NULL);

  // Non-blocking because otherwise httpd and player thread may deadlock
#ifdef HAVE_PIPE2
  ret = pipe2(streaming_pipe, O_CLOEXEC | O_NONBLOCK);
#else
  if ( pipe(streaming_pipe) < 0 ||
       fcntl(streaming_pipe[0], F_SETFL, O_CLOEXEC | O_NONBLOCK) < 0 ||
       fcntl(streaming_pipe[1], F_SETFL, O_CLOEXEC | O_NONBLOCK) < 0 )
    ret = -1;
  else
    ret = 0;
#endif
  if (ret < 0)
    {
      DPRINTF(E_FATAL, L_STREAMING, "Could not create pipe: %s\n", strerror(errno));
      goto error;
    }

#ifdef HAVE_PIPE2
  ret = pipe2(streaming_meta, O_CLOEXEC | O_NONBLOCK);
#else
  if ( pipe(streaming_meta) < 0 ||
       fcntl(streaming_meta[0], F_SETFL, O_CLOEXEC | O_NONBLOCK) < 0 ||
       fcntl(streaming_meta[1], F_SETFL, O_CLOEXEC | O_NONBLOCK) < 0 )
    ret = -1;
  else
    ret = 0;
#endif
  if (ret < 0)
    {
      DPRINTF(E_FATAL, L_STREAMING, "Could not create pipe: %s\n", strerror(errno));
      goto error;
    }

  // Listen to playback changes so we don't have to poll to check for pausing
  ret = listener_add(player_change_cb, LISTENER_PLAYER);
  if (ret < 0)
    {
      DPRINTF(E_FATAL, L_STREAMING, "Could not add listener\n");
      goto error;
    }

  // Initialize buffer for encoded mp3 audio and event for pipe reading
  CHECK_NULL(L_STREAMING, streaming_encoded_data = evbuffer_new());

  CHECK_NULL(L_STREAMING, streamingev = event_new(evbase_httpd, streaming_pipe[0], EV_TIMEOUT | EV_READ | EV_PERSIST, streaming_send_cb, NULL));
  CHECK_NULL(L_STREAMING, metaev = event_new(evbase_httpd, streaming_meta[0], EV_READ | EV_PERSIST, streaming_meta_cb, NULL));

  streaming_icy = 0;
  streaming_icy_title = NULL;

  return 0;

 error:
  close(streaming_pipe[0]);
  close(streaming_pipe[1]);
  close(streaming_meta[0]);
  close(streaming_meta[1]);

  return -1;
}

void
streaming_deinit(void)
{
  streaming_end();

  event_free(streamingev);
  streamingev = NULL;

  listener_remove(player_change_cb);

  close(streaming_pipe[0]);
  close(streaming_pipe[1]);
  close(streaming_meta[0]);
  close(streaming_meta[1]);

  transcode_encode_cleanup(&streaming_encode_ctx);
  evbuffer_free(streaming_encoded_data);
  free(streaming_icy_title);

  pthread_mutex_destroy(&streaming_sessions_lck);
}
