/*
 * $Id$
 *
 * Copyright (c) 1998-2008 John Morrison.
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
 */


/*********************************************************
*Name:          http
*Filename:      http.c
*Author:        John Morrison
*Creation Date: 16/9/01
*Last Modified: 30/3/26
*Purpose:
*  Responsible for HTTP/HTTPS communication with the
*  WinBolo.net JSON REST API via libcurl.
*
*  INI [WINBOLO.NET] Host= accepts:
*    wbn.winbolo.net          -> https://wbn.winbolo.net  (default)
*    http://wbn.winbolo.net   -> http://wbn.winbolo.net
*    https://wbn.winbolo.net  -> https://wbn.winbolo.net
*********************************************************/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <curl/curl.h>
#include <sodium.h>
#include "cJSON.h"
#include "wbn_signing_key.h"

#ifdef _WIN32
  #include "../gui/gamefront.h"   /* PREFERENCE_FILE */
#else
  /* Provided by posix_stubs.c (server) or platform_config.c (client) */
  void preferencesGetPreferenceFile(char *dest);
  unsigned int GetPrivateProfileString(const char *section, const char *key,
                                       const char *def, char *out,
                                       unsigned int outSize,
                                       const char *filePath);
  int WritePrivateProfileString(const char *section, const char *key,
                                const char *value, const char *filePath);
#endif

#include "../bolo/global.h"
#include "winbolonet.h"
#include "http.h"

static bool httpStarted = false;
static char wbnHostString[FILENAME_MAX]; /* hostname only, no scheme */
static char wbnBaseUrl[FILENAME_MAX];    /* full base URL, e.g. https://wbn.winbolo.net */
static char altIpAddress[FILENAME_MAX];

/*********************************************************
*NAME:          buildBaseUrl
*PURPOSE:
* Parses the INI host value into wbnHostString (bare hostname)
* and wbnBaseUrl (scheme + hostname, no trailing slash).
*
* Accepted INI formats:
*   wbn.winbolo.net           -> https://wbn.winbolo.net
*   http://wbn.winbolo.net    -> http://wbn.winbolo.net
*   https://wbn.winbolo.net   -> https://wbn.winbolo.net
*********************************************************/
static void buildBaseUrl(const char *iniValue) {
  const char *host;

  if (strncmp(iniValue, "https://", 8) == 0) {
    snprintf(wbnBaseUrl, sizeof(wbnBaseUrl), "%s", iniValue);
    host = iniValue + 8;
  } else if (strncmp(iniValue, "http://", 7) == 0) {
    snprintf(wbnBaseUrl, sizeof(wbnBaseUrl), "%s", iniValue);
    host = iniValue + 7;
  } else {
    /* No scheme — default to HTTPS */
    snprintf(wbnBaseUrl, sizeof(wbnBaseUrl), "https://%s", iniValue);
    host = iniValue;
  }

  /* Strip trailing slash from base URL */
  size_t len = strlen(wbnBaseUrl);
  if (len > 0 && wbnBaseUrl[len - 1] == '/') {
    wbnBaseUrl[len - 1] = '\0';
  }

  /* Store bare hostname (stop at any path separator) */
  snprintf(wbnHostString, sizeof(wbnHostString), "%s", host);
  char *slash = strchr(wbnHostString, '/');
  if (slash) *slash = '\0';
}

/*********************************************************
*NAME:          writeCallback
*PURPOSE:
* libcurl write callback for fixed-size buffers (used by
* httpSendLogFile). Appends received data, never exceeding
* the buffer capacity.
*********************************************************/
typedef struct {
  BYTE *buf;
  int   pos;
  int   maxSize;
} WriteCtx;

static size_t writeCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
  WriteCtx *ctx = (WriteCtx *)userdata;
  size_t incoming = size * nmemb;
  size_t space    = (size_t)(ctx->maxSize - ctx->pos);
  size_t toCopy   = incoming < space ? incoming : space;
  if (toCopy > 0) {
    memcpy(ctx->buf + ctx->pos, ptr, toCopy);
    ctx->pos += (int)toCopy;
  }
  return incoming;
}

/*********************************************************
*NAME:          DynBuf / dynWriteCallback
*PURPOSE:
* Dynamic growing buffer and libcurl write callback for
* JSON API responses.
*********************************************************/
typedef struct {
  char  *data;
  size_t size;
  size_t capacity;
} DynBuf;

