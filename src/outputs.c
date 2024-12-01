/*
 * Copyright (C) 2016 Espen Jürgensen <espenjurgensen@gmail.com>
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
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <inttypes.h>

#include <event2/event.h>

#include "logger.h"
#include "misc.h"
#include "transcode.h"
#include "db.h"
#include "player.h" //TODO remove me when player_pmap is removed again
#include "worker.h"
#include "outputs.h"

extern struct output_definition output_raop;
extern struct output_definition output_airplay;
extern struct output_definition output_streaming;
extern struct output_definition output_dummy;
extern struct output_definition output_fifo;
extern struct output_definition output_rcp;
#ifdef HAVE_ALSA
extern struct output_definition output_alsa;
#endif
#ifdef HAVE_LIBPULSE
extern struct output_definition output_pulse;
#endif
#ifdef CHROMECAST
extern struct output_definition output_cast;
#endif

/* From player.c */
extern struct event_base *evbase_player;

// Must be in sync with enum output_types
static struct output_definition *outputs[] = {
    &output_raop,
    &output_airplay,
    &output_streaming,
    &output_dummy,
    &output_fifo,
    &output_rcp,
#ifdef HAVE_ALSA
    &output_alsa,
#endif
#ifdef HAVE_LIBPULSE
    &output_pulse,
#endif
#ifdef CHROMECAST
    &output_cast,
#endif
    NULL
};

// Default volume (must be from 0 - 100)
#define OUTPUTS_DEFAULT_VOLUME 50

// When we stop, we keep the outputs open for a while, just in case we are
// actually just restarting. This timeout determines how long we wait before
// full stop.
// (value is in seconds)
#define OUTPUTS_STOP_TIMEOUT 10

#define OUTPUTS_MAX_CALLBACKS 64

struct outputs_callback_register
{
  output_status_cb cb;
  struct output_device *device;

  // We have received the callback with the result from the backend
  bool ready;

  // We store a device_id to avoid the risk of dangling device pointer
  uint64_t device_id;
  enum output_device_state state;
};

struct output_quality_subscription
{
  int count;
  struct media_quality quality;
  struct encode_ctx *encode_ctx;
};

// Buffer used to pass data to the backends
static struct output_buffer output_buffer;

static struct output_device *outputs_device_list;
static int outputs_master_volume;

static struct outputs_callback_register outputs_cb_register[OUTPUTS_MAX_CALLBACKS];
static struct event *outputs_deferredev;
static struct timeval outputs_stop_timeout = { OUTPUTS_STOP_TIMEOUT, 0 };

// Last element is a zero terminator
static struct output_quality_subscription output_quality_subscriptions[OUTPUTS_MAX_QUALITY_SUBSCRIPTIONS + 1];
static bool outputs_got_new_subscription;


/* ------------------------------- MISC HELPERS ----------------------------- */

static output_status_cb
callback_get(struct output_device *device)
{
  int callback_id;

  for (callback_id = 0; callback_id < ARRAY_SIZE(outputs_cb_register); callback_id++)
    {
      if (outputs_cb_register[callback_id].device == device)
	return outputs_cb_register[callback_id].cb;
    }

  return NULL;
}

static void
callback_remove(struct output_device *device)
{
  int callback_id;

  if (!device)
    return;

  for (callback_id = 0; callback_id < ARRAY_SIZE(outputs_cb_register); callback_id++)
    {
      if (outputs_cb_register[callback_id].device == device)
	{
	  DPRINTF(E_DBG, L_PLAYER, "Removing callback to %s, id %d\n", player_pmap(outputs_cb_register[callback_id].cb), callback_id);
	  memset(&outputs_cb_register[callback_id], 0, sizeof(struct outputs_callback_register));
	}
    }
}

static int
callback_add(struct output_device *device, output_status_cb cb)
{
  int callback_id;

  if (!cb)
    return -1;

  // We will replace any previously registered callbacks, since that's what the
  // player expects
  callback_remove(device);

  // Find a free slot in the queue
  for (callback_id = 0; callback_id < ARRAY_SIZE(outputs_cb_register); callback_id++)
    {
      if (outputs_cb_register[callback_id].cb == NULL)
	break;
    }

  if (callback_id == ARRAY_SIZE(outputs_cb_register))
    {
      DPRINTF(E_LOG, L_PLAYER, "Output callback queue is full! (size is %d)\n", OUTPUTS_MAX_CALLBACKS);
      return -1;
    }

  outputs_cb_register[callback_id].cb = cb;
  outputs_cb_register[callback_id].device = device; // Don't dereference this later, it might become invalid!

  DPRINTF(E_DBG, L_PLAYER, "Registered callback to %s with id %d (device %p, %s)\n", player_pmap(cb), callback_id, device, device->name);

  int active = 0;
  for (int i = 0; i < ARRAY_SIZE(outputs_cb_register); i++)
    if (outputs_cb_register[i].cb)
      active++;

  DPRINTF(E_DBG, L_PLAYER, "Number of active callbacks: %d\n", active);

  return callback_id;
};

