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
*Last Modified: 10/3/26
*Purpose:
*  Responsible for sending/receiving HTTP/HTTPS messages
*  to WinBolo.net via libcurl.
*
*  [WINBOLO.NET] Host= in the INI file accepts:
*    hostname             (implies https://)
*    http://hostname
*    https://hostname
*********************************************************/

#ifndef __HTTP_H
#define __HTTP_H

#include "../bolo/global.h"

/*********************************************************
*NAME:          httpCreate
*PURPOSE:
* Initialises the http module.  Reads the server host from
* the [WINBOLO.NET] Host INI key and initialises libcurl.
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
*NAME:          httpSendMessage
*PURPOSE:
* Sends a binary message to WinBolo.net via HTTP(S) GET
* and returns the response body length, or -1 on error.
*
*ARGUMENTS:
* message  - Binary message buffer
* len      - Length of message
* response - Buffer to receive the response
* maxSize  - Capacity of response buffer
*********************************************************/
int httpSendMessage(BYTE *message, int len, BYTE *response, int maxSize);

/*********************************************************
*NAME:          httpSendLogFile
*PURPOSE:
* Uploads a log file to WinBolo.net via HTTP(S) multipart
* POST.  Returns TRUE on success.
*
*ARGUMENTS:
* fileName     - Path to the log file
* key          - WINBOLONET_KEY_LEN-byte session key
* wantFeedback - (unused, retained for API compatibility)
*********************************************************/
bool httpSendLogFile(char *fileName, BYTE *key, bool wantFeedback);

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
