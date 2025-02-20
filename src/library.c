/*
 * Copyright (C) 2015 Christian Meffert <christian.meffert@googlemail.com>
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

#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <event2/event.h>

#include "library.h"
#include "cache.h"
#include "commands.h"
#include "conffile.h"
#include "db.h"
#include "logger.h"
#include "misc.h"
#include "listener.h"
#include "player.h"

#define LIBRARY_MAX_CALLBACKS 16

struct library_callback_register
{
  library_cb cb;
  void *arg;
  struct event *ev;
};

struct playlist_item_add_param
{
  const char *vp_playlist;
  const char *vp_item;
};

struct queue_item_add_param
{
  const char *path;
  int position;
  char reshuffle;
  uint32_t item_id;
  int *count;
  int *new_item_id;
};

struct item_param
{
  const char *path;
  uint32_t id;
  enum library_attrib attrib;
  uint32_t value;
};

static struct commands_base *cmdbase;
static pthread_t tid_library;

struct event_base *evbase_lib;

extern struct library_source filescanner;
#ifdef SPOTIFY
extern struct library_source spotifyscanner;
#endif
extern struct library_source rssscanner;

static struct library_source *sources[] = {
    &filescanner,
#ifdef SPOTIFY
    &spotifyscanner,
#endif
    &rssscanner,
    NULL
};

/* Flag for aborting scan on exit */
static bool scan_exit;

/* Flag for scan in progress */
static bool scanning;

// After being told by db that the library was updated through
// library_update_trigger(), wait 5 seconds before notifying listeners
// of LISTENER_DATABASE. This is to catch bulk updates like automated
// tag editing, music file imports/renames.  This way multiple updates
// are collected for a single update notification (useful to avoid
// repeated library reads from clients).
//
// Note: this update delay does not apply to library scans.  The scans
// use the flag `scanning` for deferring update notifcations.
static struct timeval library_update_wait = { 5, 0 };
static struct event *updateev;

// Counts the number of changes made to the database between to DATABASE
// event notifications
static unsigned int deferred_update_notifications;
static short deferred_update_events;

// Stores callbacks that backends may have requested
static struct library_callback_register library_cb_register[LIBRARY_MAX_CALLBACKS];


/* ------------------- CALLED BY LIBRARY SOURCE MODULES -------------------- */

int
library_media_save(struct media_file_info *mfi)
{
  int ret;

  if (!mfi->path || !mfi->fname || !mfi->scan_kind)
    {
      DPRINTF(E_LOG, L_LIB, "Ignoring media file with missing values (path='%s', fname='%s', scan_kind='%d', data_kind='%d')\n",
	      mfi->path, mfi->fname, mfi->scan_kind, mfi->data_kind);
      return -1;
    }

  if (!mfi->directory_id || !mfi->virtual_path)
    {
      // Missing informations for virtual_path and directory_id (may) lead to misplaced appearance in mpd clients
      DPRINTF(E_WARN, L_LIB, "Media file with missing values (path='%s', directory='%d', virtual_path='%s')\n",
	      mfi->path, mfi->directory_id, mfi->virtual_path);
    }

  if (mfi->id == 0)
    ret = db_file_add(mfi);
  else
    ret = db_file_update(mfi);

  return ret;
}

int
library_playlist_save(struct playlist_info *pli)
{
  if (!pli->path || !pli->scan_kind)
    {
      DPRINTF(E_LOG, L_LIB, "Ignoring playlist with missing values (path='%s', scan_kind='%d')\n",
	      pli->path, pli->scan_kind);
      return -1;
    }

  // Missing virtual_path and directory_id (may) lead to misplaced appearance in
  // mpd clients, but for e.g. spotify:playlistfolder they will not be set

  if (pli->id == 0)
    return db_pl_add(pli);
  else
    return db_pl_update(pli);
}