static void
deferred_cb(int fd, short what, void *arg)
{
  struct output_device *device;
  output_status_cb cb;
  enum output_device_state state;
  int callback_id;

  for (callback_id = 0; callback_id < ARRAY_SIZE(outputs_cb_register); callback_id++)
    {
      if (outputs_cb_register[callback_id].ready)
	{
	  // Must copy before making callback, since you never know what the
	  // callback might result in (could call back in)
	  cb = outputs_cb_register[callback_id].cb;
	  state = outputs_cb_register[callback_id].state;

	  // Will be NULL if the device has disappeared
	  device = outputs_device_get(outputs_cb_register[callback_id].device_id);

	  memset(&outputs_cb_register[callback_id], 0, sizeof(struct outputs_callback_register));

	  // The device has left the building (stopped/failed), and the backend
	  // is not using it any more
	  if (device && !device->advertised && !device->session)
	    {
	      outputs_device_remove(device);
	      device = NULL;
	    }
	  else if (device)
	    device->state = state;

	  DPRINTF(E_DBG, L_PLAYER, "Making deferred callback to %s, id was %d\n", player_pmap(cb), callback_id);

	  cb(device, state);
	}
    }

  for (int i = 0; i < ARRAY_SIZE(outputs_cb_register); i++)
    {
      if (outputs_cb_register[i].cb)
	DPRINTF(E_DBG, L_PLAYER, "%d. Active callback: %s\n", i, player_pmap(outputs_cb_register[i].cb));
    }
}

static void
stop_timer_cb(int fd, short what, void *arg)
{
  struct output_device *device = arg;
  output_status_cb cb = callback_get(device);

  outputs_device_stop(device, cb);
}

static void
device_stop_cb(struct output_device *device, enum output_device_state state)
{
  if (device)
    device->state = state;

  if (state == OUTPUT_STATE_FAILED)
    DPRINTF(E_WARN, L_PLAYER, "Failed to stop device\n");
  else
    DPRINTF(E_INFO, L_PLAYER, "Device stopped properly\n");
}

static enum transcode_profile
quality_to_xcode(struct media_quality *quality)
{
  if (quality->bits_per_sample == 16)
    return XCODE_PCM16;
  if (quality->bits_per_sample == 24)
    return XCODE_PCM24;
  if (quality->bits_per_sample == 32)
    return XCODE_PCM32;

  return XCODE_UNKNOWN;
}

static int
encoding_reset(struct media_quality *quality)
{
  struct transcode_encode_setup_args encode_args = { 0 };
  struct output_quality_subscription *subscription;
  enum transcode_profile profile;
  int i;

  profile = quality_to_xcode(quality);
  if  (profile == XCODE_UNKNOWN)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not create subscription decoding context, invalid quality (%d/%d/%d)\n",
	quality->sample_rate, quality->bits_per_sample, quality->channels);
      return -1;
    }

  encode_args.src_ctx = transcode_decode_setup_raw(profile, quality);
  if (!encode_args.src_ctx)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not create subscription decoding context (profile %d)\n", profile);
      return -1;
    }

  for (i = 0; output_quality_subscriptions[i].count > 0; i++)
    {
      subscription = &output_quality_subscriptions[i]; // Just for short-hand

      transcode_encode_cleanup(&subscription->encode_ctx); // Will also point the ctx to NULL

      if (quality_is_equal(quality, &subscription->quality))
	continue; // No resampling required

      encode_args.profile = quality_to_xcode(&subscription->quality);
      encode_args.quality = &subscription->quality;
      if (encode_args.profile != XCODE_UNKNOWN)
	subscription->encode_ctx = transcode_encode_setup(encode_args);
      else
	DPRINTF(E_LOG, L_PLAYER, "Could not setup resampling to %d/%d/%d for output\n",
	  subscription->quality.sample_rate, subscription->quality.bits_per_sample, subscription->quality.channels);
    }

  transcode_decode_cleanup(&encode_args.src_ctx);

  return 0;
}

