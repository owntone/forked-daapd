
/*
 * Copyright (C) 2009-2011 Julien BLACHE <jb@jblache.org>
 * Copyright (C) 2010 Kai Elwert <elwertk@googlemail.com>
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

#include <sqlite3.h>
#include <stddef.h>

#include "db_init.h"
#include "logger.h"


#define T_ADMIN						\
  "CREATE TABLE IF NOT EXISTS admin("			\
  "   key   VARCHAR(32) PRIMARY KEY NOT NULL,"		\
  "   value VARCHAR(255) NOT NULL"			\
  ");"

#define T_FILES						\
  "CREATE TABLE IF NOT EXISTS files ("			\
  "   id                 INTEGER PRIMARY KEY NOT NULL,"	\
  "   path               VARCHAR(4096) NOT NULL,"	\
  "   virtual_path       VARCHAR(4096) DEFAULT NULL,"	\
  "   fname              VARCHAR(255) NOT NULL,"	\
  "   directory_id       INTEGER DEFAULT 0,"		\
  "   title              VARCHAR(1024) DEFAULT NULL COLLATE DAAP,"	\
  "   artist             VARCHAR(1024) DEFAULT NULL COLLATE DAAP,"	\
  "   album              VARCHAR(1024) NOT NULL COLLATE DAAP,"		\
  "   album_artist       VARCHAR(1024) NOT NULL COLLATE DAAP,"		\
  "   genre              VARCHAR(255) DEFAULT NULL COLLATE DAAP,"	\
  "   comment            VARCHAR(4096) DEFAULT NULL COLLATE DAAP,"	\
  "   type               VARCHAR(255) DEFAULT NULL COLLATE DAAP,"	\
  "   composer           VARCHAR(1024) DEFAULT NULL COLLATE DAAP,"	\
  "   orchestra          VARCHAR(1024) DEFAULT NULL COLLATE DAAP,"	\
  "   conductor          VARCHAR(1024) DEFAULT NULL COLLATE DAAP,"	\
  "   grouping           VARCHAR(1024) DEFAULT NULL COLLATE DAAP,"	\
  "   url                VARCHAR(1024) DEFAULT NULL,"	\
  "   bitrate            INTEGER DEFAULT 0,"		\
  "   samplerate         INTEGER DEFAULT 0,"		\
  "   song_length        INTEGER DEFAULT 0,"		\
  "   file_size          INTEGER DEFAULT 0,"		\
  "   year               INTEGER DEFAULT 0,"		\
  "   date_released      INTEGER DEFAULT 0,"		\
  "   track              INTEGER DEFAULT 0,"		\
  "   total_tracks       INTEGER DEFAULT 0,"		\
  "   disc               INTEGER DEFAULT 0,"		\
  "   total_discs        INTEGER DEFAULT 0,"		\
  "   bpm                INTEGER DEFAULT 0,"		\
  "   compilation        INTEGER DEFAULT 0,"		\
  "   artwork            INTEGER DEFAULT 0,"		\
  "   rating             INTEGER DEFAULT 0,"		\
  "   play_count         INTEGER DEFAULT 0,"		\
  "   skip_count         INTEGER DEFAULT 0,"		\
  "   seek               INTEGER DEFAULT 0,"		\
  "   data_kind          INTEGER DEFAULT 0,"		\
  "   media_kind         INTEGER DEFAULT 0,"		\
  "   item_kind          INTEGER DEFAULT 0,"		\
  "   description        INTEGER DEFAULT 0,"		\
  "   db_timestamp       INTEGER DEFAULT 0,"		\
  "   time_added         INTEGER DEFAULT 0,"		\
  "   time_modified      INTEGER DEFAULT 0,"		\
  "   time_played        INTEGER DEFAULT 0,"		\
  "   time_skipped       INTEGER DEFAULT 0,"		\
  "   disabled           INTEGER DEFAULT 0,"		\
  "   sample_count       INTEGER DEFAULT 0,"		\
  "   codectype          VARCHAR(5) DEFAULT NULL,"	\
  "   idx                INTEGER NOT NULL,"		\
  "   has_video          INTEGER DEFAULT 0,"		\
  "   contentrating      INTEGER DEFAULT 0,"		\
  "   bits_per_sample    INTEGER DEFAULT 0,"		\
  "   tv_series_name     VARCHAR(1024) DEFAULT NULL COLLATE DAAP,"	\
  "   tv_episode_num_str VARCHAR(1024) DEFAULT NULL COLLATE DAAP,"	\
  "   tv_network_name    VARCHAR(1024) DEFAULT NULL COLLATE DAAP,"	\
  "   tv_episode_sort    INTEGER NOT NULL,"		\
  "   tv_season_num      INTEGER NOT NULL,"		\
  "   songartistid       INTEGER DEFAULT 0,"		\
  "   songalbumid        INTEGER DEFAULT 0,"		\
  "   title_sort         VARCHAR(1024) DEFAULT NULL COLLATE DAAP,"	\
  "   artist_sort        VARCHAR(1024) DEFAULT NULL COLLATE DAAP,"	\
  "   album_sort         VARCHAR(1024) DEFAULT NULL COLLATE DAAP,"	\
  "   album_artist_sort  VARCHAR(1024) DEFAULT NULL COLLATE DAAP,"	\
  "   composer_sort      VARCHAR(1024) DEFAULT NULL COLLATE DAAP,"	\
  "   channels           INTEGER DEFAULT 0,"		\
  "   usermark           INTEGER DEFAULT 0,"		\
  "   scan_kind          INTEGER DEFAULT 0,"		\
  "   lyrics             TEXT DEFAULT NULL COLLATE DAAP"		\
  ");"

#define T_PL					\
  "CREATE TABLE IF NOT EXISTS playlists ("		\
  "   id             INTEGER PRIMARY KEY NOT NULL,"	\
  "   title          VARCHAR(255) NOT NULL COLLATE DAAP,"	\
  "   type           INTEGER NOT NULL,"			\
  "   query          VARCHAR(1024),"			\
  "   db_timestamp   INTEGER NOT NULL,"			\
  "   disabled       INTEGER DEFAULT 0,"		\
  "   path           VARCHAR(4096),"			\
  "   idx            INTEGER NOT NULL,"			\
  "   special_id     INTEGER DEFAULT 0,"		\
  "   virtual_path   VARCHAR(4096),"			\
  "   parent_id      INTEGER DEFAULT 0,"		\
  "   directory_id   INTEGER DEFAULT 0,"		\
  "   query_order    VARCHAR(1024),"			\
  "   query_limit    INTEGER DEFAULT 0,"		\
  "   media_kind     INTEGER DEFAULT 1,"		\
  "   artwork_url    VARCHAR(4096) DEFAULT NULL,"	\
  "   scan_kind      INTEGER DEFAULT 0"			\
  ");"

#define T_PLITEMS				\
  "CREATE TABLE IF NOT EXISTS playlistitems ("		\
  "   id             INTEGER PRIMARY KEY NOT NULL,"	\
  "   playlistid     INTEGER NOT NULL,"			\
  "   filepath       VARCHAR(4096) NOT NULL"		\
  ");"

#define T_GROUPS							\
  "CREATE TABLE IF NOT EXISTS groups ("					\
  "   id             INTEGER PRIMARY KEY NOT NULL,"			\
  "   type           INTEGER NOT NULL,"					\
  "   name           VARCHAR(1024) NOT NULL COLLATE DAAP,"		\
  "   persistentid   INTEGER NOT NULL,"					\
  "CONSTRAINT groups_type_unique_persistentid UNIQUE (type, persistentid)" \
  ");"

#define T_PAIRINGS					\
  "CREATE TABLE IF NOT EXISTS pairings("		\
  "   remote         VARCHAR(64) PRIMARY KEY NOT NULL,"	\
  "   name           VARCHAR(255) NOT NULL,"		\
  "   guid           VARCHAR(16) NOT NULL"		\
  ");"

#define T_SPEAKERS					\
  "CREATE TABLE IF NOT EXISTS speakers("		\
  "   id             INTEGER PRIMARY KEY NOT NULL,"	\
  "   selected       INTEGER NOT NULL,"			\
  "   volume         INTEGER NOT NULL,"			\
  "   name           VARCHAR(255) DEFAULT NULL,"       \
  "   auth_key       VARCHAR(2048) DEFAULT NULL,"      \
  "   format         INTEGER DEFAULT 0"                \
  ");"

#define T_INOTIFY					\
  "CREATE TABLE IF NOT EXISTS inotify ("		\
  "   wd          INTEGER PRIMARY KEY NOT NULL,"	\
  "   cookie      INTEGER NOT NULL,"			\
  "   path        VARCHAR(4096) NOT NULL"		\
  ");"

#define T_DIRECTORIES						\
  "CREATE TABLE IF NOT EXISTS directories ("			\
  "   id                  INTEGER PRIMARY KEY NOT NULL,"	\
  "   virtual_path        VARCHAR(4096) NOT NULL,"		\
  "   db_timestamp        INTEGER DEFAULT 0,"			\
  "   disabled            INTEGER DEFAULT 0,"			\
  "   parent_id           INTEGER DEFAULT 0,"			\
  "   path                VARCHAR(4096) DEFAULT NULL,"		\
  "   scan_kind           INTEGER DEFAULT 0"			\
  ");"

#define T_QUEUE								\
  "CREATE TABLE IF NOT EXISTS queue ("					\
  "   id                  INTEGER PRIMARY KEY AUTOINCREMENT,"		\
  "   file_id             INTEGER NOT NULL,"				\
  "   pos                 INTEGER NOT NULL,"				\
  "   shuffle_pos         INTEGER NOT NULL,"				\
  "   data_kind           INTEGER NOT NULL,"				\
  "   media_kind          INTEGER NOT NULL,"				\
  "   song_length         INTEGER NOT NULL,"				\
  "   path                VARCHAR(4096) NOT NULL,"			\
  "   virtual_path        VARCHAR(4096) NOT NULL,"			\
  "   title               VARCHAR(1024) DEFAULT NULL COLLATE DAAP,"	\
  "   artist              VARCHAR(1024) DEFAULT NULL COLLATE DAAP,"	\
  "   album_artist        VARCHAR(1024) NOT NULL COLLATE DAAP,"		\
  "   album               VARCHAR(1024) NOT NULL COLLATE DAAP,"		\
  "   genre               VARCHAR(255) DEFAULT NULL COLLATE DAAP,"	\
  "   songalbumid         INTEGER NOT NULL,"				\
  "   time_modified       INTEGER DEFAULT 0,"				\
  "   artist_sort         VARCHAR(1024) DEFAULT NULL COLLATE DAAP,"	\
  "   album_sort          VARCHAR(1024) DEFAULT NULL COLLATE DAAP,"	\
  "   album_artist_sort   VARCHAR(1024) DEFAULT NULL COLLATE DAAP,"	\
  "   year                INTEGER DEFAULT 0,"				\
  "   track               INTEGER DEFAULT 0,"				\
  "   disc                INTEGER DEFAULT 0,"				\
  "   artwork_url         VARCHAR(4096) DEFAULT NULL,"			\
  "   queue_version       INTEGER DEFAULT 0,"				\
  "   composer            VARCHAR(1024) DEFAULT NULL,"			\
  "   songartistid        INTEGER NOT NULL,"				\
  "   type                VARCHAR(8) DEFAULT NULL,"			\
  "   bitrate             INTEGER DEFAULT 0,"				\
  "   samplerate          INTEGER DEFAULT 0,"				\
  "   channels            INTEGER DEFAULT 0"				\
  ");"

#define T_FILES_METADATA						\
  "CREATE TABLE IF NOT EXISTS files_metadata ("				\
  "   file_id            INTEGER NOT NULL,"				\
  "   songalbumid        INTEGER NOT NULL,"				\
  "   songartistid       INTEGER NOT NULL,"				\
  "   metadata_kind      INTEGER NOT NULL,"				\
  "   idx                INTEGER DEFAULT 0,"				\
  "   value              TEXT NOT NULL COLLATE DAAP"			\
  ");"

#define Q_PL1								\
  "INSERT INTO playlists (id, title, type, query, db_timestamp, path, idx, special_id)" \
  " VALUES(1, 'Library', 0, '1 = 1', 0, '', 0, 0);"

#define Q_PL2								\
  "INSERT INTO playlists (id, title, type, query, db_timestamp, path, idx, special_id)" \
  " VALUES(2, 'Music', 0, 'f.media_kind = 1', 0, '', 0, 6);"

#define Q_PL3								\
  "INSERT INTO playlists (id, title, type, query, db_timestamp, path, idx, special_id)" \
  " VALUES(3, 'Movies', 0, 'f.media_kind = 2', 0, '', 0, 4);"

#define Q_PL4								\
  "INSERT INTO playlists (id, title, type, query, db_timestamp, path, idx, special_id)" \
  " VALUES(4, 'TV Shows', 0, 'f.media_kind = 64', 0, '', 0, 5);"

#define Q_PL5								\
  "INSERT INTO playlists (id, title, type, query, db_timestamp, path, idx, special_id)" \
  " VALUES(5, 'Podcasts', 0, 'f.media_kind = 4', 0, '', 0, 1);"

#define Q_PL6								\
  "INSERT INTO playlists (id, title, type, query, db_timestamp, path, idx, special_id)" \
  " VALUES(6, 'Audiobooks', 0, 'f.media_kind = 8', 0, '', 0, 7);"

/* These are the remaining automatically-created iTunes playlists, but
 * their query is unknown
  " VALUES(6, 'iTunes U', 0, 'media_kind = 256', 0, '', 0, 13);"
  " VALUES(8, 'Purchased', 0, 'media_kind = 1024', 0, '', 0, 8);"
 */


