/*
 * Copyright (C) 2009-2010 Julien BLACHE <jb@jblache.org>
 *
 * Bits and pieces from mt-daapd:
 * Copyright (C) 2003 Ron Pedde (ron@pedde.com)
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

#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/inotify.h>
#include <fcntl.h>
#include <dirent.h>
#include <pthread.h>
#ifdef HAVE_PTHREAD_NP_H
# include <pthread_np.h>
#endif

#include <event2/event.h>

#ifdef HAVE_REGEX_H
# include <regex.h>
#endif

#include "logger.h"
#include "db.h"
#include "library/filescanner.h"
#include "conffile.h"
#include "misc.h"
#include "remote_pairing.h"
#include "player.h"
#include "cache.h"
#include "artwork.h"
#include "commands.h"
#include "library.h"

#ifdef LASTFM
# include "lastfm.h"
#endif


#define F_SCAN_BULK    (1 << 0)
#define F_SCAN_RESCAN  (1 << 1)
#define F_SCAN_FAST    (1 << 2)
#define F_SCAN_MOVED   (1 << 3)
#define F_SCAN_METARESCAN  (1 << 4)

#define F_SCAN_TYPE_FILE         (1 << 0)
#define F_SCAN_TYPE_PODCAST      (1 << 1)
#define F_SCAN_TYPE_AUDIOBOOK    (1 << 2)
#define F_SCAN_TYPE_COMPILATION  (1 << 3)

#ifdef __linux__
#define INOTIFY_FLAGS  (IN_ATTRIB | IN_CREATE | IN_DELETE | IN_CLOSE_WRITE | IN_MOVE | IN_DELETE | IN_MOVE_SELF)
#else
#define INOTIFY_FLAGS  (IN_CREATE | IN_DELETE | IN_MOVE)
#endif



enum file_type {
  FILE_UNKNOWN = 0,
  FILE_IGNORE,
  FILE_REGULAR,
  FILE_PLAYLIST,
  FILE_SMARTPL,
  FILE_ITUNES,
  FILE_ARTWORK,
  FILE_CTRL_REMOTE,
  FILE_CTRL_RAOP_VERIFICATION,
  FILE_CTRL_LASTFM,
  FILE_CTRL_INITSCAN,
  FILE_CTRL_METASCAN, // forced scan for meta, preserves existing db records
  FILE_CTRL_FULLSCAN,
};

struct deferred_pl {
  char *path;
  time_t mtime;
  struct deferred_pl *next;
  int directory_id;
};

struct stacked_dir {
  char *path;
  int parent_id;
  struct stacked_dir *next;
};

static int inofd;
static struct event *inoev;
static struct deferred_pl *playlists;
static struct stacked_dir *dirstack;

/* From library.c */
extern struct event_base *evbase_lib;

#ifndef __linux__
struct deferred_file
{
  struct watch_info wi;
  char path[PATH_MAX];

  struct deferred_file *next;
  /* variable sized, must be at the end */
  struct inotify_event ie;
};

static struct deferred_file *filestack;
static struct event *deferred_inoev;
#endif

/* Count of files scanned during a bulk scan */
static int counter;

/* When copying into the lib (eg. if a file is moved to the lib by copying into
 * a Samba network share) inotify might give us IN_CREATE -> n x IN_ATTRIB ->
 * IN_CLOSE_WRITE, but we don't want to do any scanning before the
 * IN_CLOSE_WRITE. So we register new files (by path hashes) in this ring buffer
 * when we get the IN_CREATE and then ignore the IN_ATTRIB for these files.
 */
#define INCOMINGFILES_BUFFER_SIZE 50
static int incomingfiles_idx;
static uint32_t incomingfiles_buffer[INCOMINGFILES_BUFFER_SIZE];

/* Forward */
static void
bulk_scan(const char *req_path, int flags);
static int
inofd_event_set(void);
static void
inofd_event_unset(void);
static int
filescanner_initscan();
static int
filescanner_rescan();
static int
filescanner_fullrescan();


/* ----------------------- Internal utility functions --------------------- */

static char *
strip_extension(const char *path)
{
  char *ptr;
  char *result;

  result = strdup(path);
  ptr = strrchr(result, '.');
  if (ptr)
    *ptr = '\0';

  return result;
}

static int
virtual_path_make(char *virtual_path, int virtual_path_len, const char *path)
{
  int ret;

  ret = snprintf(virtual_path, virtual_path_len, "/file:%s", path);
  if ((ret < 0) || (ret >= virtual_path_len))
    {
      DPRINTF(E_LOG, L_SCAN, "Virtual path '/file:%s', virtual_path_len exceeded (%d/%d)\n", path, ret, virtual_path_len);
      return -1;
    }

  return 0;
}

static int
get_parent_dir_id(const char *path)
{
  char *pathcopy;
  char *parent_dir;
  char virtual_path[PATH_MAX];
  int parent_id;
  int ret;

  pathcopy = strdup(path);
  parent_dir = dirname(pathcopy);
  ret = virtual_path_make(virtual_path, sizeof(virtual_path), parent_dir);
  if (ret == 0)
    parent_id = db_directory_id_byvirtualpath(virtual_path);
  else
    parent_id = 0;

  free(pathcopy);

  return parent_id;
}

static int
push_dir(struct stacked_dir **s, const char *path, int parent_id)
{
  struct stacked_dir *d;

  d = malloc(sizeof(struct stacked_dir));
  if (!d)
    {
      DPRINTF(E_LOG, L_SCAN, "Could not stack directory %s; out of memory\n", path);
      return -1;
    }

  d->path = strdup(path);
  if (!d->path)
    {
      DPRINTF(E_LOG, L_SCAN, "Could not stack directory %s; out of memory for path\n", path);
      free(d);
      return -1;
    }

  d->parent_id = parent_id;

  d->next = *s;
  *s = d;

  return 0;
}

static struct stacked_dir *
pop_dir(struct stacked_dir **s)
{
  struct stacked_dir *d;

  if (!*s)
    return NULL;

  d = *s;
  *s = d->next;

  return d;
}

#ifdef HAVE_REGEX_H
/* Checks if the file path is configured to be ignored */
static int
file_path_ignore(const char *path)
{
  cfg_t *lib;
  regex_t regex;
  int n;
  int i;
  int ret;

  lib = cfg_getsec(cfg, "library");
  n = cfg_size(lib, "filepath_ignore");

  for (i = 0; i < n; i++)
    {
      ret = regcomp(&regex, cfg_getnstr(lib, "filepath_ignore", i), 0);
      if (ret != 0)
	{
	  DPRINTF(E_LOG, L_SCAN, "Could not compile regex for matching with file path\n");
	  return 0;
	}

      ret = regexec(&regex, path, 0, NULL, 0);
      regfree(&regex);

      if (ret == 0)
	{
	  DPRINTF(E_DBG, L_SCAN, "Regex match: %s\n", path);
	  return 1;
	}
    }

  return 0;
}
#endif

/* Checks if the file extension is in the ignore list */
static int
file_type_ignore(const char *ext)
{
  cfg_t *lib;
  int n;
  int i;

  lib = cfg_getsec(cfg, "library");
  n = cfg_size(lib, "filetypes_ignore");

  for (i = 0; i < n; i++)
    {
      if (strcasecmp(ext, cfg_getnstr(lib, "filetypes_ignore", i)) == 0)
	return 1;
    }

  return 0;
}