static void
buffer_fill(struct output_buffer *obuf, void *buf, size_t bufsize, struct media_quality *quality, int nsamples, struct timespec *pts)
{
  transcode_frame *frame;
  int ret;
  int i;
  int n;

  obuf->pts = *pts;

  // The resampling/encoding (transcode) contexts work for a given input quality,
  // so if the quality changes we need to reset the contexts. We also do that if
  // we have received a subscription for a new quality.
  if (!quality_is_equal(quality, &obuf->data[0].quality) || outputs_got_new_subscription)
    {
      encoding_reset(quality);
      outputs_got_new_subscription = false;
    }

  // The first element of the output_buffer is always just the raw input data
  // TODO can we avoid the copy below? we can't use evbuffer_add_buffer_reference,
  // because then the outputs can't use it and we would need to copy there instead
  evbuffer_add(obuf->data[0].evbuf, buf, bufsize);
  obuf->data[0].buffer = buf;
  obuf->data[0].bufsize = bufsize;
  obuf->data[0].quality = *quality;
  obuf->data[0].samples = nsamples;

  for (i = 0, n = 1; output_quality_subscriptions[i].count > 0; i++)
    {
      if (quality_is_equal(&output_quality_subscriptions[i].quality, quality))
	continue; // Skip, no resampling required and we have the data in element 0

      if (!output_quality_subscriptions[i].encode_ctx)
	continue;

      frame = transcode_frame_new(buf, bufsize, nsamples, quality);
      if (!frame)
	continue;

      ret = transcode_encode(obuf->data[n].evbuf, output_quality_subscriptions[i].encode_ctx, frame, 0);
      transcode_frame_free(frame);
      if (ret < 0)
	continue;

      obuf->data[n].buffer  = evbuffer_pullup(obuf->data[n].evbuf, -1);
      obuf->data[n].bufsize = evbuffer_get_length(obuf->data[n].evbuf);
      obuf->data[n].quality = output_quality_subscriptions[i].quality;
      obuf->data[n].samples = BTOS(obuf->data[n].bufsize, obuf->data[n].quality.bits_per_sample, obuf->data[n].quality.channels);
      n++;
    }
}

static void
buffer_drain(struct output_buffer *obuf)
{
  int i;

  for (i = 0; obuf->data[i].buffer; i++)
    {
      evbuffer_drain(obuf->data[i].evbuf, obuf->data[i].bufsize);
      obuf->data[i].buffer  = NULL;
      obuf->data[i].bufsize = 0;
      // We don't reset quality and samples, would be a waste of time
    }
}

static struct output_buffer *
buffer_copy(struct output_buffer *obuf)
{
  struct output_buffer *copy;
  int i;

  if (!obuf)
    return NULL;

  CHECK_NULL(L_PLAYER, copy = malloc(sizeof(struct output_buffer)));

  memcpy(copy, obuf, sizeof(struct output_buffer));

  for (i = 0; obuf->data[i].buffer; i++)
    {
      CHECK_NULL(L_PLAYER, copy->data[i].evbuf = evbuffer_new());
      evbuffer_add(copy->data[i].evbuf, obuf->data[i].buffer, obuf->data[i].bufsize);
      copy->data[i].buffer = evbuffer_pullup(copy->data[i].evbuf, -1);
    }

  return copy;
}

static void
buffer_free(struct output_buffer *obuf)
{
  int i;

  if (!obuf)
    return;

  for (i = 0; obuf->data[i].buffer; i++)
    evbuffer_free(obuf->data[i].evbuf);

  free(obuf);
}

static void
device_list_sort(void)
{
  struct output_device *device;
  struct output_device *next;
  struct output_device *prev;
  int swaps;

  // Swap sorting since even the most inefficient sorting should do fine here
  do
    {
      swaps = 0;
      prev = NULL;
      for (device = outputs_device_list; device && device->next; device = device->next)
	{
	  next = device->next;
	  if ( (outputs_priority(device) > outputs_priority(next)) ||
	       (outputs_priority(device) == outputs_priority(next) && strcasecmp(device->name, next->name) > 0) )
	    {
	      if (device == outputs_device_list)
		outputs_device_list = next;
	      if (prev)
		prev->next = next;

	      device->next = next->next;
	      next->next = device;
	      swaps++;
	    }
	  prev = device;
	}
    }
  while (swaps > 0);
}