#define Q_DIR1 \
  "INSERT INTO directories (id, virtual_path, db_timestamp, disabled, parent_id, path)" \
  " VALUES (1, '/', 0, 0, 0, NULL);"
#define Q_DIR2 \
  "INSERT INTO directories (id, virtual_path, db_timestamp, disabled, parent_id, path)" \
  " VALUES (2, '/file:', 0, 0, 1, '/');"
#define Q_DIR3 \
  "INSERT INTO directories (id, virtual_path, db_timestamp, disabled, parent_id, path)" \
  " VALUES (3, '/http:', 0, 0, 1, NULL);"
#define Q_DIR4 \
  "INSERT INTO directories (id, virtual_path, db_timestamp, disabled, parent_id, path)" \
  " VALUES (4, '/spotify:', 0, 4294967296, 1, NULL);"

#define Q_QUEUE_VERSION			\
  "INSERT INTO admin (key, value) VALUES ('queue_version', '0');"

#define Q_SCVER_MAJOR					\
  "INSERT INTO admin (key, value) VALUES ('schema_version_major', '%d');"
#define Q_SCVER_MINOR					\
  "INSERT INTO admin (key, value) VALUES ('schema_version_minor', '%02d');"

struct db_init_query {
  char *query;
  char *desc;
};

static const struct db_init_query db_init_table_queries[] =
  {
    { T_ADMIN,     "create table admin" },
    { T_FILES,     "create table files" },
    { T_PL,        "create table playlists" },
    { T_PLITEMS,   "create table playlistitems" },
    { T_GROUPS,    "create table groups" },
    { T_PAIRINGS,  "create table pairings" },
    { T_SPEAKERS,  "create table speakers" },
    { T_INOTIFY,   "create table inotify" },
    { T_DIRECTORIES, "create table directories" },
    { T_QUEUE,     "create table queue" },
    { T_FILES_METADATA, "create table files_metadata" },

    { Q_PL1,       "create default playlist" },
    { Q_PL2,       "create default smart playlist 'Music'" },
    { Q_PL3,       "create default smart playlist 'Movies'" },
    { Q_PL4,       "create default smart playlist 'TV Shows'" },
    { Q_PL5,       "create default smart playlist 'Podcasts'" },
    { Q_PL6,       "create default smart playlist 'Audiobooks'" },

    { Q_DIR1,      "create default root directory '/'" },
    { Q_DIR2,      "create default base directory '/file:'" },
    { Q_DIR3,      "create default base directory '/http:'" },
    { Q_DIR4,      "create default base directory '/spotify:'" },

    { Q_QUEUE_VERSION, "initialize queue version" },
  };


