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
*Last Modified: 10/3/26
*Purpose:
*  Responsible for sending/receiving HTTP/HTTPS messages
*  to WinBolo.net via libcurl (cross-platform, TLS capable).
*
*  INI [WINBOLO.NET] Host= accepts:
*    wbn.winbolo.net          -> https://wbn.winbolo.net  (default)
*    http://wbn.winbolo.net   -> http://wbn.winbolo.net
*    https://wbn.winbolo.net  -> https://wbn.winbolo.net
*********************************************************/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <curl/curl.h>

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
* libcurl write callback — appends received data into a
* caller-supplied fixed-size buffer. Always returns the
* full incoming byte count so libcurl does not abort.
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
  return incoming; /* must return full count — libcurl aborts on mismatch */
}

/*********************************************************
*NAME:          httpCreate
*AUTHOR:        John Morrison
*CREATION DATE: 16/9/01
*LAST MODIFIED: 10/3/26
*PURPOSE:
* Initialises the http module and libcurl global state.
* Reads [WINBOLO.NET] Host from the INI file.
* Returns success.
*
*ARGUMENTS:
*
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
*AUTHOR:        John Morrison
*CREATION DATE: 16/9/01
*LAST MODIFIED: 10/3/26
*PURPOSE:
* Destroys the http module and releases libcurl global state.
*
*ARGUMENTS:
*
*********************************************************/
void httpDestroy(void) {
  if (httpStarted) {
    curl_global_cleanup();
    httpStarted = false;
  }
}

/*********************************************************
*NAME:          httpSendMessage
*AUTHOR:        John Morrison
*CREATION DATE: 16/9/01
*LAST MODIFIED: 10/3/26
*PURPOSE:
* Sends a binary message to the WinBolo.net server via
* HTTP(S) GET and returns the decoded response length,
* or -1 on error.
*
* The binary message is URL-encoded by libcurl and appended
* as the "data" query parameter:
*   <baseUrl>/wbn.php?data=<encoded>
*
*ARGUMENTS:
* message - The binary message to send
* len     - Length of the message in bytes
* response - Buffer to receive the response body
* maxSize  - Capacity of the response buffer
*********************************************************/
int httpSendMessage(BYTE *message, int len, BYTE *response, int maxSize) {
  if (!httpStarted) return -1;

  CURL *curl = curl_easy_init();
  if (!curl) return -1;

  char *encoded = curl_easy_escape(curl, (char *)message, len);
  if (!encoded) {
    curl_easy_cleanup(curl);
    return -1;
  }

  /* Build: <baseUrl>/wbn.php?data=<encoded> */
  size_t urlLen = strlen(wbnBaseUrl) + strlen(encoded) + 32;
  char *url = malloc(urlLen);
  if (!url) {
    curl_free(encoded);
    curl_easy_cleanup(curl);
    return -1;
  }
  snprintf(url, urlLen, "%s/wbn.php?data=%s", wbnBaseUrl, encoded);
  curl_free(encoded);

  WriteCtx ctx = { response, 0, maxSize };
  curl_easy_setopt(curl, CURLOPT_URL,           url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &ctx);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT,       30L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  if (altIpAddress[0] != '\0') {
    curl_easy_setopt(curl, CURLOPT_INTERFACE, altIpAddress);
  }

  CURLcode res = curl_easy_perform(curl);
  free(url);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    fprintf(stderr, "WinBolo.net httpSendMessage: %s\n", curl_easy_strerror(res));
    return -1;
  }
  return ctx.pos;
}

/*********************************************************
*NAME:          httpSendLogFile
*AUTHOR:        John Morrison
*CREATION DATE: 16/9/01
*LAST MODIFIED: 10/3/26
*PURPOSE:
* Uploads a log file to the WinBolo.net server via
* HTTP(S) multipart POST.  Returns TRUE on success.
*
*ARGUMENTS:
* fileName     - Path to the log file to upload
* key          - WINBOLONET_KEY_LEN-byte session key
* wantFeedback - (unused, retained for API compatibility)
*********************************************************/
bool httpSendLogFile(char *fileName, BYTE *key, bool wantFeedback) {
  (void)wantFeedback;

  if (!httpStarted || fileName == NULL || key == NULL) return false;

  char sKey[WINBOLONET_KEY_LEN + 1];
  memset(sKey, 0, sizeof(sKey));
  memcpy(sKey, key, WINBOLONET_KEY_LEN);

  /* Build: <baseUrl>/log.php?key=<sKey> */
  char url[FILENAME_MAX + 64];
  snprintf(url, sizeof(url), "%s/log.php?key=%s", wbnBaseUrl, sKey);

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
*AUTHOR:        Minhiriath
*CREATION DATE: 14/3/2009
*LAST MODIFIED: 10/3/26
*PURPOSE:
* Sets the alternate local interface/IP address that
* libcurl will bind to when connecting (CURLOPT_INTERFACE).
*
*ARGUMENTS:
* iptoset - Interface name, IP, or hostname to bind to
*********************************************************/
void httpSetAltIpAddress(char *iptoset) {
  strncpy(altIpAddress, iptoset, sizeof(altIpAddress) - 1);
  altIpAddress[sizeof(altIpAddress) - 1] = '\0';
}
