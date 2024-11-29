#include <errno.h>
#include <fcntl.h>

#include "librespot-c-internal.h"

/* -------------------------------- Channels -------------------------------- */

/*
Here is my current understanding of the channel concept:

1. A channel is established for retrieving chunks of audio. A channel is not a
   separate connection, all the traffic goes via the same Shannon-encrypted tcp
   connection as the rest.
2. It depends on the cmd whether a channel is used. CmdStreamChunk,
   CmdStreamChunkRes, CmdChannelError, CmdChannelAbort use channels. A channel
   is identified with a uint16_t, which is the first 2 bytes of these packets.
3. A channel is established with CmdStreamChunk where receiver picks channel id.
   Spotify responds with CmdStreamChunkRes that initially has some headers after
   the channel id. The headers are "reverse tlv": uint16_t header length,
   uint8_t header id, uint8_t header_data[]. The length includes the id length.
4. After the headers are sent the channel switches to data mode. This is
   signalled by a header length of 0. In data mode Spotify sends the requested
   chunks of audio (CmdStreamChunkRes) which have the audio right after the
   channel id prefix. The audio is AES encrypted with a per-file key. An empty
   CmdStreamChunkRes indicates the end. The caller can then make a new
   CmdStreamChunk requesting the next data.
5. For Ogg, the first 167 bytes of audio is a special Spotify header.
6. The channel can presumably be reset with CmdChannelAbort (?)
*/

static int
path_to_media_id_and_type(struct sp_file *file)
{
  char *ptr;

  file->media_type = SP_MEDIA_UNKNOWN;
  if (strstr(file->path, ":track:"))
    file->media_type = SP_MEDIA_TRACK;
  else if (strstr(file->path, ":episode:"))
    file->media_type = SP_MEDIA_EPISODE;
  else
    return -1;

  ptr = strrchr(file->path, ':');
  if (!ptr || strlen(ptr + 1) != 22)
    return -1;

  return crypto_base62_to_bin(file->media_id, sizeof(file->media_id), ptr + 1);
}

struct sp_channel *
channel_get(uint32_t channel_id, struct sp_session *session)
{
  if (channel_id >= sizeof(session->channels)/sizeof(session->channels)[0])
    return NULL;

  if (session->channels[channel_id].state == SP_CHANNEL_STATE_UNALLOCATED)
    return NULL;

  return &session->channels[channel_id];
}

void
channel_free(struct sp_channel *channel)
{
  int i;

  if (!channel || channel->state == SP_CHANNEL_STATE_UNALLOCATED)
    return;

  if (channel->audio_buf)
    evbuffer_free(channel->audio_buf);

  if (channel->audio_write_ev)
    event_free(channel->audio_write_ev);

  if (channel->audio_fd[0] >= 0)
    close(channel->audio_fd[0]);

  if (channel->audio_fd[1] >= 0)
    close(channel->audio_fd[1]);

  crypto_aes_free(&channel->file.decrypt);

  free(channel->file.path);

  for (i = 0; i < ARRAY_SIZE(channel->file.cdnurl); i++)
    free(channel->file.cdnurl[i]);

  memset(channel, 0, sizeof(struct sp_channel));

  channel->audio_fd[0] = -1;
  channel->audio_fd[1] = -1;
}

void
channel_free_all(struct sp_session *session)
{
  int i;

  for (i = 0; i < sizeof(session->channels)/sizeof(session->channels)[0]; i++)
    channel_free(&session->channels[i]);
}

int
channel_new(struct sp_channel **new_channel, struct sp_session *session, const char *path, struct event_base *evbase, event_callback_fn write_cb)
{
  struct sp_channel *channel;
  uint16_t i = SP_DEFAULT_CHANNEL;
  int ret;

  channel = &session->channels[i];

  channel_free(channel);
  channel->id = i;
  channel->state = SP_CHANNEL_STATE_OPENED;

  channel->file.path = strdup(path);
  path_to_media_id_and_type(&channel->file);

  // Set up the audio I/O
  ret = pipe(channel->audio_fd);
  if (ret < 0)
    goto error;

  if (fcntl(channel->audio_fd[0], F_SETFL, O_CLOEXEC | O_NONBLOCK) < 0)
    goto error;

  if (fcntl(channel->audio_fd[1], F_SETFL, O_CLOEXEC | O_NONBLOCK) < 0)
    goto error;

  channel->audio_write_ev = event_new(evbase, channel->audio_fd[1], EV_WRITE, write_cb, session);
  if (!channel->audio_write_ev)
    goto error;

  channel->audio_buf = evbuffer_new();
  if (!channel->audio_buf)
    goto error;

  *new_channel = channel;

  return 0;

 error:
  channel_free(channel);
  return -1;
}