static enum file_type
file_type_get(const char *path) {
  const char *filename;
  const char *ext;

  filename = strrchr(path, '/');
  if ((!filename) || (strlen(filename) == 1))
    filename = path;
  else
    filename++;

#ifdef HAVE_REGEX_H
  if (file_path_ignore(path))
    return FILE_IGNORE;
#endif

  ext = strrchr(path, '.');
  if (!ext || (strlen(ext) == 1))
    return FILE_REGULAR;

  if (file_type_ignore(ext))
    return FILE_IGNORE;

  if ((strcasecmp(ext, ".m3u") == 0) || (strcasecmp(ext, ".pls") == 0) || (strcasecmp(ext, ".m3u8") == 0))
    return FILE_PLAYLIST;

  if (strcasecmp(ext, ".smartpl") == 0)
    return FILE_SMARTPL;

  if (artwork_file_is_artwork(filename))
    return FILE_ARTWORK;

  if ((strcasecmp(ext, ".jpg") == 0) || (strcasecmp(ext, ".png") == 0))
    return FILE_IGNORE;

  if (strcasecmp(ext, ".xml") == 0)
    return FILE_ITUNES;

  if (strcasecmp(ext, ".remote") == 0)
    return FILE_CTRL_REMOTE;

  if (strcasecmp(ext, ".verification") == 0)
    return FILE_CTRL_RAOP_VERIFICATION;

  if (strcasecmp(ext, ".lastfm") == 0)
    return FILE_CTRL_LASTFM;

  if (strcasecmp(ext, ".init-rescan") == 0)
    return FILE_CTRL_INITSCAN;

  if (strcasecmp(ext, ".meta-rescan") == 0)
    return FILE_CTRL_METASCAN;

  if (strcasecmp(ext, ".full-rescan") == 0)
    return FILE_CTRL_FULLSCAN;

  if (strcasecmp(ext, ".url") == 0)
    {
      DPRINTF(E_INFO, L_SCAN, "No support for .url, use .m3u or .pls\n");
      return FILE_IGNORE;
    }

  if ((filename[0] == '_') || (filename[0] == '.'))
    return FILE_IGNORE;

  return FILE_REGULAR;
}


/* ----------------- Utility functions used by the scanners --------------- */

const char *
filename_from_path(const char *path)
{
  const char *filename;

  filename = strrchr(path, '/');
  if ((!filename) || (strlen(filename) == 1))
    filename = path;
  else
    filename++;

  return filename;
}

char *
title_from_path(const char *path)
{
  const char *filename;

  filename = filename_from_path(path);

  return strip_extension(filename);
}

int
parent_dir(const char **current, const char *path)
{
  const char *ptr;

  if (*current)
    ptr = *current;
  else
    ptr = strrchr(path, '/');

  if (!ptr || (ptr == path))
    return -1;

  for (ptr--; (ptr > path) && (*ptr != '/'); ptr--)
    ;

  *current = ptr;

  return 0;
}

int
playlist_fill(struct playlist_info *pli, const char *path)
{
  char virtual_path[PATH_MAX];
  int ret;

  ret = virtual_path_make(virtual_path, sizeof(virtual_path), path);
  if (ret < 0)
    return -1;

  memset(pli, 0, sizeof(struct playlist_info));

  pli->type  = PL_PLAIN;
  pli->path  = strdup(path);
  pli->title = title_from_path(path); // Will alloc
  pli->virtual_path = strip_extension(virtual_path); // Will alloc
  pli->scan_kind = SCAN_KIND_FILES;

  pli->directory_id = get_parent_dir_id(path);

  return 0;
}

int
playlist_add(const char *path)
{
  struct playlist_info pli;
  int ret;

  ret = playlist_fill(&pli, path);
  if (ret < 0)
    return -1;

  ret = library_playlist_save(&pli);
  if (ret < 0)
    {
      free_pli(&pli, 1);
      return -1;
    }

  free_pli(&pli, 1);

  return ret;
}


/* --------------------------- Processing procedures ---------------------- */

static void
process_playlist(char *file, time_t mtime, int dir_id)
{
  enum file_type ft;

  ft = file_type_get(file);
  if (ft == FILE_PLAYLIST)
    scan_playlist(file, mtime, dir_id);
  else if (ft == FILE_ITUNES)
    scan_itunes_itml(file, mtime, dir_id);
}

/* If we found a control file we want to kickoff some action */
static void
kickoff(void (*kickoff_func)(char **arg), const char *file, int lines)
{
  char **file_content;
  int i;

  file_content = m_readfile(file, lines);
  if (!file_content)
    return;

  kickoff_func(file_content);

  for (i = 0; i < lines; i++)
    free(file_content[i]);

  free(file_content);
}

/* Thread: scan */
static void
defer_playlist(char *path, time_t mtime, int dir_id)
{
  struct deferred_pl *pl;

  pl = (struct deferred_pl *)malloc(sizeof(struct deferred_pl));
  if (!pl)
    {
      DPRINTF(E_WARN, L_SCAN, "Out of memory for deferred playlist\n");

      return;
    }

  memset(pl, 0, sizeof(struct deferred_pl));

  pl->path = strdup(path);
  if (!pl->path)
    {
      DPRINTF(E_WARN, L_SCAN, "Out of memory for deferred playlist\n");

      free(pl);
      return;
    }

  pl->mtime = mtime;
  pl->directory_id = dir_id;
  pl->next = playlists;
  playlists = pl;

  DPRINTF(E_INFO, L_SCAN, "Deferred playlist %s\n", path);
}

/* Thread: scan (bulk only) */
static void
process_deferred_playlists(void)
{
  struct deferred_pl *pl;

  while ((pl = playlists))
    {
      playlists = pl->next;

      process_playlist(pl->path, pl->mtime, pl->directory_id);

      free(pl->path);
      free(pl);

      if (library_is_exiting())
	return;
    }
}

static void
process_regular_file(const char *file, struct stat *sb, int type, int flags, int dir_id)
{
  bool is_bulkscan = (flags & F_SCAN_BULK);
  struct media_file_info mfi;
  char virtual_path[PATH_MAX];
  int ret;

  if (!(flags & F_SCAN_METARESCAN))
    {
      // Will return 0 if file is not in library or if file mtime is not older
      // than the library timestamp. If mtime is equal we must rescan, since a
      // fast update may have been made, see issue #1782. If mtime is 0 then we
      // always scan.
      ret = db_file_ping_bypath(file, sb->st_mtime);
      if ((sb->st_mtime != 0) && (ret != 0))
        return;
    }

  // File is new or modified - (re)scan metadata and update file in library
  memset(&mfi, 0, sizeof(struct media_file_info));

  // Sets id=0 if file is not in the library already
  mfi.id = db_file_id_bypath(file);

  mfi.fname = strdup(filename_from_path(file));
  mfi.path = strdup(file);

  mfi.time_modified = sb->st_mtime;
  mfi.file_size = sb->st_size;

  snprintf(virtual_path, PATH_MAX, "/file:%s", file);
  mfi.virtual_path = strdup(virtual_path);

  mfi.directory_id = dir_id;
  mfi.scan_kind = SCAN_KIND_FILES;

  if (S_ISFIFO(sb->st_mode))
    {
      mfi.data_kind = DATA_KIND_PIPE;
      mfi.type = strdup("wav");
      mfi.codectype = strdup("wav");
      mfi.description = strdup("PCM16 pipe");
      mfi.media_kind = MEDIA_KIND_MUSIC;
    }
  else
    {
      mfi.data_kind = DATA_KIND_FILE;
      mfi.file_size = sb->st_size;

      if (type & F_SCAN_TYPE_AUDIOBOOK)
	mfi.media_kind = MEDIA_KIND_AUDIOBOOK;
      else if (type & F_SCAN_TYPE_PODCAST)
	mfi.media_kind = MEDIA_KIND_PODCAST;

      if (type & F_SCAN_TYPE_COMPILATION)
	{
	  mfi.compilation = 1;
	  mfi.album_artist = safe_strdup(cfg_getstr(cfg_getsec(cfg, "library"), "compilation_artist"));
	}

      ret = scan_metadata_ffmpeg(&mfi, file);
      if (ret < 0)
	{
	  free_mfi(&mfi, 1);
	  return;
	}
    }

  library_media_save(&mfi);

  cache_artwork_ping(file, sb->st_mtime, !is_bulkscan);
  // TODO [artworkcache] If entry in artwork cache exists for no artwork available, delete the entry if media file has embedded artwork

  free_mfi(&mfi, 1);
}