/* Indices must be prefixed with idx_ for db_drop_indices() to id them */

#define I_RESCAN				\
  "CREATE INDEX IF NOT EXISTS idx_rescan ON files(path, db_timestamp);"

#define I_FNAME					\
  "CREATE INDEX IF NOT EXISTS idx_fname ON files(disabled, fname COLLATE NOCASE);"

#define I_SONGARTISTID				\
  "CREATE INDEX IF NOT EXISTS idx_sari ON files(songartistid);"

/* Used by Q_GROUP_ALBUMS */
#define I_SONGALBUMID				\
  "CREATE INDEX IF NOT EXISTS idx_sali ON files(songalbumid, disabled, media_kind, album_sort, disc, track);"

/* Used by Q_GROUP_ARTISTS */
#define I_STATEMKINDSARI				\
  "CREATE INDEX IF NOT EXISTS idx_state_mkind_sari ON files(disabled, media_kind, songartistid);"

#define I_STATEMKINDSALI				\
  "CREATE INDEX IF NOT EXISTS idx_state_mkind_sali ON files(disabled, media_kind, songalbumid);"

/* Used by Q_BROWSE_ALBUM */
#define I_ALBUM					\
  "CREATE INDEX IF NOT EXISTS idx_album ON files(disabled, album_sort, album, media_kind);"

