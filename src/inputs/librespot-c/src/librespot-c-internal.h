#ifndef __LIBRESPOT_C_INTERNAL_H__
#define __LIBRESPOT_C_INTERNAL_H__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdbool.h>
#include <unistd.h>
#include <time.h>

#include <event2/event.h>
#include <event2/buffer.h>

#ifdef HAVE_ENDIAN_H
# include <endian.h>
#elif defined(HAVE_SYS_ENDIAN_H)
# include <sys/endian.h>
#elif defined(HAVE_LIBKERN_OSBYTEORDER_H)
#include <libkern/OSByteOrder.h>
#define htobe16(x) OSSwapHostToBigInt16(x)
#define be16toh(x) OSSwapBigToHostInt16(x)
#define htobe32(x) OSSwapHostToBigInt32(x)
#define be32toh(x) OSSwapBigToHostInt32(x)
#define htobe64(x) OSSwapHostToBigInt64(x)
#define be64toh(x) OSSwapBigToHostInt64(x)
#endif

#define ARRAY_SIZE(x) ((unsigned int)(sizeof(x) / sizeof((x)[0])))

#include "librespot-c.h"
#include "crypto.h"
#include "http.h"

#include "proto/keyexchange.pb-c.h"
#include "proto/authentication.pb-c.h"
#include "proto/mercury.pb-c.h"
#include "proto/metadata.pb-c.h"
#include "proto/clienttoken.pb-c.h"
#include "proto/login5.pb-c.h"
#include "proto/storage_resolve.pb-c.h"

// Disconnect from AP after this number of secs idle
#define SP_AP_DISCONNECT_SECS 60

// Max wait for AP to respond
#define SP_AP_TIMEOUT_SECS 10

// After a disconnect we try to reconnect, but if we are disconnected yet again
// we get the hint and won't try reconnecting again until after this cooldown
#define SP_AP_COOLDOWN_SECS 30

// How long after a connection failure we try to avoid an AP
#define SP_AP_AVOID_SECS 3600

// If client hasn't requested anything in particular
#define SP_BITRATE_DEFAULT SP_BITRATE_320

// A "mercury" response may contain multiple parts (e.g. multiple tracks), even
// though this implenentation currently expects just one.
#define SP_MERCURY_MAX_PARTS 32

// librespot uses /3, but -golang and -java use /4
#define SP_MERCURY_URI_TRACK "hm://metadata/4/track/"
#define SP_MERCURY_URI_EPISODE "hm://metadata/4/episode/"

// Special Spotify header that comes before the actual Ogg data
#define SP_OGG_HEADER_LEN 167

// For now we just always use channel 0, expand with more if needed
#define SP_DEFAULT_CHANNEL 0

// Download in chunks of 32768 bytes. The chunks shouldn't be too large because
// it makes seeking slow (seeking involves jumping around in the file), but
// large enough that the file can be probed from the first chunk.
// For comparison, Spotify for Windows seems to request 7300 byte chunks.
#define SP_CHUNK_LEN 32768

// Used to create default sysinfo, which should be librespot_[short sha]_[random 8 characters build id],
// ref https://github.com/plietar/librespot/pull/218. User may override, but
// as of 20220516 Spotify seems to have whitelisting of client name.
#define SP_CLIENT_NAME_DEFAULT "librespot"
#define SP_CLIENT_VERSION_DEFAULT "0.0.0"
#define SP_CLIENT_BUILD_ID_DEFAULT "aabbccdd"

// ClientIdHex from client_id.go. This seems to be the id that Spotify's own app
// uses. It is used in the call to https://clienttoken.spotify.com/v1/clienttoken.
// The endpoint doesn't accept client ID's of app registered at
// develop.spotify.com, so unfortunately spoofing is required.
#define SP_CLIENT_ID_DEFAULT "65b708073fc0480ea92a077233ca87bd"

// Shorthand for error handling
#define RETURN_ERROR(r, m) \
  do { ret = (r); sp_errmsg = (m); goto error; } while(0)