/* Thread: scan */
static void
process_file(char *file, struct stat *sb, enum file_type file_type, int scan_type, int flags, int dir_id)
{
  switch (file_type)
    {
      case FILE_REGULAR:
	process_regular_file(file, sb, scan_type, flags, dir_id);

	counter++;

	/* When in bulk mode, split transaction in pieces of 200 */
	if ((flags & F_SCAN_BULK) && (counter % 200 == 0))
	  {
	    DPRINTF(E_LOG, L_SCAN, "Scanned %d files...\n", counter);
	    db_transaction_end();
	    db_transaction_begin();
	  }
	break;

      case FILE_PLAYLIST:
      case FILE_ITUNES:
	if (flags & F_SCAN_BULK)
	  defer_playlist(file, sb->st_mtime, dir_id);
	else
	  process_playlist(file, sb->st_mtime, dir_id);
	break;

      case FILE_SMARTPL:
	DPRINTF(E_DBG, L_SCAN, "Smart playlist file: %s\n", file);
	scan_smartpl(file, sb->st_mtime, dir_id);
	break;

      case FILE_ARTWORK:
	DPRINTF(E_DBG, L_SCAN, "Artwork file: %s\n", file);
	cache_artwork_ping(file, sb->st_mtime, !(flags & F_SCAN_BULK));

	// TODO [artworkcache] If entry in artwork cache exists for no artwork available for a album with files in the same directory, delete the entry

	break;

      case FILE_CTRL_REMOTE:
	if (flags & F_SCAN_BULK)
	  DPRINTF(E_LOG, L_SCAN, "Bulk scan will ignore '%s' (to process, add it after startup)\n", file);
	else
	  kickoff(remote_pairing_kickoff, file, 1);
	break;

      case FILE_CTRL_RAOP_VERIFICATION:
	if (flags & F_SCAN_BULK)
	  DPRINTF(E_LOG, L_SCAN, "Bulk scan will ignore '%s' (to process, add it after startup)\n", file);
	else
	  kickoff(player_raop_verification_kickoff, file, 1);
	break;

      case FILE_CTRL_LASTFM:
#ifdef LASTFM
	if (flags & F_SCAN_BULK)
	  DPRINTF(E_LOG, L_SCAN, "Bulk scan will ignore '%s' (to process, add it after startup)\n", file);
	else
	  kickoff(lastfm_login, file, 2);
#else
	DPRINTF(E_LOG, L_SCAN, "Found '%s', but this version was built without LastFM support\n", file);
#endif
	break;

      case FILE_CTRL_INITSCAN:
	if (flags & F_SCAN_BULK)
	  break;

	DPRINTF(E_LOG, L_SCAN, "Startup rescan triggered, found init-rescan file: %s\n", file);

	library_rescan(0);
	break;

      case FILE_CTRL_METASCAN:
	if (flags & F_SCAN_BULK)
	  break;

	DPRINTF(E_LOG, L_SCAN, "Meta rescan triggered, found meta-rescan file: %s\n", file);

	library_metarescan(0);
	break;

      case FILE_CTRL_FULLSCAN:
	if (flags & F_SCAN_BULK)
	  break;

	DPRINTF(E_LOG, L_SCAN, "Full rescan triggered, found full-rescan file: %s\n", file);

	library_fullrescan();
	break;

      default:
	DPRINTF(E_WARN, L_SCAN, "Ignoring file: %s\n", file);
    }
}

/* Thread: scan */
static int
check_speciallib(char *path, const char *libtype)
{
  cfg_t *lib;
  int ndirs;
  int i;

  lib = cfg_getsec(cfg, "library");
  ndirs = cfg_size(lib, libtype);
  for (i = 0; i < ndirs; i++)
    {
      if (strstr(path, cfg_getnstr(lib, libtype, i)))
	return 1;
    }

  return 0;
}

/*
 * Returns informations about the attributes of the file at the given 'path' in the structure
 * pointed to by 'sb'.
 *
 * If path is a symbolic link, the attributes in sb describe the file that the link points to
 * and resolved_path contains the resolved path (resolved_path must be of length PATH_MAX).
 * If path is not a symbolic link, resolved_path holds the same value as path.
 *
 * The return value is 0 if the operation is successful, or -1 on failure
 */
static int
read_attributes(char *resolved_path, const char *path, struct stat *sb, int *is_link)
{
  int ret;

  *is_link = 0;

  ret = lstat(path, sb);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_SCAN, "Skipping %s, lstat() failed: %s\n", path, strerror(errno));
      return -1;
    }

  if (S_ISLNK(sb->st_mode))
    {
      *is_link = 1;

      if (!realpath(path, resolved_path))
        {
	  DPRINTF(E_LOG, L_SCAN, "Skipping %s, could not dereference symlink: %s\n", path, strerror(errno));
	  return -1;
	}

      ret = stat(resolved_path, sb);
      if (ret < 0)
        {
	  DPRINTF(E_LOG, L_SCAN, "Skipping %s, stat() failed: %s\n", resolved_path, strerror(errno));
	  return -1;
	}
    }
  else
    {
      strcpy(resolved_path, path);
    }

  return 0;
}

static void
process_directory(char *path, int parent_id, int flags)
{
  DIR *dirp;
  struct dirent *de;
  char entry[PATH_MAX];
  char resolved_path[PATH_MAX];
  struct stat sb;
  int is_link;
  int follow_symlinks;
  struct watch_info wi;
  int scan_type;
  enum file_type file_type;
  char virtual_path[PATH_MAX];
  int dir_id;
  int ret;

  DPRINTF(E_DBG, L_SCAN, "Processing directory %s (flags = 0x%x)\n", path, flags);

  dirp = opendir(path);
  if (!dirp)
    {
      DPRINTF(E_LOG, L_SCAN, "Could not open directory %s: %s\n", path, strerror(errno));
      return;
    }

  /* Add/update directories table */

  ret = virtual_path_make(virtual_path, sizeof(virtual_path), path);
  if (ret < 0)
    {
      closedir(dirp);
      return;
    }

  dir_id = library_directory_save(virtual_path, path, 0, parent_id, SCAN_KIND_FILES);
  if (dir_id <= 0)
    {
      DPRINTF(E_LOG, L_SCAN, "Insert or update of directory failed '%s'\n", virtual_path);
    }

  /* Check if compilation and/or podcast directory */
  scan_type = 0;
  if (check_speciallib(path, "compilations"))
    scan_type |= F_SCAN_TYPE_COMPILATION;
  if (check_speciallib(path, "podcasts"))
    scan_type |= F_SCAN_TYPE_PODCAST;
  if (check_speciallib(path, "audiobooks"))
    scan_type |= F_SCAN_TYPE_AUDIOBOOK;

  follow_symlinks = cfg_getbool(cfg_getsec(cfg, "library"), "follow_symlinks");

  for (;;)
    {
      if (library_is_exiting())
	break;

      errno = 0;
      de = readdir(dirp);
      if (errno)
	{
	  DPRINTF(E_LOG, L_SCAN, "readdir error in %s: %s\n", path, strerror(errno));
	  break;
	}

      if (!de)
	break;

      if (de->d_name[0] == '.')
	continue;

      ret = snprintf(entry, sizeof(entry), "%s/%s", path, de->d_name);
      if ((ret < 0) || (ret >= sizeof(entry)))
	{
	  DPRINTF(E_LOG, L_SCAN, "Skipping %s/%s, PATH_MAX exceeded\n", path, de->d_name);

	  continue;
	}

      file_type = file_type_get(entry);
      if (file_type == FILE_IGNORE)
	continue;

      ret = read_attributes(resolved_path, entry, &sb, &is_link);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_SCAN, "Skipping %s, read_attributes() failed\n", entry);
	  continue;
	}

      if (is_link && !follow_symlinks)
        {
          DPRINTF(E_DBG, L_SCAN, "Ignore symlink %s\n", entry);
          continue;
        }

      if (S_ISDIR(sb.st_mode))
	{
	  push_dir(&dirstack, resolved_path, dir_id);
	}
      else if (!(flags & F_SCAN_FAST))
	{
	  if (S_ISREG(sb.st_mode) || S_ISFIFO(sb.st_mode))
	    process_file(resolved_path, &sb, file_type, scan_type, flags, dir_id);
	  else
	    DPRINTF(E_LOG, L_SCAN, "Skipping %s, not a directory, symlink, pipe nor regular file\n", entry);
	}
    }

  closedir(dirp);

  memset(&wi, 0, sizeof(struct watch_info));

  // Add inotify watch (for FreeBSD we limit the flags so only dirs will be
  // opened, otherwise we will be opening way too many files)
  wi.wd = inotify_add_watch(inofd, path, INOTIFY_FLAGS);
  if (wi.wd < 0)
    {
      DPRINTF(E_WARN, L_SCAN, "Could not create inotify watch for %s: %s\n", path, strerror(errno));
      return;
    }

  if (!(flags & F_SCAN_MOVED))
    {
      wi.cookie = 0;
      wi.path = path;

      db_watch_add(&wi);
    }
}

