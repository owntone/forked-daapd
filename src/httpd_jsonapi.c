/*
 * Copyright (C) 2017 Christian Meffert <christian.meffert@googlemail.com>
 *
 * Adapted from httpd_adm.c:
 * Copyright (C) 2015 Stuart NAIFEH <stu@naifeh.org>
 *
 * Adapted from httpd_daap.c and httpd.c:
 * Copyright (C) 2009-2011 Julien BLACHE <jb@jblache.org>
 * Copyright (C) 2010 Kai Elwert <elwertk@googlemail.com>
 *
 * Adapted from mt-daapd:
 * Copyright (C) 2003-2007 Ron Pedde <ron@pedde.com>
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

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "httpd_internal.h"
#include "conffile.h"
#include "db.h"
#ifdef LASTFM
# include "lastfm.h"
#endif
#include "library.h"
#include "listenbrainz.h"
#include "logger.h"
#include "misc.h"
#include "misc_json.h"
#include "player.h"
#include "remote_pairing.h"
#include "settings.h"
#include "smartpl_query.h"
#ifdef SPOTIFY
# include "library/spotify_webapi.h"
# include "inputs/spotify.h"
#endif

struct track_attribs
{
  enum library_attrib type;
  const char *name;
};

// Currently these must all be uint32
static const struct track_attribs track_attribs[] =
{
  { LIBRARY_ATTRIB_PLAY_COUNT, "play_count", },
  { LIBRARY_ATTRIB_SKIP_COUNT, "skip_count", },
  { LIBRARY_ATTRIB_TIME_PLAYED, "time_played", },
  { LIBRARY_ATTRIB_TIME_SKIPPED, "time_skipped", },
  { LIBRARY_ATTRIB_RATING, "rating", },
  { LIBRARY_ATTRIB_USERMARK, "usermark", },
};

static bool allow_modifying_stored_playlists;
static char *default_playlist_directory;


/* -------------------------------- HELPERS --------------------------------- */

static bool
is_modified(struct httpd_request *hreq, const char *key)
{
  int64_t db_update = 0;

  db_admin_getint64(&db_update, key);

  return (!db_update || !httpd_request_not_modified_since(hreq, (time_t)db_update));
}

static inline void
safe_json_add_string(json_object *obj, const char *key, const char *value)
{
  if (value)
    json_object_object_add(obj, key, json_object_new_string(value));
}

static inline void
safe_json_add_string_from_int64(json_object *obj, const char *key, int64_t value)
{
  char tmp[100];
  int ret;

  if (value > 0)
    {
      ret = snprintf(tmp, sizeof(tmp), "%" PRIi64, value);
      if (ret < sizeof(tmp))
	json_object_object_add(obj, key, json_object_new_string(tmp));
    }
}

static inline void
safe_json_add_int_from_string(json_object *obj, const char *key, const char *value)
{
  int intval;
  int ret;

  if (!value)
    return;

  ret = safe_atoi32(value, &intval);
  if (ret == 0)
    json_object_object_add(obj, key, json_object_new_int(intval));
}

static inline void
safe_json_add_time_from_string(json_object *obj, const char *key, const char *value)
{
  uint32_t tmp;
  time_t timestamp;
  struct tm tm;
  char result[32];

  if (!value)
    return;

  if (safe_atou32(value, &tmp) != 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error converting timestamp to uint32_t: %s\n", value);
      return;
    }

  if (!tmp)
    return;

  timestamp = tmp;
  if (gmtime_r(&timestamp, &tm) == NULL)
    {
      DPRINTF(E_LOG, L_WEB, "Error converting timestamp to gmtime: %s\n", value);
      return;
    }

  strftime(result, sizeof(result), "%FT%TZ", &tm);

  json_object_object_add(obj, key, json_object_new_string(result));
}

static inline void
safe_json_add_date_from_string(json_object *obj, const char *key, const char *value)
{
  int64_t tmp;
  time_t timestamp;
  struct tm tm;
  char result[32];

  if (!value)
    return;

  if (safe_atoi64(value, &tmp) != 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error converting timestamp to int64_t: %s\n", value);
      return;
    }

  if (!tmp)
    return;

  timestamp = tmp;
  if (localtime_r(&timestamp, &tm) == NULL)
    {
      DPRINTF(E_LOG, L_WEB, "Error converting timestamp to localtime: %s\n", value);
      return;
    }

  strftime(result, sizeof(result), "%F", &tm);

  json_object_object_add(obj, key, json_object_new_string(result));
}

static json_object *
artist_to_json(struct db_group_info *dbgri)
{
  json_object *item;
  int intval;
  char uri[100];
  char artwork_url[100];
  int ret;

  item = json_object_new_object();

  safe_json_add_string(item, "id", dbgri->persistentid);
  safe_json_add_string(item, "name", dbgri->itemname);
  safe_json_add_string(item, "name_sort", dbgri->itemname_sort);
  safe_json_add_int_from_string(item, "album_count", dbgri->groupalbumcount);
  safe_json_add_int_from_string(item, "track_count", dbgri->itemcount);
  safe_json_add_int_from_string(item, "length_ms", dbgri->song_length);

  safe_json_add_time_from_string(item, "time_played", dbgri->time_played);
  safe_json_add_time_from_string(item, "time_added", dbgri->time_added);

  ret = safe_atoi32(dbgri->seek, &intval);
  if (ret == 0)
    json_object_object_add(item, "in_progress", json_object_new_boolean(intval > 0));

  ret = safe_atoi32(dbgri->media_kind, &intval);
  if (ret == 0)
    safe_json_add_string(item, "media_kind", db_media_kind_label(intval));

  ret = safe_atoi32(dbgri->data_kind, &intval);
  if (ret == 0)
    safe_json_add_string(item, "data_kind", db_data_kind_label(intval));

  ret = snprintf(uri, sizeof(uri), "%s:%s:%s", "library", "artist", dbgri->persistentid);
  if (ret < sizeof(uri))
    json_object_object_add(item, "uri", json_object_new_string(uri));

  ret = snprintf(artwork_url, sizeof(artwork_url), "./artwork/group/%s", dbgri->id);
  if (ret < sizeof(artwork_url))
    json_object_object_add(item, "artwork_url", json_object_new_string(artwork_url));

  return item;
}

static json_object *
album_to_json(struct db_group_info *dbgri)
{
  json_object *item;
  int intval;
  char uri[100];
  char artwork_url[100];
  int ret;

  item = json_object_new_object();

  safe_json_add_string(item, "id", dbgri->persistentid);
  safe_json_add_string(item, "name", dbgri->itemname);
  safe_json_add_string(item, "name_sort", dbgri->itemname_sort);
  safe_json_add_string(item, "artist", dbgri->songalbumartist);
  safe_json_add_string(item, "artist_id", dbgri->songartistid);
  safe_json_add_int_from_string(item, "track_count", dbgri->itemcount);
  safe_json_add_int_from_string(item, "length_ms", dbgri->song_length);

  safe_json_add_time_from_string(item, "time_played", dbgri->time_played);
  safe_json_add_time_from_string(item, "time_added", dbgri->time_added);

  ret = safe_atoi32(dbgri->seek, &intval);
  if (ret == 0)
    json_object_object_add(item, "in_progress", json_object_new_boolean(intval > 0));

  ret = safe_atoi32(dbgri->media_kind, &intval);
  if (ret == 0)
    safe_json_add_string(item, "media_kind", db_media_kind_label(intval));

  ret = safe_atoi32(dbgri->data_kind, &intval);
  if (ret == 0)
    safe_json_add_string(item, "data_kind", db_data_kind_label(intval));

  safe_json_add_date_from_string(item, "date_released", dbgri->date_released);
  safe_json_add_int_from_string(item, "year", dbgri->year);

  ret = snprintf(uri, sizeof(uri), "%s:%s:%s", "library", "album", dbgri->persistentid);
  if (ret < sizeof(uri))
    json_object_object_add(item, "uri", json_object_new_string(uri));

  ret = snprintf(artwork_url, sizeof(artwork_url), "./artwork/group/%s", dbgri->id);
  if (ret < sizeof(artwork_url))
    json_object_object_add(item, "artwork_url", json_object_new_string(artwork_url));

  return item;
}

static json_object *
track_to_json(struct db_media_file_info *dbmfi)
{
  json_object *item;
  char uri[100];
  char artwork_url[100];
  int intval;
  int ret;

  item = json_object_new_object();

  safe_json_add_int_from_string(item, "id", dbmfi->id);
  safe_json_add_string(item, "title", dbmfi->title);
  safe_json_add_string(item, "title_sort", dbmfi->title_sort);
  safe_json_add_string(item, "artist", dbmfi->artist);
  safe_json_add_string(item, "artist_sort", dbmfi->artist_sort);
  safe_json_add_string(item, "album", dbmfi->album);
  safe_json_add_string(item, "album_sort", dbmfi->album_sort);
  safe_json_add_string(item, "album_id", dbmfi->songalbumid);
  safe_json_add_string(item, "album_artist", dbmfi->album_artist);
  safe_json_add_string(item, "album_artist_sort", dbmfi->album_artist_sort);
  safe_json_add_string(item, "album_artist_id", dbmfi->songartistid);
  safe_json_add_string(item, "composer", dbmfi->composer);
  safe_json_add_string(item, "genre", dbmfi->genre);
  safe_json_add_string(item, "comment", dbmfi->comment);
  safe_json_add_int_from_string(item, "year", dbmfi->year);
  safe_json_add_int_from_string(item, "track_number", dbmfi->track);
  safe_json_add_int_from_string(item, "disc_number", dbmfi->disc);
  safe_json_add_int_from_string(item, "length_ms", dbmfi->song_length);

  safe_json_add_int_from_string(item, "rating", dbmfi->rating);
  safe_json_add_int_from_string(item, "play_count", dbmfi->play_count);
  safe_json_add_int_from_string(item, "skip_count", dbmfi->skip_count);
  safe_json_add_time_from_string(item, "time_played", dbmfi->time_played);
  safe_json_add_time_from_string(item, "time_skipped", dbmfi->time_skipped);
  safe_json_add_time_from_string(item, "time_added", dbmfi->time_added);
  safe_json_add_date_from_string(item, "date_released", dbmfi->date_released);
  safe_json_add_int_from_string(item, "seek_ms", dbmfi->seek);

  safe_json_add_string(item, "type", dbmfi->type);
  safe_json_add_int_from_string(item, "samplerate", dbmfi->samplerate);
  safe_json_add_int_from_string(item, "bitrate", dbmfi->bitrate);
  safe_json_add_int_from_string(item, "channels", dbmfi->channels);
  safe_json_add_int_from_string(item, "usermark", dbmfi->usermark);

  ret = safe_atoi32(dbmfi->media_kind, &intval);
  if (ret == 0)
    safe_json_add_string(item, "media_kind", db_media_kind_label(intval));

  ret = safe_atoi32(dbmfi->data_kind, &intval);
  if (ret == 0)
    safe_json_add_string(item, "data_kind", db_data_kind_label(intval));

  safe_json_add_string(item, "path", dbmfi->path);

  ret = snprintf(uri, sizeof(uri), "library:track:%s", dbmfi->id);
  if (ret < sizeof(uri))
    json_object_object_add(item, "uri", json_object_new_string(uri));

  ret = snprintf(artwork_url, sizeof(artwork_url), "/artwork/item/%s", dbmfi->id);
  if (ret < sizeof(artwork_url))
    json_object_object_add(item, "artwork_url", json_object_new_string(artwork_url));

  safe_json_add_string(item, "lyrics", dbmfi->lyrics);
  return item;
}

static json_object *
playlist_to_json(struct db_playlist_info *dbpli)
{
  json_object *item;
  char uri[100];
  int intval;
  bool boolval;
  int ret;

  item = json_object_new_object();

  safe_json_add_int_from_string(item, "id", dbpli->id);
  safe_json_add_string(item, "name", dbpli->title);
  safe_json_add_string(item, "path", dbpli->path);
  safe_json_add_string(item, "parent_id", dbpli->parent_id);
  ret = safe_atoi32(dbpli->type, &intval);
  if (ret == 0)
    {
      safe_json_add_string(item, "type", db_pl_type_label(intval));
      json_object_object_add(item, "smart_playlist", json_object_new_boolean(intval == PL_SMART));

      boolval = dbpli->query_order && strcasestr(dbpli->query_order, "random");
      json_object_object_add(item, "random", json_object_new_boolean(boolval));

      json_object_object_add(item, "folder", json_object_new_boolean(intval == PL_FOLDER));

      if (intval != PL_FOLDER)
	{
	  safe_json_add_int_from_string(item, "item_count", dbpli->items);
	  safe_json_add_int_from_string(item, "stream_count", dbpli->streams);
	}
    }

  ret = snprintf(uri, sizeof(uri), "%s:%s:%s", "library", "playlist", dbpli->id);
  if (ret < sizeof(uri))
    json_object_object_add(item, "uri", json_object_new_string(uri));

  return item;
}

static json_object *
browse_info_to_json(struct db_browse_info *dbbi)
{
  json_object *item;
  int intval;
  int ret;

  if (dbbi == NULL)
    {
      return NULL;
    }

  item = json_object_new_object();
  safe_json_add_string(item, "name", dbbi->itemname);
  safe_json_add_string(item, "name_sort", dbbi->itemname_sort);
  safe_json_add_int_from_string(item, "track_count", dbbi->track_count);
  safe_json_add_int_from_string(item, "album_count", dbbi->album_count);
  safe_json_add_int_from_string(item, "artist_count", dbbi->artist_count);
  safe_json_add_int_from_string(item, "length_ms", dbbi->song_length);

  safe_json_add_time_from_string(item, "time_played", dbbi->time_played);
  safe_json_add_time_from_string(item, "time_added", dbbi->time_added);

  ret = safe_atoi32(dbbi->seek, &intval);
  if (ret == 0)
    json_object_object_add(item, "in_progress", json_object_new_boolean(intval > 0));

  ret = safe_atoi32(dbbi->media_kind, &intval);
  if (ret == 0)
    safe_json_add_string(item, "media_kind", db_media_kind_label(intval));

  ret = safe_atoi32(dbbi->data_kind, &intval);
  if (ret == 0)
    safe_json_add_string(item, "data_kind", db_data_kind_label(intval));

  safe_json_add_date_from_string(item, "date_released", dbbi->date_released);
  safe_json_add_int_from_string(item, "year", dbbi->year);

  return item;
}

static json_object *
directory_to_json(struct directory_info *directory_info)
{
  json_object *item;

  if (directory_info == NULL)
    {
      return NULL;
    }

  item = json_object_new_object();
  safe_json_add_string(item, "path", directory_info->path);
//  json_object_object_add(item, "id", json_object_new_int(directory_info->id));
//  json_object_object_add(item, "parent_id", json_object_new_int(directory_info->parent_id));

  return item;
}


static int
fetch_tracks(struct query_params *query_params, json_object *items, int *total)
{
  struct db_media_file_info dbmfi;
  json_object *item;
  int ret;

  ret = db_query_start(query_params);
  if (ret < 0)
    goto error;

  while ((ret = db_query_fetch_file(&dbmfi, query_params)) == 0)
    {
      item = track_to_json(&dbmfi);
      if (!item)
	{
	  ret = -1;
	  goto error;
	}

      json_object_array_add(items, item);
    }

  if (total)
    *total = query_params->results;

 error:
  db_query_end(query_params);

  return ret;
}

static int
fetch_artists(struct query_params *query_params, json_object *items, int *total)
{
  struct db_group_info dbgri;
  json_object *item;
  int ret = 0;

  ret = db_query_start(query_params);
  if (ret < 0)
    goto error;

  while ((ret = db_query_fetch_group(&dbgri, query_params)) == 0)
    {
      /* Don't add item if no name (eg blank album name) */
      if (strlen(dbgri.itemname) == 0)
	continue;

      item = artist_to_json(&dbgri);
      if (!item)
	{
	  ret = -1;
	  goto error;
	}

      json_object_array_add(items, item);
    }

  if (total)
    *total = query_params->results;

 error:
  db_query_end(query_params);

  return ret;
}

