#ifndef __LIBRESPOT_C_H__
#define __LIBRESPOT_C_H__

#include <inttypes.h>
#include <stddef.h>
#include <pthread.h>

#define LIBRESPOT_C_VERSION_MAJOR 0
#define LIBRESPOT_C_VERSION_MINOR 4


struct sp_session;

enum sp_bitrates
{
  SP_BITRATE_ANY,
  SP_BITRATE_96,
  SP_BITRATE_160,
  SP_BITRATE_320,
};

typedef void (*sp_progress_cb)(int fd, void *arg, size_t received, size_t len);

struct sp_credentials
{
  char username[64];
  char password[32];

  uint8_t stored_cred[512]; // Actual size is 146, but leave room for some more
  size_t stored_cred_len;
  uint8_t token[512]; // Actual size is 270 for family accounts
  size_t token_len;
};

struct sp_metadata
{
  size_t file_len;
};

// How to identify towards Spotify. The device_id can be set to an actual value
// identifying the client, but the rest are unfortunately best left as zeroes,
// which will make librespot-c use defaults that spoof whitelisted clients.
struct sp_sysinfo
{
  char client_name[16];
  char client_id[33];
  char client_version[16];
  char client_build_id[16];
  char device_id[41]; // librespot gives a 20 byte id (so 40 char hex + 1 zero term)
};

struct sp_callbacks
{
  // Bring your own tcp connector
  int (*tcp_connect)(const char *address, unsigned short port);
  void (*tcp_disconnect)(int fd);

  // Optional - set name of thread
  void (*thread_name_set)(pthread_t thread);

  // Debugging
  void (*hexdump)(const char *msg, uint8_t *data, size_t data_len);
  void (*logmsg)(const char *fmt, ...);
};

// Deprecated, use login_token and login_stored_cred instead
struct sp_session *
librespotc_login_password(const char *username, const char *password) __attribute__ ((deprecated));

struct sp_session *
librespotc_login_stored_cred(const char *username, uint8_t *stored_cred, size_t stored_cred_len);

struct sp_session *
librespotc_login_token(const char *username, const char *token);

int
librespotc_logout(struct sp_session *session);

int
librespotc_legacy_set(struct sp_session *session, int use_legacy);

int
librespotc_bitrate_set(struct sp_session *session, enum sp_bitrates bitrate);

int
librespotc_credentials_get(struct sp_credentials *credentials, struct sp_session *session);

// Returns a file descriptor (in non-blocking mode) from which caller can read
// one chunk of data. To get more data written/start playback loop, call
// librespotc_play().
int
librespotc_open(const char *path, struct sp_session *session);

// Continues writing data to the file descriptor until error or end of track.
// A read of the fd that returns 0 means end of track, and a negative read
// return value means error. progress_cb and cb_arg optional.
void
librespotc_write(int fd, sp_progress_cb progress_cb, void *cb_arg);

// Seeks to pos (measured in bytes, so must not exceed file_len), flushes old
// data from the fd and prepares one chunk of data for reading.
int
librespotc_seek(int fd, size_t pos);

// Closes a track download, incl. the fd.
int
librespotc_close(int fd);

int
librespotc_metadata_get(struct sp_metadata *metadata, int fd);

const char *
librespotc_last_errmsg(void);

int
librespotc_init(struct sp_sysinfo *sysinfo, struct sp_callbacks *callbacks);

void
librespotc_deinit(void);

#endif /* !__LIBRESPOT_C_H__ */