/* Thread: scan */
static int
process_parent_directories(char *path)
{
  char *ptr;
  int dir_id;
  char buf[PATH_MAX];
  char virtual_path[PATH_MAX];
  int ret;

  dir_id = DIR_FILE;

  ptr = path + 1;
  while (ptr && (ptr = strchr(ptr, '/')))
    {
      if (strlen(ptr) <= 1)
	{
	  // Do not process trailing '/'
	  break;
	}

      strncpy(buf, path, (ptr - path));
      buf[(ptr - path)] = '\0';

      ret = virtual_path_make(virtual_path, sizeof(virtual_path), buf);
      if (ret < 0)
	return 0;

      dir_id = library_directory_save(virtual_path, buf, 0, dir_id, SCAN_KIND_FILES);
      if (dir_id <= 0)
	{
	  DPRINTF(E_LOG, L_SCAN, "Insert or update of directory failed '%s'\n", virtual_path);

	  return 0;
	}

      ptr++;
    }

  return dir_id;
}

static void
process_directories(char *root, int parent_id, int flags)
{
  struct stacked_dir *dir;

  process_directory(root, parent_id, flags);

  if (library_is_exiting())
    return;

  while ((dir = pop_dir(&dirstack)))
    {
      process_directory(dir->path, dir->parent_id, flags);

      free(dir->path);
      free(dir);

      if (library_is_exiting())
	return;
    }
}


/* Thread: scan */
static void
bulk_scan(const char *req_path, int flags)
{
  cfg_t *lib;
  int ndirs;
  char *path;
  char *deref;
  time_t start;
  time_t end;
  int parent_id;
  int i;
  char virtual_path[PATH_MAX];
  int ret;

  start = time(NULL);

  playlists = NULL;
  dirstack = NULL;

  lib = cfg_getsec(cfg, "library");
  counter = 0;

  ndirs = cfg_size(lib, "directories");
  for (i = 0; i < ndirs; i++)
    {
      path = cfg_getnstr(lib, "directories", i);

      // make sure path is in library if we've been asked to scan a specific path
      if (req_path)
        {
          if (strncmp(path, req_path, strlen(path)) == 0)
            path = (char*)req_path;
          else
            {
              DPRINTF(E_LOG, L_SCAN, "Skipping request path: '%s', not in library directories\n", req_path);
              continue;
            }
        }

      parent_id = process_parent_directories(path);

      deref = realpath(path, NULL);
      if (!deref)
	{
	  DPRINTF(E_LOG, L_SCAN, "Skipping library directory %s, could not dereference: %s\n", path, strerror(errno));

	  /* Assume dir is mistakenly not mounted, so just disable everything and update timestamps */
	  db_file_disable_bymatch(path, STRIP_NONE, 0);
	  db_pl_disable_bymatch(path, STRIP_NONE, 0);
	  db_directory_disable_bymatch(path, STRIP_NONE, 0);

	  db_file_ping_bymatch(path, 1);
	  db_pl_ping_bymatch(path, 1);
	  ret = snprintf(virtual_path, sizeof(virtual_path), "/file:%s", path);
	  if ((ret < 0) || (ret >= sizeof(virtual_path)))
	    DPRINTF(E_LOG, L_SCAN, "Virtual path exceeds PATH_MAX (/file:%s)\n", path);
	  else
	    db_directory_ping_bymatch(virtual_path);

	  continue;
	}

      db_transaction_begin();

      process_directories(deref, parent_id, flags);
      db_transaction_end();

      free(deref);

      if (library_is_exiting())
	return;
    }

  if (!(flags & F_SCAN_FAST) && playlists)
    process_deferred_playlists();

  if (library_is_exiting())
    return;

  if (dirstack)
    DPRINTF(E_LOG, L_SCAN, "WARNING: unhandled leftover directories\n");

  end = time(NULL);

  if (flags & F_SCAN_FAST)
    {
      DPRINTF(E_LOG, L_SCAN, "Bulk library scan completed in %.f sec (with file scan disabled)\n", difftime(end, start));
    }
  else
    {
      DPRINTF(E_LOG, L_SCAN, "Bulk library scan completed in %.f sec\n", difftime(end, start));
    }
}

static int
watches_clear(uint32_t wd, char *path)
{
  struct watch_enum we;
  uint32_t rm_wd;
  int ret;

  inotify_rm_watch(inofd, wd);
  db_watch_delete_bywd(wd);

  memset(&we, 0, sizeof(struct watch_enum));

  we.match = path;

  ret = db_watch_enum_start(&we);
  if (ret < 0)
    return -1;

  while ((db_watch_enum_fetchwd(&we, &rm_wd) == 0) && (rm_wd))
    {
      inotify_rm_watch(inofd, rm_wd);
    }

  db_watch_enum_end(&we);

  db_watch_delete_bymatch(path);

  return 0;
}

static int
watches_clear_bypath(char *path)
{
  struct watch_info wi;
  struct watch_enum we;
  int ret;

  memset(&wi, 0, sizeof(struct watch_info));

  wi.path = path;
  db_watch_get_bypath(&wi, path);
  watches_clear(wi.wd, path);
  free(wi.path);

  memset(&we, 0, sizeof(struct watch_enum));
  we.match = "";  // get everything

  ret = db_watch_enum_start(&we);
  if (ret < 0)
    return -1;

  memset(&wi, 0, sizeof(struct watch_info));
  wi.path = malloc(PATH_MAX);
  while (db_watch_enum_fetch(&we, &wi) == 0 && wi.wd)
    {
      inotify_rm_watch(inofd, wi.wd);
      db_watch_delete_bypath(wi.path);

#ifdef __linux__
      wi.wd = inotify_add_watch(inofd, wi.path, IN_ATTRIB | IN_CREATE | IN_DELETE | IN_CLOSE_WRITE | IN_MOVE | IN_DELETE | IN_MOVE_SELF);
#else
      wi.wd = inotify_add_watch(inofd, wi.path, IN_CREATE | IN_DELETE | IN_MOVE);
#endif
      if (wi.wd < 0)
        {
          DPRINTF(E_LOG, L_SCAN, "Failed to obtain watch: '%s' - %s\n", wi.path, strerror(errno));
        }
      else
        {
          wi.cookie = 0;
          db_watch_add(&wi);
        }

      wi.wd = 0;
      wi.path[0] = '\0';
    }

  db_watch_enum_end(&we);

  free(wi.path);
  return 0;
}