// Convenience function
static inline int
device_state_update(struct output_device *device, int ret)
{
  if (ret < 0)
    device->state = OUTPUT_STATE_FAILED;

  return ret;
}

static void
metadata_cb_send(int fd, short what, void *arg)
{
  struct output_metadata *metadata = arg;
  int ret;

  event_free(metadata->ev);
  metadata->ev = NULL;

  ret = metadata->finalize_cb(metadata);
  if (ret < 0)
    return;

  outputs[metadata->type]->metadata_send(metadata);
}

// *** Worker thread ***
static void
metadata_cb_prepare(void *arg)
{
  struct output_metadata *metadata = *((struct output_metadata **)arg);

  metadata->priv = outputs[metadata->type]->metadata_prepare(metadata);
  if (!metadata->priv)
    {
      event_free(metadata->ev);
      free(metadata);
      return;
    }

  // Metadata is prepared, let the player thread do the actual sending
  event_active(metadata->ev, 0, 0);
}

static void
metadata_free(struct output_metadata *metadata)
{
  if (!metadata)
    return;
  if (metadata->ev)
    event_free(metadata->ev);
  free(metadata);
}

static void
metadata_send(enum output_types type, uint32_t item_id, bool startup, output_metadata_finalize_cb cb)
{
  struct output_metadata *metadata;

  CHECK_NULL(L_PLAYER, metadata = calloc(1, sizeof(struct output_metadata)));

  metadata->type        = type;
  metadata->item_id     = item_id;
  metadata->startup     = startup;
  metadata->finalize_cb = cb;

  metadata->ev = event_new(evbase_player, -1, 0, metadata_cb_send, metadata);

  if (outputs[type]->metadata_prepare)
    worker_execute(metadata_cb_prepare, &metadata, sizeof(struct output_metadata *), 0);
  else
    outputs[type]->metadata_send(metadata);
}


/* ----------------------------- Volume helpers ----------------------------- */

static int
rel_to_vol(int relvol, int master_volume)
{
  float vol;

  if (relvol == 100)
    return master_volume;

  vol = ((float)relvol * (float)master_volume) / 100.0;

  return (int)vol;
}

static int
vol_to_rel(int volume, int master_volume)
{
  float rel;

  if (volume == master_volume)
    return 100;

  rel = ((float)volume / (float)master_volume) * 100.0;

  return (int)rel;
}

static void
vol_adjust(void)
{
  struct output_device *device;
  int selected_highest = -1;
  int all_highest = -1;

  for (device = outputs_device_list; device; device = device->next)
    {
      if (OUTPUTS_DEVICE_DISPLAY_SELECTED(device) && (device->volume > selected_highest))
	selected_highest = device->volume;

      if (device->volume > all_highest)
	all_highest = device->volume;
    }

  outputs_master_volume = (selected_highest >= 0) ? selected_highest : all_highest;

  for (device = outputs_device_list; device; device = device->next)
    {
      device->relvol = vol_to_rel(device->volume, outputs_master_volume);
    }

#ifdef DEBUG_VOLUME
  DPRINTF(E_DBG, L_PLAYER, "*** Master: %d\n", outputs_master_volume);

  for (device = outputs_device_list; device; device = device->next)
    {
      DPRINTF(E_DBG, L_PLAYER, "*** %s: abs %d rel %d selected %d\n", device->name, device->volume, device->relvol, OUTPUTS_DEVICE_DISPLAY_SELECTED(device));
    }
#endif
}

/* ----------------------------------- API ---------------------------------- */

struct output_device *
outputs_device_get(uint64_t device_id)
{
  struct output_device *device;

  for (device = outputs_device_list; device; device = device->next)
    {
      if (device_id == device->id)
	return device;
    }

  DPRINTF(E_WARN, L_PLAYER, "Output device with id %" PRIu64 " has disappeared from our list\n", device_id);
  return NULL;
}

/* ----------------------- Called by backend modules ------------------------ */

// Sessions free their sessions themselves, but should not touch the device,
// since they can't know for sure that it is still valid in memory
int
outputs_device_session_add(uint64_t device_id, void *session)
{
  struct output_device *device;

  device = outputs_device_get(device_id);
  if (!device)
    return -1;

  device->session = session;
  return 0;
}

void
outputs_device_session_remove(uint64_t device_id)
{
  struct output_device *device;

  device = outputs_device_get(device_id);
  if (device)
    device->session = NULL;

  return;
}