static int
channel_flush(struct sp_channel *channel)
{
  uint8_t buf[4096];
  int fd = channel->audio_fd[0];
  int flags;
  int got;
  int ret;

  evbuffer_drain(channel->audio_buf, -1);

  // Note that we flush the read side. We set the fd to non-blocking in case
  // the caller changed that, and then read until empty
  flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1)
    return -1;

  ret = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  if (ret < 0)
    return -1;

  do
    got = read(fd, buf, sizeof(buf));
  while (got > 0);

  ret = fcntl(fd, F_SETFL, flags);
  if (ret < 0)
    return -1;

  return 0;
}

void
channel_play(struct sp_channel *channel)
{
  channel->state = SP_CHANNEL_STATE_PLAYING;
}

void
channel_stop(struct sp_channel *channel)
{
  channel->state = SP_CHANNEL_STATE_STOPPED;

  // This will tell the reader that there is no more to read. He should then
  // call librespotc_close(), which will clean up the rest of the channel via
  // channel_free().
  close(channel->audio_fd[1]);
  channel->audio_fd[1] = -1;
}

static int
channel_seek_internal(struct sp_channel *channel, size_t pos, bool do_flush)
{
  uint32_t seek_words;
  int ret;

  if (do_flush)
    {
      ret = channel_flush(channel);
      if (ret < 0)
        RETURN_ERROR(SP_ERR_INVALID, "Could not flush read fd before seeking");

    }

  channel->seek_pos = pos;

  // If seek + header isn't word aligned we will get up to 3 bytes before the
  // actual seek position with the legacy protocol. We will remove those when
  // they are received.
  channel->seek_align = (pos + SP_OGG_HEADER_LEN) % 4;

  seek_words = (pos + SP_OGG_HEADER_LEN) / 4;

  ret = crypto_aes_seek(&channel->file.decrypt, 4 * seek_words, &sp_errmsg);
  if (ret < 0)
    RETURN_ERROR(SP_ERR_DECRYPTION, sp_errmsg);

  // Set the offset and received counter to match the seek
  channel->file.offset_bytes = 4 * seek_words;
  channel->file.received_bytes = 4 * seek_words;

  return 0;

 error:
  return ret;
}

int
channel_seek(struct sp_channel *channel, size_t pos)
{
  return channel_seek_internal(channel, pos, true); // true -> request flush
}

void
channel_pause(struct sp_channel *channel)
{
  channel_flush(channel);

  channel->state = SP_CHANNEL_STATE_PAUSED;
}

// After a disconnect we connect to another AP and try to resume. To make that
// work during playback some data elements need to be reset.
void
channel_retry(struct sp_channel *channel)
{
  size_t pos;

  if (!channel || channel->state != SP_CHANNEL_STATE_PLAYING)
    return;

  channel->is_data_mode = false;

  memset(&channel->header, 0, sizeof(struct sp_channel_header));
  memset(&channel->body, 0, sizeof(struct sp_channel_body));

  pos = channel->file.received_bytes - SP_OGG_HEADER_LEN;

  channel_seek_internal(channel, pos, false); // false => don't flush
}

// Always returns number of byte read so caller can advance read pointer. If
// header->len == 0 is returned it means that there are no more headers, and
// caller should switch the channel to data mode.
static ssize_t
channel_header_parse(struct sp_channel_header *header, uint8_t *data, size_t data_len)
{
  uint8_t *ptr;
  uint16_t be;

  if (data_len < sizeof(be))
    return -1;

  ptr = data;
  memset(header, 0, sizeof(struct sp_channel_header));

  memcpy(&be, ptr, sizeof(be));
  header->len = be16toh(be);
  ptr += sizeof(be);

  if (header->len == 0)
    goto done; // No more headers
  else if (data_len < header->len + sizeof(be))
    return -1;

  header->id = ptr[0];
  ptr += 1;

  header->data = ptr;
  header->data_len = header->len - 1;
  ptr += header->data_len;

  assert(ptr - data == header->len + sizeof(be));

 done:
  return header->len + sizeof(be);
}

static void
channel_header_handle(struct sp_channel *channel, struct sp_channel_header *header)
{
  uint32_t be32;

  sp_cb.hexdump("Received header\n", header->data, header->data_len);

  // The only header that librespot seems to use is 0x3, which is the audio file
  // size in words (incl. headers?)
  if (header->id == 0x3)
    {
      if (header->data_len != sizeof(be32))
	{
	  sp_cb.logmsg("Unexpected header length for header id 0x3\n");
	  return;
	}

      memcpy(&be32, header->data, sizeof(be32));
      channel->file.len_bytes = 4 * be32toh(be32);
    }
}