/* Thread: scan */
static void
process_inotify_dir(struct watch_info *wi, char *path, struct inotify_event *ie)
{
  struct watch_enum we;
  struct watch_info dummy_wi;
  uint32_t rm_wd;
  int flags = 0;
  int ret;
  int parent_id;
  int fd;

  DPRINTF(E_DBG, L_SCAN, "Directory event: 0x%08x, cookie 0x%08x, wd %d\n", ie->mask, ie->cookie, wi->wd);

  if (ie->mask & IN_UNMOUNT)
    {
      db_file_disable_bymatch(path, STRIP_NONE, 0);
      db_pl_disable_bymatch(path, STRIP_NONE, 0);
      db_directory_disable_bymatch(path, STRIP_NONE, 0);
    }

  if (ie->mask & IN_MOVE_SELF)
    {
      /* A directory we know about, that got moved from a place
       * we know about to a place we know nothing about
       */
      if (wi->cookie)
	{
	  memset(&we, 0, sizeof(struct watch_enum));

	  we.cookie = wi->cookie;

	  ret = db_watch_enum_start(&we);
	  if (ret < 0)
	    return;

	  while ((db_watch_enum_fetchwd(&we, &rm_wd) == 0) && (rm_wd))
	    {
	      inotify_rm_watch(inofd, rm_wd);
	    }

	  db_watch_enum_end(&we);

	  db_watch_delete_bycookie(wi->cookie);
	}
      else
	{
	  /* If the directory exists, it has been moved and we've
	   * kept track of it successfully, so we're done
	   */
	  ret = access(path, F_OK);
	  if (ret == 0)
	    return;

	  /* Most probably a top-level dir is getting moved,
	   * and we can't tell where it's going
	   */

	  ret = watches_clear(ie->wd, path);
	  if (ret < 0)
	    return;

	  db_file_disable_bymatch(path, STRIP_NONE, 0);
	  db_pl_disable_bymatch(path, STRIP_NONE, 0);
	}
    }

  if (ie->mask & IN_MOVED_FROM)
    {
      db_watch_mark_bypath(path, STRIP_PATH, ie->cookie);
      db_watch_mark_bymatch(path, STRIP_PATH, ie->cookie);
      db_file_disable_bymatch(path, STRIP_PATH, ie->cookie);
      db_pl_disable_bymatch(path, STRIP_PATH, ie->cookie);
      db_directory_disable_bymatch(path, STRIP_PATH, ie->cookie);
    }

  if (ie->mask & IN_MOVED_TO)
    {
      if (db_watch_cookie_known(ie->cookie))
	{
	  db_watch_move_bycookie(ie->cookie, path);
	  db_file_enable_bycookie(ie->cookie, path, NULL);
	  db_pl_enable_bycookie(ie->cookie, path);
	  db_directory_enable_bycookie(ie->cookie, path);

	  /* We'll rescan the directory tree to update playlists */
	  flags |= F_SCAN_MOVED;
	}

      ie->mask |= IN_CREATE;
    }

  if (ie->mask & IN_ATTRIB)
    {
      DPRINTF(E_DBG, L_SCAN, "Directory permissions changed (%s): %s\n", wi->path, path);

      // Find out if we are already watching the dir (ret will be 0)
      ret = db_watch_get_bypath(&dummy_wi, path);
      if (ret == 0)
	free_wi(&dummy_wi, 1);

      // We don't use access() or euidaccess() because they don't work with ACL's
      // - this also means we can't check for executable permission, which stat()
      // will require
      fd = open(path, O_RDONLY);
      if (fd < 0)
	{
	  DPRINTF(E_LOG, L_SCAN, "Directory access to '%s' failed: %s\n", path, strerror(errno));

	  if (ret == 0)
	    watches_clear(wi->wd, path);

	  db_file_disable_bymatch(path, STRIP_NONE, 0);
	  db_pl_disable_bymatch(path, STRIP_NONE, 0);
	  db_directory_disable_bymatch(path, STRIP_NONE, 0);
	}
      else if (ret < 0)
	{
	  DPRINTF(E_INFO, L_SCAN, "Directory access to '%s' achieved\n", path);

	  ie->mask |= IN_CREATE;
	}
      else
	{
	  DPRINTF(E_INFO, L_SCAN, "Directory event, but '%s' already being watched\n", path);
	}

      if (fd >= 0)
	close(fd);
    }

  if (ie->mask & IN_CREATE)
    {
      parent_id = get_parent_dir_id(path);
      process_directories(path, parent_id, flags);

      if (dirstack)
	DPRINTF(E_LOG, L_SCAN, "WARNING: unhandled leftover directories\n");
    }
}

/* Thread: scan */
static void
process_inotify_file(struct watch_info *wi, char *path, struct inotify_event *ie)
{
  struct stat sb;
  int is_link;
  uint32_t path_hash;
  char *file = path;
  char resolved_path[PATH_MAX];
  char dir_vpath[PATH_MAX];
  enum file_type file_type;
  int scan_type;
  int i;
  int dir_id;
  int fd;
  char *ptr;
  int ret;

  DPRINTF(E_DBG, L_SCAN, "File event: 0x%08x, cookie 0x%08x, wd %d\n", ie->mask, ie->cookie, wi->wd);

  file_type = file_type_get(path);
  if (file_type == FILE_IGNORE)
    return;

  path_hash = djb_hash(path, strlen(path));

  if (ie->mask & IN_DELETE)
    {
      DPRINTF(E_DBG, L_SCAN, "File deleted: %s\n", path);

      db_file_delete_bypath(path);
      db_pl_delete_bypath(path);
      cache_artwork_delete_by_path(path);
    }

  if (ie->mask & IN_MOVED_FROM)
    {
      DPRINTF(E_DBG, L_SCAN, "File moved from: %s\n", path);

      db_file_disable_bypath(path, STRIP_PATH, ie->cookie);
      db_pl_disable_bypath(path, STRIP_PATH, ie->cookie);
    }

  if (ie->mask & IN_ATTRIB)
    {
      DPRINTF(E_DBG, L_SCAN, "File attributes changed: %s\n", path);

      // Ignore the IN_ATTRIB if we just got an IN_CREATE
      for (i = 0; i < INCOMINGFILES_BUFFER_SIZE; i++)
	{
	  if (incomingfiles_buffer[i] == path_hash)
	    return;
	}

      // We don't use access() or euidaccess() because they don't work with ACL's
      fd = open(path, O_RDONLY);
      if (fd < 0)
	{
	  DPRINTF(E_LOG, L_SCAN, "File access to '%s' failed: %s\n", path, strerror(errno));

	  db_file_delete_bypath(path);
	  cache_artwork_delete_by_path(path);
	}
      else if ((file_type_get(path) == FILE_REGULAR) && (db_file_id_bypath(path) <= 0)) // TODO Playlists
	{
	  DPRINTF(E_LOG, L_SCAN, "File access to '%s' achieved\n", path);

	  ie->mask |= IN_CLOSE_WRITE;
	}

      if (fd >= 0)
	close(fd);
    }

  if (ie->mask & IN_MOVED_TO)
    {
      DPRINTF(E_DBG, L_SCAN, "File moved to: %s\n", path);

      /* handle overwriting an existing file, no inotify event generated for the
       * overwrite on existing file before we update the path of moved file
       */
      db_file_delete_bypath(path);
      cache_artwork_delete_by_path(path);

      ret = db_file_enable_bycookie(ie->cookie, path, filename_from_path(path));

      if (ret > 0)
	{
	  ret = virtual_path_make(dir_vpath, sizeof(dir_vpath), path);
	  if (ret >= 0)
	    {
	      CHECK_NULL(L_SCAN, ptr = strrchr(dir_vpath, '/'));
	      *ptr = '\0';

	      dir_id = db_directory_id_byvirtualpath(dir_vpath);
	      if (dir_id > 0)
		{
		  ret = db_file_update_directoryid(path, dir_id);
		  if (ret < 0)
		    DPRINTF(E_LOG, L_SCAN, "Error updating directory id for file: %s\n", path);
		}
	    }
	}
      else
	{
	  /* It's not a known media file, so it's either a new file
	   * or a playlist, known or not.
	   * We want to scan the new file and we want to rescan the
	   * playlist to update playlist items (relative items).
	   */
	  ie->mask |= IN_CLOSE_WRITE;
	  db_pl_enable_bycookie(ie->cookie, path);
	}
    }

  if (ie->mask & IN_CREATE)
    {
      DPRINTF(E_DBG, L_SCAN, "File created: %s\n", path);

      ret = lstat(path, &sb);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_SCAN, "Could not lstat() '%s': %s\n", path, strerror(errno));

	  return;
	}

      // Add to the list of files where we ignore IN_ATTRIB until the file is closed again
      if (S_ISREG(sb.st_mode))
	{
	  DPRINTF(E_SPAM, L_SCAN, "Incoming file created '%s' (%d), index %d\n", path, (int)path_hash, incomingfiles_idx);

	  incomingfiles_buffer[incomingfiles_idx] = path_hash;
	  incomingfiles_idx = (incomingfiles_idx + 1) % INCOMINGFILES_BUFFER_SIZE;
	}
      else if (S_ISFIFO(sb.st_mode))
	ie->mask |= IN_CLOSE_WRITE;
    }

  if (ie->mask & IN_CLOSE_WRITE)
    {
      DPRINTF(E_DBG, L_SCAN, "File closed: %s\n", path);

      // File has been closed so remove from the IN_ATTRIB ignore list
      for (i = 0; i < INCOMINGFILES_BUFFER_SIZE; i++)
	if (incomingfiles_buffer[i] == path_hash)
	  {
	    DPRINTF(E_SPAM, L_SCAN, "Incoming file closed '%s' (%d), index %d\n", path, (int)path_hash, i);

	    incomingfiles_buffer[i] = 0;
	  }

      ret = read_attributes(resolved_path, path, &sb, &is_link);
      if (ret < 0)
        {
	  DPRINTF(E_LOG, L_SCAN, "Skipping %s, read_attributes() failed\n", path);

	  return;
	}

      if (is_link && !cfg_getbool(cfg_getsec(cfg, "library"), "follow_symlinks"))
        {
          DPRINTF(E_DBG, L_SCAN, "Ignore symlink %s\n", path);
          return;
        }

      scan_type = 0;
      if (check_speciallib(path, "compilations"))
	scan_type |= F_SCAN_TYPE_COMPILATION;
      if (check_speciallib(path, "podcasts"))
	scan_type |= F_SCAN_TYPE_PODCAST;
      if (check_speciallib(path, "audiobooks"))
	scan_type |= F_SCAN_TYPE_AUDIOBOOK;

      dir_id = get_parent_dir_id(file);

      if (S_ISDIR(sb.st_mode))
        {
	  process_inotify_dir(wi, resolved_path, ie);

	  return;
	}
      else if (S_ISREG(sb.st_mode) || S_ISFIFO(sb.st_mode))
	{
	  process_file(resolved_path, &sb, file_type, scan_type, 0, dir_id);
	}
      else
	DPRINTF(E_LOG, L_SCAN, "Skipping %s, not a directory, symlink, pipe nor regular file\n", resolved_path);
    }
}

