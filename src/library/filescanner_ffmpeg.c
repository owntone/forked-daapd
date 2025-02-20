/*
 * Copyright (C) 2009-2011 Julien BLACHE <jb@jblache.org>
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
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// For fstat()
#include <sys/types.h>
#include <sys/stat.h>

// For file copy
#include <fcntl.h>
#if defined(__APPLE__)
#include <copyfile.h>
#endif

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>

#include "db.h"
#include "logger.h"
#include "misc.h"
#include "http.h"
#include "conffile.h"

// From libavutil 57.37.100
#if !defined(HAVE_DECL_AV_DICT_ITERATE) || !(HAVE_DECL_AV_DICT_ITERATE)
# define av_dict_iterate(dict, entry) av_dict_get((dict), "", (entry), AV_DICT_IGNORE_SUFFIX)
#endif

/* Mapping between the metadata name(s) and the offset
 * of the equivalent metadata field in struct media_file_info */
struct metadata_map {
  char *key;
  int as_int;
  size_t offset;
  int (*handler_function)(struct media_file_info *, const char *);
  int flags;
};

struct files_metadata_map {
  char *key;
  enum metadata_kind metadata_kind;
  int (*handler_function)(struct media_file_metadata_info **, const char *);
  int flags;
};

// Used for passing errors to DPRINTF (can't count on av_err2str being present)
static char errbuf[64];

static inline char *
err2str(int errnum)
{
  av_strerror(errnum, errbuf, sizeof(errbuf));
  return errbuf;
}

static int
parse_genre(struct media_file_info *mfi, const char *genre_string)
{
  char **genre = (char**)((char *) mfi + mfi_offsetof(genre));
  char *ptr;

  if (*genre) // Previous genre tag exists
    return 0;

  *genre = strdup(genre_string);

  if (cfg_getbool(cfg_getsec(cfg, "library"), "only_first_genre"))
    {
      ptr = strchr(*genre, ';');
      if (ptr)
        *ptr = '\0';
    }

  return 1;
}

static int
parse_slash_separated_ints(const char *string, uint32_t *firstval, uint32_t *secondval)
{
  int numvals = 0;
  char buf[64];
  char *ptr;

  // dict.h: "The returned entry key or value must not be changed, or it will
  // cause undefined behavior" -> so we must make a copy
  snprintf(buf, sizeof(buf), "%s", string);

  ptr = strchr(buf, '/');
  if (ptr)
    {
      *ptr = '\0';
      if (safe_atou32(ptr + 1, secondval) == 0)
        numvals++;
    }

  if (safe_atou32(buf, firstval) == 0)
    numvals++;

  return numvals;
}

static int
parse_track(struct media_file_info *mfi, const char *track_string)
{
  uint32_t *track = (uint32_t *) ((char *) mfi + mfi_offsetof(track));
  uint32_t *total_tracks = (uint32_t *) ((char *) mfi + mfi_offsetof(total_tracks));

  return parse_slash_separated_ints(track_string, track, total_tracks);
}

static int
parse_disc(struct media_file_info *mfi, const char *disc_string)
{
  uint32_t *disc = (uint32_t *) ((char *) mfi + mfi_offsetof(disc));
  uint32_t *total_discs = (uint32_t *) ((char *) mfi + mfi_offsetof(total_discs));

  return parse_slash_separated_ints(disc_string, disc, total_discs);
}

static int
parse_date(struct media_file_info *mfi, const char *date_string)
{
  char year_string[32];
  uint32_t *year = (uint32_t *) ((char *) mfi + mfi_offsetof(year));
  // signed in db.h to handle dates before 1970
  int64_t *date_released = (int64_t *) ((char *) mfi + mfi_offsetof(date_released));
  struct tm tm = { 0 };
  int ret = 0;

  if ((*year == 0) && (safe_atou32(date_string, year) == 0))
    ret++;

  // musl doesn't support %F, so %Y-%m-%d is used instead
  if ( strptime(date_string, "%Y-%m-%dT%T%z", &tm) // ISO 8601, %T=%H:%M:%S
       || strptime(date_string, "%Y-%m-%d %T", &tm)
       || strptime(date_string, "%Y-%m-%d %H:%M", &tm)
       || strptime(date_string, "%Y-%m-%d", &tm)
     )
    {
      *date_released = mktime(&tm);
      ret++;
    }

  if ((*date_released == 0) && (*year != 0))
    {
      snprintf(year_string, sizeof(year_string), "%" PRIu32 "-01-01T12:00:00", *year);
      if (strptime(year_string, "%Y-%m-%dT%T", &tm))
	{
	  *date_released = mktime(&tm);
	  ret++;
	}
    }

  return ret;
}

static int
parse_albumid(struct media_file_info *mfi, const char *id_string)
{
  // Already set by a previous tag that we give higher priority
  if (mfi->songalbumid)
    return 0;

  // Limit hash length to 63 bits, due to signed type in sqlite
  mfi->songalbumid = murmur_hash64(id_string, strlen(id_string), 0) >> 1;
  return 1;
}

static int
parse_rating(struct media_file_info *mfi, const char *rating_string)
{
  cfg_t *library = cfg_getsec(cfg, "library");
  int max_rating;

  if (!cfg_getbool(library, "read_rating"))
    return 0;

  if (safe_atou32(rating_string, &mfi->rating) < 0)
    return 0;

  // Make sure mfi->rating is in proper range
  max_rating = cfg_getint(library, "max_rating");
  if (max_rating < 5) // Invalid config
    max_rating = DB_FILES_RATING_MAX;

  mfi->rating = MIN(DB_FILES_RATING_MAX * mfi->rating / max_rating, DB_FILES_RATING_MAX);
  return 1;
}


