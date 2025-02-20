/*
 * Copyright (C) 2020 whatdoineed2d/Ray
 * based heavily on filescanner_playlist.c
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
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

// For strptime()
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE
#endif
#include <time.h>

#include <event2/buffer.h>

#include "conffile.h"
#include "logger.h"
#include "db.h"
#include "http.h"
#include "misc.h"
#include "misc_json.h"
#include "misc_xml.h"
#include "library.h"
#include "library/filescanner.h"

#define APPLE_PODCASTS_SERVER "https://podcasts.apple.com/"
#define APPLE_ITUNES_SERVER "https://itunes.apple.com/"
#define RSS_LIMIT_DEFAULT 10

enum rss_scan_type {
  RSS_SCAN_RESCAN,
  RSS_SCAN_META,
};

struct rss_item_info {
  const char *title;
  const char *pubdate;
  const char *link;
  const char *url;
  const char *type;
};

static struct timeval rss_refresh_interval = { 3600, 0 };

// Forward
static void
rss_refresh(void *arg);

// RSS spec: https://validator.w3.org/feed/docs/rss2.html
static void
rss_date(struct tm *tm, const char *date)
{
  // RFC822 https://tools.ietf.org/html/rfc822#section-5
  // ie Fri, 07 Feb 2020 18:58:00 +0000
  //    ^^^^                      ^^^^^
  //    optional                  ^^^^^
  //                              could also be GMT/UT/EST/A..I/M..Z

  const char *ptr = NULL;
  time_t t;

  memset(tm, 0, sizeof(struct tm));
  if (date)
    {
      ptr = strptime(date, "%a,%n", tm); // Looks for optional day of week
      if (!ptr)
	ptr = date;

      ptr = strptime(ptr, "%d%n%b%n%Y%n%H:%M:%S%n", tm);
    }

  if (!ptr)
    {
      // date is junk, using current time
      time(&t);
      gmtime_r(&t, tm);
    }

  // TODO - adjust the TZ?
}

// Makes a request to Apple based on the Apple Podcast ID in rss_url. The JSON
// response is parsed to find the original feed's url. Example rss_url:
// https://podcasts.apple.com/is/podcast/cgp-grey/id974722423
static char *
apple_rss_feedurl_get(const char *rss_url)
{
  struct http_client_ctx ctx;
  struct evbuffer *evbuf;
  char url[100];
  const char *ptr;
  unsigned podcast_id;
  json_object *jresponse;
  json_object *jfeedurl;
  char *feedurl;
  int ret;

  ptr = strrchr(rss_url, '/');
  if (!ptr)
    {
      DPRINTF(E_LOG, L_LIB, "Could not parse Apple Podcast RSS ID from '%s'\n", rss_url);
      return NULL;
    }

  ret = sscanf(ptr, "/id%u", &podcast_id);
  if (ret != 1)
    {
      DPRINTF(E_LOG, L_LIB, "Could not parse Apple Podcast RSS ID from '%s'\n", rss_url);
      return NULL;
    }

  CHECK_NULL(L_LIB, evbuf = evbuffer_new());
  snprintf(url, sizeof(url), "%slookup?id=%u", APPLE_ITUNES_SERVER, podcast_id);

  memset(&ctx, 0, sizeof(struct http_client_ctx));
  ctx.url = url;
  ctx.input_body = evbuf;

  ret = http_client_request(&ctx, NULL);
  if (ret < 0 || ctx.response_code != HTTP_OK)
    {
      evbuffer_free(evbuf);
      return NULL;
    }

  jresponse = jparse_obj_from_evbuffer(evbuf);
  evbuffer_free(evbuf);
  if (!jresponse)
    {
      DPRINTF(E_LOG, L_LIB, "Could not parse RSS Apple response, podcast id %u\n", podcast_id);
      return NULL;
    }

  /* expect json resp - get feedUrl
   * {
   *  "resultCount": 1,
   *  "results": [
   *    {
   *      "wrapperType": "track",
   *      "kind": "podcast",
   *      ...
   *      "collectionViewUrl": "https://podcasts.apple.com/us/podcast/cgp-grey/id974722423?uo=4",
   *      "feedUrl": "http://cgpgrey.libsyn.com/rss",
   *      ...
   *      "genres": [
   *        "Education",
   *        "Podcasts",
   *        "News"
   *      ]
   *    }
   *  ]
   *}
   */
  jfeedurl = JPARSE_SELECT(jresponse, "results", "feedUrl");
  if (!jfeedurl || json_object_get_type(jfeedurl) != json_type_string)
    {
      DPRINTF(E_LOG, L_LIB, "Could not find RSS feedUrl in response from Apple, podcast id %u\n", podcast_id);
      jparse_free(jresponse);
      return NULL;
    }

  feedurl = safe_strdup(json_object_get_string(jfeedurl));

  DPRINTF(E_DBG, L_LIB, "Mapped Apple podcast URL: '%s' -> '%s'\n", rss_url, feedurl);

  jparse_free(jresponse);
  return feedurl;
}