int
outputs_quality_subscribe(struct media_quality *quality)
{
  int i;

  // If someone else is already subscribing to this quality we just increase the
  // reference count.
  for (i = 0; output_quality_subscriptions[i].count > 0; i++)
    {
      if (!quality_is_equal(quality, &output_quality_subscriptions[i].quality))
	continue;

      output_quality_subscriptions[i].count++;

      DPRINTF(E_DBG, L_PLAYER, "Subscription request for quality %d/%d/%d (now %d subscribers)\n",
	quality->sample_rate, quality->bits_per_sample, quality->channels, output_quality_subscriptions[i].count);

      return 0;
    }

  if (i >= (ARRAY_SIZE(output_quality_subscriptions) - 1))
    {
      DPRINTF(E_LOG, L_PLAYER, "Bug! The number of different quality levels requested by outputs is too high\n");
      return -1;
    }

  output_quality_subscriptions[i].quality = *quality;
  output_quality_subscriptions[i].count++;

  DPRINTF(E_DBG, L_PLAYER, "Subscription request for quality %d/%d/%d (now %d subscribers)\n",
    quality->sample_rate, quality->bits_per_sample, quality->channels, output_quality_subscriptions[i].count);

  // Better way of signaling this?
  outputs_got_new_subscription = true;

  return 0;
}

void
outputs_quality_unsubscribe(struct media_quality *quality)
{
  int i;

  // Find subscription
  for (i = 0; output_quality_subscriptions[i].count > 0; i++)
    {
      if (quality_is_equal(quality, &output_quality_subscriptions[i].quality))
	break;
    }

  if (output_quality_subscriptions[i].count == 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Bug! Unsubscription request for a quality level that there is no subscription for\n");
      return;
    }

  output_quality_subscriptions[i].count--;

  DPRINTF(E_DBG, L_PLAYER, "Unsubscription request for quality %d/%d/%d (now %d subscribers)\n",
    quality->sample_rate, quality->bits_per_sample, quality->channels, output_quality_subscriptions[i].count);

  if (output_quality_subscriptions[i].count > 0)
    return;

  transcode_encode_cleanup(&output_quality_subscriptions[i].encode_ctx);

  // Shift elements
  for (; i < ARRAY_SIZE(output_quality_subscriptions) - 1; i++)
    output_quality_subscriptions[i] = output_quality_subscriptions[i + 1];
}

// Output backends call back through the below wrapper to make sure that:
// 1. Callbacks are always deferred
// 2. The callback never has a dangling pointer to a device (a device that has been removed from our list)
void
outputs_cb(int callback_id, uint64_t device_id, enum output_device_state state)
{
  if (callback_id < 0)
    return;

  if (!(callback_id < ARRAY_SIZE(outputs_cb_register)) || !outputs_cb_register[callback_id].cb)
    {
      DPRINTF(E_LOG, L_PLAYER, "Bug! Output backend called us with an illegal callback id (%d)\n", callback_id);
      return;
    }

  DPRINTF(E_DBG, L_PLAYER, "Callback request received, id is %i\n", callback_id);

  outputs_cb_register[callback_id].ready = true;
  outputs_cb_register[callback_id].device_id = device_id;
  outputs_cb_register[callback_id].state = state;
  event_active(outputs_deferredev, 0, 0);
}

void
outputs_metadata_free(struct output_metadata *metadata)
{
  metadata_free(metadata);
}

struct output_buffer *
outputs_buffer_copy(struct output_buffer *buffer)
{
  return buffer_copy(buffer);
}

void
outputs_buffer_free(struct output_buffer *buffer)
{
  buffer_free(buffer);
}

/* ---------------------------- Called by player ---------------------------- */

