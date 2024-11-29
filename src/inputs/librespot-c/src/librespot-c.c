/*
 * The MIT License (MIT)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */


/*
Illustration of the general tcp flow, where receive and writing the result are
async operations. For some commands, e.g. open and seek, the entire sequence is
encapsulated in a sync command, which doesn't return until final "done, error or
timeout". The command play is async, so all "done/error/timeout" is returned via
callbacks. Also, play will loop the flow, i.e. after writing a chunk of data it
will go back and ask for the next chunk of data from Spotify.

In some cases there is no result to write, or no reponse expected, but then the
events for proceeding are activated directly.

    |---next----*------------next-------------*----------next----------*
    v           |                             |                        |
----------> start/send  ------------------> recv ----------------> write result
^               |            ^                |       ^                |
|---reconnect---*            |------wait------*       |------wait------*
                |                             |                        |
                v                             v                        v
           done/error                done/error/timeout           done/error

"next": on success, continue with next command
"wait": waiting for more data or for write to become possible
"timeout": receive or write took too long to complete
*/

#include <pthread.h>
#include <assert.h>

#include "librespot-c-internal.h"
#include "commands.h"
#include "connection.h"
#include "channel.h"

// #define DEBUG_DISCONNECT 1

/* -------------------------------- Globals --------------------------------- */

// Shared
struct sp_callbacks sp_cb;
struct sp_sysinfo sp_sysinfo;
const char *sp_errmsg;

static struct sp_session *sp_sessions;

static bool sp_initialized;

static pthread_t sp_tid;
static struct event_base *sp_evbase;
static struct commands_base *sp_cmdbase;

static struct timeval sp_response_timeout_tv = { SP_AP_TIMEOUT_SECS, 0 };

#ifdef DEBUG_DISCONNECT
static int debug_disconnect_counter;
#endif

// Forwards
static void
sequence_continue(struct sp_session *session);

/* -------------------------------- Session --------------------------------- */

static void
session_free(struct sp_session *session)
{
  if (!session)
    return;

  channel_free_all(session);

  ap_disconnect(&session->conn);

  event_free(session->continue_ev);

  http_session_deinit(&session->http_session);

  free(session);
}

static void
session_cleanup(struct sp_session *session)
{
  struct sp_session *s;

  if (!session)
    return;

  if (session == sp_sessions)
    sp_sessions = session->next;
  else
    {
      for (s = sp_sessions; s && (s->next != session); s = s->next)
	; /* EMPTY */

      if (s)
	s->next = session->next;
    }

  session_free(session);
}

static int
session_new(struct sp_session **out, struct sp_cmdargs *cmdargs, event_callback_fn cb)
{
  struct sp_session *session;
  int ret;

  session = calloc(1, sizeof(struct sp_session));
  if (!session)
    RETURN_ERROR(SP_ERR_OOM, "Out of memory creating session");

  http_session_init(&session->http_session);

  session->continue_ev = evtimer_new(sp_evbase, cb, session);
  if (!session->continue_ev)
    RETURN_ERROR(SP_ERR_OOM, "Out of memory creating session event");

  snprintf(session->credentials.username, sizeof(session->credentials.username), "%s", cmdargs->username);

  if (cmdargs->stored_cred)
    {
      if (cmdargs->stored_cred_len > sizeof(session->credentials.stored_cred))
	RETURN_ERROR(SP_ERR_INVALID, "Stored credentials too long");

      session->credentials.stored_cred_len = cmdargs->stored_cred_len;
      memcpy(session->credentials.stored_cred, cmdargs->stored_cred, session->credentials.stored_cred_len);
    }
  else if (cmdargs->token)
    {
      if (strlen(cmdargs->token) > sizeof(session->credentials.token))
	RETURN_ERROR(SP_ERR_INVALID, "Token too long");

      session->credentials.token_len = strlen(cmdargs->token);
      memcpy(session->credentials.token, cmdargs->token, session->credentials.token_len);
    }
  else
    {
      snprintf(session->credentials.password, sizeof(session->credentials.password), "%s", cmdargs->password);
    }

  session->bitrate_preferred = SP_BITRATE_DEFAULT;

  // Add to linked list
  session->next = sp_sessions;
  sp_sessions = session;

  *out = session;

  return 0;

 error:
  session_free(session);
  return ret;
}