static json_object *
fetch_artist(bool *notfound, const char *artist_id)
{
  struct query_params query_params;
  json_object *artist;
  struct db_group_info dbgri;
  int ret = 0;

  *notfound = true;
  memset(&query_params, 0, sizeof(struct query_params));
  artist = NULL;

  query_params.type = Q_GROUP_ARTISTS;
  query_params.sort = S_ARTIST;
  query_params.filter = db_mprintf("(f.songartistid = %s)", artist_id);

  ret = db_query_start(&query_params);
  if (ret < 0)
    goto error;

  if ((ret = db_query_fetch_group(&dbgri, &query_params)) == 0)
    {
      artist = artist_to_json(&dbgri);
      *notfound = false;
    }

 error:
  db_query_end(&query_params);
  free(query_params.filter);

  return artist;
}

static int
fetch_albums(struct query_params *query_params, json_object *items, int *total)
{
  struct db_group_info dbgri;
  json_object *item;
  int ret = 0;

  ret = db_query_start(query_params);
  if (ret < 0)
    goto error;

  while ((ret = db_query_fetch_group(&dbgri, query_params)) == 0)
    {
      /* Don't add item if no name (eg blank album name) */
      if (strlen(dbgri.itemname) == 0)
	continue;

      item = album_to_json(&dbgri);
      if (!item)
	{
	  ret = -1;
	  goto error;
	}

      json_object_array_add(items, item);
    }

  if (total)
    *total = query_params->results;

 error:
  db_query_end(query_params);

  return ret;
}

static json_object *
fetch_album(bool *notfound, const char *album_id)
{
  struct query_params query_params;
  json_object *album;
  struct db_group_info dbgri;
  int ret = 0;

  *notfound = true;

  memset(&query_params, 0, sizeof(struct query_params));
  album = NULL;

  query_params.type = Q_GROUP_ALBUMS;
  query_params.sort = S_ALBUM;
  query_params.filter = db_mprintf("(f.songalbumid = %s)", album_id);

  ret = db_query_start(&query_params);
  if (ret < 0)
    goto error;

  if ((ret = db_query_fetch_group(&dbgri, &query_params)) == 0)
    {
      album = album_to_json(&dbgri);
      *notfound = false;
    }

 error:
  db_query_end(&query_params);
  free(query_params.filter);

  return album;
}

static int
fetch_playlists(struct query_params *query_params, json_object *items, int *total)
{
  struct db_playlist_info dbpli;
  json_object *item;
  int ret = 0;

  ret = db_query_start(query_params);
  if (ret < 0)
    goto error;

  while (((ret = db_query_fetch_pl(&dbpli, query_params)) == 0) && (dbpli.id))
    {
      item = playlist_to_json(&dbpli);
      if (!item)
	{
	  ret = -1;
	  goto error;
	}

      json_object_array_add(items, item);
    }

  if (total)
    *total = query_params->results;

 error:
  db_query_end(query_params);

  return ret;
}

static json_object *
fetch_playlist(bool *notfound, uint32_t playlist_id)
{
  struct query_params query_params;
  json_object *playlist;
  struct db_playlist_info dbpli;
  int ret = 0;

  *notfound = true;

  memset(&query_params, 0, sizeof(struct query_params));
  playlist = NULL;

  query_params.type = Q_PL;
  query_params.sort = S_PLAYLIST;
  query_params.filter = db_mprintf("(f.id = %d)", playlist_id);

  ret = db_query_start(&query_params);
  if (ret < 0)
    goto error;

  if (((ret = db_query_fetch_pl(&dbpli, &query_params)) == 0) && (dbpli.id))
    {
      playlist = playlist_to_json(&dbpli);
      *notfound = false;
    }

 error:
  db_query_end(&query_params);
  free(query_params.filter);

  return playlist;
}

static int
fetch_browse_info(struct query_params *query_params, json_object *items, int *total)
{
  struct db_browse_info dbbi;
  json_object *item;
  int ret;

  ret = db_query_start(query_params);
  if (ret < 0)
    goto error;

  while ((ret = db_query_fetch_browse(&dbbi, query_params)) == 0)
    {
      item = browse_info_to_json(&dbbi);
      if (!item)
	{
	  ret = -1;
	  goto error;
	}

      json_object_array_add(items, item);
    }

  if (total)
    *total = query_params->results;

 error:
  db_query_end(query_params);

  return ret;
}

static int
fetch_directories(int parent_id, json_object *items)
{
  json_object *item;
  int ret;
  struct directory_info subdir;
  struct directory_enum dir_enum;

  memset(&dir_enum, 0, sizeof(struct directory_enum));
  dir_enum.parent_id = parent_id;
  ret = db_directory_enum_start(&dir_enum);
  if (ret < 0)
    goto error;

  while ((ret = db_directory_enum_fetch(&dir_enum, &subdir)) == 0 && subdir.id > 0)
    {
      item = directory_to_json(&subdir);
      if (!item)
      {
	ret = -1;
	goto error;
      }

      json_object_array_add(items, item);
    }

 error:
  db_directory_enum_end(&dir_enum);

  return ret;
}


static int
query_params_limit_set(struct query_params *query_params, struct httpd_request *hreq)
{
  const char *param;

  query_params->idx_type = I_NONE;
  query_params->limit = -1;
  query_params->offset = 0;

  param = httpd_query_value_find(hreq->query, "limit");
  if (param)
    {
      query_params->idx_type = I_SUB;

      if (safe_atoi32(param, &query_params->limit) < 0)
        {
	  DPRINTF(E_LOG, L_WEB, "Invalid value for query parameter 'limit' (%s)\n", param);
	  return -1;
	}

      param = httpd_query_value_find(hreq->query, "offset");
      if (param && safe_atoi32(param, &query_params->offset) < 0)
        {
	  DPRINTF(E_LOG, L_WEB, "Invalid value for query parameter 'offset' (%s)\n", param);
	  return -1;
	}
    }

  return 0;
}

/* --------------------------- REPLY HANDLERS ------------------------------- */

/*
 * Endpoint to retrieve configuration values
 *
 * Example response:
 *
 * {
 *  "websocket_port": 6603,
 *  "version": "25.0"
 * }
 */
static int
jsonapi_reply_config(struct httpd_request *hreq)
{
  json_object *jreply;
  json_object *buildopts;
  int websocket_port;
  char **buildoptions;
  cfg_t *lib;
  int ndirs;
  char *path;
  char *deref;
  json_object *directories;
  int i;

  CHECK_NULL(L_WEB, jreply = json_object_new_object());

  // library name
  json_object_object_add(jreply, "library_name", json_object_new_string(cfg_getstr(cfg_getsec(cfg, "library"), "name")));

  // hide singles
  json_object_object_add(jreply, "hide_singles", json_object_new_boolean(cfg_getbool(cfg_getsec(cfg, "library"), "hide_singles")));

  // Websocket port
#ifdef HAVE_LIBWEBSOCKETS
  websocket_port = cfg_getint(cfg_getsec(cfg, "general"), "websocket_port");
#else
  websocket_port = 0;
#endif
  json_object_object_add(jreply, "websocket_port", json_object_new_int(websocket_port));

  // server version
  json_object_object_add(jreply, "version", json_object_new_string(VERSION));

  // enabled build options
  buildopts = json_object_new_array();
  buildoptions = buildopts_get();
  for (i = 0; buildoptions[i]; i++)
    {
      json_object_array_add(buildopts, json_object_new_string(buildoptions[i]));
    }
  json_object_object_add(jreply, "buildoptions", buildopts);

  // Library directories
  lib = cfg_getsec(cfg, "library");
  ndirs = cfg_size(lib, "directories");
  directories = json_object_new_array();
  for (i = 0; i < ndirs; i++)
    {
      path = cfg_getnstr(lib, "directories", i);

      // The path in the conf file may have a trailing slash character. Return the realpath like it is done in the bulk_scan function in filescanner.c
      deref = realpath(path, NULL);
      if (deref)
        {
	  json_object_array_add(directories, json_object_new_string(deref));
	  free(deref);
	}
      else
	{
	  DPRINTF(E_LOG, L_WEB, "Skipping library directory %s, could not dereference: %s\n", path, strerror(errno));
	}
    }
  json_object_object_add(jreply, "directories", directories);
  json_object_object_add(jreply, "radio_playlists", json_object_new_boolean(cfg_getbool(lib, "radio_playlists")));

  // Config for creating/modifying stored playlists
  json_object_object_add(jreply, "allow_modifying_stored_playlists", json_object_new_boolean(allow_modifying_stored_playlists));
  safe_json_add_string(jreply, "default_playlist_directory", default_playlist_directory);

  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

  jparse_free(jreply);

  return HTTP_OK;
}

static json_object *
option_get_json(struct settings_option *option)
{
  const char *optionname;
  json_object *json_option;
  int intval;
  bool boolval;
  char *strval;


  optionname = option->name;

  CHECK_NULL(L_WEB, json_option = json_object_new_object());
  json_object_object_add(json_option, "name", json_object_new_string(option->name));
  json_object_object_add(json_option, "type", json_object_new_int(option->type));

  if (option->type == SETTINGS_TYPE_INT)
    {
      intval = settings_option_getint(option);
      json_object_object_add(json_option, "value", json_object_new_int(intval));
    }
  else if (option->type == SETTINGS_TYPE_BOOL)
    {
      boolval = settings_option_getbool(option);
      json_object_object_add(json_option, "value", json_object_new_boolean(boolval));
    }
  else if (option->type == SETTINGS_TYPE_STR)
    {
      strval = settings_option_getstr(option);
      if (strval)
	{
	  json_object_object_add(json_option, "value", json_object_new_string(strval));
	  free(strval);
	}
    }
  else
    {
      DPRINTF(E_LOG, L_WEB, "Option '%s' has unknown type %d\n", optionname, option->type);
      jparse_free(json_option);
      return NULL;
    }

  return json_option;
}

static json_object *
category_get_json(struct settings_category *category)
{
  json_object *json_category;
  json_object *json_options;
  json_object *json_option;
  struct settings_option *option;
  int count;
  int i;

  json_category = json_object_new_object();

  json_object_object_add(json_category, "name", json_object_new_string(category->name));

  json_options = json_object_new_array();

  count = settings_option_count(category);
  for (i = 0; i < count; i++)
    {
      option = settings_option_get_byindex(category, i);
      json_option = option_get_json(option);
      if (json_option)
	json_object_array_add(json_options, json_option);
    }

  json_object_object_add(json_category, "options", json_options);

  return json_category;
}

static int
jsonapi_reply_settings_get(struct httpd_request *hreq)
{
  struct settings_category *category;
  json_object *jreply;
  json_object *json_categories;
  json_object *json_category;
  int count;
  int i;

  CHECK_NULL(L_WEB, jreply = json_object_new_object());

  json_categories = json_object_new_array();

  count = settings_categories_count();
  for (i = 0; i < count; i++)
    {
      category = settings_category_get_byindex(i);
      json_category = category_get_json(category);
      if (json_category)
	json_object_array_add(json_categories, json_category);
    }

  json_object_object_add(jreply, "categories", json_categories);

  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

  jparse_free(jreply);

  return HTTP_OK;
}

static int
jsonapi_reply_settings_category_get(struct httpd_request *hreq)
{
  const char *categoryname;
  struct settings_category *category;
  json_object *jreply;


  categoryname = hreq->path_parts[2];

  category = settings_category_get(categoryname);
  if (!category)
    {
      DPRINTF(E_LOG, L_WEB, "Invalid category name '%s' given\n", categoryname);
      return HTTP_NOTFOUND;
    }

  jreply = category_get_json(category);

  if (!jreply)
    {
      DPRINTF(E_LOG, L_WEB, "Error getting value for category '%s'\n", categoryname);
      return HTTP_INTERNAL;
    }

  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

  jparse_free(jreply);

  return HTTP_OK;
}

static int
jsonapi_reply_settings_option_get(struct httpd_request *hreq)
{
  const char *categoryname;
  const char *optionname;
  struct settings_category *category;
  struct settings_option *option;
  json_object *jreply;


  categoryname = hreq->path_parts[2];
  optionname = hreq->path_parts[3];

  category = settings_category_get(categoryname);
  if (!category)
    {
      DPRINTF(E_LOG, L_WEB, "Invalid category name '%s' given\n", categoryname);
      return HTTP_NOTFOUND;
    }

  option = settings_option_get(category, optionname);
  if (!option)
    {
      DPRINTF(E_LOG, L_WEB, "Invalid option name '%s' given\n", optionname);
      return HTTP_NOTFOUND;
    }

  jreply = option_get_json(option);

  if (!jreply)
    {
      DPRINTF(E_LOG, L_WEB, "Error getting value for option '%s'\n", optionname);
      return HTTP_INTERNAL;
    }

  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

  jparse_free(jreply);

  return HTTP_OK;
}

static int
jsonapi_reply_settings_option_put(struct httpd_request *hreq)
{
  const char *categoryname;
  const char *optionname;
  struct settings_category *category;
  struct settings_option *option;
  json_object* request;
  int intval;
  bool boolval;
  const char *strval;
  int ret;


  categoryname = hreq->path_parts[2];
  optionname = hreq->path_parts[3];

  category = settings_category_get(categoryname);
  if (!category)
    {
      DPRINTF(E_LOG, L_WEB, "Invalid category name '%s' given\n", categoryname);
      return HTTP_NOTFOUND;
    }

  option = settings_option_get(category, optionname);

  if (!option)
    {
      DPRINTF(E_LOG, L_WEB, "Invalid option name '%s' given\n", optionname);
      return HTTP_NOTFOUND;
    }

  request = jparse_obj_from_evbuffer(hreq->in_body);
  if (!request)
    {
      DPRINTF(E_LOG, L_WEB, "Missing request body for setting option '%s' (type %d)\n", optionname, option->type);
      return HTTP_BADREQUEST;
    }

  if (option->type == SETTINGS_TYPE_INT && jparse_contains_key(request, "value", json_type_int))
    {
      intval = jparse_int_from_obj(request, "value");
      ret = settings_option_setint(option, intval);
    }
  else if (option->type == SETTINGS_TYPE_BOOL && jparse_contains_key(request, "value", json_type_boolean))
    {
      boolval = jparse_bool_from_obj(request, "value");
      ret = settings_option_setbool(option, boolval);
    }
  else if (option->type == SETTINGS_TYPE_STR && jparse_contains_key(request, "value", json_type_string))
    {
      strval = jparse_str_from_obj(request, "value");
      ret = settings_option_setstr(option, strval);
    }
  else
    {
      DPRINTF(E_LOG, L_WEB, "Invalid value given for option '%s' (type %d): '%s'\n", optionname, option->type, json_object_to_json_string(request));
      return HTTP_BADREQUEST;
    }

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error changing setting '%s' (type %d) to '%s'\n", optionname, option->type, json_object_to_json_string(request));
      return HTTP_INTERNAL;
    }

  DPRINTF(E_INFO, L_WEB, "Setting option '%s.%s' changed to '%s'\n", categoryname, optionname, json_object_to_json_string(request));
  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_settings_option_delete(struct httpd_request *hreq)
{
  const char *categoryname;
  const char *optionname;
  struct settings_category *category;
  struct settings_option *option;
  int ret;


  categoryname = hreq->path_parts[2];
  optionname = hreq->path_parts[3];

  category = settings_category_get(categoryname);
  if (!category)
    {
      DPRINTF(E_LOG, L_WEB, "Invalid category name '%s' given\n", categoryname);
      return HTTP_NOTFOUND;
    }

  option = settings_option_get(category, optionname);
  if (!option)
    {
      DPRINTF(E_LOG, L_WEB, "Invalid option name '%s' given\n", optionname);
      return HTTP_NOTFOUND;
    }

  ret = settings_option_delete(option);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error deleting option '%s'\n", optionname);
      return HTTP_INTERNAL;
    }

  return HTTP_NOCONTENT;
}

/*
 * Endpoint to retrieve informations about the library
 *
 * Example response:
 *
 * {
 *  "artists": 84,
 *  "albums": 151,
 *  "songs": 3085,
 *  "db_playtime": 687824,
 *  "updating": false
 *}
 */