struct output_device *
outputs_device_add(struct output_device *add, bool new_deselect)
{
  struct output_device *device;
  char *keep_name;
  int ret;

  for (device = outputs_device_list; device; device = device->next)
    {
      if (device->id == add->id)
	break;
    }

  // This is relevant for Airplay 1 and 2 where the same device can support both
  if (device && device->type != add->type)
    {
      if (outputs_priority(device) < outputs_priority(add))
	{
	  DPRINTF(E_DBG, L_PLAYER, "Ignoring type %s for device '%s', will use type %s\n", add->type_name, add->name, device->type_name);
	  outputs_device_free(add);
	  return NULL;
	}

      // Remove existing device, higher priority device will be added below
      outputs_device_remove(device);
      device = NULL;
    }

  // New device
  if (!device)
    {
      device = add;

      device->stop_timer = evtimer_new(evbase_player, stop_timer_cb, device);

      keep_name = strdup(device->name);
      ret = db_speaker_get(device, device->id);
      if (ret < 0)
	{
	  device->selected = 0;
	  device->volume = (outputs_master_volume >= 0) ? outputs_master_volume : OUTPUTS_DEFAULT_VOLUME;;
	}

      free(device->name);
      device->name = keep_name;

      if (new_deselect)
	device->selected = 0;

      device->next = outputs_device_list;
      outputs_device_list = device;
    }
  // Update to a device already in the list
  else
    {
      if (add->v4_address)
	{
	  free(device->v4_address);

	  device->v4_address = add->v4_address;
	  device->v4_port = add->v4_port;

	  // Address is ours now
	  add->v4_address = NULL;
	}

      if (add->v6_address)
	{
	  free(device->v6_address);

	  device->v6_address = add->v6_address;
	  device->v6_port = add->v6_port;

	  // Address is ours now
	  add->v6_address = NULL;
	}

      free(device->name);
      device->name = add->name;
      add->name = NULL;

      device->has_password = add->has_password;
      device->password = add->password;

      outputs_device_free(add);
    }

  device_list_sort();

  vol_adjust();

  device->advertised = 1;

  return device;
}

void
outputs_device_remove(struct output_device *remove)
{
  struct output_device *device;
  struct output_device *prev;
  int ret;

  // Device stop should be able to handle that we invalidate the device, even
  // if it is an async stop. It might call outputs_device_session_remove(), but
  // that just won't do anything since the id will be unknown.
  if (remove->session)
    outputs_device_stop(remove, device_stop_cb);

  prev = NULL;
  for (device = outputs_device_list; device; device = device->next)
    {
      if (device == remove)
	break;

      prev = device;
    }

  if (!device)
    return;

  // Save device volume
  ret = db_speaker_save(remove);
  if (ret < 0)
    DPRINTF(E_LOG, L_PLAYER, "Could not save state for %s device '%s'\n", remove->type_name, remove->name);

  DPRINTF(E_INFO, L_PLAYER, "Removing %s device '%s'\n", remove->type_name, remove->name);

  if (!prev)
    outputs_device_list = remove->next;
  else
    prev->next = remove->next;

  outputs_device_free(remove);

  vol_adjust();
}

void
outputs_device_select(struct output_device *device, int max_volume)
{
  device->selected = 1;
  device->prevent_playback = 0;
  device->busy = 0;

  // The purpose of this is to cap the volume for a newly selected device. It is
  // used by the player to avoid this scenario:
  // 1 Play on two speakers, say Kitchen (100) and Office (75), master is 100
  // 2 Disable Office, reduce master to 25, Kitchen is now 25, Office is still 75
  // 3 Turn on Office, it now blasts at 75
  // We could avoid this by reducing the unselected Office in step 2, but that
  // leads to issue #1077, where volumes of unselected devices go to 0 (e.g. by
  // reducing master to 0 and then increasing again -> unselected stays at 0).
  if (max_volume >= 0 && device->volume > max_volume)
    device->volume = max_volume;

  vol_adjust();
}

void
outputs_device_deselect(struct output_device *device)
{
  device->selected = 0;

  vol_adjust();
}

int
outputs_device_start(struct output_device *device, output_status_cb cb, bool only_probe)
{
  int ret;

  if (outputs[device->type]->disabled || !outputs[device->type]->device_start || !outputs[device->type]->device_probe)
    return -1;

  if (device->session)
    return 0; // Device is already running, nothing to do

  if (only_probe)
    ret = outputs[device->type]->device_probe(device, callback_add(device, cb));
  else
    ret = outputs[device->type]->device_start(device, callback_add(device, cb));

  return device_state_update(device, ret);;
}

int
outputs_device_stop(struct output_device *device, output_status_cb cb)
{
  int ret;

  if (outputs[device->type]->disabled || !outputs[device->type]->device_stop)
    return -1;

  if (!device->session)
    return 0; // Device is already stopped, nothing to do

  ret = outputs[device->type]->device_stop(device, callback_add(device, cb));

  return device_state_update(device, ret);
}

int
outputs_device_stop_delayed(struct output_device *device, output_status_cb cb)
{
  if (outputs[device->type]->disabled || !outputs[device->type]->device_stop)
    return -1;

  if (!device->session)
    return 0; // Device is already stopped, nothing to do

  outputs[device->type]->device_cb_set(device, callback_add(device, cb));

  event_add(device->stop_timer, &outputs_stop_timeout);

  return 1;
}