#ifndef __linux__
/* Since kexec based inotify doesn't really have inotify we only get
 * a IN_CREATE. That is a bit too soon to start scanning the file,
 * so we defer it for 10 seconds.
 */
static void
inotify_deferred_cb(int fd, short what, void *arg)
{
  struct deferred_file *f;
  struct deferred_file *next;

  for (f = filestack; f; f = next)
    {
      next = f->next;

      DPRINTF(E_DBG, L_SCAN, "Processing deferred file %s\n", f->path);
      process_inotify_file(&f->wi, f->path, &f->ie);
      free(f->wi.path);
      free(f);
    }

  filestack = NULL;
}

static void
process_inotify_file_defer(struct watch_info *wi, char *path, struct inotify_event *ie)
{
  struct deferred_file *f;
  struct timeval tv = { 10, 0 };

  if (!(ie->mask & IN_CREATE))
    {
      process_inotify_file(wi, path, ie);
      return;
    }

  DPRINTF(E_INFO, L_SCAN, "Deferring scan of newly created file %s\n", path);

  ie->mask = IN_CLOSE_WRITE;
  f = calloc(1, sizeof(struct deferred_file));
  f->wi = *wi;
  f->wi.path = strdup(wi->path);
  /* ie->name not copied here, so don't use in process_inotify_* */
  f->ie = *ie;
  strcpy(f->path, path);

  f->next = filestack;
  filestack = f;

  event_add(deferred_inoev, &tv);
}
#endif


/* Thread: scan */
static void
inotify_cb(int fd, short event, void *arg)
{
  struct inotify_event *ie;
  struct watch_info wi;
  uint8_t *buf;
  uint8_t *ptr;
  char path[PATH_MAX];
  int size;
  int namelen;
  int ret;

  /* Determine the amount of bytes to read from inotify */
  ret = ioctl(fd, FIONREAD, &size);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_SCAN, "Could not determine inotify queue size: %s\n", strerror(errno));

      return;
    }

  buf = malloc(size);
  if (!buf)
    {
      DPRINTF(E_LOG, L_SCAN, "Could not allocate %d bytes for inotify events\n", size);

      return;
    }

  ret = read(fd, buf, size);
  if (ret < 0 || ret != size)
    {
      DPRINTF(E_LOG, L_SCAN, "inotify read failed: %s (ret was %d, size %d)\n", strerror(errno), ret, size);

      free(buf);
      return;
    }

  for (ptr = buf; ptr < buf + size; ptr += ie->len + sizeof(struct inotify_event))
    {
      ie = (struct inotify_event *)ptr;

      memset(&wi, 0, sizeof(struct watch_info));

      /* ie[0] contains the inotify event information
       * the memory space for ie[1+] contains the name of the file
       * see the inotify documentation
       */
      ret = db_watch_get_bywd(&wi, ie->wd);
      if (ret < 0)
	{
	  if (!(ie->mask & IN_IGNORED))
	    DPRINTF(E_LOG, L_SCAN, "No matching watch found, ignoring event (0x%x)\n", ie->mask);

	  continue;
	}

      if (ie->mask & IN_IGNORED)
	{
	  DPRINTF(E_DBG, L_SCAN, "%s deleted or backing filesystem unmounted!\n", wi.path);

	  db_watch_delete_bywd(ie->wd);
	  free_wi(&wi, 1);
	  continue;
	}

      path[0] = '\0';

      ret = snprintf(path, sizeof(path), "%s", wi.path);
      if ((ret < 0) || (ret >= sizeof(path)))
	{
	  DPRINTF(E_LOG, L_SCAN, "Skipping event under %s, PATH_MAX exceeded\n", wi.path);

	  free_wi(&wi, 1);
	  continue;
	}

      if (ie->len > 0)
	{
	  namelen = sizeof(path) - ret;
	  ret = snprintf(path + ret, namelen, "/%s", ie->name);
	  if ((ret < 0) || (ret >= namelen))
	    {
	      DPRINTF(E_LOG, L_SCAN, "Skipping %s/%s, PATH_MAX exceeded\n", wi.path, ie->name);

	      free_wi(&wi, 1);
	      continue;
	    }
	}

      /* ie->len == 0 catches events on the subject of the watch itself.
       * As we only watch directories, this catches directories.
       * General watch events like IN_UNMOUNT and IN_IGNORED do not come
       * with the IN_ISDIR flag set.
       */
      if ((ie->mask & IN_ISDIR) || (ie->len == 0))
	process_inotify_dir(&wi, path, ie);
      else
#ifdef __linux__
	process_inotify_file(&wi, path, ie);
#else
	process_inotify_file_defer(&wi, path, ie);
#endif
      free_wi(&wi, 1);
    }

  free(buf);

  event_add(inoev, NULL);
}

/* Thread: main & scan */
static int
inofd_event_set(void)
{
  inofd = inotify_init1(IN_CLOEXEC);
  if (inofd < 0)
    {
      DPRINTF(E_FATAL, L_SCAN, "Could not create inotify fd: %s\n", strerror(errno));

      return -1;
    }

  inoev = event_new(evbase_lib, inofd, EV_READ, inotify_cb, NULL);

#ifndef __linux__
  deferred_inoev = evtimer_new(evbase_lib, inotify_deferred_cb, NULL);
  if (!deferred_inoev)
    {
      DPRINTF(E_LOG, L_SCAN, "Could not create deferred inotify event\n");

      return -1;
    }
#endif

  return 0;
}

/* Thread: main & scan */
static void
inofd_event_unset(void)
{
#ifndef __linux__
  event_free(deferred_inoev);
#endif
  event_free(inoev);
  close(inofd);
}

/* Thread: scan */
static int
filescanner_initscan()
{
  int ret;

  ret = db_watch_clear();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_SCAN, "Error: could not clear old watches from DB\n");
      return -1;
    }

  if (cfg_getbool(cfg_getsec(cfg, "library"), "filescan_disable"))
    bulk_scan(NULL, F_SCAN_BULK | F_SCAN_FAST);
  else
    bulk_scan(NULL, F_SCAN_BULK);

  if (!library_is_exiting())
    {
      /* Enable inotify */
      event_add(inoev, NULL);
    }
  return 0;
}

static int
filescanner_rescan()
{
  DPRINTF(E_LOG, L_SCAN, "Startup rescan triggered\n");

  inofd_event_unset(); // Clears all inotify watches
  db_watch_clear();
  inofd_event_set();
  bulk_scan(NULL, F_SCAN_BULK | F_SCAN_RESCAN);

  if (!library_is_exiting())
    {
      /* Enable inotify */
      event_add(inoev, NULL);
    }
  return 0;
}

