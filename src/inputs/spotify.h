#ifndef __SPOTIFY_H__
#define __SPOTIFY_H__

#include <stdbool.h>
#include <stdint.h>

struct spotify_status
{
  bool installed;
  bool logged_in;
  char username[128];
  bool has_podcast_support;
};

struct spotify_backend
{
  int (*init)(void);
  void (*deinit)(void);
  int (*login)(const char *username, const char *token, const char **errmsg);
  void (*logout)(void);
  int (*relogin)(void);
  void (*uri_register)(const char *uri);
  void (*status_get)(struct spotify_status *status);
};

int
spotify_init(void);

void
spotify_deinit(void);

int
spotify_login(const char *username, const char *token, const char **errmsg);

void
spotify_logout(void);

int
spotify_relogin(void);

void
spotify_uri_register(const char *uri);

void
spotify_status_get(struct spotify_status *status);

#endif /* !__SPOTIFY_H__ */
