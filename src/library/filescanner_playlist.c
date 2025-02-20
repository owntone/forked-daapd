/*
 * Copyright (C) 2015-2017 Espen Jürgensen <espenjurgensen@gmail.com>
 * Copyright (C) 2009-2010 Julien BLACHE <jb@jblache.org>
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
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

#include "conffile.h"
#include "logger.h"
#include "db.h"
#include "library/filescanner.h"
#include "misc.h"
#include "library.h"

enum playlist_type
{
  PLAYLIST_UNKNOWN = 0,
  PLAYLIST_PLS,
  PLAYLIST_M3U,
  PLAYLIST_SMART,
};

static enum playlist_type
playlist_type(const char *path)
{
  char *ptr;

  ptr = strrchr(path, '.');
  if (!ptr)
    return PLAYLIST_UNKNOWN;

  if (strcasecmp(ptr, ".m3u") == 0)
    return PLAYLIST_M3U;
  else if (strcasecmp(ptr, ".m3u8") == 0)
    return PLAYLIST_M3U;
  else if (strcasecmp(ptr, ".pls") == 0)
    return PLAYLIST_PLS;
  else if (strcasecmp(ptr, ".smartpl") == 0)
    return PLAYLIST_SMART;
  else
    return PLAYLIST_UNKNOWN;
}

static int
extinf_read(char **artist, char **title, const char *tag)
{
  char *ptr;

  ptr = strchr(tag, ',');
  if (!ptr || strlen(ptr) < 2)
    return -1;

  *artist = strdup(ptr + 1);

  ptr = strstr(*artist, " -");
  if (ptr && strlen(ptr) > 3)
    *title = strdup(ptr + 3);
  else
    *title = strdup("");
  if (ptr)
    *ptr = '\0';

  return 0;
}

static int
extval_read(char **val, const char *tag)
{
  char *ptr;

  ptr = strchr(tag, ':');
  if (!ptr || strlen(ptr) < 2)
    return -1;

  *val = strdup(ptr + 1);
  return 0;
}

// Get metadata from a EXTINF or EXTALB tag
static int
exttag_read(struct media_file_info *mfi, const char *tag)
{
  char *artist;
  char *title;
  char *val;

  if (strncmp(tag, "#EXTINF:", strlen("#EXTINF:")) == 0 && extinf_read(&artist, &title, tag) == 0)
    {
      free(mfi->artist);
      free(mfi->title);
      mfi->artist = artist;
      mfi->title = title;
      if (!mfi->album_artist)
	mfi->album_artist = strdup(artist);
      return 0;
    }
  if (strncmp(tag, "#EXTALB:", strlen("#EXTALB:")) == 0 && extval_read(&val, tag) == 0)
    {
      free(mfi->album);
      mfi->album = val;
      return 0;
    }
  if (strncmp(tag, "#EXTART:", strlen("#EXTART:")) == 0 && extval_read(&val, tag) == 0)
    {
      free(mfi->album_artist);
      mfi->album_artist = val;
      return 0;
    }
  if (strncmp(tag, "#EXTGENRE:", strlen("#EXTGENRE:")) == 0 && extval_read(&val, tag) == 0)
    {
      free(mfi->genre);
      mfi->genre = val;
      return 0;
    }

  return -1;
}

void
scan_metadata_stream(struct media_file_info *mfi, const char *path)
{
  char *pos;
  int ret;

  mfi->path = strdup(path);
  mfi->virtual_path = safe_asprintf("/%s", mfi->path);

  pos = strchr(path, '#');
  if (pos)
    mfi->fname = strdup(pos+1);
  else
    mfi->fname = strdup(filename_from_path(mfi->path));

  mfi->data_kind = DATA_KIND_HTTP;
  mfi->time_modified = time(NULL);
  mfi->directory_id = DIR_HTTP;
  mfi->scan_kind = SCAN_KIND_FILES;

  ret = scan_metadata_ffmpeg(mfi, NULL, path);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_SCAN, "Playlist URL '%s' is unavailable for probe/metadata, assuming MP3 encoding\n", path);
      mfi->type = strdup("mp3");
      mfi->codectype = strdup("mpeg");
      mfi->description = strdup("MPEG audio file");
    }

  if (!mfi->title)
    mfi->title = strdup(mfi->fname);
}

static int
process_nested_playlist(int parent_id, const char *path)
{
  struct playlist_info *pli;
  char *deref = NULL;
  int ret;

  // First set the type of the parent playlist to folder
  pli = db_pl_fetch_byid(parent_id);
  if (!pli)
    goto error;

  pli->type = PL_FOLDER;
  pli->scan_kind = SCAN_KIND_FILES;
  ret = library_playlist_save(pli);
  if (ret < 0)
    goto error;

  free_pli(pli, 0);

  deref = realpath(path, NULL);
  if (!deref)
    {
      DPRINTF(E_LOG, L_SCAN, "Could not dereference path '%s': %s\n", path, strerror(errno));
      return -1;
    }

  // Do we already have the playlist in the database?
  pli = db_pl_fetch_bypath(deref);
  if (!pli)
    {
      pli = calloc(1, sizeof(struct playlist_info));
      ret = playlist_fill(pli, deref);
      if (ret < 0)
	goto error;

      // This is a "trick" to make sure the nested playlist will be scanned.
      // Otherwise what could happen is that we save the playlist with current
      // db_timestamp, and when the scanner finds the actual playlist it will
      // conclude from the timestamp that the playlist is unchanged, and thus
      // it would never be scanned.
      pli->db_timestamp = 1;
    }

  pli->parent_id = parent_id;

  ret = library_playlist_save(pli);
  if (ret < 0)
    goto error;

  free_pli(pli, 0);
  free(deref);

  return 0;

 error:
  DPRINTF(E_LOG, L_SCAN, "Error processing nested playlist '%s' in playlist %d\n", path, parent_id);
  free_pli(pli, 0);
  free(deref);

  return -1;
}

static int
process_url(int pl_id, const char *path, struct media_file_info *mfi)
{
  struct media_file_info m3u;
  int ret;

  mfi->id = db_file_id_bypath(path);

  if (cfg_getbool(cfg_getsec(cfg, "library"), "m3u_overrides"))
    {
      memset(&m3u, 0, sizeof(struct media_file_info));

      m3u.artist = safe_strdup(mfi->artist);
      m3u.album_artist = safe_strdup(mfi->album_artist);
      m3u.album = safe_strdup(mfi->album);
      m3u.title = safe_strdup(mfi->title);
      m3u.genre = safe_strdup(mfi->genre);

      scan_metadata_stream(mfi, path);

      if (m3u.artist)
	swap_pointers(&mfi->artist, &m3u.artist);
      if (m3u.album_artist)
	swap_pointers(&mfi->album_artist, &m3u.album_artist);
      if (m3u.album)
	swap_pointers(&mfi->album, &m3u.album);
      if (m3u.title)
	swap_pointers(&mfi->title, &m3u.title);
      if (m3u.genre)
	swap_pointers(&mfi->genre, &m3u.genre);

      free_mfi(&m3u, 1);
    }
  else
    scan_metadata_stream(mfi, path);

  ret = library_media_save(mfi, NULL);
  if (ret < 0)
    return -1;

  return db_pl_add_item_bypath(pl_id, path);
}

static int
process_regular_file(int pl_id, char *path)
{
  struct query_params qp;
  char filter[PATH_MAX];
  const char *a;
  const char *b;
  char *dbpath;
  char *winner;
  int score;
  int i;
  int ret;

  // Playlist might be from Windows so we change backslash to forward slash
  for (i = 0; i < strlen(path); i++)
    {
      if (path[i] == '\\')
	path[i] = '/';
    }

  ret = db_snprintf(filter, sizeof(filter), "f.fname = '%q' COLLATE NOCASE", filename_from_path(path));
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_SCAN, "Path in playlist is too long: '%s'\n", path);
      return -1;
    }

  memset(&qp, 0, sizeof(struct query_params));

  qp.type = Q_BROWSE_PATH;
  qp.sort = S_NONE;
  qp.filter = filter;

  ret = db_query_start(&qp);
  if (ret < 0)
    {
      db_query_end(&qp);
      return -1;
    }

  winner = NULL;
  score = 0;
  while ((db_query_fetch_string(&dbpath, &qp) == 0) && dbpath)
    {
      if (qp.results == 1)
	{
	  free(winner); // This is just here to keep scan-build happy
	  winner = strdup(dbpath);
	  break;
	}

      for (i = 0, a = NULL, b = NULL; (parent_dir(&a, path) == 0) && (parent_dir(&b, dbpath) == 0) && (strcasecmp(a, b) == 0); i++)
	;

      DPRINTF(E_SPAM, L_SCAN, "Comparison of '%s' and '%s' gave score %d\n", dbpath, path, i);

      if (i > score)
	{
	  free(winner);
	  winner = strdup(dbpath);
	  score = i;
	}
      else if (i == score)
	{
	  free(winner);
	  winner = NULL;
	}
    }

  db_query_end(&qp);

  if (!winner)
    {
      DPRINTF(E_LOG, L_SCAN, "No file in the library matches playlist entry '%s'\n", path);
      return -1;
    }

  DPRINTF(E_DBG, L_SCAN, "Adding '%s' to playlist %d (results %d)\n", winner, pl_id, qp.results);

  db_pl_add_item_bypath(pl_id, winner);
  free(winner);

  return 0;
}

static int
playlist_prepare(const char *path, time_t mtime)
{
  struct playlist_info *pli;
  char *old_title;
  int pl_id;

  pli = db_pl_fetch_bypath(path);
  if (!pli)
    {
      DPRINTF(E_LOG, L_SCAN, "New playlist found, processing '%s'\n", path);

      pl_id = playlist_add(path);
      if (pl_id < 0)
	{
	  DPRINTF(E_LOG, L_SCAN, "Error adding playlist '%s'\n", path);
	  return -1;
	}

      DPRINTF(E_INFO, L_SCAN, "Added new playlist as id %d\n", pl_id);
      return pl_id;
    }

  // So we already have the playlist, but maybe it has been renamed
  old_title = pli->title;
  pli->title = title_from_path(path);

  if (strcasecmp(old_title, pli->title) != 0)
    db_pl_update(pli);
  else
    db_pl_ping(pli->id);

  free(old_title);

  // mtime == db_timestamp is also treated as a modification because some editors do
  // stuff like 1) close the file with no changes (leading us to update db_timestamp),
  // 2) copy over a modified version from a tmp file (which may result in a mtime that
  // is equal to the newly updated db_timestamp)
  if (mtime && (pli->db_timestamp > mtime))
    {
      DPRINTF(E_LOG, L_SCAN, "Unchanged playlist found, not processing '%s'\n", path);

      // Protect this playlist's radio stations from purge after scan
      db_pl_ping_items_bymatch("http://", pli->id);
      db_pl_ping_items_bymatch("https://", pli->id);
      free_pli(pli, 0);
      return -1;
    }

  DPRINTF(E_LOG, L_SCAN, "Modified playlist found, processing '%s'\n", path);

  pl_id = pli->id;
  free_pli(pli, 0);

  db_pl_clear_items(pl_id);

  return pl_id;
}

void
scan_playlist(const char *file, time_t mtime, int dir_id)
{
  FILE *fp;
  struct media_file_info mfi;
  char buf[PATH_MAX];
  char *path;
  size_t len;
  int pl_id;
  int pl_format;
  int ntracks;
  int nadded;
  int ret;

  pl_format = playlist_type(file);
  if (pl_format != PLAYLIST_M3U && pl_format != PLAYLIST_PLS)
    return;

  // Will create or update the playlist entry in the database
  pl_id = playlist_prepare(file, mtime);
  if (pl_id < 0)
    return; // Not necessarily an error, could also be that the playlist hasn't changed

  fp = fopen(file, "r");
  if (!fp)
    {
      DPRINTF(E_LOG, L_SCAN, "Could not open playlist '%s': %s\n", file, strerror(errno));
      return;
    }

  db_transaction_begin();

  memset(&mfi, 0, sizeof(struct media_file_info));
  ntracks = 0;
  nadded = 0;

  while (fgets(buf, sizeof(buf), fp) != NULL)
    {
      len = strlen(buf);

      // Check for and strip byte-order mark
      if (memcmp("\xef\xbb\xbf", buf, 3) == 0)
	memmove(buf, buf + 3, len - 3 + 1);

      // rtrim and check that length is sane (ignore blank lines)
      while ((len > 0) && isspace(buf[len - 1]))
	{
	  len--;
	  buf[len] = '\0';
	}
      if (len < 1)
	continue;

      // Saves metadata in mfi if EXT metadata line
      if ((pl_format == PLAYLIST_M3U) && (exttag_read(&mfi, buf) == 0))
	continue;

      // For pls files we are only interested in the part after the FileX= entry
      path = NULL;
      if ((pl_format == PLAYLIST_PLS) && (strncasecmp(buf, "file", strlen("file")) == 0) && (path = strchr(buf, '=')))
	path++;
      else if (pl_format == PLAYLIST_M3U)
	path = buf;

      if (!path || path[0] == '\0' || path[0] == '#')
	continue;

      // URLs and playlists will be added to library, tracks should already be there
      if (net_is_http_or_https(path))
	ret = process_url(pl_id, path, &mfi);
      else if (playlist_type(path) != PLAYLIST_UNKNOWN)
	ret = process_nested_playlist(pl_id, path);
      else
	ret = process_regular_file(pl_id, path);

      ntracks++;
      if (ntracks % 200 == 0)
	{
	  DPRINTF(E_LOG, L_SCAN, "Processed %d items...\n", ntracks);
	  db_transaction_end();
	  db_transaction_begin();
	}

      if (ret == 0)
	nadded++;

      // Clean up in preparation for next item
      free_mfi(&mfi, 1);
    }

  db_transaction_end();

  // In case we had some m3u ext metadata that we never got to use, free it now
  // (no risk of double free when the free_mfi()'s are content_only)
  free_mfi(&mfi, 1);

  if (!feof(fp))
    DPRINTF(E_LOG, L_SCAN, "Error reading playlist '%s' (only added %d tracks): %s\n", file, nadded, strerror(errno));
  else
    DPRINTF(E_LOG, L_SCAN, "Done processing playlist, added/modified %d items\n", nadded);

  fclose(fp);
}