/* Used by Q_BROWSE_ARTIST */
#define I_ALBUMARTIST				\
  "CREATE INDEX IF NOT EXISTS idx_albumartist ON files(disabled, album_artist_sort, album_artist, media_kind);"

/* Used by Q_BROWSE_COMPOSERS */
#define I_COMPOSER				\
  "CREATE INDEX IF NOT EXISTS idx_composer ON files(disabled, composer_sort, composer, media_kind);"

/* Used by Q_BROWSE_GENRES */
#define I_GENRE					\
  "CREATE INDEX IF NOT EXISTS idx_genre ON files(disabled, genre, media_kind);"

/* Used by Q_PLITEMS for smart playlists */
#define I_TITLE					\
  "CREATE INDEX IF NOT EXISTS idx_title ON files(disabled, title_sort, media_kind);"

#define I_FILELIST					\
  "CREATE INDEX IF NOT EXISTS idx_filelist ON files(disabled, virtual_path, time_modified);"

#define I_FILE_DIR					\
  "CREATE INDEX IF NOT EXISTS idx_file_dir ON files(disabled, directory_id);"

#define I_DATE_RELEASED                    \
  "CREATE INDEX IF NOT EXISTS idx_date_released ON files(disabled, date_released DESC, media_kind);"

#define I_PL_PATH				\
  "CREATE INDEX IF NOT EXISTS idx_pl_path ON playlists(path);"