/* Lookup is case-insensitive, first occurrence takes precedence */
static const struct metadata_map md_map_generic[] =
  {
    { "title",        0, mfi_offsetof(title),              NULL },
    { "artist",       0, mfi_offsetof(artist),             NULL },
    { "author",       0, mfi_offsetof(artist),             NULL },
    { "album_artist", 0, mfi_offsetof(album_artist),       NULL },
    { "album",        0, mfi_offsetof(album),              NULL },
    { "genre",        0, mfi_offsetof(genre),              parse_genre },
    { "composer",     0, mfi_offsetof(composer),           NULL },
    { "grouping",     0, mfi_offsetof(grouping),           NULL },
    { "orchestra",    0, mfi_offsetof(orchestra),          NULL },
    { "conductor",    0, mfi_offsetof(conductor),          NULL },
    { "comment",      0, mfi_offsetof(comment),            NULL },
    { "description",  0, mfi_offsetof(comment),            NULL },
    { "track",        1, mfi_offsetof(track),              parse_track },
    { "disc",         1, mfi_offsetof(disc),               parse_disc },
    { "year",         1, mfi_offsetof(year),               NULL },
    { "date",         1, mfi_offsetof(date_released),      parse_date },
    { "title-sort",   0, mfi_offsetof(title_sort),         NULL },
    { "artist-sort",  0, mfi_offsetof(artist_sort),        NULL },
    { "album-sort",   0, mfi_offsetof(album_sort),         NULL },
    { "compilation",  1, mfi_offsetof(compilation),        NULL },
    { "lyrics",       0, mfi_offsetof(lyrics),             NULL,       AV_DICT_IGNORE_SUFFIX },
    { "rating",       1, mfi_offsetof(rating),             parse_rating },

    // ALAC sort tags
    { "sort_name",           0, mfi_offsetof(title_sort),         NULL },
    { "sort_artist",         0, mfi_offsetof(artist_sort),        NULL },
    { "sort_album",          0, mfi_offsetof(album_sort),         NULL },
    { "sort_album_artist",   0, mfi_offsetof(album_artist_sort),  NULL },
    { "sort_composer",       0, mfi_offsetof(composer_sort),      NULL },

    // These tags are used to determine if files belong to a common compilation
    // or album, ref. https://picard.musicbrainz.org/docs/tags
    { "MusicBrainz Album Id",         1, mfi_offsetof(songalbumid), parse_albumid },
    { "MUSICBRAINZ_ALBUMID",          1, mfi_offsetof(songalbumid), parse_albumid },
    { "MusicBrainz Release Group Id", 1, mfi_offsetof(songalbumid), parse_albumid },
    { "MusicBrainz DiscID",           1, mfi_offsetof(songalbumid), parse_albumid },
    { "CDDB DiscID",                  1, mfi_offsetof(songalbumid), parse_albumid },
    { "CATALOGNUMBER",                1, mfi_offsetof(songalbumid), parse_albumid },
    { "BARCODE",                      1, mfi_offsetof(songalbumid), parse_albumid },

    { NULL,           0, 0,                                NULL }
  };

static const struct metadata_map md_map_tv[] =
  {
    { "stik",         1, mfi_offsetof(media_kind),         NULL },
    { "show",         0, mfi_offsetof(tv_series_name),     NULL },
    { "episode_id",   0, mfi_offsetof(tv_episode_num_str), NULL },
    { "network",      0, mfi_offsetof(tv_network_name),    NULL },
    { "episode_sort", 1, mfi_offsetof(tv_episode_sort),    NULL },
    { "season_number",1, mfi_offsetof(tv_season_num),      NULL },

    { NULL,           0, 0,                                NULL }
  };

/* NOTE about VORBIS comments:
 *  Only a small set of VORBIS comment fields are officially designated. Most
 *  common tags are at best de facto standards. Currently, metadata conversion
 *  functionality in ffmpeg only adds support for a couple of tags. Specifically,
 *  ALBUMARTIST and TRACKNUMBER are handled as of Feb 1, 2010 (rev 21587). Tags
 *  with names that already match the generic ffmpeg scheme--TITLE and ARTIST,
 *  for example--are of course handled. The rest of these tags are reported to
 *  have been used by various programs in the wild.
 */
static const struct metadata_map md_map_vorbis[] =
  {
    { "albumartist",  0, mfi_offsetof(album_artist),      NULL },
    { "album artist", 0, mfi_offsetof(album_artist),      NULL },
    { "tracknumber",  1, mfi_offsetof(track),             NULL },
    { "tracktotal",   1, mfi_offsetof(total_tracks),      NULL },
    { "totaltracks",  1, mfi_offsetof(total_tracks),      NULL },
    { "discnumber",   1, mfi_offsetof(disc),              NULL },
    { "disctotal",    1, mfi_offsetof(total_discs),       NULL },
    { "totaldiscs",   1, mfi_offsetof(total_discs),       NULL },

    { NULL,           0, 0,                               NULL }
  };

/* NOTE about ID3 tag names:
 *  metadata conversion for ID3v2 tags was added in ffmpeg in september 2009
 *  (rev 20073) for ID3v2.3; support for ID3v2.2 tag names was added in december
 *  2009 (rev 20839).
 *
 * ID3v2.x tags will be removed from the map once a version of ffmpeg containing
 * the changes listed above will be generally available. The more entries in the
 * map, the slower the filescanner gets.
 *
 * Update 20180131: Removed tags supported by ffmpeg 2.5.4 (around 3 years old)
 * + added some tags used for grouping
 * Update 20200114: Removed TDA, TDAT, TYE, TYER, TDR since the they are
 * well supported by ffmpeg, and the server was parsing TDA/TDAT incorrectly
 *
 */
