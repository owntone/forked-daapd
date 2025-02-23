/*
 * Copyright (C) 2016 Espen Jürgensen <espenjurgensen@gmail.com>
 * Copyright (C) 2016 Christian Meffert <christian.meffert@googlemail.com>
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

#include "spotify_webapi.h"

#include <event2/event.h>
#include <json.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "artwork.h"
#include "cache.h"
#include "conffile.h"
#include "db.h"
#include "http.h"
#include "library.h"
#include "listener.h"
#include "logger.h"
#include "misc_json.h"
#include "inputs/spotify.h"


enum spotify_request_type {
  SPOTIFY_REQUEST_TYPE_DEFAULT,
  SPOTIFY_REQUEST_TYPE_RESCAN,
  SPOTIFY_REQUEST_TYPE_METARESCAN,
};

enum spotify_item_type {
  SPOTIFY_ITEM_TYPE_ALBUM,
  SPOTIFY_ITEM_TYPE_ARTIST,
  SPOTIFY_ITEM_TYPE_TRACK,
  SPOTIFY_ITEM_TYPE_PLAYLIST,
  SPOTIFY_ITEM_TYPE_SHOW,
  SPOTIFY_ITEM_TYPE_EPISODE,

  SPOTIFY_ITEM_TYPE_UNKNOWN,
};

struct spotify_album
{
  const char *added_at;
  time_t mtime;

  const char *album_type;
  bool is_compilation;
  const char *artist;
  const char *genre;
  const char *id;
  const char *label;
  const char *name;
  const char *release_date;
  const char *release_date_precision;
  time_t release_date_time;
  int release_year;
  const char *uri;
  const char *artwork_url;
  const char *type;
};

struct spotify_track
{
  const char *added_at;
  time_t mtime;

  const char *album;
  const char *album_artist;
  const char *artist;
  int disc_number;
  const char *album_type;
  bool is_compilation;
  int duration_ms;
  const char *id;
  const char *name;
  int track_number;
  const char *release_date;
  const char *release_date_precision;
  time_t release_date_time;
  int release_year;
  const char *uri;
  const char *artwork_url;

  bool is_playable;
  const char *restrictions;
  const char *linked_from_uri;
  const char *type;
};

struct spotify_playlist
{
  const char *id;
  const char *name;
  const char *owner;
  const char *uri;

  const char *href;

  const char *tracks_href;
  int tracks_count;
};

// Credentials for the web api
struct spotify_credentials
{
  char *access_token;
  char *refresh_token;
  char *granted_scope;
  char *user_country;
  char *user;

  int32_t token_expires_in;
  time_t token_time_requested;
};

struct http_client_session *session;
static struct spotify_credentials spotify_credentials;
static pthread_mutex_t spotify_credentials_lock = PTHREAD_MUTEX_INITIALIZER;

// The base playlist id for all Spotify playlists in the db
static int spotify_base_plid;

// Flag to avoid triggering playlist change events while the (re)scan is running
static bool scanning;


// Endpoints and credentials for the web api
static const char *spotify_client_id     = "0e684a5422384114a8ae7ac020f01789";
static const char *spotify_client_secret = "232af95f39014c9ba218285a5c11a239";
static const char *spotify_scope         = "playlist-read-private playlist-read-collaborative user-library-read user-read-private streaming";

static const char *spotify_auth_uri      = "https://accounts.spotify.com/authorize";
static const char *spotify_token_uri     = "https://accounts.spotify.com/api/token";

static const char *spotify_track_uri           = "https://api.spotify.com/v1/tracks/%s";
static const char *spotify_me_uri              = "https://api.spotify.com/v1/me";
static const char *spotify_albums_uri          = "https://api.spotify.com/v1/me/albums?limit=50";
static const char *spotify_album_uri           = "https://api.spotify.com/v1/albums/%s";
static const char *spotify_album_tracks_uri    = "https://api.spotify.com/v1/albums/%s/tracks";
static const char *spotify_playlists_uri       = "https://api.spotify.com/v1/me/playlists?limit=50";
static const char *spotify_playlist_tracks_uri = "https://api.spotify.com/v1/playlists/%s/tracks";
static const char *spotify_artist_albums_uri   = "https://api.spotify.com/v1/artists/%s/albums?include_groups=album,single";
static const char *spotify_shows_uri           = "https://api.spotify.com/v1/me/shows?limit=50";
static const char *spotify_shows_episodes_uri  = "https://api.spotify.com/v1/shows/%s/episodes";
static const char *spotify_episode_uri         = "https://api.spotify.com/v1/episodes/%s";


static enum spotify_item_type
parse_type_from_uri(const char *uri)
{
  if (strncasecmp(uri, "spotify:track:", strlen("spotify:track:")) == 0)
    {
      return SPOTIFY_ITEM_TYPE_TRACK;
    }
  else if (strncasecmp(uri, "spotify:artist:", strlen("spotify:artist:")) == 0)
    {
      return SPOTIFY_ITEM_TYPE_ARTIST;
    }
  else if (strncasecmp(uri, "spotify:album:", strlen("spotify:album:")) == 0)
    {
      return SPOTIFY_ITEM_TYPE_ALBUM;
    }
  else if (strncasecmp(uri, "spotify:show:", strlen("spotify:show:")) == 0)
    {
      return SPOTIFY_ITEM_TYPE_SHOW;
    }
  else if (strncasecmp(uri, "spotify:episode:", strlen("spotify:episode:")) == 0)
    {
      return SPOTIFY_ITEM_TYPE_EPISODE;
    }
  else if (strncasecmp(uri, "spotify:", strlen("spotify:")) == 0 && strstr(uri, "playlist:"))
    {
      return SPOTIFY_ITEM_TYPE_PLAYLIST;
    }

  DPRINTF(E_WARN, L_SPOTIFY, "Could not parse item type from Spotify uri: %s\n", uri);
  return SPOTIFY_ITEM_TYPE_UNKNOWN;
}

static void
credentials_update_token(const char *access_token, const char *refresh_token, const char *scope, int32_t expires_in)
{
  CHECK_ERR(L_SPOTIFY, pthread_mutex_lock(&spotify_credentials_lock));

  free(spotify_credentials.access_token);
  free(spotify_credentials.refresh_token);
  free(spotify_credentials.granted_scope);

  spotify_credentials.access_token = safe_strdup(access_token);
  spotify_credentials.refresh_token = safe_strdup(refresh_token);
  spotify_credentials.granted_scope = safe_strdup(scope);
  if (expires_in > 0)
    spotify_credentials.token_expires_in = expires_in;
  else
    spotify_credentials.token_expires_in = 3600;
  spotify_credentials.token_time_requested = time(NULL);

  CHECK_ERR(L_SPOTIFY, pthread_mutex_unlock(&spotify_credentials_lock));
}

static void
credentials_update_user(const char *user, const char *country)
{
  CHECK_ERR(L_SPOTIFY, pthread_mutex_lock(&spotify_credentials_lock));

  free(spotify_credentials.user);
  free(spotify_credentials.user_country);

  spotify_credentials.user = safe_strdup(user);
  spotify_credentials.user_country = safe_strdup(country);

  CHECK_ERR(L_SPOTIFY, pthread_mutex_unlock(&spotify_credentials_lock));
}

static void
credentials_get_auth_header(char *header, size_t header_size)
{
  CHECK_ERR(L_SPOTIFY, pthread_mutex_lock(&spotify_credentials_lock));

  snprintf(header, header_size, "Authorization: Bearer %s", spotify_credentials.access_token);

  CHECK_ERR(L_SPOTIFY, pthread_mutex_unlock(&spotify_credentials_lock));
}

static char *
credentials_query_param_market(const char *href)
{
  char *next_href = NULL;

  CHECK_ERR(L_SPOTIFY, pthread_mutex_lock(&spotify_credentials_lock));

  if (!spotify_credentials.user_country)
    {
      next_href = safe_strdup(href);
    }
  else
    {
      if (strchr(href, '?'))
        next_href = safe_asprintf("%s&market=%s", href, spotify_credentials.user_country);
      else
        next_href = safe_asprintf("%s?market=%s", href, spotify_credentials.user_country);
    }

  CHECK_ERR(L_SPOTIFY, pthread_mutex_unlock(&spotify_credentials_lock));

  return next_href;
}

static bool
credentials_token_valid(void)
{
  bool valid = false;

  CHECK_ERR(L_SPOTIFY, pthread_mutex_lock(&spotify_credentials_lock));

  if (spotify_credentials.access_token
        && spotify_credentials.token_time_requested
        && difftime(time(NULL), spotify_credentials.token_time_requested) < spotify_credentials.token_expires_in)
    {
      valid = true; // Spotify token still valid
    }

  CHECK_ERR(L_SPOTIFY, pthread_mutex_unlock(&spotify_credentials_lock));

  return valid;
}

static bool
credentials_token_exists(void)
{
  bool exists = false;

  CHECK_ERR(L_SPOTIFY, pthread_mutex_lock(&spotify_credentials_lock));

  exists = (spotify_credentials.access_token != NULL);

  CHECK_ERR(L_SPOTIFY, pthread_mutex_unlock(&spotify_credentials_lock));

  return exists;
}

static void
credentials_user_token_get(char **user, char **token)
{
  CHECK_ERR(L_SPOTIFY, pthread_mutex_lock(&spotify_credentials_lock));

  if (user)
    *user = safe_strdup(spotify_credentials.user);
  if (token)
    *token = safe_strdup(spotify_credentials.access_token);

  CHECK_ERR(L_SPOTIFY, pthread_mutex_unlock(&spotify_credentials_lock));
}

static void
credentials_token_info(struct spotifywebapi_access_token *info)
{
  memset(info, 0, sizeof(struct spotifywebapi_access_token));

  CHECK_ERR(L_SPOTIFY, pthread_mutex_lock(&spotify_credentials_lock));
  
  if (spotify_credentials.token_time_requested > 0)
    info->expires_in = spotify_credentials.token_expires_in - difftime(time(NULL), spotify_credentials.token_time_requested);
  else
    info->expires_in = 0;

  info->token = safe_strdup(spotify_credentials.access_token);
  
  CHECK_ERR(L_SPOTIFY, pthread_mutex_unlock(&spotify_credentials_lock));
}

static void
credentials_status_info(struct spotifywebapi_status_info *info)
{
  memset(info, 0, sizeof(struct spotifywebapi_status_info));

  CHECK_ERR(L_SPOTIFY, pthread_mutex_lock(&spotify_credentials_lock));

  info->token_valid = (spotify_credentials.access_token != NULL);
  if (spotify_credentials.user)
    {
      strncpy(info->user, spotify_credentials.user, (sizeof(info->user) - 1));
    }
  if (spotify_credentials.user_country)
    {
      strncpy(info->country, spotify_credentials.user_country, (sizeof(info->country) - 1));
    }
  if (spotify_credentials.granted_scope)
    {
      strncpy(info->granted_scope, spotify_credentials.granted_scope, (sizeof(info->granted_scope) - 1));
    }
  if (spotify_scope)
    {
      strncpy(info->required_scope, spotify_scope, (sizeof(info->required_scope) - 1));
    }

  CHECK_ERR(L_SPOTIFY, pthread_mutex_unlock(&spotify_credentials_lock));
}

static void
credentials_clear()
{
  CHECK_ERR(L_SPOTIFY, pthread_mutex_lock(&spotify_credentials_lock));

  free(spotify_credentials.access_token);
  free(spotify_credentials.refresh_token);
  free(spotify_credentials.granted_scope);
  free(spotify_credentials.user_country);
  free(spotify_credentials.user);

  memset(&spotify_credentials, 0, sizeof(struct spotify_credentials));

  CHECK_ERR(L_SPOTIFY, pthread_mutex_unlock(&spotify_credentials_lock));
}

static void
free_http_client_ctx(struct http_client_ctx *ctx)
{
  if (!ctx)
    return;

  if (ctx->input_body)
    evbuffer_free(ctx->input_body);
  if (ctx->output_headers)
    {
      keyval_clear(ctx->output_headers);
      free(ctx->output_headers);
    }
  free(ctx);
}

static int
request_access_tokens(struct keyval *kv, const char **err)
{
  struct http_client_ctx ctx;
  char *param;
  char *body;
  json_object *haystack = NULL;
  const char *access_token;
  const char *refresh_token;
  const char *scope;
  uint32_t expires_in;
  int ret;

  param = http_form_urlencode(kv);
  if (!param)
    {
      *err = "http_form_uriencode() failed";
      ret = -1;
      goto out_clear_kv;
    }

  memset(&ctx, 0, sizeof(struct http_client_ctx));
  ctx.url = (char *)spotify_token_uri;
  ctx.output_body = param;
  ctx.input_body = evbuffer_new();

  ret = http_client_request(&ctx, session);
  if (ret < 0)
    {
      *err = "Did not get a reply from Spotify";
      goto out_free_input_body;
    }

  // 0-terminate for safety
  evbuffer_add(ctx.input_body, "", 1);

  body = (char *)evbuffer_pullup(ctx.input_body, -1);
  if (!body || (strlen(body) == 0))
    {
      *err = "The reply from Spotify is empty or invalid";
      ret = -1;
      goto out_free_input_body;
    }

  DPRINTF(E_DBG, L_SPOTIFY, "Token reply: %s\n", body);

  haystack = json_tokener_parse(body);
  if (!haystack)
    {
      *err = "JSON parser returned an error";
      ret = -1;
      goto out_free_input_body;
    }

  access_token = jparse_str_from_obj(haystack, "access_token");
  if (!access_token)
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Could not find access token in reply: %s\n", body);

      *err = "Could not find access token in Spotify reply (see log)";
      ret = -1;
      goto out_free_input_body;
    }

  refresh_token = jparse_str_from_obj(haystack, "refresh_token");
  if (refresh_token)
    db_admin_set(DB_ADMIN_SPOTIFY_REFRESH_TOKEN, refresh_token);

  scope = jparse_str_from_obj(haystack, "scope");
  expires_in = jparse_int_from_obj(haystack, "expires_in");

  credentials_update_token(access_token, refresh_token, scope, expires_in);

  ret = 0;

 out_free_input_body:
   jparse_free(haystack);
   evbuffer_free(ctx.input_body);
   free(param);
 out_clear_kv:

  return ret;
}

/*
 * Request the api endpoint at 'href' and returns the response body as
 * an allocated JSON object (must be freed by the caller) or NULL.
 *
 * @param href The spotify endpoint uri
 * @return Response as JSON object or NULL
 */