static int
jsonapi_reply_library(struct httpd_request *hreq)
{
  struct query_params qp;
  struct filecount_info fci;
  json_object *jreply;
  int ret;
  char *s;
  int i;
  struct library_source **sources;
  json_object *jscanners;
  json_object *jsource;


  CHECK_NULL(L_WEB, jreply = json_object_new_object());

  memset(&qp, 0, sizeof(struct query_params));
  qp.type = Q_COUNT_ITEMS;
  ret = db_filecount_get(&fci, &qp);
  if (ret == 0)
    {
      json_object_object_add(jreply, "songs", json_object_new_int(fci.count));
      json_object_object_add(jreply, "db_playtime", json_object_new_int64((fci.length / 1000)));
      json_object_object_add(jreply, "artists", json_object_new_int(fci.artist_count));
      json_object_object_add(jreply, "albums", json_object_new_int(fci.album_count));
      json_object_object_add(jreply, "file_size", json_object_new_int64(fci.file_size));
    }
  else
    {
      DPRINTF(E_LOG, L_WEB, "library: failed to get file count info\n");
    }

  ret = db_admin_get(&s, DB_ADMIN_START_TIME);
  if (ret == 0)
    {
      safe_json_add_time_from_string(jreply, "started_at", s);
      free(s);
    }

  ret = db_admin_get(&s, DB_ADMIN_DB_UPDATE);
  if (ret == 0)
    {
      safe_json_add_time_from_string(jreply, "updated_at", s);
      free(s);
    }

  json_object_object_add(jreply, "updating", json_object_new_boolean(library_is_scanning()));

  jscanners = json_object_new_array();
  json_object_object_add(jreply, "scanners", jscanners);
  sources = library_sources();
  for (i = 0; sources[i]; i++)
    {
      if (!sources[i]->disabled)
	{
	  jsource = json_object_new_object();
	  safe_json_add_string(jsource, "name", db_scan_kind_label(sources[i]->scan_kind));
	  json_object_array_add(jscanners, jsource);
	}
    }

  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));
  jparse_free(jreply);

  return HTTP_OK;
}

/*
 * Endpoint to trigger a library rescan
 */
static int
jsonapi_reply_update(struct httpd_request *hreq)
{
  const char *param;
  const char *param_path;

  param = httpd_query_value_find(hreq->query, "scan_kind");
  param_path = httpd_query_value_find(hreq->query, "path");

  if (param_path)
    library_rescan_path(param_path);
  else
    library_rescan(db_scan_kind_enum(param));

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_meta_rescan(struct httpd_request *hreq)
{
  const char *param;

  param = httpd_query_value_find(hreq->query, "scan_kind");

  library_metarescan(db_scan_kind_enum(param));
  return HTTP_NOCONTENT;
}


/*
 * Endpoint to retrieve information about the spotify integration
 *
 * Exampe response:
 *
 * {
 *  "enabled": true,
 *  "oauth_uri": "https://accounts.spotify.com/authorize/?client_id=...
 * }
 */
static int
jsonapi_reply_spotify(struct httpd_request *hreq)
{
  json_object *jreply;

  CHECK_NULL(L_WEB, jreply = json_object_new_object());

#ifdef SPOTIFY
  int httpd_port;
  char redirect_uri[256];
  char *oauth_uri;
  struct spotify_status sp_status;
  struct spotifywebapi_status_info webapi_info;
  struct spotifywebapi_access_token webapi_token;

  json_object_object_add(jreply, "enabled", json_object_new_boolean(true));

  httpd_port = cfg_getint(cfg_getsec(cfg, "library"), "port");
  snprintf(redirect_uri, sizeof(redirect_uri), "http://owntone.local:%d/oauth/spotify", httpd_port);

  oauth_uri = spotifywebapi_oauth_uri_get(redirect_uri);
  if (!oauth_uri)
    {
      DPRINTF(E_LOG, L_WEB, "Cannot display Spotify oauth interface (http_form_uriencode() failed)\n");
      jparse_free(jreply);
      return HTTP_INTERNAL;
    }

  json_object_object_add(jreply, "oauth_uri", json_object_new_string(oauth_uri));
  free(oauth_uri);

  spotify_status_get(&sp_status);
  json_object_object_add(jreply, "spotify_installed", json_object_new_boolean(sp_status.installed));
  json_object_object_add(jreply, "spotify_logged_in", json_object_new_boolean(sp_status.logged_in));
  json_object_object_add(jreply, "has_podcast_support", json_object_new_boolean(sp_status.has_podcast_support));

  spotifywebapi_status_info_get(&webapi_info);
  json_object_object_add(jreply, "webapi_token_valid", json_object_new_boolean(webapi_info.token_valid));
  safe_json_add_string(jreply, "webapi_user", webapi_info.user);
  safe_json_add_string(jreply, "webapi_country", webapi_info.country);
  safe_json_add_string(jreply, "webapi_granted_scope", webapi_info.granted_scope);
  safe_json_add_string(jreply, "webapi_required_scope", webapi_info.required_scope);

  spotifywebapi_access_token_get(&webapi_token);
  safe_json_add_string(jreply, "webapi_token", webapi_token.token);
  json_object_object_add(jreply, "webapi_token_expires_in", json_object_new_int(webapi_token.expires_in));
  free(webapi_token.token);
#else
  json_object_object_add(jreply, "enabled", json_object_new_boolean(false));
#endif

  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

  jparse_free(jreply);

  return HTTP_OK;
}

static int
jsonapi_reply_spotify_logout(struct httpd_request *hreq)
{
#ifdef SPOTIFY
  spotifywebapi_purge();
  spotify_logout();
#endif
  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_lastfm(struct httpd_request *hreq)
{
  json_object *jreply;
  bool enabled = false;
  bool scrobbling_enabled = false;

#ifdef LASTFM
  enabled = true;
  scrobbling_enabled = lastfm_is_enabled();
#endif

  CHECK_NULL(L_WEB, jreply = json_object_new_object());

  json_object_object_add(jreply, "enabled", json_object_new_boolean(enabled));
  json_object_object_add(jreply, "scrobbling_enabled", json_object_new_boolean(scrobbling_enabled));

  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

  jparse_free(jreply);

  return HTTP_OK;
}

/*
 * Endpoint to log into LastFM
 */
static int
jsonapi_reply_lastfm_login(struct httpd_request *hreq)
{
#ifdef LASTFM
  json_object *request;
  const char *user;
  const char *password;
  char *errmsg = NULL;
  json_object *jreply;
  json_object *errors;
  int ret;

  DPRINTF(E_DBG, L_WEB, "Received LastFM login request\n");

  request = jparse_obj_from_evbuffer(hreq->in_body);
  if (!request)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to parse incoming request\n");
      return HTTP_BADREQUEST;
    }

  CHECK_NULL(L_WEB, jreply = json_object_new_object());

  user = jparse_str_from_obj(request, "user");
  password = jparse_str_from_obj(request, "password");
  if (user && strlen(user) > 0 && password && strlen(password) > 0)
    {
      ret = lastfm_login_user(user, password, &errmsg);
      if (ret < 0)
        {
	  json_object_object_add(jreply, "success", json_object_new_boolean(false));
	  errors = json_object_new_object();
	  if (errmsg)
	    json_object_object_add(errors, "error", json_object_new_string(errmsg));
	  else
	    json_object_object_add(errors, "error", json_object_new_string("Unknown error"));
	  json_object_object_add(jreply, "errors", errors);
	}
      else
        {
	  json_object_object_add(jreply, "success", json_object_new_boolean(true));
	}
      free(errmsg);
    }
  else
    {
      DPRINTF(E_LOG, L_WEB, "No user or password in LastFM login post request\n");

      json_object_object_add(jreply, "success", json_object_new_boolean(false));
      errors = json_object_new_object();
      if (!user || strlen(user) == 0)
	json_object_object_add(errors, "user", json_object_new_string("Username is required"));
      if (!password || strlen(password) == 0)
	json_object_object_add(errors, "password", json_object_new_string("Password is required"));
      json_object_object_add(jreply, "errors", errors);
    }

  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

  jparse_free(jreply);

#else
  DPRINTF(E_LOG, L_WEB, "Received LastFM login request but was not compiled with enable-lastfm\n");
#endif

  return HTTP_OK;
}

static int
jsonapi_reply_lastfm_logout(struct httpd_request *hreq)
{
#ifdef LASTFM
  lastfm_logout();
#endif
  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_listenbrainz(struct httpd_request *hreq)
{
  struct listenbrainz_status status;
  json_object *jreply;

  listenbrainz_status_get(&status);

  CHECK_NULL(L_WEB, jreply = json_object_new_object());

  json_object_object_add(jreply, "enabled", json_object_new_boolean(!status.disabled));
  json_object_object_add(jreply, "token_valid", json_object_new_boolean(status.token_valid));
  if (status.user_name)
    json_object_object_add(jreply, "user_name", json_object_new_string(status.user_name));
  if (status.message)
    json_object_object_add(jreply, "message", json_object_new_string(status.message));


  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

  jparse_free(jreply);
  listenbrainz_status_free(&status, true);

  return HTTP_OK;
}

static int
jsonapi_reply_listenbrainz_token_add(struct httpd_request *hreq)
{
  json_object *request;
  const char *token;
  int ret;

  request = jparse_obj_from_evbuffer(hreq->in_body);
  if (!request)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to parse incoming request\n");
      return HTTP_BADREQUEST;
    }

  token = jparse_str_from_obj(request, "token");

  ret = listenbrainz_token_set(token);

  jparse_free(request);

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to set ListenBrainz token\n");
      return HTTP_INTERNAL;
    }

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_listenbrainz_token_delete(struct httpd_request *hreq)
{
  int ret;
  
  ret = listenbrainz_token_delete();

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to delete ListenBrainz token\n");
      return HTTP_INTERNAL;
    }

  return HTTP_NOCONTENT;
}

/*
 * Kicks off pairing of a daap/dacp client
 *
 * Expects the paring pin to be present in the post request body, e. g.:
 *
 * {
 *   "pin": "1234"
 * }
 */
static int
jsonapi_reply_pairing_pair(struct httpd_request *hreq)
{
  json_object* request;
  const char* pin;
  int ret;

  request = jparse_obj_from_evbuffer(hreq->in_body);
  if (!request)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to parse incoming request\n");
      return HTTP_BADREQUEST;
    }

  DPRINTF(E_DBG, L_WEB, "Received pairing post request: %s\n", json_object_to_json_string(request));

  pin = jparse_str_from_obj(request, "pin");
  if (pin)
    {
      ret = remote_pairing_pair(pin);
    }
  else
    {
      DPRINTF(E_LOG, L_WEB, "Missing pin in request body: %s\n", json_object_to_json_string(request));
      ret = REMOTE_INVALID_PIN;
    }

  jparse_free(request);

  if (ret == 0)
    return HTTP_NOCONTENT;
  else if (ret == REMOTE_INVALID_PIN)
    return HTTP_BADREQUEST;

  return HTTP_INTERNAL;
}

/*
 * Retrieves pairing information
 *
 * Example response:
 *
 * {
 *  "active": true,
 *  "remote": "remote name"
 * }
 */
static int
jsonapi_reply_pairing_get(struct httpd_request *hreq)
{
  char *remote_name;
  json_object *jreply;

  remote_name = remote_pairing_get_name();

  CHECK_NULL(L_WEB, jreply = json_object_new_object());

  if (remote_name)
    {
      json_object_object_add(jreply, "active", json_object_new_boolean(true));
      json_object_object_add(jreply, "remote", json_object_new_string(remote_name));
    }
  else
    {
      json_object_object_add(jreply, "active", json_object_new_boolean(false));
    }

  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

  jparse_free(jreply);
  free(remote_name);

  return HTTP_OK;
}

struct outputs_param
{
  json_object *output;
  uint64_t output_id;
  int output_volume;
};

static json_object *
speaker_to_json(struct player_speaker_info *spk)
{
  json_object *output;
  json_object *supported_formats;
  char output_id[21];
  enum media_format format;

  output = json_object_new_object();

  supported_formats = json_object_new_array();
  for (format = MEDIA_FORMAT_FIRST; format <= MEDIA_FORMAT_LAST; format = MEDIA_FORMAT_NEXT(format))
    {
      if (format & spk->supported_formats)
	json_object_array_add(supported_formats, json_object_new_string(media_format_to_string(format)));
    }

  snprintf(output_id, sizeof(output_id), "%" PRIu64, spk->id);
  json_object_object_add(output, "id", json_object_new_string(output_id));
  json_object_object_add(output, "name", json_object_new_string(spk->name));
  json_object_object_add(output, "type", json_object_new_string(spk->output_type));
  json_object_object_add(output, "selected", json_object_new_boolean(spk->selected));
  json_object_object_add(output, "has_password", json_object_new_boolean(spk->has_password));
  json_object_object_add(output, "requires_auth", json_object_new_boolean(spk->requires_auth));
  json_object_object_add(output, "needs_auth_key", json_object_new_boolean(spk->needs_auth_key));
  json_object_object_add(output, "volume", json_object_new_int(spk->absvol));
  json_object_object_add(output, "format", json_object_new_string(media_format_to_string(spk->format)));
  json_object_object_add(output, "supported_formats", supported_formats);

  return output;
}

static void
speaker_enum_cb(struct player_speaker_info *spk, void *arg)
{
  json_object *outputs;
  json_object *output;

  outputs = arg;

  output = speaker_to_json(spk);
  json_object_array_add(outputs, output);
}

/*
 * GET /api/outputs/[output_id]
 */
static int
jsonapi_reply_outputs_get_byid(struct httpd_request *hreq)
{
  struct player_speaker_info speaker_info;
  uint64_t output_id;
  json_object *jreply;
  int ret;

  ret = safe_atou64(hreq->path_parts[2], &output_id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "No valid output id given to outputs endpoint '%s'\n", hreq->path);

      return HTTP_BADREQUEST;
    }

  ret = player_speaker_get_byid(&speaker_info, output_id);

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "No output found for '%s'\n", hreq->path);

      return HTTP_BADREQUEST;
    }

  jreply = speaker_to_json(&speaker_info);
  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

  jparse_free(jreply);

  return HTTP_OK;
}

/*
 * PUT /api/outputs/[output_id]
 */
static int
jsonapi_reply_outputs_put_byid(struct httpd_request *hreq)
{
  uint64_t output_id;
  json_object* request;
  bool selected;
  int volume;
  const char *pin;
  const char *format;
  int ret;

  ret = safe_atou64(hreq->path_parts[2], &output_id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "No valid output id given to outputs endpoint '%s'\n", hreq->path);

      return HTTP_BADREQUEST;
    }

  request = jparse_obj_from_evbuffer(hreq->in_body);
  if (!request)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to parse incoming request\n");

      return HTTP_BADREQUEST;
    }

  ret = 0;

  if (jparse_contains_key(request, "selected", json_type_boolean))
    {
      selected = jparse_bool_from_obj(request, "selected");
      if (selected)
	ret = player_speaker_enable(output_id);
      else
	ret = player_speaker_disable(output_id);
    }

  if (ret == 0 && jparse_contains_key(request, "volume", json_type_int))
    {
      volume = jparse_int_from_obj(request, "volume");
      ret = player_volume_setabs_speaker(output_id, volume);
    }

  if (ret == 0 && jparse_contains_key(request, "pin", json_type_string))
    {
      pin = jparse_str_from_obj(request, "pin");
      if (pin)
	ret = player_speaker_authorize(output_id, pin);
    }

  if (ret == 0 && jparse_contains_key(request, "format", json_type_string))
    {
      format = jparse_str_from_obj(request, "format");
      if (format)
	ret = player_speaker_format_set(output_id, media_format_from_string(format));
    }

  jparse_free(request);

  if (ret < 0)
    return HTTP_BADREQUEST;

  return HTTP_NOCONTENT;
}

/*
 * PUT /api/outputs/[output_id]/toggle
 */
static int
jsonapi_reply_outputs_toggle_byid(struct httpd_request *hreq)
{
  uint64_t output_id;
  struct player_speaker_info spk;
  int ret;

  ret = safe_atou64(hreq->path_parts[2], &output_id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "No valid output id given to outputs endpoint '%s'\n", hreq->path);

      return HTTP_BADREQUEST;
    }

  ret = player_speaker_get_byid(&spk, output_id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "No output found for the given output id, toggle failed for '%s'\n", hreq->path);
      return HTTP_BADREQUEST;
    }

  if (spk.selected)
    ret = player_speaker_disable(output_id);
  else
    ret = player_speaker_enable(output_id);

  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_NOCONTENT;
}

/*
 * Endpoint "/api/outputs"
 */
static int
jsonapi_reply_outputs(struct httpd_request *hreq)
{
  json_object *outputs;
  json_object *jreply;

  outputs = json_object_new_array();

  player_speaker_enumerate(speaker_enum_cb, outputs);

  jreply = json_object_new_object();
  json_object_object_add(jreply, "outputs", outputs);

  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

  jparse_free(jreply);

  return HTTP_OK;
}