int
library_directory_save(char *virtual_path, char *path, int disabled, int parent_id, enum scan_kind scan_kind)
{
  struct directory_info di = { 0 };
  int id;
  int ret;

  id = db_directory_id_byvirtualpath(virtual_path);

  di.id = id;
  di.parent_id = parent_id;
  di.virtual_path = safe_strdup(virtual_path);
  di.path = safe_strdup(path);
  di.disabled = disabled;
  di.db_timestamp = (uint64_t)time(NULL);
  di.scan_kind = scan_kind;

  if (di.id == 0)
    ret = db_directory_add(&di, &id);
  else
    ret = db_directory_update(&di);

  free_di(&di, 1);

  if (ret < 0 || id <= 0)
  {
    DPRINTF(E_LOG, L_DB, "Insert or update of directory failed '%s'\n", virtual_path);
    return -1;
  }

  return id;
}

static void
scheduled_cb(int fd, short what, void *arg)
{
  struct library_callback_register *cbreg = arg;
  library_cb cb = cbreg->cb;
  void *cb_arg = cbreg->arg;

  // Must reset the register before calling back, otherwise it won't work if the
  // callback reschedules by calling library_callback_schedule()
  event_free(cbreg->ev);
  memset(cbreg, 0, sizeof(struct library_callback_register));

  DPRINTF(E_DBG, L_LIB, "Executing library callback to %p\n", cb);
  cb(cb_arg);
}

int
library_callback_schedule(library_cb cb, void *arg, struct timeval *wait, enum library_cb_action action)
{
  struct library_callback_register *cbreg;
  bool replace_done;
  int idx_available;
  int i;

  for (i = 0, idx_available = -1, replace_done = false; i < ARRAY_SIZE(library_cb_register); i++)
    {
      if (idx_available == -1 && library_cb_register[i].cb == NULL)
	idx_available = i;

      if (library_cb_register[i].cb != cb)
	continue;

      if (action == LIBRARY_CB_REPLACE || action == LIBRARY_CB_ADD_OR_REPLACE)
	{
	  event_add(library_cb_register[i].ev, wait);
	  library_cb_register[i].arg = arg;
	  replace_done = true;
	}
      else if (action == LIBRARY_CB_DELETE)
	{
	  event_free(library_cb_register[i].ev);
	  memset(&library_cb_register[i], 0, sizeof(struct library_callback_register));
	}
    }

  if (action == LIBRARY_CB_REPLACE || action == LIBRARY_CB_DELETE || (action == LIBRARY_CB_ADD_OR_REPLACE && replace_done))
    {
      return 0; // All done
    }
  else if (idx_available == -1)
    {
      DPRINTF(E_LOG, L_LIB, "Error scheduling callback, register full (size=%d, action=%d)\n", LIBRARY_MAX_CALLBACKS, action);
      return -1;
    }

  cbreg = &library_cb_register[idx_available];
  cbreg->cb = cb;
  cbreg->arg = arg;

  if (!cbreg->ev)
    cbreg->ev = evtimer_new(evbase_lib, scheduled_cb, cbreg);

  CHECK_NULL(L_LIB, cbreg->ev);

  event_add(cbreg->ev, wait);

  DPRINTF(E_DBG, L_LIB, "Added library callback to %p (id %d), wait %ld.%06ld\n", cbreg->cb, idx_available, (long) wait->tv_sec, (long) wait->tv_usec);

  return idx_available;
}


/* ---------------------- LIBRARY ABSTRACTION --------------------- */
/*                          thread: library                         */

static bool
handle_deferred_update_notifications(void)
{
  time_t update_time;
  bool ret = (deferred_update_notifications > 0);

  if (ret)
    {
      DPRINTF(E_DBG, L_LIB, "Database changed (%d changes)\n", deferred_update_notifications);

      deferred_update_notifications = 0;
      update_time = time(NULL);
      db_admin_setint64(DB_ADMIN_DB_UPDATE, (int64_t) update_time);
      db_admin_setint64(DB_ADMIN_DB_MODIFIED, (int64_t) update_time);
    }

  return ret;
}

static void
purge_cruft(time_t start, enum scan_kind scan_kind)
{
  DPRINTF(E_DBG, L_LIB, "Purging old library content\n");
  if (scan_kind > 0)
    db_purge_cruft_bysource(start, scan_kind);
  else
    db_purge_cruft(start);
  db_groups_cleanup();
  db_queue_cleanup();

  if (scan_kind <= 0)
    {
      DPRINTF(E_DBG, L_LIB, "Purging old artwork content\n");
      cache_artwork_purge_cruft(start);
    }
}