static struct playlist_info *
playlist_fetch(bool *is_new, const char *path)
{
  struct playlist_info *pli;
  int ret;

  pli = db_pl_fetch_bypath(path);
  if (pli)
    {
      *is_new = false;
      return pli;
    }

  CHECK_NULL(L_SCAN, pli = calloc(1, sizeof(struct playlist_info)));

  ret = playlist_fill(pli, path);
  if (ret < 0)
    goto error;

  pli->directory_id = DIR_HTTP;
  pli->type = PL_RSS;
  pli->query_limit = RSS_LIMIT_DEFAULT;
  pli->scan_kind = SCAN_KIND_RSS;

  ret = library_playlist_save(pli);
  if (ret < 0)
    goto error;

  pli->id = ret;
  *is_new = true;
  return pli;

 error:
  DPRINTF(E_LOG, L_SCAN, "Error adding playlist for RSS feed '%s'\n", path);
  free_pli(pli, 0);
  return NULL;
}

static xml_node *
rss_xml_get(const char *url)
{
  struct http_client_ctx ctx = { 0 };
  const char *raw = NULL;
  xml_node *xml = NULL;
  char *feedurl;
  int ret;

  // Is it an apple podcast stream?
  // ie https://podcasts.apple.com/is/podcast/cgp-grey/id974722423
  if (strncmp(url, APPLE_PODCASTS_SERVER, strlen(APPLE_PODCASTS_SERVER)) == 0)
    {
      feedurl = apple_rss_feedurl_get(url);
      if (!feedurl)
	return NULL;
    }
  else
    feedurl = strdup(url);

  CHECK_NULL(L_LIB, ctx.input_body = evbuffer_new());
  ctx.url = feedurl;

  ret = http_client_request(&ctx, NULL);
  if (ret < 0 || ctx.response_code != HTTP_OK)
    {
      DPRINTF(E_LOG, L_LIB, "Failed to fetch RSS from '%s' (return %d, error code %d)\n", ctx.url, ret, ctx.response_code);
      goto cleanup;
    }

  evbuffer_add(ctx.input_body, "", 1);

  raw = (const char*)evbuffer_pullup(ctx.input_body, -1);

  xml = xml_from_string(raw);
  if (!xml)
    {
      DPRINTF(E_LOG, L_LIB, "Failed to parse RSS XML from '%s'\n", ctx.url);
      goto cleanup;
    }

 cleanup:
  evbuffer_free(ctx.input_body);
  free(feedurl);
  return xml;
}

static int
feed_metadata_from_xml(const char **feed_title, const char **feed_author, const char **feed_artwork, xml_node *xml)
{
  xml_node *channel = xml_get_node(xml, "rss/channel");
  if (!channel)
    {
      DPRINTF(E_LOG, L_LIB, "Invalid RSS/xml, missing 'channel' node\n");
      return -1;
    }

  *feed_title = xml_get_val(channel, "title");
  if (!*feed_title)
    {
      DPRINTF(E_LOG, L_LIB, "Invalid RSS/xml, missing 'title' node\n");
      return -1;
    }

  *feed_author = xml_get_val(channel, "itunes:author");
  *feed_artwork = xml_get_val(channel, "image/url");

  return 0;
}

static void
ri_from_item(struct rss_item_info *ri, xml_node *item)
{
  memset(ri, 0, sizeof(struct rss_item_info));

  ri->title   = xml_get_val(item, "title");
  ri->pubdate = xml_get_val(item, "pubDate");
  ri->link    = xml_get_val(item, "link");

  ri->url     = xml_get_attr(item, "enclosure", "url");
  ri->type    = xml_get_attr(item, "enclosure", "type");
}

