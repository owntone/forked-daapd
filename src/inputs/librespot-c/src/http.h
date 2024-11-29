#ifndef __HTTP_H__
#define __HTTP_H__

#include <stdbool.h>
#include <stdint.h>

#define HTTP_MAX_HEADERS 32

/* Response codes from event2/http.h with 206 added */
#define HTTP_CONTINUE          100	/**< client should proceed to send */
#define HTTP_SWITCH_PROTOCOLS  101	/**< switching to another protocol */
#define HTTP_PROCESSING        102	/**< processing the request, but no response is available yet */
#define HTTP_EARLYHINTS        103	/**< return some response headers */
#define HTTP_OK                200	/**< request completed ok */
#define HTTP_CREATED           201	/**< new resource is created */
#define HTTP_ACCEPTED          202	/**< accepted for processing */
#define HTTP_NONAUTHORITATIVE  203	/**< returning a modified version of the origin's response */
#define HTTP_NOCONTENT         204	/**< request does not have content */
#define HTTP_PARTIALCONTENT    206	/**< partial content returned*/
#define HTTP_MOVEPERM          301	/**< the uri moved permanently */
#define HTTP_MOVETEMP          302	/**< the uri moved temporarily */
#define HTTP_NOTMODIFIED       304	/**< page was not modified from last */
#define HTTP_BADREQUEST        400	/**< invalid http request was made */
#define HTTP_UNAUTHORIZED      401	/**< authentication is required */
#define HTTP_PAYMENTREQUIRED   402	/**< user exceeded limit on requests */
#define HTTP_FORBIDDEN         403	/**< user not having the necessary permissions */
#define HTTP_NOTFOUND          404	/**< could not find content for uri */
#define HTTP_BADMETHOD         405 	/**< method not allowed for this uri */
#define HTTP_ENTITYTOOLARGE    413	/**< request is larger than the server is able to process */
#define HTTP_EXPECTATIONFAILED 417	/**< we can't handle this expectation */
#define HTTP_INTERNAL          500     /**< internal error */
#define HTTP_NOTIMPLEMENTED    501     /**< not implemented */
#define HTTP_BADGATEWAY        502	/**< received an invalid response from the upstream */
#define HTTP_SERVUNAVAIL       503	/**< the server is not available */

struct http_session
{
  void *internal;
};

struct http_request
{
  char *url;

  const char *user_agent;
  bool headers_only; // HEAD request
  bool ssl_verify_peer;

  char *headers[HTTP_MAX_HEADERS];
  uint8_t *body; // If not NULL and body_len > 0 -> POST request
  size_t body_len;
};

struct http_response
{
  int code;
  ssize_t content_length; // -1 = unknown

  char *headers[HTTP_MAX_HEADERS];
  uint8_t *body; // Allocated, must be freed by caller
  size_t body_len;
};

void
http_session_init(struct http_session *session);

void
http_session_deinit(struct http_session *session);

void
http_request_free(struct http_request *request, bool only_content);

void
http_response_free(struct http_response *response, bool only_content);

// The session is optional but increases performance when making many requests.
int
http_request(struct http_response *response, struct http_request *request, struct http_session *session);

char *
http_response_header_find(const char *key, struct http_response *response);

#endif /* !__HTTP_H__ */