static json_object *
request_endpoint(const char *uri)
{
  struct http_client_ctx *ctx;
  char bearer_token[1024];
  char *response_body;
  json_object *json_response = NULL;
  int ret;

  CHECK_NULL(L_SPOTIFY, ctx = calloc(1, sizeof(struct http_client_ctx)));
  CHECK_NULL(L_SPOTIFY, ctx->output_headers = calloc(1, sizeof(struct keyval)));
  CHECK_NULL(L_SPOTIFY, ctx->input_body = evbuffer_new());

  ctx->url = uri;

  credentials_get_auth_header(bearer_token, sizeof(bearer_token));
  if (keyval_add(ctx->output_headers, "Authorization", bearer_token) < 0)
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Add bearer_token to keyval failed for request '%s'\n", uri);
      goto out;
    }

  DPRINTF(E_DBG, L_SPOTIFY, "Making request to '%s'\n", uri);

  ret = http_client_request(ctx, session);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Request for '%s' failed\n", uri);
      goto out;
    }

  // 0-terminate for safety
  evbuffer_add(ctx->input_body, "", 1);

  response_body = (char *) evbuffer_pullup(ctx->input_body, -1);
  if (!response_body || (strlen(response_body) == 0))
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Request for '%s' failed, response was empty\n", uri);
      goto out;
    }

//  DPRINTF(E_DBG, L_SPOTIFY, "Web api response for '%s'\n%s\n", uri, response_body);

  json_response = json_tokener_parse(response_body);
  if (!json_response)
    DPRINTF(E_LOG, L_SPOTIFY, "JSON parser returned an error for '%s'\n", uri);
  else
    DPRINTF(E_DBG, L_SPOTIFY, "Got JSON response for request to '%s'\n", uri);

 out:
  free_http_client_ctx(ctx);

  return json_response;
}

/*
 * Request user information
 *
 * API endpoint: https://api.spotify.com/v1/me
 */