static const struct metadata_map md_map_id3[] =
  {
    { "TT1",                 0, mfi_offsetof(grouping),              NULL },              /* ID3v2.2 */
    { "TIT1",                0, mfi_offsetof(grouping),              NULL },              /* ID3v2.3 */
    { "GP1",                 0, mfi_offsetof(grouping),              NULL },              /* unofficial iTunes */
    { "GRP1",                0, mfi_offsetof(grouping),              NULL },              /* unofficial iTunes */
    { "TCM",                 0, mfi_offsetof(composer),              NULL },              /* ID3v2.2 */
    { "TPA",                 1, mfi_offsetof(disc),                  parse_disc },        /* ID3v2.2 */
    { "XSOA",                0, mfi_offsetof(album_sort),            NULL },              /* ID3v2.3 */
    { "XSOP",                0, mfi_offsetof(artist_sort),           NULL },              /* ID3v2.3 */
    { "XSOT",                0, mfi_offsetof(title_sort),            NULL },              /* ID3v2.3 */
    { "TS2",                 0, mfi_offsetof(album_artist_sort),     NULL },              /* ID3v2.2 */
    { "TSO2",                0, mfi_offsetof(album_artist_sort),     NULL },              /* ID3v2.3 */
    { "ALBUMARTISTSORT",     0, mfi_offsetof(album_artist_sort),     NULL },              /* ID3v2.x */
    { "TSC",                 0, mfi_offsetof(composer_sort),         NULL },              /* ID3v2.2 */
    { "TSOC",                0, mfi_offsetof(composer_sort),         NULL },              /* ID3v2.3 */

    { NULL,                  0, 0,                                   NULL }
  };


static int
parse_list(struct media_file_metadata_info **mfmi, enum metadata_kind md_kind, const char *val, const char *delim)
{
  char *str;
  char *token;
  char *ptr;
  int idx = 0;

  str = strdup(val);

  token = strtok_r(str, delim, &ptr);
  for (token = strtok_r(str, delim, &ptr); token; token = strtok_r(NULL, delim, &ptr))
    {
      struct media_file_metadata_info *mfmi_new;
      mfmi_new = calloc(1, sizeof(struct media_file_metadata_info));
      mfmi_new->metadata_kind = md_kind;
      mfmi_new->value = strdup(token);
      mfmi_new->idx = idx++;
      mfmi_new->next = *mfmi;
      *mfmi = mfmi_new;
    }

  free(str);
  return idx;
}

static int
parse_genre_list(struct media_file_metadata_info **mfmi, const char *val)
{
  return parse_list(mfmi, MD_GENRE, val, ";/,");
}

static int
parse_composer_list(struct media_file_metadata_info **mfmi, const char *val)
{
  return parse_list(mfmi, MD_COMPOSER, val, ";/,");
}

struct files_metadata_map files_md_map[] =
  {
    { "genre",                        MD_GENRE,                      parse_genre_list,    0 },
    { "composer",                     MD_COMPOSER,                   parse_composer_list, 0 },
    { "lyrics",                       MD_LYRICS,                     NULL,                AV_DICT_IGNORE_SUFFIX },
    { "MusicBrainz Album Id",         MD_MUSICBRAINZ_ALBUMID,        NULL,                0 },
    { "MusicBrainz Artist Id",        MD_MUSICBRAINZ_ARTISTID,       NULL,                0 },
    { "MusicBrainz Album Artist Id",  MD_MUSICBRAINZ_ALBUMARTISTID,  NULL,                0 },
    { NULL,                           0,                             NULL,                0 }
  };

static int
extract_metadata_from_dict(struct media_file_info *mfi, AVDictionary *md, const struct metadata_map *md_map)
{
  AVDictionaryEntry *mdt;
  char **strval;
  uint32_t *intval;
  int mdcount = 0;
  int i;

#if 0
  /* Dump all the metadata reported by ffmpeg */
  for (mdt = av_dict_iterate(md, NULL); mdt; mdt = av_dict_iterate(md, mdt))
    {
      DPRINTF(E_DBG, L_SCAN, " -> %s = %s\n", mdt->key, mdt->value);
    }
#endif

  for (i = 0; md_map[i].key != NULL; i++)
    {
      mdt = av_dict_get(md, md_map[i].key, NULL, md_map[i].flags);
      if (!mdt || !mdt->value || strlen(mdt->value) == 0)
	continue;

      if (md_map[i].handler_function)
	{
	  mdcount += md_map[i].handler_function(mfi, mdt->value);
	  continue;
	}

      if (!md_map[i].as_int)
	{
	  strval = (char **) ((char *) mfi + md_map[i].offset);

	  if (*strval != NULL)
	    continue;

	  *strval = strdup(mdt->value);
	}
      else
	{
	  intval = (uint32_t *) ((char *) mfi + md_map[i].offset);

	  if (*intval != 0)
	    continue;

	  if (safe_atou32(mdt->value, intval) < 0)
	    continue;
	}

      mdcount++;
    }

  return mdcount;
}

