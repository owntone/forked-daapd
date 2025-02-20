/*
 * Copyright (C) 2009-2011 Julien BLACHE <jb@jblache.org>
 * Copyright (C) 2010 Kai Elwert <elwertk@googlemail.com>
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
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unictype.h>
#include <uninorm.h>
#include <unistr.h>
#include <sys/mman.h>
#include <limits.h>
#include <assert.h>

#include <sqlite3.h>

#include "conffile.h"
#include "logger.h"
#include "cache.h"
#include "listener.h"
#include "library.h"
#include "misc.h"
#include "db.h"
#include "db_init.h"
#include "db_upgrade.h"
#include "rng.h"


// Inotify cookies are uint32_t
#define INOTIFY_FAKE_COOKIE ((int64_t)1 << 32)

// Flags that the field will not be bound to prepared statements, which is relevant if the field has no
// matching column, or if the the column value is set automatically by the db, e.g. by a trigger
#define DB_FLAG_NO_BIND  (1 << 0)
// Flags that we will only update column value if we have non-zero value (to avoid zeroing e.g. rating)
#define DB_FLAG_NO_ZERO  (1 << 1)

// The two last columns of playlist_info are calculated fields, so all playlist retrieval functions must use this query
#define Q_PL_SELECT "SELECT f.*, COUNT(pi.id), SUM(pi.filepath NOT NULL AND pi.filepath LIKE 'http%%')" \
                    " FROM playlists f LEFT JOIN playlistitems pi ON (f.id = pi.playlistid)"

enum group_type {
  G_ALBUMS = 1,
  G_ARTISTS = 2,
};

enum field_type {
  DB_TYPE_INT,
  DB_TYPE_INT64,
  DB_TYPE_STRING,
};

enum fixup_type {
  DB_FIXUP_STANDARD = 0,
  DB_FIXUP_NO_SANITIZE,
  DB_FIXUP_TITLE,
  DB_FIXUP_ARTIST,
  DB_FIXUP_ALBUM,
  DB_FIXUP_ALBUM_ARTIST,
  DB_FIXUP_GENRE,
  DB_FIXUP_COMPOSER,
  DB_FIXUP_TYPE,
  DB_FIXUP_CODECTYPE,
  DB_FIXUP_MEDIA_KIND,
  DB_FIXUP_ITEM_KIND,
  DB_FIXUP_TITLE_SORT,
  DB_FIXUP_ARTIST_SORT,
  DB_FIXUP_ALBUM_SORT,
  DB_FIXUP_ALBUM_ARTIST_SORT,
  DB_FIXUP_COMPOSER_SORT,
  DB_FIXUP_TIME_MODIFIED,
  DB_FIXUP_SONGARTISTID,
  DB_FIXUP_SONGALBUMID,
};

struct db_unlock {
  char thread_name_tid[32];
  int proceed;
  pthread_cond_t cond;
  pthread_mutex_t lck;
};

struct db_statements
{
  sqlite3_stmt *files_insert;
  sqlite3_stmt *files_update;
  sqlite3_stmt *files_ping;

  sqlite3_stmt *playlists_insert;
  sqlite3_stmt *playlists_update;

  sqlite3_stmt *queue_items_insert;
  sqlite3_stmt *queue_items_update;
};

struct col_type_map {
  char *name;
  ssize_t offset;
  enum field_type type;
  enum fixup_type fixup;
  short flag;
};

struct qi_mfi_map
{
  ssize_t qi_offset;
  ssize_t mfi_offset;
  ssize_t dbmfi_offset;
};

struct fixup_ctx
{
  const struct col_type_map *map;
  size_t map_size;
  void *data;
  struct media_file_info *mfi;
  struct playlist_info *pli;
  struct db_queue_item *qi;
};

struct query_clause {
  char *where;
  char *group;
  char *having;
  char *order;
  char *index;
};

struct browse_clause {
  char *select;
  char *where;
  char *group;
};

/* This list must be kept in sync with
 * - the order of the columns in the files table
 * - the type and name of the fields in struct media_file_info
 */
static const struct col_type_map mfi_cols_map[] =
  {
    { "id",                 mfi_offsetof(id),                 DB_TYPE_INT,    DB_FIXUP_STANDARD, DB_FLAG_NO_BIND },
    { "path",               mfi_offsetof(path),               DB_TYPE_STRING, DB_FIXUP_NO_SANITIZE },
    { "virtual_path",       mfi_offsetof(virtual_path),       DB_TYPE_STRING },
    { "fname",              mfi_offsetof(fname),              DB_TYPE_STRING, DB_FIXUP_NO_SANITIZE },
    { "directory_id",       mfi_offsetof(directory_id),       DB_TYPE_INT },
    { "title",              mfi_offsetof(title),              DB_TYPE_STRING, DB_FIXUP_TITLE },
    { "artist",             mfi_offsetof(artist),             DB_TYPE_STRING, DB_FIXUP_ARTIST },
    { "album",              mfi_offsetof(album),              DB_TYPE_STRING, DB_FIXUP_ALBUM },
    { "album_artist",       mfi_offsetof(album_artist),       DB_TYPE_STRING, DB_FIXUP_ALBUM_ARTIST },
    { "genre",              mfi_offsetof(genre),              DB_TYPE_STRING, DB_FIXUP_GENRE },
    { "comment",            mfi_offsetof(comment),            DB_TYPE_STRING },
    { "type",               mfi_offsetof(type),               DB_TYPE_STRING, DB_FIXUP_TYPE },
    { "composer",           mfi_offsetof(composer),           DB_TYPE_STRING, DB_FIXUP_COMPOSER },
    { "orchestra",          mfi_offsetof(orchestra),          DB_TYPE_STRING },
    { "conductor",          mfi_offsetof(conductor),          DB_TYPE_STRING },
    { "grouping",           mfi_offsetof(grouping),           DB_TYPE_STRING },
    { "url",                mfi_offsetof(url),                DB_TYPE_STRING },
    { "bitrate",            mfi_offsetof(bitrate),            DB_TYPE_INT },
    { "samplerate",         mfi_offsetof(samplerate),         DB_TYPE_INT },
    { "song_length",        mfi_offsetof(song_length),        DB_TYPE_INT },
    { "file_size",          mfi_offsetof(file_size),          DB_TYPE_INT64 },
    { "year",               mfi_offsetof(year),               DB_TYPE_INT },
    { "date_released",      mfi_offsetof(date_released),      DB_TYPE_INT64 },
    { "track",              mfi_offsetof(track),              DB_TYPE_INT },
    { "total_tracks",       mfi_offsetof(total_tracks),       DB_TYPE_INT },
    { "disc",               mfi_offsetof(disc),               DB_TYPE_INT },
    { "total_discs",        mfi_offsetof(total_discs),        DB_TYPE_INT },
    { "bpm",                mfi_offsetof(bpm),                DB_TYPE_INT },
    { "compilation",        mfi_offsetof(compilation),        DB_TYPE_INT },
    { "artwork",            mfi_offsetof(artwork),            DB_TYPE_INT },
    { "rating",             mfi_offsetof(rating),             DB_TYPE_INT,    DB_FIXUP_STANDARD, DB_FLAG_NO_ZERO },
    { "play_count",         mfi_offsetof(play_count),         DB_TYPE_INT,    DB_FIXUP_STANDARD, DB_FLAG_NO_ZERO },
    { "skip_count",         mfi_offsetof(skip_count),         DB_TYPE_INT,    DB_FIXUP_STANDARD, DB_FLAG_NO_ZERO },
    { "seek",               mfi_offsetof(seek),               DB_TYPE_INT,    DB_FIXUP_STANDARD, DB_FLAG_NO_ZERO },
    { "data_kind",          mfi_offsetof(data_kind),          DB_TYPE_INT },
    { "media_kind",         mfi_offsetof(media_kind),         DB_TYPE_INT,    DB_FIXUP_MEDIA_KIND },
    { "item_kind",          mfi_offsetof(item_kind),          DB_TYPE_INT,    DB_FIXUP_ITEM_KIND },
    { "description",        mfi_offsetof(description),        DB_TYPE_STRING },
    { "db_timestamp",       mfi_offsetof(db_timestamp),       DB_TYPE_INT },
    { "time_added",         mfi_offsetof(time_added),         DB_TYPE_INT,    DB_FIXUP_STANDARD, DB_FLAG_NO_ZERO },
    { "time_modified",      mfi_offsetof(time_modified),      DB_TYPE_INT,    DB_FIXUP_TIME_MODIFIED },
    { "time_played",        mfi_offsetof(time_played),        DB_TYPE_INT,    DB_FIXUP_STANDARD, DB_FLAG_NO_ZERO },
    { "time_skipped",       mfi_offsetof(time_skipped),       DB_TYPE_INT,    DB_FIXUP_STANDARD, DB_FLAG_NO_ZERO },
    { "disabled",           mfi_offsetof(disabled),           DB_TYPE_INT64 },
    { "sample_count",       mfi_offsetof(sample_count),       DB_TYPE_INT64 },
    { "codectype",          mfi_offsetof(codectype),          DB_TYPE_STRING, DB_FIXUP_CODECTYPE },
    { "idx",                mfi_offsetof(idx),                DB_TYPE_INT },
    { "has_video",          mfi_offsetof(has_video),          DB_TYPE_INT },
    { "contentrating",      mfi_offsetof(contentrating),      DB_TYPE_INT },
    { "bits_per_sample",    mfi_offsetof(bits_per_sample),    DB_TYPE_INT },
    { "tv_series_name",     mfi_offsetof(tv_series_name),     DB_TYPE_STRING },
    { "tv_episode_num_str", mfi_offsetof(tv_episode_num_str), DB_TYPE_STRING },
    { "tv_network_name",    mfi_offsetof(tv_network_name),    DB_TYPE_STRING },
    { "tv_episode_sort",    mfi_offsetof(tv_episode_sort),    DB_TYPE_INT },
    { "tv_season_num",      mfi_offsetof(tv_season_num),      DB_TYPE_INT },
    { "songartistid",       mfi_offsetof(songartistid),       DB_TYPE_INT64,  DB_FIXUP_SONGARTISTID },
    { "songalbumid",        mfi_offsetof(songalbumid),        DB_TYPE_INT64,  DB_FIXUP_SONGALBUMID },
    { "title_sort",         mfi_offsetof(title_sort),         DB_TYPE_STRING, DB_FIXUP_TITLE_SORT },
    { "artist_sort",        mfi_offsetof(artist_sort),        DB_TYPE_STRING, DB_FIXUP_ARTIST_SORT },
    { "album_sort",         mfi_offsetof(album_sort),         DB_TYPE_STRING, DB_FIXUP_ALBUM_SORT },
    { "album_artist_sort",  mfi_offsetof(album_artist_sort),  DB_TYPE_STRING, DB_FIXUP_ALBUM_ARTIST_SORT },
    { "composer_sort",      mfi_offsetof(composer_sort),      DB_TYPE_STRING, DB_FIXUP_COMPOSER_SORT },
    { "channels",           mfi_offsetof(channels),           DB_TYPE_INT },
    { "usermark",           mfi_offsetof(usermark),           DB_TYPE_INT },
    { "scan_kind",          mfi_offsetof(scan_kind),          DB_TYPE_INT },
    { "lyrics",             mfi_offsetof(lyrics),             DB_TYPE_STRING },
  };

/* This list must be kept in sync with
 * - the order of the columns in the playlists table
 * - the type and name of the fields in struct playlist_info
 */
static const struct col_type_map pli_cols_map[] =
  {
    { "id",                 pli_offsetof(id),                 DB_TYPE_INT,    DB_FIXUP_STANDARD, DB_FLAG_NO_BIND },
    { "title",              pli_offsetof(title),              DB_TYPE_STRING, DB_FIXUP_TITLE },
    { "type",               pli_offsetof(type),               DB_TYPE_INT },
    { "query",              pli_offsetof(query),              DB_TYPE_STRING, DB_FIXUP_NO_SANITIZE },
    { "db_timestamp",       pli_offsetof(db_timestamp),       DB_TYPE_INT },
    { "disabled",           pli_offsetof(disabled),           DB_TYPE_INT64 },
    { "path",               pli_offsetof(path),               DB_TYPE_STRING, DB_FIXUP_NO_SANITIZE },
    { "idx",                pli_offsetof(index),              DB_TYPE_INT },
    { "special_id",         pli_offsetof(special_id),         DB_TYPE_INT },
    { "virtual_path",       pli_offsetof(virtual_path),       DB_TYPE_STRING, DB_FIXUP_NO_SANITIZE },
    { "parent_id",          pli_offsetof(parent_id),          DB_TYPE_INT },
    { "directory_id",       pli_offsetof(directory_id),       DB_TYPE_INT },
    { "query_order",        pli_offsetof(query_order),        DB_TYPE_STRING, DB_FIXUP_NO_SANITIZE },
    { "query_limit",        pli_offsetof(query_limit),        DB_TYPE_INT },
    { "media_kind",         pli_offsetof(media_kind),         DB_TYPE_INT,    DB_FIXUP_MEDIA_KIND },
    { "artwork_url",        pli_offsetof(artwork_url),        DB_TYPE_STRING, DB_FIXUP_NO_SANITIZE },
    { "scan_kind",          pli_offsetof(scan_kind),          DB_TYPE_INT },

    // Not in the database, but returned via the query's COUNT()/SUM()
    { "items",              pli_offsetof(items),              DB_TYPE_INT,    DB_FIXUP_STANDARD, DB_FLAG_NO_BIND },
    { "streams",            pli_offsetof(streams),            DB_TYPE_INT,    DB_FIXUP_STANDARD, DB_FLAG_NO_BIND },
  };

/* This list must be kept in sync with
 * - the order of the columns in the queue table
 * - the type and name of the fields in struct db_queue_item
 * - with qi_mfi_map
 */
static const struct col_type_map qi_cols_map[] =
  {
    { "id",                 qi_offsetof(id),                  DB_TYPE_INT,    DB_FIXUP_STANDARD, DB_FLAG_NO_BIND },
    { "file_id",            qi_offsetof(file_id),             DB_TYPE_INT },
    { "pos",                qi_offsetof(pos),                 DB_TYPE_INT },
    { "shuffle_pos",        qi_offsetof(shuffle_pos),         DB_TYPE_INT },
    { "data_kind",          qi_offsetof(data_kind),           DB_TYPE_INT },
    { "media_kind",         qi_offsetof(media_kind),          DB_TYPE_INT,    DB_FIXUP_MEDIA_KIND },
    { "song_length",        qi_offsetof(song_length),         DB_TYPE_INT },
    { "path",               qi_offsetof(path),                DB_TYPE_STRING, DB_FIXUP_NO_SANITIZE },
    { "virtual_path",       qi_offsetof(virtual_path),        DB_TYPE_STRING, DB_FIXUP_NO_SANITIZE },
    { "title",              qi_offsetof(title),               DB_TYPE_STRING, DB_FIXUP_TITLE },
    { "artist",             qi_offsetof(artist),              DB_TYPE_STRING, DB_FIXUP_ARTIST },
    { "album_artist",       qi_offsetof(album_artist),        DB_TYPE_STRING, DB_FIXUP_ALBUM_ARTIST },
    { "album",              qi_offsetof(album),               DB_TYPE_STRING, DB_FIXUP_ALBUM },
    { "genre",              qi_offsetof(genre),               DB_TYPE_STRING, DB_FIXUP_GENRE },
    { "songalbumid",        qi_offsetof(songalbumid),         DB_TYPE_INT64 },
    { "time_modified",      qi_offsetof(time_modified),       DB_TYPE_INT },
    { "artist_sort",        qi_offsetof(artist_sort),         DB_TYPE_STRING, DB_FIXUP_ARTIST_SORT },
    { "album_sort",         qi_offsetof(album_sort),          DB_TYPE_STRING, DB_FIXUP_ALBUM_SORT },
    { "album_artist_sort",  qi_offsetof(album_artist_sort),   DB_TYPE_STRING, DB_FIXUP_ALBUM_ARTIST_SORT },
    { "year",               qi_offsetof(year),                DB_TYPE_INT },
    { "track",              qi_offsetof(track),               DB_TYPE_INT },
    { "disc",               qi_offsetof(disc),                DB_TYPE_INT },
    { "artwork_url",        qi_offsetof(artwork_url),         DB_TYPE_STRING, DB_FIXUP_NO_SANITIZE },
    { "queue_version",      qi_offsetof(queue_version),       DB_TYPE_INT },
    { "composer",           qi_offsetof(composer),            DB_TYPE_STRING, DB_FIXUP_COMPOSER },
    { "songartistid",       qi_offsetof(songartistid),        DB_TYPE_INT64 },
    { "type",               qi_offsetof(type),                DB_TYPE_STRING, DB_FIXUP_CODECTYPE },
    { "bitrate",            qi_offsetof(bitrate),             DB_TYPE_INT },
    { "samplerate",         qi_offsetof(samplerate),          DB_TYPE_INT },
    { "channels",           qi_offsetof(channels),            DB_TYPE_INT },
  };

/* This list must be kept in sync with
 * - the order of the columns in the files table
 * - the name of the fields in struct db_media_file_info
 */
static const ssize_t dbmfi_cols_map[] =
  {
    dbmfi_offsetof(id),
    dbmfi_offsetof(path),
    dbmfi_offsetof(virtual_path),
    dbmfi_offsetof(fname),
    dbmfi_offsetof(directory_id),
    dbmfi_offsetof(title),
    dbmfi_offsetof(artist),
    dbmfi_offsetof(album),
    dbmfi_offsetof(album_artist),
    dbmfi_offsetof(genre),
    dbmfi_offsetof(comment),
    dbmfi_offsetof(type),
    dbmfi_offsetof(composer),
    dbmfi_offsetof(orchestra),
    dbmfi_offsetof(conductor),
    dbmfi_offsetof(grouping),
    dbmfi_offsetof(url),
    dbmfi_offsetof(bitrate),
    dbmfi_offsetof(samplerate),
    dbmfi_offsetof(song_length),
    dbmfi_offsetof(file_size),
    dbmfi_offsetof(year),
    dbmfi_offsetof(date_released),
    dbmfi_offsetof(track),
    dbmfi_offsetof(total_tracks),
    dbmfi_offsetof(disc),
    dbmfi_offsetof(total_discs),
    dbmfi_offsetof(bpm),
    dbmfi_offsetof(compilation),
    dbmfi_offsetof(artwork),
    dbmfi_offsetof(rating),
    dbmfi_offsetof(play_count),
    dbmfi_offsetof(skip_count),
    dbmfi_offsetof(seek),
    dbmfi_offsetof(data_kind),
    dbmfi_offsetof(media_kind),
    dbmfi_offsetof(item_kind),
    dbmfi_offsetof(description),
    dbmfi_offsetof(db_timestamp),
    dbmfi_offsetof(time_added),
    dbmfi_offsetof(time_modified),
    dbmfi_offsetof(time_played),
    dbmfi_offsetof(time_skipped),
    dbmfi_offsetof(disabled),
    dbmfi_offsetof(sample_count),
    dbmfi_offsetof(codectype),
    dbmfi_offsetof(idx),
    dbmfi_offsetof(has_video),
    dbmfi_offsetof(contentrating),
    dbmfi_offsetof(bits_per_sample),
    dbmfi_offsetof(tv_series_name),
    dbmfi_offsetof(tv_episode_num_str),
    dbmfi_offsetof(tv_network_name),
    dbmfi_offsetof(tv_episode_sort),
    dbmfi_offsetof(tv_season_num),
    dbmfi_offsetof(songartistid),
    dbmfi_offsetof(songalbumid),
    dbmfi_offsetof(title_sort),
    dbmfi_offsetof(artist_sort),
    dbmfi_offsetof(album_sort),
    dbmfi_offsetof(album_artist_sort),
    dbmfi_offsetof(composer_sort),
    dbmfi_offsetof(channels),
    dbmfi_offsetof(usermark),
    dbmfi_offsetof(scan_kind),
    dbmfi_offsetof(lyrics),
  };

/* This list must be kept in sync with
 * - the order of the columns in the playlists table
 * - the name of the fields in struct playlist_info
 */
static const ssize_t dbpli_cols_map[] =
  {
    dbpli_offsetof(id),
    dbpli_offsetof(title),
    dbpli_offsetof(type),
    dbpli_offsetof(query),
    dbpli_offsetof(db_timestamp),
    dbpli_offsetof(disabled),
    dbpli_offsetof(path),
    dbpli_offsetof(index),
    dbpli_offsetof(special_id),
    dbpli_offsetof(virtual_path),
    dbpli_offsetof(parent_id),
    dbpli_offsetof(directory_id),
    dbpli_offsetof(query_order),
    dbpli_offsetof(query_limit),
    dbpli_offsetof(media_kind),
    dbpli_offsetof(artwork_url),
    dbpli_offsetof(scan_kind),

    dbpli_offsetof(items),
    dbpli_offsetof(streams),
  };

/* This list must be kept in sync with
 * - the order of fields in the Q_GROUP_ALBUMS and Q_GROUP_ARTISTS query
 * - the name of the fields in struct group_info
 */
static const ssize_t dbgri_cols_map[] =
  {
    dbgri_offsetof(id),
    dbgri_offsetof(persistentid),
    dbgri_offsetof(itemname),
    dbgri_offsetof(itemname_sort),
    dbgri_offsetof(itemcount),
    dbgri_offsetof(groupalbumcount),
    dbgri_offsetof(songalbumartist),
    dbgri_offsetof(songartistid),
    dbgri_offsetof(song_length),
    dbgri_offsetof(data_kind),
    dbgri_offsetof(media_kind),
    dbgri_offsetof(year),
    dbgri_offsetof(date_released),
    dbgri_offsetof(time_added),
    dbgri_offsetof(time_played),
    dbgri_offsetof(seek),
  };

/* This list must be kept in sync with
 * - the order of fields in the Q_BROWSE_INFO query
 * - the name of the fields in struct db_browse_info
 */
static const ssize_t dbbi_cols_map[] =
  {
    dbbi_offsetof(itemname),
    dbbi_offsetof(itemname_sort),
    dbbi_offsetof(track_count),
    dbbi_offsetof(album_count),
    dbbi_offsetof(artist_count),
    dbbi_offsetof(song_length),
    dbbi_offsetof(data_kind),
    dbbi_offsetof(media_kind),
    dbbi_offsetof(year),
    dbbi_offsetof(date_released),
    dbbi_offsetof(time_added),
    dbbi_offsetof(time_played),
    dbbi_offsetof(seek),
  };

/* This list must be kept in sync with
 * - qi_cols_map
 */
static const struct qi_mfi_map qi_mfi_map[] =
  {
    { qi_offsetof(id),                  -1,                                -1 },
    { qi_offsetof(file_id),             mfi_offsetof(id),                  dbmfi_offsetof(id) },
    { qi_offsetof(pos),                 -1,                                -1 },
    { qi_offsetof(shuffle_pos),         -1,                                -1 },
    { qi_offsetof(data_kind),           mfi_offsetof(data_kind),           dbmfi_offsetof(data_kind) },
    { qi_offsetof(media_kind),          mfi_offsetof(media_kind),          dbmfi_offsetof(media_kind) },
    { qi_offsetof(song_length),         mfi_offsetof(song_length),         dbmfi_offsetof(song_length) },
    { qi_offsetof(path),                mfi_offsetof(path),                dbmfi_offsetof(path) },
    { qi_offsetof(virtual_path),        mfi_offsetof(virtual_path),        dbmfi_offsetof(virtual_path) },
    { qi_offsetof(title),               mfi_offsetof(title),               dbmfi_offsetof(title) },
    { qi_offsetof(artist),              mfi_offsetof(artist),              dbmfi_offsetof(artist) },
    { qi_offsetof(album_artist),        mfi_offsetof(album_artist),        dbmfi_offsetof(album_artist) },
    { qi_offsetof(album),               mfi_offsetof(album),               dbmfi_offsetof(album) },
    { qi_offsetof(genre),               mfi_offsetof(genre),               dbmfi_offsetof(genre) },
    { qi_offsetof(songalbumid),         mfi_offsetof(songalbumid),         dbmfi_offsetof(songalbumid) },
    { qi_offsetof(time_modified),       mfi_offsetof(time_modified),       dbmfi_offsetof(time_modified) },
    { qi_offsetof(artist_sort),         mfi_offsetof(artist_sort),         dbmfi_offsetof(artist_sort) },
    { qi_offsetof(album_sort),          mfi_offsetof(album_sort),          dbmfi_offsetof(album_sort) },
    { qi_offsetof(album_artist_sort),   mfi_offsetof(album_artist_sort),   dbmfi_offsetof(album_artist_sort) },
    { qi_offsetof(year),                mfi_offsetof(year),                dbmfi_offsetof(year) },
    { qi_offsetof(track),               mfi_offsetof(track),               dbmfi_offsetof(track) },
    { qi_offsetof(disc),                mfi_offsetof(disc),                dbmfi_offsetof(disc) },
    { qi_offsetof(artwork_url),         -1,                                -1 },
    { qi_offsetof(queue_version),       -1,                                -1 },
    { qi_offsetof(composer),            mfi_offsetof(composer),            dbmfi_offsetof(composer) },
    { qi_offsetof(songartistid),        mfi_offsetof(songartistid),        dbmfi_offsetof(songartistid) },
    { qi_offsetof(type),                mfi_offsetof(type),                dbmfi_offsetof(type) },
    { qi_offsetof(bitrate),             mfi_offsetof(bitrate),             dbmfi_offsetof(bitrate) },
    { qi_offsetof(samplerate),          mfi_offsetof(samplerate),          dbmfi_offsetof(samplerate) },
    { qi_offsetof(channels),            mfi_offsetof(channels),            dbmfi_offsetof(channels) },
  };

/* This list must be kept in sync with
 * - the order of the columns in the inotify table
 * - the name and type of the fields in struct watch_info
 */
static const struct col_type_map wi_cols_map[] =
  {
    { "wd",          wi_offsetof(wd),     DB_TYPE_INT, DB_FLAG_NO_BIND },
    { "cookie",      wi_offsetof(cookie), DB_TYPE_INT },
    { "path",        wi_offsetof(path),   DB_TYPE_STRING },
  };

/* Sort clauses, used for ORDER BY */
/* Keep in sync with enum sort_type and indices */
static const char *sort_clause[] =
  {
    "",
    "f.title_sort",
    "f.album_sort, f.disc, f.track",
    "f.album_artist_sort, f.album_sort, f.disc, f.track",
    "f.type, f.parent_id, f.special_id, f.title",
    "f.year",
    "f.genre",
    "f.composer_sort",
    "f.disc",
    "f.track",
    "f.virtual_path COLLATE NOCASE",
    "pos",
    "shuffle_pos",
    "f.date_released DESC, f.title_sort DESC",
  };

/* Browse clauses, used for SELECT, WHERE, GROUP BY and for default ORDER BY
 * Keep in sync with enum query_type and indices
 * Col 1: for SELECT, Col 2: for WHERE, Col 3: for GROUP BY/ORDER BY
 */