static int
session_check(struct sp_session *session)
{
  struct sp_session *s;

  for (s = sp_sessions; s; s = s->next)
    {
      if (s == session)
	return 0;
    }

  return -1;
}

static struct sp_session *
session_find_by_fd(int fd)
{
  struct sp_session *s;

  for (s = sp_sessions; s; s = s->next)
    {
      if (s->now_streaming_channel && s->now_streaming_channel->audio_fd[0] == fd)
	return s;
    }

  return NULL;
}

static void
session_return(struct sp_session *session, enum sp_error err)
{
  struct sp_channel *channel = session->now_streaming_channel;
  int ret;

  ret = commands_exec_returnvalue(sp_cmdbase);
  if (ret == 0) // Here we are async, i.e. no pending command
    {
      // track_write() completed, close the write end which means reader will
      // get an EOF
      if (channel && channel->state == SP_CHANNEL_STATE_PLAYING && err == SP_OK_DONE)
	channel_stop(channel);
      return;
    }

  commands_exec_end(sp_cmdbase, err);
}

// Disconnects after an error situation. If it is a failed login then the
// session, otherwise we end download and disconnect.
static void
session_error(struct sp_session *session, enum sp_error err)
{
  sp_cb.logmsg("Session error %d: %s\n", err, sp_errmsg);

  session_return(session, err);

  if (!session->is_logged_in)
    {
      session_cleanup(session);
      return;
    }

  channel_free_all(session);
  session->now_streaming_channel = NULL;

  ap_disconnect(&session->conn);
}

// Called if an access point disconnects. Will clear current connection and
// start a flow where the same request will be made to another access point.
// This is currently only implemented for the non-http connection.
static void
session_retry(struct sp_session *session)
{
  struct sp_channel *channel = session->now_streaming_channel;

  sp_cb.logmsg("Retrying after disconnect\n");

  channel_retry(channel);

  ap_blacklist(session->conn.server);

  ap_disconnect(&session->conn);

  // If we were doing something other than login, queue that
  if (session->request->seq_type != SP_SEQ_LOGIN)
    seq_next_set(session, session->request->seq_type);

  // Trigger login on a new server
  session->request = seq_request_get(SP_SEQ_LOGIN, 0, session->use_legacy);
  sequence_continue(session);
}


/* ------------------------ Main sequence control --------------------------- */

// This callback is triggered by response_cb when the message response handler
// said that there was data to write. If not all data can be written in one pass
// it will re-add the event.
static void
audio_write_cb(int fd, short what, void *arg)
{
  struct sp_session *session = arg;
  struct sp_channel *channel = session->now_streaming_channel;
  int ret;

  if (!channel)
    RETURN_ERROR(SP_ERR_INVALID, "Write result request, but not streaming right now");

  ret = channel_data_write(channel);
  switch (ret)
    {
      case SP_OK_WAIT:
	event_add(channel->audio_write_ev, NULL);
	break;
      case SP_OK_DONE:
	event_active(session->continue_ev, 0, 0);
	break;
      default:
	goto error;
    }

  return;

 error:
  session_error(session, ret);
}

static void
timeout_tcp_cb(int fd, short what, void *arg)
{
  struct sp_session *session = arg;

  sp_errmsg = "Timeout waiting for Spotify response";

  session_error(session, SP_ERR_TIMEOUT);
}

static void
audio_data_received(struct sp_session *session)
{
  struct sp_channel *channel = session->now_streaming_channel;

  if (channel->state == SP_CHANNEL_STATE_PLAYING && !channel->file.end_of_file)
    seq_next_set(session, SP_SEQ_MEDIA_GET);
  if (channel->progress_cb)
    channel->progress_cb(channel->audio_fd[0], channel->cb_arg, channel->file.received_bytes - SP_OGG_HEADER_LEN, channel->file.len_bytes - SP_OGG_HEADER_LEN);

  event_add(channel->audio_write_ev, NULL);
}