static int
jsonapi_reply_verification(struct httpd_request *hreq)
{
  json_object* request;
  const char* message;

  request = jparse_obj_from_evbuffer(hreq->in_body);
  if (!request)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to parse incoming request\n");
      return HTTP_BADREQUEST;
    }

  DPRINTF(E_DBG, L_WEB, "Received verification post request: %s\n", json_object_to_json_string(request));

  message = jparse_str_from_obj(request, "pin");
  if (message)
    player_raop_verification_kickoff((char **)&message);
  else
    DPRINTF(E_LOG, L_WEB, "Missing pin in request body: %s\n", json_object_to_json_string(request));

  jparse_free(request);

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_outputs_set(struct httpd_request *hreq)
{
  json_object *request;
  json_object *outputs;
  json_object *output_id;
  int nspk, i, ret;
  uint64_t *ids;

  request = jparse_obj_from_evbuffer(hreq->in_body);
  if (!request)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to parse incoming request\n");
      return HTTP_BADREQUEST;
    }

  DPRINTF(E_DBG, L_WEB, "Received select-outputs post request: %s\n", json_object_to_json_string(request));

  ret = jparse_array_from_obj(request, "outputs", &outputs);
  if (ret == 0)
    {
      nspk = json_object_array_length(outputs);

      CHECK_NULL(L_WEB, ids = calloc((nspk + 1), sizeof(uint64_t)));
      ids[0] = nspk;

      ret = 0;
      for (i = 0; i < nspk; i++)
	{
	  output_id = json_object_array_get_idx(outputs, i);
	  ret = safe_atou64(json_object_get_string(output_id), &ids[i + 1]);
	  if (ret < 0)
	    {
	      DPRINTF(E_LOG, L_WEB, "Failed to convert output id: %s\n", json_object_to_json_string(request));
	      break;
	    }
	}

      if (ret == 0)
	player_speaker_set(ids);

      free(ids);
    }
  else
    DPRINTF(E_LOG, L_WEB, "Missing outputs in request body: %s\n", json_object_to_json_string(request));

  jparse_free(request);

  return HTTP_NOCONTENT;
}

static int
play_item_with_id(const char *param)
{
  uint32_t item_id;
  struct db_queue_item *queue_item;
  int ret;

  ret = safe_atou32(param, &item_id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "No valid item id given '%s'\n", param);

      return HTTP_BADREQUEST;
    }

  queue_item = db_queue_fetch_byitemid(item_id);
  if (!queue_item)
    {
      DPRINTF(E_LOG, L_WEB, "No queue item with item id '%d'\n", item_id);

      return HTTP_BADREQUEST;
    }

  player_playback_stop();
  ret = player_playback_start_byitem(queue_item);
  free_queue_item(queue_item, 0);

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to start playback from item with id '%d'\n", item_id);

      return HTTP_INTERNAL;
    }

  return HTTP_NOCONTENT;
}

static int
play_item_at_position(const char *param)
{
  uint32_t position;
  struct player_status status;
  struct db_queue_item *queue_item;
  int ret;

  ret = safe_atou32(param, &position);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "No valid position given '%s'\n", param);

      return HTTP_BADREQUEST;
    }

  player_get_status(&status);

  queue_item = db_queue_fetch_bypos(position, status.shuffle);
  if (!queue_item)
    {
      DPRINTF(E_LOG, L_WEB, "No queue item at position '%d'\n", position);

      return HTTP_BADREQUEST;
    }

  player_playback_stop();
  ret = player_playback_start_byitem(queue_item);
  free_queue_item(queue_item, 0);

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to start playback from position '%d'\n", position);

      return HTTP_INTERNAL;
    }

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_player_play(struct httpd_request *hreq)
{
  const char *param;
  int ret;

  if ((param = httpd_query_value_find(hreq->query, "item_id")))
    {
      return play_item_with_id(param);
    }
  else if ((param = httpd_query_value_find(hreq->query, "position")))
    {
      return play_item_at_position(param);
    }

  ret = player_playback_start();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error starting playback.\n");
      return HTTP_INTERNAL;
    }

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_player_pause(struct httpd_request *hreq)
{
  int ret;

  ret = player_playback_pause();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error pausing playback.\n");
      return HTTP_INTERNAL;
    }

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_player_stop(struct httpd_request *hreq)
{
  int ret;

  ret = player_playback_stop();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error stopping playback.\n");
      return HTTP_INTERNAL;
    }

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_player_toggle(struct httpd_request *hreq)
{
  struct player_status status;
  int ret;

  player_get_status(&status);
  DPRINTF(E_DBG, L_WEB, "Toggle playback request with current state %d.\n", status.status);

  if (status.status == PLAY_PLAYING)
    {
      ret = player_playback_pause();
    }
  else
    {
      ret = player_playback_start();
    }

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error toggling playback state.\n");
      return HTTP_INTERNAL;
    }

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_player_next(struct httpd_request *hreq)
{
  int ret;

  ret = player_playback_next();
  if (ret < 0)
    {
      // If skipping to the next song failed, it is most likely we reached the end of the queue,
      // ignore the error (play status change will be reported to the client over the websocket)
      DPRINTF(E_DBG, L_WEB, "Error switching to next item (possibly end of queue reached).\n");
      return HTTP_NOCONTENT;
    }

  ret = player_playback_start();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error starting playback after switching to next item.\n");
      return HTTP_INTERNAL;
    }

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_player_previous(struct httpd_request *hreq)
{
  int ret;

  ret = player_playback_prev();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error switching to previous item.\n");
      return HTTP_INTERNAL;
    }

  ret = player_playback_start();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error starting playback after switching to previous item.\n");
      return HTTP_INTERNAL;
    }

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_player_seek(struct httpd_request *hreq)
{
  const char *param_pos;
  const char *param_seek;
  int position_ms;
  int seek_ms;
  int ret;

  param_pos = httpd_query_value_find(hreq->query, "position_ms");
  param_seek = httpd_query_value_find(hreq->query, "seek_ms");
  if (!param_pos && !param_seek)
    return HTTP_BADREQUEST;

  if (param_pos)
    {
      ret = safe_atoi32(param_pos, &position_ms);
      if (ret < 0)
	return HTTP_BADREQUEST;

      ret = player_playback_seek(position_ms, PLAYER_SEEK_POSITION);
    }
  else
    {
      ret = safe_atoi32(param_seek, &seek_ms);
      if (ret < 0)
	return HTTP_BADREQUEST;

      ret = player_playback_seek(seek_ms, PLAYER_SEEK_RELATIVE);
    }

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error seeking (position_ms=%s, seek_ms=%s).\n",
	      (param_pos ? param_pos : ""), (param_seek ? param_seek : ""));
      return HTTP_INTERNAL;
    }

  ret = player_playback_start();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error starting playback after seeking (position_ms=%s, seek_ms=%s).\n",
	      (param_pos ? param_pos : ""), (param_seek ? param_seek : ""));
      return HTTP_INTERNAL;
    }

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_player(struct httpd_request *hreq)
{
  struct player_status status;
  struct db_queue_item *queue_item;
  json_object *reply;

  player_get_status(&status);

  reply = json_object_new_object();

  switch (status.status)
    {
      case PLAY_PAUSED:
	json_object_object_add(reply, "state", json_object_new_string("pause"));
	break;

      case PLAY_PLAYING:
	json_object_object_add(reply, "state", json_object_new_string("play"));
	break;

      default:
	json_object_object_add(reply, "state", json_object_new_string("stop"));
	break;
    }

  switch (status.repeat)
    {
      case REPEAT_SONG:
	json_object_object_add(reply, "repeat", json_object_new_string("single"));
	break;

      case REPEAT_ALL:
	json_object_object_add(reply, "repeat", json_object_new_string("all"));
	break;

      default:
	json_object_object_add(reply, "repeat", json_object_new_string("off"));
	break;
    }

  json_object_object_add(reply, "consume", json_object_new_boolean(status.consume));
  json_object_object_add(reply, "shuffle", json_object_new_boolean(status.shuffle));
  json_object_object_add(reply, "volume", json_object_new_int(status.volume));

  if (status.item_id)
    {
      json_object_object_add(reply, "item_id", json_object_new_int(status.item_id));
      json_object_object_add(reply, "item_length_ms", json_object_new_int(status.len_ms));
      json_object_object_add(reply, "item_progress_ms", json_object_new_int(status.pos_ms));
      json_object_object_add(reply, "artwork_url", json_object_new_string("./artwork/nowplaying"));
    }
  else
    {
      queue_item = db_queue_fetch_bypos(0, status.shuffle);

      if (queue_item)
	{
	  json_object_object_add(reply, "item_id", json_object_new_int(queue_item->id));
	  json_object_object_add(reply, "item_length_ms", json_object_new_int(queue_item->song_length));
	  json_object_object_add(reply, "item_progress_ms", json_object_new_int(0));
	  free_queue_item(queue_item, 0);
	}
      else
	{
	  json_object_object_add(reply, "item_id", json_object_new_int(0));
	  json_object_object_add(reply, "item_length_ms", json_object_new_int(0));
	  json_object_object_add(reply, "item_progress_ms", json_object_new_int(0));
	}
    }

  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply)));

  jparse_free(reply);

  return HTTP_OK;
}

static json_object *
queue_item_to_json(struct db_queue_item *queue_item, char shuffle)
{
  json_object *item;
  char uri[100];
  char artwork_url[100];
  int ret;

  item = json_object_new_object();

  json_object_object_add(item, "id", json_object_new_int(queue_item->id));
  if (shuffle)
    json_object_object_add(item, "position", json_object_new_int(queue_item->shuffle_pos));
  else
    json_object_object_add(item, "position", json_object_new_int(queue_item->pos));

  if (queue_item->file_id > 0 && queue_item->file_id != DB_MEDIA_FILE_NON_PERSISTENT_ID)
    json_object_object_add(item, "track_id", json_object_new_int(queue_item->file_id));

  safe_json_add_string(item, "title", queue_item->title);
  safe_json_add_string(item, "artist", queue_item->artist);
  safe_json_add_string(item, "artist_sort", queue_item->artist_sort);
  safe_json_add_string(item, "album", queue_item->album);
  safe_json_add_string(item, "album_sort", queue_item->album_sort);
  safe_json_add_string_from_int64(item, "album_id", queue_item->songalbumid);
  safe_json_add_string(item, "album_artist", queue_item->album_artist);
  safe_json_add_string(item, "album_artist_sort", queue_item->album_artist_sort);
  safe_json_add_string_from_int64(item, "album_artist_id", queue_item->songartistid);
  safe_json_add_string(item, "composer", queue_item->composer);
  safe_json_add_string(item, "genre", queue_item->genre);

  json_object_object_add(item, "year", json_object_new_int(queue_item->year));
  json_object_object_add(item, "track_number", json_object_new_int(queue_item->track));
  json_object_object_add(item, "disc_number", json_object_new_int(queue_item->disc));
  json_object_object_add(item, "length_ms", json_object_new_int(queue_item->song_length));

  safe_json_add_string(item, "media_kind", db_media_kind_label(queue_item->media_kind));
  safe_json_add_string(item, "data_kind", db_data_kind_label(queue_item->data_kind));

  safe_json_add_string(item, "path", queue_item->path);

  if (queue_item->file_id > 0 && queue_item->file_id != DB_MEDIA_FILE_NON_PERSISTENT_ID)
    {
      ret = snprintf(uri, sizeof(uri), "%s:%s:%d", "library", "track", queue_item->file_id);
      if (ret < sizeof(uri))
	json_object_object_add(item, "uri", json_object_new_string(uri));
    }
  else
    {
      safe_json_add_string(item, "uri", queue_item->path);
    }

  if (queue_item->artwork_url && net_is_http_or_https(queue_item->artwork_url))
    {
      // The queue item contains a valid http url for an artwork image, there is no need
      // for the client to request the image through the server artwork handler.
      // Directly pass the artwork url to the client.
      safe_json_add_string(item, "artwork_url", queue_item->artwork_url);
    }
  else if (queue_item->file_id > 0 && queue_item->file_id != DB_MEDIA_FILE_NON_PERSISTENT_ID)
    {
      if (queue_item->data_kind == DATA_KIND_FILE)
	{
	  // Queue item does not have a valid artwork url, construct artwork url to
	  // get the image through the httpd_artworkapi (uses the artwork handlers).
	  ret = snprintf(artwork_url, sizeof(artwork_url), "./artwork/item/%d", queue_item->file_id);
	  if (ret < sizeof(artwork_url))
	    json_object_object_add(item, "artwork_url", json_object_new_string(artwork_url));
	}
      else
	{
	  // Pipe and stream metadata can change if the queue version changes. Construct artwork url
	  // similar to non-pipe items, but append the queue version to the url to force
	  // clients to reload image if the queue version changes (additional metadata was found).
	  ret = snprintf(artwork_url, sizeof(artwork_url), "./artwork/item/%d?v=%d", queue_item->file_id, queue_item->queue_version);
	  if (ret < sizeof(artwork_url))
	    json_object_object_add(item, "artwork_url", json_object_new_string(artwork_url));
	}
    }

  safe_json_add_string(item, "type", queue_item->type);
  json_object_object_add(item, "bitrate", json_object_new_int(queue_item->bitrate));
  json_object_object_add(item, "samplerate", json_object_new_int(queue_item->samplerate));
  json_object_object_add(item, "channels", json_object_new_int(queue_item->channels));

  return item;
}

static int
queue_tracks_add_byuris(const char *param, char shuffle, uint32_t item_id, int pos, int *total_count, int *new_item_id)
{
  char *uris;
  const char *uri;
  char *ptr;
  int count;
  int new;
  int ret;

  *total_count = 0;
  *new_item_id = -1;

  CHECK_NULL(L_WEB, uris = strdup(param));

  uri = strtok_r(uris, ",", &ptr);
  if (!uri)
    {
      DPRINTF(E_LOG, L_WEB, "Empty query parameter 'uris'\n");
      goto error;
    }

  for (; uri; uri = strtok_r(NULL, ",", &ptr))
    {
      ret = library_queue_item_add(uri, pos, shuffle, item_id, &count, &new);
      if (ret != LIBRARY_OK)
	{
	  DPRINTF(E_LOG, L_WEB, "Invalid uri '%s'\n", uri);
	  goto error;
	}

      *total_count += count;
      if (pos >= 0)
	pos += count;
      if (*new_item_id == -1)
        *new_item_id = new;
    }

  free(uris);
  return 0;

 error:
  free(uris);
  return -1;
}

static int
queue_tracks_add_byexpression(const char *param, char shuffle, uint32_t item_id, int pos, int limit, int *total_count, int *new_item_id)
{
  struct query_params query_params = { .type = Q_ITEMS, .sort = S_NAME };
  struct smartpl smartpl_expression = { 0 };
  char *expression;
  int ret;

  expression = safe_asprintf("\"query\" { %s }", param);
  ret = smartpl_query_parse_string(&smartpl_expression, expression);
  free(expression);

  if (ret < 0)
    return -1;

  query_params.filter = strdup(smartpl_expression.query_where);
  query_params.order = safe_strdup(smartpl_expression.order);
  query_params.limit = limit > 0 ? limit : smartpl_expression.limit;
  query_params.idx_type = query_params.limit > 0 ? I_FIRST : I_NONE;
  free_smartpl(&smartpl_expression, 1);

  ret = db_queue_add_by_query(&query_params, shuffle, item_id, pos, total_count, new_item_id);

  free_query_params(&query_params, 1);
  return ret;
}

