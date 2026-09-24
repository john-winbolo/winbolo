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

#endif /* UPLOAD_POLICY_H */