static void dynBufInit(DynBuf *buf) {
  buf->capacity = 1024;
  buf->size = 0;
  buf->data = malloc(buf->capacity);
  if (buf->data) {
    buf->data[0] = '\0';
  }
}

static size_t dynWriteCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
  DynBuf *buf = (DynBuf *)userdata;
  size_t incoming = size * nmemb;
  size_t needed = buf->size + incoming + 1;
  if (needed > buf->capacity) {
    size_t newcap = buf->capacity * 2;
    if (newcap < needed) newcap = needed;
    char *tmp = realloc(buf->data, newcap);
    if (!tmp) return 0;
    buf->data = tmp;
    buf->capacity = newcap;
  }
  memcpy(buf->data + buf->size, ptr, incoming);
  buf->size += incoming;
  buf->data[buf->size] = '\0';
  return incoming;
}

/*********************************************************
*NAME:          httpCreate
*PURPOSE:
* Initialises the http module and libcurl global state.
* Reads [WINBOLO.NET] Host from the INI file.
* Returns success.
*********************************************************/
bool httpCreate(void) {
  char prefs[FILENAME_MAX];
  char iniValue[FILENAME_MAX];

#ifdef _WIN32
  strcpy(prefs, PREFERENCE_FILE);
#else
  preferencesGetPreferenceFile(prefs);
#endif

  iniValue[0] = '\0';
  GetPrivateProfileString("WINBOLO.NET", "Host", "wbn.winbolo.net",
                          iniValue, (unsigned int)sizeof(iniValue), prefs);
  /* Write back so servers without a client config get a default entry */
  WritePrivateProfileString("WINBOLO.NET", "Host", iniValue, prefs);

  buildBaseUrl(iniValue);

  altIpAddress[0] = '\0';

  if (sodium_init() < 0) {
    fprintf(stderr, "WinBolo.net: sodium_init failed\n");
    httpStarted = false;
    return false;
  }

  CURLcode res = curl_global_init(CURL_GLOBAL_ALL);
  if (res != CURLE_OK) {
    fprintf(stderr, "WinBolo.net: curl_global_init failed: %s\n",
            curl_easy_strerror(res));
    httpStarted = false;
    return false;
  }

  httpStarted = true;
  return true;
}

/*********************************************************
*NAME:          httpDestroy
*PURPOSE:
* Destroys the http module and releases libcurl global state.
*********************************************************/
void httpDestroy(void) {
  if (httpStarted) {
    curl_global_cleanup();
    httpStarted = false;
  }
}

/*********************************************************
*NAME:          wbn_sign_request
*PURPOSE:
* Produces a hex-encoded Ed25519 signature over the
* concatenation of timestamp_str and json_body.
* sig_hex_out must be at least 129 bytes.
*********************************************************/
static void wbn_sign_request(const char *timestamp_str, const char *json_body, char *sig_hex_out) {
  size_t ts_len   = strlen(timestamp_str);
  size_t body_len = strlen(json_body);
  size_t msg_len  = ts_len + body_len;

  unsigned char *message = malloc(msg_len);
  memcpy(message, timestamp_str, ts_len);
  memcpy(message + ts_len, json_body, body_len);

  unsigned char sig[crypto_sign_BYTES];
  crypto_sign_detached(sig, NULL, message, msg_len, WBN_SIGNING_KEY);

  sodium_bin2hex(sig_hex_out, 129, sig, crypto_sign_BYTES);

  free(message);
}