static int
request_user_info()
{
  json_object *response;
  const char *user = NULL;
  const char *user_country = NULL;

  response = request_endpoint(spotify_me_uri);
  if (response)
    {
      user = jparse_str_from_obj(response, "id");
      user_country = jparse_str_from_obj(response, "country");

      DPRINTF(E_DBG, L_SPOTIFY, "User '%s', country '%s'\n", user, user_country);
      credentials_update_user(user, user_country);

      jparse_free(response);
    }

  return 0;
}

/*
 * Called from the oauth callback to get a new access and refresh token
 *
 * @return 0 on success, -1 on failure
 */
static int
token_get(const char *code, const char *redirect_uri, const char **err)
{
  struct keyval kv = { 0 };
  int ret;

  *err = "";
  ret = ( (keyval_add(&kv, "grant_type", "authorization_code") == 0) &&
          (keyval_add(&kv, "code", code) == 0) &&
          (keyval_add(&kv, "client_id", spotify_client_id) == 0) &&
          (keyval_add(&kv, "client_secret", spotify_client_secret) == 0) &&
          (keyval_add(&kv, "redirect_uri", redirect_uri) == 0) );

  if (!ret)
    {
      *err = "Add parameters to keyval failed";
      ret = -1;
    }
  else
    ret = request_access_tokens(&kv, err);

  keyval_clear(&kv);

  if (ret == 0)
    request_user_info();

  return ret;
}

/*
 * Get a new access token for the stored refresh token (user already granted
 * access to the web api)
 *
 * First checks if the current access token is still valid and only requests
 * a new token if not.
 *
 * @return 0 on success, -1 on failure
 */
static int
token_refresh(void)
{
  struct keyval kv = { 0 };
  char *refresh_token = NULL;
  const char *err;
  int ret;

  if (credentials_token_valid())
    {
      return 0; // Spotify token still valid
    }

  ret = db_admin_get(&refresh_token, DB_ADMIN_SPOTIFY_REFRESH_TOKEN);
  if (ret < 0)
    {
      return -1; // No refresh token (user not logged in)
    }

  DPRINTF(E_DBG, L_SPOTIFY, "Spotify refresh-token: '%s'\n", refresh_token);

  ret = ( (keyval_add(&kv, "grant_type", "refresh_token") == 0) &&
	  (keyval_add(&kv, "client_id", spotify_client_id) == 0) &&
	  (keyval_add(&kv, "client_secret", spotify_client_secret) == 0) &&
          (keyval_add(&kv, "refresh_token", refresh_token) == 0) );
  if (!ret)
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Add parameters to keyval failed");
      goto error;
    }

  ret = request_access_tokens(&kv, &err);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Error requesting access token: %s", err);
      goto error;
    }

  request_user_info();

  free(refresh_token);
  keyval_clear(&kv);

  return 0;

 error:
  free(refresh_token);
  keyval_clear(&kv);
  return -1;
}

/*
 * Request the api endpoint at 'href' and retuns the response body as
 * an allocated JSON object (must be freed by the caller) or NULL.
 *
 * Before making the request, the validity of the current access token
 * is checked and if necessary a token refresh request is issued before
 * requesting the given endpoint.
 *
 * @param credentials Updated credentials
 * @param href The spotify endpoint uri
 * @return Response as JSON object or NULL
 */
static json_object *
request_endpoint_with_token_refresh(const char *href)
{
  if (0 > token_refresh())
    {
      return NULL;
    }

  return request_endpoint(href);
}

typedef int (*paging_request_cb)(void *arg);
typedef int (*paging_item_cb)(json_object *item, int index, int total, enum spotify_request_type request_type, void *arg);

/*
 * Request the spotify endpoint at 'href'
 *
 * The endpoint must return a "paging object" e. g.:
 *
 *   {
 *     "items": [ item1, item2, ... ],
 *     "limit": 50,
 *     "next": "{uri for the next set of items}",
 *     "offset": 0,
 *     "total": {total number of items},
 *   }
 *
 * The given callback is invoked for every item in the "items" array.
 * If "next" is set in the response, after processing all items, the next uri
 * is requested and the callback is invoked for every item of this request.
 * The function returns after all items are processed and there is no "next"
 * request.
 *
 * @param endpoint_uri The endpont uri
 * @param item_cb The callback function invoked for every item
 * @param pre_request_cb Callback function invoked before each request (optional)
 * @param post_request_cb Callback function invoked after each request (optional)
 * @param with_market If TRUE appends the user country as market to the request (applies track relinking)
 * @param Spotify credentials
 * @param arg User data passed to each callback
 * @return 0 on success, -1 on failure
 */
static int
request_pagingobject_endpoint(const char *href, paging_item_cb item_cb, paging_request_cb pre_request_cb, paging_request_cb post_request_cb,
                              bool with_market, enum spotify_request_type request_type, void *arg)
{
  char *next_href;
  json_object *response;
  json_object *items;
  json_object *item;
  int count;
  int i;
  int offset;
  int total;
  int ret;

  if (!with_market)
    next_href = safe_strdup(href);
  else
    next_href = credentials_query_param_market(href);

  while (next_href)
    {
      if (pre_request_cb)
	pre_request_cb(arg);

      response = request_endpoint_with_token_refresh(next_href);

      if (!response)
	{
	  DPRINTF(E_LOG, L_SPOTIFY, "Unexpected JSON: no response for paging endpoint (API endpoint: '%s')\n", next_href);

	  if (post_request_cb)
	    post_request_cb(arg);

	  free(next_href);
	  return -1;
	}

      free(next_href);
      next_href = safe_strdup(jparse_str_from_obj(response, "next"));

      offset = jparse_int_from_obj(response, "offset");
      total = jparse_int_from_obj(response, "total");

      if (jparse_array_from_obj(response, "items", &items) == 0)
        {
	  count = json_object_array_length(items);
	  for (i = 0; i < count; i++)
	    {
	      item = json_object_array_get_idx(items, i);
	      if (!item)
	        {
		  DPRINTF(E_LOG, L_SPOTIFY, "Unexpected JSON: no item at index %d in '%s' (API endpoint: '%s')\n",
			  i, json_object_to_json_string(items), href);
		  continue;
		}

	      ret = item_cb(item, (i + offset), total, request_type, arg);
	      if (ret < 0)
		{
		  DPRINTF(E_LOG, L_SPOTIFY, "Couldn't add item at index %d '%s' (API endpoint: '%s')\n",
			  i, json_object_to_json_string(item), href);
		}
	    }
	}

      if (post_request_cb)
	post_request_cb(arg);

      jparse_free(response);
    }

  return 0;
}

static const char *
get_album_image(json_object *jsonalbum, int max_w)
{
  json_object *jsonimages;
  json_object *jsonimage;
  int image_count;
  int index;
  int width;
  int candidate_width;
  const char *artwork_url = NULL;
  bool use_image;

  if (!json_object_object_get_ex(jsonalbum, "images", &jsonimages))
    {
      DPRINTF(E_DBG, L_SPOTIFY, "No images in for spotify album object found\n");
      return NULL;
    }

  // Find first image that has a smaller width than the given max_w (this should
  // avoid the need for resizing and improve performance at the cost of some
  // quality loss). If no sufficiently small image available, return smallest
  // best alternative. Special case is if no max width (max_w = 0) is given, the
  // widest images will be used.
  //
  // Note that Spotify should return the images ordered descending by width
  // (widest image first), but at one point had a bug that meant they didn't, so
  // we don't rely on that here.
  image_count = json_object_array_length(jsonimages);
  for (index = 0, candidate_width = 0; index < image_count; index++)
    {
      jsonimage = json_object_array_get_idx(jsonimages, index);
      if (!jsonimage)
	continue;

      width = jparse_int_from_obj(jsonimage, "width");

      if (max_w == 0 || candidate_width == 0)
	use_image = (width > candidate_width);
      else if (candidate_width > max_w)
	use_image = (width < candidate_width);
      else
	use_image = (candidate_width < width && width <= max_w);

      if (!use_image)
	continue;

      candidate_width = width;
      artwork_url = jparse_str_from_obj(jsonimage, "url");
    }

  return artwork_url;
}