static int
extract_extra_metadata_from_dict(struct media_file_metadata_info **mfmi, AVDictionary *md, const struct files_metadata_map *md_map)
{
  AVDictionaryEntry *mdt;
  char *strval;
  struct media_file_metadata_info *mfmi_new;
  int mdcount = 0;
  int i;

  for (i = 0; md_map[i].key != NULL; i++)
    {
      mdt = av_dict_get(md, md_map[i].key, NULL, md_map[i].flags);
      if (!mdt || !mdt->value || strlen(mdt->value) == 0)
	continue;

      if (md_map[i].handler_function)
	{
	  mdcount += md_map[i].handler_function(mfmi, mdt->value);
	  continue;
	}
      else
        {
	  strval = strdup(mdt->value);
	  mfmi_new = calloc(1, sizeof(struct media_file_metadata_info));
	  mfmi_new->metadata_kind = md_map[i].metadata_kind;
	  mfmi_new->value = strval;
	  mfmi_new->next = *mfmi;
	  *mfmi = mfmi_new;
	  mdcount++;
        }
    }

  return mdcount;
}

static int
extract_metadata(struct media_file_info *mfi, struct media_file_metadata_info **mfmi, AVFormatContext *ctx, AVStream *audio_stream, AVStream *video_stream, const struct metadata_map *md_map)
{
  int mdcount = 0;
  int ret;

  if (ctx->metadata)
    {
      ret = extract_metadata_from_dict(mfi, ctx->metadata, md_map);
      if (mfmi)
	ret += extract_extra_metadata_from_dict(mfmi, ctx->metadata, files_md_map);
      mdcount += ret;

      DPRINTF(E_DBG, L_SCAN, "Picked up %d tags from file metadata\n", ret);
    }

  if (audio_stream->metadata)
    {
      ret = extract_metadata_from_dict(mfi, audio_stream->metadata, md_map);
      if (mfmi)
	ret += extract_extra_metadata_from_dict(mfmi, audio_stream->metadata, files_md_map);
      mdcount += ret;

      DPRINTF(E_DBG, L_SCAN, "Picked up %d tags from audio stream metadata\n", ret);
    }

  if (video_stream && video_stream->metadata)
    {
      ret = extract_metadata_from_dict(mfi, video_stream->metadata, md_map);
      if (mfmi)
	ret += extract_extra_metadata_from_dict(mfmi, video_stream->metadata, files_md_map);
      mdcount += ret;

      DPRINTF(E_DBG, L_SCAN, "Picked up %d tags from video stream metadata\n", ret);
    }

  return mdcount;
}

/*
 * Fills metadata read with ffmpeg/libav from the given path into the given mfi
 *
 * Following attributes from the given mfi are read to control how to read metadata:
 * - data_kind: if data_kind is http, icy metadata is used, if the path points to a playlist the first stream-uri in that playlist is used
 * - media_kind: if media_kind is podcast or audiobook, video streams in the file are ignored
 * - compilation: like podcast/audiobook video streams are ignored for compilations
 * - file_size: if bitrate could not be read through ffmpeg/libav, file_size is used to estimate the bitrate
 * - fname: (filename) used as fallback for artist
 */