static void
incoming_tcp_cb(int fd, short what, void *arg)
{
  struct sp_session *session = arg;
  struct sp_connection *conn = &session->conn;
  struct sp_message msg = { .type = SP_MSG_TYPE_TCP };
  int ret;

  if (what == EV_READ)
    {
      ret = evbuffer_read(conn->incoming, fd, -1);
#ifdef DEBUG_DISCONNECT
      debug_disconnect_counter++;
      if (debug_disconnect_counter == 1000)
	{
	  sp_cb.logmsg("Simulating a disconnection from the access point (last request was %s)\n", session->request->name);
	  ret = 0;
	}
#endif

      if (ret == 0)
	RETURN_ERROR(SP_ERR_NOCONNECTION, "The access point disconnected");
      else if (ret < 0)
	RETURN_ERROR(SP_ERR_NOCONNECTION, "Connection to Spotify returned an error");
    }

  // Allocates *data in msg
  ret = msg_tcp_read_one(&msg.payload.tmsg, conn);
  if (ret == SP_OK_WAIT)
    return;
  else if (ret < 0)
    goto error;

  if (msg.payload.tmsg.len < 128)
    sp_cb.hexdump("Received tcp message\n", msg.payload.tmsg.data, msg.payload.tmsg.len);
  else
    sp_cb.hexdump("Received tcp message (truncated)\n", msg.payload.tmsg.data, 128);

  ret = msg_handle(&msg, session);
  switch (ret)
    {
      case SP_OK_WAIT: // Incomplete, wait for more data
	break;
      case SP_OK_DATA:
	audio_data_received(session);

	event_del(conn->timeout_ev);
	break;
      case SP_OK_DONE: // Got the response we expected, but possibly more to process
	if (evbuffer_get_length(conn->incoming) > 0)
	  event_active(conn->response_ev, 0, 0);

	event_del(conn->timeout_ev);
	event_active(session->continue_ev, 0, 0);
	break;
      case SP_OK_OTHER: // Not the response we were waiting for, check for other
	if (evbuffer_get_length(conn->incoming) > 0)
	  event_active(conn->response_ev, 0, 0);
	break;
      default:
	event_del(conn->timeout_ev);
	goto error;
    }

  msg_clear(&msg);
  return;

 error:
  msg_clear(&msg);

  if (ret == SP_ERR_NOCONNECTION)
    session_retry(session);
  else
    session_error(session, ret);
}

static enum sp_error
msg_send(struct sp_message *msg, struct sp_session *session)
{
  struct sp_message res;
  struct sp_connection *conn = &session->conn;
  enum sp_error ret;

  if (session->request->proto == SP_PROTO_TCP)
    {
      if (msg->payload.tmsg.encrypt)
        conn->is_encrypted = true;

      ret = msg_tcp_send(&msg->payload.tmsg, conn);
      if (ret < 0)
        RETURN_ERROR(ret, sp_errmsg);

      // Only start timeout timer if a response is expected, otherwise go
      // straight to next message
      if (session->request->response_handler)
        event_add(conn->timeout_ev, &sp_response_timeout_tv);
      else
        event_active(session->continue_ev, 0, 0);
    }
  else if (session->request->proto == SP_PROTO_HTTP)
    {
      res.type = SP_MSG_TYPE_HTTP_RES;

      // Using http_session ensures that Curl will use keepalive and doesn't
      // need to reconnect with every request
      ret = msg_http_send(&res.payload.hres, &msg->payload.hreq, &session->http_session);
      if (ret < 0)
        RETURN_ERROR(ret, sp_errmsg);

      // Since http requests are currently sync we can handle the response right
      // away. In an async future we would need to make an incoming event and
      // have a callback func for msg_handle, like for tcp.
      ret = msg_handle(&res, session);
      msg_clear(&res);
      if (ret < 0)
        RETURN_ERROR(ret, sp_errmsg);
      else if (ret == SP_OK_DATA)
	audio_data_received(session);
      else
	event_active(session->continue_ev, 0, 0);
    }
  else
    RETURN_ERROR(SP_ERR_INVALID, "Bug! Request is missing protocol type");

  return SP_OK_DONE;

 error:
  return ret;
}

static void
sequence_continue(struct sp_session *session)
{
  struct sp_conn_callbacks cb = { sp_evbase, incoming_tcp_cb, timeout_tcp_cb };
  struct sp_message msg = { 0 };
  int ret;

//  sp_cb.logmsg("Preparing request '%s'\n", session->request->name);

  // Checks if the dependencies for making the request are met - e.g. do we have
  // a connection and a valid token. If not, tries to satisfy them.
  ret = seq_request_prepare(session->request, &cb, session);
  if (ret == SP_OK_WAIT)
    sp_cb.logmsg("Sequence queued, first making request '%s'\n", session->request->name);
  else if (ret < 0)
    RETURN_ERROR(ret, sp_errmsg);

  ret = msg_make(&msg, session->request, session);
  if (ret > 0)
    {
      event_active(session->continue_ev, 0, 0);
      return;
    }
  else if (ret < 0)
    RETURN_ERROR(SP_ERR_INVALID, "Error constructing message to Spotify");

  ret = msg_send(&msg, session);
  if (ret < 0)
    RETURN_ERROR(ret, sp_errmsg);

  msg_clear(&msg);
  return; // Proceed in sequence_continue_cb

 error:
  msg_clear(&msg);
  session_error(session, ret);
}