static void
parse_metadata_track(json_object *jsontrack, struct spotify_track *track, int max_w)
{
  json_object *jsonalbum;
  json_object *jsonartists;
  json_object *needle;

  memset(track, 0, sizeof(struct spotify_track));

  if (json_object_object_get_ex(jsontrack, "album", &jsonalbum))
    {
      track->album = jparse_str_from_obj(jsonalbum, "name");
      if (json_object_object_get_ex(jsonalbum, "artists", &jsonartists))
	track->album_artist = jparse_str_from_array(jsonartists, 0, "name");

      track->artwork_url = get_album_image(jsonalbum, max_w);
    }

  if (json_object_object_get_ex(jsontrack, "artists", &jsonartists))
    track->artist = jparse_str_from_array(jsonartists, 0, "name");

  track->disc_number = jparse_int_from_obj(jsontrack, "disc_number");
  track->album_type = jparse_str_from_obj(jsonalbum, "album_type");
  track->is_compilation = (track->album_type && 0 == strcmp(track->album_type, "compilation"));
  track->duration_ms = jparse_int_from_obj(jsontrack, "duration_ms");
  track->name = jparse_str_from_obj(jsontrack, "name");
  track->track_number = jparse_int_from_obj(jsontrack, "track_number");
  track->uri = jparse_str_from_obj(jsontrack, "uri");
  track->id = jparse_str_from_obj(jsontrack, "id");
  track->type = jparse_str_from_obj(jsontrack, "type");

  // "is_playable" is only returned for a request with a market parameter, default to true if it is not in the response
  track->is_playable = true;
  if (json_object_object_get_ex(jsontrack, "is_playable", NULL))
    {
      track->is_playable = jparse_bool_from_obj(jsontrack, "is_playable");

      if (json_object_object_get_ex(jsontrack, "restrictions", &needle))
	track->restrictions = json_object_to_json_string(needle);

      if (json_object_object_get_ex(jsontrack, "linked_from", &needle))
	track->linked_from_uri = jparse_str_from_obj(needle, "uri");
    }
}

static int
get_year_from_date(const char *date)
{
  char tmp[5];
  uint32_t year = 0;

  if (date && strlen(date) >= 4)
    {
      strncpy(tmp, date, sizeof(tmp));
      tmp[4] = '\0';
      safe_atou32(tmp, &year);
    }

  return year;
}

static void
parse_metadata_album(json_object *jsonalbum, struct spotify_album *album, int max_w)
{
  json_object* jsonartists;

  memset(album, 0, sizeof(struct spotify_album));

  if (json_object_object_get_ex(jsonalbum, "artists", &jsonartists))
    album->artist = jparse_str_from_array(jsonartists, 0, "name");

  album->name = jparse_str_from_obj(jsonalbum, "name");
  album->uri = jparse_str_from_obj(jsonalbum, "uri");
  album->id = jparse_str_from_obj(jsonalbum, "id");
  album->type = jparse_str_from_obj(jsonalbum, "type");

  album->album_type = jparse_str_from_obj(jsonalbum, "album_type");
  album->is_compilation = (album->album_type && 0 == strcmp(album->album_type, "compilation"));

  album->label = jparse_str_from_obj(jsonalbum, "label");

  album->release_date = jparse_str_from_obj(jsonalbum, "release_date");
  album->release_date_precision = jparse_str_from_obj(jsonalbum, "release_date_precision");
  if (album->release_date_precision && strcmp(album->release_date_precision, "day") == 0)
    album->release_date_time = jparse_time_from_obj(jsonalbum, "release_date");
  album->release_year = get_year_from_date(album->release_date);

  if (max_w > 0)
    album->artwork_url = get_album_image(jsonalbum, max_w);

  // TODO Genre is an array of strings ('genres'), but it is always empty (https://github.com/spotify/web-api/issues/157)
  //album->genre = jparse_str_from_obj(jsonalbum, "genre");
}

static void
parse_metadata_playlist(json_object *jsonplaylist, struct spotify_playlist *playlist)
{
  json_object *needle;

  memset(playlist, 0, sizeof(struct spotify_playlist));

  playlist->name = jparse_str_from_obj(jsonplaylist, "name");
  playlist->uri = jparse_str_from_obj(jsonplaylist, "uri");
  playlist->id = jparse_str_from_obj(jsonplaylist, "id");
  playlist->href = jparse_str_from_obj(jsonplaylist, "href");

  if (json_object_object_get_ex(jsonplaylist, "owner", &needle))
    playlist->owner = jparse_str_from_obj(needle, "id");

  if (json_object_object_get_ex(jsonplaylist, "tracks", &needle))
    {
      playlist->tracks_href = jparse_str_from_obj(needle, "href");
      playlist->tracks_count = jparse_int_from_obj(needle, "total");
    }
}

static void
parse_metadata_show(json_object *jsonshow, struct spotify_album *show)
{
  memset(show, 0, sizeof(struct spotify_album));

  show->name = jparse_str_from_obj(jsonshow, "name");
  show->artist = jparse_str_from_obj(jsonshow, "publisher");
  show->uri = jparse_str_from_obj(jsonshow, "uri");
  show->id = jparse_str_from_obj(jsonshow, "id");
  show->type = jparse_str_from_obj(jsonshow, "type");
}

static void
parse_metadata_episode(json_object *jsonepisode, struct spotify_track *episode, int max_w)
{
  json_object *jsonshow;

  memset(episode, 0, sizeof(struct spotify_track));

  if (json_object_object_get_ex(jsonepisode, "show", &jsonshow))
    {
      episode->album = jparse_str_from_obj(jsonshow, "name");
      episode->artwork_url = get_album_image(jsonshow, max_w);
    }

  episode->name = jparse_str_from_obj(jsonepisode, "name");
  episode->uri = jparse_str_from_obj(jsonepisode, "uri");
  episode->id = jparse_str_from_obj(jsonepisode, "id");
  episode->type = jparse_str_from_obj(jsonepisode, "type");
  episode->duration_ms = jparse_int_from_obj(jsonepisode, "duration_ms");

  episode->release_date = jparse_str_from_obj(jsonepisode, "release_date");
  episode->release_date_precision = jparse_str_from_obj(jsonepisode, "release_date_precision");
  if (episode->release_date_precision && strcmp(episode->release_date_precision, "day") == 0)
    episode->release_date_time = jparse_time_from_obj(jsonepisode, "release_date");
  episode->release_year = get_year_from_date(episode->release_date);
  episode->mtime = episode->release_date_time;

  // "is_playable" is only returned for a request with a market parameter, default to true if it is not in the response
  episode->is_playable = true;
  if (json_object_object_get_ex(jsonepisode, "is_playable", NULL))
    {
      episode->is_playable = jparse_bool_from_obj(jsonepisode, "is_playable");
    }
}

/*
 * Creates a new string for the playlist API endpoint for the given playist-uri.
 * The returned string needs to be freed by the caller.
 *
 * @param uri Playlist uri (e. g. "spotify:user:username:playlist:59ZbFPES4DQwEjBpWHzrtC")
 * @return Playlist endpoint uri (e. g. "https://api.spotify.com/v1/users/username/playlists/59ZbFPES4DQwEjBpWHzrtC")
 */
static int
get_id_from_uri(const char *uri, char **id)
{
  char *tmp;
  tmp = strrchr(uri, ':');
  if (!tmp)
    {
      return -1;
    }
  tmp++;

  *id = strdup(tmp);

  return 0;
}

static char *
get_playlist_tracks_endpoint_uri(const char *uri)
{
  char *endpoint_uri = NULL;
  char *id = NULL;
  int ret;

  ret = get_id_from_uri(uri, &id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Error extracting owner and id from playlist uri '%s'\n", uri);
      goto out;
    }

  endpoint_uri = safe_asprintf(spotify_playlist_tracks_uri, id);

 out:
  free(id);
  return endpoint_uri;
}

static char *
get_album_endpoint_uri(const char *uri)
{
  char *endpoint_uri = NULL;
  char *id = NULL;
  int ret;

  ret = get_id_from_uri(uri, &id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Error extracting id from uri '%s'\n", uri);
      goto out;
    }

  endpoint_uri = safe_asprintf(spotify_album_uri, id);

 out:
  free(id);
  return endpoint_uri;
}

static char *
get_album_tracks_endpoint_uri(const char *uri)
{
  char *endpoint_uri = NULL;
  char *id = NULL;
  int ret;

  ret = get_id_from_uri(uri, &id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Error extracting id from uri '%s'\n", uri);
      goto out;
    }

  endpoint_uri = safe_asprintf(spotify_album_tracks_uri, id);

 out:
  free(id);
  return endpoint_uri;
}

