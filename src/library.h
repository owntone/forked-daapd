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

#ifndef SRC_LIBRARY_H_
#define SRC_LIBRARY_H_

#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#include "commands.h"
#include "db.h"

#define LIBRARY_OK 0
#define LIBRARY_ERROR -1
#define LIBRARY_PATH_INVALID -2

typedef void (*library_cb)(void *arg);

/*
 * Argument to library_callback_schedule()
 */
enum library_cb_action
{
  // Add as new callback
  LIBRARY_CB_ADD,
  // Replace callback if it already exists
  LIBRARY_CB_REPLACE,
  // Replace callback if it already exists, otherwise add as new
  LIBRARY_CB_ADD_OR_REPLACE,
  // Delete a callback
  LIBRARY_CB_DELETE,
};

enum library_attrib
{
  LIBRARY_ATTRIB_RATING,
  LIBRARY_ATTRIB_USERMARK,
  LIBRARY_ATTRIB_PLAY_COUNT,
  LIBRARY_ATTRIB_SKIP_COUNT,
  LIBRARY_ATTRIB_TIME_PLAYED,
  LIBRARY_ATTRIB_TIME_SKIPPED,
};

/*
 * Definition of a library source
 *
 * A library source is responsible for scanning items into the library db.
 */
struct library_source
{
  enum scan_kind scan_kind;
  int disabled;

  /*
   * Initialize library source (called from the main thread)
   */
  int (*init)(void);

  /*
   * Shutdown library source (called from the main thread after
   * terminating the library thread)
   */
  void (*deinit)(void);

  /*
   * Run initial scan after startup (called from the library thread)
   */
  int (*initscan)(void);

  /*
   * Run rescan (called from the library thread)
   */
  int (*rescan)(void);

  /*
   * Run a metadata rescan of library even if files not changed (called from the library thread)
   */
  int (*metarescan)(void);

  /*
   * Run a full rescan (purge library entries and rescan) (called from the library thread)
   */
  int (*fullrescan)(void);

  /*
   * Write metadata to an item in the library
   */
  int (*write_metadata)(struct media_file_info *mfi);

  /*
   * Run rescan (called from the library thread)
   */
  int (*rescan_path)(const char *path);

  /*
   * Add an item to the library
   */
  int (*item_add)(const char *path);

  /*
   * Add item to playlist
   */
  int (*playlist_item_add)(const char *vp_playlist, const char *vp_item);

  /*
   * Removes the playlist under the given virtual path
   */
  int (*playlist_remove)(const char *virtual_path);

  /*
   * Save queue as a new playlist under the given virtual path
   */
  int (*queue_save)(const char *virtual_path);

  /*
   * Add item for the given path to the current queue
   */
  int (*queue_item_add)(const char *path, int position, char reshuffle, uint32_t item_id, int *count, int *new_item_id);
};

/* --------------------- Interface towards source backends ----------------- */

/*
 * Adds a mfi if mfi->id == 0, otherwise updates.
 *
 * @param mfi Media to save
 * @return    0 if operation succeeded, -1 on failure.
 */
int
library_media_save(struct media_file_info *mfi);

/*
 * Adds a playlist if pli->id == 0, otherwise updates.
 *
 * @param pli Playlist to save
 * @return    Playlist id if operation succeeded, -1 on failure.
 */
int
library_playlist_save(struct playlist_info *pli);

int
library_directory_save(char *virtual_path, char *path, int disabled, int parent_id, enum scan_kind library_source);

/*
 * @param cb      Callback to call
 * @param arg     Argument to call back with
 * @param timeval How long to wait before calling back
 * @param action  (see enum)
 * @return        id of the scheduled event, -1 on failure
 */
int
library_callback_schedule(library_cb cb, void *arg, struct timeval *wait, enum library_cb_action action);

/*
 * @return true if a running scan should be aborted due to imminent shutdown
 */
bool
library_is_exiting();


/* ------------------------ Library external interface --------------------- */

/*
 * Rescan library: find new, remove deleted and update modified tracks and playlists
 * If a "source_name" is given, only tracks / playlists belonging to that source are
 * updated.
 *
 * Update is done asynchronously in the library thread.
 *
 * @param library_source 0 to update everything, one of LIBRARY_SOURCE_xxx to only update specific source
 */
void
library_rescan(enum scan_kind library_source);

/*
 * Same as library_rescan but also updates unmodified tracks and playlists
 */
void
library_rescan_path(const char *path);

void
library_metarescan(enum scan_kind library_source);

/*
 * Wipe library and do a full rescan of all library sources
 */
void
library_fullrescan();

/*
 * @return true if scan is running, otherwise false
 */
bool
library_is_scanning();

/*
 * @param is_scanning true if scan is running, otherwise false
 */
void
library_set_scanning(bool is_scanning);

/*
 * Trigger for sending the DATABASE event
 *
 * Needs to be called, if an update to the database (library tables) occurred. The DATABASE event
 * is emitted with the delay 'library_update_wait'. It is safe to call this function from any thread.
 */
void
library_update_trigger(short update_events);

int
library_playlist_item_add(const char *vp_playlist, const char *vp_item);

int
library_playlist_remove(char *virtual_path);

int
library_playlist_remove_byid(int plid);

int
library_queue_save(char *path);

int
library_queue_item_add(const char *path, int position, char reshuffle, uint32_t item_id, int *count, int *new_item_id);

int
library_item_add(const char *path);

/*
 * Async function to set selected attributes for an item in the library. In case
 * of ratrings also writes the rating to the source if the "write_rating" config
 * option is enabled.
 */
void
library_item_attrib_save(uint32_t id, enum library_attrib attrib, uint32_t value);

struct library_source **
library_sources(void);

/*
 * Execute the function 'func' with the given argument 'arg' in the library thread.
 *
 * The pointer passed as argument is freed in the library thread after func returned.
 *
 * @param func The function to be executed
 * @param arg Argument passed to func
 * @return 0 if triggering the function execution succeeded, -1 on failure.
 */
int
library_exec_async(command_function func, void *arg);

int
library_init();

void
library_deinit();


#endif /* SRC_LIBRARY_H_ */