static void
sequence_continue_cb(int fd, short what, void *arg)
{
  struct sp_session *session = arg;

  // If set, we are in a sequence and should proceed to the next request
  if (session->request)
    session->request++;

  // Starting a sequence, or ending one and should possibly start the next
  if (!session->request || !session->request->name)
    {
      session->request = seq_request_get(session->next_seq, 0, session->use_legacy);
      seq_next_set(session, SP_SEQ_STOP);
    }

  if (session->request && session->request->name)
    sequence_continue(session);
  else
    session_return(session, SP_OK_DONE); // All done, yay!
}

// All errors that may occur during a sequence are called back async
static void
sequence_start(enum sp_seq_type seq_type, struct sp_session *session)
{
  session->request = NULL;
  seq_next_set(session, seq_type);

  event_active(session->continue_ev, 0, 0);
}


/* ----------------------------- Implementation ----------------------------- */

// This command is async
static enum command_state
track_write(void *arg, int *retval)
{
  struct sp_cmdargs *cmdargs = arg;
  struct sp_session *session;
  struct sp_channel *channel;
  int ret;

  *retval = 0;

  session = session_find_by_fd(cmdargs->fd_read);
  if (!session)
    RETURN_ERROR(SP_ERR_NOSESSION, "Cannot play track, no valid session found");

  channel = session->now_streaming_channel;
  if (!channel || channel->state == SP_CHANNEL_STATE_UNALLOCATED)
    RETURN_ERROR(SP_ERR_INVALID, "No active channel to play, has track been opened?");

  channel_play(channel);

  sequence_start(SP_SEQ_MEDIA_GET, session);

  channel->progress_cb = cmdargs->progress_cb;
  channel->cb_arg = cmdargs->cb_arg;

  return COMMAND_END;

 error:
  sp_cb.logmsg("Error %d: %s\n", ret, sp_errmsg);

  return COMMAND_END;
}

static enum command_state
track_pause(void *arg, int *retval)
{
  struct sp_cmdargs *cmdargs = arg;
  struct sp_session *session;
  struct sp_channel *channel;
  int ret;

  session = session_find_by_fd(cmdargs->fd_read);
  if (!session)
    RETURN_ERROR(SP_ERR_NOSESSION, "Cannot pause track, no valid session found");

  channel = session->now_streaming_channel;
  if (!channel || channel->state == SP_CHANNEL_STATE_UNALLOCATED)
    RETURN_ERROR(SP_ERR_INVALID, "No active channel to pause, has track been opened?");

  // If we are playing we are in the process of downloading a chunk, and in that
  // case we need that to complete before doing anything else with the channel,
  // e.g. reset it as track_close() does.
  if (channel->state != SP_CHANNEL_STATE_PLAYING)
    {
      *retval = 0;
      return COMMAND_END;
    }

  channel_pause(channel);
  seq_next_set(session, SP_SEQ_STOP); // TODO test if this will work

  *retval = 1;
  return COMMAND_PENDING;

 error:
  *retval = ret;
  return COMMAND_END;
}

static enum command_state
track_seek(void *arg, int *retval)
{
  struct sp_cmdargs *cmdargs = arg;
  struct sp_session *session;
  struct sp_channel *channel;
  int ret;

  session = session_find_by_fd(cmdargs->fd_read);
  if (!session)
    RETURN_ERROR(SP_ERR_NOSESSION, "Cannot seek, no valid session found");

  channel = session->now_streaming_channel;
  if (!channel)
    RETURN_ERROR(SP_ERR_INVALID, "No active channel to seek, has track been opened?");
  else if (channel->state != SP_CHANNEL_STATE_OPENED)
    RETURN_ERROR(SP_ERR_INVALID, "Seeking during playback not currently supported");

  // This operation is not safe during chunk downloading because it changes the
  // AES decryptor to match the new position. It also flushes the pipe.
  channel_seek(channel, cmdargs->seek_pos);

  sequence_start(SP_SEQ_MEDIA_GET, session);

  *retval = 1;
  return COMMAND_PENDING;

 error:
  *retval = ret;
  return COMMAND_END;
}