int
scan_metadata_ffmpeg(struct media_file_info *mfi, struct media_file_metadata_info **mfmi, const char *file)
{
  AVFormatContext *ctx;
  AVDictionary *options;
  const struct metadata_map *extra_md_map;
  struct http_icy_metadata *icy_metadata;
  enum AVMediaType codec_type;
  enum AVCodecID codec_id;
  enum AVCodecID video_codec_id;
  enum AVCodecID audio_codec_id;
  enum AVSampleFormat sample_fmt;
  AVStream *video_stream;
  AVStream *audio_stream;
  char *path;
  int mdcount;
  int sample_rate;
  int channels;
  int i;
  int ret;

  ctx = NULL;
  options = NULL;
  path = strdup(file);

  if (mfi->data_kind == DATA_KIND_HTTP)
    {
#ifndef HAVE_FFMPEG
      // Without this, libav is slow to probe some internet streams
      ctx = avformat_alloc_context();
      ctx->probesize = 64000;
#endif

      free(path);
      ret = http_stream_setup(&path, file);
      if (ret < 0)
	return -1;

      av_dict_set(&options, "icy", "1", 0);
    }
  else if (mfi->data_kind == DATA_KIND_FILE && mfi->file_size == 0)
    {
      // a 0-byte mp3 will make ffmpeg die with arithmetic exception (with 3.2.15-0+deb9u4)
      free(path);
      return -1;
    }

  ret = avformat_open_input(&ctx, path, NULL, &options);

  if (options)
    av_dict_free(&options);

  if (ret != 0)
    {
      DPRINTF(E_WARN, L_SCAN, "Cannot open media file '%s': %s\n", path, err2str(ret));

      free(path);
      return -1;
    }

  ret = avformat_find_stream_info(ctx, NULL);
  if (ret < 0)
    {
      DPRINTF(E_WARN, L_SCAN, "Cannot get stream info of '%s': %s\n", path, err2str(ret));

      avformat_close_input(&ctx);
      free(path);
      return -1;
    }

  free(path);

#if 0
  /* Dump input format as determined by ffmpeg */
  av_dump_format(ctx, 0, file, 0);
#endif

  DPRINTF(E_DBG, L_SCAN, "File has %d streams\n", ctx->nb_streams);

  /* Extract codec IDs, check for video */
  video_codec_id = AV_CODEC_ID_NONE;
  video_stream = NULL;

  audio_codec_id = AV_CODEC_ID_NONE;
  audio_stream = NULL;

  for (i = 0; i < ctx->nb_streams; i++)
    {
      codec_type = ctx->streams[i]->codecpar->codec_type;
      codec_id = ctx->streams[i]->codecpar->codec_id;
      sample_rate = ctx->streams[i]->codecpar->sample_rate;
      sample_fmt = ctx->streams[i]->codecpar->format;
// Matches USE_CH_LAYOUT in transcode.c
#if (LIBAVCODEC_VERSION_MAJOR > 59) || ((LIBAVCODEC_VERSION_MAJOR == 59) && (LIBAVCODEC_VERSION_MINOR > 24))
      channels = ctx->streams[i]->codecpar->ch_layout.nb_channels;
#else
      channels = ctx->streams[i]->codecpar->channels;
#endif
      switch (codec_type)
	{
	  case AVMEDIA_TYPE_VIDEO:
	    if (ctx->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC)
	      {
		DPRINTF(E_DBG, L_SCAN, "Found embedded artwork (stream %d)\n", i);
		mfi->artwork = ARTWORK_EMBEDDED;

		break;
	      }

	    // We treat these as audio no matter what
	    if (mfi->compilation || (mfi->media_kind & (MEDIA_KIND_PODCAST | MEDIA_KIND_AUDIOBOOK)))
	      break;

	    if (!video_stream)
	      {
		DPRINTF(E_DBG, L_SCAN, "File has video (stream %d)\n", i);

		video_stream = ctx->streams[i];
		video_codec_id = codec_id;

		mfi->has_video = 1;
	      }
	    break;

	  case AVMEDIA_TYPE_AUDIO:
	    if (!audio_stream)
	      {
		audio_stream = ctx->streams[i];
		audio_codec_id = codec_id;

		mfi->samplerate = sample_rate;
		mfi->bits_per_sample = 8 * av_get_bytes_per_sample(sample_fmt);
		if (mfi->bits_per_sample == 0)
		  mfi->bits_per_sample = av_get_bits_per_sample(codec_id);
		mfi->channels = channels;
	      }
	    break;

	  default:
	    break;
	}
    }

  if (audio_codec_id == AV_CODEC_ID_NONE)
    {
      DPRINTF(E_DBG, L_SCAN, "File has no audio streams, discarding\n");

      avformat_close_input(&ctx);
      return -1;
    }

  /* Common media information */
  if (ctx->duration > 0)
    mfi->song_length = ctx->duration / (AV_TIME_BASE / 1000); /* ms */

  if (ctx->bit_rate > 0)
    mfi->bitrate = ctx->bit_rate / 1000;
  else if (ctx->duration > AV_TIME_BASE) /* guesstimate */
    mfi->bitrate = ((mfi->file_size * 8) / (ctx->duration / AV_TIME_BASE)) / 1000;

  DPRINTF(E_DBG, L_SCAN, "Duration %d ms, bitrate %d kbps, samplerate %d channels %d\n", mfi->song_length, mfi->bitrate, mfi->samplerate, mfi->channels);

  /* Try to extract ICY metadata if http stream */
  if (mfi->data_kind == DATA_KIND_HTTP)
    {
      icy_metadata = http_icy_metadata_get(ctx, 0);
      if (icy_metadata && icy_metadata->name)
	{
	  DPRINTF(E_DBG, L_SCAN, "Found ICY metadata, name is '%s'\n", icy_metadata->name);

	  if (mfi->title)
	    free(mfi->title);
	  if (mfi->artist)
	    free(mfi->artist);
	  if (mfi->album_artist)
	    free(mfi->album_artist);

	  mfi->title = strdup(icy_metadata->name);
	  mfi->artist = strdup(icy_metadata->name);
	  mfi->album_artist = strdup(icy_metadata->name);
	}
      if (icy_metadata && icy_metadata->description)
	{
	  DPRINTF(E_DBG, L_SCAN, "Found ICY metadata, description is '%s'\n", icy_metadata->description);

	  if (mfi->album)
	    free(mfi->album);

	  mfi->album = strdup(icy_metadata->description);
	}
      if (icy_metadata && icy_metadata->genre)
	{
	  DPRINTF(E_DBG, L_SCAN, "Found ICY metadata, genre is '%s'\n", icy_metadata->genre);

	  if (mfi->genre)
	    free(mfi->genre);

	  mfi->genre = strdup(icy_metadata->genre);
	}
      if (icy_metadata)
	http_icy_metadata_free(icy_metadata, 0);
    }

  /* Check codec */
  extra_md_map = NULL;
  codec_id = (mfi->has_video) ? video_codec_id : audio_codec_id;
  switch (codec_id)
    {
      case AV_CODEC_ID_AAC:
	DPRINTF(E_DBG, L_SCAN, "AAC\n");
	mfi->type = strdup("m4a");
	mfi->codectype = strdup("mp4a");
	mfi->description = strdup("AAC audio file");
	break;

      case AV_CODEC_ID_ALAC:
	DPRINTF(E_DBG, L_SCAN, "ALAC\n");
	mfi->type = strdup("m4a");
	mfi->codectype = strdup("alac");
	mfi->description = strdup("Apple Lossless audio file");
	break;

      case AV_CODEC_ID_FLAC:
	DPRINTF(E_DBG, L_SCAN, "FLAC\n");
	mfi->type = strdup("flac");
	mfi->codectype = strdup("flac");
	mfi->description = strdup("FLAC audio file");

	extra_md_map = md_map_vorbis;
	break;

      case AV_CODEC_ID_APE:
	DPRINTF(E_DBG, L_SCAN, "APE\n");
	mfi->type = strdup("ape");
	mfi->codectype = strdup("ape");
	mfi->description = strdup("Monkey's audio");
	break;

      case AV_CODEC_ID_MUSEPACK7:
      case AV_CODEC_ID_MUSEPACK8:
	DPRINTF(E_DBG, L_SCAN, "Musepack\n");
	mfi->type = strdup("mpc");
	mfi->codectype = strdup("mpc");
	mfi->description = strdup("Musepack audio file");
	break;

      case AV_CODEC_ID_MPEG4: /* Video */
      case AV_CODEC_ID_H264:
	DPRINTF(E_DBG, L_SCAN, "MPEG4 video\n");
	mfi->type = strdup("m4v");
	mfi->codectype = strdup("mp4v");
	mfi->description = strdup("MPEG-4 video file");

	extra_md_map = md_map_tv;
	break;

      case AV_CODEC_ID_MP3:
	DPRINTF(E_DBG, L_SCAN, "MP3\n");
	mfi->type = strdup("mp3");
	mfi->codectype = strdup("mpeg");
	mfi->description = strdup("MPEG audio file");

	extra_md_map = md_map_id3;
	break;

      case AV_CODEC_ID_VORBIS:
	DPRINTF(E_DBG, L_SCAN, "VORBIS\n");
	mfi->type = strdup("ogg");
	mfi->codectype = strdup("ogg");
	mfi->description = strdup("Ogg Vorbis audio file");

	extra_md_map = md_map_vorbis;
	break;

      case AV_CODEC_ID_WMAV1:
      case AV_CODEC_ID_WMAV2:
      case AV_CODEC_ID_WMAVOICE:
	DPRINTF(E_DBG, L_SCAN, "WMA Voice\n");
	mfi->type = strdup("wma");
	mfi->codectype = strdup("wmav");
	mfi->description = strdup("WMA audio file");
	break;

      case AV_CODEC_ID_WMAPRO:
	DPRINTF(E_DBG, L_SCAN, "WMA Pro\n");
	mfi->type = strdup("wmap");
	mfi->codectype = strdup("wma");
	mfi->description = strdup("WMA audio file");
	break;

      case AV_CODEC_ID_WMALOSSLESS:
	DPRINTF(E_DBG, L_SCAN, "WMA Lossless\n");
	mfi->type = strdup("wma");
	mfi->codectype = strdup("wmal");
	mfi->description = strdup("WMA audio file");
	break;

      case AV_CODEC_ID_PCM_S16LE ... AV_CODEC_ID_PCM_F64LE:
	if (strcmp(ctx->iformat->name, "aiff") == 0)
	  {
	    DPRINTF(E_DBG, L_SCAN, "AIFF\n");
	    mfi->type = strdup("aif");
	    mfi->codectype = strdup("aif");
	    mfi->description = strdup("AIFF audio file");
	    break;
	  }
	else if (strcmp(ctx->iformat->name, "wav") == 0)
	  {
	    DPRINTF(E_DBG, L_SCAN, "WAV\n");
	    mfi->type = strdup("wav");
	    mfi->codectype = strdup("wav");
	    mfi->description = strdup("WAV audio file");
	    break;
	  }
	/* WARNING: will fallthrough to default case, don't move */
	/* FALLTHROUGH */

      default:
	DPRINTF(E_DBG, L_SCAN, "Unknown codec 0x%x (video: %s), format %s (%s)\n",
		codec_id, (mfi->has_video) ? "yes" : "no", ctx->iformat->name, ctx->iformat->long_name);
	mfi->type = strdup("unkn");
	mfi->codectype = strdup("unkn");
	if (mfi->has_video)
	  {
	    mfi->description = strdup("Unknown video file format");
	    extra_md_map = md_map_tv;
	  }
	else
	  mfi->description = strdup("Unknown audio file format");
	break;
    }

  mdcount = 0;

  if ((!ctx->metadata) && (!audio_stream->metadata)
      && (video_stream && !video_stream->metadata))
    {
      DPRINTF(E_WARN, L_SCAN, "ffmpeg reports no metadata\n");

      goto skip_extract;
    }

  if (extra_md_map)
    {
      ret = extract_metadata(mfi, NULL, ctx, audio_stream, video_stream, extra_md_map);
      mdcount += ret;

      DPRINTF(E_DBG, L_SCAN, "Picked up %d tags with extra md_map\n", ret);
    }

  ret = extract_metadata(mfi, mfmi, ctx, audio_stream, video_stream, md_map_generic);
  mdcount += ret;

  DPRINTF(E_DBG, L_SCAN, "Picked up %d tags with generic md_map, %d tags total\n", ret, mdcount);

  /* fix up TV metadata */
  if (mfi->media_kind == 10)
    {
      /* I have no idea why this is, but iTunes reports a media kind of 64 for stik==10 (?!) */
      mfi->media_kind = MEDIA_KIND_TVSHOW;
    }
  /* Unspecified video files are "Movies", media_kind 2 */
  else if (mfi->has_video == 1)
    {
      mfi->media_kind = MEDIA_KIND_MOVIE;
    }

 skip_extract:
  avformat_close_input(&ctx);

  if (mdcount == 0)
    DPRINTF(E_WARN, L_SCAN, "ffmpeg/libav could not extract any metadata\n");

  /* Just in case there's no title set ... */
  if (mfi->title == NULL)
    mfi->title = strdup(mfi->fname);

  /* All done */

  return 0;
}