static const struct browse_clause browse_clause[] =
  {
    { "",                                      "",                 "" },
    { "f.album_artist, f.album_artist_sort",   "f.album_artist",   "f.album_artist_sort, f.album_artist" },
    { "f.album, f.album_sort",                 "f.album",          "f.album_sort, f.album" },
    { "f.genre, f.genre",                      "f.genre",          "f.genre" },
    { "f.composer, f.composer_sort",           "f.composer",       "f.composer_sort, f.composer" },
    { "f.year, f.year",                        "f.year",           "f.year" },
    { "f.disc, f.disc",                        "f.disc",           "f.disc" },
    { "f.track, f.track",                      "f.track",          "f.track" },
    { "f.virtual_path, f.virtual_path",        "f.virtual_path",   "f.virtual_path" },
    { "f.path, f.path",                        "f.path",           "f.path" },
  };


struct enum_label {
  int type;
  const char *label;
};


/* Keep in sync with enum media_kind */
static const struct enum_label media_kind_labels[] =
  {
    { MEDIA_KIND_MUSIC,      "music" },
    { MEDIA_KIND_MOVIE,      "movie" },
    { MEDIA_KIND_PODCAST,    "podcast" },
    { MEDIA_KIND_AUDIOBOOK,  "audiobook" },
    { MEDIA_KIND_MUSICVIDEO, "musicvideo" },
    { MEDIA_KIND_TVSHOW,     "tvshow" },
  };

const char *
db_media_kind_label(enum media_kind media_kind)
{
  int i;

  for (i = 0; i < ARRAY_SIZE(media_kind_labels); i++)
    {
      if (media_kind == media_kind_labels[i].type)
	return media_kind_labels[i].label;
    }

  return NULL;
}

enum media_kind
db_media_kind_enum(const char *label)
{
  int i;

  if (!label)
    return 0;

  for (i = 0; i < ARRAY_SIZE(media_kind_labels); i++)
    {
      if (strcmp(label, media_kind_labels[i].label) == 0)
	return media_kind_labels[i].type;
    }

  return 0;
}

/* Keep in sync with enum data_kind */
static char *data_kind_label[] = { "file", "url", "spotify", "pipe" };

const char *
db_data_kind_label(enum data_kind data_kind)
{
  if (data_kind < ARRAY_SIZE(data_kind_label))
    {
      return data_kind_label[data_kind];
    }

  return NULL;
}

/* Keep in sync with enum scan_kind */
static const struct enum_label scan_kind_labels[] =
  {
    { SCAN_KIND_UNKNOWN,    "unknown" },
    { SCAN_KIND_FILES,      "files" },
    { SCAN_KIND_SPOTIFY,    "spotify" },
    { SCAN_KIND_RSS,        "rss" },
  };

const char *
db_scan_kind_label(enum scan_kind scan_kind)
{
  if (scan_kind < ARRAY_SIZE(scan_kind_labels))
    {
      return scan_kind_labels[scan_kind].label;
    }

  return NULL;
}

enum scan_kind
db_scan_kind_enum(const char *name)
{
  int i;

  if (!name)
    return 0;

  for (i = 0; i < ARRAY_SIZE(scan_kind_labels); i++)
    {
      if (strcmp(name, scan_kind_labels[i].label) == 0)
	return scan_kind_labels[i].type;
    }

  return 0;
}

/* Keep in sync with enum pl_type */
static char *pl_type_label[] = { "special", "folder", "smart", "plain", "rss" };

const char *
db_pl_type_label(enum pl_type pl_type)
{
  if (pl_type < ARRAY_SIZE(pl_type_label))
    {
      return pl_type_label[pl_type];
    }

  return NULL;
}

/* Shuffle RNG state */
struct rng_ctx shuffle_rng;

static char *db_path;
static char *db_sqlite_ext_path;
static bool db_rating_updates;

static __thread sqlite3 *hdl;
static __thread struct db_statements db_statements;


/* Forward */
static enum group_type
db_group_type_bypersistentid(int64_t persistentid);

static int
db_query_run(char *query, int free, short update_events);


char *
db_escape_string(const char *str)
{
  char *escaped;
  char *ret;

  escaped = sqlite3_mprintf("%q", str);
  if (!escaped)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for escaped string\n");

      return NULL;
    }

  ret = strdup(escaped);

  sqlite3_free(escaped);

  return ret;
}

// Basically a wrapper for sqlite3_mprintf()
char *
db_mprintf(const char *fmt, ...)
{
  char *query;
  char *ret;
  va_list va;

  va_start(va, fmt);
  ret = sqlite3_vmprintf(fmt, va);
  if (!ret)
    {
      DPRINTF(E_FATAL, L_MISC, "Out of memory for db_mprintf\n");
      abort();
    }
  va_end(va);

  query = strdup(ret);
  sqlite3_free(ret);

  return query;
}

int
db_snprintf(char *s, int n, const char *fmt, ...)
{
  char *ret;
  va_list va;

  if (n < 2)
    return -1;

  // For size check since sqlite3_vsnprintf does not seem to support it
  s[n - 2] = '\0';

  va_start(va, fmt);
  ret = sqlite3_vsnprintf(n, s, fmt, va);
  va_end(va);

  if (!ret || (s[n - 2] != '\0'))
    return -1;

  return 0;
}

void
free_pi(struct pairing_info *pi, int content_only)
{
  if (!pi)
    return;

  free(pi->remote_id);
  free(pi->name);
  free(pi->guid);

  if (!content_only)
    free(pi);
  else
    memset(pi, 0, sizeof(struct pairing_info));
}

void
free_mfi(struct media_file_info *mfi, int content_only)
{
  if (!mfi)
    return;

  free(mfi->path);
  free(mfi->fname);
  free(mfi->title);
  free(mfi->artist);
  free(mfi->album);
  free(mfi->genre);
  free(mfi->comment);
  free(mfi->type);
  free(mfi->composer);
  free(mfi->orchestra);
  free(mfi->conductor);
  free(mfi->grouping);
  free(mfi->url);
  free(mfi->description);
  free(mfi->codectype);
  free(mfi->album_artist);
  free(mfi->tv_series_name);
  free(mfi->tv_episode_num_str);
  free(mfi->tv_network_name);
  free(mfi->title_sort);
  free(mfi->artist_sort);
  free(mfi->album_sort);
  free(mfi->composer_sort);
  free(mfi->album_artist_sort);
  free(mfi->virtual_path);
  free(mfi->lyrics);

  if (!content_only)
    free(mfi);
  else
    memset(mfi, 0, sizeof(struct media_file_info));
}

void
free_pli(struct playlist_info *pli, int content_only)
{
  if (!pli)
    return;

  free(pli->title);
  free(pli->query);
  free(pli->path);
  free(pli->virtual_path);
  free(pli->query_order);
  free(pli->artwork_url);

  if (!content_only)
    free(pli);
  else
    memset(pli, 0, sizeof(struct playlist_info));
}

void
free_di(struct directory_info *di, int content_only)
{
  if (!di)
    return;

  free(di->path);
  free(di->virtual_path);

  if (!content_only)
    free(di);
  else
    memset(di, 0, sizeof(struct directory_info));
}

void
free_wi(struct watch_info *wi, int content_only)
{
  if (!wi)
    return;

  free(wi->path);

  if (!content_only)
    free(wi);
  else
    memset(wi, 0, sizeof(struct watch_info));
}

void
free_query_params(struct query_params *qp, int content_only)
{
  if (!qp)
    return;

  free(qp->filter);
  free(qp->having);
  free(qp->order);
  free(qp->group);

  if (!content_only)
    free(qp);
  else
    memset(qp, 0, sizeof(struct query_params));
}

void
free_queue_item(struct db_queue_item *qi, int content_only)
{
  if (!qi)
    return;

  free(qi->path);
  free(qi->virtual_path);
  free(qi->title);
  free(qi->artist);
  free(qi->album_artist);
  free(qi->composer);
  free(qi->album);
  free(qi->genre);
  free(qi->artist_sort);
  free(qi->album_sort);
  free(qi->album_artist_sort);
  free(qi->artwork_url);
  free(qi->type);

  if (!content_only)
    free(qi);
  else
    memset(qi, 0, sizeof(struct db_queue_item));
}

// Utility function for stepping through mfi/pli/qi structs and setting the
// values from some source, which could be another struct. Some examples:
// - src is a dbmfi, dst is a qi, dst_type is DB_TYPE_INT, caller wants to own
//   qi so must_strdup is true, dbmfi has strings so parse_integers is true
// - src is a **char, dst is a qi, dst_type is DB_TYPE_STRING, src_offset is 0
//   because src is not a struct, caller wants to strdup the string so
//   must_strdup is true
static inline void
struct_field_set_str(void *dst, const void *src, bool must_strdup)
{
  char *srcstr;
  char **dstptr;

  srcstr = *(char **)(src);
  dstptr = (char **)(dst);
  *dstptr = must_strdup ? safe_strdup(srcstr) : srcstr;
}

static inline void
struct_field_set_uint32(void *dst, const void *src, bool parse_integers)
{
  char *srcstr;
  uint32_t srcu32val;
  int ret;

  if (parse_integers)
    {
      srcstr = *(char **)(src);
      ret = safe_atou32(srcstr, &srcu32val);
      if (ret < 0)
	srcu32val = 0;
    }
  else
    srcu32val = *(uint32_t *)(src);

  memcpy(dst, &srcu32val, sizeof(srcu32val));
}

static inline void
struct_field_set_int64(void *dst, const void *src, bool parse_integers)
{
  char *srcstr;
  int64_t srci64val;
  int ret;

  if (parse_integers)
    {
      srcstr = *(char **)(src);
      ret = safe_atoi64(srcstr, &srci64val);
      if (ret < 0)
	srci64val = 0;
    }
  else
    srci64val = *(int64_t *)(src);

  memcpy(dst, &srci64val, sizeof(srci64val));
}

static void
struct_field_from_field(void *dst_struct, ssize_t dst_offset, enum field_type dst_type, const void *src_struct, ssize_t src_offset, bool must_strdup, bool parse_integers)
{
  switch (dst_type)
    {
      case DB_TYPE_STRING:
	struct_field_set_str(dst_struct + dst_offset, src_struct + src_offset, must_strdup);
	break;

      case DB_TYPE_INT:
	struct_field_set_uint32(dst_struct + dst_offset, src_struct + src_offset, parse_integers);
	break;

      case DB_TYPE_INT64:
	struct_field_set_int64(dst_struct + dst_offset, src_struct + src_offset, parse_integers);
	break;
    }
}

static void
struct_field_from_statement(void *dst_struct, ssize_t dst_offset, enum field_type dst_type, sqlite3_stmt *stmt, int col, bool must_strdup, bool parse_integers)
{
  const char *str;
  uint32_t u32;
  int64_t i64;

  switch (dst_type)
    {
      case DB_TYPE_STRING:
        str = (const char *)sqlite3_column_text(stmt, col);
	struct_field_set_str(dst_struct + dst_offset, &str, must_strdup);
	break;

      case DB_TYPE_INT:
        u32 = (uint32_t)sqlite3_column_int64(stmt, col); // _int64() because _int() wouldn't be enough for uint32
	struct_field_set_uint32(dst_struct + dst_offset, &u32, parse_integers);
	break;

      case DB_TYPE_INT64:
        i64 = sqlite3_column_int64(stmt, col);
	struct_field_set_int64(dst_struct + dst_offset, &i64, parse_integers);
	break;
    }
}

static void
sort_tag_create(char **sort_tag, const char *src_tag)
{
  const uint8_t *i_ptr;
  const uint8_t *n_ptr;
  const uint8_t *number;
  uint8_t out[1024];
  uint8_t *o_ptr;
  int append_number;
  ucs4_t puc;
  int numlen;
  size_t len;
  int charlen;

  /* Note: include terminating NUL in string length for u8_normalize */

  // If the source provides a source tag then we rely on a normalized version of
  // that instead of creating our own (see issue #1257). FIXME this produces the
  // following bug:
  // - Track with no sort tags, assume queue_item->artist is "A", then
  //   queue_item->artist_sort will be created and set to "A".
  // - Update the artist name of the queue item with JSON API to "B".
  // - queue_item->artist_sort will still be "A".
  if (*sort_tag)
    {
      DPRINTF(E_DBG, L_DB, "Existing sort tag will be normalized: %s\n", *sort_tag);
      o_ptr = u8_normalize(UNINORM_NFD, (uint8_t *)*sort_tag, strlen(*sort_tag) + 1, NULL, &len);
      free(*sort_tag);
      *sort_tag = (char *)o_ptr;
      return;
    }

  if (!src_tag || ((len = strlen(src_tag)) == 0))
    {
      *sort_tag = NULL;
      return;
    }

  // Set input pointer past article if present and disregard certain special chars
  if ((strncasecmp(src_tag, "a ", 2) == 0) && (len > 2))
    i_ptr = (uint8_t *)(src_tag + 2);
  else if ((strncasecmp(src_tag, "an ", 3) == 0) && (len > 3))
    i_ptr = (uint8_t *)(src_tag + 3);
  else if ((strncasecmp(src_tag, "the ", 4) == 0) && (len > 4))
    i_ptr = (uint8_t *)(src_tag + 4);
  else if (strchr("[('\"", src_tag[0]) && (len > 1))
    i_ptr = (uint8_t *)(src_tag + 1);
  else
    i_ptr = (uint8_t *)src_tag;

  // Poor man's natural sort. Makes sure we sort like this: a1, a2, a10, a11, a21, a111
  // We do this by padding zeroes to (short) numbers. As an alternative we could have
  // made a proper natural sort algorithm in sqlext.c, but we don't, since we don't
  // want any risk of hurting response times
  memset(&out, 0, sizeof(out));
  o_ptr = (uint8_t *)&out;
  number = NULL;
  append_number = 0;

  do
    {
      n_ptr = u8_next(&puc, i_ptr);

      if (uc_is_digit(puc))
	{
	  if (!number) // We have encountered the beginning of a number
	    number = i_ptr;
	  append_number = (n_ptr == NULL); // If last char in string append number now
	}
      else
	{
	  if (number)
	    append_number = 1; // A number has ended so time to append it
	  else
	    {
              charlen = u8_strmblen(i_ptr);
              if (charlen >= 0)
	    	o_ptr = u8_stpncpy(o_ptr, i_ptr, charlen); // No numbers in sight, just append char
	    }
	}

      // Break if less than 100 bytes remain (prevent buffer overflow)
      if (sizeof(out) - u8_strlen(out) < 100)
	break;

      // Break if number is very large (prevent buffer overflow)
      if (number && (i_ptr - number > 50))
	break;

      if (append_number)
	{
	  numlen = i_ptr - number;
	  if (numlen < 5) // Max pad width
	    {
	      u8_strcpy(o_ptr, (uint8_t *)"00000");
	      o_ptr += (5 - numlen);
	    }
	  o_ptr = u8_stpncpy(o_ptr, number, numlen + u8_strmblen(i_ptr));

	  number = NULL;
	  append_number = 0;
	}

      i_ptr = n_ptr;
    }
  while (n_ptr);

  *sort_tag = (char *)u8_normalize(UNINORM_NFD, (uint8_t *)&out, u8_strlen(out) + 1, NULL, &len);
}

static void
fixup_sanitize(char **tag, enum fixup_type fixup, struct fixup_ctx *ctx)
{
  char *ret;

  if (!tag || !*tag)
    return;

  switch (fixup)
    {
      case DB_FIXUP_NO_SANITIZE:
      case DB_FIXUP_CODECTYPE:
	break; // Don't touch the above

      default:
	trim(*tag);

	// By default we set empty strings to NULL
	if (*tag[0] == '\0')
	  {
	    free(*tag);
	    *tag = NULL;
	    break;
	  }

	ret = unicode_fixup_string(*tag, "ascii");
	if (ret != *tag)
	  {
	    free(*tag);
	    *tag = ret;
	  }
    }
}

static void
fixup_defaults(char **tag, enum fixup_type fixup, struct fixup_ctx *ctx)
{
  char *ca;

  switch(fixup)
    {
      case DB_FIXUP_SONGARTISTID:
	if (ctx->mfi && ctx->mfi->songartistid == 0)
	  ctx->mfi->songartistid = two_str_hash(ctx->mfi->album_artist, NULL);
	break;

      case DB_FIXUP_SONGALBUMID:
	if (ctx->mfi && ctx->mfi->songalbumid == 0)
	  ctx->mfi->songalbumid = two_str_hash(ctx->mfi->album_artist, ctx->mfi->album) + ctx->mfi->data_kind;
	break;

      case DB_FIXUP_TITLE:
	if (*tag)
	  break;

	// fname is left untouched by fixup_sanitize() for obvious reasons, so ensure it is proper UTF-8
	if (ctx->mfi && ctx->mfi->fname)
	  {
	    *tag = unicode_fixup_string(ctx->mfi->fname, "ascii");
            if (*tag == ctx->mfi->fname)
	      *tag = strdup(ctx->mfi->fname);
	  }
	else if (ctx->pli && ctx->pli->path)
	  *tag = strdup(ctx->pli->path);
	else if (ctx->qi && ctx->qi->path)
	  *tag = strdup(ctx->qi->path);
	else
	  *tag = strdup(CFG_NAME_UNKNOWN_TITLE);
	break;

      case DB_FIXUP_ARTIST:
	if (*tag)
	  break;

	if (ctx->mfi && ctx->mfi->album_artist)
	  *tag = strdup(ctx->mfi->album_artist);
	else if (ctx->mfi && ctx->mfi->orchestra && ctx->mfi->conductor)
	  *tag = safe_asprintf("%s - %s", ctx->mfi->orchestra, ctx->mfi->conductor);
	else if (ctx->mfi && ctx->mfi->orchestra)
	  *tag = strdup(ctx->mfi->orchestra);
        else if (ctx->mfi && ctx->mfi->conductor)
	  *tag = strdup(ctx->mfi->conductor);
        else if (ctx->mfi && ctx->mfi->tv_series_name)
	  *tag = strdup(ctx->mfi->tv_series_name);
	else
	  *tag = strdup(CFG_NAME_UNKNOWN_ARTIST);
	break;

      case DB_FIXUP_ALBUM:
	if (*tag)
	  break;

	if (ctx->mfi && ctx->mfi->tv_series_name)
	  *tag = safe_asprintf("%s, Season %u", ctx->mfi->tv_series_name, ctx->mfi->tv_season_num);
	else
	  *tag = strdup(CFG_NAME_UNKNOWN_ALBUM);
	break;

      case DB_FIXUP_ALBUM_ARTIST: // Will be set after artist, because artist (must) come first in the col_maps
	if (ctx->mfi && ctx->mfi->media_kind == MEDIA_KIND_PODCAST)
	  {
	    free(*tag);
	    *tag = strdup("");
	  }

	if (ctx->mfi && ctx->mfi->compilation && (ca = cfg_getstr(cfg_getsec(cfg, "library"), "compilation_artist")))
	  {
	    free(*tag);
	    *tag = strdup(ca); // If ca is empty string then the artist will not be shown in artist view
	  }

	if (*tag)
	  break;

	if (ctx->mfi && ctx->mfi->artist)
	  *tag = strdup(ctx->mfi->artist);
	else if (ctx->qi && ctx->qi->artist)
	  *tag = strdup(ctx->qi->artist);
	else
	  *tag = strdup(CFG_NAME_UNKNOWN_ARTIST);
	break;

      case DB_FIXUP_GENRE:
	if (*tag)
	  break;

	*tag = strdup(CFG_NAME_UNKNOWN_GENRE);
	break;

      case DB_FIXUP_MEDIA_KIND:
	if (ctx->mfi && ctx->mfi->tv_series_name)
	  ctx->mfi->media_kind = MEDIA_KIND_TVSHOW;
	else if (ctx->mfi && !ctx->mfi->media_kind)
	  ctx->mfi->media_kind = MEDIA_KIND_MUSIC;
	else if (ctx->pli && !ctx->pli->media_kind)
	  ctx->pli->media_kind = MEDIA_KIND_MUSIC;
	else if (ctx->qi && !ctx->qi->media_kind)
	  ctx->qi->media_kind = MEDIA_KIND_MUSIC;

	break;

      case DB_FIXUP_ITEM_KIND:
	if (ctx->mfi && !ctx->mfi->item_kind)
	  ctx->mfi->item_kind = 2; // music

	break;

      case DB_FIXUP_TIME_MODIFIED:
	if (ctx->mfi && ctx->mfi->time_modified == 0)
	  ctx->mfi->time_modified = ctx->mfi->db_timestamp;
	break;

      case DB_FIXUP_CODECTYPE:
      case DB_FIXUP_TYPE:
	// Default to mpeg4 video/audio for unknown file types in an attempt to allow streaming of DRM-afflicted files
	if (ctx->mfi && ctx->mfi->codectype && strcmp(ctx->mfi->codectype, "unkn") == 0)
	  {
	    if (ctx->mfi->has_video)
	      {
		strcpy(ctx->mfi->codectype, "mp4v");
		strcpy(ctx->mfi->type, "m4v");
	      }
	    else
	      {
		strcpy(ctx->mfi->codectype, "mp4a");
		strcpy(ctx->mfi->type, "m4a");
	      }
	  }
	break;

      default:
	break;
    }
}

static void
fixup_sort_tags(char **tag, enum fixup_type fixup, struct fixup_ctx *ctx)
{
  switch(fixup)
    {
      case DB_FIXUP_TITLE_SORT:
	if (ctx->mfi)
	  sort_tag_create(tag, ctx->mfi->title);
	break;

      case DB_FIXUP_ARTIST_SORT:
	if (ctx->mfi)
	  sort_tag_create(tag, ctx->mfi->artist);
	else if (ctx->qi)
	  sort_tag_create(tag, ctx->qi->artist);
	break;

      case DB_FIXUP_ALBUM_SORT:
	if (ctx->mfi)
	  sort_tag_create(tag, ctx->mfi->album);
	else if (ctx->qi)
	  sort_tag_create(tag, ctx->qi->album);
	break;

      case DB_FIXUP_ALBUM_ARTIST_SORT:
	if (ctx->mfi)
	  sort_tag_create(tag, ctx->mfi->album_artist);
	else if (ctx->qi)
	  sort_tag_create(tag, ctx->qi->album_artist);
	break;

      case DB_FIXUP_COMPOSER_SORT:
	if (ctx->mfi)
	  sort_tag_create(tag, ctx->mfi->composer);
	break;

      default:
	break;
    }
}

static void
fixup_tags(struct fixup_ctx *ctx)
{
  void (*fixup_func[])(char **, enum fixup_type, struct fixup_ctx *) = { fixup_sanitize, fixup_defaults, fixup_sort_tags };
  char **tag;
  int i;
  int j;

  for (i = 0; i < ARRAY_SIZE(fixup_func); i++)
    {
      for (j = 0; j < ctx->map_size; j++)
	{
	  switch (ctx->map[j].type)
	    {
	      case DB_TYPE_STRING:
		tag = (char **) ((char *)ctx->data + ctx->map[j].offset);
		fixup_func[i](tag, ctx->map[j].fixup, ctx);
		break;

	      case DB_TYPE_INT:
	      case DB_TYPE_INT64:
		fixup_func[i](NULL, ctx->map[j].fixup, ctx);
		break;
	    }
	}
    }
}

static void
fixup_tags_mfi(struct media_file_info *mfi)
{
  struct fixup_ctx ctx = { 0 };
  ctx.data = mfi;
  ctx.mfi = mfi;
  ctx.map = mfi_cols_map;
  ctx.map_size = ARRAY_SIZE(mfi_cols_map);

  fixup_tags(&ctx);
}

static void
fixup_tags_pli(struct playlist_info *pli)
{
  struct fixup_ctx ctx = { 0 };
  ctx.data = pli;
  ctx.pli = pli;
  ctx.map = pli_cols_map;
  ctx.map_size = ARRAY_SIZE(pli_cols_map);

  fixup_tags(&ctx);
}

static void
fixup_tags_qi(struct db_queue_item *qi)
{
  struct fixup_ctx ctx = { 0 };
  ctx.data = qi;
  ctx.qi = qi;
  ctx.map = qi_cols_map;
  ctx.map_size = ARRAY_SIZE(qi_cols_map);

  fixup_tags(&ctx);
}

static int
bind_generic(sqlite3_stmt *stmt, void *data, const struct col_type_map *map, size_t map_size, int id)
{
  char **strptr;
  char *ptr;
  int i;
  int n;

  for (i = 0, n = 1; i < map_size; i++)
    {
      if (map[i].flag & DB_FLAG_NO_BIND)
	continue;

      ptr = data + map[i].offset;
      strptr = (char **)(data + map[i].offset);

      switch (map[i].type)
	{
	  case DB_TYPE_INT:
	    sqlite3_bind_int64(stmt, n, *((uint32_t *)ptr)); // Use _int64 because _int is for signed int32
	    break;

	  case DB_TYPE_INT64:
	    sqlite3_bind_int64(stmt, n, *((int64_t *)ptr));
	    break;

	  case DB_TYPE_STRING:
	    sqlite3_bind_text(stmt, n, *strptr, -1, SQLITE_STATIC); // TODO should we use _TRANSIENT?
	    break;

	  default:
	    DPRINTF(E_LOG, L_DB, "BUG: Unknown type %d in column map\n", map[i].type);
	    return -1;
	}

      n++;
    }

  // This binds the final "WHERE id = ?" if it is an update
  if (id)
    sqlite3_bind_int(stmt, n, id);

  return 0;
}

static int
bind_mfi(sqlite3_stmt *stmt, struct media_file_info *mfi)
{
  return bind_generic(stmt, mfi, mfi_cols_map, ARRAY_SIZE(mfi_cols_map), mfi->id);
}

static int
bind_pli(sqlite3_stmt *stmt, struct playlist_info *pli)
{
  return bind_generic(stmt, pli, pli_cols_map, ARRAY_SIZE(pli_cols_map), pli->id);
}

static int
bind_qi(sqlite3_stmt *stmt, struct db_queue_item *qi)
{
  return bind_generic(stmt, qi, qi_cols_map, ARRAY_SIZE(qi_cols_map), qi->id);
}

/* Unlock notification support */
static void
unlock_notify_cb(void **args, int nargs)
{
  struct db_unlock *u;
  int i;

  for (i = 0; i < nargs; i++)
    {
      u = (struct db_unlock *)args[i];

      DPRINTF(E_DBG, L_DB, "Notify DB unlock, thread: %s\n", u->thread_name_tid);
      CHECK_ERR(L_DB, pthread_mutex_lock(&u->lck));

      u->proceed = 1;
      CHECK_ERR(L_DB, pthread_cond_signal(&u->cond));

      CHECK_ERR(L_DB, pthread_mutex_unlock(&u->lck));
    }
}

static int
db_wait_unlock(void)
{
  struct db_unlock u;
  int ret;

  thread_getnametid(u.thread_name_tid, sizeof(u.thread_name_tid));

  u.proceed = 0;
  CHECK_ERR(L_DB, mutex_init(&u.lck));
  CHECK_ERR(L_DB, pthread_cond_init(&u.cond, NULL));

  ret = sqlite3_unlock_notify(hdl, unlock_notify_cb, &u);
  if (ret == SQLITE_OK)
    {
      CHECK_ERR(L_DB, pthread_mutex_lock(&u.lck));

      if (!u.proceed)
	{
	  DPRINTF(E_INFO, L_DB, "Waiting for database unlock\n");
	  CHECK_ERR(L_DB, pthread_cond_wait(&u.cond, &u.lck));
	}

      CHECK_ERR(L_DB, pthread_mutex_unlock(&u.lck));
}

  CHECK_ERR(L_DB, pthread_cond_destroy(&u.cond));
  CHECK_ERR(L_DB, pthread_mutex_destroy(&u.lck));

  return ret;
}