static int
create_reply_queue_tracks_add(struct evbuffer *evbuf, int count, int new_item_id, char shuffle)
{
  json_object *reply = json_object_new_object();
  json_object *items = json_object_new_array();
  json_object *item;
  struct query_params query_params = { 0 };
  struct db_queue_item queue_item;
  int version = 0;
  int ret;

  db_admin_getint(&version, DB_ADMIN_QUEUE_VERSION);

  json_object_object_add(reply, "version", json_object_new_int(version));
  json_object_object_add(reply, "count", json_object_new_int(count));
  json_object_object_add(reply, "items", items);

  ret = db_queue_enum_start(&query_params);
  if (ret < 0)
    goto error;

  while ((ret = db_queue_enum_fetch(&query_params, &queue_item)) == 0 && queue_item.id > 0)
    {
      if (queue_item.id < new_item_id)
	continue;

      item = queue_item_to_json(&queue_item, shuffle);
      if (!item)
	goto error;

      json_object_array_add(items, item);
    }

  ret = evbuffer_add_printf(evbuf, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    goto error;

  db_queue_enum_end(&query_params);
  jparse_free(reply);
  return 0;

 error:
  db_queue_enum_end(&query_params);
  jparse_free(reply);
  return -1;
}

static int
jsonapi_reply_queue_tracks_add(struct httpd_request *hreq)
{
  const char *param_pos;
  const char *param_uris;
  const char *param_expression;
  const char *param;
  struct player_status status;
  int pos;
  int limit;
  bool shuffle;
  int total_count = 0;
  int new_item_id = 0;
  int ret = 0;


  param_pos = httpd_query_value_find(hreq->query, "position");
  if (param_pos)
    {
      if (safe_atoi32(param_pos, &pos) < 0)
        {
	  DPRINTF(E_LOG, L_WEB, "Invalid position parameter '%s'\n", param_pos);

	  return HTTP_BADREQUEST;
	}

      DPRINTF(E_DBG, L_WEB, "Add tracks starting at position %d\n", pos);
    }
  else
    pos = -1;

  param_uris = httpd_query_value_find(hreq->query, "uris");
  param_expression = httpd_query_value_find(hreq->query, "expression");

  if (!param_uris && !param_expression)
    {
      DPRINTF(E_LOG, L_WEB, "Missing query parameter 'uris' or 'expression'\n");

      return HTTP_BADREQUEST;
    }

  // if query parameter "clear" is "true", stop playback and clear the queue before adding new queue items
  param = httpd_query_value_find(hreq->query, "clear");
  if (param && strcmp(param, "true") == 0)
    {
      player_playback_stop();
      db_queue_clear(0);
    }

  // if query parameter "shuffle" is present, update the shuffle state before adding new queue items
  param = httpd_query_value_find(hreq->query, "shuffle");
  if (param)
    {
      shuffle = (strcmp(param, "true") == 0);
      player_shuffle_set(shuffle);
    }

  player_get_status(&status);

  if (param_uris)
    {
      ret = queue_tracks_add_byuris(param_uris, status.shuffle, status.item_id, pos, &total_count, &new_item_id);
    }
  else
    {
      // This overrides the value specified in query
      param = httpd_query_value_find(hreq->query, "limit");
      if (param && safe_atoi32(param, &limit) == 0)
	ret = queue_tracks_add_byexpression(param_expression, status.shuffle, status.item_id, pos, limit, &total_count, &new_item_id);
      else
	ret = queue_tracks_add_byexpression(param_expression, status.shuffle, status.item_id, pos, -1, &total_count, &new_item_id);
    }
  if (ret < 0)
    return HTTP_INTERNAL;

  ret = create_reply_queue_tracks_add(hreq->out_body, total_count, new_item_id, status.shuffle);
  if (ret < 0)
    return HTTP_INTERNAL;

  // If query parameter "playback" is "start", start playback after successfully adding new items
  param = httpd_query_value_find(hreq->query, "playback");
  if (param && strcmp(param, "start") == 0)
    {
      if ((param = httpd_query_value_find(hreq->query, "playback_from_position")))
	ret = (play_item_at_position(param) == HTTP_NOCONTENT) ? 0 : -1;
      else
	ret = player_playback_start();

      if (ret < 0)
	return HTTP_INTERNAL;
    }

  return HTTP_OK;
}

static int
update_pos(uint32_t item_id, const char *new, char shuffle)
{
  uint32_t new_position;
  int ret;

  if (safe_atou32(new, &new_position) < 0)
    {
      DPRINTF(E_LOG, L_WEB, "No valid item new_position '%s'\n", new);
      return HTTP_BADREQUEST;
    }

  ret = db_queue_move_byitemid(item_id, new_position, shuffle);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Moving item '%d' to new position %d failed\n", item_id, new_position);
      return HTTP_INTERNAL;
    }

  return HTTP_OK;
}

static inline void
update_str(bool *is_changed, char **str, const char *new)
{
  free(*str);
  *str = strdup(new);
  *is_changed = true;
}

