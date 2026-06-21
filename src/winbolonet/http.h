/*
 * $Id$
 *
 * Copyright (c) 1998-2026 John Morrison.
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
*Filename:      http.h
*Author:        John Morrison
*Creation Date: 16/9/01
*Last Modified: 30/3/26
*Purpose:
*  Responsible for HTTP/HTTPS communication with the
*  WinBolo.net JSON REST API via libcurl.
*
*  [WINBOLO.NET] Host= in the INI file accepts:
*    hostname             (implies https://)
*    http://hostname
*    https://hostname
*********************************************************/

#ifndef __HTTP_H
#define __HTTP_H

#include "global.h"

struct cJSON;

/*********************************************************
*NAME:          httpSetHostOverride
*PURPOSE:
* Sets a command-line override for the WBN host. When set,
* httpCreate() will use this value instead of reading from
* the preferences file. Pass a bare hostname (defaults to
* https://), or include http:// or https:// scheme.
*
*ARGUMENTS:
* host - Host value to use
*********************************************************/
void httpSetHostOverride(const char *host);

/*********************************************************
*NAME:          httpCreate
*PURPOSE:
* Initialises the http module.  Uses the command-line host
* override if set, otherwise reads from the [WINBOLO.NET]
* Host INI key.  Initialises libcurl.
* Returns TRUE on success.
*********************************************************/
bool httpCreate(void);

/*********************************************************
*NAME:          httpDestroy
*PURPOSE:
* Destroys the http module and releases libcurl resources.
*********************************************************/
void httpDestroy(void);

/*********************************************************
*NAME:          wbn_api_post
*PURPOSE:
* Low-level POST of a JSON string to a WinBolo.net API
* endpoint. Builds the full URL as <baseUrl>/api/v1/<endpoint>.
* Returns the HTTP status code, or -1 on transport error.
* On success, *response_out is a heap-allocated string that
* the caller must free. On error, *response_out may be NULL.
*
*ARGUMENTS:
* endpoint     - API path after /api/v1/ (e.g. "server/register")
* json_body    - JSON request body string
* response_out - Receives heap-allocated response string (caller frees)
*********************************************************/
int wbn_api_post(const char *endpoint, const char *json_body, char **response_out);

/*********************************************************
*NAME:          wbn_api_post_server
*PURPOSE:
* Server-scoped low-level POST. Same as wbn_api_post plus
* an Authorization: Bearer <token> header sourced from the
* in-memory bearer (set after POST server/register).
* Refuses to send (returns -1) and logs to stderr when the
* bearer is empty; no curl is invoked in that case.
*
*ARGUMENTS:
* endpoint     - API path after /api/v1/
* json_body    - JSON request body string
* response_out - Receives heap-allocated response string (caller frees)
*********************************************************/
int wbn_api_post_server(const char *endpoint, const char *json_body, char **response_out);

/*********************************************************
*NAME:          wbn_api_call
*PURPOSE:
* High-level JSON API call. Serializes the cJSON body,
* POSTs it to the endpoint, and parses the response.
* Returns the HTTP status code, or -1 on transport error.
* On success, *response is a parsed cJSON object that the
* caller must free with cJSON_Delete().
*
*ARGUMENTS:
* endpoint - API path after /api/v1/ (e.g. "server/register")
* body     - cJSON object for the request body
* response - Receives parsed cJSON response (caller frees)
*********************************************************/
int wbn_api_call(const char *endpoint, struct cJSON *body, struct cJSON **response);

/*********************************************************
*NAME:          wbn_api_call_server
*PURPOSE:
* Server-scoped high-level JSON API call. Same shape as
* wbn_api_call but attaches Authorization: Bearer using the
* stored server bearer. Returns -1 without invoking curl
* when the bearer is empty (no *response allocated).
*
*ARGUMENTS:
* endpoint - API path after /api/v1/
* body     - cJSON object for the request body
* response - Receives parsed cJSON response (caller frees)
*********************************************************/
int wbn_api_call_server(const char *endpoint, struct cJSON *body, struct cJSON **response);

/*********************************************************
*NAME:          httpSendLogFile
*PURPOSE:
* Uploads a log file to WinBolo.net via HTTP(S) multipart
* POST.  Returns TRUE on success.
*
*ARGUMENTS:
* fileName     - Path to the log file
* key          - Null-terminated session key string
* wantFeedback - (unused, retained for API compatibility)
*********************************************************/
bool httpSendLogFile(char *fileName, char *key, bool wantFeedback);

