
#ifndef __FILESCANNER_H__
#define __FILESCANNER_H__

#include "db.h"


/* --------------------------- Actual scanners ---------------------------- */

int
scan_metadata_ffmpeg(struct media_file_info *mfi, struct media_file_metadata_info **mfmi, const char *file);

void
scan_metadata_stream(struct media_file_info *mfi, const char *path);

void
scan_playlist(const char *file, time_t mtime, int dir_id);

void
scan_smartpl(const char *file, time_t mtime, int dir_id);

void
scan_itunes_itml(const char *file, time_t mtime, int dir_id);


/* ------------  Common utility functions used by the scanners ------------ */

/* Returns a pointer to the filename part of path.
 *
 * @in path        the complete path
 * @return         pointer to filename
 */
const char *
filename_from_path(const char *path);

/* Sets a title (=filename without extension and path) from a path. Caller must
 * free the result.
 *
 * @in path        the complete path
 * @return         allocated title
 */
char *
title_from_path(const char *path);

/* Iterate up a file path.
 *
 * Example of three calls where path is '/foo/bar/file.mp3', and starting with
 * current = NULL:
 *   ret = parent_dir(&current, path) -> ret = 0, current = /bar/file.mp3
 *   ret = parent_dir(&current, path) -> ret = 0, current = /foo/bar/file.mp3
 *   ret = parent_dir(&current, path) -> ret = -1, current = /foo/bar/file.mp3
 *
 * @in/out current if the input pointer is not a null pointer, it will be moved
 *                 one level up, otherwise it will be set to the point at the
 *                 file's directory
 * @in path        the complete path
 * @return         0 if parent dir found, otherwise -1
 */
int
parent_dir(const char **current, const char *path);

/* Fills a playlist struct with default values based on path. The title will
 * for instance be set to the base filename without file extension. Since
 * the fields in the struct are alloc'ed, caller must free with free_pli().
 *
 * @out pli        the playlist struct to be filled
 * @in path        the path to the playlist
 * @return         0 if ok, negative on error
 */
int
playlist_fill(struct playlist_info *pli, const char *path);

/* Adds a playlist to the database with the fields set by playlist_fill()
 *
 * @in path        the path to the playlist
 * @return         the id of the playlist (pli->id), negative on error
 */
int
playlist_add(const char *path);


/* --------------------------------- Other -------------------------------- */

int
write_metadata_ffmpeg(const struct media_file_info *mfi);

#endif /* !__FILESCANNER_H__ */
