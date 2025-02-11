/*
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
#include <unistd.h>
#include <stdint.h>
#include <string.h>

#include "logger.h"
#include "spotify.h"

// With just one backend the abstraction implemented here is a bit overkill, but
// it was added back when there was also libspotify. Keep it around for a while
// and then consider removing.

#ifdef SPOTIFY_LIBRESPOTC
extern struct spotify_backend spotify_librespotc;
#endif

static struct spotify_backend *
backend_set(void)
{
#ifdef SPOTIFY_LIBRESPOTC
  return &spotify_librespotc;
#endif
  DPRINTF(E_LOG, L_SPOTIFY, "Invalid Spotify configuration (not built with the configured backend)\n");
  return NULL;
}


/* -------------- Dispatches functions exposed via spotify.h ---------------- */
/*             Called from other threads than the input thread                */

int
spotify_init(void)
{
  struct spotify_backend *backend = backend_set();

  if (!backend || !backend->init)
    return 0; // Just a no-op

  return backend->init();
}

void
spotify_deinit(void)
{
  struct spotify_backend *backend = backend_set();

  if (!backend || !backend->deinit)
    return;

  backend->deinit();
}

int
spotify_login(const char *username, const char *token, const char **errmsg)
{
  struct spotify_backend *backend = backend_set();

  if (!backend || !backend->login)
    return -1;

  return backend->login(username, token, errmsg);
}

void
spotify_logout(void)
{
  struct spotify_backend *backend = backend_set();

  if (!backend || !backend->logout)
    return;

  backend->logout();
}

int
spotify_relogin(void)
{
  struct spotify_backend *backend = backend_set();

  if (!backend || !backend->relogin)
    return -1;

  return backend->relogin();
}

void
spotify_uri_register(const char *uri)
{
  struct spotify_backend *backend = backend_set();

  if (!backend || !backend->uri_register)
    return;

  backend->uri_register(uri);
}

void
spotify_status_get(struct spotify_status *status)
{
  struct spotify_backend *backend = backend_set();

  memset(status, 0, sizeof(struct spotify_status));

  if (!backend || !backend->status_get)
    return;

  backend->status_get(status);
}