static ssize_t
channel_header_trailer_read(struct sp_channel *channel, uint8_t *msg, size_t msg_len)
{
  ssize_t parsed_len;
  ssize_t consumed_len;
  int ret;

  channel->file.end_of_chunk = false;
  channel->file.end_of_file = false;

  if (msg_len == 0)
    {
      channel->file.end_of_chunk = true;
      channel->file.end_of_file = (channel->file.received_bytes >= channel->file.len_bytes);

      // In preparation for next chunk
      channel->file.offset_bytes += SP_CHUNK_LEN;
      channel->is_data_mode = false;

      return 0;
    }
  else if (channel->is_data_mode)
    {
      return 0;
    }

  for (consumed_len = 0; msg_len > 0; msg += parsed_len, msg_len -= parsed_len)
    {
      parsed_len = channel_header_parse(&channel->header, msg, msg_len);
      if (parsed_len < 0)
	RETURN_ERROR(SP_ERR_INVALID, "Invalid channel header");

      consumed_len += parsed_len;

      if (channel->header.len == 0)
	{
	  channel->is_data_mode = true;
	  break; // All headers read
	}

      channel_header_handle(channel, &channel->header);
    }

  return consumed_len;

 error:
  return ret;
}

static int
channel_data_read(struct sp_channel *channel, uint8_t *msg, size_t msg_len)
{
  const char *errmsg;
  int ret;

  channel->file.received_bytes += msg_len;

  ret = crypto_aes_decrypt(msg, msg_len, &channel->file.decrypt, &errmsg);
  if (ret < 0)
    RETURN_ERROR(SP_ERR_DECRYPTION, errmsg);

  // Skip Spotify header
  if (!channel->is_spotify_header_received)
    {
      if (msg_len < SP_OGG_HEADER_LEN)
	RETURN_ERROR(SP_ERR_INVALID, "Invalid data received");

      channel->is_spotify_header_received = true;

      msg += SP_OGG_HEADER_LEN;
      msg_len -= SP_OGG_HEADER_LEN;
    }

  // See explanation of this in channel_seek()
  if (channel->seek_align)
    {
      msg += channel->seek_align;
      msg_len -= channel->seek_align;
      channel->seek_align = 0;
    }

  channel->body.data = msg;
  channel->body.data_len = msg_len;

  return 0;

 error:
  return ret;
}

int
channel_data_write(struct sp_channel *channel)
{
  ssize_t wrote;
  int ret;

  if (channel->state == SP_CHANNEL_STATE_PAUSED || channel->state == SP_CHANNEL_STATE_STOPPED)
    return SP_OK_DONE;

  wrote = evbuffer_write(channel->audio_buf, channel->audio_fd[1]);
  if (wrote < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    return SP_OK_WAIT;
  else if (wrote < 0)
    RETURN_ERROR(SP_ERR_WRITE, "Error writing to audio pipe");

  channel->audio_written_len += wrote;

  if (evbuffer_get_length(channel->audio_buf) > 0)
    return SP_OK_WAIT;

  return SP_OK_DONE;

 error:
  return ret;
}

int
channel_msg_read(uint16_t *channel_id, uint8_t *msg, size_t msg_len, struct sp_session *session)
{
  struct sp_channel *channel;
  uint16_t be;
  ssize_t consumed_len;
  int ret;

  if (msg_len < sizeof(be))
    RETURN_ERROR(SP_ERR_INVALID, "Chunk response is too small");

  memcpy(&be, msg, sizeof(be));
  *channel_id = be16toh(be);

  channel = channel_get(*channel_id, session);
  if (!channel)
    {
      sp_cb.hexdump("Message with unknown channel\n", msg, msg_len);
      RETURN_ERROR(SP_ERR_INVALID, "Could not recognize channel in chunk response");
    }

  msg += sizeof(be);
  msg_len -= sizeof(be);

  // Will set data_mode, end_of_file and end_of_chunk as appropriate
  consumed_len = channel_header_trailer_read(channel, msg, msg_len);
  if (consumed_len < 0)
    RETURN_ERROR((int)consumed_len, sp_errmsg);

  msg += consumed_len;
  msg_len -= consumed_len;

  channel->body.data = NULL;
  channel->body.data_len = 0;

  if (!channel->is_data_mode || !(msg_len > 0))
    return 0; // Not in data mode or no data to read

  ret = channel_data_read(channel, msg, msg_len);
  if (ret < 0)
    RETURN_ERROR(ret, sp_errmsg);

  return 0;

 error:
  return ret;
}

// With http there is the Spotify Ogg header, but no chunk header/trailer
int
channel_http_body_read(struct sp_channel *channel, uint8_t *body, size_t body_len)
{
  int ret;

  ret = channel_data_read(channel, body, body_len);
  if (ret < 0)
    goto error;

  channel->file.end_of_chunk = true;
  channel->file.end_of_file = (channel->file.received_bytes >= channel->file.len_bytes);
  channel->file.offset_bytes += SP_CHUNK_LEN;
  return 0;

 error:
  return ret;
}