enum sp_error
{
  SP_OK_OTHER      = 3,
  SP_OK_WAIT       = 2,
  SP_OK_DATA       = 1,
  SP_OK_DONE       = 0,
  SP_ERR_OOM        = -1,
  SP_ERR_INVALID    = -2,
  SP_ERR_DECRYPTION = -3,
  SP_ERR_WRITE      = -4,
  SP_ERR_NOCONNECTION = -5,
  SP_ERR_OCCUPIED     = -6,
  SP_ERR_NOSESSION    = -7,
  SP_ERR_LOGINFAILED  = -8,
  SP_ERR_TIMEOUT      = -9,
};

enum sp_msg_type
{
  SP_MSG_TYPE_HTTP_REQ,
  SP_MSG_TYPE_HTTP_RES,
  SP_MSG_TYPE_TCP,
};

enum sp_seq_type
{
  SP_SEQ_STOP = 0,
  SP_SEQ_LOGIN,
  SP_SEQ_MEDIA_OPEN,
  SP_SEQ_MEDIA_GET,
  SP_SEQ_PONG,
};

enum sp_proto
{
  SP_PROTO_TCP,
  SP_PROTO_HTTP,
};

enum sp_media_type
{
  SP_MEDIA_UNKNOWN,
  SP_MEDIA_TRACK,
  SP_MEDIA_EPISODE,
};

enum sp_channel_state
{
  SP_CHANNEL_STATE_UNALLOCATED = 0,
  SP_CHANNEL_STATE_OPENED,
  SP_CHANNEL_STATE_PLAYING,
  SP_CHANNEL_STATE_PAUSED,
  SP_CHANNEL_STATE_STOPPED,
};

// From librespot-golang
enum sp_cmd_type
{
  CmdNone           = 0x00,
  CmdSecretBlock    = 0x02,
  CmdPing           = 0x04,
  CmdStreamChunk    = 0x08,

  CmdStreamChunkRes = 0x09,
  CmdChannelError   = 0x0a,
  CmdChannelAbort   = 0x0b,
  CmdRequestKey     = 0x0c,
  CmdAesKey         = 0x0d,
  CmdAesKeyError    = 0x0e,

  CmdImage          = 0x19,
  CmdCountryCode    = 0x1b,

  CmdPong           = 0x49,
  CmdPongAck        = 0x4a,
  CmdPause          = 0x4b,

  CmdProductInfo    = 0x50,
  CmdLegacyWelcome  = 0x69,

  CmdLicenseVersion = 0x76,
  CmdLogin          = 0xab,
  CmdAPWelcome      = 0xac,
  CmdAuthFailure    = 0xad,

  CmdMercuryReq     = 0xb2,
  CmdMercurySub     = 0xb3,
  CmdMercuryUnsub   = 0xb4,
};

struct sp_cmdargs
{
  struct sp_session *session;
  struct sp_credentials *credentials;
  struct sp_metadata *metadata;
  const char *username;
  const char *password;
  uint8_t *stored_cred;
  size_t stored_cred_len;
  const char *token;
  const char *path;
  int fd_read;
  int fd_write;
  size_t seek_pos;
  enum sp_bitrates bitrate;
  int use_legacy;

  sp_progress_cb progress_cb;
  void *cb_arg;
};

struct sp_conn_callbacks
{
  struct event_base *evbase;

  event_callback_fn response_cb;
  event_callback_fn timeout_cb;
};

struct sp_tcp_message
{
  enum sp_cmd_type cmd;

  bool encrypt;
  bool add_version_header;

  size_t len;
  uint8_t *data;
};

struct sp_message
{
  enum sp_msg_type type;

  union payload
    {
      struct sp_tcp_message tmsg;
      struct http_request hreq;
      struct http_response hres;
    } payload;
};

struct sp_server
{
  char address[256]; // e.g. ap-gue1.spotify.com
  unsigned short port; // normally 433 or 4070
  time_t last_connect_ts;
  time_t last_resolved_ts;
  time_t last_failed_ts;
};

struct sp_connection
{
  struct sp_server *server; // NULL or pointer to session.accesspoint

  bool is_connected;
  bool is_encrypted;

  // Where we receive data from Spotify
  int response_fd;
  struct event *response_ev;

  // Connection timers
  struct event *idle_ev;
  struct event *timeout_ev;