static int
db_blocking_step(sqlite3_stmt *stmt)
{
  int ret;

  while ((ret = sqlite3_step(stmt)) == SQLITE_LOCKED)
    {
      ret = db_wait_unlock();
      if (ret != SQLITE_OK)
	{
	  DPRINTF(E_LOG, L_DB, "Database deadlocked!\n");
	  break;
	}

      sqlite3_reset(stmt);
    }

  return ret;
}

static int
db_blocking_prepare_v2(const char *query, int len, sqlite3_stmt **stmt, const char **end)
{
  int ret;

  while ((ret = sqlite3_prepare_v2(hdl, query, len, stmt, end)) == SQLITE_LOCKED)
    {
      ret = db_wait_unlock();
      if (ret != SQLITE_OK)
	{
	  DPRINTF(E_LOG, L_DB, "Database deadlocked!\n");
	  break;
	}
    }

  return ret;
}

static int
db_statement_run(sqlite3_stmt *stmt, short update_events)
{
  int ret;
  int changes = 0;

#ifdef HAVE_SQLITE3_EXPANDED_SQL
  char *query;
  if (logger_severity() >= E_DBG)
    {
      query = sqlite3_expanded_sql(stmt);
      DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);
      sqlite3_free(query);
    }
#else
  DPRINTF(E_DBG, L_DB, "Running query (prepared statement)\n");
#endif

  while ((ret = db_blocking_step(stmt)) == SQLITE_ROW)
    ; /* EMPTY */

  if (ret != SQLITE_DONE)
    {
      DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));
    }

  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);

  changes = sqlite3_changes(hdl);
  if (update_events && changes > 0)
    library_update_trigger(update_events);

  return (ret == SQLITE_DONE) ? changes : -1;
}


/* Modelled after sqlite3_exec() */
static int
db_exec(const char *query, char **errmsg)
{
  sqlite3_stmt *stmt;
  int try;
  int ret;

  *errmsg = NULL;

  for (try = 0; try < 5; try++)
    {
      ret = db_blocking_prepare_v2(query, -1, &stmt, NULL);
      if (ret != SQLITE_OK)
	{
	  *errmsg = sqlite3_mprintf("prepare failed: %s", sqlite3_errmsg(hdl));
	  return ret;
	}

      while ((ret = db_blocking_step(stmt)) == SQLITE_ROW)
	; /* EMPTY */

      sqlite3_finalize(stmt);

      if (ret != SQLITE_SCHEMA)
	break;
    }

  if (ret != SQLITE_DONE)
    {
      *errmsg = sqlite3_mprintf("step failed: %s", sqlite3_errmsg(hdl));
      return ret;
    }

  return SQLITE_OK;
}


/* Maintenance and DB hygiene */
static void
db_pragma_optimize(void)
{
  const char *query = "ANALYZE;";
  char *errmsg;
  int ret;

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  ret = db_exec(query, &errmsg);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "ANALYZE failed: %s\n", errmsg);

      sqlite3_free(errmsg);
    }
}

/* Set names of default playlists according to config */
static void
db_set_cfg_names(void)
{
#define Q_TMPL "UPDATE playlists SET title = '%q' WHERE type = %d AND special_id = %d;"
  char *cfg_item[6] = { "name_library", "name_music", "name_movies", "name_tvshows", "name_podcasts", "name_audiobooks" };
  char special_id[6] = { 0, 6, 4, 5, 1, 7 };
  cfg_t *lib;
  char *query;
  char *title;
  char *errmsg;
  int ret;
  int i;

  lib = cfg_getsec(cfg, "library");

  for (i = 0; i < (sizeof(cfg_item) / sizeof(cfg_item[0])); i++)
    {
      title = cfg_getstr(lib, cfg_item[i]);
      if (!title)
	{
	  DPRINTF(E_LOG, L_DB, "Internal error, unknown config item '%s'\n", cfg_item[i]);

	  continue;
	}

      query = sqlite3_mprintf(Q_TMPL, title, PL_SPECIAL, special_id[i]);
      if (!query)
	{
	  DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

	  return;
	}

      ret = db_exec(query, &errmsg);
      if (ret != SQLITE_OK)
	{
	  DPRINTF(E_LOG, L_DB, "Error setting playlist title, query %s, error: %s\n", query, errmsg);

	  sqlite3_free(errmsg);
	}
      else
	DPRINTF(E_DBG, L_DB, "Playlist title for config item '%s' set with query '%s'\n", cfg_item[i], query);

      sqlite3_free(query);
    }
#undef Q_TMPL
}

void
db_hook_post_scan(void)
{
  DPRINTF(E_DBG, L_DB, "Running post-scan DB maintenance tasks...\n");

  db_pragma_optimize();

  DPRINTF(E_DBG, L_DB, "Done with post-scan DB maintenance\n");
}

void
db_purge_cruft(time_t ref)
{
#define Q_TMPL "DELETE FROM directories WHERE id >= %d AND db_timestamp < %" PRIi64 ";"
  int i;
  int ret;
  char *query;
  char *queries_tmpl[4] =
    {
      "DELETE FROM playlistitems WHERE playlistid IN (SELECT p.id FROM playlists p WHERE p.type <> %d AND p.db_timestamp < %" PRIi64 ");",
      "DELETE FROM playlistitems WHERE filepath IN (SELECT f.path FROM files f WHERE -1 <> %d AND f.db_timestamp < %" PRIi64 ");",
      "DELETE FROM playlists WHERE type <> %d AND db_timestamp < %" PRIi64 ";",
      "DELETE FROM files WHERE -1 <> %d AND db_timestamp < %" PRIi64 ";",
    };

  db_transaction_begin();

  for (i = 0; i < (sizeof(queries_tmpl) / sizeof(queries_tmpl[0])); i++)
    {
      query = sqlite3_mprintf(queries_tmpl[i], PL_SPECIAL, (int64_t)ref);
      if (!query)
	{
	  DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
	  db_transaction_end();
	  return;
	}

      DPRINTF(E_DBG, L_DB, "Running purge query '%s'\n", query);

      ret = db_query_run(query, 1, 0);
      if (ret == 0)
	DPRINTF(E_DBG, L_DB, "Purged %d rows\n", sqlite3_changes(hdl));
    }

  query = sqlite3_mprintf(Q_TMPL, DIR_MAX, (int64_t)ref);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
      db_transaction_end();
      return;
    }

  DPRINTF(E_DBG, L_DB, "Running purge query '%s'\n", query);

  ret = db_query_run(query, 1, LISTENER_DATABASE);
  if (ret == 0)
    DPRINTF(E_DBG, L_DB, "Purged %d rows\n", sqlite3_changes(hdl));

  db_transaction_end();

#undef Q_TMPL
}

void
db_purge_cruft_bysource(time_t ref, enum scan_kind scan_kind)
{
#define Q_TMPL "DELETE FROM directories WHERE id >= %d AND db_timestamp < %" PRIi64 " AND scan_kind = %d;"
  int i;
  int ret;
  char *query;
  char *queries_tmpl[4] =
    {
      "DELETE FROM playlistitems WHERE playlistid IN (SELECT p.id FROM playlists p WHERE p.type <> %d AND p.db_timestamp < %" PRIi64 " AND scan_kind = %d);",
      "DELETE FROM playlistitems WHERE filepath IN (SELECT f.path FROM files f WHERE -1 <> %d AND f.db_timestamp < %" PRIi64 " AND scan_kind = %d);",
      "DELETE FROM playlists WHERE type <> %d AND db_timestamp < %" PRIi64 " AND scan_kind = %d;",
      "DELETE FROM files WHERE -1 <> %d AND db_timestamp < %" PRIi64 " AND scan_kind = %d;",
    };

  db_transaction_begin();

  for (i = 0; i < (sizeof(queries_tmpl) / sizeof(queries_tmpl[0])); i++)
    {
      query = sqlite3_mprintf(queries_tmpl[i], PL_SPECIAL, (int64_t)ref, scan_kind);
      if (!query)
	{
	  DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
	  db_transaction_end();
	  return;
	}

      DPRINTF(E_DBG, L_DB, "Running purge query '%s'\n", query);

      ret = db_query_run(query, 1, 0);
      if (ret == 0)
	DPRINTF(E_DBG, L_DB, "Purged %d rows\n", sqlite3_changes(hdl));
    }

  query = sqlite3_mprintf(Q_TMPL, DIR_MAX, (int64_t)ref, scan_kind);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
      db_transaction_end();
      return;
    }

  DPRINTF(E_DBG, L_DB, "Running purge query '%s'\n", query);

  ret = db_query_run(query, 1, LISTENER_DATABASE);
  if (ret == 0)
    DPRINTF(E_DBG, L_DB, "Purged %d rows\n", sqlite3_changes(hdl));

  db_transaction_end();

#undef Q_TMPL
}

void
db_purge_all(void)
{
#define Q_TMPL_PL "DELETE FROM playlists WHERE type <> %d;"
#define Q_TMPL_DIR "DELETE FROM directories WHERE id >= %d;"
  char *queries[4] =
    {
      "DELETE FROM inotify;",
      "DELETE FROM playlistitems;",
      "DELETE FROM files;",
      "DELETE FROM groups;",
    };
  char *errmsg;
  char *query;
  int i;
  int ret;

  for (i = 0; i < (sizeof(queries) / sizeof(queries[0])); i++)
    {
      DPRINTF(E_DBG, L_DB, "Running purge query '%s'\n", queries[i]);

      ret = db_exec(queries[i], &errmsg);
      if (ret != SQLITE_OK)
	{
	  DPRINTF(E_LOG, L_DB, "Purge query %d error: %s\n", i, errmsg);

	  sqlite3_free(errmsg);
	}
      else
	DPRINTF(E_DBG, L_DB, "Purged %d rows\n", sqlite3_changes(hdl));
    }

  // Purge playlists
  query = sqlite3_mprintf(Q_TMPL_PL, PL_SPECIAL);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
      return;
    }

  DPRINTF(E_DBG, L_DB, "Running purge query '%s'\n", query);

  ret = db_exec(query, &errmsg);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Purge query '%s' error: %s\n", query, errmsg);

      sqlite3_free(errmsg);
    }
  else
    DPRINTF(E_DBG, L_DB, "Purged %d rows\n", sqlite3_changes(hdl));

  sqlite3_free(query);

  // Purge directories
  query = sqlite3_mprintf(Q_TMPL_DIR, DIR_MAX);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
      return;
    }

  DPRINTF(E_DBG, L_DB, "Running purge query '%s'\n", query);

  ret = db_exec(query, &errmsg);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Purge query '%s' error: %s\n", query, errmsg);

      sqlite3_free(errmsg);
    }
  else
    DPRINTF(E_DBG, L_DB, "Purged %d rows\n", sqlite3_changes(hdl));

  sqlite3_free(query);

#undef Q_TMPL_PL
#undef Q_TMPL_DIR
}

static int
db_get_one_int(const char *query)
{
  sqlite3_stmt *stmt;
  int ret;

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  ret = db_blocking_prepare_v2(query, -1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));
      return -1;
    }

  ret = db_blocking_step(stmt);
  if (ret != SQLITE_ROW)
    {
      if (ret == SQLITE_DONE)
	DPRINTF(E_INFO, L_DB, "No matching row found for query: %s\n", query);
      else
	DPRINTF(E_LOG, L_DB, "Could not step: %s (%s)\n", sqlite3_errmsg(hdl), query);

      sqlite3_finalize(stmt);
      return -1;
    }

  ret = sqlite3_column_int(stmt, 0);

#ifdef DB_PROFILE
  while (db_blocking_step(stmt) == SQLITE_ROW)
    ; /* EMPTY */
#endif

  sqlite3_finalize(stmt);

  return ret;
}


/* Transactions */
void
db_transaction_begin(void)
{
  char *query = "BEGIN TRANSACTION;";
  char *errmsg;
  int ret;

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  ret = db_exec(query, &errmsg);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "SQL error running '%s': %s\n", query, errmsg);

      sqlite3_free(errmsg);
    }
}

void
db_transaction_end(void)
{
  char *query = "END TRANSACTION;";
  char *errmsg;
  int ret;

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  ret = db_exec(query, &errmsg);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "SQL error running '%s': %s\n", query, errmsg);

      sqlite3_free(errmsg);
    }
}

void
db_transaction_rollback(void)
{
  char *query = "ROLLBACK TRANSACTION;";
  char *errmsg;
  int ret;

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  ret = db_exec(query, &errmsg);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "SQL error running '%s': %s\n", query, errmsg);

      sqlite3_free(errmsg);
    }
}

static void
db_free_query_clause(struct query_clause *qc)
{
  if (!qc)
    return;

  sqlite3_free(qc->where);
  sqlite3_free(qc->group);
  sqlite3_free(qc->having);
  sqlite3_free(qc->order);
  sqlite3_free(qc->index);
  free(qc);
}

// Builds the generic parts of the query. Parts that are specific to the query
// type are in db_build_query_* implementations.
static struct query_clause *
db_build_query_clause(struct query_params *qp)
{
  struct query_clause *qc;

  qc = calloc(1, sizeof(struct query_clause));
  if (!qc)
    goto error;

  if (qp->type & Q_F_BROWSE)
    qc->group = sqlite3_mprintf("GROUP BY %s", browse_clause[qp->type & ~Q_F_BROWSE].group);
  else if (qp->group)
    qc->group = sqlite3_mprintf("GROUP BY %s", qp->group);
  else
    qc->group = sqlite3_mprintf("");

  if (qp->filter && !qp->with_disabled)
    qc->where = sqlite3_mprintf("WHERE f.disabled = 0 AND %s", qp->filter);
  else if (!qp->with_disabled)
    qc->where = sqlite3_mprintf("WHERE f.disabled = 0");
  else if (qp->filter)
    qc->where = sqlite3_mprintf("WHERE %s", qp->filter);
  else
    qc->where = sqlite3_mprintf("");

  if (qp->having && (qp->type & (Q_GROUP_ALBUMS | Q_GROUP_ARTISTS)))
    qc->having = sqlite3_mprintf("HAVING %s", qp->having);
  else
    qc->having = sqlite3_mprintf("");

  if (qp->order)
    qc->order = sqlite3_mprintf("ORDER BY %s", qp->order);
  else if (qp->sort)
    qc->order = sqlite3_mprintf("ORDER BY %s", sort_clause[qp->sort]);
  else if (qp->type & Q_F_BROWSE)
    qc->order = sqlite3_mprintf("ORDER BY %s", browse_clause[qp->type & ~Q_F_BROWSE].group);
  else
    qc->order = sqlite3_mprintf("");

  switch (qp->idx_type)
    {
      case I_FIRST:
	if (qp->limit)
	  qc->index = sqlite3_mprintf("LIMIT %d", qp->limit);
	else
	  qc->index = sqlite3_mprintf("");
	break;

      case I_LAST:
	qc->index = sqlite3_mprintf("LIMIT -1 OFFSET %d", qp->results - qp->limit);
	break;

      case I_SUB:
	if (qp->limit)
	  qc->index = sqlite3_mprintf("LIMIT %d OFFSET %d", qp->limit, qp->offset);
	else
	  qc->index = sqlite3_mprintf("LIMIT -1 OFFSET %d", qp->offset);
	break;

      case I_NONE:
	qc->index = sqlite3_mprintf("");
	break;
    }

  if (!qc->where || !qc->index)
    goto error;

  return qc;

 error:
  DPRINTF(E_LOG, L_DB, "Error building query clause\n");
  db_free_query_clause(qc);
  return NULL;
}

static char *
db_build_query_check(struct query_params *qp, char *count, char *query)
{
  if (!count || !query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
      goto failed;
    }

  qp->results = db_get_one_int(count);
  if (qp->results < 0)
    {
      DPRINTF(E_LOG, L_DB, "No results for count\n");
      goto failed;
    }

  sqlite3_free(count);

  return query;

 failed:
  sqlite3_free(count);
  sqlite3_free(query);
  return NULL;
}

static char *
db_build_query_items(struct query_params *qp, struct query_clause *qc)
{
  char *count;
  char *query;

  if (qp->id == 0)
    {
      count = sqlite3_mprintf("SELECT COUNT(*) FROM files f %s;", qc->where);
      query = sqlite3_mprintf("SELECT f.* FROM files f %s %s %s %s;", qc->where, qc->group, qc->order, qc->index);
    }
  else if (qc->where[0] == '\0')
    {
      count = sqlite3_mprintf("SELECT COUNT(*) FROM files f WHERE f.id = %d;", qp->id);
      query = sqlite3_mprintf("SELECT f.* FROM files f WHERE f.id = %d %s %s %s;", qp->id, qc->group, qc->order, qc->index);
    }
  else
    {
      count = sqlite3_mprintf("SELECT COUNT(*) FROM files f %s AND f.id = %d;", qc->where, qp->id);
      query = sqlite3_mprintf("SELECT f.* FROM files f %s AND f.id = %d %s %s %s;", qc->where, qp->id, qc->group, qc->order, qc->index);
    }

  return db_build_query_check(qp, count, query);
}

static char *
db_build_query_pls(struct query_params *qp, struct query_clause *qc)
{
  char *count;
  char *query;

  count = sqlite3_mprintf("SELECT COUNT(*) FROM playlists f %s;", qc->where);
  query = sqlite3_mprintf(Q_PL_SELECT " %s GROUP BY f.id %s %s;", qc->where, qc->order, qc->index);

  return db_build_query_check(qp, count, query);
}

static char *
db_build_query_find_pls(struct query_params *qp, struct query_clause *qc)
{
  if (!qp->filter)
    {
      DPRINTF(E_LOG, L_DB, "Bug! Playlist find called without search criteria\n");
      return NULL;
    }

  // Use qp->filter because qc->where has a f.disabled which is not a column in playlistitems
  sqlite3_free(qc->where);
  qc->where = sqlite3_mprintf("WHERE f.id IN (SELECT playlistid FROM playlistitems WHERE %s)", qp->filter);

  return db_build_query_pls(qp, qc);
}

static char *
db_build_query_plitems_plain(struct query_params *qp, struct query_clause *qc)
{
  char *count;
  char *query;

  count = sqlite3_mprintf("SELECT COUNT(*) FROM files f JOIN playlistitems pi ON f.path = pi.filepath %s AND pi.playlistid = %d;", qc->where, qp->id);
  query = sqlite3_mprintf("SELECT f.* FROM files f JOIN playlistitems pi ON f.path = pi.filepath %s AND pi.playlistid = %d ORDER BY pi.id ASC %s;", qc->where, qp->id, qc->index);

  return db_build_query_check(qp, count, query);
}

static char *
db_build_query_plitems_smart(struct query_params *qp, struct playlist_info *pli)
{
  struct query_clause *qc;
  char *count;
  char *query;
  bool free_orderby = false;

  if (pli->query_limit > 0)
    {
      if (qp->idx_type == I_SUB)
	{
	  if (pli->query_limit > qp->offset + qp->limit)
	    qp->limit = pli->query_limit;
	}
      else if (qp->idx_type == I_NONE)
	{
	  qp->idx_type = I_SUB;
	  qp->limit = pli->query_limit;
	  qp->offset = 0;
	}
      else
	{
	  DPRINTF(E_WARN, L_DB, "Cannot append limit from smart playlist '%s' to query\n", pli->path);
	}
    }

  if (pli->query_order)
    {
      if (!qp->order && qp->sort == S_NONE)
	{
	  qp->order = strdup(pli->query_order);
	  free_orderby = true;
	}
      else
	DPRINTF(E_WARN, L_DB, "Cannot append order by from smart playlist '%s' to query\n", pli->path);
    }

  qc = db_build_query_clause(qp);
  if (free_orderby)
    {
      free(qp->order);
      qp->order = NULL;
    }
  if (!qc)
    return NULL;

  count = sqlite3_mprintf("SELECT COUNT(*) FROM files f %s AND %s LIMIT %d;", qc->where, pli->query, pli->query_limit ? pli->query_limit : -1);
  query = sqlite3_mprintf("SELECT f.* FROM files f %s AND %s %s %s;", qc->where, pli->query, qc->order, qc->index);

  db_free_query_clause(qc);

  return db_build_query_check(qp, count, query);
}

static char *
db_build_query_plitems(struct query_params *qp, struct query_clause *qc)
{
  struct playlist_info *pli;
  char *query;

  if (qp->id <= 0)
    {
      DPRINTF(E_LOG, L_DB, "No playlist id specified in playlist items query\n");
      return NULL;
    }

  pli = db_pl_fetch_byid(qp->id);
  if (!pli)
    return NULL;

  switch (pli->type)
    {
      case PL_SPECIAL:
      case PL_SMART:
	query = db_build_query_plitems_smart(qp, pli);
	break;

      case PL_RSS:
      case PL_PLAIN:
      case PL_FOLDER:
	query = db_build_query_plitems_plain(qp, qc);
	break;

      default:
	DPRINTF(E_LOG, L_DB, "Unknown playlist type %d in playlist items query\n", pli->type);
	query = NULL;
	break;
    }

  free_pli(pli, 0);

  return query;
}

static char *
db_build_query_group_albums(struct query_params *qp, struct query_clause *qc)
{
  char *count;
  char *query;

  count = sqlite3_mprintf("SELECT COUNT(DISTINCT f.songalbumid) FROM files f %s;", qc->where);
  query = sqlite3_mprintf("SELECT" \
			  " g.id, g.persistentid, f.album, f.album_sort, COUNT(f.id) AS track_count," \
			  " 1 AS album_count, f.album_artist, f.songartistid," \
			  " SUM(f.song_length) AS song_length, MIN(f.data_kind) AS data_kind, MIN(f.media_kind) AS media_kind," \
			  " MAX(f.year) AS year, MAX(f.date_released) AS date_released," \
			  " MAX(f.time_added) AS time_added, MAX(f.time_played) AS time_played, MAX(f.seek) AS seek " \
			  "FROM files f JOIN groups g ON f.songalbumid = g.persistentid %s " \
			  "GROUP BY f.songalbumid %s %s %s;", qc->where, qc->having, qc->order, qc->index);

  return db_build_query_check(qp, count, query);
}

static char *
db_build_query_group_artists(struct query_params *qp, struct query_clause *qc)
{
  char *count;
  char *query;

  count = sqlite3_mprintf("SELECT COUNT(DISTINCT f.songartistid) FROM files f %s;", qc->where);
  query = sqlite3_mprintf("SELECT" \
			  " g.id, g.persistentid, f.album_artist, f.album_artist_sort, COUNT(f.id) AS track_count," \
			  " COUNT(DISTINCT f.songalbumid) AS album_count, f.album_artist, f.songartistid," \
			  " SUM(f.song_length) AS song_length, MIN(f.data_kind) AS data_kind, MIN(f.media_kind) AS media_kind," \
			  " MAX(f.year) AS year, MAX(f.date_released) AS date_released," \
			  " MAX(f.time_added) AS time_added, MAX(f.time_played) AS time_played, MAX(f.seek) AS seek " \
			  "FROM files f JOIN groups g ON f.songartistid = g.persistentid %s " \
			  "GROUP BY f.songartistid %s %s %s;",
			  qc->where, qc->having, qc->order, qc->index);

  return db_build_query_check(qp, count, query);
}

static char *
db_build_query_group_items(struct query_params *qp, struct query_clause *qc)
{
  enum group_type gt;
  char *count;
  char *query;

  gt = db_group_type_bypersistentid(qp->persistentid);

  switch (gt)
    {
      case G_ALBUMS:
	count = sqlite3_mprintf("SELECT COUNT(*) FROM files f %s AND f.songalbumid = %" PRIi64 ";", qc->where, qp->persistentid);
	query = sqlite3_mprintf("SELECT f.* FROM files f %s AND f.songalbumid = %" PRIi64 " %s %s;", qc->where, qp->persistentid, qc->order, qc->index);
	break;

      case G_ARTISTS:
	count = sqlite3_mprintf("SELECT COUNT(*) FROM files f %s AND f.songartistid = %" PRIi64 ";", qc->where, qp->persistentid);
	query = sqlite3_mprintf("SELECT f.* FROM files f %s AND f.songartistid = %" PRIi64 " %s %s;", qc->where, qp->persistentid, qc->order, qc->index);
	break;

      default:
	DPRINTF(E_LOG, L_DB, "Unsupported group type %d for group id %" PRIi64 "\n", gt, qp->persistentid);
	return NULL;
    }

  return db_build_query_check(qp, count, query);
}

static char *
db_build_query_group_dirs(struct query_params *qp, struct query_clause *qc)
{
  enum group_type gt;
  char *count;
  char *query;

  gt = db_group_type_bypersistentid(qp->persistentid);

  switch (gt)
    {
      case G_ALBUMS:
	count = sqlite3_mprintf("SELECT COUNT(DISTINCT(SUBSTR(f.path, 1, LENGTH(f.path) - LENGTH(f.fname) - 1)))"
				" FROM files f %s AND f.songalbumid = %" PRIi64 ";", qc->where, qp->persistentid);
	query = sqlite3_mprintf("SELECT DISTINCT(SUBSTR(f.path, 1, LENGTH(f.path) - LENGTH(f.fname) - 1))"
				" FROM files f %s AND f.songalbumid = %" PRIi64 " %s %s;", qc->where, qp->persistentid, qc->order, qc->index);
	break;

      case G_ARTISTS:
	count = sqlite3_mprintf("SELECT COUNT(DISTINCT(SUBSTR(f.path, 1, LENGTH(f.path) - LENGTH(f.fname) - 1)))"
				" FROM files f %s AND f.songartistid = %" PRIi64 ";", qc->where, qp->persistentid);
	query = sqlite3_mprintf("SELECT DISTINCT(SUBSTR(f.path, 1, LENGTH(f.path) - LENGTH(f.fname) - 1))"
				" FROM files f %s AND f.songartistid = %" PRIi64 " %s %s;", qc->where, qp->persistentid, qc->order, qc->index);
	break;

      default:
	DPRINTF(E_LOG, L_DB, "Unsupported group type %d for group id %" PRIi64 "\n", gt, qp->persistentid);
	return NULL;
    }

  return db_build_query_check(qp, count, query);
}

static char *
db_build_query_browse(struct query_params *qp, struct query_clause *qc)
{
  const char *where;
  const char *select;
  char *count;
  char *query;

  select = browse_clause[qp->type & ~Q_F_BROWSE].select;
  where  = browse_clause[qp->type & ~Q_F_BROWSE].where;

  count = sqlite3_mprintf("SELECT COUNT(*) FROM (SELECT %s FROM files f %s AND %s != '' %s);", select, qc->where, where, qc->group);
  query = sqlite3_mprintf("SELECT %s, COUNT(f.id) AS track_count, COUNT(DISTINCT f.songalbumid) AS album_count, COUNT(DISTINCT f.songartistid) AS artist_count,"
			  " SUM(f.song_length) AS song_length, MIN(f.data_kind) AS data_kind, MIN(f.media_kind) AS media_kind,"
			  " MAX(f.year) AS year, MAX(f.date_released) AS date_released,"
			  " MAX(f.time_added) AS time_added, MAX(f.time_played) AS time_played, MAX(f.seek) AS seek "
			  "FROM files f %s AND %s != '' %s %s %s;",
			  select, qc->where, where, qc->group, qc->order, qc->index);

  return db_build_query_check(qp, count, query);
}