static char *
get_track_endpoint_uri(const char *uri)
{
  char *endpoint_uri = NULL;
  char *id = NULL;
  int ret;

  ret = get_id_from_uri(uri, &id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Error extracting id from track uri '%s'\n", uri);
      goto out;
    }

  endpoint_uri = safe_asprintf(spotify_track_uri, id);

 out:
  free(id);
  return endpoint_uri;
}

static char *
get_artist_albums_endpoint_uri(const char *uri)
{
  char *endpoint_uri = NULL;
  char *id = NULL;
  int ret;

  ret = get_id_from_uri(uri, &id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Error extracting id from uri '%s'\n", uri);
      goto out;
    }

  endpoint_uri = safe_asprintf(spotify_artist_albums_uri, id);

 out:
  free(id);
  return endpoint_uri;
}

static char *
get_episode_endpoint_uri(const char *uri)
{
  char *endpoint_uri = NULL;
  char *id = NULL;
  int ret;

  ret = get_id_from_uri(uri, &id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Error extracting id from track uri '%s'\n", uri);
      goto out;
    }

  endpoint_uri = safe_asprintf(spotify_episode_uri, id);

 out:
  free(id);
  return endpoint_uri;
}

static json_object *
request_track(const char *path)
{
  char *endpoint_uri;
  json_object *response;

  endpoint_uri = get_track_endpoint_uri(path);
  response = request_endpoint_with_token_refresh(endpoint_uri);
  free(endpoint_uri);

  return response;
}

static json_object *
request_episode(const char *path)
{
  char *endpoint_uri;
  json_object *response;

  endpoint_uri = get_episode_endpoint_uri(path);
  response = request_endpoint_with_token_refresh(endpoint_uri);
  free(endpoint_uri);

  return response;
}

static int
transaction_start(void *arg)
{
  db_transaction_begin();
  return 0;
}

static int
transaction_end(void *arg)
{
  db_transaction_end();
  return 0;
}

static void
map_track_to_queueitem(struct db_queue_item *item, const struct spotify_track *track, const struct spotify_album *album)
{
  char virtual_path[PATH_MAX];

  memset(item, 0, sizeof(struct db_queue_item));

  item->file_id = DB_MEDIA_FILE_NON_PERSISTENT_ID;
  item->title = safe_strdup(track->name);
  item->artist = safe_strdup(track->artist);

  if (album)
    {
      item->album_artist = safe_strdup(album->artist);
      item->album = safe_strdup(album->name);
      item->artwork_url = safe_strdup(album->artwork_url);
    }
  else
    {
      item->album_artist = safe_strdup(track->album_artist);
      item->album = safe_strdup(track->album);
      item->artwork_url = safe_strdup(track->artwork_url);
    }

  item->disc = track->disc_number;
  item->song_length = track->duration_ms;
  item->track = track->track_number;

  item->data_kind = DATA_KIND_SPOTIFY;
  item->media_kind = MEDIA_KIND_MUSIC;

  item->path = safe_strdup(track->uri);

  snprintf(virtual_path, PATH_MAX, "/%s", track->uri);
  item->virtual_path = strdup(virtual_path);
}

static int
queue_add_track(int *count, int *new_item_id, const char *uri, int position, char reshuffle, uint32_t item_id)
{
  json_object *response = NULL;
  struct spotify_track track;
  struct db_queue_item item = { 0 };
  struct db_queue_add_info queue_add_info;
  int ret;

  response = request_track(uri);
  if (!response)
    goto error;

  parse_metadata_track(response, &track, ART_DEFAULT_WIDTH);

  DPRINTF(E_DBG, L_SPOTIFY, "Got track: '%s' (%s) \n", track.name, track.uri);

  map_track_to_queueitem(&item, &track, NULL);

  ret = db_queue_add_start(&queue_add_info, position);
  if (ret < 0)
    goto error;

  ret = db_queue_add_next(&queue_add_info, &item);
  ret = db_queue_add_end(&queue_add_info, reshuffle, item_id, ret);
  if (ret < 0)
    goto error;

  if (count)
    *count = queue_add_info.count;
  if (new_item_id)
    *new_item_id = queue_add_info.new_item_id;

  free_queue_item(&item, 1);
  jparse_free(response);
  return 0;

 error:
  free_queue_item(&item, 1);
  jparse_free(response);
  return -1;
}

struct queue_add_album_param {
  struct spotify_album album;
  struct db_queue_add_info queue_add_info;
};

static int
queue_add_album_tracks(json_object *item, int index, int total, enum spotify_request_type request_type, void *arg)
{
  struct queue_add_album_param *param;
  struct spotify_track track;
  struct db_queue_item queue_item;
  int ret;

  param = arg;

  parse_metadata_track(item, &track, ART_DEFAULT_WIDTH);

  if (!track.uri || !track.is_playable)
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Track not available for playback: '%s' - '%s' (%s) (restrictions: %s)\n", track.artist, track.name, track.uri, track.restrictions);
      return -1;
    }

  map_track_to_queueitem(&queue_item, &track, &param->album);

  ret = db_queue_add_next(&param->queue_add_info, &queue_item);

  free_queue_item(&queue_item, 1);

  return ret;
}

static int
queue_add_album(int *count, int *new_item_id, const char *uri, int position, char reshuffle, uint32_t item_id)
{
  char *album_endpoint_uri = NULL;
  char *endpoint_uri = NULL;
  json_object *json_album;
  struct queue_add_album_param param;
  int ret;

  album_endpoint_uri = get_album_endpoint_uri(uri);
  json_album = request_endpoint_with_token_refresh(album_endpoint_uri);
  parse_metadata_album(json_album, &param.album, ART_DEFAULT_WIDTH);

  ret = db_queue_add_start(&param.queue_add_info, position);
  if (ret < 0)
    goto out;

  endpoint_uri = get_album_tracks_endpoint_uri(uri);

  ret = request_pagingobject_endpoint(endpoint_uri, queue_add_album_tracks, NULL, NULL, true, SPOTIFY_REQUEST_TYPE_DEFAULT, &param);

  ret = db_queue_add_end(&param.queue_add_info, reshuffle, item_id, ret);
  if (ret < 0)
    goto out;

  if (count)
    *count = param.queue_add_info.count;

 out:
  free(album_endpoint_uri);
  free(endpoint_uri);
  jparse_free(json_album);

  return ret;
}

static int
queue_add_albums(json_object *item, int index, int total, enum spotify_request_type request_type, void *arg)
{
  struct db_queue_add_info *param;
  struct queue_add_album_param param_add_album;
  char *endpoint_uri = NULL;
  int ret;

  param = arg;
  param_add_album.queue_add_info = *param;

  parse_metadata_album(item, &param_add_album.album, ART_DEFAULT_WIDTH);

  endpoint_uri = get_album_tracks_endpoint_uri(param_add_album.album.uri);
  ret = request_pagingobject_endpoint(endpoint_uri, queue_add_album_tracks, NULL, NULL, true, SPOTIFY_REQUEST_TYPE_DEFAULT, &param_add_album);

  *param = param_add_album.queue_add_info;

  free(endpoint_uri);
  return ret;

}

static int
queue_add_artist(int *count, int *new_item_id, const char *uri, int position, char reshuffle, uint32_t item_id)
{
  struct db_queue_add_info queue_add_info;
  char *endpoint_uri = NULL;
  int ret;

  ret = db_queue_add_start(&queue_add_info, position);
  if (ret < 0)
    goto out;

  endpoint_uri = get_artist_albums_endpoint_uri(uri);
  ret = request_pagingobject_endpoint(endpoint_uri, queue_add_albums, NULL, NULL, true, SPOTIFY_REQUEST_TYPE_DEFAULT, &queue_add_info);

  ret = db_queue_add_end(&queue_add_info, reshuffle, item_id, ret);
  if (ret < 0)
    goto out;

  if (count)
    *count = queue_add_info.count;

 out:
  free(endpoint_uri);
  return ret;
}