// The RSS spec states:
//    Elements of <item>
//    .... All elements of an item are optional, however at least one of title or description must be present
static void
mfi_metadata_fixup(struct media_file_info *mfi, struct rss_item_info *ri, const char *feed_title, const char *feed_author, uint32_t time_added)
{
  struct tm tm;

  // Always take the artist and album from the RSS feed and not the stream
  free(mfi->artist);
  mfi->artist = safe_strdup(feed_author);
  free(mfi->album);
  mfi->album  = safe_strdup(feed_title);

  // Some podcasts (Apple) can use mp4 streams which tend not to have decent tags so
  // in those cases take info from the RSS and not the stream
  if (!mfi->url)
    mfi->url    = safe_strdup(ri->link);

  if (mfi->genre && strcmp("(186)Podcast", mfi->genre) == 0)
    {
      free(mfi->genre);
      mfi->genre  = strdup("Podcast");
    }

  // The title from the xml is usually better quality
  if (ri->title)
    {
      free(mfi->title);
      mfi->title = strdup(ri->title);
    }

  // Remove, some can be very verbose
  free(mfi->comment);
  mfi->comment = NULL;

  // Date is always from the RSS feed info
  rss_date(&tm, ri->pubdate);
  mfi->date_released = mktime(&tm);
  mfi->year = 1900 + tm.tm_year;

  mfi->media_kind = MEDIA_KIND_PODCAST;

  mfi->time_added = time_added;
}

static int
rss_save(struct playlist_info *pli, int *count, enum rss_scan_type scan_type)
{
  xml_node *xml;
  xml_node *item;
  const char *feed_title;
  const char *feed_author;
  const char *feed_artwork;
  struct media_file_info mfi = { 0 };
  struct rss_item_info ri;
  uint32_t time_added;
  int ret;

  xml = rss_xml_get(pli->path);
  if (!xml)
    {
      DPRINTF(E_LOG, L_LIB, "Could not get RSS/xml from '%s' (id %d)\n", pli->path, pli->id);
      return -1;
    }

  ret = feed_metadata_from_xml(&feed_title, &feed_author, &feed_artwork, xml);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_LIB, "Invalid RSS/xml received from '%s' (id %d)\n", pli->path, pli->id);
      xml_free(xml);
      return -1;
    }

  free(pli->title);
  pli->title = safe_strdup(feed_title);

  free(pli->artwork_url);
  pli->artwork_url = safe_strdup(feed_artwork);

  free(pli->virtual_path);
  pli->virtual_path = safe_asprintf("/%s", pli->path);

  // Fake the time - useful when we are adding a new stream - since the
  // newest podcasts are added first (the stream is most recent first)
  // having time_added date which is older on the most recent episodes
  // makes no sense so make all the dates the same for a singleu update
  time_added = (uint32_t)time(NULL);

  // Walk through the xml, saving each item
  *count = 0;
  db_transaction_begin();
  db_pl_clear_items(pli->id);
  for (item = xml_get_node(xml, "rss/channel/item"); item && (*count < pli->query_limit); item = xml_get_next(xml, item))
    {
      if (library_is_exiting())
	{
	  db_transaction_rollback();
	  xml_free(xml);
	  return -1;
	}

      ri_from_item(&ri, item);
      if (!ri.url)
	{
	  DPRINTF(E_WARN, L_LIB, "Missing URL for item '%s' (date %s) in RSS feed '%s'\n", ri.title, ri.pubdate, feed_title);
	  continue;
	}

      DPRINTF(E_DBG, L_LIB, "RSS/xml item: title '%s' pubdate: '%s' link: '%s' url: '%s' type: '%s'\n", ri.title, ri.pubdate, ri.link, ri.url, ri.type);

      db_pl_add_item_bypath(pli->id, ri.url);
      (*count)++;

      // Try to just ping if already in library
      if (scan_type == RSS_SCAN_RESCAN)
	{
	  ret = db_file_ping_bypath(ri.url, 0);
	  if (ret > 0)
	    continue;
	}
      else if (scan_type == RSS_SCAN_META)
	{
	  // Using existing file id if already in library, resulting in update but preserving play_count etc
	  mfi.id = db_file_id_bypath(ri.url);
	  if (mfi.id > 0)
	    time_added = 0;
	}

      scan_metadata_stream(&mfi, ri.url);
      mfi.scan_kind = SCAN_KIND_RSS;

      mfi_metadata_fixup(&mfi, &ri, feed_title, feed_author, time_added);

      library_media_save(&mfi, NULL);

      free_mfi(&mfi, 1);
    }

  db_transaction_end();
  xml_free(xml);

  return 0;
}