static char *
db_build_query_count_items(struct query_params *qp, struct query_clause *qc)
{
  char *query;

  qp->results = 1;

  query = sqlite3_mprintf("SELECT COUNT(*), SUM(song_length), COUNT(DISTINCT songartistid), COUNT(DISTINCT songalbumid), SUM(file_size) FROM files f %s;", qc->where);
  if (!query)
    DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

  return query;
}

int
db_query_start(struct query_params *qp)
{
  struct query_clause *qc;
  sqlite3_stmt *stmt;
  char *query;
  int ret;

  qp->stmt = NULL;
  qp->results = -1;

  qc = db_build_query_clause(qp);
  if (!qc)
    return -1;

  switch (qp->type)
    {
      case Q_ITEMS:
	query = db_build_query_items(qp, qc);
	break;

      case Q_PL:
	query = db_build_query_pls(qp, qc);
	break;

      case Q_FIND_PL:
	query = db_build_query_find_pls(qp, qc);
	break;

      case Q_PLITEMS:
	query = db_build_query_plitems(qp, qc);
	break;

      case Q_GROUP_ALBUMS:
	query = db_build_query_group_albums(qp, qc);
	break;

      case Q_GROUP_ARTISTS:
	query = db_build_query_group_artists(qp, qc);
	break;

      case Q_GROUP_ITEMS:
	query = db_build_query_group_items(qp, qc);
	break;

      case Q_GROUP_DIRS:
	query = db_build_query_group_dirs(qp, qc);
	break;

      case Q_COUNT_ITEMS:
	query = db_build_query_count_items(qp, qc);
	break;

      default:
	if (qp->type & Q_F_BROWSE)
	  query = db_build_query_browse(qp, qc);
        else
	  query = NULL;
    }

  db_free_query_clause(qc);

  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Could not create query, unknown type %d\n", qp->type);
      return -1;
    }

  DPRINTF(E_DBG, L_DB, "Starting query '%s'\n", query);

  ret = db_blocking_prepare_v2(query, -1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));

      sqlite3_free(query);
      return -1;
    }

  sqlite3_free(query);

  qp->stmt = stmt;

  return 0;
}

void
db_query_end(struct query_params *qp)
{
  if (!qp->stmt)
    return;

  sqlite3_finalize(qp->stmt);
  qp->stmt = NULL;
}

/*
 * Utility function for running write queries (INSERT, UPDATE, DELETE). If you
 * set free to non-zero, the function will free the query. If you set
 * library_update to non-zero it means that the update was not just of some
 * internal value (like a timestamp), but of something that requires clients
 * to update their cache of the library (and of course also of our own cache).
 */
static int
db_query_run(char *query, int free, short update_events)
{
  char *errmsg;
  int changes = 0;
  int ret;

  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return -1;
    }

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  /* If the query will be long running we don't want the cache to start regenerating */
  cache_daap_suspend();

  ret = db_exec(query, &errmsg);
  if (ret != SQLITE_OK)
    DPRINTF(E_LOG, L_DB, "Error '%s' while runnning '%s'\n", errmsg, query);
  else
    changes = sqlite3_changes(hdl);

  sqlite3_free(errmsg);

  if (free)
    sqlite3_free(query);

  cache_daap_resume();

  if (update_events && changes > 0)
    library_update_trigger(update_events);

  return ((ret != SQLITE_OK) ? -1 : 0);
}

static int
db_query_fetch(void *item, struct query_params *qp, const ssize_t cols_map[], int size)
{
  int ncols;
  char **strcol;
  int i;
  int ret;

  if (!qp->stmt)
    {
      DPRINTF(E_LOG, L_DB, "Query not started!\n");
      return -1;
    }

  ret = db_blocking_step(qp->stmt);
  if (ret == SQLITE_DONE)
    {
      DPRINTF(E_DBG, L_DB, "End of query results\n");
      return 1;
    }
  else if (ret != SQLITE_ROW)
    {
      DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));
      return -1;
    }

  ncols = sqlite3_column_count(qp->stmt);

  // We allow more cols in db than in map because the db may be a future schema
  if (ncols < size)
    {
      DPRINTF(E_LOG, L_DB, "BUG: database has fewer columns (%d) than column map (%u)\n", ncols, size);
      return -1;
    }

  for (i = 0; i < size; i++)
    {
      strcol = (char **) ((char *)item + cols_map[i]);

      *strcol = (char *)sqlite3_column_text(qp->stmt, i);
    }

  return 0;
}

int
db_query_fetch_file(struct db_media_file_info *dbmfi, struct query_params *qp)
{
  int ret;

  memset(dbmfi, 0, sizeof(struct db_media_file_info));

  if ((qp->type != Q_ITEMS) && (qp->type != Q_PLITEMS) && (qp->type != Q_GROUP_ITEMS))
    {
      DPRINTF(E_LOG, L_DB, "Not an items, playlist or group items query!\n");
      return -1;
    }

  ret = db_query_fetch(dbmfi, qp, dbmfi_cols_map, ARRAY_SIZE(dbmfi_cols_map));
  if (ret < 0) {
      DPRINTF(E_LOG, L_DB, "Failed to fetch db_media_file_info\n");
  }
  return ret;
}

int
db_query_fetch_pl(struct db_playlist_info *dbpli, struct query_params *qp)
{
  int ncols;
  char **strcol;
  uint32_t nitems;
  uint32_t nstreams;
  int type;
  int i;
  int ret;

  memset(dbpli, 0, sizeof(struct db_playlist_info));

  if (!qp->stmt)
    {
      DPRINTF(E_LOG, L_DB, "Query not started!\n");
      return -1;
    }

  if ((qp->type != Q_PL) && (qp->type != Q_FIND_PL))
    {
      DPRINTF(E_LOG, L_DB, "Not a playlist query!\n");
      return -1;
    }

  ret = db_blocking_step(qp->stmt);
  if (ret == SQLITE_DONE)
    {
      DPRINTF(E_DBG, L_DB, "End of query results\n");
      dbpli->id = NULL;
      return 0;
    }
  else if (ret != SQLITE_ROW)
    {
      DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));	
      return -1;
    }

  ncols = sqlite3_column_count(qp->stmt);

  // We allow more cols in db than in map because the db may be a future schema
  if (ncols < ARRAY_SIZE(dbpli_cols_map))
    {
      DPRINTF(E_LOG, L_DB, "BUG: database has fewer columns (%d) than dbpli column map (%u)\n", ncols, ARRAY_SIZE(dbpli_cols_map));
      return -1;
    }

  for (i = 0; i < ARRAY_SIZE(dbpli_cols_map); i++)
    {
      strcol = (char **) ((char *)dbpli + dbpli_cols_map[i]);

      *strcol = (char *)sqlite3_column_text(qp->stmt, i);
    }

  type = sqlite3_column_int(qp->stmt, 2);
  if (type == PL_SPECIAL || type == PL_SMART)
    {
      db_files_get_count(&nitems, &nstreams, dbpli->query);
      snprintf(qp->buf1, sizeof(qp->buf1), "%d", (int)nitems);
      snprintf(qp->buf2, sizeof(qp->buf2), "%d", (int)nstreams);
      dbpli->items = qp->buf1;
      dbpli->streams = qp->buf2;
    }

  return 0;
}

int
db_query_fetch_group(struct db_group_info *dbgri, struct query_params *qp)
{
  int ret;

  memset(dbgri, 0, sizeof(struct db_group_info));

  if ((qp->type != Q_GROUP_ALBUMS) && (qp->type != Q_GROUP_ARTISTS))
    {
      DPRINTF(E_LOG, L_DB, "Not a groups query!\n");
      return -1;
    }

  ret = db_query_fetch(dbgri, qp, dbgri_cols_map, ARRAY_SIZE(dbgri_cols_map));
  if (ret < 0) {
      DPRINTF(E_LOG, L_DB, "Failed to fetch db_group_info\n");
  }
  return ret;
}

int
db_query_fetch_browse(struct db_browse_info *dbbi, struct query_params *qp)
{
  int ret;

  memset(dbbi, 0, sizeof(struct db_browse_info));

  if (!(qp->type & Q_F_BROWSE))
    {
      DPRINTF(E_LOG, L_DB, "Not a browse query!\n");
      return -1;
    }

  ret = db_query_fetch(dbbi, qp, dbbi_cols_map, ARRAY_SIZE(dbbi_cols_map));
  if (ret < 0) {
      DPRINTF(E_LOG, L_DB, "Failed to fetch db_browse_info\n");
  }
  return ret;
}

int
db_query_fetch_count(struct filecount_info *fci, struct query_params *qp)
{
  int ret;

  memset(fci, 0, sizeof(struct filecount_info));

  if (!qp->stmt)
    {
      DPRINTF(E_LOG, L_DB, "Query not started!\n");
      return -1;
    }

  if (qp->type != Q_COUNT_ITEMS)
    {
      DPRINTF(E_LOG, L_DB, "Not a count query!\n");
      return -1;
    }

  ret = db_blocking_step(qp->stmt);
  if (ret == SQLITE_DONE)
    {
      DPRINTF(E_DBG, L_DB, "End of query results for count query\n");
      return 0;
    }
  else if (ret != SQLITE_ROW)
    {
      DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));
      return -1;
    }

  fci->count = sqlite3_column_int(qp->stmt, 0);
  fci->length = sqlite3_column_int64(qp->stmt, 1);
  fci->artist_count = sqlite3_column_int(qp->stmt, 2);
  fci->album_count = sqlite3_column_int(qp->stmt, 3);
  fci->file_size = sqlite3_column_int64(qp->stmt, 4);

  return 0;
}

int
db_filecount_get(struct filecount_info *fci, struct query_params *qp)
{
  int ret;

  ret = db_query_start(qp);
  if (ret < 0)
    {
      db_query_end(qp);

      return -1;
    }

  ret = db_query_fetch_count(fci, qp);
  if (ret < 0)
    {
      db_query_end(qp);

      return -1;
    }

  db_query_end(qp);
  return 0;
}

int
db_query_fetch_string(char **string, struct query_params *qp)
{
  int ret;

  *string = NULL;

  if (!qp->stmt)
    {
      DPRINTF(E_LOG, L_DB, "Query not started!\n");
      return -1;
    }

  ret = db_blocking_step(qp->stmt);
  if (ret == SQLITE_DONE)
    {
      DPRINTF(E_DBG, L_DB, "End of query results\n");
      *string = NULL;
      return 0;
    }
  else if (ret != SQLITE_ROW)
    {
      DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));	
      return -1;
    }

  *string = (char *)sqlite3_column_text(qp->stmt, 0);

  return 0;
}

int
db_query_fetch_string_sort(char **string, char **sortstring, struct query_params *qp)
{
  int ret;

  *string = NULL;

  if (!qp->stmt)
    {
      DPRINTF(E_LOG, L_DB, "Query not started!\n");
      return -1;
    }

  if (!(qp->type & Q_F_BROWSE))
    {
      DPRINTF(E_LOG, L_DB, "Not a browse query!\n");
      return -1;
    }

  ret = db_blocking_step(qp->stmt);
  if (ret == SQLITE_DONE)
    {
      DPRINTF(E_DBG, L_DB, "End of query results\n");
      *string = NULL;
      return 0;
    }
  else if (ret != SQLITE_ROW)
    {
      DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));
      return -1;
    }

  *string = (char *)sqlite3_column_text(qp->stmt, 0);
  *sortstring = (char *)sqlite3_column_text(qp->stmt, 1);

  return 0;
}


/* Files */
int
db_files_get_count(uint32_t *nitems, uint32_t *nstreams, const char *filter)
{
  sqlite3_stmt *stmt = NULL;
  char *query = NULL;
  int ret;

  if (!filter && !nstreams)
    query = sqlite3_mprintf("SELECT COUNT(*) FROM files f WHERE f.disabled = 0;");
  else if (!filter)
    query = sqlite3_mprintf("SELECT COUNT(*), SUM(data_kind = %d) FROM files f WHERE f.disabled = 0;", DATA_KIND_HTTP);
  else if (!nstreams)
    query = sqlite3_mprintf("SELECT COUNT(*) FROM files f WHERE f.disabled = 0 AND %s;", filter);
  else
    query = sqlite3_mprintf("SELECT COUNT(*), SUM(data_kind = %d) FROM files f WHERE f.disabled = 0 AND %s;", DATA_KIND_HTTP, filter);

  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
      goto error;
    }

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  ret = db_blocking_prepare_v2(query, -1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));
      goto error;
    }

  ret = db_blocking_step(stmt);
  if (ret != SQLITE_ROW)
    {
      if (ret == SQLITE_DONE)
	DPRINTF(E_INFO, L_DB, "No matching row found for query: %s\n", query);
      else
	DPRINTF(E_LOG, L_DB, "Could not step: %s (%s)\n", sqlite3_errmsg(hdl), query);

      goto error;
    }

  if (nitems)
    *nitems = sqlite3_column_int(stmt, 0);
  if (nstreams)
    *nstreams = sqlite3_column_int(stmt, 1);

#ifdef DB_PROFILE
  while (db_blocking_step(stmt) == SQLITE_ROW)
    ; /* EMPTY */
#endif

  sqlite3_free(query);
  sqlite3_finalize(stmt);
  return 0;

 error:
  if (query)
    sqlite3_free(query);
  if (stmt)
    sqlite3_finalize(stmt);
  return -1;
}

static void
db_file_inc_playcount_byfilter(const char *filter)
{
#define Q_TMPL "UPDATE files SET play_count = play_count + 1, time_played = %" PRIi64 ", seek = 0 WHERE %s;"
  /*
   * Rating calculation is taken from from the beets plugin "mpdstats" (see https://beets.readthedocs.io/en/latest/plugins/mpdstats.html)
   * and adapted to this servers rating rage (0 to 100).
   *
   * Rating consist of the stable rating and a rolling rating.
   * The stable rating is calculated based on the number was played and skipped:
   *   stable rating = (play_count + 1.0) / (play_count + skip_count + 2.0) * 100
   * The rolling rating is calculated based on the current action (played or skipped):
   *   rolling rating for played = rating + ((100.0 - rating) / 2.0)
   *   rolling rating for skipped = rating - (rating / 2.0)
   *
   * The new rating is a mix of stable and rolling rating (factor 0.75):
   *   new rating = stable rating * 0.75 + rolling rating * 0.25
   */
#define Q_TMPL_WITH_RATING \
               "UPDATE files "\
               " SET play_count = play_count + 1, time_played = %" PRIi64 ", seek = 0, "\
	       "     rating = CAST(((play_count + 1.0) / (play_count + skip_count + 2.0) * 100 * 0.75) + ((rating + ((100.0 - rating) / 2.0)) * 0.25) AS INT)" \
               " WHERE %s;"
  char *query;
  int ret;


  if (db_rating_updates)
    query = sqlite3_mprintf(Q_TMPL_WITH_RATING, (int64_t)time(NULL), filter);
  else
    query = sqlite3_mprintf(Q_TMPL, (int64_t)time(NULL), filter);

  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return;
    }

  // Perhaps this should in principle emit LISTENER_DATABASE, but that would
  // cause a lot of useless cache updates
  ret = db_query_run(query, 1, db_rating_updates ? LISTENER_RATING : 0);
  if (ret == 0)
    db_admin_setint64(DB_ADMIN_DB_MODIFIED, (int64_t) time(NULL));
#undef Q_TMPL
#undef Q_TMPL_WITH_RATING
}

void
db_file_inc_playcount_byplid(int id, bool only_unplayed)
{
  char *filter;

  filter = sqlite3_mprintf("path IN (SELECT filepath FROM playlistitems WHERE playlistid = %d) %s",
			   id, only_unplayed ? "AND play_count = 0" : "");

  db_file_inc_playcount_byfilter(filter);
  sqlite3_free(filter);
}

void
db_file_inc_playcount_bysongalbumid(int64_t id, bool only_unplayed)
{
  char *filter;

  filter = sqlite3_mprintf("songalbumid = %" PRIi64 " %s",
			   id, only_unplayed ? "AND play_count = 0" : "");

  db_file_inc_playcount_byfilter(filter);
  sqlite3_free(filter);
}

void
db_file_inc_playcount(int id)
{
  char *filter;

  filter = sqlite3_mprintf("id = %d", id);

  db_file_inc_playcount_byfilter(filter);
  sqlite3_free(filter);
}

void
db_file_inc_skipcount(int id)
{
#define Q_TMPL "UPDATE files SET skip_count = skip_count + 1, time_skipped = %" PRIi64 " WHERE id = %d;"
  // see db_file_inc_playcount for a description of how the rating is calculated
#define Q_TMPL_WITH_RATING \
               "UPDATE files "\
               " SET skip_count = skip_count + 1, time_skipped = %" PRIi64 ", seek = 0, "\
	       "     rating = CAST(((play_count + 1.0) / (play_count + skip_count + 2.0) * 100 * 0.75) + ((rating - (rating / 2.0)) * 0.25) AS INT)" \
               " WHERE id = %d;"
  char *query;
  int ret;

  if (db_rating_updates)
    query = sqlite3_mprintf(Q_TMPL_WITH_RATING, (int64_t)time(NULL), id);
  else
    query = sqlite3_mprintf(Q_TMPL, (int64_t)time(NULL), id);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return;
    }

  ret = db_query_run(query, 1, db_rating_updates ? LISTENER_RATING : 0);
  if (ret == 0)
    db_admin_setint64(DB_ADMIN_DB_MODIFIED, (int64_t) time(NULL));
#undef Q_TMPL
#undef Q_TMPL_WITH_RATING
}

void
db_file_reset_playskip_count(int id)
{
#define Q_TMPL "UPDATE files SET play_count = 0, skip_count = 0, time_played = 0, time_skipped = 0 WHERE id = %d;"
  char *query;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, id);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return;
    }

  ret = db_query_run(query, 1, 0);
  if (ret == 0)
    db_admin_setint64(DB_ADMIN_DB_MODIFIED, (int64_t) time(NULL));
#undef Q_TMPL
}

void
db_file_ping(int id)
{
#define Q_TMPL "UPDATE files SET db_timestamp = %" PRIi64 ", disabled = 0 WHERE id = %d;"
  char *query;

  query = sqlite3_mprintf(Q_TMPL, (int64_t)time(NULL), id);

  db_query_run(query, 1, 0);
#undef Q_TMPL
}

int
db_file_ping_bypath(const char *path, time_t mtime_max)
{
  sqlite3_bind_int64(db_statements.files_ping, 1, (int64_t)time(NULL));
  sqlite3_bind_text(db_statements.files_ping, 2, path, -1, SQLITE_STATIC);
  sqlite3_bind_int64(db_statements.files_ping, 3, (int64_t)mtime_max);

  return db_statement_run(db_statements.files_ping, 0);
}

void
db_file_ping_bymatch(const char *path, int isdir)
{
#define Q_TMPL_DIR "UPDATE files SET db_timestamp = %" PRIi64 " WHERE path LIKE '%q/%%';"
#define Q_TMPL_NODIR "UPDATE files SET db_timestamp = %" PRIi64 " WHERE path LIKE '%q%%';"
  char *query;

  if (isdir)
    query = sqlite3_mprintf(Q_TMPL_DIR, (int64_t)time(NULL), path);
  else
    query = sqlite3_mprintf(Q_TMPL_NODIR, (int64_t)time(NULL), path);

  db_query_run(query, 1, 0);
#undef Q_TMPL_DIR
#undef Q_TMPL_NODIR
}

void
db_file_ping_excl_bymatch(const char *path)
{
#define Q_TMPL_DIR "UPDATE files SET db_timestamp = %" PRIi64 " WHERE path NOT LIKE '%q/%%';"
  char *query;

  query = sqlite3_mprintf(Q_TMPL_DIR, (int64_t)time(NULL), path);
  db_query_run(query, 1, 0);

#undef Q_TMPL_DIR
}

char *
db_file_path_byid(int id)
{
#define Q_TMPL "SELECT f.path FROM files f WHERE f.id = %d;"
  char *query;
  sqlite3_stmt *stmt;
  char *res;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, id);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return NULL;
    }

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  ret = db_blocking_prepare_v2(query, strlen(query) + 1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));

      sqlite3_free(query);
      return NULL;
    }

  ret = db_blocking_step(stmt);
  if (ret != SQLITE_ROW)
    {
      if (ret == SQLITE_DONE)
	DPRINTF(E_DBG, L_DB, "No results\n");
      else
	DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));

      sqlite3_finalize(stmt);
      sqlite3_free(query);
      return NULL;
    }

  res = (char *)sqlite3_column_text(stmt, 0);
  if (res)
    res = strdup(res);

#ifdef DB_PROFILE
  while (db_blocking_step(stmt) == SQLITE_ROW)
    ; /* EMPTY */
#endif

  sqlite3_finalize(stmt);
  sqlite3_free(query);

  return res;

#undef Q_TMPL
}

static int
db_file_id_byquery(const char *query)
{
  sqlite3_stmt *stmt;
  int ret;

  if (!query)
    return 0;

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  ret = db_blocking_prepare_v2(query, strlen(query) + 1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));

      return 0;
    }

  ret = db_blocking_step(stmt);
  if (ret != SQLITE_ROW)
    {
      if (ret == SQLITE_DONE)
	DPRINTF(E_DBG, L_DB, "No results\n");
      else
	DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));

      sqlite3_finalize(stmt);
      return 0;
    }

  ret = sqlite3_column_int(stmt, 0);

#ifdef DB_PROFILE
  while (db_blocking_step(stmt) == SQLITE_ROW)
    ; /* EMPTY */
#endif

  sqlite3_finalize(stmt);

  return ret;
}

bool
db_file_id_exists(int id)
{
#define Q_TMPL "SELECT f.id FROM files f WHERE f.id = %d;"
  char *query;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, id);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return 0;
    }

  ret = db_file_id_byquery(query);

  sqlite3_free(query);

  return (id == ret);

#undef Q_TMPL
}

int
db_file_id_bypath(const char *path)
{
#define Q_TMPL "SELECT f.id FROM files f WHERE f.path = '%q';"
  char *query;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, path);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return 0;
    }

  ret = db_file_id_byquery(query);

  sqlite3_free(query);

  return ret;

#undef Q_TMPL
}

int
db_file_id_byfile(const char *filename)
{
#define Q_TMPL "SELECT f.id FROM files f WHERE f.fname = '%q';"
  char *query;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, filename);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return 0;
    }

  ret = db_file_id_byquery(query);

  sqlite3_free(query);

  return ret;

#undef Q_TMPL
}

int
db_file_id_byurl(const char *url)
{
#define Q_TMPL "SELECT f.id FROM files f WHERE f.url = '%q';"
  char *query;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, url);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return 0;
    }

  ret = db_file_id_byquery(query);

  sqlite3_free(query);

  return ret;

#undef Q_TMPL
}

int
db_file_id_byvirtualpath(const char *virtual_path)
{
#define Q_TMPL "SELECT f.id FROM files f WHERE f.virtual_path = %Q;"
  char *query;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, virtual_path);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return 0;
    }

  ret = db_file_id_byquery(query);

  sqlite3_free(query);

  return ret;

#undef Q_TMPL
}

int
db_file_id_byvirtualpath_match(const char *virtual_path)
{
#define Q_TMPL "SELECT f.id FROM files f WHERE f.virtual_path LIKE '%%%q%%';"
  char *query;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, virtual_path);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return 0;
    }

  ret = db_file_id_byquery(query);

  sqlite3_free(query);

  return ret;

#undef Q_TMPL
}

static struct media_file_info *
db_file_fetch_byquery(char *query)
{
  struct media_file_info *mfi;
  sqlite3_stmt *stmt;
  int ncols;
  int i;
  int ret;

  if (!query)
    return NULL;

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  mfi = calloc(1, sizeof(struct media_file_info));
  if (!mfi)
    {
      DPRINTF(E_LOG, L_DB, "Could not allocate struct media_file_info, out of memory\n");
      return NULL;
    }

  ret = db_blocking_prepare_v2(query, -1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));

      free(mfi);
      return NULL;
    }

  ret = db_blocking_step(stmt);

  if (ret != SQLITE_ROW)
    {
      if (ret == SQLITE_DONE)
	DPRINTF(E_DBG, L_DB, "No results\n");
      else
	DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));

      sqlite3_finalize(stmt);
      free(mfi);
      return NULL;
    }

  ncols = sqlite3_column_count(stmt);

  // We allow more cols in db than in map because the db may be a future schema
  if (ncols < ARRAY_SIZE(mfi_cols_map))
    {
      DPRINTF(E_LOG, L_DB, "BUG: database has fewer columns (%d) than mfi column map (%u)\n", ncols, ARRAY_SIZE(mfi_cols_map));

      sqlite3_finalize(stmt);
      free(mfi);
      return NULL;
    }

  for (i = 0; i < ARRAY_SIZE(mfi_cols_map); i++)
    {
      struct_field_from_statement(mfi, mfi_cols_map[i].offset, mfi_cols_map[i].type, stmt, i, true, false);
    }

#ifdef DB_PROFILE
  while (db_blocking_step(stmt) == SQLITE_ROW)
    ; /* EMPTY */
#endif

  sqlite3_finalize(stmt);

  return mfi;
}

struct media_file_info *
db_file_fetch_byid(int id)
{
#define Q_TMPL "SELECT f.* FROM files f WHERE f.id = %d;"
  struct media_file_info *mfi;
  char *query;

  query = sqlite3_mprintf(Q_TMPL, id);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return NULL;
    }

  mfi = db_file_fetch_byquery(query);

  sqlite3_free(query);

  return mfi;

#undef Q_TMPL
}

struct media_file_info *
db_file_fetch_byvirtualpath(const char *virtual_path)
{
#define Q_TMPL "SELECT f.* FROM files f WHERE f.virtual_path = %Q;"
  struct media_file_info *mfi;
  char *query;

  query = sqlite3_mprintf(Q_TMPL, virtual_path);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return NULL;
    }

  mfi = db_file_fetch_byquery(query);

  sqlite3_free(query);

  return mfi;

#undef Q_TMPL
}

int
db_file_add(struct media_file_info *mfi)
{
  int ret;

  if (mfi->id != 0)
    {
      DPRINTF(E_WARN, L_DB, "Trying to add file with non-zero id; use db_file_update()?\n");
      return -1;
    }

  mfi->db_timestamp = (uint64_t)time(NULL);

  // We don't do this in fixup_tags_mfi() to avoid affecting db_file_update()
  if (mfi->time_added == 0)
    mfi->time_added = mfi->db_timestamp;

  fixup_tags_mfi(mfi);

  ret = bind_mfi(db_statements.files_insert, mfi);
  if (ret < 0)
    return -1;

  ret = db_statement_run(db_statements.files_insert, 0);
  if (ret < 0)
    return -1;

  library_update_trigger(LISTENER_DATABASE);

  return 0;
}

int
db_file_update(struct media_file_info *mfi)
{
  int ret;

  if (mfi->id == 0)
    {
      DPRINTF(E_WARN, L_DB, "Trying to update file with id 0; use db_file_add()?\n");
      return -1;
    }

  mfi->db_timestamp = (uint64_t)time(NULL);

  fixup_tags_mfi(mfi);

  ret = bind_mfi(db_statements.files_update, mfi);
  if (ret < 0)
    return -1;

  ret = db_statement_run(db_statements.files_update, 0);
  if (ret < 0)
    return -1;

  library_update_trigger(LISTENER_DATABASE);

  return 0;
}