/*********************************************************
*NAME:          wbn_api_get
*PURPOSE:
* Low-level GET request to a WinBolo.net API endpoint.
* Builds the full URL as <baseUrl>/api/v1/<path>.
* path may include query parameters (e.g. "logs/recent?limit=20").
* Returns the HTTP status code, or -1 on transport error.
* On success, *response_out is a heap-allocated string that
* the caller must free. On error, *response_out may be NULL.
*
*ARGUMENTS:
* path         - API path after /api/v1/ (e.g. "logs/recent?limit=20")
* response_out - Receives heap-allocated response string (caller frees)
*********************************************************/
int wbn_api_get(const char *path, char **response_out);

/*********************************************************
*NAME:          wbn_api_get_cancellable
*PURPOSE:
* Like wbn_api_get, but the caller may abort the request
* mid-flight by setting *cancel_flag to non-zero (treat as
* volatile/atomic). On cancellation the partial response is
* freed and -2 is returned. Pass cancel_flag = NULL for no
* cancellation (identical to wbn_api_get).
*
*ARGUMENTS:
* path         - API path after /api/v1/ (e.g. "logs/recent?limit=20")
* response_out - Receives heap-allocated response string (caller frees)
* cancel_flag  - Pointer to an int polled during transfer
*                (NULL = no cancellation)
*********************************************************/
int wbn_api_get_cancellable(const char *path, char **response_out,
                            volatile int *cancel_flag);

/*********************************************************
*NAME:          wbn_api_get_public
*PURPOSE:
* Like wbn_api_get, but issues an unsigned GET: no
* X-WBN-Signature / X-WBN-Timestamp headers are sent. For
* public endpoints (e.g. the games list) that take no Ed25519
* signature. Builds the full URL as <baseUrl>/api/v1/<path>
* and otherwise behaves like wbn_api_get (30s timeout, follows
* redirects, returns the HTTP status code or -1 on transport
* error; *response_out is a heap-allocated string the caller
* must free, and may be NULL on error).
*
*ARGUMENTS:
* path         - API path after /api/v1/ (e.g. "games")
* response_out - Receives heap-allocated response string (caller frees)
*********************************************************/
int wbn_api_get_public(const char *path, char **response_out);

/*********************************************************
*NAME:          wbn_prefs_get
*PURPOSE:
* GET /api/v1/prefs for the cloud preferences sync. Signs the
* timestamp + an empty body like the other v1 calls and sends
* Authorization: Bearer <bearerToken> (the user WBN token).
* Returns the HTTP status code, or -1 on transport error or
* when bearerToken is NULL/empty.
* On success, *response_out is a heap-allocated response string
* the caller must free. On error, *response_out may be NULL.
*
*ARGUMENTS:
* bearerToken  - User WBN bearer token (must be non-empty)
* response_out - Receives heap-allocated response string (caller frees)
*********************************************************/
int wbn_prefs_get(const char *bearerToken, char **response_out);

/*********************************************************
*NAME:          wbn_prefs_put
*PURPOSE:
* PUT /api/v1/prefs with json_body for the cloud preferences
* sync. Signs the timestamp + json_body and sends
* Authorization: Bearer <bearerToken> (the user WBN token).
* Returns the HTTP status code, or -1 on transport error or
* when bearerToken is NULL/empty.
* On success, *response_out is a heap-allocated response string
* the caller must free. On error, *response_out may be NULL.
*
*ARGUMENTS:
* bearerToken  - User WBN bearer token (must be non-empty)
* json_body    - JSON request body string
* response_out - Receives heap-allocated response string (caller frees)
*********************************************************/
int wbn_prefs_put(const char *bearerToken, const char *json_body,
                  char **response_out);

/*********************************************************
*NAME:          httpGetBaseUrl
*PURPOSE:
* Returns the configured WBN base URL (no trailing slash,
* e.g. "https://wbn.winbolo.net"). Returns an empty string
* before httpCreate() has succeeded.
*********************************************************/
const char *httpGetBaseUrl(void);

/*********************************************************
*NAME:          WbnProgressFn
*PURPOSE:
* Progress callback invoked periodically during a download.
* dlNow is the number of bytes transferred so far; dlTotal
* is the total expected size, or 0 when the server did not
* advertise a Content-Length. Called on the download thread,
* so implementations must be thread-safe.
*
*ARGUMENTS:
* userData - Opaque pointer passed through from the caller
* dlNow    - Bytes received so far
* dlTotal  - Total bytes expected (0 if unknown)
*********************************************************/
typedef void (*WbnProgressFn)(void *userData, int64_t dlNow, int64_t dlTotal);