static enum command_state
track_close(void *arg, int *retval)
{
  struct sp_cmdargs *cmdargs = arg;
  struct sp_session *session;
  int ret;

  session = session_find_by_fd(cmdargs->fd_read);
  if (!session)
    RETURN_ERROR(SP_ERR_NOSESSION, "Cannot close track, no valid session found");

  channel_free(session->now_streaming_channel);
  session->now_streaming_channel = NULL;

  *retval = 0;
  return COMMAND_END;

 error:
  *retval = ret;
  return COMMAND_END;
}

static enum command_state
media_open(void *arg, int *retval)
{
  struct sp_cmdargs *cmdargs = arg;
  struct sp_session *session = cmdargs->session;
  struct sp_channel *channel = NULL;
  int ret;

  ret = session_check(session);
  if (ret < 0)
    RETURN_ERROR(SP_ERR_NOSESSION, "Cannot open media, session is invalid");

  if (session->now_streaming_channel)
    RETURN_ERROR(SP_ERR_OCCUPIED, "Already getting media");

  ret = channel_new(&channel, session, cmdargs->path, sp_evbase, audio_write_cb);
  if (ret < 0)
    RETURN_ERROR(SP_ERR_OOM, "Could not setup a channel");

  cmdargs->fd_read = channel->audio_fd[0];

  // Must be set before calling sequence_start() because this info is needed for
  // making the request
  session->now_streaming_channel = channel;

  // Kicks of a sequence where we first get file info, then get the AES key and
  // then the first chunk (incl. headers)
  sequence_start(SP_SEQ_MEDIA_OPEN, session);

  *retval = 1;
  return COMMAND_PENDING;

 error:
  if (channel)
    {
      session->now_streaming_channel = NULL;
      channel_free(channel);
    }

  *retval = ret;
  return COMMAND_END;
}

static enum command_state
media_open_bh(void *arg, int *retval)
{
  struct sp_cmdargs *cmdargs = arg;

  if (*retval == SP_OK_DONE)
    *retval = cmdargs->fd_read;

  return COMMAND_END;
}

static enum command_state
login(void *arg, int *retval)
{
  struct sp_cmdargs *cmdargs = arg;
  struct sp_session *session = NULL;
  int ret;

  ret = session_new(&session, cmdargs, sequence_continue_cb);
  if (ret < 0)
    goto error;

  sequence_start(SP_SEQ_LOGIN, session);

  cmdargs->session = session;

  *retval = 1; // Pending command_exec_sync, i.e. response from Spotify
  return COMMAND_PENDING;

 error:
  session_cleanup(session);

  *retval = ret;
  return COMMAND_END;
}

static enum command_state
login_bh(void *arg, int *retval)
{
  struct sp_cmdargs *cmdargs = arg;

  if (*retval == SP_OK_DONE)
    cmdargs->session->is_logged_in = true;
  else
    cmdargs->session = NULL;

  return COMMAND_END;
}

static enum command_state
logout(void *arg, int *retval)
{
  struct sp_cmdargs *cmdargs = arg;
  struct sp_session *session = cmdargs->session;
  int ret;

  ret = session_check(session);
  if (ret < 0)
    RETURN_ERROR(SP_ERR_NOSESSION, "Session has disappeared, cannot logout");

  session_cleanup(session);

 error:
  *retval = ret;
  return COMMAND_END;
}

static enum command_state
legacy_set(void *arg, int *retval)
{
  struct sp_cmdargs *cmdargs = arg;
  struct sp_session *session = cmdargs->session;
  int ret;

  ret = session_check(session);
  if (ret < 0)
    RETURN_ERROR(SP_ERR_NOSESSION, "Session has disappeared, cannot set legacy mode");

  if (session->request && session->request->name)
    RETURN_ERROR(SP_ERR_INVALID, "Can't switch mode while session is active");

  session->use_legacy = cmdargs->use_legacy;

 error:
  *retval = ret;
  return COMMAND_END;
}