static int
rss_scan(const char *path, enum rss_scan_type scan_type)
{
  struct playlist_info *pli;
  bool pl_is_new;
  int count;
  int ret;

  // Fetches or creates playlist
  pli = playlist_fetch(&pl_is_new, path);
  if (!pli)
    return -1;

  // Retrieves the RSS and reads the feed, saving each item as a track, and also
  // adds the relationship to playlistitems. The pli will also be updated with
  // metadata from the RSS.
  //
  // playlistitems are only cleared if we are ready to add entries
  ret = rss_save(pli, &count, scan_type);
  if (ret < 0)
    goto error;

  // Save the playlist again, title etc may have been modified by rss_save().
  // This also updates the db_timestamp which protects the RSS from deletion.
  ret = library_playlist_save(pli);
  if (ret < 0)
    goto error;

  DPRINTF(E_INFO, L_SCAN, "Added or updated %d items from RSS feed '%s' (id %d)\n", count, path, pli->id);

  free_pli(pli, 0);
  return 0;

 error:
  if (pl_is_new)
    db_pl_delete(pli->id);
  free_pli(pli, 0);
  return -1;
}

static void
rss_scan_all(enum rss_scan_type scan_type)
{
  struct query_params qp = { 0 };
  struct db_playlist_info dbpli;
  time_t start;
  time_t end;
  int count;
  int ret;

  DPRINTF(E_DBG, L_LIB, "Refreshing RSS feeds\n");

  start = time(NULL);

  qp.type = Q_PL;
  qp.sort = S_PLAYLIST;
  qp.filter = db_mprintf("(f.type = %d)", PL_RSS);

  ret = db_query_start(&qp);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_LIB, "Failed to find current RSS feeds from db\n");
      free(qp.filter);
      return;
    }

  count = 0;
  while (((ret = db_query_fetch_pl(&dbpli, &qp)) == 0) && (dbpli.path))
    {
      ret = rss_scan(dbpli.path, scan_type);
      if (ret == 0)
	count++;
    }

  db_query_end(&qp);
  free(qp.filter);

  end = time(NULL);

  if (count == 0)
    return;

  library_callback_schedule(rss_refresh, NULL, &rss_refresh_interval, LIBRARY_CB_ADD_OR_REPLACE);

  DPRINTF(E_INFO, L_LIB, "Refreshed %d RSS feeds in %.f sec (scan type %d)\n", count, difftime(end, start), scan_type);
}

static void
rss_refresh(void *arg)
{
  rss_scan_all(RSS_SCAN_RESCAN);
}

static int
rss_rescan(void)
{
  rss_scan_all(RSS_SCAN_RESCAN);

  return LIBRARY_OK;
}

static int
rss_metascan(void)
{
  rss_scan_all(RSS_SCAN_META);

  return LIBRARY_OK;
}

static int
rss_fullscan(void)
{
  DPRINTF(E_LOG, L_LIB, "RSS feeds removed during full-rescan\n");
  return LIBRARY_OK;
}

static int
rss_add(const char *path)
{
  int ret;

  if (!net_is_http_or_https(path))
    {
      DPRINTF(E_SPAM, L_LIB, "Invalid RSS path '%s'\n", path);
      return LIBRARY_PATH_INVALID;
    }

  DPRINTF(E_DBG, L_LIB, "Adding RSS '%s'\n", path);

  ret = rss_scan(path, RSS_SCAN_RESCAN);
  if (ret < 0)
    return LIBRARY_PATH_INVALID;

  library_callback_schedule(rss_refresh, NULL, &rss_refresh_interval, LIBRARY_CB_ADD_OR_REPLACE);

  return LIBRARY_OK;
}

struct library_source rssscanner =
{
  .scan_kind = SCAN_KIND_RSS,
  .disabled = 0,
  .initscan = rss_rescan,
  .rescan = rss_rescan,
  .metarescan = rss_metascan,
  .fullrescan = rss_fullscan,
  .item_add = rss_add,
};