/*********************************************************
*NAME:          wbn_api_download
*PURPOSE:
* Downloads a file from WBN to disk. Builds the full URL
* as <baseUrl>/api/v1/<path>.
* Returns the HTTP status code, or -1 on transport error.
*
*ARGUMENTS:
* path      - API path after /api/v1/ (e.g. "logs/<key>/download")
* dest_path - Local filesystem path to write the file to
*********************************************************/
int wbn_api_download(const char *path, const char *dest_path);

/*********************************************************
*NAME:          wbn_api_download_progress
*PURPOSE:
* Like wbn_api_download, but reports transfer progress via
* progressFn as bytes arrive. Pass progressFn = NULL for no
* reporting (identical to wbn_api_download).
*
* The caller may abort the transfer mid-flight by setting
* *cancel_flag to non-zero (treat as volatile/atomic);
* in that case the partial file is removed and -2 returned.
*
*ARGUMENTS:
* path             - API path after /api/v1/
* dest_path        - Local filesystem path to write the file to
* progressFn       - Progress callback, or NULL
* progressUserData - Opaque pointer passed to progressFn
* cancel_flag      - Pointer to an int polled during transfer
*                    (NULL = no cancellation)
*********************************************************/
int wbn_api_download_progress(const char *path, const char *dest_path,
                              WbnProgressFn progressFn, void *progressUserData,
                              volatile int *cancel_flag);

/*********************************************************
*NAME:          wbn_api_download_to_memory
*PURPOSE:
* Downloads a file from WBN into a heap-allocated memory
* buffer. Builds the full URL as <baseUrl>/api/v1/<path>.
* Returns the HTTP status code, or -1 on transport error.
* On success, *data_out receives the buffer and *size_out
* receives its length. Caller must free *data_out.
*
*ARGUMENTS:
* path     - API path after /api/v1/ (e.g. "logs/<key>/download")
* data_out - Receives heap-allocated buffer (caller frees)
* size_out - Receives buffer size in bytes
*********************************************************/
int wbn_api_download_to_memory(const char *path, uint8_t **data_out, size_t *size_out);

/*********************************************************
*NAME:          wbn_api_download_to_memory_progress
*PURPOSE:
* Like wbn_api_download_to_memory, but reports transfer
* progress via progressFn as bytes arrive. Pass progressFn
* = NULL for no reporting.
*
* The caller may abort the transfer mid-flight by setting
* *cancel_flag to non-zero (treat as volatile/atomic);
* in that case any partial buffer is freed and -2 returned.
*
*ARGUMENTS:
* path             - API path after /api/v1/
* data_out         - Receives heap-allocated buffer (caller frees)
* size_out         - Receives buffer size in bytes
* progressFn       - Progress callback, or NULL
* progressUserData - Opaque pointer passed to progressFn
* cancel_flag      - Pointer to an int polled during transfer
*                    (NULL = no cancellation)
*********************************************************/
int wbn_api_download_to_memory_progress(const char *path,
                                        uint8_t **data_out, size_t *size_out,
                                        WbnProgressFn progressFn,
                                        void *progressUserData,
                                        volatile int *cancel_flag);

/*********************************************************
*NAME:          wbn_api_download_to_memory_cancellable
*PURPOSE:
* Like wbn_api_download_to_memory, but the caller can abort
* the transfer mid-flight by setting *cancel_flag to a
* non-zero value (atomic int). Returns -2 on cancel,
* otherwise the HTTP status code or -1 on transport error.
* On success, *data_out / *size_out are populated as in the
* non-cancellable variant; on any non-200 outcome they are
* left as NULL/0 and any partial buffer is freed.
*
*ARGUMENTS:
* path        - API path after /api/v1/
* data_out    - Receives heap-allocated buffer (caller frees)
* size_out    - Receives buffer size in bytes
* cancel_flag - Pointer to an int polled during transfer
*               (treat as volatile/atomic). NULL = no cancel.
*********************************************************/
int wbn_api_download_to_memory_cancellable(const char *path,
                                            uint8_t **data_out,
                                            size_t *size_out,
                                            volatile int *cancel_flag);

/*********************************************************
*NAME:          httpSetAltIpAddress
*PURPOSE:
* Sets the local interface/IP for libcurl to bind to when
* connecting (maps to CURLOPT_INTERFACE).  Pass an IP
* address, interface name, or hostname.
*
*ARGUMENTS:
* iptoset - Interface to bind to
*********************************************************/
void httpSetAltIpAddress(char *iptoset);

#endif /* __HTTP_H */