static enum command_state
metadata_get(void *arg, int *retval)
{
  struct sp_cmdargs *cmdargs = arg;
  struct sp_session *session;
  struct sp_metadata *metadata = cmdargs->metadata;
  int ret = 0;

  session = session_find_by_fd(cmdargs->fd_read);
  if (!session || !session->now_streaming_channel)
    RETURN_ERROR(SP_ERR_NOSESSION, "Session has disappeared, cannot get metadata");

  memset(metadata, 0, sizeof(struct sp_metadata));
  metadata->file_len = session->now_streaming_channel->file.len_bytes - SP_OGG_HEADER_LEN;;

 error:
  *retval = ret;
  return COMMAND_END;
}

static enum command_state
bitrate_set(void *arg, int *retval)
{
  struct sp_cmdargs *cmdargs = arg;
  struct sp_session *session = cmdargs->session;
  int ret;

  if (cmdargs->bitrate == SP_BITRATE_ANY)
    cmdargs->bitrate = SP_BITRATE_DEFAULT;

  ret = session_check(session);
  if (ret < 0)
    RETURN_ERROR(SP_ERR_NOSESSION, "Session has disappeared, cannot set bitrate");

  session->bitrate_preferred = cmdargs->bitrate;

 error:
  *retval = ret;
  return COMMAND_END;
}

static enum command_state
credentials_get(void *arg, int *retval)
{
  struct sp_cmdargs *cmdargs = arg;
  struct sp_session *session = cmdargs->session;
  struct sp_credentials *credentials = cmdargs->credentials;
  int ret;

  ret = session_check(session);
  if (ret < 0)
    RETURN_ERROR(SP_ERR_NOSESSION, "Session has disappeared, cannot get credentials");

  memcpy(credentials, &session->credentials, sizeof(struct sp_credentials));

 error:
  *retval = ret;
  return COMMAND_END;
}


/* ------------------------------ Event loop -------------------------------- */

static void *
librespotc(void *arg)
{
  event_base_dispatch(sp_evbase);

  pthread_exit(NULL);
}


/* ---------------------------------- API ----------------------------------- */

int
librespotc_open(const char *path, struct sp_session *session)
{
  struct sp_cmdargs cmdargs = { 0 };

  cmdargs.session  = session;
  cmdargs.path     = path;

  return commands_exec_sync(sp_cmdbase, media_open, media_open_bh, &cmdargs);
}

int
librespotc_seek(int fd, size_t pos)
{
  struct sp_cmdargs cmdargs = { 0 };

  cmdargs.fd_read  = fd;
  cmdargs.seek_pos = pos;

  return commands_exec_sync(sp_cmdbase, track_seek, NULL, &cmdargs);
}

// Starts writing audio for the caller to read from the file descriptor
void
librespotc_write(int fd, sp_progress_cb progress_cb, void *cb_arg)
{
  struct sp_cmdargs *cmdargs;

  cmdargs = calloc(1, sizeof(struct sp_cmdargs));

  assert(cmdargs);

  cmdargs->fd_read     = fd;
  cmdargs->progress_cb = progress_cb;
  cmdargs->cb_arg      = cb_arg;

  commands_exec_async(sp_cmdbase, track_write, cmdargs);
}

int
librespotc_close(int fd)
{
  struct sp_cmdargs cmdargs = { 0 };

  cmdargs.fd_read  = fd;

  return commands_exec_sync(sp_cmdbase, track_pause, track_close, &cmdargs);
}

struct sp_session *
librespotc_login_password(const char *username, const char *password)
{
  struct sp_cmdargs cmdargs = { 0 };

  cmdargs.username = username;
  cmdargs.password = password;

  commands_exec_sync(sp_cmdbase, login, login_bh, &cmdargs);

  return cmdargs.session;
}

struct sp_session *
librespotc_login_stored_cred(const char *username, uint8_t *stored_cred, size_t stored_cred_len)
{
  struct sp_cmdargs cmdargs = { 0 };

  cmdargs.username        = username;
  cmdargs.stored_cred     = stored_cred;
  cmdargs.stored_cred_len = stored_cred_len;

  commands_exec_sync(sp_cmdbase, login, login_bh, &cmdargs);

  return cmdargs.session;
}

struct sp_session *
librespotc_login_token(const char *username, const char *token)
{
  struct sp_cmdargs cmdargs = { 0 };

  cmdargs.username        = username;
  cmdargs.token           = token;

  commands_exec_sync(sp_cmdbase, login, login_bh, &cmdargs);

  return cmdargs.session;
}

