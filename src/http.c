/*
 * Copyright (C) 2016 Espen Jürgensen <espenjurgensen@gmail.com>
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
#include <unistd.h>
#include <uniconv.h>
#include <unistr.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

#include <libavutil/opt.h>

#include <event2/event.h>
#include <event2/keyvalq_struct.h>

#include <curl/curl.h>
#include <pthread.h>

#include "conffile.h"
#include "http.h"
#include "logger.h"
#include "misc.h"

/* Formats we can read so far */
#define PLAYLIST_UNK 0
#define PLAYLIST_PLS 1
#define PLAYLIST_M3U 2

/* ======================= libevent HTTP client  =============================*/

// Number of seconds the client will wait for a response before aborting
#define HTTP_CLIENT_TIMEOUT 8

struct http_client_session {
  CURL *curl;
  const char *user_agent;
  long verifypeer;
  long timeout_sec;
  pthread_mutex_t mutex;
};

struct http_client_session *
http_client_session_new(void)
{
  struct http_client_session *session;
  CHECK_NULL(L_HTTP, session = calloc(1, sizeof(struct http_client_session)));
  CHECK_NULL(L_HTTP, session->curl = curl_easy_init());
  session->user_agent = cfg_getstr(cfg_getsec(cfg, "general"), "user_agent");
  session->verifypeer = cfg_getbool(cfg_getsec(cfg, "general"), "ssl_verifypeer");
  session->timeout_sec = HTTP_CLIENT_TIMEOUT;
  pthread_mutex_init(&session->mutex, NULL);
  return session;
}

void
http_client_session_free(struct http_client_session *session)
{
  curl_easy_cleanup(session->curl);
  pthread_mutex_destroy(&session->mutex);
  free(session);
}

static void
curl_headers_save(struct keyval *kv, CURL *curl)
{
  char *content_type;
  int ret;

  if (!kv || !curl)
    return;

  ret = curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);
  if (ret == CURLE_OK && content_type)
    {
      keyval_add(kv, "Content-Type", content_type);
    }
}

static size_t
curl_request_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
  size_t realsize;
  struct http_client_ctx *ctx;
  int ret;

  realsize = size * nmemb;
  ctx = (struct http_client_ctx *)userdata;

  if (!ctx->input_body)
    return realsize;

  ret = evbuffer_add(ctx->input_body, ptr, realsize);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_HTTP, "Error adding reply from %s to input buffer\n", ctx->url);
      return 0;
    }

  return realsize;
}

int
http_client_request(struct http_client_ctx *ctx, struct http_client_session *client_session)
{
  struct http_client_session *session;
  CURLcode res;
  struct curl_slist *headers = NULL;
  struct onekeyval *okv;
  char header[1024];
  long response_code;
  int ret;

  if (!client_session)
    {
      session = http_client_session_new();
    }
  else
    {
      session = client_session;

      CHECK_ERR(L_HTTP, pthread_mutex_lock(&session->mutex));
      curl_easy_reset(session->curl);
    }

  curl_easy_setopt(session->curl, CURLOPT_USERAGENT, session->user_agent);
  curl_easy_setopt(session->curl, CURLOPT_URL, ctx->url);
  curl_easy_setopt(session->curl, CURLOPT_SSL_VERIFYPEER, session->verifypeer);

  if (ctx->output_headers)
    {
      for (okv = ctx->output_headers->head; okv; okv = okv->next)
	{
	  ret = snprintf(header, sizeof(header), "%s: %s", okv->name, okv->value);
	  if (ret < 0 || ret >= sizeof(header))
	    {
	      DPRINTF(E_LOG, L_HTTP, "Could not add header, value has more than %zd chars: '%s: %s'\n", sizeof(header),
	          okv->name, okv->value);
	      res = CURLE_FAILED_INIT;
	      goto out;
	    }
	  headers = curl_slist_append(headers, header);
        }

      curl_easy_setopt(session->curl, CURLOPT_HTTPHEADER, headers);
    }

  if (ctx->output_body)
    curl_easy_setopt(session->curl, CURLOPT_POSTFIELDS, ctx->output_body); // POST request

  curl_easy_setopt(session->curl, CURLOPT_TIMEOUT, session->timeout_sec);
  curl_easy_setopt(session->curl, CURLOPT_WRITEFUNCTION, curl_request_cb);
  curl_easy_setopt(session->curl, CURLOPT_WRITEDATA, ctx);

  // Artwork and playlist requests might require redirects
  curl_easy_setopt(session->curl, CURLOPT_FOLLOWLOCATION, 1);
  curl_easy_setopt(session->curl, CURLOPT_MAXREDIRS, 5);

  // Make request
  DPRINTF(E_INFO, L_HTTP, "Making request for %s\n", ctx->url);

  res = curl_easy_perform(session->curl);

  if (res != CURLE_OK)
    {
      DPRINTF(E_WARN, L_HTTP, "Request to %s failed: %s\n", ctx->url, curl_easy_strerror(res));
      goto out;
    }

  curl_easy_getinfo(session->curl, CURLINFO_RESPONSE_CODE, &response_code);
  ctx->response_code = (int) response_code;
  curl_headers_save(ctx->input_headers, session->curl);

out:
  if (client_session)
    {
      CHECK_ERR(L_HTTP, pthread_mutex_unlock(&session->mutex));
    }
  else
    {
      http_client_session_free(session);
    }
  curl_slist_free_all(headers);

  return res == CURLE_OK ? 0 : -1;
}

