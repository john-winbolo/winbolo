/*
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
 *Name:          Upload Policy
 *Filename:      upload_policy.h
 *Author:        John Morrison
 *Purpose:
 *  Operator-chosen handling for client-pushed map uploads
 *  (UploadPolicy) and for client-pushed scripts
 *  (ScriptUploadPolicy). Both are set once at server startup
 *  and broadcast to remote clients via the lobby-settings
 *  control event so the lobby UI can reflect them.
 *********************************************************/

#ifndef UPLOAD_POLICY_H
#define UPLOAD_POLICY_H

#include <stdint.h>

/* Values are ordered so a zero-initialized server (and a client that
 * connects to an old server that doesn't broadcast the policy) defaults
 * to ALLOW. */
typedef enum {
    UPLOAD_POLICY_ALLOW   = 0,  /* default; play the upload, drop on map change */
    UPLOAD_POLICY_OFF     = 1,  /* refuse MAP_UPLOAD_BEGIN */
    UPLOAD_POLICY_PERSIST = 2   /* accept and write to data/maps/Uploads/ */
} UploadPolicy;

/* Player scripts have their own policy, separate from the map one.
 * Zero is ALLOW so a zeroed server behaves as it did before the
 * policy existed (upload scripts enabled). */
typedef enum {
    SCRIPT_UPLOAD_ALLOW   = 0,  /* default; accept uploads, keep for the session */
    SCRIPT_UPLOAD_OFF     = 1,  /* refuse script uploads; don't run an uploaded map's script */
    SCRIPT_UPLOAD_PERSIST = 2   /* accept uploads and keep them for good */
} ScriptUploadPolicy;

/* What an upload carries: the first byte of PACKET_LOBBY_MAP_UPLOAD_BEGIN. */
#define UPLOAD_KIND_MAP    0
#define UPLOAD_KIND_SCRIPT 1

/* Why a script upload was refused, as a code the client puts into its own
 * language, with up to two numbers the line needs. Carried on the script
 * variant of PACKET_LOBBY_MAP_UPLOAD_DONE. The text beside it is one line in
 * English for the operator's console and the log; a client never shows it. */
#define SCRIPT_REFUSE_NONE        0  /* taken, or no reason given */
#define SCRIPT_REFUSE_SCRIPTS_OFF 1  /* this server takes no scripts */
#define SCRIPT_REFUSE_NAME_TAKEN  2  /* a higher directory holds the name */
#define SCRIPT_REFUSE_WRITE       3  /* the server could not save the file */
#define SCRIPT_REFUSE_MANIFEST    4  /* the package's manifest will not parse */
#define SCRIPT_REFUSE_SYNTAX      5  /* the script will not load; a = line */
#define SCRIPT_REFUSE_NO_TABLE    6  /* the script declares no scenario table */
#define SCRIPT_REFUSE_API         7  /* a = api asked for, b = api this server runs */
#define SCRIPT_REFUSE_KIND        8  /* a kind this server does not know */
#define SCRIPT_REFUSE_BOUND       9  /* bound to a map; its map is what is sent */

#define SCRIPT_REFUSE_TEXT_LEN 256

typedef struct ScriptUploadRefusal {
    uint8_t reason;   /* SCRIPT_REFUSE_* */
    int32_t a;        /* the reason's first number, 0 for none */
    int32_t b;        /* its second, 0 for none */
    char    text[SCRIPT_REFUSE_TEXT_LEN]; /* the operator's line; "" when none */
} ScriptUploadRefusal;

#endif /* UPLOAD_POLICY_H */