static int
queue_add_playlist_tracks(json_object *item, int index, int total, enum spotify_request_type request_type, void *arg)
{
  struct db_queue_add_info *queue_add_info;
  struct spotify_track track;
  json_object *jsontrack;
  struct db_queue_item queue_item;
  int ret;

  queue_add_info = arg;

  if (!(item && json_object_object_get_ex(item, "track", &jsontrack)))
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Unexpected JSON: missing 'track' in JSON object at index %d\n", index);
      return -1;
    }

  parse_metadata_track(jsontrack, &track, ART_DEFAULT_WIDTH);
  track.added_at = jparse_str_from_obj(item, "added_at");
  track.mtime = jparse_time_from_obj(item, "added_at");

  if (!track.uri || !track.is_playable)
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Track not available for playback: '%s' - '%s' (%s) (restrictions: %s)\n", track.artist, track.name, track.uri, track.restrictions);
      return -1;
    }

  map_track_to_queueitem(&queue_item, &track, NULL);

  ret = db_queue_add_next(queue_add_info, &queue_item);

  free_queue_item(&queue_item, 1);

  return ret;
}

static int
queue_add_playlist(int *count, int *new_item_id, const char *uri, int position, char reshuffle, uint32_t item_id)
{
  char *endpoint_uri = NULL;
  struct db_queue_add_info queue_add_info;
  int ret;

  ret = db_queue_add_start(&queue_add_info, position);
  if (ret < 0)
    goto out;

  endpoint_uri = get_playlist_tracks_endpoint_uri(uri);

  ret = request_pagingobject_endpoint(endpoint_uri, queue_add_playlist_tracks, NULL, NULL, true, SPOTIFY_REQUEST_TYPE_DEFAULT, &queue_add_info);

  ret = db_queue_add_end(&queue_add_info, reshuffle, item_id, ret);
  if (ret < 0)
    goto out;

  if (count)
    *count = queue_add_info.count;

 out:
  free(endpoint_uri);
  return ret;
}


/*
 * Returns the directory id for /spotify:/<artist>/<album>, if the directory (or the parent
 * directories) does not yet exist, they will be created.
 * If an error occured the return value is -1.
 *
 * @return directory id for the given artist/album directory
 */
static int
prepare_directories(const char *artist, const char *album)
{
  int dir_id;
  char virtual_path[PATH_MAX];
  int ret;

  ret = snprintf(virtual_path, sizeof(virtual_path), "/spotify:/%s", artist);
  if ((ret < 0) || (ret >= sizeof(virtual_path)))
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Virtual path exceeds PATH_MAX (/spotify:/%s)\n", artist);
      return -1;
    }
  dir_id = library_directory_save(virtual_path, NULL, 0, DIR_SPOTIFY, SCAN_KIND_SPOTIFY);
  if (dir_id <= 0)
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Could not add or update directory '%s'\n", virtual_path);
      return -1;
    }
  ret = snprintf(virtual_path, sizeof(virtual_path), "/spotify:/%s/%s", artist, album);
  if ((ret < 0) || (ret >= sizeof(virtual_path)))
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Virtual path exceeds PATH_MAX (/spotify:/%s/%s)\n", artist, album);
      return -1;
    }
  dir_id = library_directory_save(virtual_path, NULL, 0, dir_id, SCAN_KIND_SPOTIFY);
  if (dir_id <= 0)
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Could not add or update directory '%s'\n", virtual_path);
      return -1;
    }

  return dir_id;
}

static void
map_track_to_mfi(struct media_file_info *mfi, const struct spotify_track *track, const struct spotify_album *album, const char *pl_name)
{
  char virtual_path[PATH_MAX];

  mfi->title = safe_strdup(track->name);
  mfi->artist = safe_strdup(track->artist);
  mfi->disc = track->disc_number;
  mfi->song_length = track->duration_ms;
  mfi->track = track->track_number;

  mfi->data_kind   = DATA_KIND_SPOTIFY;
  if (strcmp(track->type, "episode") == 0)
    mfi->media_kind  = MEDIA_KIND_PODCAST;
  else
    mfi->media_kind  = MEDIA_KIND_MUSIC;
  mfi->type        = strdup("spotify");
  mfi->codectype   = strdup("wav");
  mfi->description = strdup("Spotify audio");

  mfi->path = strdup(track->uri);
  mfi->fname = strdup(track->uri);

  mfi->time_modified = track->mtime;
  mfi->time_added = track->mtime;

  if (album && album->uri)
    {
      mfi->album_artist = safe_strdup(album->artist);
      mfi->album = safe_strdup(album->name);
      mfi->genre = safe_strdup(album->genre);
      mfi->compilation = album->is_compilation;
      mfi->date_released = album->release_date_time;
      mfi->year = album->release_year;
    }
  else
    {
      mfi->album_artist = safe_strdup(track->album_artist);
      mfi->album = safe_strdup(track->album);
      mfi->compilation = track->is_compilation;
    }

  if (cfg_getbool(cfg_getsec(cfg, "spotify"), "album_override") && pl_name)
    {
      free(mfi->album);
      mfi->album = safe_strdup(pl_name);
    }
  if (cfg_getbool(cfg_getsec(cfg, "spotify"), "artist_override") && pl_name)
    {
      mfi->compilation = true;
    }

  if (mfi->media_kind == MEDIA_KIND_PODCAST)
    {
      // For podcasts we want the tracks/episodes release date
      mfi->date_released = track->release_date_time;
      mfi->year = track->release_year;
    }
  snprintf(virtual_path, PATH_MAX, "/spotify:/%s/%s/%s", mfi->album_artist, mfi->album, mfi->title);
  mfi->virtual_path = strdup(virtual_path);
  mfi->scan_kind = SCAN_KIND_SPOTIFY;
}

static int
track_add(struct spotify_track *track, struct spotify_album *album, const char *pl_name, int dir_id, enum spotify_request_type request_type)
{
  struct media_file_info mfi;
  int ret;

  if (!track->uri || !track->is_playable)
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Track not available for playback: '%s' - '%s' (%s) (restrictions: %s)\n",
	      track->artist, track->name, track->uri, track->restrictions);
      return -1;
    }

  if (track->linked_from_uri)
    DPRINTF(E_DBG, L_SPOTIFY, "Track '%s' (%s) linked from %s\n", track->name, track->uri, track->linked_from_uri);

  ret = db_file_ping_bypath(track->uri, track->mtime);
  if (ret == 0 || request_type == SPOTIFY_REQUEST_TYPE_METARESCAN)
    {
      DPRINTF(E_DBG, L_SPOTIFY, "Track '%s' (%s) is new or modified (mtime is %" PRIi64 ")\n",
	      track->name, track->uri, (int64_t)track->mtime);

      memset(&mfi, 0, sizeof(struct media_file_info));

      mfi.id = db_file_id_bypath(track->uri);
      mfi.directory_id = dir_id;

      map_track_to_mfi(&mfi, track, album, pl_name);

      library_media_save(&mfi);

      free_mfi(&mfi, 1);
    }

  if (album && album->uri)
    cache_artwork_ping(track->uri, album->mtime, 0);
  else
    cache_artwork_ping(track->uri, 1, 0);

  return 0;
}

static int
playlist_add_or_update(struct playlist_info *pli)
{
  int pl_id;

  pl_id = db_pl_id_bypath(pli->path);
  if (pl_id < 0)
    return library_playlist_save(pli);

  pli->id = pl_id;

  db_pl_clear_items(pli->id);

  return library_playlist_save(pli);
}

/*
 * Add a saved album to the library
 */
static int
saved_album_add(json_object *item, int index, int total, enum spotify_request_type request_type, void *arg)
{
  json_object *jsonalbum;
  struct spotify_album album;
  struct spotify_track track;
  json_object *needle;
  json_object *jsontracks;
  json_object *jsontrack;
  int track_count;
  int dir_id;
  int i;

  if (!json_object_object_get_ex(item, "album", &jsonalbum))
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Unexpected JSON: Item %d is missing the 'album' field\n", index);
      return -1;
    }
  if (!json_object_object_get_ex(jsonalbum, "tracks", &needle))
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Unexpected JSON: Item %d is missing the 'tracks' field'\n", index);
      return -1;
    }
  if (jparse_array_from_obj(needle, "items", &jsontracks) < 0)
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Unexpected JSON: Item %d has an empty 'tracks' array\n", index);
      return -1;
    }

  // Map album information
  parse_metadata_album(jsonalbum, &album, 0);
  album.added_at = jparse_str_from_obj(item, "added_at");
  album.mtime = jparse_time_from_obj(item, "added_at");

  // Now map the album tracks and insert/update them in the files database
  db_transaction_begin();

  // Get or create the directory structure for this album
  dir_id = prepare_directories(album.artist, album.name);

  track_count = json_object_array_length(jsontracks);
  for (i = 0; i < track_count; i++)
    {
      jsontrack = json_object_array_get_idx(jsontracks, i);
      if (!jsontrack)
	break;

      parse_metadata_track(jsontrack, &track, 0);
      track.mtime = album.mtime;

      track_add(&track, &album, NULL, dir_id, request_type);
    }

  db_transaction_end();

  if ((index + 1) >= total || ((index + 1) % 10 == 0))
    DPRINTF(E_LOG, L_SPOTIFY, "Scanned %d of %d saved albums\n", (index + 1), total);

  return 0;
}