int
http_form_urldecode(struct keyval *kv, const char *uri)
{
  struct evhttp_uri *ev_uri = NULL;
  struct evkeyvalq ev_query = { 0 };
  struct evkeyval *param;
  const char *query;
  int ret;

  ev_uri = evhttp_uri_parse_with_flags(uri, EVHTTP_URI_NONCONFORMANT);
  if (!ev_uri)
    return -1;

  query = evhttp_uri_get_query(ev_uri);
  if (!query)
    goto error;

  ret = evhttp_parse_query_str(query, &ev_query);
  if (ret < 0)
    goto error;

  // musl libc doesn't have sys/queue.h so don't use TAILQ_FOREACH
  for (param = ev_query.tqh_first; param; param = param->next.tqe_next)
    keyval_add(kv, param->key, param->value);

  evhttp_uri_free(ev_uri);
  evhttp_clear_headers(&ev_query);
  return 0;

 error:
  evhttp_uri_free(ev_uri);
  evhttp_clear_headers(&ev_query);
  return -1;
}

char *
http_form_urlencode(struct keyval *kv)
{
  struct evbuffer *evbuf;
  struct onekeyval *okv;
  char *body;
  char *k;
  char *v;

  evbuf = evbuffer_new();

  for (okv = kv->head; okv; okv = okv->next)
    {
      k = evhttp_encode_uri(okv->name);
      if (!k)
        continue;

      v = evhttp_encode_uri(okv->value);
      if (!v)
	{
	  free(k);
	  continue;
	}

      evbuffer_add_printf(evbuf, "%s=%s", k, v);
      if (okv->next)
	evbuffer_add_printf(evbuf, "&");

      free(k);
      free(v);
    }

  evbuffer_add(evbuf, "\n", 1);

  body = evbuffer_readln(evbuf, NULL, EVBUFFER_EOL_ANY);

  evbuffer_free(evbuf);

  DPRINTF(E_DBG, L_HTTP, "Parameters in request are: %s\n", body);

  return body;
}