static enum command_state
rescan(void *arg, int *ret)
{
  enum scan_kind *scan_kind;
  time_t starttime;
  time_t endtime;
  int i;

  DPRINTF(E_LOG, L_LIB, "Library rescan triggered\n");
  listener_notify(LISTENER_UPDATE);
  starttime = time(NULL);

  scan_kind = arg;

  for (i = 0; sources[i]; i++)
    {
      if (!sources[i]->disabled && sources[i]->rescan)
	{
	  if (*scan_kind > 0 && *scan_kind != sources[i]->scan_kind)
	    {
	      DPRINTF(E_DBG, L_LIB, "Skipping library source '%s'\n", db_scan_kind_label(sources[i]->scan_kind));
	    }
	  else
	    {
	      DPRINTF(E_INFO, L_LIB, "Rescan library source '%s'\n", db_scan_kind_label(sources[i]->scan_kind));
	      sources[i]->rescan();
	    }
	}
      else
	{
	  DPRINTF(E_INFO, L_LIB, "Library source '%s' is disabled\n", db_scan_kind_label(sources[i]->scan_kind));
	}
    }

  purge_cruft(starttime, *scan_kind);

  DPRINTF(E_DBG, L_LIB, "Running post library scan jobs\n");
  db_hook_post_scan();

  endtime = time(NULL);
  DPRINTF(E_LOG, L_LIB, "Library rescan completed in %.f sec (%d changes)\n", difftime(endtime, starttime), deferred_update_notifications);
  scanning = false;

  if (handle_deferred_update_notifications())
    listener_notify(LISTENER_UPDATE | LISTENER_DATABASE);
  else
    listener_notify(LISTENER_UPDATE);

  *ret = 0;
  return COMMAND_END;
}

static enum command_state
rescan_path(void *arg, int *ret)
{
  time_t starttime;
  time_t endtime;
  int i;
  char *path = (char *)arg;
  char virtual_path[PATH_MAX];
  struct stat st;
  int ret1;
  char *ptr;

  // drop any trailing '/' on path
  ptr = path + strlen(path)-1;
  while (ptr > path)
    {
      if (*ptr != '/')
        break;

      *ptr = '\0';
      --ptr;
    }

  listener_notify(LISTENER_UPDATE);
  starttime = time(NULL);

  DPRINTF(E_LOG, L_LIB, "Library partial rescan triggered: '%s'\n", path);
  ret1 = lstat(path, &st);
  if (ret1 < 0 || (ret1 == 0 && (st.st_mode & S_IFMT) != S_IFDIR))
    {
      DPRINTF(E_LOG, L_LIB, "Partial rescan on '%s' is not a valid directory\n", path);
      goto out;
    }

  // protecting everything else other than the request path
  db_file_ping_excl_bymatch(path);
  db_pl_ping_excl_bymatch(path);
  ret1 = snprintf(virtual_path, sizeof(virtual_path), "/file:%s", path);
  if ((ret1 < 0) || (ret1 >= sizeof(virtual_path)))
    DPRINTF(E_LOG, L_SCAN, "Virtual path exceeds PATH_MAX (/file:%s)\n", path);
  else
    db_directory_ping_excl_bymatch(virtual_path);

  for (i = 0; sources[i]; i++)
    {
      if (!sources[i]->disabled && sources[i]->rescan_path && sources[i]->scan_kind == SCAN_KIND_FILES)
	{
	  DPRINTF(E_INFO, L_LIB, "Rescan partial library source '%s'\n", db_scan_kind_label(sources[i]->scan_kind));
	  sources[i]->rescan_path(path);
	}
      else
	{
	  DPRINTF(E_INFO, L_LIB, "Library partial source '%s' is disabled\n", db_scan_kind_label(sources[i]->scan_kind));
	}
    }

  purge_cruft(starttime, SCAN_KIND_FILES);

  DPRINTF(E_DBG, L_LIB, "Running post library partial scan jobs\n");
  db_hook_post_scan();

out:
  endtime = time(NULL);
  DPRINTF(E_LOG, L_LIB, "Library partial rescan completed in %.f sec (%d changes)\n", difftime(endtime, starttime), deferred_update_notifications);
  scanning = false;

  if (handle_deferred_update_notifications())
    listener_notify(LISTENER_UPDATE | LISTENER_DATABASE);
  else
    listener_notify(LISTENER_UPDATE);

  *ret = 0;
  return COMMAND_END;
}