/* ----------------------- Writing metadata to files ------------------------ */

static int
fast_copy(int fd_dst, int fd_src)
{
  // Here we use kernel-space copying for performance reasons
#if defined(__APPLE__)
  return fcopyfile(fd_src, fd_dst, 0, COPYFILE_ALL);
#else
  struct stat fileinfo = { 0 };
  ssize_t bytes_copied;

  fstat(fd_src, &fileinfo);
  bytes_copied = copy_file_range(fd_src, NULL, fd_dst, NULL, fileinfo.st_size, 0);
  if (bytes_copied < 0 || bytes_copied != fileinfo.st_size)
    return -1;

  return 0;
#endif
}

static int
file_copy(const char *dst, const char *src)
{
  int fd_src = -1;
  int fd_dst = -1;
  int ret;

  fd_src = open(src, O_RDONLY);
  if (fd_src < 0)
    {
      DPRINTF(E_LOG, L_SCAN, "Error opening source '%s' for copy: %s\n", src, strerror(errno));
      goto error;
    }

  fd_dst = open(dst, O_WRONLY);
  if (fd_src < 0)
    {
      DPRINTF(E_LOG, L_SCAN, "Error opening destination '%s' for copy: %s\n", dst, strerror(errno));
      goto error;
    }

  ret = fast_copy(fd_dst, fd_src);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_SCAN, "Error copying '%s' to file '%s': %s\n", src, dst, strerror(errno));
      goto error;
    }

  close(fd_src);
  close(fd_dst);
  return 0;

 error:
  if (fd_src != -1)
    close(fd_src);
  if (fd_dst != -1)
    close(fd_dst);
  return -1;
}