int
http_stream_setup(char **stream, const char *url)
{
  CURLU *url_handle;
  CURLUcode rc;
  struct http_client_ctx ctx;
  struct evbuffer *evbuf;
  char *path;
  const char *ext;
  char *line;
  char *pos;
  int ret;
  int n;
  int pl_format;
  bool in_playlist;

  *stream = NULL;

  CHECK_NULL(L_HTTP, url_handle = curl_url());

  rc = curl_url_set(url_handle, CURLUPART_URL, url, 0);
  if (rc != 0)
    {
      DPRINTF(E_LOG, L_HTTP, "Couldn't parse internet playlist: '%s'\n", url);
      curl_url_cleanup(url_handle);
      return -1;
    }

  rc = curl_url_get(url_handle, CURLUPART_PATH, &path, 0);
  if (rc != 0)
    {
      DPRINTF(E_LOG, L_HTTP, "Couldn't find internet playlist path: '%s'\n", url);
      curl_url_cleanup(url_handle);
      return -1;
    }

  // path does not include query or fragment, so should work with any url's
  // e.g. http://yp.shoutcast.com/sbin/tunein-station.pls?id=99179772#Air Jazz
  pl_format = PLAYLIST_UNK;
  if (path && (ext = strrchr(path, '.')))
    {
      if (strcasecmp(ext, ".m3u") == 0)
	pl_format = PLAYLIST_M3U;
      else if (strcasecmp(ext, ".pls") == 0)
	pl_format = PLAYLIST_PLS;
    }

  curl_free(path);
  curl_url_cleanup(url_handle);


  if (pl_format==PLAYLIST_UNK)
    {
      *stream = strdup(url);
      return 0;
    }

  // It was a m3u or pls playlist, so now retrieve it
  memset(&ctx, 0, sizeof(struct http_client_ctx));

  evbuf = evbuffer_new();
  if (!evbuf)
    return -1;

  ctx.url = url;
  ctx.input_body = evbuf;

  ret = http_client_request(&ctx, NULL);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_HTTP, "Couldn't fetch internet playlist: %s\n", url);

      evbuffer_free(evbuf);
      return -1;
    }

  // Pad with CRLF because evbuffer_readln() might not read the last line otherwise
  evbuffer_add(ctx.input_body, "\r\n", 2);

  /* Read the playlist until the first stream link is found, but give up if
   * nothing is found in the first 10 lines
   */
  in_playlist = false;
  n = 0;
  while ((line = evbuffer_readln(ctx.input_body, NULL, EVBUFFER_EOL_ANY)) && (n < 10))
    {
      // Skip comments and blank lines without counting for the limit
      if (pl_format == PLAYLIST_M3U && (line[0] == '#' || line[0] == '\0'))
	goto line_done;

      n++;

      if (pl_format == PLAYLIST_PLS && !in_playlist)
	{
	  if (strncasecmp(line, "[playlist]", strlen("[playlist]")) == 0)
	    {
	      in_playlist = true;
	      n = 0;
	    }
	  goto line_done;
	}

      if (pl_format == PLAYLIST_PLS)
	{
	  pos = line;
	  while (*pos == ' ')
	    ++pos;

          // We are only interested in `FileN=http://foo/bar.mp3` entries
	  if (strncasecmp(pos, "file", strlen("file")) != 0)
	    goto line_done;

	  while (*pos != '=' && *pos != '\0')
	    ++pos;

	  if (*pos == '\0')
	    goto line_done;

	  ++pos;
	  while (*pos == ' ')
	    ++pos;
        
	  // allocate the value part and proceed as with m3u
	  pos = strdup(pos);
	  free(line);
	  line = pos;
	}

      if (net_is_http_or_https(line))
	{
	  DPRINTF(E_DBG, L_HTTP, "Found internet playlist stream (line %d): %s\n", n, line);

	  n = -1;
	  break;
	}

    line_done:
      free(line);
    }

  evbuffer_free(ctx.input_body);

  if (n != -1)
    {
      DPRINTF(E_LOG, L_HTTP, "Couldn't find stream in internet playlist: %s\n", url);

      return -1;
    }

  *stream = line;

  return 0;
}


/* ======================= ICY metadata handling =============================*/