/*
 * Thread: library
 *
 * Scan users saved albums into the library
 */
static int
scan_saved_albums(enum spotify_request_type request_type)
{
  int ret;

  ret = request_pagingobject_endpoint(spotify_albums_uri, saved_album_add, NULL, NULL, true, request_type, NULL);

  return ret;
}

/*
 * Add a saved podcast show to the library
 */
static int
saved_episodes_add(json_object *item, int index, int total, enum spotify_request_type request_type, void *arg)
{
  struct spotify_album *show = arg;
  struct spotify_track episode;
  int dir_id;

  DPRINTF(E_DBG, L_SPOTIFY, "saved_episodes_add: %s\n", json_object_to_json_string(item));

  // Map episode information
  parse_metadata_episode(item, &episode, 0);

  // Get or create the directory structure for this album
  dir_id = prepare_directories(show->artist, show->name);

  track_add(&episode, show, NULL, dir_id, request_type);

  return 0;
}

/*
 * Add a saved podcast show to the library
 */
static int
saved_show_add(json_object *item, int index, int total, enum spotify_request_type request_type, void *arg)
{
  json_object *jsonshow;
  struct spotify_album show;
  char *endpoint_uri;

  DPRINTF(E_DBG, L_SPOTIFY, "saved_show_add: %s\n", json_object_to_json_string(item));

  if (!json_object_object_get_ex(item, "show", &jsonshow))
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Unexpected JSON: Item %d is missing the 'show' field\n", index);
      return -1;
    }

  // Map show information
  parse_metadata_show(jsonshow, &show);
  show.added_at = jparse_str_from_obj(item, "added_at");
  show.mtime = jparse_time_from_obj(item, "added_at");


  // Now map the show episodes and insert/update them in the files database
  endpoint_uri = safe_asprintf(spotify_shows_episodes_uri, show.id);
  request_pagingobject_endpoint(endpoint_uri, saved_episodes_add, transaction_start, transaction_end, true, request_type, &show);
  free(endpoint_uri);

  if ((index + 1) >= total || ((index + 1) % 10 == 0))
    DPRINTF(E_LOG, L_SPOTIFY, "Scanned %d of %d saved albums\n", (index + 1), total);

  return 0;
}

/*
 * Thread: library
 *
 * Scan users saved podcast shows into the library
 */
static int
scan_saved_shows(enum spotify_request_type request_type)
{
  int ret;

  ret = request_pagingobject_endpoint(spotify_shows_uri, saved_show_add, NULL, NULL, true, request_type, NULL);

  return ret;
}

/*
 * Add a saved playlist's tracks to the library
 */
static int
saved_playlist_tracks_add(json_object *item, int index, int total, enum spotify_request_type request_type, void *arg)
{
  struct spotify_track track;
  struct spotify_album album;
  json_object *jsontrack;
  json_object *jsonalbum;
  struct playlist_info *pli;
  int dir_id;
  int ret;

  pli = arg;

  if (!(item && json_object_object_get_ex(item, "track", &jsontrack)))
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Unexpected JSON: missing 'track' in JSON object at index %d\n", index);
      return -1;
    }

  parse_metadata_track(jsontrack, &track, 0);
  track.added_at = jparse_str_from_obj(item, "added_at");
  track.mtime = jparse_time_from_obj(item, "added_at");

  if (!track.uri || !track.is_playable)
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Track not available for playback: '%s' - '%s' (%s) (restrictions: %s)\n", track.artist, track.name, track.uri, track.restrictions);
      return 0;
    }

  if (!cfg_getbool(cfg_getsec(cfg, "spotify"), "album_override")
      && json_object_object_get_ex(jsontrack, "album", &jsonalbum))
    {
      parse_metadata_album(jsonalbum, &album, 0);
    }
  else
    {
      memset(&album, 0, sizeof(struct spotify_album));
    }

  dir_id = prepare_directories(track.album_artist, track.album);
  ret = track_add(&track, &album, pli->title, dir_id, request_type);
  if (ret == 0)
    db_pl_add_item_bypath(pli->id, track.uri);

  return 0;
}

/* Thread: library */
static int
scan_playlist_tracks(const char *playlist_tracks_endpoint_uri, struct playlist_info *pli, enum spotify_request_type request_type)
{
  int ret;

  ret = request_pagingobject_endpoint(playlist_tracks_endpoint_uri, saved_playlist_tracks_add, transaction_start, transaction_end, true, request_type, pli);

  return ret;
}

static void
map_playlist_to_pli(struct playlist_info *pli, struct spotify_playlist *playlist)
{
  memset(pli, 0, sizeof(struct playlist_info));

  pli->type  = PL_PLAIN;
  pli->path  = strdup(playlist->uri);
  pli->title = safe_strdup(playlist->name);

  pli->parent_id      = spotify_base_plid;
  pli->directory_id   = DIR_SPOTIFY;
  pli->scan_kind = SCAN_KIND_SPOTIFY;

  if (playlist->owner)
    pli->virtual_path = safe_asprintf("/spotify:/%s (%s)", playlist->name, playlist->owner);
  else
    pli->virtual_path = safe_asprintf("/spotify:/%s", playlist->name);
}

/*
 * Add a saved playlist to the library
 */
static int
saved_playlist_add(json_object *item, int index, int total, enum spotify_request_type request_type, void *arg)
{
  struct spotify_playlist playlist;
  struct playlist_info pli;
  int pl_id;

  // Map playlist information
  parse_metadata_playlist(item, &playlist);

  DPRINTF(E_DBG, L_SPOTIFY, "Got playlist: '%s' with %d tracks (%s) \n", playlist.name, playlist.tracks_count, playlist.uri);

  if (!playlist.uri || !playlist.name || playlist.tracks_count == 0)
    {
      DPRINTF(E_INFO, L_SPOTIFY, "Ignoring playlist '%s' with %d tracks (%s)\n", playlist.name, playlist.tracks_count, playlist.uri);
      return 0; // Ignore
    }

  map_playlist_to_pli(&pli, &playlist);

  pl_id = playlist_add_or_update(&pli);
  pli.id = pl_id;

  if (pl_id > 0)
    scan_playlist_tracks(playlist.tracks_href, &pli, request_type);
  else
    DPRINTF(E_LOG, L_SPOTIFY, "Error adding playlist: '%s' (%s) \n", playlist.name, playlist.uri);

  free_pli(&pli, 1);

  DPRINTF(E_LOG, L_SPOTIFY, "Scanned %d of %d saved playlists\n", (index + 1), total);

  return 0;
}

/*
 * Thread: library
 *
 * Scan users saved playlists into the library
 */
static int
scan_playlists(enum spotify_request_type request_type)
{
  int ret;

  ret = request_pagingobject_endpoint(spotify_playlists_uri, saved_playlist_add, NULL, NULL, false, request_type, NULL);

  return ret;
}

/*
 * Add or update playlist folder for all spotify playlists (if enabled in config)
 */
static void
create_base_playlist(void)
{
  cfg_t *spotify_cfg;
  struct playlist_info pli =
    {
      .path = strdup("spotify:playlistfolder"),
      .title = strdup("Spotify"),
      .type = PL_FOLDER,
      .scan_kind = SCAN_KIND_SPOTIFY,
    };

  spotify_base_plid = 0;
  spotify_cfg = cfg_getsec(cfg, "spotify");
  if (cfg_getbool(spotify_cfg, "base_playlist_disable"))
    {
      free_pli(&pli, 1);
      return;
    }

  spotify_base_plid = playlist_add_or_update(&pli);
  if (spotify_base_plid < 0)
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Error adding base playlist\n");
      spotify_base_plid = 0;
    }

  free_pli(&pli, 1);
}