#define I_PL_DISABLED				\
  "CREATE INDEX IF NOT EXISTS idx_pl_disabled ON playlists(disabled, type, virtual_path, db_timestamp);"

#define I_PL_DIR					\
  "CREATE INDEX IF NOT EXISTS idx_pl_dir ON files(disabled, directory_id);"

#define I_FILEPATH							\
  "CREATE INDEX IF NOT EXISTS idx_filepath ON playlistitems(filepath ASC);"

#define I_PLITEMID							\
  "CREATE INDEX IF NOT EXISTS idx_playlistid ON playlistitems(playlistid, filepath);"

#define I_GRP_PERSIST				\
  "CREATE INDEX IF NOT EXISTS idx_grp_persist ON groups(persistentid);"

#define I_PAIRING				\
  "CREATE INDEX IF NOT EXISTS idx_pairingguid ON pairings(guid);"

#define I_DIR_VPATH				\
  "CREATE INDEX IF NOT EXISTS idx_dir_vpath ON directories(disabled, virtual_path);"

#define I_DIR_PARENT				\
  "CREATE INDEX IF NOT EXISTS idx_dir_parentid ON directories(parent_id);"

#define I_QUEUE_POS				\
  "CREATE INDEX IF NOT EXISTS idx_queue_pos ON queue(pos);"