void
db_file_seek_update(int id, uint32_t seek)
{
#define Q_TMPL "UPDATE files SET seek = %d WHERE id = %d;"
  char *query;
  int ret;

  if (id == 0)
    return;

  query = sqlite3_mprintf(Q_TMPL, seek, id);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return;
    }

  ret = db_query_run(query, 1, 0);
  if (ret == 0)
    db_admin_setint64(DB_ADMIN_DB_MODIFIED, (int64_t) time(NULL));
#undef Q_TMPL
}

void
db_file_delete_bypath(const char *path)
{
#define Q_TMPL "DELETE FROM files WHERE path = '%q';"
  char *query;

  query = sqlite3_mprintf(Q_TMPL, path);

  db_query_run(query, 1, LISTENER_DATABASE);
#undef Q_TMPL
}

void
db_file_disable_bypath(const char *path, enum strip_type strip, uint32_t cookie)
{
#define Q_TMPL "UPDATE files SET path = substr(path, %d), virtual_path = substr(virtual_path, %d), disabled = %" PRIi64 " WHERE path = '%q';"
  char *query;
  int64_t disabled;
  int path_striplen;
  int vpath_striplen;

  disabled = (cookie != 0) ? cookie : INOTIFY_FAKE_COOKIE;

  path_striplen = (strip == STRIP_PATH) ? strlen(path) : 0;
  vpath_striplen = (strip == STRIP_PATH) ? strlen("/file:") + path_striplen : 0;

  query = sqlite3_mprintf(Q_TMPL, path_striplen + 1, vpath_striplen + 1, disabled, path);

  db_query_run(query, 1, LISTENER_DATABASE);
#undef Q_TMPL
}

void
db_file_disable_bymatch(const char *path, enum strip_type strip, uint32_t cookie)
{
#define Q_TMPL "UPDATE files SET path = substr(path, %d), virtual_path = substr(virtual_path, %d), disabled = %" PRIi64 " WHERE path LIKE '%q/%%';"
  char *query;
  int64_t disabled;
  int path_striplen;
  int vpath_striplen;

  disabled = (cookie != 0) ? cookie : INOTIFY_FAKE_COOKIE;

  path_striplen = (strip == STRIP_PATH) ? strlen(path) : 0;
  vpath_striplen = (strip == STRIP_PATH) ? strlen("/file:") + path_striplen : 0;

  query = sqlite3_mprintf(Q_TMPL, path_striplen + 1, vpath_striplen + 1, disabled, path);

  db_query_run(query, 1, LISTENER_DATABASE);
#undef Q_TMPL
}

// "path" will be the directory part for directory updates (dir moved) and the
// full path for file updates (file moved). The db will have the filename in 
// the path field for the former case (with a "/" prefix), and an empty path
// field for the latter.
int
db_file_enable_bycookie(uint32_t cookie, const char *path, const char *filename)
{
#define Q_TMPL_UPDATE_FNAME "UPDATE files SET path = ('%q' || path), virtual_path = ('/file:%q' || virtual_path), fname = '%q', disabled = 0 WHERE disabled = %" PRIi64 ";"
#define Q_TMPL "UPDATE files SET path = ('%q' || path), virtual_path = ('/file:%q' || virtual_path), disabled = 0 WHERE disabled = %" PRIi64 ";"
  char *query;
  int ret;

  if (filename)
    query = sqlite3_mprintf(Q_TMPL_UPDATE_FNAME, path, path, filename, (int64_t)cookie);
  else
    query = sqlite3_mprintf(Q_TMPL, path, path, (int64_t)cookie);

  ret = db_query_run(query, 1, LISTENER_DATABASE);

  return ((ret < 0) ? -1 : sqlite3_changes(hdl));
#undef Q_TMPL_UPDATE_FNAME
#undef Q_TMPL
}

int
db_file_update_directoryid(const char *path, int dir_id)
{
#define Q_TMPL "UPDATE files SET directory_id = %d WHERE path = %Q;"
  char *query;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, dir_id, path);

  ret = db_query_run(query, 1, 0);

  return ((ret < 0) ? -1 : sqlite3_changes(hdl));
#undef Q_TMPL
}


/* Playlists */
int
db_pl_get_count(uint32_t *nitems)
{
  int ret = db_get_one_int("SELECT COUNT(*) FROM playlists p WHERE p.disabled = 0;");

  if (ret < 0)
    return -1;

  *nitems = (uint32_t)ret;

  return 0;
}

void
db_pl_ping(int id)
{
#define Q_TMPL "UPDATE playlists SET db_timestamp = %" PRIi64 ", disabled = 0 WHERE id = %d;"
  char *query;

  query = sqlite3_mprintf(Q_TMPL, (int64_t)time(NULL), id);

  db_query_run(query, 1, 0);
#undef Q_TMPL
}

void
db_pl_ping_bymatch(const char *path, int isdir)
{
#define Q_TMPL_DIR "UPDATE playlists SET db_timestamp = %" PRIi64 " WHERE path LIKE '%q/%%';"
#define Q_TMPL_NODIR "UPDATE playlists SET db_timestamp = %" PRIi64 " WHERE path LIKE '%q%%';"
  char *query;

  if (isdir)
    query = sqlite3_mprintf(Q_TMPL_DIR, (int64_t)time(NULL), path);
  else
    query = sqlite3_mprintf(Q_TMPL_NODIR, (int64_t)time(NULL), path);

  db_query_run(query, 1, 0);
#undef Q_TMPL_DIR
#undef Q_TMPL_NODIR
}

void
db_pl_ping_excl_bymatch(const char *path)
{
#define Q_TMPL_DIR "UPDATE playlists SET db_timestamp = %" PRIi64 " WHERE path NOT LIKE '%q/%%';"
  char *query;

  query = sqlite3_mprintf(Q_TMPL_DIR, (int64_t)time(NULL), path);
  db_query_run(query, 1, 0);

#undef Q_TMPL_DIR
}

void
db_pl_ping_items_bymatch(const char *path, int id)
{
#define Q_TMPL "UPDATE files SET db_timestamp = %" PRIi64 ", disabled = 0 WHERE path IN (SELECT filepath FROM playlistitems WHERE filepath LIKE '%q%%' AND playlistid = %d);"
  char *query;

  query = sqlite3_mprintf(Q_TMPL, (int64_t)time(NULL), path, id);

  db_query_run(query, 1, 0);
#undef Q_TMPL
}

int
db_pl_id_bypath(const char *path)
{
#define Q_TMPL "SELECT p.id FROM playlists p WHERE p.path = '%q';"
  char *query;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, path);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return -1;
    }

  ret = db_get_one_int(query);

  sqlite3_free(query);

  return ret;

#undef Q_TMPL
}

static struct playlist_info *
db_pl_fetch_byquery(const char *query)
{
  struct playlist_info *pli;
  sqlite3_stmt *stmt;
  int ncols;
  int i;
  int ret;

  if (!query)
    return NULL;

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  pli = calloc(1, sizeof(struct playlist_info));
  if (!pli)
    {
      DPRINTF(E_LOG, L_DB, "Could not allocate struct playlist_info, out of memory\n");
      return NULL;
    }

  ret = db_blocking_prepare_v2(query, -1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));

      free(pli);
      return NULL;
    }

  ret = db_blocking_step(stmt);
  if (ret != SQLITE_ROW)
    {
      if (ret == SQLITE_DONE)
	DPRINTF(E_DBG, L_DB, "No results\n");
      else
	DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));

      sqlite3_finalize(stmt);
      free(pli);
      return NULL;
    }

  ncols = sqlite3_column_count(stmt);

  if (ncols < ARRAY_SIZE(pli_cols_map))
    {
      DPRINTF(E_LOG, L_DB, "BUG: database has fewer columns (%d) than pli column map (%u)\n", ncols, ARRAY_SIZE(pli_cols_map));

      sqlite3_finalize(stmt);
      free(pli);
      return NULL;
    }

  for (i = 0; i < ARRAY_SIZE(pli_cols_map); i++)
    {
      struct_field_from_statement(pli, pli_cols_map[i].offset, pli_cols_map[i].type, stmt, i, true, false);
    }

  ret = db_blocking_step(stmt);
  sqlite3_finalize(stmt);

  if (ret != SQLITE_DONE)
    {
      DPRINTF(E_WARN, L_DB, "Query had more than a single result!\n");

      free_pli(pli, 0);
      return NULL;
    }

  if (pli->type == PL_SPECIAL || pli->type == PL_SMART)
    db_files_get_count(&pli->items, &pli->streams, pli->query);

  return pli;
}

struct playlist_info *
db_pl_fetch_bypath(const char *path)
{
  struct playlist_info *pli;
  char *query;

  query = sqlite3_mprintf(Q_PL_SELECT " WHERE f.path = '%q' GROUP BY f.id;", path);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return NULL;
    }

  pli = db_pl_fetch_byquery(query);

  sqlite3_free(query);

  return pli;
}

struct playlist_info *
db_pl_fetch_byvirtualpath(const char *virtual_path)
{
  struct playlist_info *pli;
  char *query;

  query = sqlite3_mprintf(Q_PL_SELECT " WHERE f.virtual_path = '%q' GROUP BY f.id;", virtual_path);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return NULL;
    }

  pli = db_pl_fetch_byquery(query);

  sqlite3_free(query);

  return pli;
}

struct playlist_info *
db_pl_fetch_byid(int id)
{
  struct playlist_info *pli;
  char *query;

  query = sqlite3_mprintf(Q_PL_SELECT " WHERE f.id = %d GROUP BY f.id;", id);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return NULL;
    }

  pli = db_pl_fetch_byquery(query);

  sqlite3_free(query);

  return pli;
}

struct playlist_info *
db_pl_fetch_bytitlepath(const char *title, const char *path)
{
  struct playlist_info *pli;
  char *query;

  query = sqlite3_mprintf(Q_PL_SELECT " WHERE f.title = '%q' AND f.path = '%q' GROUP BY f.id;", title, path);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return NULL;
    }

  pli = db_pl_fetch_byquery(query);

  sqlite3_free(query);

  return pli;
}

int
db_pl_add(struct playlist_info *pli)
{
  int ret;

  // If the backend sets 1 it must be preserved, because the backend is still
  // scanning and is going to update it later (see filescanner_playlist.c)
  if (pli->db_timestamp != 1)
    pli->db_timestamp = (uint64_t)time(NULL);

  fixup_tags_pli(pli);

  ret = bind_pli(db_statements.playlists_insert, pli);
  if (ret < 0)
    return -1;

  ret = db_statement_run(db_statements.playlists_insert, 0);
  if (ret < 0)
    return -1;

  ret = (int)sqlite3_last_insert_rowid(hdl);
  if (ret == 0)
    {
      DPRINTF(E_LOG, L_DB, "Successful playlist insert but no last_insert_rowid!\n");
      return -1;
    }

  DPRINTF(E_DBG, L_DB, "Added playlist %s (path %s) as id %d\n", pli->title, pli->path, ret);

  return ret;
}

int
db_pl_update(struct playlist_info *pli)
{
  int ret;

  // If the backend sets 1 it must be preserved, because the backend is still
  // scanning and is going to update it later (see filescanner_playlist.c)
  if (pli->db_timestamp != 1)
    pli->db_timestamp = (uint64_t)time(NULL);

  fixup_tags_pli(pli);

  ret = bind_pli(db_statements.playlists_update, pli);
  if (ret < 0)
    return -1;

  ret = db_statement_run(db_statements.playlists_update, LISTENER_DATABASE);
  if (ret < 0)
    return -1;

  return pli->id;
}

int
db_pl_add_item_bypath(int plid, const char *path)
{
#define Q_TMPL "INSERT INTO playlistitems (playlistid, filepath) VALUES (%d, '%q');"
  char *query;

  query = sqlite3_mprintf(Q_TMPL, plid, path);

  return db_query_run(query, 1, LISTENER_DATABASE);
#undef Q_TMPL
}

int
db_pl_add_item_byid(int plid, int fileid)
{
#define Q_TMPL "INSERT INTO playlistitems (playlistid, filepath) VALUES (%d, (SELECT f.path FROM files f WHERE f.id = %d));"
  char *query;

  query = sqlite3_mprintf(Q_TMPL, plid, fileid);

  return db_query_run(query, 1, LISTENER_DATABASE);
#undef Q_TMPL
}

void
db_pl_clear_items(int id)
{
#define Q_TMPL_ITEMS "DELETE FROM playlistitems WHERE playlistid = %d;"
#define Q_TMPL_NESTED "UPDATE playlists SET parent_id = 0 WHERE parent_id = %d;"
  char *query;

  query = sqlite3_mprintf(Q_TMPL_ITEMS, id);
  db_query_run(query, 1, 0);

  query = sqlite3_mprintf(Q_TMPL_NESTED, id);
  db_query_run(query, 1, LISTENER_DATABASE);
#undef Q_TMPL_NESTED
#undef Q_TMPL_ITEMS
}

void
db_pl_delete(int id)
{
#define Q_TMPL "DELETE FROM playlists WHERE id = %d;"
#define Q_ORPHAN "SELECT filepath FROM playlistitems WHERE filepath NOT IN (SELECT filepath FROM playlistitems WHERE playlistid <> %d) AND playlistid = %d"
#define Q_FILES "DELETE FROM files WHERE data_kind = %d AND path IN (" Q_ORPHAN ");"
  char *query;
  int ret;

  if (id == 1)
    return;

  db_transaction_begin();

  query = sqlite3_mprintf(Q_TMPL, id);

  ret = db_query_run(query, 1, LISTENER_DATABASE);
  if (ret < 0)
    {
      db_transaction_rollback();
      return;
    }

  // Remove orphaned files (http items in files must have been added by the
  // playlist. The GROUP BY/count makes sure the files are not referenced by any
  // other playlist.
  // TODO find a cleaner way of identifying tracks added by a playlist
  query = sqlite3_mprintf(Q_FILES, DATA_KIND_HTTP, id, id);

  ret = db_query_run(query, 1, LISTENER_DATABASE);
  if (ret < 0)
    {
      db_transaction_rollback();
      return;
    }

  // Clear playlistitems
  db_pl_clear_items(id);

  db_transaction_end();
#undef Q_FILES
#undef Q_ORPHAN
#undef Q_TMPL
}

void
db_pl_delete_bypath(const char *path)
{
  struct query_params qp;
  struct db_playlist_info dbpli;
  int32_t id;
  int ret;

  memset(&qp, 0, sizeof(struct query_params));

  qp.type = Q_PL;
  qp.with_disabled = 1;
  CHECK_NULL(L_DB, qp.filter = db_mprintf("path = '%q'", path));

  ret = db_query_start(&qp);
  if (ret < 0)
    {
      free(qp.filter);
      return;
    }

  while (((ret = db_query_fetch_pl(&dbpli, &qp)) == 0) && (dbpli.id))
    {
      if (safe_atoi32(dbpli.id, &id) != 0)
	continue;

      db_pl_delete(id);
    }

  db_query_end(&qp);
  free(qp.filter);
}

void
db_pl_disable_bypath(const char *path, enum strip_type strip, uint32_t cookie)
{
#define Q_TMPL "UPDATE playlists SET path = substr(path, %d), virtual_path = substr(virtual_path, %d), disabled = %" PRIi64 " WHERE path = '%q';"
  char *query;
  int64_t disabled;
  int path_striplen;
  int vpath_striplen;

  disabled = (cookie != 0) ? cookie : INOTIFY_FAKE_COOKIE;

  path_striplen = (strip == STRIP_PATH) ? strlen(path) : 0;
  vpath_striplen = (strip == STRIP_PATH) ? strlen("/file:") + path_striplen : 0;

  query = sqlite3_mprintf(Q_TMPL, path_striplen + 1, vpath_striplen + 1, disabled, path);

  db_query_run(query, 1, 0);
#undef Q_TMPL
}

void
db_pl_disable_bymatch(const char *path, enum strip_type strip, uint32_t cookie)
{
#define Q_TMPL "UPDATE playlists SET path = substr(path, %d), virtual_path = substr(virtual_path, %d), disabled = %" PRIi64 " WHERE path LIKE '%q/%%';"
  char *query;
  int64_t disabled;
  int path_striplen;
  int vpath_striplen;

  disabled = (cookie != 0) ? cookie : INOTIFY_FAKE_COOKIE;

  path_striplen = (strip == STRIP_PATH) ? strlen(path) : 0;
  vpath_striplen = (strip == STRIP_PATH) ? strlen("/file:") + path_striplen : 0;

  query = sqlite3_mprintf(Q_TMPL, path_striplen + 1, vpath_striplen + 1, disabled, path);

  db_query_run(query, 1, 0);
#undef Q_TMPL
}

int
db_pl_enable_bycookie(uint32_t cookie, const char *path)
{
#define Q_TMPL "UPDATE playlists SET path = ('%q' || path), virtual_path = ('/file:%q' || virtual_path), disabled = 0 WHERE disabled = %" PRIi64 ";"
  char *query;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, path, path, (int64_t)cookie);

  ret = db_query_run(query, 1, 0);

  return ((ret < 0) ? -1 : sqlite3_changes(hdl));
#undef Q_TMPL
}



/* Groups */

// Remove album and artist entries in the groups table that are not longer referenced from the files table
int
db_groups_cleanup()
{
#define Q_TMPL_ALBUM "DELETE FROM groups WHERE type = 1 AND NOT persistentid IN (SELECT songalbumid from files WHERE disabled = 0);"
#define Q_TMPL_ARTIST "DELETE FROM groups WHERE type = 2 AND NOT persistentid IN (SELECT songartistid from files WHERE disabled = 0);"
  int ret;

  db_transaction_begin();

  ret = db_query_run(Q_TMPL_ALBUM, 0, LISTENER_DATABASE);
  if (ret < 0)
    {
      db_transaction_rollback();
      return -1;
    }

  DPRINTF(E_DBG, L_DB, "Removed album group-entries: %d\n", sqlite3_changes(hdl));

  ret = db_query_run(Q_TMPL_ARTIST, 0, LISTENER_DATABASE);
  if (ret < 0)
    {
      db_transaction_rollback();
      return -1;
    }

  DPRINTF(E_DBG, L_DB, "Removed artist group-entries: %d\n", sqlite3_changes(hdl));
  db_transaction_end();

  return 0;

#undef Q_TMPL_ALBUM
#undef Q_TMPL_ARTIST
}

static enum group_type
db_group_type_bypersistentid(int64_t persistentid)
{
#define Q_TMPL "SELECT g.type FROM groups g WHERE g.persistentid = %" PRIi64 ";"
  char *query;
  sqlite3_stmt *stmt;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, persistentid);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return 0;
    }

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  ret = db_blocking_prepare_v2(query, strlen(query) + 1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));

      sqlite3_free(query);
      return 0;
    }

  ret = db_blocking_step(stmt);
  if (ret != SQLITE_ROW)
    {
      if (ret == SQLITE_DONE)
	DPRINTF(E_DBG, L_DB, "No results\n");
      else
	DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));

      sqlite3_finalize(stmt);
      sqlite3_free(query);
      return 0;
    }

  ret = sqlite3_column_int(stmt, 0);

#ifdef DB_PROFILE
  while (db_blocking_step(stmt) == SQLITE_ROW)
    ; /* EMPTY */
#endif

  sqlite3_finalize(stmt);
  sqlite3_free(query);

  return ret;

#undef Q_TMPL
}

int
db_group_persistentid_byid(int id, int64_t *persistentid)
{
#define Q_TMPL "SELECT g.persistentid FROM groups g WHERE g.id = %d;"
  char *query;
  sqlite3_stmt *stmt;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, id);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return -1;
    }

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  ret = db_blocking_prepare_v2(query, -1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));

      sqlite3_free(query);
      return -1;
    }

  ret = db_blocking_step(stmt);
  if (ret != SQLITE_ROW)
    {
      if (ret == SQLITE_DONE)
	DPRINTF(E_DBG, L_DB, "No results\n");
      else
	DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));

      sqlite3_finalize(stmt);
      sqlite3_free(query);
      return -1;
    }

  *persistentid = sqlite3_column_int64(stmt, 0);

#ifdef DB_PROFILE
  while (db_blocking_step(stmt) == SQLITE_ROW)
  ; /* EMPTY */
#endif

  sqlite3_finalize(stmt);
  sqlite3_free(query);

  return 0;

#undef Q_TMPL
}


/* Directories */
int
db_directory_id_byvirtualpath(const char *virtual_path)
{
#define Q_TMPL "SELECT d.id FROM directories d WHERE d.virtual_path = '%q';"
  char *query;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, virtual_path);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return 0;
    }

  ret = db_file_id_byquery(query);

  sqlite3_free(query);

  return ret;

#undef Q_TMPL
}

int
db_directory_id_bypath(const char *path)
{
#define Q_TMPL "SELECT d.id FROM directories d WHERE d.path = '%q';"
  char *query;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, path);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return 0;
    }

  ret = db_file_id_byquery(query);

  sqlite3_free(query);

  return ret;

#undef Q_TMPL
}

int
db_directory_enum_start(struct directory_enum *de)
{
#define Q_TMPL "SELECT * FROM directories WHERE disabled = 0 AND parent_id = %d ORDER BY virtual_path COLLATE NOCASE;"
  sqlite3_stmt *stmt;
  char *query;
  int ret;

  de->stmt = NULL;

  query = sqlite3_mprintf(Q_TMPL, de->parent_id);

  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return -1;
    }

  DPRINTF(E_DBG, L_DB, "Starting enum '%s'\n", query);

  ret = db_blocking_prepare_v2(query, -1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));

      sqlite3_free(query);
      return -1;
    }

  sqlite3_free(query);

  de->stmt = stmt;

  return 0;

#undef Q_TMPL
}

int
db_directory_enum_fetch(struct directory_enum *de, struct directory_info *di)
{
  int ret;

  memset(di, 0, sizeof(struct directory_info));

  if (!de->stmt)
    {
      DPRINTF(E_LOG, L_DB, "Directory enum not started!\n");
      return -1;
    }

  ret = db_blocking_step(de->stmt);
  if (ret == SQLITE_DONE)
    {
      DPRINTF(E_DBG, L_DB, "End of directory enum results\n");
      return 0;
    }
  else if (ret != SQLITE_ROW)
    {
      DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));
      return -1;
    }

  di->id = sqlite3_column_int(de->stmt, 0);
  di->virtual_path = (char *)sqlite3_column_text(de->stmt, 1);
  di->db_timestamp = sqlite3_column_int(de->stmt, 2);
  di->disabled = sqlite3_column_int64(de->stmt, 3);
  di->parent_id = sqlite3_column_int(de->stmt, 4);
  di->path = (char *)sqlite3_column_text(de->stmt, 5);
  di->scan_kind = sqlite3_column_int(de->stmt, 6);

  return 0;
}

void
db_directory_enum_end(struct directory_enum *de)
{
  if (!de->stmt)
    return;

  sqlite3_finalize(de->stmt);
  de->stmt = NULL;
}

int
db_directory_add(struct directory_info *di, int *id)
{
#define QADD_TMPL "INSERT INTO directories (virtual_path, db_timestamp, disabled, parent_id, path, scan_kind)" \
                  " VALUES (TRIM(%Q), %d, %" PRIi64 ", %d, TRIM(%Q), %d);"

  char *query;
  char *errmsg;
  int vp_len = strlen(di->virtual_path);
  int ret;

  if (vp_len && di->virtual_path[vp_len-1] == ' ')
    {
      /* Since sqlite removes the trailing space, so these
       * directories will be found as new in perpetuity.
       */
      DPRINTF(E_LOG, L_DB, "Directory name ends with space: '%s'\n", di->virtual_path);
    }

  query = sqlite3_mprintf(QADD_TMPL, di->virtual_path, di->db_timestamp, di->disabled, di->parent_id, di->path, di->scan_kind);

  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
      return -1;
    }

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  ret = db_exec(query, &errmsg);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Query error: %s\n", errmsg);

      sqlite3_free(errmsg);
      sqlite3_free(query);
      return -1;
    }

  sqlite3_free(query);

  *id = (int)sqlite3_last_insert_rowid(hdl);
  if (*id == 0)
    {
      DPRINTF(E_LOG, L_DB, "Successful insert but no last_insert_rowid!\n");
      return -1;
    }

  DPRINTF(E_DBG, L_DB, "Added directory '%s' with id %d\n", di->virtual_path, *id);

  return 0;

#undef QADD_TMPL
}

int
db_directory_update(struct directory_info *di)
{
#define QADD_TMPL "UPDATE directories SET virtual_path = TRIM(%Q), db_timestamp = %d, disabled = %" PRIi64 ", parent_id = %d, path = TRIM(%Q), scan_kind = %d" \
                  " WHERE id = %d;"
  char *query;
  char *errmsg;
  int ret;

  /* Add */
  query = sqlite3_mprintf(QADD_TMPL, di->virtual_path, di->db_timestamp, di->disabled, di->parent_id, di->path, di->scan_kind, di->id);

  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
      return -1;
    }

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  ret = db_exec(query, &errmsg);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Query error: %s\n", errmsg);

      sqlite3_free(errmsg);
      sqlite3_free(query);
      return -1;
    }

  sqlite3_free(query);

  DPRINTF(E_DBG, L_DB, "Updated directory '%s' with id %d\n", di->virtual_path, di->id);

  return 0;

#undef QADD_TMPL
}

void
db_directory_ping_bymatch(char *virtual_path)
{
#define Q_TMPL_DIR "UPDATE directories SET db_timestamp = %" PRIi64 " WHERE virtual_path = '%q' OR virtual_path LIKE '%q/%%';"
  char *query;

  query = sqlite3_mprintf(Q_TMPL_DIR, (int64_t)time(NULL), virtual_path, virtual_path);

  db_query_run(query, 1, 0);
#undef Q_TMPL_DIR
}

void
db_directory_ping_excl_bymatch(const char *virtual_path)
{
#define Q_TMPL_DIR "UPDATE directories SET db_timestamp = %" PRIi64 " WHERE virtual_path <> '%q' OR virtual_path NOT LIKE '%q/%%';"
  char *query;

  query = sqlite3_mprintf(Q_TMPL_DIR, (int64_t)time(NULL), virtual_path, virtual_path);

  db_query_run(query, 1, 0);
#undef Q_TMPL_DIR
}

void
db_directory_disable_bymatch(const char *path, enum strip_type strip, uint32_t cookie)
{
#define Q_TMPL "UPDATE directories SET virtual_path = substr(virtual_path, %d)," \
               " disabled = %" PRIi64 " WHERE virtual_path = '/file:%q' OR virtual_path LIKE '/file:%q/%%';"
  char *query;
  int64_t disabled;
  int vpath_striplen;

  disabled = (cookie != 0) ? cookie : INOTIFY_FAKE_COOKIE;

  vpath_striplen = (strip == STRIP_PATH) ? strlen("/file:") + strlen(path) : 0;

  query = sqlite3_mprintf(Q_TMPL, vpath_striplen + 1, disabled, path, path, path);

  db_query_run(query, 1, LISTENER_DATABASE);
#undef Q_TMPL
}

int
db_directory_enable_bycookie(uint32_t cookie, const char *path)
{
#define Q_TMPL "UPDATE directories SET virtual_path = ('/file:%q' || virtual_path)," \
               " disabled = 0 WHERE disabled = %" PRIi64 ";"
  char *query;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, path, (int64_t)cookie);

  ret = db_query_run(query, 1, LISTENER_DATABASE);

  return ((ret < 0) ? -1 : sqlite3_changes(hdl));