int
librespotc_logout(struct sp_session *session)
{
  struct sp_cmdargs cmdargs = { 0 };

  cmdargs.session         = session;

  return commands_exec_sync(sp_cmdbase, logout, NULL, &cmdargs);
}

int
librespotc_legacy_set(struct sp_session *session, int use_legacy)
{
  struct sp_cmdargs cmdargs = { 0 };

  cmdargs.session         = session;
  cmdargs.use_legacy      = use_legacy;

  return commands_exec_sync(sp_cmdbase, legacy_set, NULL, &cmdargs);
}

int
librespotc_metadata_get(struct sp_metadata *metadata, int fd)
{
  struct sp_cmdargs cmdargs = { 0 };

  cmdargs.metadata = metadata;
  cmdargs.fd_read  = fd;

  return commands_exec_sync(sp_cmdbase, metadata_get, NULL, &cmdargs);
}

int
librespotc_bitrate_set(struct sp_session *session, enum sp_bitrates bitrate)
{
  struct sp_cmdargs cmdargs = { 0 };

  cmdargs.session  = session;
  cmdargs.bitrate  = bitrate;

  return commands_exec_sync(sp_cmdbase, bitrate_set, NULL, &cmdargs);
}

int
librespotc_credentials_get(struct sp_credentials *credentials, struct sp_session *session)
{
  struct sp_cmdargs cmdargs = { 0 };

  cmdargs.credentials = credentials;
  cmdargs.session = session;

  return commands_exec_sync(sp_cmdbase, credentials_get, NULL, &cmdargs);
}

const char *
librespotc_last_errmsg(void)
{
  return sp_errmsg ? sp_errmsg : "(no error)";
}

static void
system_info_set(struct sp_sysinfo *si_out, struct sp_sysinfo *si_user)
{
  memcpy(si_out, si_user, sizeof(struct sp_sysinfo));

  if (si_out->client_name[0] == '\0')
    snprintf(si_out->client_name, sizeof(si_out->client_name), SP_CLIENT_NAME_DEFAULT);
  if (si_out->client_id[0] == '\0')
    snprintf(si_out->client_id, sizeof(si_out->client_id), SP_CLIENT_ID_DEFAULT);
  if (si_out->client_version[0] == '\0')
    snprintf(si_out->client_version, sizeof(si_out->client_version), SP_CLIENT_VERSION_DEFAULT);
  if (si_out->client_build_id[0] == '\0')
    snprintf(si_out->client_build_id, sizeof(si_out->client_build_id), SP_CLIENT_BUILD_ID_DEFAULT);
}

int
librespotc_init(struct sp_sysinfo *sysinfo, struct sp_callbacks *callbacks)
{
  int ret;

  if (sp_initialized)
    RETURN_ERROR(SP_ERR_INVALID, "librespot-c already initialized");

  ret = seq_requests_check();
  if (ret < 0)
    RETURN_ERROR(SP_ERR_INVALID, "Bug! Misalignment between enum seq_type and seq_requests");

  sp_cb     = *callbacks;

  system_info_set(&sp_sysinfo, sysinfo);

  sp_evbase = event_base_new();
  if (!sp_evbase)
    RETURN_ERROR(SP_ERR_OOM, "event_base_new() failed");

  sp_cmdbase = commands_base_new(sp_evbase, NULL);
  if (!sp_cmdbase)
    RETURN_ERROR(SP_ERR_OOM, "commands_base_new() failed");

  ret = pthread_create(&sp_tid, NULL, librespotc, NULL);
  if (ret < 0)
    RETURN_ERROR(SP_ERR_OOM, "Could not start thread");

  if (sp_cb.thread_name_set)
    sp_cb.thread_name_set(sp_tid);

  sp_initialized = true;
  return 0;

 error:
  librespotc_deinit();
  return ret;
}

void
librespotc_deinit()
{
  struct sp_session *session;

  if (sp_cmdbase)
    {
      commands_base_destroy(sp_cmdbase);
      sp_cmdbase = NULL;
    }

  for (session = sp_sessions; sp_sessions; session = sp_sessions)
    {
      sp_sessions = session->next;
      session_free(session);
    }

  if (sp_tid)
    {
      pthread_join(sp_tid, NULL);
    }

  if (sp_evbase)
    {
      event_base_free(sp_evbase);
      sp_evbase = NULL;
    }

  sp_initialized = false;
  memset(&sp_cb, 0, sizeof(struct sp_callbacks));

  return;
}