static int
filescanner_rescan_path(const char *path)
{
  DPRINTF(E_LOG, L_SCAN, "rescan triggered for '%s'\n", path);

  inofd_event_unset(); // Clears all inotify watches, closing hdl
  inofd_event_set();   // and get a new inotify hdl

  // readd watchers but exclude path and let the bulk_scan to readd
  watches_clear_bypath((char*)path);
  bulk_scan(path, F_SCAN_BULK | F_SCAN_RESCAN);

  if (!library_is_exiting())
    {
      /* Enable inotify */
      event_add(inoev, NULL);
    }
  return 0;
}

static int
filescanner_metarescan()
{
  DPRINTF(E_LOG, L_SCAN, "meta rescan triggered\n");

  inofd_event_unset(); // Clears all inotify watches
  db_watch_clear();
  inofd_event_set();
  bulk_scan(NULL, F_SCAN_BULK | F_SCAN_METARESCAN);

  if (!library_is_exiting())
    {
      /* Enable inotify */
      event_add(inoev, NULL);
    }
  return 0;
}

static int
filescanner_fullrescan()
{
  DPRINTF(E_LOG, L_SCAN, "Full rescan triggered\n");

  inofd_event_unset(); // Clears all inotify watches
  inofd_event_set();
  bulk_scan(NULL, F_SCAN_BULK);

  if (!library_is_exiting())
    {
      /* Enable inotify */
      event_add(inoev, NULL);
    }
  return 0;
}

static int
filescanner_write_metadata(struct media_file_info *mfi)
{
  return write_metadata_ffmpeg(mfi);
}

static int
queue_item_file_add(const char *sub_uri, int position, char reshuffle, uint32_t item_id, int *count, int *new_item_id)
{
  struct query_params query_params = { 0 };
  int64_t id;

  if (strncmp(sub_uri, "artist:", strlen("artist:")) == 0)
    {
      if (safe_atoi64(sub_uri + (strlen("artist:")), &id) < 0)
	return -1;

      query_params.type = Q_GROUP_ITEMS;
      query_params.sort = S_ALBUM;
      query_params.persistentid = id;
    }
  else if (strncmp(sub_uri, "album:", strlen("album:")) == 0)
    {
      if (safe_atoi64(sub_uri + (strlen("album:")), &id) < 0)
	return -1;

      query_params.type = Q_GROUP_ITEMS;
      query_params.sort = S_ALBUM;
      query_params.persistentid = id;
    }
  else if (strncmp(sub_uri, "track:", strlen("track:")) == 0)
    {
      if (safe_atoi64(sub_uri + (strlen("track:")), &id) < 0)
	return -1;

      query_params.type = Q_ITEMS;
      query_params.id = id;
    }
  else if (strncmp(sub_uri, "playlist:", strlen("playlist:")) == 0)
    {
      if (safe_atoi64(sub_uri + (strlen("playlist:")), &id) < 0)
	return -1;

      query_params.type = Q_PLITEMS;
      query_params.id = id;
    }
  else
    {
      return -1;
    }

  return db_queue_add_by_query(&query_params, reshuffle, item_id, position, count, new_item_id);
}

static int
queue_item_stream_add(const char *path, int position, char reshuffle, uint32_t item_id, int *count, int *new_item_id)
{
  struct media_file_info mfi = { 0 };
  struct db_queue_item qi;
  struct db_queue_add_info queue_add_info;
  int ret;

  scan_metadata_stream(&mfi, path);

  db_queue_item_from_mfi(&qi, &mfi);

  ret = db_queue_add_start(&queue_add_info, position);
  if (ret < 0)
    goto error;

  ret = db_queue_add_next(&queue_add_info, &qi);
  ret = db_queue_add_end(&queue_add_info, reshuffle, item_id, ret);
  if (ret < 0)
    goto error;

  if (count)
    *count = queue_add_info.count;
  if (new_item_id)
    *new_item_id = queue_add_info.new_item_id;

  free_queue_item(&qi, 1);
  free_mfi(&mfi, 1);
  return 0;

 error:
  free_queue_item(&qi, 1);
  free_mfi(&mfi, 1);
  return -1;
}

static int
queue_item_add(const char *uri, int position, char reshuffle, uint32_t item_id, int *count, int *new_item_id)
{
  int ret;

  if (strncmp(uri, "library:", strlen("library:")) == 0)
    ret = queue_item_file_add(uri + strlen("library:"), position, reshuffle, item_id, count, new_item_id);
  else if (net_is_http_or_https(uri))
    ret = queue_item_stream_add(uri, position, reshuffle, item_id, count, new_item_id);
  else
    ret = -1;

  return (ret == 0) ? LIBRARY_OK : LIBRARY_PATH_INVALID;
}

static const char *
virtual_path_to_path(const char *virtual_path)
{
  if (strncmp(virtual_path, "/file:", strlen("/file:")) == 0)
    return virtual_path + strlen("/file:");

  if (strncmp(virtual_path, "file:", strlen("file:")) == 0)
    return virtual_path + strlen("file:");

  return NULL;
}

static bool
check_path_in_directories(const char *path)
{
  cfg_t *lib;
  int ndirs;
  int i;
  char *tmp_path;
  char *dir;
  const char *lib_dir;
  bool ret;

  if (strstr(path, "/../"))
    return false;

  tmp_path = strdup(path);
  dir = dirname(tmp_path);
  if (!dir)
    {
      free(tmp_path);
      return false;
    }

  ret = false;
  lib = cfg_getsec(cfg, "library");
  ndirs = cfg_size(lib, "directories");
  for (i = 0; i < ndirs; i++)
    {
      lib_dir = cfg_getnstr(lib, "directories", i);
      if (strncmp(dir, lib_dir, strlen(lib_dir)) == 0)
	{
	  ret = true;
	  break;
	}
    }

  free(tmp_path);
  return ret;
}

static bool
has_suffix(const char *file, const char *suffix)
{
  return (strlen(file) > 4 && !strcmp(file + strlen(file) - 4, suffix));
}

/*
 * Checks if the given virtual path for a playlist is a valid path for an m3u playlist file in one
 * of the configured library directories and translates it to real path.
 *
 * Returns NULL on error and a new allocated path on success.
 */
static char *
playlist_path_create(const char *vp_playlist)
{
  const char *path;
  char *pl_path;
  struct playlist_info *pli;

  path = virtual_path_to_path(vp_playlist);
  if (!path)
    {
      DPRINTF(E_LOG, L_SCAN, "Unsupported virtual path '%s'\n", vp_playlist);
      return NULL;
    }

  pl_path = safe_asprintf("%s.m3u", path);

  if (!check_path_in_directories(pl_path))
    {
      DPRINTF(E_LOG, L_SCAN, "Path '%s' is not a virtual path for a configured (local) library directory.\n", pl_path);
      free(pl_path);
      return NULL;
    }

  pli = db_pl_fetch_byvirtualpath(vp_playlist);
  if (pli && (pli->type != PL_PLAIN || !has_suffix(pli->path, ".m3u")))
    {
      DPRINTF(E_LOG, L_SCAN, "Playlist with virtual path '%s' already exists and is not a m3u playlist.\n", vp_playlist);
      free_pli(pli, 0);
      free(pl_path);
      return NULL;
    }
  free_pli(pli, 0);

  return pl_path;
}

static int
playlist_add_path(FILE *fp, int pl_id, const char *path)
{
  int ret;

  ret = fprintf(fp, "%s\n", path);
  if (ret >= 0)
    {
      ret = db_pl_add_item_bypath(pl_id, path);
    }

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_SCAN, "Failed to add path '%s' to playlist (id = %d)\n", path, pl_id);
      return -1;
    }

  return 0;
}