static enum command_state
metarescan(void *arg, int *ret)
{
  enum scan_kind *scan_kind;
  time_t starttime;
  time_t endtime;
  int i;

  DPRINTF(E_LOG, L_LIB, "Library meta rescan triggered\n");
  listener_notify(LISTENER_UPDATE);
  starttime = time(NULL);

  scan_kind = arg;

  for (i = 0; sources[i]; i++)
    {
      if (!sources[i]->disabled && sources[i]->metarescan)
	{
	  if (*scan_kind > 0 && *scan_kind != sources[i]->scan_kind)
	    {
	      DPRINTF(E_DBG, L_LIB, "Skipping library source '%s'\n", db_scan_kind_label(sources[i]->scan_kind));
	    }
	  else
	    {
	      DPRINTF(E_INFO, L_LIB, "Meta rescan library source '%s'\n", db_scan_kind_label(sources[i]->scan_kind));
	      sources[i]->metarescan();
	    }
	}
      else
	{
	  DPRINTF(E_INFO, L_LIB, "Library source '%s' is disabled\n", db_scan_kind_label(sources[i]->scan_kind));
	}
    }

  purge_cruft(starttime, *scan_kind);

  DPRINTF(E_DBG, L_LIB, "Running post library scan jobs\n");
  db_hook_post_scan();

  endtime = time(NULL);
  DPRINTF(E_LOG, L_LIB, "Library meta rescan completed in %.f sec (%d changes)\n", difftime(endtime, starttime), deferred_update_notifications);
  scanning = false;

  if (handle_deferred_update_notifications())
    listener_notify(LISTENER_UPDATE | LISTENER_DATABASE);
  else
    listener_notify(LISTENER_UPDATE);

  *ret = 0;
  return COMMAND_END;
}


static enum command_state
fullrescan(void *arg, int *ret)
{
  time_t starttime;
  time_t endtime;
  int i;

  DPRINTF(E_LOG, L_LIB, "Library full-rescan triggered\n");
  listener_notify(LISTENER_UPDATE);
  starttime = time(NULL);

  player_playback_stop();
  db_queue_clear(0);
  db_purge_all(); // Clears files, playlists, playlistitems, inotify and groups, incl RSS

  for (i = 0; sources[i]; i++)
    {
      if (!sources[i]->disabled && sources[i]->fullrescan)
	{
	  DPRINTF(E_INFO, L_LIB, "Full-rescan library source '%s'\n", db_scan_kind_label(sources[i]->scan_kind));
	  sources[i]->fullrescan();
	}
      else
	{
	  DPRINTF(E_INFO, L_LIB, "Library source '%s' is disabled\n", db_scan_kind_label(sources[i]->scan_kind));
	}
    }

  endtime = time(NULL);
  DPRINTF(E_LOG, L_LIB, "Library full-rescan completed in %.f sec (%d changes)\n", difftime(endtime, starttime), deferred_update_notifications);
  scanning = false;

  if (handle_deferred_update_notifications())
    listener_notify(LISTENER_UPDATE | LISTENER_DATABASE);
  else
    listener_notify(LISTENER_UPDATE);

  *ret = 0;
  return COMMAND_END;
}

static enum command_state
playlist_item_add(void *arg, int *retval)
{
  struct playlist_item_add_param *param = arg;
  int i;
  int ret = LIBRARY_ERROR;

  DPRINTF(E_DBG, L_LIB, "Adding item '%s' to playlist '%s'\n", param->vp_item, param->vp_playlist);

  for (i = 0; sources[i]; i++)
    {
      if (sources[i]->disabled || !sources[i]->playlist_item_add)
	{
	  DPRINTF(E_DBG, L_LIB, "Library source '%s' is disabled or does not support playlist_item_add\n", db_scan_kind_label(sources[i]->scan_kind));
	  continue;
	}

      ret = sources[i]->playlist_item_add(param->vp_playlist, param->vp_item);

      if (ret == LIBRARY_OK)
	{
	  DPRINTF(E_DBG, L_LIB, "Adding item '%s' to playlist '%s' with library source '%s'\n", param->vp_item, param->vp_playlist, db_scan_kind_label(sources[i]->scan_kind));
	  listener_notify(LISTENER_STORED_PLAYLIST);
	  break;
	}
    }

  *retval = ret;
  return COMMAND_END;
}