static int
metadata_packet_get(struct http_icy_metadata *metadata, AVFormatContext *fmtctx)
{
  uint8_t *buffer;
  uint8_t *utf;
  char *icy_token;
  char *save_pr;
  char *ptr;
  char *end;

  av_opt_get(fmtctx, "icy_metadata_packet", AV_OPT_SEARCH_CHILDREN, &buffer);
  if (!buffer)
    return -1;

  /* Some servers send ISO-8859-1 instead of UTF-8 */
  if (u8_check(buffer, strlen((char *)buffer)))
    {
      utf = u8_strconv_from_encoding((char *)buffer, "ISO−8859−1", iconveh_question_mark);
      av_free(buffer);
      if (utf == NULL)
        return -1;
    }
  else
    utf = buffer;

  icy_token = strtok_r((char *)utf, ";", &save_pr);
  while (icy_token != NULL)
    {
      ptr = strchr(icy_token, '=');
      if (!ptr || (ptr[1] == '\0'))
	{
	  icy_token = strtok_r(NULL, ";", &save_pr);
	  continue;
	}

      ptr++;
      if (ptr[0] == '\'')
	ptr++;

      end = strrchr(ptr, '\'');
      if (end)
        *end = '\0';

      if ((strncmp(icy_token, "StreamTitle", strlen("StreamTitle")) == 0) && !metadata->title)
	{
	  metadata->title = ptr;

	  /* Dash separates artist from title, if no dash assume all is title */
	  ptr = strstr(ptr, " - ");
	  if (ptr)
	    {
	      *ptr = '\0';
	      metadata->artist = strdup(metadata->title);
	      *ptr = ' ';

	      metadata->title = strdup(ptr + 3);
	    }
	  else if (strlen(metadata->title) == 0)
	    metadata->title = NULL;
	  else
	    metadata->title = strdup(metadata->title);
	}
      else if ((strncmp(icy_token, "StreamUrl", strlen("StreamUrl")) == 0) && !metadata->url && strlen(ptr) > 0)
	{
	  metadata->url = strdup(ptr);
	}

      if (end)
	*end = '\'';

      icy_token = strtok_r(NULL, ";", &save_pr);
    }
  av_free(utf);

  if (metadata->title)
    metadata->hash = djb_hash(metadata->title, strlen(metadata->title));

  return 0;
}

static int
metadata_header_get(struct http_icy_metadata *metadata, AVFormatContext *fmtctx)
{
  uint8_t *buffer;
  uint8_t *utf;
  char *icy_token;
  char *save_pr;
  char *ptr;

  av_opt_get(fmtctx, "icy_metadata_headers", AV_OPT_SEARCH_CHILDREN, &buffer);
  if (!buffer)
    return -1;

  /* Headers are ascii or iso-8859-1 according to:
   * http://www.w3.org/Protocols/rfc2616/rfc2616-sec2.html#sec2.2
   */
  utf = u8_strconv_from_encoding((char *)buffer, "ISO−8859−1", iconveh_question_mark);
  av_free(buffer);
  if (!utf)
    return -1;

  icy_token = strtok_r((char *)utf, "\r\n", &save_pr);
  while (icy_token != NULL)
    {
      ptr = strchr(icy_token, ':');
      if (!ptr || (ptr[1] == '\0'))
	{
	  icy_token = strtok_r(NULL, "\r\n", &save_pr);
	  continue;
	}

      ptr++;
      if (ptr[0] == ' ')
	ptr++;

      if ((strncmp(icy_token, "icy-name", strlen("icy-name")) == 0) && ptr[0] != '\0' && !metadata->name)
	metadata->name = strdup(ptr);
      else if ((strncmp(icy_token, "icy-description", strlen("icy-description")) == 0) && ptr[0] != '\0' && !metadata->description)
	metadata->description = strdup(ptr);
      else if ((strncmp(icy_token, "icy-genre", strlen("icy-genre")) == 0) && ptr[0] != '\0' && !metadata->genre)
	metadata->genre = strdup(ptr);

      icy_token = strtok_r(NULL, "\r\n", &save_pr);
    }
  free(utf);

  return 0;
}

struct http_icy_metadata *
http_icy_metadata_get(AVFormatContext *fmtctx, int packet_only)
{
  struct http_icy_metadata *metadata;
  int got_packet;
  int got_header;

  CHECK_NULL(L_HTTP, metadata = calloc(1, sizeof(struct http_icy_metadata)));

  got_packet = (metadata_packet_get(metadata, fmtctx) == 0);
  got_header = (!packet_only) && (metadata_header_get(metadata, fmtctx) == 0);

  if (!got_packet && !got_header)
   {
     free(metadata);
     return NULL;
   }

/*  DPRINTF(E_DBG, L_HTTP, "Found ICY: N %s, D %s, G %s, T %s, A %s, U %s, I %" PRIu32 "\n",
	metadata->name,
	metadata->description,
	metadata->genre,
	metadata->title,
	metadata->artist,
	metadata->url,
	metadata->hash
	);
*/
  return metadata;
}

void
http_icy_metadata_free(struct http_icy_metadata *metadata, int content_only)
{
  if (!metadata)
    return;

  free(metadata->name);
  free(metadata->description);
  free(metadata->genre);
  free(metadata->title);
  free(metadata->artist);
  free(metadata->url);
  free(metadata);
}