static int
file_copy_to_tmp(char *dst, size_t dst_size, const char *src)
{
  int fd_src = -1;
  int fd_dst = -1;
  const char *ext;
  int ret;

  ext = strrchr(src, '.');
  if (!ext || strlen(ext) < 2)
    return -1;

  // Obviously, copying only requires read access, but we will need write access
  // later, so let's fail early if it isn't going to work.
  fd_src = open(src, O_RDWR);
  if (fd_src < 0)
    {
      DPRINTF(E_LOG, L_SCAN, "Error opening '%s' for metadata update: %s\n", src, strerror(errno));
      goto error;
    }

  ret = snprintf(dst, dst_size, "/tmp/owntone.tmpXXXXXX%s", ext);
  if (ret < 0 || ret >= dst_size)
    {
      DPRINTF(E_LOG, L_SCAN, "Error creating tmp file name\n");
      goto error;
    }

  fd_dst = mkstemps(dst, strlen(ext));
  if (fd_dst < 0)
    {
      DPRINTF(E_LOG, L_SCAN, "Error creating tmp file '%s' for metadata update: %s\n", dst, strerror(errno));
      goto error;
    }

  ret = fast_copy(fd_dst, fd_src);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_SCAN, "Error copying '%s' to tmp file '%s': %s\n", src, dst, strerror(errno));
      goto error;
    }

  close(fd_src);
  close(fd_dst);
  return 0;

 error:
  if (fd_src != -1)
    close(fd_src);
  if (fd_dst != -1)
    close(fd_dst);
  return -1;
}