int
outputs_device_flush(struct output_device *device, output_status_cb cb)
{
  int ret;

  if (outputs[device->type]->disabled || !outputs[device->type]->device_flush)
    return -1;

  if (!device->session)
    return 0; // Nothing to flush

  ret = outputs[device->type]->device_flush(device, callback_add(device, cb));

  return ret; // We don't change device state just because of a failed flush
}

void
outputs_device_volume_register(struct output_device *device, int absvol, int relvol)
{
  if (absvol > -1)
    device->volume = absvol;
  else if (relvol > -1)
    device->volume = rel_to_vol(relvol, outputs_master_volume);

  vol_adjust();
}

int
outputs_device_volume_set(struct output_device *device, output_status_cb cb)
{
  int ret;

  if (outputs[device->type]->disabled || !outputs[device->type]->device_volume_set)
    return -1;

  if (!device->session)
    return 0; // Device isn't active

  ret = outputs[device->type]->device_volume_set(device, callback_add(device, cb));

  return ret; // We don't change device state just because of a failed volume change
}

int
outputs_device_volume_to_pct(struct output_device *device, const char *volume)
{
  if (outputs[device->type]->disabled || !outputs[device->type]->device_volume_to_pct)
    return -1;

  return outputs[device->type]->device_volume_to_pct(device, volume);
}

int
outputs_device_quality_set(struct output_device *device, struct media_quality *quality, output_status_cb cb)
{
  int ret;

  if (outputs[device->type]->disabled || !outputs[device->type]->device_quality_set)
    return -1;

  ret = outputs[device->type]->device_quality_set(device, quality, callback_add(device, cb));

  return device_state_update(device, ret);
}

int
outputs_device_authorize(struct output_device *device, const char *pin, output_status_cb cb)
{
  int ret;

  if (outputs[device->type]->disabled || !outputs[device->type]->device_authorize)
    return -1;

  if (device->session)
    return 0; // We are already connected to the device - no auth required

  ret = outputs[device->type]->device_authorize(device, pin, callback_add(device, cb));

  return device_state_update(device, ret); // If ret < 0 then we couldn't reach the speaker
}

void
outputs_device_cb_set(struct output_device *device, output_status_cb cb)
{
  if (outputs[device->type]->disabled || !outputs[device->type]->device_cb_set)
    return;

  if (!device->session)
    return;

  outputs[device->type]->device_cb_set(device, callback_add(device, cb));
}

void
outputs_device_free(struct output_device *device)
{
  if (!device)
    return;

  if (outputs[device->type]->disabled)
    DPRINTF(E_LOG, L_PLAYER, "BUG! Freeing device from a disabled output?\n");

  if (device->session)
    DPRINTF(E_LOG, L_PLAYER, "BUG! Freeing device with active session?\n");

  if (outputs[device->type]->device_free_extra)
    outputs[device->type]->device_free_extra(device);

  if (device->stop_timer)
    event_free(device->stop_timer);

  free(device->name);
  free(device->auth_key);
  free(device->v4_address);
  free(device->v6_address);

  free(device);
}

// The return value will be the number of devices we need to wait for, either
// because they are starting or shutting down. The return value is only negative
// if we don't have to wait, i.e. all the selected devices failed immediately.
int
outputs_start(output_status_cb started_cb, output_status_cb stopped_cb, bool only_probe)
{
  struct output_device *device;
  int pending = 0;
  int ret;

  for (device = outputs_device_list; device; device = device->next)
    {
      if (device->selected)
	continue;

      ret = outputs_device_stop(device, stopped_cb);
      if (ret > 0)
	pending += ret;
    }

  ret = 0; // We don't care about devices that returned an error on stop
  for (device = outputs_device_list; device; device = device->next)
    {
      if (!device->selected)
	continue;

      ret = outputs_device_start(device, started_cb, only_probe);
      if (ret > 0)
	pending += ret;
    }

  return (pending > 0) ? pending : ret;
}

int
outputs_stop(output_status_cb cb)
{
  struct output_device *device;
  int pending = 0;
  int ret;

  for (device = outputs_device_list; device; device = device->next)
    {
      ret = outputs_device_stop(device, cb);
      if (ret < 0)
	continue;

      pending += ret;
    }

  return pending;
}