static void
scan(enum spotify_request_type request_type)
{
  struct spotify_status sp_status;
  time_t start;
  time_t end;

  if (!credentials_token_exists() || scanning)
    {
      DPRINTF(E_DBG, L_SPOTIFY, "No valid web api token or scan already in progress, rescan ignored\n");
      return;
    }

  start = time(NULL);
  scanning = true;

  db_directory_enable_bypath("/spotify:");
  create_base_playlist();

  scan_saved_albums(request_type);
  scan_playlists(request_type);
  spotify_status_get(&sp_status);
  if (sp_status.has_podcast_support)
    scan_saved_shows(request_type);

  scanning = false;
  end = time(NULL);

  DPRINTF(E_LOG, L_SPOTIFY, "Spotify scan completed in %.f sec\n", difftime(end, start));
}


/* --------------------------- Library interface ---------------------------- */
/*                              Thread: library                               */

static int
spotifywebapi_library_queue_item_add(const char *uri, int position, char reshuffle, uint32_t item_id, int *count, int *new_item_id)
{
  enum spotify_item_type type;

  type = parse_type_from_uri(uri);
  if (type == SPOTIFY_ITEM_TYPE_TRACK)
    {
      queue_add_track(count, new_item_id, uri, position, reshuffle, item_id);
      goto out;
    }
  else if (type == SPOTIFY_ITEM_TYPE_ARTIST)
    {
      queue_add_artist(count, new_item_id, uri, position, reshuffle, item_id);
      goto out;
    }
  else if (type == SPOTIFY_ITEM_TYPE_ALBUM)
    {
      queue_add_album(count, new_item_id, uri, position, reshuffle, item_id);
      goto out;
    }
  else if (type == SPOTIFY_ITEM_TYPE_PLAYLIST)
    {
      queue_add_playlist(count, new_item_id, uri, position, reshuffle, item_id);
      goto out;
    }

  return LIBRARY_PATH_INVALID;

 out:
  return LIBRARY_OK;
}

static int
spotifywebapi_library_initscan(void)
{
  int ret;

  /* Refresh access token for the spotify webapi */
  ret = token_refresh();
  if (ret < 0)
    {
      // User not logged in or error refreshing token
      db_spotify_purge();
      return 0;
    }

  /*
   * Check that the playback Spotify backend can log in, so we don't add tracks
   * to the library that can't be played.
   */
  ret = spotify_relogin();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Spotify playback library could not log in. In order to use Spotify, "
	"provide valid credentials by visiting http://owntone.local:3689\n");

      db_spotify_purge();
      return 0;
    }

  /*
   * Scan saved tracks from the web api
   */
  scan(SPOTIFY_REQUEST_TYPE_RESCAN);
  return 0;
}

static int
spotifywebapi_library_rescan(void)
{
  scan(SPOTIFY_REQUEST_TYPE_RESCAN);
  return 0;
}

static int
spotifywebapi_library_metarescan(void)
{
  scan(SPOTIFY_REQUEST_TYPE_METARESCAN);
  return 0;
}

static int
spotifywebapi_library_fullrescan(void)
{
  db_spotify_purge();

  scan(SPOTIFY_REQUEST_TYPE_RESCAN);
  return 0;
}

static int
spotifywebapi_library_init()
{
  int ret;

  ret = spotify_init();
  if (ret < 0)
    return -1;

  session = http_client_session_new();
  return 0;
}

static void
spotifywebapi_library_deinit()
{
  spotify_deinit();

  http_client_session_free(session);

  credentials_clear();
}

struct library_source spotifyscanner =
{
  .scan_kind = SCAN_KIND_SPOTIFY,
  .disabled = 0,
  .queue_item_add = spotifywebapi_library_queue_item_add,
  .initscan = spotifywebapi_library_initscan,
  .rescan = spotifywebapi_library_rescan,
  .metarescan = spotifywebapi_library_metarescan,
  .fullrescan = spotifywebapi_library_fullrescan,
  .init = spotifywebapi_library_init,
  .deinit = spotifywebapi_library_deinit,
};


/* ------------------------ Public API command callbacks -------------------- */
/*                              Thread: library                               */

static enum command_state
webapi_fullrescan(void *arg, int *ret)
{
  *ret = spotifywebapi_library_fullrescan();
  return COMMAND_END;
}

static enum command_state
webapi_rescan(void *arg, int *ret)
{
  *ret = spotifywebapi_library_rescan();
  return COMMAND_END;
}

static enum command_state
webapi_purge(void *arg, int *ret)
{
  credentials_clear();

  db_spotify_purge();
  db_admin_delete(DB_ADMIN_SPOTIFY_REFRESH_TOKEN);

  *ret = 0;
  return COMMAND_END;
}


/* ------------------------------ Public API -------------------------------- */

char *
spotifywebapi_oauth_uri_get(const char *redirect_uri)
{
  struct keyval kv = { 0 };
  char *param;
  char *uri;
  int uri_len;
  int ret;

  uri = NULL;
  ret = ( (keyval_add(&kv, "client_id", spotify_client_id) == 0) &&
	  (keyval_add(&kv, "response_type", "code") == 0) &&
	  (keyval_add(&kv, "redirect_uri", redirect_uri) == 0) &&
	  (keyval_add(&kv, "scope", spotify_scope) == 0) &&
	  (keyval_add(&kv, "show_dialog", "false") == 0) );
  if (!ret)
    {
      DPRINTF(E_LOG, L_SPOTIFY, "Cannot display Spotify oath interface (error adding parameters to keyval)\n");
      goto out_clear_kv;
    }

  param = http_form_urlencode(&kv);
  if (param)
    {
      uri_len = strlen(spotify_auth_uri) + strlen(param) + 3;

      CHECK_NULL(L_SPOTIFY, uri = calloc(uri_len, sizeof(char)));

      snprintf(uri, uri_len, "%s/?%s", spotify_auth_uri, param);

      free(param);
    }

 out_clear_kv:
  keyval_clear(&kv);

  return uri;
}

int
spotifywebapi_oauth_callback(struct evkeyvalq *param, const char *redirect_uri, const char **errmsg)
{
  const char *code;
  char *user = NULL;
  char *access_token = NULL;
  int ret;

  *errmsg = NULL;

  code = evhttp_find_header(param, "code");
  if (!code)
    {
      *errmsg = "Error: Didn't receive a code from Spotify";
      return -1;
    }

  DPRINTF(E_DBG, L_SPOTIFY, "Received OAuth code: %s\n", code);

  ret = token_get(code, redirect_uri, errmsg);
  if (ret < 0)
    goto error;

  credentials_user_token_get(&user, &access_token);
  ret = spotify_login_token(user, access_token, errmsg);

  free(user);
  free(access_token);

  if (ret < 0)
    goto error;

  // Trigger scan after successful access to spotifywebapi
  spotifywebapi_fullrescan();

  listener_notify(LISTENER_SPOTIFY);

  return 0;

 error:
  return -1;
}

void
spotifywebapi_fullrescan(void)
{
  library_exec_async(webapi_fullrescan, NULL);
}

void
spotifywebapi_rescan(void)
{
  library_exec_async(webapi_rescan, NULL);
}

void
spotifywebapi_purge(void)
{
  library_exec_async(webapi_purge, NULL);
}

char *
spotifywebapi_artwork_url_get(const char *uri, int max_w, int max_h)
{
  enum spotify_item_type type;
  json_object *response;
  struct spotify_track track;
  char *artwork_url;

  type = parse_type_from_uri(uri);
  if (type == SPOTIFY_ITEM_TYPE_TRACK)
    {
      response = request_track(uri);
      if (response)
	parse_metadata_track(response, &track, max_w);
    }
  else if (type == SPOTIFY_ITEM_TYPE_EPISODE)
    {
      response = request_episode(uri);
      if (response)
	parse_metadata_episode(response, &track, max_w);
    }
  else
    {
      DPRINTF(E_WARN, L_SPOTIFY, "Unsupported Spotify type for artwork request: '%s'\n", uri);
      return NULL;
    }

  if (!response)
    {
      return NULL;
    }

  DPRINTF(E_DBG, L_SPOTIFY, "Got track artwork url: '%s' (%s) \n", track.artwork_url, track.uri);

  artwork_url = safe_strdup(track.artwork_url);
  jparse_free(response);

  return artwork_url;
}

void
spotifywebapi_status_info_get(struct spotifywebapi_status_info *info)
{
  credentials_status_info(info);
}

void
spotifywebapi_access_token_get(struct spotifywebapi_access_token *info)
{
  token_refresh();
  credentials_token_info(info);
}