static enum command_state
playlist_remove(void *arg, int *retval)
{
  const char *virtual_path = arg;
  int i;
  int ret = LIBRARY_ERROR;

  DPRINTF(E_DBG, L_LIB, "Removing playlist at path '%s'\n", virtual_path);

  for (i = 0; sources[i]; i++)
    {
      if (sources[i]->disabled || !sources[i]->playlist_remove)
	{
	  DPRINTF(E_DBG, L_LIB, "Library source '%s' is disabled or does not support playlist_remove\n", db_scan_kind_label(sources[i]->scan_kind));
	  continue;
	}

      ret = sources[i]->playlist_remove(virtual_path);

      if (ret == LIBRARY_OK)
	{
	  DPRINTF(E_DBG, L_LIB, "Removing playlist '%s' with library source '%s'\n", virtual_path, db_scan_kind_label(sources[i]->scan_kind));
	  listener_notify(LISTENER_STORED_PLAYLIST);
	  break;
	}
    }

  *retval = ret;
  return COMMAND_END;
}

static enum command_state
queue_item_add(void *arg, int *retval)
{
  struct queue_item_add_param *param = arg;
  int i;
  int ret;

  DPRINTF(E_DBG, L_LIB, "Add items for path '%s' to the queue\n", param->path);

  ret = LIBRARY_PATH_INVALID;
  for (i = 0; sources[i] && ret == LIBRARY_PATH_INVALID; i++)
    {
      if (sources[i]->disabled || !sources[i]->queue_item_add)
        {
	  DPRINTF(E_DBG, L_LIB, "Library source '%s' is disabled or does not support queue_add\n", db_scan_kind_label(sources[i]->scan_kind));
	  continue;
	}

      ret = sources[i]->queue_item_add(param->path, param->position, param->reshuffle, param->item_id, param->count, param->new_item_id);

      if (ret == LIBRARY_OK)
	{
	  DPRINTF(E_DBG, L_LIB, "Items for path '%s' from library source '%s' added to the queue\n", param->path, db_scan_kind_label(sources[i]->scan_kind));
	  break;
	}
    }

  if (ret != LIBRARY_OK)
    DPRINTF(E_LOG, L_LIB, "Failed to add items for path '%s' to the queue (%d)\n", param->path, ret);

  *retval = ret;
  return COMMAND_END;
}

static enum command_state
queue_save(void *arg, int *retval)
{
  const char *virtual_path = arg;
  int i;
  int ret = LIBRARY_ERROR;

  DPRINTF(E_DBG, L_LIB, "Saving queue to path '%s'\n", virtual_path);

  for (i = 0; sources[i]; i++)
    {
      if (sources[i]->disabled || !sources[i]->queue_save)
	{
	  DPRINTF(E_DBG, L_LIB, "Library source '%s' is disabled or does not support queue_save\n", db_scan_kind_label(sources[i]->scan_kind));
	  continue;
	}

      ret = sources[i]->queue_save(virtual_path);

      if (ret == LIBRARY_OK)
	{
	  DPRINTF(E_DBG, L_LIB, "Saving queue to path '%s' with library source '%s'\n", virtual_path, db_scan_kind_label(sources[i]->scan_kind));
	  listener_notify(LISTENER_STORED_PLAYLIST);
	  break;
	}
    }

  *retval = ret;
  return COMMAND_END;
}

static enum command_state
item_add(void *arg, int *retval)
{
  struct item_param *param = arg;
  int i;
  int ret = LIBRARY_ERROR;

  DPRINTF(E_DBG, L_LIB, "Adding item to library '%s'\n", param->path);

  for (i = 0; sources[i]; i++)
    {
      if (sources[i]->disabled || !sources[i]->item_add)
	{
	  DPRINTF(E_DBG, L_LIB, "Library source '%s' is disabled or does not support add_item\n", db_scan_kind_label(sources[i]->scan_kind));
	  continue;
	}

      ret = sources[i]->item_add(param->path);

      if (ret == LIBRARY_OK)
	{
	  DPRINTF(E_DBG, L_LIB, "Add item to path '%s' with library source '%s'\n", param->path, db_scan_kind_label(sources[i]->scan_kind));
	  listener_notify(LISTENER_DATABASE);
	  break;
	}
    }

  scanning = false;

  if (ret == LIBRARY_OK)
    {
      if (handle_deferred_update_notifications())
	listener_notify(LISTENER_UPDATE | LISTENER_DATABASE);
      else
	listener_notify(LISTENER_UPDATE);
    }

  *retval = ret;
  return COMMAND_END;
}