  // Holds incoming data
  struct evbuffer *incoming;

  // Buffer holding client hello and ap response, since they are needed for
  // MAC calculation
  bool handshake_completed;
  struct evbuffer *handshake_packets;

  struct crypto_keys keys;
  struct crypto_cipher encrypt;
  struct crypto_cipher decrypt;
};

struct sp_token
{
  char value[512]; // base64 string, actual size 360 bytes
  int32_t expires_after_seconds;
  int32_t refresh_after_seconds;
  time_t received_ts;
};

struct sp_mercury
{
  char *uri;
  char *method;
  char *content_type;

  uint64_t seq;

  uint16_t parts_num;
  struct sp_mercury_parts
    {
      uint8_t *data;
      size_t len;

      Track *track;
    } parts[SP_MERCURY_MAX_PARTS];
};

struct sp_file
{
  uint8_t id[20];

  char *path; // The Spotify URI, e.g. spotify:episode:3KRjRyqv5ou5SilNMYBR4E
  uint8_t media_id[16]; // Decoded value of the URIs base62
  enum sp_media_type media_type; // track or episode from URI

  // For files that are served via http/"new protocol" (we may receive multiple
  // urls).
  char *cdnurl[4];

  uint8_t key[16];

  uint16_t channel_id;

  // Length and download progress
  size_t len_bytes;
  size_t offset_bytes;
  size_t received_bytes;
  bool end_of_file;
  bool end_of_chunk;
  bool open;

  struct crypto_aes_cipher decrypt;
};

struct sp_channel_header
{
  uint16_t len;
  uint8_t id;
  uint8_t *data;
  size_t data_len;
};

struct sp_channel_body
{
  uint8_t *data;
  size_t data_len;
};

struct sp_channel
{
  int id;

  enum sp_channel_state state;

  bool is_data_mode;
  bool is_spotify_header_received;
  size_t seek_pos;
  size_t seek_align;

  // pipe where we write audio data
  int audio_fd[2];
  // Triggers when fd is writable
  struct event *audio_write_ev;
  // Storage of audio until it can be written to the pipe
  struct evbuffer *audio_buf;
  // How much we have written to the fd (only used for debug)
  size_t audio_written_len;

  struct sp_file file;

  // Latest header and body received
  struct sp_channel_header header;
  struct sp_channel_body body;

  // Callbacks made during playback
  sp_progress_cb progress_cb;
  void *cb_arg;
};

// Linked list of sessions
struct sp_session
{
  struct sp_server accesspoint;
  struct sp_server spclient;
  struct sp_server dealer;

  struct sp_connection conn;
  time_t cooldown_ts;

  // Use legacy protocol (non http, see seq_requests_legacy)
  bool use_legacy;

  struct http_session http_session;
  struct sp_token http_clienttoken;
  struct sp_token http_accesstoken;

  int n_hashcash_challenges;
  struct crypto_hashcash_challenge *hashcash_challenges;

  bool is_logged_in;
  struct sp_credentials credentials;
  char country[3]; // Incl null term

  enum sp_bitrates bitrate_preferred;

  struct sp_channel channels[8];

  // Points to the channel that is streaming, and via this information about
  // the current track is also available
  struct sp_channel *now_streaming_channel;

  // Current request in the sequence
  struct sp_seq_request *request;

  // Go to next step in a request sequence
  struct event *continue_ev;

  // Which sequence comes next
  enum sp_seq_type next_seq;

  struct sp_session *next;
};

struct sp_seq_request
{
  enum sp_seq_type seq_type;
  const char *name; // Name of request (for logging)
  enum sp_proto proto;
  int (*payload_make)(struct sp_message *, struct sp_session *);
  enum sp_error (*request_prepare)(struct sp_seq_request *, struct sp_conn_callbacks *, struct sp_session *);
  enum sp_error (*response_handler)(struct sp_message *, struct sp_session *);
};

struct sp_err_map
{
  int errorcode;
  const char *errmsg;
};


extern struct sp_callbacks sp_cb;
extern struct sp_sysinfo sp_sysinfo;
extern const char *sp_errmsg;

#endif // __LIBRESPOT_C_INTERNAL_H__