static int
jsonapi_reply_queue_tracks_update(struct httpd_request *hreq)
{
  struct db_queue_item *queue_item;
  struct player_status status;
  uint32_t item_id = 0;
  const char *param;
  bool is_changed;
  int ret;

  player_get_status(&status);

  if (strcmp(hreq->path_parts[3], "now_playing") != 0)
    {
      ret = safe_atou32(hreq->path_parts[3], &item_id);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_WEB, "No valid item id given: '%s'\n", hreq->path);
	  return HTTP_BADREQUEST;
	}
    }
  else
    item_id = status.item_id;

  queue_item = db_queue_fetch_byitemid(item_id);
  if (!queue_item)
    {
      DPRINTF(E_LOG, L_WEB, "No valid item id given, or now_playing given but not playing: '%s'\n", hreq->path);
      return HTTP_BADREQUEST;
    }

  ret = HTTP_OK;
  is_changed = false;
  if ((param = httpd_query_value_find(hreq->query, "new_position")))
    ret = update_pos(item_id, param, status.shuffle);
  if ((param = httpd_query_value_find(hreq->query, "title")))
    update_str(&is_changed, &queue_item->title, param);
  if ((param = httpd_query_value_find(hreq->query, "album")))
    update_str(&is_changed, &queue_item->album, param);
  if ((param = httpd_query_value_find(hreq->query, "artist")))
    update_str(&is_changed, &queue_item->artist, param);
  if ((param = httpd_query_value_find(hreq->query, "album_artist")))
    update_str(&is_changed, &queue_item->album_artist, param);
  if ((param = httpd_query_value_find(hreq->query, "composer")))
    update_str(&is_changed, &queue_item->composer, param);
  if ((param = httpd_query_value_find(hreq->query, "genre")))
    update_str(&is_changed, &queue_item->genre, param);
  if ((param = httpd_query_value_find(hreq->query, "artwork_url")))
    update_str(&is_changed, &queue_item->artwork_url, param);

  if (ret != HTTP_OK)
    return ret;

  if (is_changed)
    db_queue_item_update(queue_item);

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_queue_tracks_delete(struct httpd_request *hreq)
{
  uint32_t item_id;
  uint32_t count;
  int ret;

  ret = safe_atou32(hreq->path_parts[3], &item_id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "No valid item id given '%s'\n", hreq->path);

      return HTTP_BADREQUEST;
    }

  ret = db_queue_delete_byitemid(item_id);
  if (ret < 0)
    {
      return HTTP_INTERNAL;
    }

  db_queue_get_count(&count);
  if (count == 0)
    {
      player_playback_stop();
      db_queue_clear(0);
    }

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_queue_clear(struct httpd_request *hreq)
{
  player_playback_stop();
  db_queue_clear(0);

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_queue(struct httpd_request *hreq)
{
  struct query_params query_params;
  const char *param;
  uint32_t item_id;
  uint32_t count;
  int start_pos, end_pos;
  int version = 0;
  char etag[21];
  struct player_status status;
  struct db_queue_item queue_item;
  json_object *reply;
  json_object *items;
  json_object *item;
  int ret = 0;

  db_admin_getint(&version, DB_ADMIN_QUEUE_VERSION);
  db_queue_get_count(&count);

  snprintf(etag, sizeof(etag), "%d", version);
  if (httpd_request_etag_matches(hreq, etag))
    return HTTP_NOTMODIFIED;

  memset(&query_params, 0, sizeof(struct query_params));
  reply = json_object_new_object();

  json_object_object_add(reply, "version", json_object_new_int(version));
  json_object_object_add(reply, "count", json_object_new_int((int)count));

  items = json_object_new_array();
  json_object_object_add(reply, "items", items);

  player_get_status(&status);
  if (status.shuffle)
    query_params.sort = S_SHUFFLE_POS;

  param = httpd_query_value_find(hreq->query, "id");
  if (param && strcmp(param, "now_playing") == 0)
    {
      query_params.filter = db_mprintf("id = %d", status.item_id);
    }
  else if (param && safe_atou32(param, &item_id) == 0)
    {
      query_params.filter = db_mprintf("id = %d", item_id);
    }
  else
    {
      param = httpd_query_value_find(hreq->query, "start");
      if (param && safe_atoi32(param, &start_pos) == 0)
	{
	  param = httpd_query_value_find(hreq->query, "end");
	  if (!param || safe_atoi32(param, &end_pos) != 0)
	    {
	      end_pos = start_pos + 1;
	    }

	  if (query_params.sort == S_SHUFFLE_POS)
	    query_params.filter = db_mprintf("shuffle_pos >= %d AND shuffle_pos < %d", start_pos, end_pos);
	  else
	    query_params.filter = db_mprintf("pos >= %d AND pos < %d", start_pos, end_pos);
	}
    }

  ret = db_queue_enum_start(&query_params);
  if (ret < 0)
    goto db_start_error;

  while ((ret = db_queue_enum_fetch(&query_params, &queue_item)) == 0 && queue_item.id > 0)
    {
      item = queue_item_to_json(&queue_item, status.shuffle);
      if (!item)
	goto error;

      json_object_array_add(items, item);
    }

  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "outputs: Couldn't add outputs to response buffer.\n");

 error:
  db_queue_enum_end(&query_params);
 db_start_error:
  jparse_free(reply);
  free(query_params.filter);

  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_player_repeat(struct httpd_request *hreq)
{
  const char *param;

  param = httpd_query_value_find(hreq->query, "state");
  if (!param)
    return HTTP_BADREQUEST;

  if (strcmp(param, "single") == 0)
    {
      player_repeat_set(REPEAT_SONG);
    }
  else if (strcmp(param, "all") == 0)
    {
      player_repeat_set(REPEAT_ALL);
    }
  else if (strcmp(param, "off") == 0)
    {
      player_repeat_set(REPEAT_OFF);
    }

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_player_shuffle(struct httpd_request *hreq)
{
  const char *param;
  bool shuffle;

  param = httpd_query_value_find(hreq->query, "state");
  if (!param)
    return HTTP_BADREQUEST;

  shuffle = (strcmp(param, "true") == 0);
  player_shuffle_set(shuffle);

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_player_consume(struct httpd_request *hreq)
{
  const char *param;
  bool consume;

  param = httpd_query_value_find(hreq->query, "state");
  if (!param)
    return HTTP_BADREQUEST;

  consume = (strcmp(param, "true") == 0);
  player_consume_set(consume);

  return HTTP_NOCONTENT;
}

static int
volume_set(int volume, int step)
{
  int new_volume;
  struct player_status status;
  int ret;

  new_volume = volume;

  if (step != 0)
    {
      // Calculate new volume from given step value
      player_get_status(&status);
      new_volume = status.volume + step;
    }

  // Make sure we are setting a correct value
  new_volume = new_volume > 100 ? 100 : new_volume;
  new_volume = new_volume < 0 ? 0 : new_volume;

  ret = player_volume_set(new_volume);
  return ret;
}

static int
output_volume_set(int volume, int step, uint64_t output_id)
{
  int new_volume;
  struct player_speaker_info speaker_info;
  int ret;

  new_volume = volume;

  if (step != 0)
    {
      // Calculate new output volume from the given step value
      ret = player_speaker_get_byid(&speaker_info, output_id);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_WEB, "No output found for the given output id .\n");
	  return -1;
	}

      new_volume = speaker_info.absvol + step;
    }

  // Make sure we are setting a correct value
  new_volume = new_volume > 100 ? 100 : new_volume;
  new_volume = new_volume < 0 ? 0 : new_volume;

  ret = player_volume_setabs_speaker(output_id, new_volume);
  return ret;
}

static int
jsonapi_reply_player_volume(struct httpd_request *hreq)
{
  const char *param_volume;
  const char *param_step;
  const char *param;
  uint64_t output_id;
  int volume;
  int step;
  int ret;

  volume = 0;
  step = 0;

  // Parse and validate parameters
  param_volume = httpd_query_value_find(hreq->query, "volume");
  if (param_volume)
    {
      ret = safe_atoi32(param_volume, &volume);
      if (ret < 0)
	return HTTP_BADREQUEST;
    }

  param_step = httpd_query_value_find(hreq->query, "step");
  if (param_step)
    {
      ret = safe_atoi32(param_step, &step);
      if (ret < 0)
	return HTTP_BADREQUEST;
    }

  if ((!param_volume && !param_step)
      || (param_volume && param_step))
    {
      DPRINTF(E_LOG, L_WEB, "Invalid parameters for player/volume request. Either 'volume' or 'step' parameter required.\n");
      return HTTP_BADREQUEST;
    }

  param = httpd_query_value_find(hreq->query, "output_id");
  if (param)
    {
      // Update volume for individual output
      ret = safe_atou64(param, &output_id);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_WEB, "Invalid value for parameter 'output_id'. Output id must be an integer (output_id='%s').\n", param);
	  return HTTP_BADREQUEST;
	}
      ret = output_volume_set(volume, step, output_id);
    }
  else
    {
      // Update master volume
      ret = volume_set(volume, step);
    }

  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_library_artists(struct httpd_request *hreq)
{
  struct query_params query_params;
  const char *param;
  enum media_kind media_kind;
  json_object *reply;
  json_object *items;
  int total;
  int ret = 0;

  if (!is_modified(hreq, DB_ADMIN_DB_UPDATE))
    return HTTP_NOTMODIFIED;

  media_kind = 0;
  param = httpd_query_value_find(hreq->query, "media_kind");
  if (param)
    {
      media_kind = db_media_kind_enum(param);
      if (!media_kind)
	{
	  DPRINTF(E_LOG, L_WEB, "Invalid media kind '%s'\n", param);
	  return HTTP_BADREQUEST;
	}
    }

  reply = json_object_new_object();
  items = json_object_new_array();
  json_object_object_add(reply, "items", items);

  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto error;

  query_params.type = Q_GROUP_ARTISTS;
  query_params.sort = S_ARTIST;

  if (media_kind)
    query_params.filter = db_mprintf("(f.media_kind = %d)", media_kind);

  ret = fetch_artists(&query_params, items, &total);
  if (ret < 0)
    goto error;

  json_object_object_add(reply, "total", json_object_new_int(total));
  json_object_object_add(reply, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(reply, "limit", json_object_new_int(query_params.limit));

  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "browse: Couldn't add artists to response buffer.\n");

 error:
  free_query_params(&query_params, 1);
  jparse_free(reply);

  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_library_artist(struct httpd_request *hreq)
{
  const char *artist_id;
  json_object *reply;
  int ret = 0;
  bool notfound = false;

  if (!is_modified(hreq, DB_ADMIN_DB_UPDATE))
    return HTTP_NOTMODIFIED;

  artist_id = hreq->path_parts[3];

  reply = fetch_artist(&notfound, artist_id);
  if (!reply)
    {
      ret = -1;
      goto error;
    }

  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "browse: Couldn't add artists to response buffer.\n");

 error:
  jparse_free(reply);

  if (ret < 0)
    return notfound ? HTTP_NOTFOUND : HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_library_artist_albums(struct httpd_request *hreq)
{
  struct query_params query_params;
  const char *artist_id;
  json_object *reply;
  json_object *items;
  int total;
  int ret = 0;

  if (!is_modified(hreq, DB_ADMIN_DB_UPDATE))
    return HTTP_NOTMODIFIED;

  artist_id = hreq->path_parts[3];

  reply = json_object_new_object();
  items = json_object_new_array();
  json_object_object_add(reply, "items", items);

  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto error;

  query_params.type = Q_GROUP_ALBUMS;
  query_params.sort = S_ALBUM;
  query_params.filter = db_mprintf("(f.songartistid = %q)", artist_id);

  ret = fetch_albums(&query_params, items, &total);
  free(query_params.filter);

  if (ret < 0)
    goto error;

  json_object_object_add(reply, "total", json_object_new_int(total));
  json_object_object_add(reply, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(reply, "limit", json_object_new_int(query_params.limit));

  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "browse: Couldn't add albums to response buffer.\n");

 error:
  jparse_free(reply);

  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_library_albums(struct httpd_request *hreq)
{
  struct query_params query_params;
  const char *param;
  enum media_kind media_kind;
  json_object *reply;
  json_object *items;
  int total;
  int ret = 0;

  if (!is_modified(hreq, DB_ADMIN_DB_UPDATE))
    return HTTP_NOTMODIFIED;

  media_kind = 0;
  param = httpd_query_value_find(hreq->query, "media_kind");
  if (param)
    {
      media_kind = db_media_kind_enum(param);
      if (!media_kind)
	{
	  DPRINTF(E_LOG, L_WEB, "Invalid media kind '%s'\n", param);
	  return HTTP_BADREQUEST;
	}
    }

  reply = json_object_new_object();
  items = json_object_new_array();
  json_object_object_add(reply, "items", items);

  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto error;

  query_params.type = Q_GROUP_ALBUMS;
  query_params.sort = S_ALBUM;

  if (media_kind)
    query_params.filter = db_mprintf("(f.media_kind = %d)", media_kind);

  ret = fetch_albums(&query_params, items, &total);
  if (ret < 0)
    goto error;

  json_object_object_add(reply, "total", json_object_new_int(total));
  json_object_object_add(reply, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(reply, "limit", json_object_new_int(query_params.limit));

  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "browse: Couldn't add albums to response buffer.\n");

 error:
  free_query_params(&query_params, 1);
  jparse_free(reply);

  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_library_album(struct httpd_request *hreq)
{
  const char *album_id;
  json_object *reply;
  int ret = 0;
  bool notfound = false;

  if (!is_modified(hreq, DB_ADMIN_DB_UPDATE))
    return HTTP_NOTMODIFIED;

  album_id = hreq->path_parts[3];

  reply = fetch_album(&notfound, album_id);
  if (!reply)
    {
      ret = -1;
      goto error;
    }

  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "browse: Couldn't add artists to response buffer.\n");

 error:
  jparse_free(reply);

  if (ret < 0)
    return notfound ? HTTP_NOTFOUND : HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_library_album_tracks(struct httpd_request *hreq)
{
  struct query_params query_params;
  const char *album_id;
  json_object *reply;
  json_object *items;
  int total;
  int ret = 0;

  if (!is_modified(hreq, DB_ADMIN_DB_MODIFIED))
    return HTTP_NOTMODIFIED;

  album_id = hreq->path_parts[3];

  reply = json_object_new_object();
  items = json_object_new_array();
  json_object_object_add(reply, "items", items);

  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto error;

  query_params.type = Q_ITEMS;
  query_params.sort = S_ALBUM;
  query_params.filter = db_mprintf("(f.songalbumid = %q)", album_id);

  ret = fetch_tracks(&query_params, items, &total);
  free(query_params.filter);

  if (ret < 0)
    goto error;

  json_object_object_add(reply, "total", json_object_new_int(total));
  json_object_object_add(reply, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(reply, "limit", json_object_new_int(query_params.limit));

  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "browse: Couldn't add tracks to response buffer.\n");

 error:
  jparse_free(reply);

  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_library_album_tracks_put_byid(struct httpd_request *hreq)
{
  const char *param;
  int64_t album_id;;
  int ret;

  ret = safe_atoi64(hreq->path_parts[3], &album_id);
  if (ret < 0)
    return HTTP_INTERNAL;

  param = httpd_query_value_find(hreq->query, "play_count");
  if (!param)
    return HTTP_BADREQUEST;

  if (strcmp(param, "increment") == 0)
    {
      db_file_inc_playcount_bysongalbumid(album_id, false);
    }
  else if (strcmp(param, "played") == 0)
    {
      db_file_inc_playcount_bysongalbumid(album_id, true);
    }
  else
    {
      DPRINTF(E_WARN, L_WEB, "Ignoring invalid play_count param '%s'\n", param);
      return HTTP_BADREQUEST;
    }

  return HTTP_OK;
}

static int
jsonapi_reply_library_tracks_get_byid(struct httpd_request *hreq)
{
  struct query_params query_params;
  const char *track_id;
  struct db_media_file_info dbmfi;
  json_object *reply = NULL;
  int ret = 0;
  bool notfound = false;

  if (!is_modified(hreq, DB_ADMIN_DB_MODIFIED))
    return HTTP_NOTMODIFIED;

  track_id = hreq->path_parts[3];

  memset(&query_params, 0, sizeof(struct query_params));

  query_params.type = Q_ITEMS;
  query_params.filter = db_mprintf("(f.id = %q)", track_id);

  ret = db_query_start(&query_params);
  if (ret < 0)
    goto error;

  ret = db_query_fetch_file(&dbmfi, &query_params);
  if (ret < 0)
    goto error;
  else if (ret == 1)
    {
      DPRINTF(E_LOG, L_WEB, "Track with id '%s' not found.\n", track_id);
      ret = -1;
      notfound = true;
      goto error;
    }

  reply = track_to_json(&dbmfi);

  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "browse: Couldn't add track to response buffer.\n");

 error:
  db_query_end(&query_params);
  free(query_params.filter);
  jparse_free(reply);

  if (ret < 0)
    return notfound ? HTTP_NOTFOUND : HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_library_tracks_put(struct httpd_request *hreq)
{
  json_object *request = NULL;
  json_object *tracks;
  json_object *track = NULL;
  int ret;
  int err;
  int32_t track_id;
  int i;
  int j;

  request = jparse_obj_from_evbuffer(hreq->in_body);
  if (!request)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to read json tracks request\n");
      err = HTTP_BADREQUEST;
      goto error;
    }

  ret = jparse_array_from_obj(request, "tracks", &tracks);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to parse json tracks request\n");
      err = HTTP_BADREQUEST;
      goto error;
    }

  db_transaction_begin();
  i = 0;
  while ((track = json_object_array_get_idx(tracks, i)))
    {
      track_id = jparse_int_from_obj(track, "id");
      if (track_id == 0)
	{
	  DPRINTF(E_LOG, L_WEB, "Invalid or missing track id in json tracks request\n");
	  err = HTTP_BADREQUEST;
	  goto error;
	}

      if (!db_file_id_exists(track_id))
	{
	  DPRINTF(E_LOG, L_WEB, "Unknown track_id %d in json tracks request\n", track_id);
	  err = HTTP_NOTFOUND;
	  goto error;
	}

      for (j = 0; j < ARRAY_SIZE(track_attribs); j++)
	{
	  if (!jparse_contains_key(track, track_attribs[j].name, json_type_int))
	    continue;

	  ret = jparse_int_from_obj(track, track_attribs[j].name);
	  if (ret < 0)
	    continue;

	  // async, so no error check
	  library_item_attrib_save(track_id, track_attribs[j].type, ret);
	}

      i++;
    }

  jparse_free(request);
  db_transaction_end();
  return HTTP_OK;

 error:
  jparse_free(request);
  if (track)
    db_transaction_rollback();
  return err;
}

static int
jsonapi_reply_library_tracks_put_byid(struct httpd_request *hreq)
{
  int track_id;
  const char *param;
  uint32_t val;
  int ret;
  int i;

  ret = safe_atoi32(hreq->path_parts[3], &track_id);
  if (ret < 0 || !db_file_id_exists(track_id))
    {
      DPRINTF(E_WARN, L_WEB, "Invalid or unknown track id in request '%s'\n", hreq->path);
      return HTTP_NOTFOUND;
    }

  for (i = 0; i < ARRAY_SIZE(track_attribs); i++)
    {
      param = httpd_query_value_find(hreq->query, track_attribs[i].name);
      if (!param)
	continue;

      // Special cases
      if (track_attribs[i].type == LIBRARY_ATTRIB_PLAY_COUNT && strcmp(param, "increment") == 0)
	{
	  db_file_inc_playcount(track_id);
	  continue;
	}
      if (track_attribs[i].type == LIBRARY_ATTRIB_PLAY_COUNT && strcmp(param, "reset") == 0)
	{
	  db_file_reset_playskip_count(track_id);
	  continue;
	}

      ret = safe_atou32(param, &val);
      if (ret < 0)
	{
	  DPRINTF(E_WARN, L_WEB, "Invalid %s value '%s' for track '%d'.\n", track_attribs[i].name, param, track_id);
	  return HTTP_BADREQUEST;
	}

      library_item_attrib_save(track_id, track_attribs[i].type, val);
    }

  return HTTP_OK;
}

static int
jsonapi_reply_library_track_playlists(struct httpd_request *hreq)
{
  struct query_params query_params;
  json_object *reply;
  json_object *items;
  char *path;
  const char *track_id;
  int id;
  int total;
  int ret = 0;

  if (!is_modified(hreq, DB_ADMIN_DB_MODIFIED))
    return HTTP_NOTMODIFIED;

  track_id = hreq->path_parts[3];
  if (safe_atoi32(track_id, &id) < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error converting track id '%s' to int.\n", track_id);
      return HTTP_INTERNAL;
    }

  path = db_file_path_byid(id);
  if (!path)
    {
      DPRINTF(E_WARN, L_WEB, "No file path found for track with id '%s' not found.\n", track_id);
      return HTTP_BADREQUEST;
    }

  reply = json_object_new_object();
  items = json_object_new_array();
  json_object_object_add(reply, "items", items);

  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto error;

  query_params.type = Q_FIND_PL;
  query_params.filter = db_mprintf("filepath = '%q'", path);

  ret = fetch_playlists(&query_params, items, &total);
  if (ret < 0)
    goto error;

  json_object_object_add(reply, "total", json_object_new_int(total));
  json_object_object_add(reply, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(reply, "limit", json_object_new_int(query_params.limit));

  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "track playlists: Couldn't add playlists to response buffer.\n");

 error:
  free_query_params(&query_params, 1);
  jparse_free(reply);
  free(path);

  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_library_playlists(struct httpd_request *hreq)
{
  struct query_params query_params;
  json_object *reply;
  json_object *items;
  int total;
  int ret = 0;

  if (!is_modified(hreq, DB_ADMIN_DB_UPDATE))
    return HTTP_NOTMODIFIED;

  reply = json_object_new_object();
  items = json_object_new_array();
  json_object_object_add(reply, "items", items);

  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto error;

  query_params.type = Q_PL;
  query_params.sort = S_PLAYLIST;
  query_params.filter = db_mprintf("(f.type = %d OR f.type = %d OR f.type = %d)", PL_PLAIN, PL_SMART, PL_RSS);

  ret = fetch_playlists(&query_params, items, &total);
  free(query_params.filter);

  if (ret < 0)
    goto error;

  json_object_object_add(reply, "total", json_object_new_int(total));
  json_object_object_add(reply, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(reply, "limit", json_object_new_int(query_params.limit));

  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "browse: Couldn't add playlists to response buffer.\n");

 error:
  jparse_free(reply);

  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_library_playlist_get(struct httpd_request *hreq)
{
  uint32_t playlist_id;
  json_object *reply = NULL;
  int ret = 0;
  bool notfound = false;

  if (!is_modified(hreq, DB_ADMIN_DB_UPDATE))
    return HTTP_NOTMODIFIED;

  ret = safe_atou32(hreq->path_parts[3], &playlist_id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Could not parse playlist id to integer\n");
      goto error;
    }

  if (playlist_id == 0)
    {
      reply = json_object_new_object();
      json_object_object_add(reply, "id", json_object_new_int(0));
      json_object_object_add(reply, "name", json_object_new_string("Playlists"));
      json_object_object_add(reply, "type", json_object_new_string(db_pl_type_label(PL_FOLDER)));
      json_object_object_add(reply, "smart_playlist", json_object_new_boolean(false));
      json_object_object_add(reply, "folder", json_object_new_boolean(true));
    }
  else
    {
      reply = fetch_playlist(&notfound, playlist_id);
    }

  if (!reply)
    {
      ret = -1;
      goto error;
    }

  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "browse: Couldn't add playlist to response buffer.\n");

 error:
  jparse_free(reply);

  if (ret < 0)
    return notfound ? HTTP_NOTFOUND : HTTP_INTERNAL;

  return HTTP_OK;
}

static int
playlist_attrib_query_limit_set(int playlist_id, const char *param)
{
  struct playlist_info *pli;
  int query_limit;
  int ret;

  ret = safe_atoi32(param, &query_limit);
  if (ret < 0)
    return -1;

  pli = db_pl_fetch_byid(playlist_id);
  if (!pli)
    return -1;

  pli->query_limit = query_limit;

  ret = db_pl_update(pli);

  free_pli(pli, 0);

  return ret;
}

static int
jsonapi_reply_library_playlist_put(struct httpd_request *hreq)
{
  uint32_t playlist_id;
  const char *param;
  int ret;

  ret = safe_atou32(hreq->path_parts[3], &playlist_id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Could not parse playlist id to integer\n");
      return HTTP_BADREQUEST;
    }

  if ((param = httpd_query_value_find(hreq->query, "query_limit")))
    ret = playlist_attrib_query_limit_set(playlist_id, param);
  else
    ret = -1;

  if (ret < 0)
    return HTTP_BADREQUEST;

  return HTTP_OK;
}

static int
jsonapi_reply_library_playlist_tracks(struct httpd_request *hreq)
{
  struct query_params query_params;
  json_object *reply;
  json_object *items;
  int playlist_id;
  int total;
  int ret = 0;

  // Due to smart playlists possibly changing their tracks between rescans, disable caching in clients
  httpd_response_not_cachable(hreq);

  ret = safe_atoi32(hreq->path_parts[3], &playlist_id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "No valid playlist id given '%s'\n", hreq->path);

      return HTTP_BADREQUEST;
    }

  reply = json_object_new_object();
  items = json_object_new_array();
  json_object_object_add(reply, "items", items);

  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto error;

  query_params.type = Q_PLITEMS;
  query_params.id = playlist_id;

  ret = fetch_tracks(&query_params, items, &total);
  if (ret < 0)
    goto error;

  json_object_object_add(reply, "total", json_object_new_int(total));
  json_object_object_add(reply, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(reply, "limit", json_object_new_int(query_params.limit));

  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "playlist tracks: Couldn't add tracks to response buffer.\n");

 error:
  free_query_params(&query_params, 1);
  jparse_free(reply);

  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_library_playlist_delete(struct httpd_request *hreq)
{
  uint32_t pl_id;
  int ret;

  ret = safe_atou32(hreq->path_parts[3], &pl_id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "No valid playlist id given '%s'\n", hreq->path);

      return HTTP_BADREQUEST;
    }

  library_playlist_remove_byid(pl_id);

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_library_playlist_playlists(struct httpd_request *hreq)
{
  struct query_params query_params;
  json_object *reply;
  json_object *items;
  int playlist_id;
  int total;
  int ret = 0;

  if (!is_modified(hreq, DB_ADMIN_DB_MODIFIED))
    return HTTP_NOTMODIFIED;


  ret = safe_atoi32(hreq->path_parts[3], &playlist_id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "No valid playlist id given '%s'\n", hreq->path);

      return HTTP_BADREQUEST;
    }

  reply = json_object_new_object();
  items = json_object_new_array();
  json_object_object_add(reply, "items", items);

  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto error;

  query_params.type = Q_PL;
  query_params.sort = S_PLAYLIST;
  query_params.filter = db_mprintf("f.parent_id = %d AND (f.type = %d OR f.type = %d OR f.type = %d OR f.type = %d)",
				   playlist_id, PL_PLAIN, PL_SMART, PL_RSS, PL_FOLDER);

  ret = fetch_playlists(&query_params, items, &total);
  if (ret < 0)
    goto error;

  json_object_object_add(reply, "total", json_object_new_int(total));
  json_object_object_add(reply, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(reply, "limit", json_object_new_int(query_params.limit));

  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "playlist tracks: Couldn't add tracks to response buffer.\n");

 error:
  free_query_params(&query_params, 1);
  jparse_free(reply);

  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_library_playlist_tracks_put_byid(struct httpd_request *hreq)
{
  const char *param;
  int playlist_id;
  int ret;

  ret = safe_atoi32(hreq->path_parts[3], &playlist_id);
  if (ret < 0)
    return HTTP_INTERNAL;

  param = httpd_query_value_find(hreq->query, "play_count");
  if (!param)
    return HTTP_BADREQUEST;

  if (strcmp(param, "increment") == 0)
    {
      db_file_inc_playcount_byplid(playlist_id, false);
    }
  else if (strcmp(param, "played") == 0)
    {
      db_file_inc_playcount_byplid(playlist_id, true);
    }
  else
    {
      DPRINTF(E_WARN, L_WEB, "Ignoring invalid play_count param '%s'\n", param);
      return HTTP_BADREQUEST;
    }

  return HTTP_OK;
}

static int
jsonapi_reply_queue_save(struct httpd_request *hreq)
{
  const char *param;
  char buf[PATH_MAX+7];
  char *playlist_name = NULL;
  int ret = 0;

  if ((param = httpd_query_value_find(hreq->query, "name")) == NULL)
    {
      DPRINTF(E_LOG, L_WEB, "Invalid argument, missing 'name'\n");
      return HTTP_BADREQUEST;
    }

  if (!allow_modifying_stored_playlists)
    {
      DPRINTF(E_LOG, L_WEB, "Modifying stored playlists is not enabled in the config file\n");
      return 403;
    }

  if (access(default_playlist_directory, W_OK) < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Invalid playlist save directory '%s'\n", default_playlist_directory);
      return 403;
   }

  playlist_name = atrim(param);

  if (strlen(playlist_name) < 1) {
      free(playlist_name);

      DPRINTF(E_LOG, L_WEB, "Empty playlist name parameter is not allowed\n");
      return HTTP_BADREQUEST;
  }

  snprintf(buf, sizeof(buf), "/file:%s/%s", default_playlist_directory, playlist_name);
  free(playlist_name);

  ret = library_queue_save(buf);

  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_library_browse(struct httpd_request *hreq)
{
  struct query_params query_params;
  const char *param;
  const char *browse_type;
  enum media_kind media_kind;
  json_object *reply;
  json_object *items;
  int total;
  int ret;

  if (!is_modified(hreq, DB_ADMIN_DB_UPDATE))
    return HTTP_NOTMODIFIED;

  browse_type = hreq->path_parts[2];
  DPRINTF(E_DBG, L_WEB, "Browse query with type '%s'\n", browse_type);

  media_kind = 0;
  param = httpd_query_value_find(hreq->query, "media_kind");
  if (param)
    {
      media_kind = db_media_kind_enum(param);
      if (!media_kind)
	{
	  DPRINTF(E_LOG, L_WEB, "Invalid media kind '%s'\n", param);
	  return HTTP_BADREQUEST;
	}
    }

  reply = json_object_new_object();
  items = json_object_new_array();
  json_object_object_add(reply, "items", items);

  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto error;

  if (strcmp(browse_type, "genres") == 0)
    {
      query_params.type = Q_BROWSE_GENRES;
      query_params.sort = S_GENRE;
      query_params.idx_type = I_NONE;
    }
  else if (strcmp(browse_type, "composers") == 0)
    {
      query_params.type = Q_BROWSE_COMPOSERS;
      query_params.sort = S_COMPOSER;
      query_params.idx_type = I_NONE;
    }
  else
    {
      DPRINTF(E_LOG, L_WEB, "Invalid browse type '%s'\n", browse_type);
      goto error;
    }

  if (media_kind)
    query_params.filter = db_mprintf("(f.media_kind = %d)", media_kind);

  ret = fetch_browse_info(&query_params, items, &total);
  if (ret < 0)
    goto error;

  json_object_object_add(reply, "total", json_object_new_int(total));
  json_object_object_add(reply, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(reply, "limit", json_object_new_int(query_params.limit));

  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "browse: Couldn't add browse items to response buffer.\n");

 error:
  jparse_free(reply);
  free_query_params(&query_params, 1);

  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_library_browseitem(struct httpd_request *hreq)
{
  struct query_params query_params;
  const char *browse_type;
  const char *item_name;
  struct db_browse_info dbbi;
  json_object *reply;
  int ret;

  if (!is_modified(hreq, DB_ADMIN_DB_UPDATE))
    return HTTP_NOTMODIFIED;

  browse_type = hreq->path_parts[2];
  item_name = hreq->path_parts[3];
  DPRINTF(E_DBG, L_WEB, "Browse item query with type '%s'\n", browse_type);

  reply = json_object_new_object();

  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto error;

  if (strcmp(browse_type, "genres") == 0)
    {
      query_params.type = Q_BROWSE_GENRES;
      query_params.sort = S_GENRE;
      query_params.idx_type = I_NONE;
      query_params.filter = db_mprintf("(f.genre = %Q)", item_name);
    }
  else if (strcmp(browse_type, "composers") == 0)
    {
      query_params.type = Q_BROWSE_COMPOSERS;
      query_params.sort = S_COMPOSER;
      query_params.idx_type = I_NONE;
      query_params.filter = db_mprintf("(f.composer = %Q)", item_name);
    }
  else
    {
      DPRINTF(E_LOG, L_WEB, "Invalid browse type '%s'\n", browse_type);
      goto error;
    }

  ret = db_query_start(&query_params);
  if (ret < 0)
    goto error;

  if ((ret = db_query_fetch_browse(&dbbi, &query_params)) == 0)
    {
      reply = browse_info_to_json(&dbbi);
    }
  if (!reply)
    {
      ret = -1;
      goto error;
    }

  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "browse: Couldn't add browse item to response buffer.\n");

 error:
  db_query_end(&query_params);
  jparse_free(reply);
  free_query_params(&query_params, 1);

  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_library_count(struct httpd_request *hreq)
{
  const char *param_expression;
  char *expression;
  struct smartpl smartpl_expression;
  struct query_params qp;
  struct filecount_info fci;
  json_object *jreply;
  int ret;

  if (!is_modified(hreq, DB_ADMIN_DB_UPDATE))
    return HTTP_NOTMODIFIED;

  memset(&qp, 0, sizeof(struct query_params));
  qp.type = Q_COUNT_ITEMS;

  param_expression = httpd_query_value_find(hreq->query, "expression");
  if (param_expression)
    {
      memset(&smartpl_expression, 0, sizeof(struct smartpl));
      expression = safe_asprintf("\"query\" { %s }", param_expression);
      ret = smartpl_query_parse_string(&smartpl_expression, expression);
      free(expression);

      if (ret < 0)
	return HTTP_BADREQUEST;

      qp.filter = strdup(smartpl_expression.query_where);
      free_smartpl(&smartpl_expression, 1);
    }

  CHECK_NULL(L_WEB, jreply = json_object_new_object());

  ret = db_filecount_get(&fci, &qp);
  if (ret == 0)
    {
      json_object_object_add(jreply, "tracks", json_object_new_int(fci.count));
      json_object_object_add(jreply, "artists", json_object_new_int(fci.artist_count));
      json_object_object_add(jreply, "albums", json_object_new_int(fci.album_count));
      json_object_object_add(jreply, "db_playtime", json_object_new_int64((fci.length / 1000)));
      json_object_object_add(jreply, "file_size", json_object_new_int64((fci.file_size)));
    }
  else
    {
      DPRINTF(E_LOG, L_WEB, "library: failed to get count info\n");
    }

  free(qp.filter);

  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));
  jparse_free(jreply);

  return HTTP_OK;
}

static int
jsonapi_reply_library_files(struct httpd_request *hreq)
{
  const char *param;
  int directory_id;
  json_object *reply;
  json_object *directories;
  struct query_params query_params;
  json_object *tracks;
  json_object *tracks_items;
  json_object *playlists;
  json_object *playlists_items;
  int total;
  int ret;

  param = httpd_query_value_find(hreq->query, "directory");

  directory_id = DIR_FILE;
  if (param)
    {
      directory_id = db_directory_id_bypath(param);
      if (directory_id <= 0)
	return HTTP_INTERNAL;
    }

  reply = json_object_new_object();

  // Add sub directories to response
  directories = json_object_new_array();
  json_object_object_add(reply, "directories", directories);

  ret = fetch_directories(directory_id, directories);
  if (ret < 0)
    {
      goto error;
    }

  // Add tracks to response
  tracks = json_object_new_object();
  json_object_object_add(reply, "tracks", tracks);
  tracks_items = json_object_new_array();
  json_object_object_add(tracks, "items", tracks_items);
  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto error;

  query_params.type = Q_ITEMS;
  query_params.sort = S_VPATH;
  query_params.filter = db_mprintf("(f.directory_id = %d)", directory_id);

  ret = fetch_tracks(&query_params, tracks_items, &total);
  free(query_params.filter);

  if (ret < 0)
    goto error;

  json_object_object_add(tracks, "total", json_object_new_int(total));
  json_object_object_add(tracks, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(tracks, "limit", json_object_new_int(query_params.limit));

  // Add playlists
  playlists = json_object_new_object();
  json_object_object_add(reply, "playlists", playlists);
  playlists_items = json_object_new_array();
  json_object_object_add(playlists, "items", playlists_items);
  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto error;

  query_params.type = Q_PL;
  query_params.sort = S_VPATH;
  query_params.filter = db_mprintf("(f.directory_id = %d)", directory_id);

  ret = fetch_playlists(&query_params, playlists_items, &total);
  free(query_params.filter);

  if (ret < 0)
    goto error;

  json_object_object_add(playlists, "total", json_object_new_int(total));
  json_object_object_add(playlists, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(playlists, "limit", json_object_new_int(query_params.limit));

  // Build JSON response
  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "browse: Couldn't add directories to response buffer.\n");

 error:
  jparse_free(reply);

  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_library_add(struct httpd_request *hreq)
{
  const char *url;
  int ret;

  url = httpd_query_value_find(hreq->query, "url");
  if (!url)
    {
      DPRINTF(E_LOG, L_WEB, "Missing URL parameter for library add\n");
      return HTTP_BADREQUEST;
    }

  ret = library_item_add(url);
  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_OK;
}

static int
search_tracks(json_object *reply, struct httpd_request *hreq, const char *param_query, struct smartpl *smartpl_expression, enum media_kind media_kind)
{
  json_object *type;
  json_object *items;
  struct query_params query_params;
  int total;
  int ret;

  memset(&query_params, 0, sizeof(struct query_params));

  type = json_object_new_object();
  json_object_object_add(reply, "tracks", type);
  items = json_object_new_array();
  json_object_object_add(type, "items", items);

  query_params.type = Q_ITEMS;
  query_params.sort = S_NAME;

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto out;

  if (param_query)
    {
      if (media_kind)
	query_params.filter = db_mprintf("(f.title LIKE '%%%q%%' AND f.media_kind = %d)", param_query, media_kind);
      else
	query_params.filter = db_mprintf("(f.title LIKE '%%%q%%')", param_query);
    }
  else
    {
      query_params.filter = strdup(smartpl_expression->query_where);
      query_params.order = safe_strdup(smartpl_expression->order);

      if (smartpl_expression->limit > 0)
	{
	  query_params.idx_type = I_SUB;
	  query_params.limit = smartpl_expression->limit;
	  query_params.offset = 0;
	}
    }

  ret = fetch_tracks(&query_params, items, &total);
  if (ret < 0)
    goto out;

  json_object_object_add(type, "total", json_object_new_int(total));
  json_object_object_add(type, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(type, "limit", json_object_new_int(query_params.limit));

 out:
  free_query_params(&query_params, 1);

  return ret;
}

static int
search_artists(json_object *reply, struct httpd_request *hreq, const char *param_query, struct smartpl *smartpl_expression, enum media_kind media_kind)
{
  json_object *type;
  json_object *items;
  struct query_params query_params;
  int total;
  int ret;

  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto out;

  type = json_object_new_object();
  json_object_object_add(reply, "artists", type);
  items = json_object_new_array();
  json_object_object_add(type, "items", items);

  query_params.type = Q_GROUP_ARTISTS;
  query_params.sort = S_ARTIST;

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto out;

  if (param_query)
    {
      if (media_kind)
	query_params.filter = db_mprintf("(f.album_artist LIKE '%%%q%%' AND f.media_kind = %d)", param_query, media_kind);
      else
	query_params.filter = db_mprintf("(f.album_artist LIKE '%%%q%%')", param_query);
    }
  else
    {
      query_params.filter = strdup(smartpl_expression->query_where);
      query_params.having = safe_strdup(smartpl_expression->having);
      query_params.order = safe_strdup(smartpl_expression->order);

      if (smartpl_expression->limit > 0)
	{
	  query_params.idx_type = I_SUB;
	  query_params.limit = smartpl_expression->limit;
	  query_params.offset = 0;
	}
    }

  ret = fetch_artists(&query_params, items, &total);
  if (ret < 0)
    goto out;

  json_object_object_add(type, "total", json_object_new_int(total));
  json_object_object_add(type, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(type, "limit", json_object_new_int(query_params.limit));

 out:
  free_query_params(&query_params, 1);

  return ret;
}

static int
search_albums(json_object *reply, struct httpd_request *hreq, const char *param_query, struct smartpl *smartpl_expression, enum media_kind media_kind)
{
  json_object *type;
  json_object *items;
  struct query_params query_params;
  int total;
  int ret;

  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto out;

  type = json_object_new_object();
  json_object_object_add(reply, "albums", type);
  items = json_object_new_array();
  json_object_object_add(type, "items", items);

  query_params.type = Q_GROUP_ALBUMS;
  query_params.sort = S_ALBUM;

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto out;

  if (param_query)
    {
      if (media_kind)
	query_params.filter = db_mprintf("(f.album LIKE '%%%q%%' AND f.media_kind = %d)", param_query, media_kind);
      else
	query_params.filter = db_mprintf("(f.album LIKE '%%%q%%')", param_query);
    }
  else
    {
      query_params.filter = strdup(smartpl_expression->query_where);
      query_params.having = safe_strdup(smartpl_expression->having);
      query_params.order = safe_strdup(smartpl_expression->order);

      if (smartpl_expression->limit > 0)
	{
	  query_params.idx_type = I_SUB;
	  query_params.limit = smartpl_expression->limit;
	  query_params.offset = 0;
	}
    }

  ret = fetch_albums(&query_params, items, &total);
  if (ret < 0)
    goto out;

  json_object_object_add(type, "total", json_object_new_int(total));
  json_object_object_add(type, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(type, "limit", json_object_new_int(query_params.limit));

 out:
  free_query_params(&query_params, 1);

  return ret;
}

static int
search_composers(json_object *reply, struct httpd_request *hreq, const char *param_query, struct smartpl *smartpl_expression, enum media_kind media_kind)
{
  json_object *type;
  json_object *items;
  struct query_params query_params;
  int total;
  int ret;

  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto out;

  type = json_object_new_object();
  json_object_object_add(reply, "composers", type);
  items = json_object_new_array();
  json_object_object_add(type, "items", items);

  query_params.type = Q_BROWSE_COMPOSERS;
  query_params.sort = S_COMPOSER;

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto out;

  if (param_query)
    {
      if (media_kind)
	query_params.filter = db_mprintf("(f.composer LIKE '%%%q%%' AND f.media_kind = %d)", param_query, media_kind);
      else
	query_params.filter = db_mprintf("(f.composer LIKE '%%%q%%')", param_query);
    }
  else
    {
      query_params.filter = strdup(smartpl_expression->query_where);
      query_params.having = safe_strdup(smartpl_expression->having);
      query_params.order = safe_strdup(smartpl_expression->order);

      if (smartpl_expression->limit > 0)
	{
	  query_params.idx_type = I_SUB;
	  query_params.limit = smartpl_expression->limit;
	  query_params.offset = 0;
	}
    }

  ret = fetch_browse_info(&query_params, items, &total);
  if (ret < 0)
    goto out;

  json_object_object_add(type, "total", json_object_new_int(total));
  json_object_object_add(type, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(type, "limit", json_object_new_int(query_params.limit));

 out:
  free_query_params(&query_params, 1);

  return ret;
}

static int
search_genres(json_object *reply, struct httpd_request *hreq, const char *param_query, struct smartpl *smartpl_expression, enum media_kind media_kind)
{
  json_object *type;
  json_object *items;
  struct query_params query_params;
  int total;
  int ret;

  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto out;

  type = json_object_new_object();
  json_object_object_add(reply, "genres", type);
  items = json_object_new_array();
  json_object_object_add(type, "items", items);

  query_params.type = Q_BROWSE_GENRES;
  query_params.sort = S_GENRE;

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto out;

  if (param_query)
    {
      if (media_kind)
	query_params.filter = db_mprintf("(f.genre LIKE '%%%q%%' AND f.media_kind = %d)", param_query, media_kind);
      else
	query_params.filter = db_mprintf("(f.genre LIKE '%%%q%%')", param_query);
    }
  else
    {
      query_params.filter = strdup(smartpl_expression->query_where);
      query_params.having = safe_strdup(smartpl_expression->having);
      query_params.order = safe_strdup(smartpl_expression->order);

      if (smartpl_expression->limit > 0)
	{
	  query_params.idx_type = I_SUB;
	  query_params.limit = smartpl_expression->limit;
	  query_params.offset = 0;
	}
    }

  ret = fetch_browse_info(&query_params, items, &total);
  if (ret < 0)
    goto out;

  json_object_object_add(type, "total", json_object_new_int(total));
  json_object_object_add(type, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(type, "limit", json_object_new_int(query_params.limit));

 out:
  free_query_params(&query_params, 1);

  return ret;
}

static int
search_playlists(json_object *reply, struct httpd_request *hreq, const char *param_query)
{
  json_object *type;
  json_object *items;
  struct query_params query_params;
  int total;
  int ret;

  if (!param_query)
    return 0;

  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto out;

  type = json_object_new_object();
  json_object_object_add(reply, "playlists", type);
  items = json_object_new_array();
  json_object_object_add(type, "items", items);

  query_params.type = Q_PL;
  query_params.sort = S_PLAYLIST;
  query_params.filter = db_mprintf("((f.type = %d OR f.type = %d OR f.type = %d) AND f.title LIKE '%%%q%%')", PL_PLAIN, PL_SMART, PL_RSS, param_query);

  ret = fetch_playlists(&query_params, items, &total);
  if (ret < 0)
    goto out;

  json_object_object_add(type, "total", json_object_new_int(total));
  json_object_object_add(type, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(type, "limit", json_object_new_int(query_params.limit));

 out:
  free_query_params(&query_params, 1);

  return ret;
}

static int
jsonapi_reply_search(struct httpd_request *hreq)
{
  const char *param_type;
  const char *param_query;
  const char *param_expression;
  const char *param_media_kind;
  enum media_kind media_kind;
  char *expression;
  struct smartpl smartpl_expression;
  json_object *reply;
  int ret = 0;

  reply = NULL;

  param_type = httpd_query_value_find(hreq->query, "type");
  param_query = httpd_query_value_find(hreq->query, "query");
  param_expression = httpd_query_value_find(hreq->query, "expression");

  if (!param_type || (!param_query && !param_expression))
    {
      DPRINTF(E_LOG, L_WEB, "Missing request parameter\n");

      return HTTP_BADREQUEST;
    }

  media_kind = 0;
  param_media_kind = httpd_query_value_find(hreq->query, "media_kind");
  if (param_media_kind)
    {
      media_kind = db_media_kind_enum(param_media_kind);
      if (!media_kind)
      {
	DPRINTF(E_LOG, L_WEB, "Invalid media kind '%s'\n", param_media_kind);
	return HTTP_BADREQUEST;
      }
    }

  memset(&smartpl_expression, 0, sizeof(struct smartpl));

  if (param_expression)
    {
      expression = safe_asprintf("\"query\" { %s }", param_expression);

      ret = smartpl_query_parse_string(&smartpl_expression, expression);
      free(expression);

      if (ret < 0)
	return HTTP_BADREQUEST;
    }

  reply = json_object_new_object();

  if (strstr(param_type, "track"))
    {
      ret = search_tracks(reply, hreq, param_query, &smartpl_expression, media_kind);
      if (ret < 0)
	goto error;
    }

  if (strstr(param_type, "artist"))
    {
      ret = search_artists(reply, hreq, param_query, &smartpl_expression, media_kind);
      if (ret < 0)
	goto error;
    }

  if (strstr(param_type, "album"))
    {
      ret = search_albums(reply, hreq, param_query, &smartpl_expression, media_kind);
      if (ret < 0)
	goto error;
    }

  if (strstr(param_type, "composer"))
    {
      ret = search_composers(reply, hreq, param_query, &smartpl_expression, media_kind);
      if (ret < 0)
        goto error;
    }

  if (strstr(param_type, "genre"))
    {
      ret = search_genres(reply, hreq, param_query, &smartpl_expression, media_kind);
      if (ret < 0)
        goto error;
    }

  if (strstr(param_type, "playlist") && param_query)
    {
      ret = search_playlists(reply, hreq, param_query);
      if (ret < 0)
	goto error;
    }

  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "playlist tracks: Couldn't add tracks to response buffer.\n");

 error:
  jparse_free(reply);
  free_smartpl(&smartpl_expression, 1);

  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_library_backup(struct httpd_request *hreq)
{
  int ret;
  ret = db_backup();

  if (ret < 0)
  {
    if (ret == -2)
      return HTTP_SERVUNAVAIL;  // not enabled by config

    return HTTP_INTERNAL;
  }

  return HTTP_OK;
}


static struct httpd_uri_map adm_handlers[] =
  {
    { HTTPD_METHOD_GET,    "^/api/config$",                                jsonapi_reply_config },
    { HTTPD_METHOD_GET,    "^/api/settings$",                              jsonapi_reply_settings_get },
    { HTTPD_METHOD_GET,    "^/api/settings/[A-Za-z0-9_]+$",                jsonapi_reply_settings_category_get },
    { HTTPD_METHOD_GET,    "^/api/settings/[A-Za-z0-9_]+/[A-Za-z0-9_]+$",  jsonapi_reply_settings_option_get },
    { HTTPD_METHOD_PUT,    "^/api/settings/[A-Za-z0-9_]+/[A-Za-z0-9_]+$",  jsonapi_reply_settings_option_put },
    { HTTPD_METHOD_DELETE, "^/api/settings/[A-Za-z0-9_]+/[A-Za-z0-9_]+$",  jsonapi_reply_settings_option_delete },
    { HTTPD_METHOD_GET,    "^/api/library$",                               jsonapi_reply_library },
    { HTTPD_METHOD_GET |
      HTTPD_METHOD_PUT,    "^/api/update$",                                jsonapi_reply_update },
    { HTTPD_METHOD_PUT,    "^/api/rescan$",                                jsonapi_reply_meta_rescan },
    { HTTPD_METHOD_GET,    "^/api/spotify-logout$",                        jsonapi_reply_spotify_logout },
    { HTTPD_METHOD_GET,    "^/api/spotify$",                               jsonapi_reply_spotify },
    { HTTPD_METHOD_GET,    "^/api/pairing$",                               jsonapi_reply_pairing_get },
    { HTTPD_METHOD_POST,   "^/api/pairing$",                               jsonapi_reply_pairing_pair },
    { HTTPD_METHOD_POST,   "^/api/lastfm-login$",                          jsonapi_reply_lastfm_login },
    { HTTPD_METHOD_GET,    "^/api/lastfm-logout$",                         jsonapi_reply_lastfm_logout },
    { HTTPD_METHOD_GET,    "^/api/lastfm$",                                jsonapi_reply_lastfm },
    { HTTPD_METHOD_POST,   "^/api/verification$",                          jsonapi_reply_verification },

    { HTTPD_METHOD_GET,    "^/api/outputs$",                               jsonapi_reply_outputs },
    { HTTPD_METHOD_PUT,    "^/api/outputs/set$",                           jsonapi_reply_outputs_set },
    { HTTPD_METHOD_POST,   "^/api/select-outputs$",                        jsonapi_reply_outputs_set }, // deprecated: use "/api/outputs/set"
    { HTTPD_METHOD_GET,    "^/api/outputs/[[:digit:]]+$",                  jsonapi_reply_outputs_get_byid },
    { HTTPD_METHOD_PUT,    "^/api/outputs/[[:digit:]]+$",                  jsonapi_reply_outputs_put_byid },
    { HTTPD_METHOD_PUT,    "^/api/outputs/[[:digit:]]+/toggle$",           jsonapi_reply_outputs_toggle_byid },

    { HTTPD_METHOD_GET,    "^/api/player$",                                jsonapi_reply_player },
    { HTTPD_METHOD_PUT,    "^/api/player/play$",                           jsonapi_reply_player_play },
    { HTTPD_METHOD_PUT,    "^/api/player/pause$",                          jsonapi_reply_player_pause },
    { HTTPD_METHOD_PUT,    "^/api/player/stop$",                           jsonapi_reply_player_stop },
    { HTTPD_METHOD_PUT,    "^/api/player/toggle$",                         jsonapi_reply_player_toggle },
    { HTTPD_METHOD_PUT,    "^/api/player/next$",                           jsonapi_reply_player_next },
    { HTTPD_METHOD_PUT,    "^/api/player/previous$",                       jsonapi_reply_player_previous },
    { HTTPD_METHOD_PUT,    "^/api/player/shuffle$",                        jsonapi_reply_player_shuffle },
    { HTTPD_METHOD_PUT,    "^/api/player/repeat$",                         jsonapi_reply_player_repeat },
    { HTTPD_METHOD_PUT,    "^/api/player/consume$",                        jsonapi_reply_player_consume },
    { HTTPD_METHOD_PUT,    "^/api/player/volume$",                         jsonapi_reply_player_volume },
    { HTTPD_METHOD_PUT,    "^/api/player/seek$",                           jsonapi_reply_player_seek },

    { HTTPD_METHOD_GET,    "^/api/queue$",                                 jsonapi_reply_queue },
    { HTTPD_METHOD_PUT,    "^/api/queue/clear$",                           jsonapi_reply_queue_clear },
    { HTTPD_METHOD_POST,   "^/api/queue/items/add$",                       jsonapi_reply_queue_tracks_add },
    { HTTPD_METHOD_PUT,    "^/api/queue/items/[[:digit:]]+$",              jsonapi_reply_queue_tracks_update },
    { HTTPD_METHOD_PUT,    "^/api/queue/items/now_playing$",               jsonapi_reply_queue_tracks_update },
    { HTTPD_METHOD_DELETE, "^/api/queue/items/[[:digit:]]+$",              jsonapi_reply_queue_tracks_delete },
    { HTTPD_METHOD_POST,   "^/api/queue/save$",                            jsonapi_reply_queue_save},

    { HTTPD_METHOD_GET,    "^/api/library/playlists$",                     jsonapi_reply_library_playlists },
    { HTTPD_METHOD_GET,    "^/api/library/playlists/[[:digit:]]+$",        jsonapi_reply_library_playlist_get },
    { HTTPD_METHOD_PUT,    "^/api/library/playlists/[[:digit:]]+$",        jsonapi_reply_library_playlist_put },
    { HTTPD_METHOD_GET,    "^/api/library/playlists/[[:digit:]]+/tracks$", jsonapi_reply_library_playlist_tracks },
    { HTTPD_METHOD_PUT,    "^/api/library/playlists/[[:digit:]]+/tracks",  jsonapi_reply_library_playlist_tracks_put_byid},
//    { HTTPD_METHOD_POST,   "^/api/library/playlists/[[:digit:]]+/tracks$", jsonapi_reply_library_playlists_tracks },
    { HTTPD_METHOD_DELETE, "^/api/library/playlists/[[:digit:]]+$",        jsonapi_reply_library_playlist_delete },
    { HTTPD_METHOD_GET,    "^/api/library/playlists/[[:digit:]]+/playlists", jsonapi_reply_library_playlist_playlists },
    { HTTPD_METHOD_GET,    "^/api/library/artists$",                       jsonapi_reply_library_artists },
    { HTTPD_METHOD_GET,    "^/api/library/artists/[[:digit:]]+$",          jsonapi_reply_library_artist },
    { HTTPD_METHOD_GET,    "^/api/library/artists/[[:digit:]]+/albums$",   jsonapi_reply_library_artist_albums },
    { HTTPD_METHOD_GET,    "^/api/library/albums$",                        jsonapi_reply_library_albums },
    { HTTPD_METHOD_GET,    "^/api/library/albums/[[:digit:]]+$",           jsonapi_reply_library_album },
    { HTTPD_METHOD_GET,    "^/api/library/albums/[[:digit:]]+/tracks$",    jsonapi_reply_library_album_tracks },
    { HTTPD_METHOD_PUT,    "^/api/library/albums/[[:digit:]]+/tracks$",    jsonapi_reply_library_album_tracks_put_byid },
    { HTTPD_METHOD_PUT,    "^/api/library/tracks$",                        jsonapi_reply_library_tracks_put },
    { HTTPD_METHOD_GET,    "^/api/library/tracks/[[:digit:]]+$",           jsonapi_reply_library_tracks_get_byid },
    { HTTPD_METHOD_PUT,    "^/api/library/tracks/[[:digit:]]+$",           jsonapi_reply_library_tracks_put_byid },
    { HTTPD_METHOD_GET,    "^/api/library/tracks/[[:digit:]]+/playlists$", jsonapi_reply_library_track_playlists },
    { HTTPD_METHOD_GET,    "^/api/library/(genres|composers)$",            jsonapi_reply_library_browse },
    { HTTPD_METHOD_GET,    "^/api/library/(genres|composers)/.*$",         jsonapi_reply_library_browseitem },
    { HTTPD_METHOD_GET,    "^/api/library/count$",                         jsonapi_reply_library_count },
    { HTTPD_METHOD_GET,    "^/api/library/files$",                         jsonapi_reply_library_files },
    { HTTPD_METHOD_POST,   "^/api/library/add$",                           jsonapi_reply_library_add },
    { HTTPD_METHOD_PUT,    "^/api/library/backup$",                        jsonapi_reply_library_backup },

    { HTTPD_METHOD_GET,    "^/api/search$",                                jsonapi_reply_search },

    { HTTPD_METHOD_GET,    "^/api/listenbrainz$",                          jsonapi_reply_listenbrainz },
    { HTTPD_METHOD_POST,   "^/api/listenbrainz/token$",                    jsonapi_reply_listenbrainz_token_add },
    { HTTPD_METHOD_DELETE, "^/api/listenbrainz/token$",                    jsonapi_reply_listenbrainz_token_delete },

    { 0, NULL, NULL }
  };


/* ------------------------------- JSON API --------------------------------- */

static void
jsonapi_request(struct httpd_request *hreq)
{
  int status_code;

  if (!httpd_request_is_authorized(hreq))
    {
      return;
    }

  if (!hreq->handler)
    {
      DPRINTF(E_LOG, L_WEB, "Unrecognized JSON API request: '%s'\n", hreq->uri);
      httpd_send_error(hreq, HTTP_BADREQUEST, "Bad Request");
      return;
    }

  status_code = hreq->handler(hreq);

  if (status_code >= 400)
    DPRINTF(E_LOG, L_WEB, "JSON api request failed with error code %d (%s)\n", status_code, hreq->uri);

  switch (status_code)
    {
      case HTTP_OK:                  /* 200 OK */
	httpd_header_add(hreq->out_headers, "Content-Type", "application/json");
	httpd_send_reply(hreq, status_code, "OK", HTTPD_SEND_NO_GZIP);
	break;
      case HTTP_NOCONTENT:           /* 204 No Content */
	httpd_send_reply(hreq, status_code, "No Content", HTTPD_SEND_NO_GZIP);
	break;
      case HTTP_NOTMODIFIED:         /* 304 Not Modified */
	httpd_send_reply(hreq, HTTP_NOTMODIFIED, NULL, HTTPD_SEND_NO_GZIP);
	break;
      case HTTP_BADREQUEST:          /* 400 Bad Request */
	httpd_send_error(hreq, status_code, "Bad Request");
	break;
      case 403:
	httpd_send_error(hreq, status_code, "Forbidden");
	break;
      case HTTP_NOTFOUND:            /* 404 Not Found */
	httpd_send_error(hreq, status_code, "Not Found");
	break;
      case HTTP_SERVUNAVAIL:            /* 503 */
        httpd_send_error(hreq, status_code, "Service Unavailable");
        break;
      case HTTP_INTERNAL:            /* 500 Internal Server Error */
      default:
	httpd_send_error(hreq, HTTP_INTERNAL, "Internal Server Error");
	break;
    }
}

static int
jsonapi_init(void)
{
  char *temp_path;

  default_playlist_directory = NULL;
  allow_modifying_stored_playlists = cfg_getbool(cfg_getsec(cfg, "library"), "allow_modifying_stored_playlists");
  if (allow_modifying_stored_playlists)
    {
      temp_path = cfg_getstr(cfg_getsec(cfg, "library"), "default_playlist_directory");
      if (temp_path)
	{
	  // The path in the conf file may have a trailing slash character. Return the realpath like it is done for the library directories.
	  default_playlist_directory = realpath(temp_path, NULL);
	  if (default_playlist_directory)
	    {
	      if (access(default_playlist_directory, W_OK) < 0)
	        DPRINTF(E_WARN, L_WEB, "Non-writable playlist save directory '%s'\n", default_playlist_directory);
	    }
	}

      if (!default_playlist_directory)
	{
	  DPRINTF(E_LOG, L_WEB, "Invalid playlist save directory, disabling modifying stored playlists\n");
	  allow_modifying_stored_playlists = false;
	}
     }

  return 0;
}

static void
jsonapi_deinit(void)
{
  free(default_playlist_directory);
}

struct httpd_module httpd_jsonapi =
{
  .name = "JSON API",
  .type = MODULE_JSONAPI,
  .logdomain = L_WEB,
  .subpaths = { "/api/", NULL },
  .handlers = adm_handlers,
  .init = jsonapi_init,
  .deinit = jsonapi_deinit,
  .request = jsonapi_request,
};