static int
write_metadata(struct media_file_info *mfi)
{
  int ret;
  int i;

  for (i = 0; sources[i]; i++)
    {
      if (sources[i]->disabled || !sources[i]->write_metadata)
	continue;

      ret = sources[i]->write_metadata(mfi);
      if (ret == LIBRARY_OK)
	return ret;
    }

  return LIBRARY_PATH_INVALID;
}

static enum command_state
item_attrib_save(void *arg, int *retval)
{
  struct item_param *param = arg;
  struct media_file_info *mfi = NULL;
  int ret;

  if (scanning)
    goto error;

  mfi = db_file_fetch_byid(param->id);
  if (!mfi)
    goto error;

  *retval = LIBRARY_OK;

  switch (param->attrib)
    {
      case LIBRARY_ATTRIB_RATING:
	if (param->value > DB_FILES_RATING_MAX)
	  goto error;

	mfi->rating = param->value;

	if (cfg_getbool(cfg_getsec(cfg, "library"), "write_rating"))
	  *retval = write_metadata(mfi);

        listener_notify(LISTENER_RATING);
	break;

      case LIBRARY_ATTRIB_USERMARK:
	mfi->usermark = param->value;
	break;

      case LIBRARY_ATTRIB_PLAY_COUNT:
	mfi->play_count = param->value;
	break;

      case LIBRARY_ATTRIB_SKIP_COUNT:
	mfi->skip_count = param->value;
	break;

      case LIBRARY_ATTRIB_TIME_PLAYED:
	mfi->time_played = param->value;
	break;

      case LIBRARY_ATTRIB_TIME_SKIPPED:
	mfi->time_skipped = param->value;
	break;

       default:
	goto error;
    }

  ret = db_file_update(mfi);
  if (ret < 0)
    goto error;

  free_mfi(mfi, 0);
  return COMMAND_END;

 error:
  DPRINTF(E_LOG, L_LIB, "Error updating attribute %d to %d for file with id %d\n", param->attrib, param->value, param->id);
  *retval = LIBRARY_ERROR;
  free_mfi(mfi, 0);
  return COMMAND_END;
}

// Callback to notify listeners of database changes
static void
update_trigger_cb(int fd, short what, void *arg)
{
  if (handle_deferred_update_notifications())
    {
      listener_notify(deferred_update_events);
      deferred_update_events = 0;
    }
}

static enum command_state
update_trigger(void *arg, int *retval)
{
  short *events = arg;

  ++deferred_update_notifications;
  deferred_update_events |= *events;

  // Only add the timer event if the update occurred outside a (init-/re-/fullre-) scan.
  // The scanning functions take care of notifying clients of database changes directly
  // after the scan finished.
  if (!scanning)
    evtimer_add(updateev, &library_update_wait);

  *retval = 0;
  return COMMAND_END;
}


/* ----------------------- LIBRARY EXTERNAL INTERFACE ---------------------- */

void
library_rescan(enum scan_kind scan_kind)
{
  int *param;

  if (scanning)
    {
      DPRINTF(E_INFO, L_LIB, "Scan already running, ignoring request to trigger a new init scan\n");
      return;
    }

  scanning = true;
  param = malloc(sizeof(int));
  *param = scan_kind;

  commands_exec_async(cmdbase, rescan, param);
}

void
library_rescan_path(const char *path)
{
  if (scanning)
    {
      DPRINTF(E_INFO, L_LIB, "Scan already running, ignoring request to trigger a new init path scan\n");
      return;
    }

  scanning = true; // TODO Guard "scanning" with a mutex
  commands_exec_async(cmdbase, rescan_path, (void*)strdup(path));
}