// based on FFmpeg's doc/examples and in particular mux.c
static int
file_write_rating(const char *dst, const char *src, const char *rating)
{
  AVFormatContext *in_fmt_ctx = NULL;
  AVFormatContext *out_fmt_ctx = NULL;
  AVPacket pkt;
  const AVDictionaryEntry *tag;
  AVStream *out_stream;
  AVStream *in_stream;
#if (LIBAVCODEC_VERSION_MAJOR > 59) || ((LIBAVCODEC_VERSION_MAJOR == 59) && (LIBAVCODEC_VERSION_MINOR >= 0) && (LIBAVCODEC_VERSION_MICRO >= 100))
  const AVOutputFormat *out_fmt;
#else
  AVOutputFormat *out_fmt;
#endif
  bool restore_src = false;
  int ret;
  int i;
  int stream_idx;
  int *stream_mapping = NULL;

  ret = avformat_open_input(&in_fmt_ctx, src, NULL, NULL);
  if (ret != 0)
    {
      DPRINTF(E_LOG, L_SCAN, "Error opening tmpfile '%s' for rating metadata update: %s\n", src, av_err2str(ret));
      goto error;
    }

  av_dict_set(&in_fmt_ctx->metadata, "rating", rating, 0);

  ret = avformat_find_stream_info(in_fmt_ctx, NULL);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_SCAN, "Error reading input stream information from '%s': %s\n", in_fmt_ctx->url, av_err2str(ret));
      goto error;
    }

  out_fmt = av_guess_format(in_fmt_ctx->iformat->name, in_fmt_ctx->url, in_fmt_ctx->iformat->mime_type);
  if (out_fmt == NULL)
    {
      DPRINTF(E_LOG, L_SCAN, "Could not determine output format from '%s'\n", in_fmt_ctx->url);
      goto error;
    }

  ret = avformat_alloc_output_context2(&out_fmt_ctx, out_fmt, NULL, NULL);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_SCAN, "Could not create output context '%s' - %s\n", in_fmt_ctx->url, av_err2str(ret));
      goto error;
    }

  CHECK_NULL(L_SCAN, stream_mapping = av_calloc(in_fmt_ctx->nb_streams, sizeof(*stream_mapping)));

  tag = NULL;
  while ((tag = av_dict_iterate(in_fmt_ctx->metadata, tag)))
    {
      av_dict_set(&(out_fmt_ctx->metadata), tag->key, tag->value, 0);
    }

  stream_idx = 0;
  for (i = 0; i < in_fmt_ctx->nb_streams; i++)
    {
      in_stream = in_fmt_ctx->streams[i];
      stream_mapping[i] = stream_idx++;

      out_stream = avformat_new_stream(out_fmt_ctx, NULL);
      if (!out_stream)
        {
	  DPRINTF(E_LOG, L_SCAN, "Error allocating output stream for '%s'\n", in_fmt_ctx->url);
	  goto error;
        }

      ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_SCAN, "Error copying codec parameters from '%s': %s\n", in_fmt_ctx->url, av_err2str(ret));
	  goto error;
	}

      if (in_stream->metadata)
	{
	  tag = NULL;
	  while ((tag = av_dict_iterate(in_stream->metadata, tag)))
	    {
	      av_dict_set(&(out_stream->metadata), tag->key, tag->value, 0);
	    }
	}
    }

  ret = avio_open(&out_fmt_ctx->pb, dst, AVIO_FLAG_WRITE);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_SCAN, "Could not open output rating file '%s': %s\n", dst, av_err2str(ret));
      goto error;
    }

  ret = avformat_write_header(out_fmt_ctx, NULL);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_SCAN, "Error occurred when writing output header to '%s': %s\n", dst, av_err2str(ret));
      goto error;
    }

  while (1)
    {
      ret = av_read_frame(in_fmt_ctx, &pkt);
      if (ret < 0)
	{
	  if (ret == AVERROR_EOF)
	    break;

	  DPRINTF(E_LOG, L_SCAN, "Error reading '%s': %s\n", in_fmt_ctx->url, av_err2str(ret));
	  restore_src = true;
	  goto error;
	}

      in_stream = in_fmt_ctx->streams[pkt.stream_index];
      if (pkt.stream_index >= in_fmt_ctx->nb_streams || stream_mapping[pkt.stream_index] < 0)
	{
	  av_packet_unref(&pkt);
	  continue;
	}

      pkt.stream_index = stream_mapping[pkt.stream_index];
      out_stream = out_fmt_ctx->streams[pkt.stream_index];

      /* copy packet */
      pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
      pkt.dts = av_rescale_q_rnd(pkt.dts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
      pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base, out_stream->time_base);
      pkt.pos = -1;

      ret = av_interleaved_write_frame(out_fmt_ctx, &pkt);
      av_packet_unref(&pkt);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_SCAN, "Error muxing pkt for rating '%s': %s\n", in_fmt_ctx->url, av_err2str(ret));
	  restore_src = true;
	  goto error;
	}
    }

  av_write_trailer(out_fmt_ctx);

  if (out_fmt_ctx && !(out_fmt_ctx->oformat->flags & AVFMT_NOFILE))
    avio_closep(&out_fmt_ctx->pb);
  avformat_free_context(out_fmt_ctx);
  av_freep(&stream_mapping);
  return 0;

 error:
  if (out_fmt_ctx && !(out_fmt_ctx->oformat->flags & AVFMT_NOFILE))
    avio_closep(&out_fmt_ctx->pb);
  avformat_free_context(out_fmt_ctx);
  av_freep(&stream_mapping);
  if (restore_src)
    file_copy(dst, src);
  return -1;
}

static bool
file_rating_matches(const char *path, const char *rating)
{
  AVFormatContext *in_fmt_ctx = NULL;
  AVDictionaryEntry *entry;
  bool has_rating;
  int ret;

  ret = avformat_open_input(&in_fmt_ctx, path, NULL, NULL);
  if (ret != 0)
    {
      DPRINTF(E_LOG, L_SCAN, "Failed to open library file for rating metadata update '%s' - %s\n", path, av_err2str(ret));
      return true; // Return true so called aborts
    }

  entry = av_dict_get(in_fmt_ctx->metadata, "rating", NULL, 0);
  has_rating = (entry && entry->value && strcmp(entry->value, rating) == 0);

  avformat_close_input(&in_fmt_ctx);

  return has_rating;
}

// ffmpeg's metadata update is limited - some formats do not support rating
// update even though the write completes; keep this in sync with supported
// formats
static bool
format_is_supported(const char *format)
{
  if (strcmp(format, "mp3") == 0)
    return true;
  if (strcmp(format, "flac") == 0)
    return true;

  return false;
}

int
write_metadata_ffmpeg(struct media_file_info *mfi)
{
  char rating_str[32];
  char tmpfile[PATH_MAX];
  int max_rating;
  int file_rating;
  int ret;

  if (mfi->data_kind != DATA_KIND_FILE || !format_is_supported(mfi->type))
    {
      DPRINTF(E_WARN, L_SCAN, "Update of rating metadata requires file in MP3 or FLAC format: '%s'\n", mfi->path);
      return -1;
    }

  max_rating = cfg_getint(cfg_getsec(cfg, "library"), "max_rating");
  if (max_rating < 5) // Invalid config
    max_rating = DB_FILES_RATING_MAX;
  file_rating = mfi->rating * max_rating / DB_FILES_RATING_MAX;
  snprintf(rating_str, sizeof(rating_str), "%d", file_rating);

  // Save a write if metadata of the underlying file matches requested rating
  if (file_rating_matches(mfi->path, rating_str))
    return 0;

  ret = file_copy_to_tmp(tmpfile, sizeof(tmpfile), mfi->path);
  if (ret < 0)
    return -1;

  ret = file_write_rating(mfi->path, tmpfile, rating_str);
  unlink(tmpfile);
  if (ret < 0)
    return -1;

  DPRINTF(E_DBG, L_SCAN, "Wrote rating metadata to '%s'\n", mfi->path);

  return 0;
}
