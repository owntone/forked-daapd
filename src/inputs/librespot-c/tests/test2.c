#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h> // for isprint()

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

// For file output
#include <sys/stat.h>
#include <fcntl.h>

#include <event2/event.h>
#include <event2/buffer.h>

#include "librespot-c.h"

static int audio_fd = -1;
static int test_file = -1;
static struct event_base *evbase;
static struct evbuffer *audio_buf;

static int total_bytes;

static void
hexdump(const char *msg, uint8_t *mem, size_t len)
{
  int i, j;
  int hexdump_cols = 16;

  if (msg)
    printf("%s", msg);

  for (i = 0; i < len + ((len % hexdump_cols) ? (hexdump_cols - len % hexdump_cols) : 0); i++)
    {
      if(i % hexdump_cols == 0)
	printf("0x%06x: ", i);

      if (i < len)
	printf("%02x ", 0xFF & ((char*)mem)[i]);
      else
	printf("   ");

      if (i % hexdump_cols == (hexdump_cols - 1))
	{
	  for (j = i - (hexdump_cols - 1); j <= i; j++)
	    {
	      if (j >= len)
		putchar(' ');
	      else if (isprint(((char*)mem)[j]))
		putchar(0xFF & ((char*)mem)[j]);
	      else
		putchar('.');
	    }

	  putchar('\n');
	}
    }
}

static void
logmsg(const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
}

static int
tcp_connect(const char *address, unsigned short port)
{
  struct addrinfo hints = { 0 };
  struct addrinfo *servinfo;
  struct addrinfo *ptr;
  char strport[8];
  int fd;
  int ret;

  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_UNSPEC;

  snprintf(strport, sizeof(strport), "%hu", port);
  ret = getaddrinfo(address, strport, &hints, &servinfo);
  if (ret < 0)
    {
      printf("Could not connect to %s (port %u): %s\n", address, port, gai_strerror(ret));
      return -1;
    }

  for (ptr = servinfo; ptr; ptr = ptr->ai_next)
    {
      fd = socket(ptr->ai_family, SOCK_STREAM, ptr->ai_protocol);
      if (fd < 0)
	{
	  continue;
	}

      ret = connect(fd, ptr->ai_addr, ptr->ai_addrlen);
      if (ret < 0)
	{
	  close(fd);
	  continue;
	}

      break;
    }

  freeaddrinfo(servinfo);

  if (!ptr)
    {
      printf("Could not connect to '%s' (port %u): %s\n", address, port, strerror(errno));
      return -1;
    }

  printf("Connected to %s (port %u)\n", address, port);

  return fd;
}

static void
tcp_disconnect(int fd)
{
  if (fd < 0)
    return;

  close(fd);
}

static void
progress_cb(int fd, void *arg, size_t received, size_t len)
{
  printf("Progress on fd %d is %zu/%zu\n", fd, received, len);
}

// This thread
static void
audio_read_cb(int fd, short what, void *arg)
{
  int got;

  got = evbuffer_read(audio_buf, fd, -1);
  if (got <= 0)
    {
      printf("Playback ended (%d)\n", got);
      event_base_loopbreak(evbase);
      return;
    }

  total_bytes += got;

  printf("Got %d bytes of audio, total received is %d bytes\n", got, total_bytes);

  evbuffer_write(audio_buf, test_file);
}

struct sp_callbacks callbacks =
{
  .tcp_connect = tcp_connect,
  .tcp_disconnect = tcp_disconnect,

  .thread_name_set = NULL,

  .hexdump  = hexdump,
  .logmsg   = logmsg,
};

int
main(int argc, char * argv[])
{
  struct sp_session *session = NULL;
  struct sp_sysinfo sysinfo = { 0 };
//  struct sp_credentials credentials;
  struct sp_metadata metadata;
  struct event *read_ev;
  FILE *f_stored_cred = NULL;
  uint8_t stored_cred[256];
  size_t stored_cred_len;
//  struct event *stop_ev;
//  struct timeval tv = { 0 };
  int ret;

  if (argc != 4)
    {
      printf("%s spotify_path username stored_credentials_file\n", argv[0]);
      goto error;
    }

  snprintf(sysinfo.device_id, sizeof(sysinfo.device_id), "622682995d5c1db29722de8dd85f6c3acd6fc592");

  ret = librespotc_init(&sysinfo, &callbacks);
  if (ret < 0)
    {
      printf("Error initializing Spotify: %s\n", librespotc_last_errmsg());
      goto error;
    }

  f_stored_cred = fopen(argv[3], "rb");
  if (!f_stored_cred)
    {
      printf("Error opening file with stored credentials\n");
      goto error;
    }

  stored_cred_len = fread(stored_cred, 1, sizeof(stored_cred), f_stored_cred);
  if (stored_cred_len == 0)
    {
      printf("Stored credentials file is empty\n");
      goto error;
    }

  session = librespotc_login_stored_cred(argv[2], stored_cred, stored_cred_len);
  if (!session)
    {
      printf("Error logging in with stored credentials: %s\n", librespotc_last_errmsg());
      goto error;
    }

  printf("\n--- Login with stored credentials OK ---\n\n");

  audio_fd = librespotc_open(argv[1], session);
  if (audio_fd < 0)
    {
      printf("Error opening file: %s\n", librespotc_last_errmsg());
      goto error;
    }

  ret = librespotc_metadata_get(&metadata, audio_fd);
  if (ret < 0)
    {
      printf("Error getting track metadata: %s\n", librespotc_last_errmsg());
      goto error;
    }

  printf("File is open, length is %zu\n", metadata.file_len);

  test_file = open("testfile.ogg", O_CREAT | O_RDWR, 0664);
  if (test_file < 0)
    {
      printf("Error opening testfile.ogg: %s\n", strerror(errno));
      goto error;
    }

  evbase = event_base_new();
  audio_buf = evbuffer_new();

  read_ev = event_new(evbase, audio_fd, EV_READ | EV_PERSIST, audio_read_cb, NULL);
  event_add(read_ev, NULL);

  librespotc_write(audio_fd, progress_cb, NULL);

//  stop_ev = evtimer_new(evbase, stop, &audio_fd);
//  tv.tv_sec = 2;
//  event_add(stop_ev, &tv);

  event_base_dispatch(evbase);

//  event_free(stop_ev);
  event_free(read_ev);

  close(test_file);

  evbuffer_free(audio_buf);
  event_base_free(evbase);

 error:
  if (audio_fd >= 0)
    librespotc_close(audio_fd);
  if (session)
    librespotc_logout(session);
  if (f_stored_cred)
    fclose(f_stored_cred);

  librespotc_deinit();
  return ret;
}