void
library_metarescan(enum scan_kind scan_kind)
{
  int *param;

  if (scanning)
    {
      DPRINTF(E_INFO, L_LIB, "Scan already running, ignoring request to trigger metadata scan\n");
      return;
    }

  scanning = true;
  param = malloc(sizeof(int));
  *param = scan_kind;

  commands_exec_async(cmdbase, metarescan, param);
}

void
library_fullrescan()
{
  if (scanning)
    {
      DPRINTF(E_INFO, L_LIB, "Scan already running, ignoring request to trigger a new full rescan\n");
      return;
    }

  scanning = true;
  commands_exec_async(cmdbase, fullrescan, NULL);
}

static void
initscan()
{
  time_t starttime;
  time_t endtime;
  bool clear_queue_disabled;
  int i;

  scanning = true;
  starttime = time(NULL);
  listener_notify(LISTENER_UPDATE);

  // Only clear the queue if enabled (default) in config
  clear_queue_disabled = cfg_getbool(cfg_getsec(cfg, "library"), "clear_queue_on_stop_disable");

  /* Handle deprecated config options */
  if (0 < cfg_opt_size(cfg_getopt(cfg_getsec(cfg, "mpd"), "clear_queue_on_stop_disable")))
    {
      DPRINTF(E_LOG, L_MPD, "Found deprecated option 'clear_queue_on_stop_disable' in section 'mpd', please update configuration file (move option to section 'library').\n");
      clear_queue_disabled = cfg_getbool(cfg_getsec(cfg, "mpd"), "clear_queue_on_stop_disable");
    }

  if (!clear_queue_disabled)
    {
      db_queue_clear(0);
    }

  for (i = 0; sources[i]; i++)
    {
      if (!sources[i]->disabled && sources[i]->initscan)
	sources[i]->initscan();
    }

  if (! (cfg_getbool(cfg_getsec(cfg, "library"), "filescan_disable")))
    {
      purge_cruft(starttime, 0);

      DPRINTF(E_DBG, L_LIB, "Running post library scan jobs\n");
      db_hook_post_scan();
    }

  endtime = time(NULL);
  DPRINTF(E_LOG, L_LIB, "Library init scan completed in %.f sec (%d changes)\n", difftime(endtime, starttime), deferred_update_notifications);

  scanning = false;

  if (handle_deferred_update_notifications())
    listener_notify(LISTENER_UPDATE | LISTENER_DATABASE);
  else
    listener_notify(LISTENER_UPDATE);
}

bool
library_is_scanning()
{
  return scanning;
}

void
library_set_scanning(bool is_scanning)
{
  scanning = is_scanning;
}

bool
library_is_exiting()
{
  return scan_exit;
}

void
library_update_trigger(short update_events)
{
  short *events;
  int ret;

  pthread_t current_thread = pthread_self();
  if (pthread_equal(current_thread, tid_library))
    {
      // We are already running in the library thread, it is safe to directly call update_trigger
      update_trigger(&update_events, &ret);
    }
  else
    {
      events = malloc(sizeof(short));
      *events = update_events;
      commands_exec_async(cmdbase, update_trigger, events);
    }
}

int
library_playlist_item_add(const char *vp_playlist, const char *vp_item)
{
  struct playlist_item_add_param param;

  if (library_is_scanning())
    return -1;

  param.vp_playlist = vp_playlist;
  param.vp_item = vp_item;
  return commands_exec_sync(cmdbase, playlist_item_add, NULL, &param);
}

int
library_playlist_remove(char *virtual_path)
{
  if (library_is_scanning())
    return -1;

  return commands_exec_sync(cmdbase, playlist_remove, NULL, virtual_path);
}

int
library_playlist_remove_byid(int pl_id)
{
  if (scanning)
    {
      DPRINTF(E_INFO, L_LIB, "Scan already running, ignoring request to remove playlist '%d'\n", pl_id);
      return -1;
    }

  db_pl_delete(pl_id);

  if (handle_deferred_update_notifications())
    listener_notify(LISTENER_UPDATE | LISTENER_DATABASE);
  else
    listener_notify(LISTENER_UPDATE);

  return 0;
}

int
library_queue_save(char *path)
{
  if (library_is_scanning())
    return -1;

  return commands_exec_sync(cmdbase, queue_save, NULL, path);
}