#undef Q_TMPL
}

int
db_directory_enable_bypath(char *path)
{
#define Q_TMPL "UPDATE directories SET disabled = 0 WHERE virtual_path = %Q AND disabled <> 0;"
  char *query;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, path);

  ret = db_query_run(query, 1, LISTENER_DATABASE);

  return ((ret < 0) ? -1 : sqlite3_changes(hdl));
#undef Q_TMPL
}


/* Remotes */
static int
db_pairing_delete_byremote(char *remote_id)
{
#define Q_TMPL "DELETE FROM pairings WHERE remote = '%q';"
  char *query;

  query = sqlite3_mprintf(Q_TMPL, remote_id);

  return db_query_run(query, 1, 0);
#undef Q_TMPL
}

int
db_pairing_add(struct pairing_info *pi)
{
#define Q_TMPL "INSERT INTO pairings (remote, name, guid) VALUES ('%q', '%q', '%q');"
  char *query;
  int ret;

  ret = db_pairing_delete_byremote(pi->remote_id);
  if (ret < 0)
    return ret;

  query = sqlite3_mprintf(Q_TMPL, pi->remote_id, pi->name, pi->guid);

  return db_query_run(query, 1, 0);
#undef Q_TMPL
}

int
db_pairing_fetch_byguid(struct pairing_info *pi)
{
#define Q_TMPL "SELECT p.* FROM pairings p WHERE p.guid = '%q';"
  char *query;
  sqlite3_stmt *stmt;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, pi->guid);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
      return -1;
    }

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  ret = db_blocking_prepare_v2(query, -1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));
      return -1;
    }

  ret = db_blocking_step(stmt);
  if (ret != SQLITE_ROW)
    {
      if (ret == SQLITE_DONE)
	DPRINTF(E_INFO, L_DB, "Pairing GUID %s not found\n", pi->guid);
      else
	DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));

      sqlite3_finalize(stmt);
      sqlite3_free(query);
      return -1;
    }

  pi->remote_id = strdup((char *)sqlite3_column_text(stmt, 0));
  pi->name = strdup((char *)sqlite3_column_text(stmt, 1));

#ifdef DB_PROFILE
  while (db_blocking_step(stmt) == SQLITE_ROW)
    ; /* EMPTY */
#endif

  sqlite3_finalize(stmt);
  sqlite3_free(query);

  return 0;

#undef Q_TMPL
}

/* Spotify */
void
db_spotify_purge(void)
{
#define Q_TMPL "UPDATE directories SET disabled = %" PRIi64 " WHERE virtual_path = '/spotify:' AND disabled <> %" PRIi64 ";"
  char *queries[4] =
    {
      "DELETE FROM files WHERE path LIKE 'spotify:%%';",
      "DELETE FROM playlistitems WHERE filepath LIKE 'spotify:%%';",
      "DELETE FROM playlists WHERE path LIKE 'spotify:%%';",
      "DELETE FROM directories WHERE virtual_path LIKE '/spotify:/%%';",
    };
  char *query;
  int i;
  int ret;

  for (i = 0; i < (sizeof(queries) / sizeof(queries[0])); i++)
    {
      ret = db_query_run(queries[i], 0, LISTENER_DATABASE);

      if (ret == 0)
	DPRINTF(E_DBG, L_DB, "Processed %d rows\n", sqlite3_changes(hdl));
    }

  // Disable the spotify directory by setting 'disabled' to INOTIFY_FAKE_COOKIE value
  query = sqlite3_mprintf(Q_TMPL, INOTIFY_FAKE_COOKIE, INOTIFY_FAKE_COOKIE);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
      return;
    }
  ret = db_query_run(query, 1, LISTENER_DATABASE);

  if (ret == 0)
    DPRINTF(E_DBG, L_DB, "Disabled spotify directory\n");

#undef Q_TMPL
}

/* Spotify */
void
db_spotify_pl_delete(int id)
{
  char *queries_tmpl[2] =
    {
      "DELETE FROM playlists WHERE id = %d;",
      "DELETE FROM playlistitems WHERE playlistid = %d;",
    };
  char *query;
  int i;
  int ret;

  for (i = 0; i < (sizeof(queries_tmpl) / sizeof(queries_tmpl[0])); i++)
    {
      query = sqlite3_mprintf(queries_tmpl[i], id);

      ret = db_query_run(query, 1, LISTENER_DATABASE);

      if (ret == 0)
	DPRINTF(E_DBG, L_DB, "Deleted %d rows\n", sqlite3_changes(hdl));
    }
}

/* Spotify */
void
db_spotify_files_delete(void)
{
#define Q_TMPL "DELETE FROM files WHERE path LIKE 'spotify:%%' AND NOT path IN (SELECT filepath FROM playlistitems);"
  char *query;
  int ret;

  query = sqlite3_mprintf(Q_TMPL);

  ret = db_query_run(query, 1, LISTENER_DATABASE);

  if (ret == 0)
    DPRINTF(E_DBG, L_DB, "Deleted %d rows\n", sqlite3_changes(hdl));
#undef Q_TMPL
}

/* Admin */
int
db_admin_set(const char *key, const char *value)
{
#define Q_TMPL "INSERT OR REPLACE INTO admin (key, value) VALUES ('%q', '%q');"
  char *query;

  query = sqlite3_mprintf(Q_TMPL, key, value);

  return db_query_run(query, 1, 0);
#undef Q_TMPL
}

int
db_admin_setint(const char *key, int value)
{
#define Q_TMPL "INSERT OR REPLACE INTO admin (key, value) VALUES ('%q', '%d');"
  char *query;

  query = sqlite3_mprintf(Q_TMPL, key, value);

  return db_query_run(query, 1, 0);
#undef Q_TMPL
}

int
db_admin_setint64(const char *key, int64_t value)
{
#define Q_TMPL "INSERT OR REPLACE INTO admin (key, value) VALUES ('%q', '%" PRIi64 "');"
  char *query;

  query = sqlite3_mprintf(Q_TMPL, key, value);

  return db_query_run(query, 1, 0);
#undef Q_TMPL
}

static int
admin_get(void *value, const char *key, short type)
{
#define Q_TMPL "SELECT value FROM admin a WHERE a.key = '%q';"
  char *query;
  sqlite3_stmt *stmt;
  int ret;

  CHECK_NULL(L_DB, query = sqlite3_mprintf(Q_TMPL, key));

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  ret = db_blocking_prepare_v2(query, strlen(query) + 1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));

      sqlite3_free(query);
      return -1;
    }

  ret = db_blocking_step(stmt);
  if (ret != SQLITE_ROW)
    {
      if (ret == SQLITE_DONE)
	DPRINTF(E_DBG, L_DB, "No results\n");
      else
	DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));

      sqlite3_finalize(stmt);
      sqlite3_free(query);
      return -1;
    }

  struct_field_from_statement(value, 0, type, stmt, 0, true, false);

#ifdef DB_PROFILE
  while (db_blocking_step(stmt) == SQLITE_ROW)
    ; /* EMPTY */
#endif

  sqlite3_finalize(stmt);
  sqlite3_free(query);

  return 0;

#undef Q_TMPL
}

int
db_admin_get(char **value, const char *key)
{
  return admin_get(value, key, DB_TYPE_STRING);
}

int
db_admin_getint(int *intval, const char *key)
{
  return admin_get(intval, key, DB_TYPE_INT);
}

int
db_admin_getint64(int64_t *int64val, const char *key)
{
  return admin_get(int64val, key, DB_TYPE_INT64);
}

int
db_admin_delete(const char *key)
{
#define Q_TMPL "DELETE FROM admin WHERE key='%q';"
  char *query;

  query = sqlite3_mprintf(Q_TMPL, key);

  return db_query_run(query, 1, 0);
#undef Q_TMPL
}

/* Speakers */
int
db_speaker_save(struct output_device *device)
{
#define Q_TMPL "INSERT OR REPLACE INTO speakers (id, selected, volume, name, auth_key, format) VALUES (%" PRIi64 ", %d, %d, %Q, %Q, %d);"
  char *query;

  query = sqlite3_mprintf(Q_TMPL, device->id, device->selected, device->volume, device->name, device->auth_key, device->selected_format);

  return db_query_run(query, 1, 0);
#undef Q_TMPL
}

int
db_speaker_get(struct output_device *device, uint64_t id)
{
#define Q_TMPL "SELECT s.selected, s.volume, s.name, s.auth_key, s.format FROM speakers s WHERE s.id = %" PRIi64 ";"
  sqlite3_stmt *stmt;
  char *query;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, id);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
      return -1;
    }

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  ret = db_blocking_prepare_v2(query, -1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));
      ret = -1;
      goto out;
    }

  ret = db_blocking_step(stmt);
  if (ret != SQLITE_ROW)
    {
      if (ret != SQLITE_DONE)
	DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));
      sqlite3_finalize(stmt);
      ret = -1;
      goto out;
    }

  device->id = id;
  device->selected = sqlite3_column_int(stmt, 0);
  device->volume = sqlite3_column_int(stmt, 1);

  free(device->name);
  device->name = safe_strdup((char *)sqlite3_column_text(stmt, 2));

  free(device->auth_key);
  device->auth_key = safe_strdup((char *)sqlite3_column_text(stmt, 3));

  device->selected_format = sqlite3_column_int(stmt, 4);

#ifdef DB_PROFILE
  while (db_blocking_step(stmt) == SQLITE_ROW)
    ; /* EMPTY */
#endif

  sqlite3_finalize(stmt);

  ret = 0;

 out:
  sqlite3_free(query);
  return ret;

#undef Q_TMPL
}


/* Queue */

/*
 * Start a new transaction for modifying the queue. Returns the new queue version for the following changes.
 * After finishing all queue modifications 'queue_transaction_end' needs to be called.
 */
static int
queue_transaction_begin()
{
  int queue_version = 0;

  db_transaction_begin();

  db_admin_getint(&queue_version, DB_ADMIN_QUEUE_VERSION);
  queue_version++;

  return queue_version;
}

/*
 * If retval == 0, updates the version of the queue in the admin table, commits the transaction
 * and notifies listener of LISTENER_QUEUE about the changes.
 * If retval < 0, rollsback the transaction.
 *
 * This function must be called after modifying the queue.
 *
 * @param retval 'retval' >= 0, if modifying the queue was successful or 'retval' < 0 if an error occurred
 * @param queue_version The new queue version, for the pending modifications
 */
static void
queue_transaction_end(int retval, int queue_version)
{
  int ret;

  if (retval < 0)
    goto error;

  ret = db_admin_setint(DB_ADMIN_QUEUE_VERSION, queue_version);
  if (ret < 0)
    goto error;

  db_transaction_end();
  listener_notify(LISTENER_QUEUE);
  return;

 error:
  db_transaction_rollback();
}

static int
queue_reshuffle(uint32_t item_id, int queue_version);

static int
queue_item_add(struct db_queue_item *qi)
{
  int ret;

  fixup_tags_qi(qi);

  ret = bind_qi(db_statements.queue_items_insert, qi);
  if (ret < 0)
    return -1;

  // Do not update events here, only caller knows if more items will be added
  ret = db_statement_run(db_statements.queue_items_insert, 0);
  if (ret < 0)
    return -1;

  ret = (int)sqlite3_last_insert_rowid(hdl);
  if (ret == 0)
    {
      DPRINTF(E_LOG, L_DB, "Successful queue item insert but no last_insert_rowid!\n");
      return -1;
    }

  return ret;
}

static int
queue_item_add_from_file(struct db_media_file_info *dbmfi, int pos, int shuffle_pos, int queue_version)
{
  struct db_queue_item qi;
  int ret;

  // No allocations, just the struct content is copied
  db_queue_item_from_dbmfi(&qi, dbmfi);

  // No tag fixup, we can't touch the strings in qi, plus must assume dbmfi has proper tags

  qi.pos = pos;
  qi.shuffle_pos = shuffle_pos;
  qi.queue_version = queue_version;

  ret = bind_qi(db_statements.queue_items_insert, &qi);
  if (ret < 0)
    return -1;

  // Do not update events here, only caller knows if more items will be added
  ret = db_statement_run(db_statements.queue_items_insert, 0);
  if (ret < 0)
    return -1;

  ret = (int)sqlite3_last_insert_rowid(hdl);
  if (ret == 0)
    {
      DPRINTF(E_LOG, L_DB, "Successful queue item insert but no last_insert_rowid!\n");
      return -1;
    }

  return ret;
}

static int
queue_item_update(struct db_queue_item *qi)
{
  int ret;

  fixup_tags_qi(qi);

  ret = bind_qi(db_statements.queue_items_update, qi);
  if (ret < 0)
    return -1;

  // Do not update events here, only caller knows if more items will be updated
  ret = db_statement_run(db_statements.queue_items_update, 0);
  if (ret < 0)
    return -1;

  return qi->id;
}

void
db_queue_item_from_mfi(struct db_queue_item *qi, struct media_file_info *mfi)
{
  int i;

  memset(qi, 0, sizeof(struct db_queue_item));

  for (i = 0; i < ARRAY_SIZE(qi_mfi_map); i++)
    {
      if (qi_mfi_map[i].mfi_offset < 0)
	continue;

      struct_field_from_field(qi, qi_cols_map[i].offset, qi_cols_map[i].type, mfi, qi_mfi_map[i].mfi_offset, true, false);
    }

  if (!qi->file_id)
    qi->file_id = DB_MEDIA_FILE_NON_PERSISTENT_ID;
}

void
db_queue_item_from_dbmfi(struct db_queue_item *qi, struct db_media_file_info *dbmfi)
{
  int i;

  memset(qi, 0, sizeof(struct db_queue_item));

  for (i = 0; i < ARRAY_SIZE(qi_mfi_map); i++)
    {
      if (qi_mfi_map[i].dbmfi_offset < 0)
	continue;

      struct_field_from_field(qi, qi_cols_map[i].offset, qi_cols_map[i].type, dbmfi, qi_mfi_map[i].dbmfi_offset, false, true);
    }

  if (!qi->file_id)
    qi->file_id = DB_MEDIA_FILE_NON_PERSISTENT_ID;
}


int
db_queue_item_update(struct db_queue_item *qi)
{
  int queue_version;
  int ret;

  queue_version = queue_transaction_begin();

  qi->queue_version = queue_version;

  ret = queue_item_update(qi);

  // MPD changes playlist version when metadata changes, also makes LISTENER_QUEUE
  queue_transaction_end(ret, queue_version);

  return ret;
}

/*
 * Adds the files matching the given query to the queue after the item with the given item id
 *
 * The files table is queried with the given parameters and all found files are added after the
 * item with the given item id to the "normal" queue. They are appended to end of the shuffled queue
 * (assuming that the shuffled queue will get reshuffled after adding new items).
 *
 * The function returns -1 on failure (e. g. error reading from database) and if the given item id
 * does not exist. It wraps all database access in a transaction and performs a rollback if an error
 * occurs, leaving the queue in a consistent state.
 *
 * @param qp Query parameters for the files table
 * @param item_id Files are added after item with this id
 * @return 0 on success, -1 on failure
 */
int
db_queue_add_by_queryafteritemid(struct query_params *qp, uint32_t item_id)
{
  int pos;
  int ret;

  // Position of the first new item
  pos = db_queue_get_pos(item_id, 0);
  if (pos < 0)
    {
      return -1;
    }
  pos++;

  ret = db_queue_add_by_query(qp, 0, 0, pos, NULL, NULL);
  return ret;
}

int
db_queue_add_start(struct db_queue_add_info *queue_add_info, int pos)
{
  uint32_t queue_count;
  int ret;

  memset(queue_add_info, 0, sizeof(struct db_queue_add_info));
  queue_add_info->queue_version = queue_transaction_begin();

  ret = db_queue_get_count(&queue_count);
  if (ret < 0)
    {
      ret = -1;
      queue_transaction_end(ret, queue_add_info->queue_version);
      return ret;
    }

  queue_add_info->pos = queue_count;
  queue_add_info->shuffle_pos = queue_count;

  if (pos >= 0 && pos < queue_count)
    queue_add_info->pos = pos;

  queue_add_info->start_pos = queue_add_info->pos;

  return 0;
}

int
db_queue_add_end(struct db_queue_add_info *queue_add_info, char reshuffle, uint32_t item_id, int ret)
{
  char *query;

  if (ret < 0)
    goto end;

  // Update pos for all items from the given position
  query = sqlite3_mprintf("UPDATE queue SET pos = pos + %d, queue_version = %d WHERE pos >= %d AND queue_version < %d;",
                          queue_add_info->count, queue_add_info->queue_version, queue_add_info->start_pos, queue_add_info->queue_version);
  ret = db_query_run(query, 1, 0);
  if (ret < 0)
    goto end;

  // Reshuffle after adding new items
  if (reshuffle)
    ret = queue_reshuffle(item_id, queue_add_info->queue_version);

 end:
  queue_transaction_end(ret, queue_add_info->queue_version);
  return ret;
}

int
db_queue_add_next(struct db_queue_add_info *queue_add_info, struct db_queue_item *qi)
{
  int ret;

  qi->pos = queue_add_info->pos;
  qi->shuffle_pos = queue_add_info->shuffle_pos;
  qi->queue_version = queue_add_info->queue_version;

  ret = queue_item_add(qi);
  if (ret < 0)
    return ret;

  queue_add_info->pos++;
  queue_add_info->shuffle_pos++;
  queue_add_info->count++;

  if (queue_add_info->new_item_id == 0)
    queue_add_info->new_item_id = ret;

  return ret;
}

/*
 * Adds the files matching the given query to the queue
 *
 * The files table is queried with the given parameters and all found files are added to the end of the
 * "normal" queue and the shuffled queue.
 *
 * The function returns -1 on failure (e. g. error reading from database). It wraps all database access
 * in a transaction and performs a rollback if an error occurs, leaving the queue in a consistent state.
 *
 * @param qp Query parameters for the files table
 * @param reshuffle If 1 queue will be reshuffled after adding new items
 * @param item_id The base item id, all items after this will be reshuffled
 * @param position The position in the queue for the new queue item, -1 to add at end of queue
 * @param count If not NULL returns the number of items added to the queue
 * @param new_item_id If not NULL return the queue item id of the first new queue item
 * @return 0 on success, -1 on failure
 */
int
db_queue_add_by_query(struct query_params *qp, char reshuffle, uint32_t item_id, int position, int *count, int *new_item_id)
{
  struct db_media_file_info dbmfi;
  char *query;
  int queue_version;
  uint32_t queue_count;
  int pos;
  int shuffle_pos;
  bool append_to_queue;
  int ret;

  if (new_item_id)
    *new_item_id = 0;
  if (count)
    *count = 0;

  queue_version = queue_transaction_begin();

  ret = db_queue_get_count(&queue_count);
  if (ret < 0)
    {
      ret = -1;
      goto end_transaction;
    }

  ret = db_query_start(qp);
  if (ret < 0)
    goto end_transaction;

  DPRINTF(E_DBG, L_DB, "Player queue query returned %d items\n", qp->results);

  if (qp->results == 0)
    {
      db_query_end(qp);
      db_transaction_end();
      return 0;
    }

  append_to_queue = (position < 0 || position > queue_count);

  if (append_to_queue)
    {
      pos = queue_count;
      shuffle_pos = queue_count;
    }
  else
    {
      pos = position;
      shuffle_pos = position;

      // Update pos for all items from the given position (make room for the new items in the queue)
      query = sqlite3_mprintf("UPDATE queue SET pos = pos + %d, queue_version = %d WHERE pos >= %d;", qp->results, queue_version, pos);
      ret = db_query_run(query, 1, 0);
      if (ret < 0)
	goto end_transaction;

      // and similary update on shuffle_pos
      query = sqlite3_mprintf("UPDATE queue SET shuffle_pos = shuffle_pos + %d, queue_version = %d WHERE shuffle_pos >= %d;", qp->results, queue_version, pos);
      ret = db_query_run(query, 1, 0);
      if (ret < 0)
	goto end_transaction;
    }

  while ((ret = db_query_fetch_file(&dbmfi, qp)) == 0)
    {
      ret = queue_item_add_from_file(&dbmfi, pos, shuffle_pos, queue_version);

      if (ret < 0)
	{
	  DPRINTF(E_DBG, L_DB, "Failed to add song id %s (%s)\n", dbmfi.id, dbmfi.title);
	  break;
	}

      DPRINTF(E_DBG, L_DB, "Added (pos=%d shuffle_pos=%d reshuffle=%d req position=%d) song id %s (%s) to queue with item id %d\n", pos, shuffle_pos, reshuffle, position, dbmfi.id, dbmfi.title, ret);

      if (new_item_id && *new_item_id == 0)
	*new_item_id = ret;
      if (count)
	(*count)++;

      pos++;
      shuffle_pos++;
    }

  if (ret > 0)
    ret = 0;

  db_query_end(qp);

  if (ret < 0)
    goto end_transaction;

  // Reshuffle after adding new items, if no queue position was specified - this
  // case would indicate an 'add next' condition where if shuffling invalidates
  // the tracks added to 'next'
  if (append_to_queue && reshuffle)
    {
      ret = queue_reshuffle(item_id, queue_version);
    }

 end_transaction:
  queue_transaction_end(ret, queue_version);

  return ret;
}

static int
queue_enum_start(struct query_params *qp)
{
#define Q_TMPL "SELECT * FROM queue f WHERE %s ORDER BY %s;"
  sqlite3_stmt *stmt;
  char *query;
  const char *orderby;
  int ret;

  qp->stmt = NULL;

  if (qp->order)
    orderby = qp->order;
  else if (qp->sort)
    orderby = sort_clause[qp->sort];
  else
    orderby = sort_clause[S_POS];

  if (qp->filter)
    query = sqlite3_mprintf(Q_TMPL, qp->filter, orderby);
  else
    query = sqlite3_mprintf(Q_TMPL, "1=1", orderby);

  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return -1;
    }

  DPRINTF(E_DBG, L_DB, "Starting enum '%s'\n", query);

  ret = db_blocking_prepare_v2(query, -1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));

      sqlite3_free(query);
      return -1;
    }

  sqlite3_free(query);

  qp->stmt = stmt;

  return 0;

#undef Q_TMPL
}

static int
queue_enum_fetch(struct query_params *qp, struct db_queue_item *qi, int must_strdup)
{
  int ret;
  int i;

  memset(qi, 0, sizeof(struct db_queue_item));

  if (!qp->stmt)
    {
      DPRINTF(E_LOG, L_DB, "Queue enum not started!\n");
      return -1;
    }

  ret = db_blocking_step(qp->stmt);
  if (ret == SQLITE_DONE)
    {
      DPRINTF(E_DBG, L_DB, "End of queue enum results\n");
      return 0;
    }
  else if (ret != SQLITE_ROW)
    {
      DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));
      return -1;
    }

  for (i = 0; i < ARRAY_SIZE(qi_cols_map); i++)
    {
      struct_field_from_statement(qi, qi_cols_map[i].offset, qi_cols_map[i].type, qp->stmt, i, must_strdup, false);
    }

  return 0;
}

int
db_queue_enum_start(struct query_params *qp)
{
  int ret;

  db_transaction_begin();

  ret = queue_enum_start(qp);

  if (ret < 0)
    db_transaction_rollback();

  return ret;
}

void
db_queue_enum_end(struct query_params *qp)
{
  db_query_end(qp);
  db_transaction_end();
}

int
db_queue_enum_fetch(struct query_params *qp, struct db_queue_item *qi)
{
  return queue_enum_fetch(qp, qi, 0);
}

int
db_queue_get_pos(uint32_t item_id, char shuffle)
{
#define Q_TMPL "SELECT pos FROM queue WHERE id = %d;"
#define Q_TMPL_SHUFFLE "SELECT shuffle_pos FROM queue WHERE id = %d;"

  char *query;
  int pos;

  if (shuffle)
    query = sqlite3_mprintf(Q_TMPL_SHUFFLE, item_id);
  else
    query = sqlite3_mprintf(Q_TMPL, item_id);

  pos = db_get_one_int(query);

  sqlite3_free(query);

  return pos;

#undef Q_TMPL
#undef Q_TMPL_SHUFFLE
}

static int
queue_fetch_byitemid(uint32_t item_id, struct db_queue_item *qi, int with_metadata)
{
  struct query_params qp;
  int ret;

  memset(&qp, 0, sizeof(struct query_params));
  qp.filter = sqlite3_mprintf("id = %d", item_id);

  ret = queue_enum_start(&qp);
  if (ret < 0)
    {
      sqlite3_free(qp.filter);
      return -1;
    }

  ret = queue_enum_fetch(&qp, qi, with_metadata);
  db_query_end(&qp);
  sqlite3_free(qp.filter);
  return ret;
}

struct db_queue_item *
db_queue_fetch_byitemid(uint32_t item_id)
{
  struct db_queue_item *qi;
  int ret;

  qi = calloc(1, sizeof(struct db_queue_item));
  if (!qi)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for queue_item\n");
      return NULL;
    }

  db_transaction_begin();
  ret = queue_fetch_byitemid(item_id, qi, 1);
  db_transaction_end();

  if (ret < 0)
    {
      free_queue_item(qi, 0);
      DPRINTF(E_LOG, L_DB, "Error fetching queue item by item id\n");
      return NULL;
    }
  else if (qi->id == 0)
    {
      // No item found
      free_queue_item(qi, 0);
      return NULL;
    }

  return qi;
}

struct db_queue_item *
db_queue_fetch_byfileid(uint32_t file_id)
{
  struct db_queue_item *qi;
  struct query_params qp;
  int ret;

  memset(&qp, 0, sizeof(struct query_params));
  qi = calloc(1, sizeof(struct db_queue_item));
  if (!qi)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for queue_item\n");
      return NULL;
    }

  db_transaction_begin();

  qp.filter = sqlite3_mprintf("file_id = %d", file_id);

  ret = queue_enum_start(&qp);
  if (ret < 0)
    {
      sqlite3_free(qp.filter);
      db_transaction_end();
      free_queue_item(qi, 0);
      DPRINTF(E_LOG, L_DB, "Error fetching queue item by file id\n");
      return NULL;
    }

  ret = queue_enum_fetch(&qp, qi, 1);
  db_query_end(&qp);
  sqlite3_free(qp.filter);
  db_transaction_end();

  if (ret < 0)
    {
      free_queue_item(qi, 0);
      DPRINTF(E_LOG, L_DB, "Error fetching queue item by file id\n");
      return NULL;
    }
  else if (qi->id == 0)
    {
      // No item found
      free_queue_item(qi, 0);
      return NULL;
    }

  return qi;
}

static int
queue_fetch_bypos(uint32_t pos, char shuffle, struct db_queue_item *qi, int with_metadata)
{
  struct query_params qp;
  int ret;

  memset(&qp, 0, sizeof(struct query_params));
  if (shuffle)
    qp.filter = sqlite3_mprintf("shuffle_pos = %d", pos);
  else
    qp.filter = sqlite3_mprintf("pos = %d", pos);

  ret = queue_enum_start(&qp);
  if (ret < 0)
    {
      sqlite3_free(qp.filter);
      return -1;
    }

  ret = queue_enum_fetch(&qp, qi, with_metadata);
  db_query_end(&qp);
  sqlite3_free(qp.filter);
  return ret;
}