static int
playlist_add_files(FILE *fp, int pl_id, const char *virtual_path)
{
  struct query_params qp;
  struct db_media_file_info dbmfi;
  uint32_t data_kind;
  const char *path;
  struct media_file_info mfi;
  int ret;

  memset(&qp, 0, sizeof(struct query_params));
  qp.type = Q_ITEMS;
  qp.sort = S_ARTIST;
  qp.idx_type = I_NONE;
  qp.filter = db_mprintf("(f.virtual_path = %Q OR f.virtual_path LIKE '%q/%%')", virtual_path, virtual_path);

  ret = db_query_start(&qp);
  if (ret < 0)
    goto out;

  if (qp.results > 0)
    {
      while ((ret = db_query_fetch_file(&dbmfi, &qp)) == 0)
        {
	  if ((safe_atou32(dbmfi.data_kind, &data_kind) < 0)
	      || (data_kind == DATA_KIND_PIPE))
	    {
	      DPRINTF(E_WARN, L_SCAN, "Item '%s' not added to playlist (id = %d), unsupported data kind\n", dbmfi.path, pl_id);
	      continue;
	    }

	  ret = playlist_add_path(fp, pl_id, dbmfi.path);
	  if (ret < 0)
	    break;

	  DPRINTF(E_DBG, L_SCAN, "Item '%s' added to playlist (id = %d)\n", dbmfi.path, pl_id);
	}
    }
  else if (virtual_path[0] != '\0' && net_is_http_or_https(virtual_path + 1))
    {
      path = (virtual_path + 1);

      DPRINTF(E_DBG, L_SCAN, "Scan stream '%s' and add to playlist (id = %d)\n", path, pl_id);

      memset(&mfi, 0, sizeof(struct media_file_info));
      scan_metadata_stream(&mfi, path);
      library_media_save(&mfi);
      free_mfi(&mfi, 1);

      ret = playlist_add_path(fp, pl_id, path);
      if (ret < 0)
	DPRINTF(E_LOG, L_SCAN, "Failed to add stream '%s' to playlist (id = %d)\n", path, pl_id);
      else
	DPRINTF(E_DBG, L_SCAN, "Item '%s' added to playlist (id = %d)\n", path, pl_id);
    }

 out:
  db_query_end(&qp);
  free(qp.filter);

  return ret;
}

static int
playlist_item_add(const char *vp_playlist, const char *vp_item)
{
  char *pl_path;
  FILE *fp;
  int pl_id;
  int ret;

  pl_path = playlist_path_create(vp_playlist);
  if (!pl_path)
    return LIBRARY_PATH_INVALID;

  fp = fopen(pl_path, "a");
  if (!fp)
    {
      DPRINTF(E_LOG, L_SCAN, "Error opening file '%s' for writing: %d\n", pl_path, errno);
      goto error;
    }

  pl_id = db_pl_id_bypath(pl_path);
  if (pl_id < 0)
    {
      pl_id = playlist_add(pl_path);
      if (pl_id < 0)
	goto error;
    }

  ret = playlist_add_files(fp, pl_id, vp_item);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_SCAN, "Could not add %s to playlist\n", vp_item);
      goto error;
    }

  fclose(fp);
  free(pl_path);

  db_pl_ping(pl_id);

  return LIBRARY_OK;

 error:
  if (fp)
    fclose(fp);
  free(pl_path);
  return LIBRARY_ERROR;
}

static int
playlist_remove(const char *vp_playlist)
{
  char *pl_path;
  struct playlist_info *pli;
  int pl_id;
  int ret;

  pl_path = playlist_path_create(vp_playlist);
  if (!pl_path)
    {
      DPRINTF(E_LOG, L_SCAN, "Unsupported virtual path '%s'\n", vp_playlist);
      return LIBRARY_PATH_INVALID;
    }

  pli = db_pl_fetch_byvirtualpath(vp_playlist);
  if (!pli || pli->type != PL_PLAIN)
    {
      DPRINTF(E_LOG, L_SCAN, "Playlist with virtual path '%s' does not exist or is not a plain playlist.\n", vp_playlist);
      free_pli(pli, 0);
      free(pl_path);
      return LIBRARY_ERROR;
    }
  pl_id = pli->id;
  free_pli(pli, 0);

  ret = unlink(pl_path);
  free(pl_path);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_SCAN, "Could not remove playlist \"%s\": %d\n", vp_playlist, errno);
      return LIBRARY_ERROR;
    }

  db_pl_delete(pl_id);
  return LIBRARY_OK;
}

static int
queue_save(const char *virtual_path)
{
  char *pl_path;
  FILE *fp;
  struct query_params query_params;
  struct db_queue_item queue_item;
  struct media_file_info mfi;
  int pl_id;
  int ret;

  pl_path = playlist_path_create(virtual_path);
  if (!pl_path)
    return LIBRARY_PATH_INVALID;

  fp = fopen(pl_path, "a");
  if (!fp)
    {
      DPRINTF(E_LOG, L_SCAN, "Error opening file '%s' for writing: %d\n", pl_path, errno);
      goto error;
    }

  pl_id = db_pl_id_bypath(pl_path);
  if (pl_id < 0)
    {
      pl_id = playlist_add(pl_path);
      if (pl_id < 0)
	goto error;
    }

  memset(&query_params, 0, sizeof(struct query_params));
  ret = db_queue_enum_start(&query_params);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_SCAN, "Failed to start queue enum\n");
      goto error;
    }

  while ((ret = db_queue_enum_fetch(&query_params, &queue_item)) == 0 && queue_item.id > 0)
    {
      if (queue_item.data_kind == DATA_KIND_PIPE)
	{
	  DPRINTF(E_LOG, L_SCAN, "Unsupported data kind for playlist file '%s' ignoring item '%s'\n", virtual_path, queue_item.path);
	  continue;
	}

      if (queue_item.file_id == DB_MEDIA_FILE_NON_PERSISTENT_ID)
	{
	  // If the queue item is not in the library and it is a http stream, scan and add to the library prior to saving to the playlist file.
	  if (queue_item.data_kind == DATA_KIND_HTTP)
	    {
	      DPRINTF(E_DBG, L_SCAN, "Scan stream '%s' and add to playlist (id = %d)\n", queue_item.path, pl_id);

	      memset(&mfi, 0, sizeof(struct media_file_info));
	      scan_metadata_stream(&mfi, queue_item.path);
	      library_media_save(&mfi);
	      free_mfi(&mfi, 1);
	    }
	  else
	    {
	      DPRINTF(E_LOG, L_SCAN, "Unsupported item for playlist file '%s' ignoring item '%s'\n", virtual_path, queue_item.path);
	      continue;
	    }
	}

      ret = fprintf(fp, "%s\n", queue_item.path);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_SCAN, "Failed to write path '%s' to file '%s'\n", queue_item.path, virtual_path);
	  break;
	}

      ret = db_pl_add_item_bypath(pl_id, queue_item.path);
      if (ret < 0)
	DPRINTF(E_WARN, L_SCAN, "Could not add %s to playlist\n", queue_item.path);
      else
	DPRINTF(E_DBG, L_SCAN, "Item '%s' added to playlist (id = %d)\n", queue_item.path, pl_id);
    }

  db_queue_enum_end(&query_params);

  fclose(fp);
  free(pl_path);

  db_pl_ping(pl_id);

  if (ret < 0)
    return LIBRARY_ERROR;

  return LIBRARY_OK;

 error:
  if (fp)
    fclose(fp);
  free(pl_path);
  return LIBRARY_ERROR;
}

/* Thread: main */
static int
filescanner_init(void)
{
  int ret;

  ret = inofd_event_set();
  if (ret < 0)
    {
      return -1;
    }

  return 0;
}

/* Thread: main */
static void
filescanner_deinit(void)
{
  inofd_event_unset();
}


struct library_source filescanner =
{
  .scan_kind = SCAN_KIND_FILES,
  .disabled = 0,
  .init = filescanner_init,
  .deinit = filescanner_deinit,
  .initscan = filescanner_initscan,
  .rescan = filescanner_rescan,
  .rescan_path = filescanner_rescan_path,
  .metarescan = filescanner_metarescan,
  .fullrescan = filescanner_fullrescan,
  .write_metadata = filescanner_write_metadata,
  .playlist_item_add = playlist_item_add,
  .playlist_remove = playlist_remove,
  .queue_save = queue_save,
  .queue_item_add = queue_item_add,
};