/*********************************************************
*NAME:          wbn_api_post
*PURPOSE:
* Low-level POST of a JSON string to a WinBolo.net API
* endpoint. Builds the full URL as <baseUrl>/api/v1/<endpoint>.
* Returns the HTTP status code, or -1 on transport error.
*********************************************************/
int wbn_api_post(const char *endpoint, const char *json_body, char **response_out) {
  if (response_out) *response_out = NULL;
  if (!httpStarted) return -1;

  CURL *curl = curl_easy_init();
  if (!curl) return -1;

  /* Build URL: <baseUrl>/api/v1/<endpoint> */
  char url[FILENAME_MAX];
  snprintf(url, sizeof(url), "%s/api/v1/%s", wbnBaseUrl, endpoint);

  /* Generate timestamp and Ed25519 signature */
  char timestamp_str[32];
  snprintf(timestamp_str, sizeof(timestamp_str), "%ld", (long)time(NULL));

  char sig_hex[129];
  wbn_sign_request(timestamp_str, json_body, sig_hex);

  char sig_header[256];
  char ts_header[64];
  snprintf(sig_header, sizeof(sig_header), "X-WBN-Signature: %s", sig_hex);
  snprintf(ts_header, sizeof(ts_header), "X-WBN-Timestamp: %s", timestamp_str);

  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, sig_header);
  headers = curl_slist_append(headers, ts_header);

  DynBuf respBuf;
  dynBufInit(&respBuf);
  if (!respBuf.data) {
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return -1;
  }

  curl_easy_setopt(curl, CURLOPT_URL,            url);
  curl_easy_setopt(curl, CURLOPT_POST,           1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     json_body);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  dynWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &respBuf);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  if (altIpAddress[0] != '\0') {
    curl_easy_setopt(curl, CURLOPT_INTERFACE, altIpAddress);
  }

  CURLcode res = curl_easy_perform(curl);

  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    fprintf(stderr, "WinBolo.net wbn_api_post [%s]: %s\n", endpoint, curl_easy_strerror(res));
    free(respBuf.data);
    return -1;
  }

  if (response_out) {
    *response_out = respBuf.data;
  } else {
    free(respBuf.data);
  }
  return (int)http_code;
}

/*********************************************************
*NAME:          wbn_api_call
*PURPOSE:
* High-level JSON API call. Serializes the cJSON body,
* POSTs it, and parses the JSON response.
* Returns the HTTP status code, or -1 on error.
*********************************************************/
int wbn_api_call(const char *endpoint, cJSON *body, cJSON **response) {
  if (response) *response = NULL;

  char *json_str = cJSON_PrintUnformatted(body);
  if (!json_str) return -1;

  char *resp_str = NULL;
  int status = wbn_api_post(endpoint, json_str, &resp_str);
  free(json_str);

  if (resp_str && response) {
    *response = cJSON_Parse(resp_str);
    if (!*response) {
      fprintf(stderr, "WinBolo.net: failed to parse JSON response from %s\n", endpoint);
    }
  }
  free(resp_str);

  return status;
}

/*********************************************************
*NAME:          httpSendLogFile
*PURPOSE:
* Uploads a log file to WinBolo.net via HTTP(S) multipart
* POST.  Returns TRUE on success.
*********************************************************/
bool httpSendLogFile(char *fileName, char *key, bool wantFeedback) {
  (void)wantFeedback;

  if (!httpStarted || fileName == NULL || key == NULL) return false;

  /* Build: <baseUrl>/log.php?key=<key> */
  char url[FILENAME_MAX + 64];
  snprintf(url, sizeof(url), "%s/log.php?key=%s", wbnBaseUrl, key);

  CURL *curl = curl_easy_init();
  if (!curl) return false;

  curl_mime     *mime = curl_mime_init(curl);
  curl_mimepart *part = curl_mime_addpart(mime);
  curl_mime_name(part,     "logfile");
  curl_mime_filedata(part, fileName);
  curl_mime_filename(part, "log.dat");
  curl_mime_type(part,     "application/octet-stream");

  /* Discard response body */
  char respBuf[1024];
  WriteCtx ctx = { (BYTE *)respBuf, 0, (int)sizeof(respBuf) };

  curl_easy_setopt(curl, CURLOPT_URL,            url);
  curl_easy_setopt(curl, CURLOPT_MIMEPOST,       mime);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  writeCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &ctx);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT,        60L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  if (altIpAddress[0] != '\0') {
    curl_easy_setopt(curl, CURLOPT_INTERFACE, altIpAddress);
  }

  CURLcode res = curl_easy_perform(curl);
  curl_mime_free(mime);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    fprintf(stderr, "WinBolo.net httpSendLogFile: %s\n", curl_easy_strerror(res));
    return false;
  }
  return true;
}

/*********************************************************
*NAME:          httpSetAltIpAddress
*PURPOSE:
* Sets the alternate local interface/IP address that
* libcurl will bind to when connecting (CURLOPT_INTERFACE).
*********************************************************/
void httpSetAltIpAddress(char *iptoset) {
  strncpy(altIpAddress, iptoset, sizeof(altIpAddress) - 1);
  altIpAddress[sizeof(altIpAddress) - 1] = '\0';
}