#define I_QUEUE_SHUFFLEPOS				\
  "CREATE INDEX IF NOT EXISTS idx_queue_shufflepos ON queue(shuffle_pos);"

#define I_MD_FILEID_TYPE_IDX				\
  "CREATE INDEX IF NOT EXISTS idx_filesmd_fileid_type_idx ON files_metadata(file_id, metadata_kind, idx);"

#define I_MD_ALBUMPERSID_TYPE_IDX				\
  "CREATE INDEX IF NOT EXISTS idx_filesmd_albumid_type_idx ON files_metadata(songalbumid, metadata_kind, idx);"

#define I_MD_ARTISTPERSID_TYPE_IDX				\
  "CREATE INDEX IF NOT EXISTS idx_filesmd_artistid_type_idx ON files_metadata(songartistid, metadata_kind, idx);"

static const struct db_init_query db_init_index_queries[] =
  {
    { I_RESCAN,    "create rescan index" },
    { I_FNAME,     "create filename index" },
    { I_SONGARTISTID, "create songartistid index" },
    { I_SONGALBUMID, "create songalbumid index" },
    { I_STATEMKINDSARI, "create state/mkind/sari index" },
    { I_STATEMKINDSALI, "create state/mkind/sali index" },

    { I_ALBUMARTIST, "create album_artist index" },
    { I_COMPOSER,  "create composer index" },
    { I_GENRE,     "create genre index" },
    { I_TITLE,     "create title index" },
    { I_ALBUM,     "create album index" },
    { I_FILELIST,  "create filelist index" },
    { I_FILE_DIR,  "create file dir index" },
    { I_DATE_RELEASED, "create date_released index" },

    { I_PL_PATH,   "create playlist path index" },
    { I_PL_DISABLED, "create playlist state index" },
    { I_PL_DIR, "create playlist dir index" },

    { I_FILEPATH,  "create file path index" },
    { I_PLITEMID,  "create playlist id index" },

    { I_GRP_PERSIST, "create groups persistentid index" },

    { I_PAIRING,   "create pairing guid index" },

    { I_DIR_VPATH,   "create directories disabled_virtualpath index" },
    { I_DIR_PARENT,  "create directories parentid index" },

    { I_QUEUE_POS,  "create queue pos index" },
    { I_QUEUE_SHUFFLEPOS,  "create queue shuffle pos index" },

    { I_MD_FILEID_TYPE_IDX,  "create files_metadata file_id type idx index" },
  };


/* Triggers must be prefixed with trg_ for db_drop_triggers() to id them */

#define TRG_GROUPS_INSERT										\
  "CREATE TRIGGER trg_groups_insert AFTER INSERT ON files FOR EACH ROW"					\
  " BEGIN"												\
  "   INSERT OR IGNORE INTO groups (type, name, persistentid) VALUES (1, NEW.album, NEW.songalbumid);"	\
  "   INSERT OR IGNORE INTO groups (type, name, persistentid) VALUES (2, NEW.album_artist, NEW.songartistid);"	\
  " END;"

