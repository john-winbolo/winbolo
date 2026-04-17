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

#include "../bolo/global.h"

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