struct db_queue_item *
db_queue_fetch_bypos(uint32_t pos, char shuffle)
{
  struct db_queue_item *qi;
  int ret;

  qi = calloc(1, sizeof(struct db_queue_item));
  if (!qi)
    {
      DPRINTF(E_LOG, L_MAIN, "Out of memory for queue_item\n");
      return NULL;
    }

  db_transaction_begin();
  ret = queue_fetch_bypos(pos, shuffle, qi, 1);
  db_transaction_end();

  if (ret < 0)
    {
      free_queue_item(qi, 0);
      DPRINTF(E_LOG, L_DB, "Error fetching queue item by pos id\n");
      return NULL;
    }
  else if (qi->id == 0)
    {
      // No item found
      free_queue_item(qi, 0);
      return NULL;
    }

  return qi;
}

static int
queue_fetch_byposrelativetoitem(int pos, uint32_t item_id, char shuffle, struct db_queue_item *qi, int with_metadata)
{
  int pos_absolute;
  int ret;

  DPRINTF(E_DBG, L_DB, "Fetch by pos: pos (%d) relative to item with id (%d)\n", pos, item_id);

  pos_absolute = db_queue_get_pos(item_id, shuffle);
  if (pos_absolute < 0)
    {
      return -1;
    }

  DPRINTF(E_DBG, L_DB, "Fetch by pos: item (%d) has absolute pos %d\n", item_id, pos_absolute);

  pos_absolute += pos;

  ret = queue_fetch_bypos(pos_absolute, shuffle, qi, with_metadata);

  if (ret < 0)
    DPRINTF(E_LOG, L_DB, "Error fetching item by pos: pos (%d) relative to item with id (%d)\n", pos, item_id);
  else
    DPRINTF(E_DBG, L_DB, "Fetch by pos: fetched item (id=%d, pos=%d, file-id=%d)\n", qi->id, qi->pos, qi->file_id);

  return ret;
}

struct db_queue_item *
db_queue_fetch_byposrelativetoitem(int pos, uint32_t item_id, char shuffle)
{
  struct db_queue_item *qi;
  int ret;

  DPRINTF(E_DBG, L_DB, "Fetch by pos: pos (%d) relative to item with id (%d)\n", pos, item_id);

  qi = calloc(1, sizeof(struct db_queue_item));
  if (!qi)
    {
      DPRINTF(E_LOG, L_MAIN, "Out of memory for queue_item\n");
      return NULL;
    }

  db_transaction_begin();

  ret = queue_fetch_byposrelativetoitem(pos, item_id, shuffle, qi, 1);

  db_transaction_end();

  if (ret < 0)
    {
      free_queue_item(qi, 0);
      DPRINTF(E_LOG, L_DB, "Error fetching queue item by pos relative to item id\n");
      return NULL;
    }
  else if (qi->id == 0)
    {
      // No item found
      free_queue_item(qi, 0);
      return NULL;
    }

  DPRINTF(E_DBG, L_DB, "Fetch by pos: fetched item (id=%d, pos=%d, file-id=%d)\n", qi->id, qi->pos, qi->file_id);

  return qi;
}

struct db_queue_item *
db_queue_fetch_next(uint32_t item_id, char shuffle)
{
  return db_queue_fetch_byposrelativetoitem(1, item_id, shuffle);
}

struct db_queue_item *
db_queue_fetch_prev(uint32_t item_id, char shuffle)
{
  return db_queue_fetch_byposrelativetoitem(-1, item_id, shuffle);
}

static int
queue_fix_pos(enum sort_type sort, int queue_version)
{
#define Q_TMPL "UPDATE queue SET %q = %d, queue_version = %d WHERE id = %d and %q <> %d;"

  struct query_params qp;
  struct db_queue_item queue_item;
  char *query;
  int pos;
  int ret;

  memset(&qp, 0, sizeof(struct query_params));
  qp.sort = sort;

  ret = queue_enum_start(&qp);
  if (ret < 0)
    {
      return -1;
    }

  pos = 0;
  while ((ret = queue_enum_fetch(&qp, &queue_item, 0)) == 0 && (queue_item.id > 0))
    {
      if (queue_item.pos != pos)
        {
	  if (sort == S_SHUFFLE_POS)
	    query = sqlite3_mprintf(Q_TMPL, "shuffle_pos", pos, queue_version, queue_item.id, "shuffle_pos", pos);
	  else
	    query = sqlite3_mprintf(Q_TMPL, "pos", pos, queue_version, queue_item.id, "pos", pos);

	  ret = db_query_run(query, 1, 0);
	  if (ret < 0)
	    {
	      DPRINTF(E_LOG, L_DB, "Failed to update item with item-id: %d\n", queue_item.id);
	      break;
	    }
	}

      pos++;
    }

  db_query_end(&qp);
  return ret;

#undef Q_TMPL
}

/*
 * Remove files that are disabled or non existent in the library and repair ordering of
 * the queue (shuffle and normal)
 */
int
db_queue_cleanup()
{
#define Q_TMPL "DELETE FROM queue WHERE NOT file_id IN (SELECT id from files WHERE disabled = 0);"

  int queue_version;
  int deleted;
  int ret;

  queue_version = queue_transaction_begin();

  ret = db_query_run(Q_TMPL, 0, 0);
  if (ret < 0)
    goto end_transaction;

  deleted = sqlite3_changes(hdl);
  if (deleted <= 0)
    {
      // Nothing to do
      db_transaction_end();
      return 0;
    }

  // Update position of normal queue
  ret = queue_fix_pos(S_POS, queue_version);
  if (ret < 0)
    goto end_transaction;

  // Update position of shuffle queue
  ret = queue_fix_pos(S_SHUFFLE_POS, queue_version);

 end_transaction:
  queue_transaction_end(ret, queue_version);

  return ret;

#undef Q_TMPL
}

/*
 * Removes all items from the queue except the item give by 'keep_item_id' (if 'keep_item_id' > 0).
 *
 * @param keep_item_id item-id (e. g. the now playing item) to be left in the queue
 */
int
db_queue_clear(uint32_t keep_item_id)
{
  int queue_version;
  char *query;
  int ret;

  queue_version = queue_transaction_begin();

  query = sqlite3_mprintf("DELETE FROM queue where id <> %d;", keep_item_id);
  ret = db_query_run(query, 1, 0);

  if (ret == 0 && keep_item_id)
    {
      query = sqlite3_mprintf("UPDATE queue SET pos = 0, shuffle_pos = 0, queue_version = %d where id = %d;", queue_version, keep_item_id);
      ret = db_query_run(query, 1, 0);
    }

  queue_transaction_end(ret, queue_version);

  return ret;
}

static int
queue_delete_item(struct db_queue_item *qi, int queue_version)
{
  char *query;
  int ret;

  // Remove item with the given item_id
  query = sqlite3_mprintf("DELETE FROM queue where id = %d;", qi->id);
  ret = db_query_run(query, 1, 0);
  if (ret < 0)
    {
      return -1;
    }

  // Update pos for all items after the item with given item_id
  query = sqlite3_mprintf("UPDATE queue SET pos = pos - 1, queue_version = %d WHERE pos > %d;", queue_version, qi->pos);
  ret = db_query_run(query, 1, 0);
  if (ret < 0)
    {
      return -1;
    }

  // Update shuffle_pos for all items after the item with given item_id
  query = sqlite3_mprintf("UPDATE queue SET shuffle_pos = shuffle_pos - 1, queue_version = %d WHERE shuffle_pos > %d;", queue_version, qi->shuffle_pos);
  ret = db_query_run(query, 1, 0);
  if (ret < 0)
    {
      return -1;
    }

  return 0;
}

int
db_queue_delete_byitemid(uint32_t item_id)
{
  int queue_version;
  struct db_queue_item queue_item;
  int ret;

  queue_version = queue_transaction_begin();

  ret = queue_fetch_byitemid(item_id, &queue_item, 0);

  if (ret < 0)
    goto end_transaction;

  if (queue_item.id == 0)
    {
      db_transaction_end();
      return 0;
    }

  ret = queue_delete_item(&queue_item, queue_version);

 end_transaction:
  queue_transaction_end(ret, queue_version);

  return ret;
}

int
db_queue_delete_bypos(uint32_t pos, int count)
{
  int queue_version;
  char *query;
  int to_pos;
  int ret;

  queue_version = queue_transaction_begin();

  // Remove item with the given item_id
  to_pos = pos + count;
  query = sqlite3_mprintf("DELETE FROM queue where pos >= %d AND pos < %d;", pos, to_pos);
  ret = db_query_run(query, 1, 0);
  if (ret < 0)
    goto end_transaction;

  ret = queue_fix_pos(S_POS, queue_version);
  if (ret < 0)
    goto end_transaction;

  ret = queue_fix_pos(S_SHUFFLE_POS, queue_version);

 end_transaction:
  queue_transaction_end(ret, queue_version);

  return ret;
}

int
db_queue_delete_byposrelativetoitem(uint32_t pos, uint32_t item_id, char shuffle)
{
  int queue_version;
  struct db_queue_item queue_item;
  int ret;

  queue_version = queue_transaction_begin();

  ret = queue_fetch_byposrelativetoitem(pos, item_id, shuffle, &queue_item, 0);
  if (ret < 0)
    goto end_transaction;

  if (queue_item.id == 0)
    {
      // No item found
      db_transaction_end();
      return 0;
    }

  ret = queue_delete_item(&queue_item, queue_version);

 end_transaction:
  queue_transaction_end(ret, queue_version);

  return ret;
}

/*
 * Moves the queue item with the given id to the given position (zero-based).
 *
 * @param item_id Queue item id
 * @param pos_to target position in the queue (zero-based)
 * @þaram shuffle If 1 move item in the shuffle queue
 * @return 0 on success, -1 on failure
 */
int
db_queue_move_byitemid(uint32_t item_id, int pos_to, char shuffle)
{
  int queue_version;
  char *query;
  int pos_from;
  int ret;

  queue_version = queue_transaction_begin();

  // Find item with the given item_id
  pos_from = db_queue_get_pos(item_id, shuffle);
  if (pos_from < 0)
    {
      ret = -1;
      goto end_transaction;
    }

  // Update pos for all items after the item with given item_id
  if (shuffle)
    query = sqlite3_mprintf("UPDATE queue SET shuffle_pos = shuffle_pos - 1, queue_version = %d WHERE shuffle_pos > %d;", queue_version, pos_from);
  else
    query = sqlite3_mprintf("UPDATE queue SET pos = pos - 1, queue_version = %d WHERE pos > %d;", queue_version, pos_from);

  ret = db_query_run(query, 1, 0);
  if (ret < 0)
    goto end_transaction;

  // Update pos for all items from the given pos_to
  if (shuffle)
    query = sqlite3_mprintf("UPDATE queue SET shuffle_pos = shuffle_pos + 1, queue_version = %d WHERE shuffle_pos >= %d;", queue_version, pos_to);
  else
    query = sqlite3_mprintf("UPDATE queue SET pos = pos + 1, queue_version = %d WHERE pos >= %d;", queue_version, pos_to);

  ret = db_query_run(query, 1, 0);
  if (ret < 0)
    goto end_transaction;

  // Update item with the given item_id
  if (shuffle)
    query = sqlite3_mprintf("UPDATE queue SET shuffle_pos = %d, queue_version = %d where id = %d;", pos_to, queue_version, item_id);
  else
    query = sqlite3_mprintf("UPDATE queue SET pos = %d, queue_version = %d where id = %d;", pos_to, queue_version, item_id);

  ret = db_query_run(query, 1, 0);

 end_transaction:
  queue_transaction_end(ret, queue_version);

  return ret;
}

/*
 * Moves the queue item at the given position to the given position (zero-based).
 *
 * @param pos_from Position of the queue item to move
 * @param pos_to target position in the queue (zero-based)
 * @return 0 on success, -1 on failure
 */
int
db_queue_move_bypos(int pos_from, int pos_to)
{
  int queue_version;
  struct db_queue_item queue_item;
  char *query;
  int ret;

  queue_version = queue_transaction_begin();

  // Find item to move
  ret = queue_fetch_bypos(pos_from, 0, &queue_item, 0);
  if (ret < 0)
    goto end_transaction;

  if (queue_item.id == 0)
    {
      db_transaction_end();
      return 0;
    }

  // Update pos for all items after the item with given position
  query = sqlite3_mprintf("UPDATE queue SET pos = pos - 1, queue_version = %d WHERE pos > %d;", queue_version, queue_item.pos);
  ret = db_query_run(query, 1, 0);
  if (ret < 0)
    goto end_transaction;

  // Update pos for all items from the given pos_to
  query = sqlite3_mprintf("UPDATE queue SET pos = pos + 1, queue_version = %d WHERE pos >= %d;", queue_version, pos_to);
  ret = db_query_run(query, 1, 0);
  if (ret < 0)
    goto end_transaction;

  // Update item with the given item_id
  query = sqlite3_mprintf("UPDATE queue SET pos = %d, queue_version = %d where id = %d;", pos_to, queue_version, queue_item.id);
  ret = db_query_run(query, 1, 0);

 end_transaction:
  queue_transaction_end(ret, queue_version);

  return ret;
}

int
db_queue_move_bypos_range(int range_begin, int range_end, int pos_to)
{
#define Q_TMPL "UPDATE queue SET pos = CASE WHEN pos < %d THEN pos + %d ELSE pos - %d END, queue_version = %d WHERE pos >= %d AND pos < %d;"
  int queue_version;
  char *query;
  int count;
  int update_begin;
  int update_end;
  int ret;
  int cut_off;
  int offset_up;
  int offset_down;

  queue_version = queue_transaction_begin();

  count = range_end - range_begin;
  update_begin = MIN(range_begin, pos_to);
  update_end = MAX(range_begin + count, pos_to + count);

  if (range_begin < pos_to)
    {
      cut_off = range_begin + count;
      offset_up = pos_to - range_begin;
      offset_down = count;
    }
  else
    {
      cut_off = range_begin;
      offset_up = count;
      offset_down = range_begin - pos_to;
    }

  query = sqlite3_mprintf(Q_TMPL, cut_off, offset_up, offset_down, queue_version, update_begin, update_end);
  ret = db_query_run(query, 1, 0);

  queue_transaction_end(ret, queue_version);

  return ret;
#undef Q_TMPL
}

/*
 * Moves the queue item at the given position to the given target position. The positions
 * are relavtive to the given base item (item id).
 *
 * @param from_pos Relative position of the queue item to the base item
 * @param to_offset Target position relative to the base item
 * @param item_id The base item id (normaly the now playing item)
 * @return 0 on success, -1 on failure
 */
int
db_queue_move_byposrelativetoitem(uint32_t from_pos, uint32_t to_offset, uint32_t item_id, char shuffle)
{
  int queue_version;
  struct db_queue_item queue_item;
  char *query;
  int pos_move_from;
  int pos_move_to;
  int ret;

  queue_version = queue_transaction_begin();

  DPRINTF(E_DBG, L_DB, "Move by pos: from %d offset %d relative to item (%d)\n", from_pos, to_offset, item_id);

  // Find item with the given item_id
  ret = queue_fetch_byitemid(item_id, &queue_item, 0);
  if (ret < 0)
    goto end_transaction;

  DPRINTF(E_DBG, L_DB, "Move by pos: base item (id=%d, pos=%d, file-id=%d)\n", queue_item.id, queue_item.pos, queue_item.file_id);

  if (queue_item.id == 0)
    {
      db_transaction_end();
      return 0;
    }

  // Calculate the position of the item to move
  if (shuffle)
    pos_move_from = queue_item.shuffle_pos + from_pos;
  else
    pos_move_from = queue_item.pos + from_pos;

  // Calculate the position where to move the item to
  if (shuffle)
    pos_move_to = queue_item.shuffle_pos + to_offset;
  else
    pos_move_to = queue_item.pos + to_offset;

  if (pos_move_to < pos_move_from)
    {
      /*
       * Moving an item to a previous position seems to send an offset incremented by one
       */
      pos_move_to++;
    }

  DPRINTF(E_DBG, L_DB, "Move by pos: absolute pos: move from %d to %d\n", pos_move_from, pos_move_to);

  // Find item to move
  ret = queue_fetch_bypos(pos_move_from, shuffle, &queue_item, 0);
  if (ret < 0)
    goto end_transaction;

  DPRINTF(E_DBG, L_DB, "Move by pos: move item (id=%d, pos=%d, file-id=%d)\n", queue_item.id, queue_item.pos, queue_item.file_id);

  if (queue_item.id == 0)
    {
      db_transaction_end();
      return 0;
    }

  // Update pos for all items after the item with given position
  if (shuffle)
    query = sqlite3_mprintf("UPDATE queue SET shuffle_pos = shuffle_pos - 1, queue_version = %d WHERE shuffle_pos > %d;", queue_version, queue_item.shuffle_pos);
  else
    query = sqlite3_mprintf("UPDATE queue SET pos = pos - 1, queue_version = %d WHERE pos > %d;", queue_version, queue_item.pos);

  ret = db_query_run(query, 1, 0);
  if (ret < 0)
    goto end_transaction;

  // Update pos for all items from the given pos_to
  if (shuffle)
    query = sqlite3_mprintf("UPDATE queue SET shuffle_pos = shuffle_pos + 1, queue_version = %d WHERE shuffle_pos >= %d;", queue_version, pos_move_to);
  else
    query = sqlite3_mprintf("UPDATE queue SET pos = pos + 1, queue_version = %d WHERE pos >= %d;", queue_version, pos_move_to);

  ret = db_query_run(query, 1, 0);
  if (ret < 0)
    goto end_transaction;

  // Update item with the given item_id
  if (shuffle)
    query = sqlite3_mprintf("UPDATE queue SET shuffle_pos = %d, queue_version = %d where id = %d;", pos_move_to, queue_version, queue_item.id);
  else
    query = sqlite3_mprintf("UPDATE queue SET pos = %d, queue_version = %d where id = %d;", pos_move_to, queue_version, queue_item.id);

  ret = db_query_run(query, 1, 0);

 end_transaction:
  queue_transaction_end(ret, queue_version);

  return ret;
}

/*
 * Reshuffles the shuffle queue
 *
 * If the given item_id is 0, the whole shuffle queue is reshuffled, otherwise the
 * queue is reshuffled after the item with the given id (excluding this item).
 *
 * @param item_id The base item, after this item the queue is reshuffled
 * @return 0 on success, -1 on failure
 */
static int
queue_reshuffle(uint32_t item_id, int queue_version)
{
  char *query;
  int pos;
  uint32_t count;
  struct db_queue_item queue_item;
  int *shuffle_pos = NULL;
  int len;
  int i;
  struct query_params qp = { 0 };
  int ret;

  DPRINTF(E_DBG, L_DB, "Reshuffle queue after item with item-id: %d\n", item_id);

  // Reset the shuffled order and mark all items as changed
  query = sqlite3_mprintf("UPDATE queue SET shuffle_pos = pos, queue_version = %d;", queue_version);
  ret = db_query_run(query, 1, 0);
  if (ret < 0)
    goto error;

  pos = 0;
  if (item_id > 0)
    {
      pos = db_queue_get_pos(item_id, 0);
      if (pos < 0)
	goto error;

      pos++; // Do not reshuffle the base item
    }

  ret = db_queue_get_count(&count);
  if (ret < 0)
    goto error;

  len = count - pos;

  DPRINTF(E_DBG, L_DB, "Reshuffle %d items off %" PRIu32 " total items, starting from pos %d\n", len, count, pos);

  CHECK_NULL(L_DB, shuffle_pos = malloc(len * sizeof(int)));
  for (i = 0; i < len; i++)
    {
      shuffle_pos[i] = i + pos;
    }

  rng_shuffle_int(&shuffle_rng, shuffle_pos, len);

  qp.filter = sqlite3_mprintf("pos >= %d", pos);

  ret = queue_enum_start(&qp);
  if (ret < 0)
    goto error;

  i = 0;
  while ((ret = queue_enum_fetch(&qp, &queue_item, 0)) == 0 && (queue_item.id > 0) && (i < len))
    {
      query = sqlite3_mprintf("UPDATE queue SET shuffle_pos = %d where id = %d;", shuffle_pos[i], queue_item.id);
      ret = db_query_run(query, 1, 0);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_DB, "Failed to delete item with item-id: %d\n", queue_item.id);
	  break;
	}

      i++;
    }

  db_query_end(&qp);

  if (ret < 0)
    goto error;

  sqlite3_free(qp.filter);
  free(shuffle_pos);
  return 0;

 error:
  sqlite3_free(qp.filter);
  free(shuffle_pos);
  return -1;
}

/*
 * Reshuffles the shuffle queue
 *
 * If the given item_id is 0, the whole shuffle queue is reshuffled, otherwise the
 * queue is reshuffled after the item with the given id (excluding this item).
 *
 * @param item_id The base item, after this item the queue is reshuffled
 * @return 0 on success, -1 on failure
 */
int
db_queue_reshuffle(uint32_t item_id)
{
  int queue_version;
  int ret;

  queue_version = queue_transaction_begin();

  ret = queue_reshuffle(item_id, queue_version);

  queue_transaction_end(ret, queue_version);

  return ret;
}

/*
 * Increment queue version (triggers queue change event)
 */
int
db_queue_inc_version()
{
  int queue_version;

  queue_version = queue_transaction_begin();
  queue_transaction_end(0, queue_version);

  return 0;
}

int
db_queue_get_count(uint32_t *nitems)
{
  int ret = db_get_one_int("SELECT COUNT(*) FROM queue;");

  if (ret < 0)
    return -1;

  *nitems = (uint32_t)ret;

  return 0;
}


/* Inotify */
int
db_watch_clear(void)
{
  return db_query_run("DELETE FROM inotify;", 0, 0);
}

int
db_watch_add(struct watch_info *wi)
{
#define Q_TMPL "INSERT INTO inotify (wd, cookie, path) VALUES (%d, 0, '%q');"
  char *query;

  query = sqlite3_mprintf(Q_TMPL, wi->wd, wi->path);

  return db_query_run(query, 1, 0);
#undef Q_TMPL
}

int
db_watch_delete_bywd(uint32_t wd)
{
#define Q_TMPL "DELETE FROM inotify WHERE wd = %d;"
  char *query;

  query = sqlite3_mprintf(Q_TMPL, wd);

  return db_query_run(query, 1, 0);
#undef Q_TMPL
}

int
db_watch_delete_bypath(const char *path)
{
#define Q_TMPL "DELETE FROM inotify WHERE path = '%q';"
  char *query;

  query = sqlite3_mprintf(Q_TMPL, path);

  return db_query_run(query, 1, 0);
#undef Q_TMPL
}

int
db_watch_delete_bymatch(const char *path)
{
#define Q_TMPL "DELETE FROM inotify WHERE path LIKE '%q/%%';"
  char *query;

  query = sqlite3_mprintf(Q_TMPL, path);

  return db_query_run(query, 1, 0);
#undef Q_TMPL
}

int
db_watch_delete_bycookie(uint32_t cookie)
{
#define Q_TMPL "DELETE FROM inotify WHERE cookie = %" PRIi64 ";"
  char *query;

  if (cookie == 0)
    return -1;

  query = sqlite3_mprintf(Q_TMPL, (int64_t)cookie);

  return db_query_run(query, 1, 0);
#undef Q_TMPL
}

static int
db_watch_get_byquery(struct watch_info *wi, char *query)
{
  sqlite3_stmt *stmt;
  int64_t cookie;
  int ncols;
  int i;
  int ret;

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  ret = db_blocking_prepare_v2(query, -1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));
      return -1;
    }

  ret = db_blocking_step(stmt);
  if (ret != SQLITE_ROW)
    {
      sqlite3_finalize(stmt);
      sqlite3_free(query);
      return -1;
    }

  ncols = sqlite3_column_count(stmt);

  if (ncols < ARRAY_SIZE(wi_cols_map))
    {
      DPRINTF(E_LOG, L_DB, "BUG: database has fewer columns (%d) than wi column map (%u)\n", ncols, ARRAY_SIZE(wi_cols_map));

      sqlite3_finalize(stmt);
      sqlite3_free(query);
      return -1;
    }

  for (i = 0; i < ARRAY_SIZE(wi_cols_map); i++)
    {
      struct_field_from_statement(wi, wi_cols_map[i].offset, wi_cols_map[i].type, stmt, i, true, false);

      if (wi_cols_map[i].offset == wi_offsetof(cookie))
	{
	  cookie = sqlite3_column_int64(stmt, i);
	  wi->cookie = (cookie == INOTIFY_FAKE_COOKIE) ? 0 : cookie;
	}
    }

#ifdef DB_PROFILE
  while (db_blocking_step(stmt) == SQLITE_ROW)
    ; /* EMPTY */
#endif

  sqlite3_finalize(stmt);
  sqlite3_free(query);

  return 0;
}

int
db_watch_get_bywd(struct watch_info *wi, int wd)
{
#define Q_TMPL "SELECT * FROM inotify WHERE wd = %d;"
  char *query;

  query = sqlite3_mprintf(Q_TMPL, wd);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
      return -1;
    }

  return db_watch_get_byquery(wi, query);
#undef Q_TMPL
}

int
db_watch_get_bypath(struct watch_info *wi, const char *path)
{
#define Q_TMPL "SELECT * FROM inotify WHERE path = '%q';"
  char *query;

  query = sqlite3_mprintf(Q_TMPL, path);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");
      return -1;
    }

  return db_watch_get_byquery(wi, query);
#undef Q_TMPL
}

void
db_watch_mark_bypath(const char *path, enum strip_type strip, uint32_t cookie)
{
#define Q_TMPL "UPDATE inotify SET path = substr(path, %d), cookie = %" PRIi64 " WHERE path = '%q';"
  char *query;
  int64_t disabled;
  int path_striplen;

  disabled = (cookie != 0) ? cookie : INOTIFY_FAKE_COOKIE;

  path_striplen = (strip == STRIP_PATH) ? strlen(path) : 0;

  query = sqlite3_mprintf(Q_TMPL, path_striplen + 1, disabled, path);

  db_query_run(query, 1, 0);
#undef Q_TMPL
}

void
db_watch_mark_bymatch(const char *path, enum strip_type strip, uint32_t cookie)
{
#define Q_TMPL "UPDATE inotify SET path = substr(path, %d), cookie = %" PRIi64 " WHERE path LIKE '%q/%%';"
  char *query;
  int64_t disabled;
  int path_striplen;

  disabled = (cookie != 0) ? cookie : INOTIFY_FAKE_COOKIE;

  path_striplen = (strip == STRIP_PATH) ? strlen(path) : 0;

  query = sqlite3_mprintf(Q_TMPL, path_striplen + 1, disabled, path);

  db_query_run(query, 1, 0);
#undef Q_TMPL
}

void
db_watch_move_bycookie(uint32_t cookie, const char *path)
{
#define Q_TMPL "UPDATE inotify SET path = '%q' || path, cookie = 0 WHERE cookie = %" PRIi64 ";"
  char *query;

  if (cookie == 0)
    return;

  query = sqlite3_mprintf(Q_TMPL, path, (int64_t)cookie);

  db_query_run(query, 1, 0);
#undef Q_TMPL
}