#define TRG_GROUPS_UPDATE										\
  "CREATE TRIGGER trg_groups_update AFTER UPDATE OF songartistid, songalbumid ON files FOR EACH ROW"	\
  " BEGIN"												\
  "   INSERT OR IGNORE INTO groups (type, name, persistentid) VALUES (1, NEW.album, NEW.songalbumid);"	\
  "   INSERT OR IGNORE INTO groups (type, name, persistentid) VALUES (2, NEW.album_artist, NEW.songartistid);"	\
  " END;"

static const struct db_init_query db_init_trigger_queries[] =
  {
    { TRG_GROUPS_INSERT,           "create trigger trg_groups_insert" },
    { TRG_GROUPS_UPDATE,           "create trigger trg_groups_update" },
  };


int
db_init_indices(sqlite3 *hdl)
{
  char *errmsg;
  int i;
  int ret;

  for (i = 0; i < (sizeof(db_init_index_queries) / sizeof(db_init_index_queries[0])); i++)
    {
      DPRINTF(E_DBG, L_DB, "DB init index query: %s\n", db_init_index_queries[i].desc);

      ret = sqlite3_exec(hdl, db_init_index_queries[i].query, NULL, NULL, &errmsg);
      if (ret != SQLITE_OK)
	{
	  DPRINTF(E_FATAL, L_DB, "DB init error: %s\n", errmsg);

	  sqlite3_free(errmsg);
	  return -1;
	}
    }

  return 0;
}

int
db_init_triggers(sqlite3 *hdl)
{
  char *errmsg;
  int i;
  int ret;

  for (i = 0; i < (sizeof(db_init_trigger_queries) / sizeof(db_init_trigger_queries[0])); i++)
    {
      DPRINTF(E_DBG, L_DB, "DB init trigger query: %s\n", db_init_trigger_queries[i].desc);

      ret = sqlite3_exec(hdl, db_init_trigger_queries[i].query, NULL, NULL, &errmsg);
      if (ret != SQLITE_OK)
	{
	  DPRINTF(E_FATAL, L_DB, "DB init error: %s\n", errmsg);

	  sqlite3_free(errmsg);
	  return -1;
	}
    }

  return 0;
}

int
db_init_tables(sqlite3 *hdl)
{
  char *query;
  char *errmsg;
  int i;
  int ret;

  for (i = 0; i < (sizeof(db_init_table_queries) / sizeof(db_init_table_queries[0])); i++)
    {
      DPRINTF(E_DBG, L_DB, "DB init table query: %s\n", db_init_table_queries[i].desc);

      ret = sqlite3_exec(hdl, db_init_table_queries[i].query, NULL, NULL, &errmsg);
      if (ret != SQLITE_OK)
	{
	  DPRINTF(E_FATAL, L_DB, "DB init error: %s\n", errmsg);

	  sqlite3_free(errmsg);
	  return -1;
	}
    }

  query = sqlite3_mprintf(Q_SCVER_MAJOR, SCHEMA_VERSION_MAJOR);
  DPRINTF(E_DBG, L_DB, "DB init table query: %s\n", query);

  ret = sqlite3_exec(hdl, query, NULL, NULL, &errmsg);
  sqlite3_free(query);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_FATAL, L_DB, "DB init error: %s\n", errmsg);
      sqlite3_free(errmsg);
      return -1;
    }

  query = sqlite3_mprintf(Q_SCVER_MINOR, SCHEMA_VERSION_MINOR);
  DPRINTF(E_DBG, L_DB, "DB init table query: %s\n", query);

  ret = sqlite3_exec(hdl, query, NULL, NULL, &errmsg);
  sqlite3_free(query);
  if (ret != SQLITE_OK)
    {
      DPRINTF(E_FATAL, L_DB, "DB init error: %s\n", errmsg);
      sqlite3_free(errmsg);
      return -1;
    }

  ret = db_init_indices(hdl);
  if (ret < 0)
    {
      DPRINTF(E_FATAL, L_DB, "DB init error: failed to create indices\n");
      return -1;
    }

  ret = db_init_triggers(hdl);
  if (ret < 0)
    {
      DPRINTF(E_FATAL, L_DB, "DB init error: failed to create triggers\n");
      return -1;
    }

  return ret;
}