int
library_queue_item_add(const char *path, int position, char reshuffle, uint32_t item_id, int *count, int *new_item_id)
{
  struct queue_item_add_param param;
  int count_internal;
  int new_item_id_internal;

  if (library_is_scanning())
    return -1;

  param.path = path;
  param.position = position;
  param.reshuffle = reshuffle;
  param.item_id = item_id;
  param.count = count ? count : &count_internal;
  param.new_item_id = new_item_id ? new_item_id : &new_item_id_internal;

  return commands_exec_sync(cmdbase, queue_item_add, NULL, &param);
}

int
library_item_add(const char *path)
{
  struct item_param param;

  if (scanning)
    {
      DPRINTF(E_INFO, L_LIB, "Scan already running, ignoring request to add item '%s'\n", path);
      return -1;
    }

  scanning = true;

  param.path = path;

  return commands_exec_sync(cmdbase, item_add, NULL, &param);
}

void
library_item_attrib_save(uint32_t id, enum library_attrib attrib, uint32_t value)
{
  struct item_param *param;

  param = malloc(sizeof(struct item_param));
  param->id = id;
  param->attrib = attrib;
  param->value = value;

  commands_exec_async(cmdbase, item_attrib_save, param);
}

struct library_source **
library_sources(void)
{
  return sources;
}

int
library_exec_async(command_function func, void *arg)
{
  return commands_exec_async(cmdbase, func, arg);
}

static void *
library(void *arg)
{
  int ret;

#ifdef __linux__
  struct sched_param param;

  /* Lower the priority of the thread so the server may still respond
   * during library scan on low power devices. Param must be 0 for the SCHED_BATCH
   * policy.
   */
  memset(&param, 0, sizeof(struct sched_param));
  ret = pthread_setschedparam(pthread_self(), SCHED_BATCH, &param);
  if (ret != 0)
    {
      DPRINTF(E_LOG, L_LIB, "Warning: Could not set thread priority to SCHED_BATCH\n");
    }
#endif

  ret = db_perthread_init();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_LIB, "Error: DB init failed\n");

      pthread_exit(NULL);
    }

  initscan();

  event_base_dispatch(evbase_lib);

  if (!scan_exit)
    DPRINTF(E_FATAL, L_LIB, "Scan event loop terminated ahead of time!\n");

  db_hook_post_scan();
  db_perthread_deinit();

  pthread_exit(NULL);
}

/* Thread: main */
int
library_init(void)
{
  int i;
  int ret;

  scan_exit = false;
  scanning = false;

  CHECK_NULL(L_LIB, evbase_lib = event_base_new());
  CHECK_NULL(L_LIB, updateev = evtimer_new(evbase_lib, update_trigger_cb, NULL));

  for (i = 0; sources[i]; i++)
    {
      if (!sources[i]->initscan || !sources[i]->rescan || !sources[i]->metarescan || !sources[i]->fullrescan)
	{
	  DPRINTF(E_FATAL, L_LIB, "BUG: library source '%s' is missing a scanning method\n", db_scan_kind_label(sources[i]->scan_kind));
	  return -1;
	}

      if (sources[i]->init && !sources[i]->disabled)
	{
	  ret = sources[i]->init();
	  if (ret < 0)
	    sources[i]->disabled = 1;
	}
    }

  CHECK_NULL(L_LIB, cmdbase = commands_base_new(evbase_lib, NULL));

  CHECK_ERR(L_LIB, pthread_create(&tid_library, NULL, library, NULL));

  thread_setname(tid_library, "library");

  return 0;
}

/* Thread: main */
void
library_deinit()
{
  int i;
  int ret;

  scan_exit = true;
  commands_base_destroy(cmdbase);

  ret = pthread_join(tid_library, NULL);
  if (ret != 0)
    {
      DPRINTF(E_FATAL, L_LIB, "Could not join library thread: %s\n", strerror(errno));

      return;
    }

  for (i = 0; sources[i]; i++)
    {
      if (sources[i]->deinit && !sources[i]->disabled)
	sources[i]->deinit();
    }

  for (i = 0; i < ARRAY_SIZE(library_cb_register); i++)
    {
      if (library_cb_register[i].ev)
	event_free(library_cb_register[i].ev);
    }

  event_free(updateev);
  event_base_free(evbase_lib);
}