int
outputs_stop_delayed_cancel(void)
{
  struct output_device *device;

  for (device = outputs_device_list; device; device = device->next)
    event_del(device->stop_timer);

  return 0;
}

int
outputs_flush(output_status_cb cb)
{
  struct output_device *device;
  int pending = 0;
  int ret;

  for (device = outputs_device_list; device; device = device->next)
    {
      ret = outputs_device_flush(device, cb);
      if (ret < 0)
	continue;

      pending += ret;
    }

  return pending;
}

int
outputs_volume_get(void)
{
  return outputs_master_volume;
}

int
outputs_volume_set(int volume, output_status_cb cb)
{
  struct output_device *device;
  int pending = 0;
  int ret;

  if (outputs_master_volume == volume)
    return 0;

  outputs_master_volume = volume;

  for (device = outputs_device_list; device; device = device->next)
    {
      if (!device->selected)
	continue;

      device->volume = rel_to_vol(device->relvol, outputs_master_volume);

      ret = outputs_device_volume_set(device, cb);
      if (ret < 0)
	continue;

      pending += ret;
    }

  return pending;
}

int
outputs_sessions_count(void)
{
  struct output_device *device;
  int count = 0;

  for (device = outputs_device_list; device; device = device->next)
    if (device->session)
      count++;

  return count;
}

void
outputs_write(void *buf, size_t bufsize, int nsamples, struct media_quality *quality, struct timespec *pts)
{
  int i;

  buffer_fill(&output_buffer, buf, bufsize, quality, nsamples, pts);

  for (i = 0; outputs[i]; i++)
    {
      if (outputs[i]->disabled)
	continue;

      if (outputs[i]->write)
	outputs[i]->write(&output_buffer);
    }

  buffer_drain(&output_buffer);
}

void
outputs_metadata_send(uint32_t item_id, bool startup, output_metadata_finalize_cb cb)
{
  return;
/*	
  int i;

  for (i = 0; outputs[i]; i++)
    {
      if (outputs[i]->disabled || !outputs[i]->metadata_send)
	continue;

      metadata_send(i, item_id, startup, cb);
    }
*/
}

void
outputs_metadata_purge(void)
{
  int i;

  for (i = 0; outputs[i]; i++)
    {
      if (outputs[i]->disabled || !outputs[i]->metadata_purge)
	continue;

      outputs[i]->metadata_purge();
    }
}

int
outputs_priority(struct output_device *device)
{
  return outputs[device->type]->priority;
}

const char *
outputs_name(enum output_types type)
{
  return outputs[type]->name;
}

struct output_device *
outputs_list(void)
{
  return outputs_device_list;
}

int
outputs_init(void)
{
  int no_output;
  int ret;
  int i;

  outputs_master_volume = -1;

  CHECK_NULL(L_PLAYER, outputs_deferredev = evtimer_new(evbase_player, deferred_cb, NULL));

  no_output = 1;
  for (i = 0; outputs[i]; i++)
    {
      if (outputs[i]->type != i)
	{
	  DPRINTF(E_FATAL, L_PLAYER, "BUG! Output definitions are misaligned with output enum\n");
	  return -1;
	}

      if (outputs[i]->disabled)
	{
	  continue;
	}

      if (!outputs[i]->init)
	{
	  no_output = 0;
	  continue;
	}

      ret = outputs[i]->init();
      if (ret < 0)
	outputs[i]->disabled = 1;
      else
	no_output = 0;
    }

  if (no_output)
    return -1;

  for (i = 0; i < ARRAY_SIZE(output_buffer.data); i++)
    output_buffer.data[i].evbuf = evbuffer_new();

  return 0;
}

void
outputs_deinit(void)
{
  int i;

  event_free(outputs_deferredev);

  for (i = 0; outputs[i]; i++)
    {
      if (outputs[i]->disabled)
	continue;

      if (outputs[i]->deinit)
        outputs[i]->deinit();
    }

  // In case some outputs forgot to unsubscribe
  for (i = 0; i < ARRAY_SIZE(output_quality_subscriptions); i++)
    if (output_quality_subscriptions[i].count > 0)
      {
	transcode_encode_cleanup(&output_quality_subscriptions[i].encode_ctx);
	memset(&output_quality_subscriptions[i], 0, sizeof(struct output_quality_subscription));
      }

  for (i = 0; i < ARRAY_SIZE(output_buffer.data); i++)
    evbuffer_free(output_buffer.data[i].evbuf);
}