int
db_watch_cookie_known(uint32_t cookie)
{
#define Q_TMPL "SELECT COUNT(*) FROM inotify WHERE cookie = %" PRIi64 ";"
  char *query;
  int ret;

  if (cookie == 0)
    return 0;

  query = sqlite3_mprintf(Q_TMPL, (int64_t)cookie);
  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return 0;
    }

  ret = db_get_one_int(query);

  sqlite3_free(query);

  return (ret > 0);

#undef Q_TMPL
}

int
db_watch_enum_start(struct watch_enum *we)
{
#define Q_MATCH_TMPL "SELECT wd,path FROM inotify WHERE path LIKE '%q/%%';"
#define Q_COOKIE_TMPL "SELECT wd,path FROM inotify WHERE cookie = %" PRIi64 ";"
  sqlite3_stmt *stmt;
  char *query;
  int ret;

  we->stmt = NULL;

  if (we->match)
    query = sqlite3_mprintf(Q_MATCH_TMPL, we->match);
  else if (we->cookie != 0)
    query = sqlite3_mprintf(Q_COOKIE_TMPL, we->cookie);
  else
    {
      DPRINTF(E_LOG, L_DB, "Could not start enum, no parameter given\n");
      return -1;
    }

  if (!query)
    {
      DPRINTF(E_LOG, L_DB, "Out of memory for query string\n");

      return -1;
    }

  DPRINTF(E_DBG, L_DB, "Starting enum '%s'\n", query);

  ret = db_blocking_prepare_v2(query, -1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));

      sqlite3_free(query);
      return -1;
    }

  sqlite3_free(query);

  we->stmt = stmt;

  return 0;

#undef Q_MATCH_TMPL
#undef Q_COOKIE_TMPL
}

void
db_watch_enum_end(struct watch_enum *we)
{
  if (!we->stmt)
    return;

  sqlite3_finalize(we->stmt);
  we->stmt = NULL;
}

int
db_watch_enum_fetchwd(struct watch_enum *we, uint32_t *wd)
{
  int ret;

  *wd = 0;

  if (!we->stmt)
    {
      DPRINTF(E_LOG, L_DB, "Watch enum not started!\n");
      return -1;
    }

  ret = db_blocking_step(we->stmt);
  if (ret == SQLITE_DONE)
    {
      DPRINTF(E_DBG, L_DB, "End of watch enum results\n");
      return 0;
    }
  else if (ret != SQLITE_ROW)
    {
      DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));	
      return -1;
    }

  *wd = (uint32_t)sqlite3_column_int(we->stmt, 0);

  return 0;
}

int
db_watch_enum_fetch(struct watch_enum *we, struct watch_info *wi)
{
  int ret;

  wi->wd = 0;
  wi->cookie = 0;

  if (!we->stmt)
    {
      DPRINTF(E_LOG, L_DB, "Watch enum not started!\n");
      return -1;
    }

  ret = db_blocking_step(we->stmt);
  if (ret == SQLITE_DONE)
    {
      DPRINTF(E_INFO, L_DB, "End of watch enum results\n");
      return 0;
    }
  else if (ret != SQLITE_ROW)
    {
      DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));
      return -1;
    }

  wi->wd = (uint32_t)sqlite3_column_int(we->stmt, 0);
  if (wi->path)
    snprintf(wi->path, PATH_MAX, (char*)sqlite3_column_text(we->stmt, 1));

  return 0;
}



#ifdef DB_PROFILE
static int
db_xprofile(unsigned int trace_type, void *notused, void *ptr, void *ptr_data)
{
  sqlite3_stmt *pstmt;
  int64_t ms = 0;
  sqlite3_stmt *stmt;
  const char *pquery;
  char *query;
  int log_level;
  int ret;

  if (trace_type != SQLITE_TRACE_PROFILE)
    return 0;

  pstmt = ptr;
  pquery = sqlite3_sql(pstmt);
  ms = *((int64_t *) ptr_data) / 1000000;

  if (ms > 1000)
    log_level = E_LOG;
  else if (ms > 500)
    log_level = E_WARN;
  else if (ms > 10)
    log_level = E_DBG;
  else
    log_level = E_SPAM;

  if (log_level > logger_severity())
    return 0;

  DPRINTF(log_level, L_DBPERF, "SQL PROFILE query: %s\n", pquery);
  DPRINTF(log_level, L_DBPERF, "SQL PROFILE time: %" PRIi64 " ms\n", ms);

  if ((strncmp(pquery, "SELECT", 6) != 0)
       && (strncmp(pquery, "UPDATE", 6) != 0)
       && (strncmp(pquery, "DELETE", 6) != 0))
      return 0;

  /* Disable profiling callback */
  sqlite3_trace_v2(hdl, 0, NULL, NULL);

  query = sqlite3_mprintf("EXPLAIN QUERY PLAN %s", pquery);
  if (!query)
    {
      DPRINTF(log_level, L_DBPERF, "Query plan: Out of memory\n");

      goto out;
    }

  ret = db_blocking_prepare_v2(query, -1, &stmt, NULL);
  sqlite3_free(query);
  if (ret != SQLITE_OK)
    {
      DPRINTF(log_level, L_DBPERF, "Query plan: Could not prepare statement: %s\n", sqlite3_errmsg(hdl));

      goto out;
    }

  DPRINTF(log_level, L_DBPERF, "Query plan:\n");

  while ((ret = db_blocking_step(stmt)) == SQLITE_ROW)
    {
      DPRINTF(log_level, L_DBPERF, "(%d,%d,%d) %s\n",
	      sqlite3_column_int(stmt, 0), sqlite3_column_int(stmt, 1), sqlite3_column_int(stmt, 2),
	      sqlite3_column_text(stmt, 3));
    }

  if (ret != SQLITE_DONE)
    DPRINTF(log_level, L_DBPERF, "Query plan: Could not step: %s\n", sqlite3_errmsg(hdl));

  sqlite3_finalize(stmt);

 out:
  /* Reenable profiling callback */
  sqlite3_trace_v2(hdl, SQLITE_TRACE_PROFILE, db_xprofile, NULL);

  return 0;
}
#endif

static int
db_pragma_get_cache_size()
{
  sqlite3_stmt *stmt;
  char *query = "PRAGMA cache_size;";
  int ret;

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  ret = db_blocking_prepare_v2(query, -1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));
      return 0;
    }

  ret = db_blocking_step(stmt);
  if (ret == SQLITE_DONE)
    {
      DPRINTF(E_DBG, L_DB, "End of query results\n");
      return 0;
    }
  else if (ret != SQLITE_ROW)
    {
      DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));
      return -1;
    }

  ret = sqlite3_column_int(stmt, 0);

  sqlite3_finalize(stmt);
  return ret;
}

static int
db_pragma_set_cache_size(int pages)
{
#define Q_TMPL "PRAGMA cache_size=%d;"
  sqlite3_stmt *stmt;
  char *query;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, pages);
  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  ret = db_blocking_prepare_v2(query, -1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));

      sqlite3_free(query);
      return 0;
    }

  sqlite3_finalize(stmt);
  sqlite3_free(query);
  return 0;
#undef Q_TMPL
}

static char *
db_pragma_set_journal_mode(char *mode)
{
#define Q_TMPL "PRAGMA journal_mode=%s;"
  sqlite3_stmt *stmt;
  char *query;
  int ret;
  char *new_mode;

  query = sqlite3_mprintf(Q_TMPL, mode);
  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  ret = db_blocking_prepare_v2(query, -1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));

      sqlite3_free(query);
      return NULL;
    }

  ret = db_blocking_step(stmt);
  if (ret == SQLITE_DONE)
    {
      DPRINTF(E_DBG, L_DB, "End of query results\n");
      sqlite3_free(query);
      return NULL;
    }
  else if (ret != SQLITE_ROW)
    {
      DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));
      sqlite3_free(query);
      return NULL;
    }

  new_mode = (char *) sqlite3_column_text(stmt, 0);
  sqlite3_finalize(stmt);
  sqlite3_free(query);
  return new_mode;
#undef Q_TMPL
}

static int
db_pragma_get_synchronous()
{
  sqlite3_stmt *stmt;
  char *query = "PRAGMA synchronous;";
  int ret;

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  ret = db_blocking_prepare_v2(query, -1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));
      return 0;
    }

  ret = db_blocking_step(stmt);
  if (ret == SQLITE_DONE)
    {
      DPRINTF(E_DBG, L_DB, "End of query results\n");
      return 0;
    }
  else if (ret != SQLITE_ROW)
    {
      DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));
      return -1;
    }

  ret = sqlite3_column_int(stmt, 0);

  sqlite3_finalize(stmt);
  return ret;
}

static int
db_pragma_set_synchronous(int synchronous)
{
#define Q_TMPL "PRAGMA synchronous=%d;"
  sqlite3_stmt *stmt;
  char *query;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, synchronous);
  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  ret = db_blocking_prepare_v2(query, -1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));

      sqlite3_free(query);
      return 0;
    }

  sqlite3_finalize(stmt);
  sqlite3_free(query);
  return 0;
#undef Q_TMPL
}

static int
db_pragma_get_mmap_size()
{
  sqlite3_stmt *stmt;
  char *query = "PRAGMA mmap_size;";
  int ret;

  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  ret = db_blocking_prepare_v2(query, -1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));
      return 0;
    }

  ret = db_blocking_step(stmt);
  if (ret == SQLITE_DONE)
    {
      DPRINTF(E_DBG, L_DB, "End of query results\n");
      return 0;
    }
  else if (ret != SQLITE_ROW)
    {
      DPRINTF(E_LOG, L_DB, "Could not step: %s\n", sqlite3_errmsg(hdl));
      return -1;
    }

  ret = sqlite3_column_int(stmt, 0);

  sqlite3_finalize(stmt);
  return ret;
}

static int
db_pragma_set_mmap_size(int mmap_size)
{
#define Q_TMPL "PRAGMA mmap_size=%d;"
  sqlite3_stmt *stmt;
  char *query;
  int ret;

  query = sqlite3_mprintf(Q_TMPL, mmap_size);
  DPRINTF(E_DBG, L_DB, "Running query '%s'\n", query);

  ret = db_blocking_prepare_v2(query, -1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statement: %s\n", sqlite3_errmsg(hdl));

      sqlite3_free(query);
      return 0;
    }

  sqlite3_finalize(stmt);
  sqlite3_free(query);
  return 0;
#undef Q_TMPL
}

static int
db_open(void)
{
  char *errmsg;
  int ret;
  int cache_size;
  char *journal_mode;
  int synchronous;
  int mmap_size;

  ret = sqlite3_open(db_path, &hdl);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not open '%s': %s\n", db_path, sqlite3_errmsg(hdl));

      sqlite3_close(hdl);
      return -1;
    }

  ret = sqlite3_enable_load_extension(hdl, 1);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not enable extension loading\n");

      sqlite3_close(hdl);
      return -1;
    }

  ret = sqlite3_load_extension(hdl, db_sqlite_ext_path, NULL, &errmsg);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not load SQLite extension: %s\n", errmsg);

      sqlite3_free(errmsg);
      sqlite3_close(hdl);
      return -1;
    }

  ret = sqlite3_enable_load_extension(hdl, 0);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_LOG, L_DB, "Could not disable extension loading\n");

      sqlite3_close(hdl);
      return -1;
    }

#ifdef DB_PROFILE
  sqlite3_trace_v2(hdl, SQLITE_TRACE_PROFILE, db_xprofile, NULL);
#endif

  cache_size = cfg_getint(cfg_getsec(cfg, "sqlite"), "pragma_cache_size_library");
  if (cache_size > -1)
    {
      db_pragma_set_cache_size(cache_size);
      cache_size = db_pragma_get_cache_size();
      DPRINTF(E_DBG, L_DB, "Database cache size in pages: %d\n", cache_size);
    }

  journal_mode = cfg_getstr(cfg_getsec(cfg, "sqlite"), "pragma_journal_mode");
  if (journal_mode)
    {
      journal_mode = db_pragma_set_journal_mode(journal_mode);
      DPRINTF(E_DBG, L_DB, "Database journal mode: %s\n", journal_mode);
    }

  synchronous = cfg_getint(cfg_getsec(cfg, "sqlite"), "pragma_synchronous");
  if (synchronous > -1)
    {
      db_pragma_set_synchronous(synchronous);
      synchronous = db_pragma_get_synchronous();
      DPRINTF(E_DBG, L_DB, "Database synchronous: %d\n", synchronous);
    }

  mmap_size = cfg_getint(cfg_getsec(cfg, "sqlite"), "pragma_mmap_size_library");
  if (mmap_size > -1)
    {
      db_pragma_set_mmap_size(mmap_size);
      mmap_size = db_pragma_get_mmap_size();
      DPRINTF(E_DBG, L_DB, "Database mmap_size: %d\n", mmap_size);
    }

  return 0;
}

static sqlite3_stmt *
db_statements_prepare_insert(const struct col_type_map *map, size_t map_size, const char *table)
{
  char *query;
  char keystr[2048];
  char valstr[1024];
  sqlite3_stmt *stmt;
  int ret;
  int i;

  memset(keystr, 0, sizeof(keystr));
  memset(valstr, 0, sizeof(valstr));
  for (i = 0; i < map_size; i++)
    {
      if (map[i].flag & DB_FLAG_NO_BIND)
	continue;

      CHECK_ERR(L_DB, safe_snprintf_cat(keystr, sizeof(keystr), "%s, ", map[i].name));
      CHECK_ERR(L_DB, safe_snprintf_cat(valstr, sizeof(valstr), "?, "));
    }

  // Terminate at the ending ", "
  *(strrchr(keystr, ',')) = '\0';
  *(strrchr(valstr, ',')) = '\0';

  CHECK_NULL(L_DB, query = db_mprintf("INSERT INTO %s (%s) VALUES (%s);", table, keystr, valstr));

  ret = db_blocking_prepare_v2(query, -1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_FATAL, L_DB, "Could not prepare statement '%s': %s\n", query, sqlite3_errmsg(hdl));
      free(query);
      return NULL;
    }

  free(query);

  return stmt;
}

static sqlite3_stmt *
db_statements_prepare_update(const struct col_type_map *map, size_t map_size, const char *table)
{
  char *query;
  char keystr[2048];
  sqlite3_stmt *stmt;
  int ret;
  int i;

  memset(keystr, 0, sizeof(keystr));
  for (i = 0; i < map_size; i++)
    {
      if (map[i].flag & DB_FLAG_NO_BIND)
	continue;

      if (map[i].flag & DB_FLAG_NO_ZERO)
	CHECK_ERR(L_DB, safe_snprintf_cat(keystr, sizeof(keystr), "%s = daap_no_zero(?, %s), ", map[i].name, map[i].name));
      else
	CHECK_ERR(L_DB, safe_snprintf_cat(keystr, sizeof(keystr), "%s = ?, ", map[i].name));
    }

  // Terminate at the ending ", "
  *(strrchr(keystr, ',')) = '\0';

  CHECK_NULL(L_DB, query = db_mprintf("UPDATE %s SET %s WHERE %s = ?;", table, keystr, map[0].name));

  ret = db_blocking_prepare_v2(query, -1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_FATAL, L_DB, "Could not prepare statement '%s': %s\n", query, sqlite3_errmsg(hdl));
      free(query);
      return NULL;
    }

  free(query);

  return stmt;
}

static sqlite3_stmt *
db_statements_prepare_ping(const char *table)
{
  char *query;
  sqlite3_stmt *stmt;
  int ret;

  // The last param will be the file mtime. We must not update if the mtime is
  // newer or equal than the current db_timestamp, since the file may have been
  // modified and must be rescanned.
  CHECK_NULL(L_DB, query = db_mprintf("UPDATE %s SET db_timestamp = ?, disabled = 0 WHERE path = ? AND db_timestamp > ?;", table));

  ret = db_blocking_prepare_v2(query, -1, &stmt, NULL);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_FATAL, L_DB, "Could not prepare statement '%s': %s\n", query, sqlite3_errmsg(hdl));
      free(query);
      return NULL;
    }

  free(query);

  return stmt;
}

static int
db_statements_prepare(void)
{
  db_statements.files_insert = db_statements_prepare_insert(mfi_cols_map, ARRAY_SIZE(mfi_cols_map), "files");
  db_statements.files_update = db_statements_prepare_update(mfi_cols_map, ARRAY_SIZE(mfi_cols_map), "files");
  db_statements.files_ping   = db_statements_prepare_ping("files");

  db_statements.playlists_insert = db_statements_prepare_insert(pli_cols_map, ARRAY_SIZE(pli_cols_map), "playlists");
  db_statements.playlists_update = db_statements_prepare_update(pli_cols_map, ARRAY_SIZE(pli_cols_map), "playlists");

  db_statements.queue_items_insert = db_statements_prepare_insert(qi_cols_map, ARRAY_SIZE(qi_cols_map), "queue");
  db_statements.queue_items_update = db_statements_prepare_update(qi_cols_map, ARRAY_SIZE(qi_cols_map), "queue");

  if ( !db_statements.files_insert || !db_statements.files_update || !db_statements.files_ping
       || !db_statements.playlists_insert || !db_statements.playlists_update
       || !db_statements.queue_items_insert || !db_statements.queue_items_update
     )
    return -1;

  return 0;
}

// Returns -2 if backup not enabled in config
int
db_backup(void)
{
  int ret;
  sqlite3 *backup_hdl;
  sqlite3_backup *backup;
  const char *backup_path;

  char resolved_bp[PATH_MAX];
  char resolved_dbp[PATH_MAX];

  backup_path = cfg_getstr(cfg_getsec(cfg, "general"), "db_backup_path");
  if (!backup_path)
    {
      DPRINTF(E_LOG, L_DB, "Backup not enabled, 'db_backup_path' is unset\n");
      return -2;
    }

  if (realpath(db_path, resolved_dbp) == NULL || realpath(backup_path, resolved_bp) == NULL)
    {
      DPRINTF(E_LOG, L_DB, "Failed to resolve real path of db/backup path: %s\n", strerror(errno));
      return -1;
    }

  if (strcmp(resolved_bp, resolved_dbp) == 0)
    {
      DPRINTF(E_LOG, L_DB, "Backup path same as main db path, ignoring\n");
      return -2;
    }

  DPRINTF(E_INFO, L_DB, "Backup starting...\n");

  ret = sqlite3_open(backup_path, &backup_hdl);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_WARN, L_DB, "Failed to create backup '%s': %s\n", backup_path, sqlite3_errmsg(backup_hdl));
      return -1;
    }

  backup = sqlite3_backup_init(backup_hdl, "main", hdl, "main");
  if (!backup)
    {
      DPRINTF(E_WARN, L_DB, "Failed to initiate backup '%s': %s\n", backup_path, sqlite3_errmsg(backup_hdl));
      sqlite3_close(backup_hdl);
      return -1;
    }

  ret = sqlite3_backup_step(backup, -1);
  sqlite3_backup_finish(backup);
  sqlite3_close(backup_hdl);

  if (ret == SQLITE_DONE || ret == SQLITE_OK)
    DPRINTF(E_INFO, L_DB, "Backup complete to '%s'\n", backup_path);
  else
    DPRINTF(E_WARN, L_DB, "Failed to complete backup '%s': %s (%d)\n", backup_path, sqlite3_errstr(ret), ret);

  return 0;
}

int
db_perthread_init(void)
{
  int ret;

  ret = db_open();
  if (ret < 0)
    return -1;

  ret = db_statements_prepare();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_DB, "Could not prepare statements\n");

      sqlite3_close(hdl);
      return -1;
    }

  return 0;
}

void
db_perthread_deinit(void)
{
  sqlite3_stmt *stmt;

  if (!hdl)
    return;

  /* Tear down anything that's in flight */
  while ((stmt = sqlite3_next_stmt(hdl, 0)))
    sqlite3_finalize(stmt);

  sqlite3_close(hdl);
}


static int
db_check_version(void)
{
#define Q_VACUUM "VACUUM;"
  char *errmsg;
  int db_ver_major = 0;
  int db_ver_minor = 0;
  int db_ver;
  int vacuum;
  int ret;

  vacuum = cfg_getbool(cfg_getsec(cfg, "sqlite"), "vacuum");

  db_admin_getint(&db_ver_major, DB_ADMIN_SCHEMA_VERSION_MAJOR);
  if (!db_ver_major)
    db_admin_getint(&db_ver_major, DB_ADMIN_SCHEMA_VERSION); // Pre schema v15.1

  if (!db_ver_major)
    return 1; // Will create new database

  db_admin_getint(&db_ver_minor, DB_ADMIN_SCHEMA_VERSION_MINOR);

  db_ver = db_ver_major * 100 + db_ver_minor;

  if (db_ver_major < 17)
    {
      DPRINTF(E_FATAL, L_DB, "Database schema v%d too old, cannot upgrade\n", db_ver_major);

      return -1;
    }
  else if (db_ver_major > SCHEMA_VERSION_MAJOR)
    {
      DPRINTF(E_FATAL, L_DB, "Database schema v%d is newer than the supported version\n", db_ver_major);

      return -1;
    }
  else if (db_ver < (SCHEMA_VERSION_MAJOR * 100 + SCHEMA_VERSION_MINOR))
    {
      DPRINTF(E_LOG, L_DB, "Database schema outdated, upgrading schema v%d.%d -> v%d.%d...\n",
                           db_ver_major, db_ver_minor, SCHEMA_VERSION_MAJOR, SCHEMA_VERSION_MINOR);

      ret = sqlite3_exec(hdl, "BEGIN TRANSACTION;", NULL, NULL, &errmsg);
      if (ret != SQLITE_OK)
	{
	  DPRINTF(E_LOG, L_DB, "DB error while running 'BEGIN TRANSACTION': %s\n",  errmsg);

	  sqlite3_free(errmsg);
	  return -1;
	}

      // Will drop indices and triggers
      ret = db_upgrade(hdl, db_ver);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_DB, "Database upgrade errored out, rolling back changes ...\n");
	  ret = sqlite3_exec(hdl, "ROLLBACK TRANSACTION;", NULL, NULL, &errmsg);
	  if (ret != SQLITE_OK)
	    {
	      DPRINTF(E_LOG, L_DB, "DB error while running 'ROLLBACK TRANSACTION': %s\n",  errmsg);

	      sqlite3_free(errmsg);
	    }

	  return -1;
	}

      ret = db_init_indices(hdl);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_DB, "Database upgrade errored out, rolling back changes ...\n");
	  ret = sqlite3_exec(hdl, "ROLLBACK TRANSACTION;", NULL, NULL, &errmsg);
	  if (ret != SQLITE_OK)
	    {
	      DPRINTF(E_LOG, L_DB, "DB error while running 'ROLLBACK TRANSACTION': %s\n",  errmsg);

	      sqlite3_free(errmsg);
	    }

	  return -1;
	}

      ret = db_init_triggers(hdl);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_DB, "Database upgrade errored out, rolling back changes ...\n");
	  ret = sqlite3_exec(hdl, "ROLLBACK TRANSACTION;", NULL, NULL, &errmsg);
	  if (ret != SQLITE_OK)
	    {
	      DPRINTF(E_LOG, L_DB, "DB error while running 'ROLLBACK TRANSACTION': %s\n",  errmsg);

	      sqlite3_free(errmsg);
	    }

	  return -1;
	}

      ret = sqlite3_exec(hdl, "COMMIT TRANSACTION;", NULL, NULL, &errmsg);
      if (ret != SQLITE_OK)
	{
	  DPRINTF(E_LOG, L_DB, "DB error while running 'COMMIT TRANSACTION': %s\n", errmsg);

	  sqlite3_free(errmsg);
	  return -1;
	}

      DPRINTF(E_LOG, L_DB, "Upgrading schema to v%d.%d completed\n", SCHEMA_VERSION_MAJOR, SCHEMA_VERSION_MINOR);

      vacuum = 1;
    }
  else if (db_ver_minor > SCHEMA_VERSION_MINOR)
    {
      DPRINTF(E_LOG, L_DB, "Future (but compatible) database version detected (v%d.%d)\n", db_ver_major, db_ver_minor);
    }

  if (vacuum)
    {
      DPRINTF(E_LOG, L_DB, "Now vacuuming database, this may take some time...\n");

      ret = sqlite3_exec(hdl, Q_VACUUM, NULL, NULL, &errmsg);
      if (ret != SQLITE_OK)
	{
	  DPRINTF(E_LOG, L_DB, "Could not VACUUM database: %s\n", errmsg);

	  sqlite3_free(errmsg);
	  return -1;
	}
    }

  return 0;

#undef Q_VACUUM
}

int
db_init(char *sqlite_ext_path)
{
  uint32_t files;
  uint32_t pls;
  int ret;
  int i;

  static_assert(ARRAY_SIZE(dbmfi_cols_map) == ARRAY_SIZE(mfi_cols_map), "mfi column maps are not in sync");
  static_assert(ARRAY_SIZE(dbpli_cols_map) == ARRAY_SIZE(pli_cols_map), "pli column maps are not in sync");
  static_assert(ARRAY_SIZE(qi_cols_map) == ARRAY_SIZE(qi_mfi_map), "queue_item column maps are not in sync");

  for (i = 0; i < ARRAY_SIZE(qi_cols_map); i++)
    {
      assert(qi_cols_map[i].offset == qi_mfi_map[i].qi_offset);
    }

  db_path = cfg_getstr(cfg_getsec(cfg, "general"), "db_path");
  db_sqlite_ext_path = sqlite_ext_path;
  db_rating_updates = cfg_getbool(cfg_getsec(cfg, "library"), "rating_updates");

  DPRINTF(E_INFO, L_DB, "Configured to use database file '%s'\n", db_path);

  ret = sqlite3_config(SQLITE_CONFIG_MULTITHREAD);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_FATAL, L_DB, "Could not switch SQLite3 to multithread mode\n");
      DPRINTF(E_FATAL, L_DB, "Check that SQLite3 has been configured for thread-safe operations\n");
      goto error;
    }

  ret = sqlite3_enable_shared_cache(1);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_FATAL, L_DB, "Could not enable SQLite3 shared-cache mode\n");
      goto error;
    }

  ret = sqlite3_initialize();
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_FATAL, L_DB, "SQLite3 failed to initialize\n");
      goto error;
    }

  ret = db_open();
  if (ret < 0)
    {
      DPRINTF(E_FATAL, L_DB, "Could not open database\n");
      goto error;
    }

  ret = db_check_version();
  if (ret < 0)
    {
      DPRINTF(E_FATAL, L_DB, "Database version check errored out, incompatible database\n");

      db_perthread_deinit();
      goto error;
    }
  else if (ret > 0)
    {
      DPRINTF(E_LOG, L_DB, "Could not check database version, trying DB init\n");

      ret = db_init_tables(hdl);
      if (ret < 0)
	{
	  DPRINTF(E_FATAL, L_DB, "Could not create tables\n");
	  db_perthread_deinit();
	  goto error;
	}
    }

  db_set_cfg_names();

  CHECK_ERR(L_DB, db_files_get_count(&files, NULL, NULL));
  CHECK_ERR(L_DB, db_pl_get_count(&pls));

  db_admin_setint64(DB_ADMIN_START_TIME, (int64_t) time(NULL));

  db_perthread_deinit();

  DPRINTF(E_LOG, L_DB, "Database OK with %" PRIu32 " active files and %" PRIu32 " active playlists\n", files, pls);

  rng_init(&shuffle_rng);

  return 0;

 error:
  return -1;
}

void
db_deinit(void)
{
  sqlite3_shutdown();
}
