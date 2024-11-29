#define _GNU_SOURCE // For asprintf and vasprintf
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> // strncasecmp
#include <limits.h>
#include <sys/param.h>
#include <sys/types.h>
#include <errno.h>

#include <event2/event.h>
#include <curl/curl.h>

#include "http.h"

// Number of seconds the client will wait for a response before aborting
#define HTTP_CLIENT_TIMEOUT 8

void
http_session_init(struct http_session *session)
{
  session->internal = curl_easy_init();
}

void
http_session_deinit(struct http_session *session)
{
  curl_easy_cleanup(session->internal);
}

void
http_request_free(struct http_request *request, bool only_content)
{
  int i;

  if (!request)
    return;

  free(request->url);
  free(request->body);

  for (i = 0; request->headers[i]; i++)
    free(request->headers[i]);

  if (only_content)
    memset(request, 0, sizeof(struct http_request));
  else
    free(request);
}

void
http_response_free(struct http_response *response, bool only_content)
{
  int i;

  if (!response)
    return;

  free(response->body);

  for (i = 0; response->headers[i]; i++)
    free(response->headers[i]);

  if (only_content)
    memset(response, 0, sizeof(struct http_response));
  else
    free(response);
}

static void
headers_save(struct http_response *response, CURL *curl)
{
  struct curl_header *prev = NULL;
  struct curl_header *header;
  int i = 0;
  
  while ((header = curl_easy_nextheader(curl, CURLH_HEADER, 0, prev)) && i < HTTP_MAX_HEADERS)
    {
      if (asprintf(&response->headers[i], "%s:%s", header->name, header->value) < 0)
	return;

      prev = header;
      i++;
    }
 }

static size_t
body_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
  struct http_response *response = userdata;
  size_t realsize = size * nmemb;
  size_t new_size;
  uint8_t *new;

  if (realsize == 0)
    {
      return 0;
    }

  // Make sure the size is +1 larger than needed so we can zero terminate for safety
  new_size = response->body_len + realsize + 1;
  new = realloc(response->body, new_size);
  if (!new)
    {
      free(response->body);
      response->body = NULL;
      response->body_len = 0;
      return 0;
    }

  memcpy(new + response->body_len, ptr, realsize);
  response->body_len += realsize;

  memset(new + response->body_len, 0, 1); // Zero terminate in case we need to address as C string
  response->body = new;
  return nmemb;
}

int
http_request(struct http_response *response, struct http_request *request, struct http_session *session)
{
  CURL *curl;
  CURLcode res;
  struct curl_slist *headers = NULL;
  long response_code;
  long opt;
  curl_off_t content_length;
  int i;

  if (session)
    {
      curl = session->internal;
      curl_easy_reset(curl);
    }
  else
    {
      curl = curl_easy_init();
    }
  if (!curl)
    return -1;

  memset(response, 0, sizeof(struct http_response));

  curl_easy_setopt(curl, CURLOPT_URL, request->url);

  // Set optional params
  if (request->user_agent)
    curl_easy_setopt(curl, CURLOPT_USERAGENT, request->user_agent);
  for (i = 0; i < HTTP_MAX_HEADERS && request->headers[i]; i++)
    headers = curl_slist_append(headers, request->headers[i]);
  if (headers)
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  if ((opt = request->ssl_verify_peer))
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, opt);

  if (request->headers_only)
    {
      curl_easy_setopt(curl, CURLOPT_NOBODY, 1L); // Makes curl make a HEAD request
    }
  else if (request->body && request->body_len > 0)
    {
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request->body);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, request->body_len);
    }

  curl_easy_setopt(curl, CURLOPT_TIMEOUT, HTTP_CLIENT_TIMEOUT);

  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, body_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);

  // Allow redirects
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5);

  res = curl_easy_perform(curl);
  if (res != CURLE_OK)
    goto error;

  res = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
  response->code = (res == CURLE_OK) ? (int) response_code : -1;

  res = curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &content_length);
  response->content_length = (res == CURLE_OK) ? (ssize_t)content_length : -1;

  headers_save(response, curl);

  curl_slist_free_all(headers);
  if (!session)
    curl_easy_cleanup(curl);

  return 0;

 error:
  curl_slist_free_all(headers);
  if (!session)
    curl_easy_cleanup(curl);

  return -1;
}

char *
http_response_header_find(const char *key, struct http_response *response)
{
  char **header;
  size_t key_len;

  key_len = strlen(key);

  for (header = response->headers; *header; header++)
    {
      if (strncasecmp(key, *header, key_len) == 0 && (*header)[key_len] == ':')
        return *header + key_len + 1;
    }

  return NULL;
}
