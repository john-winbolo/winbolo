/*
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
 *Name:          WBN key wire codec
 *Filename:      wbn_key_codec.h
 *Author:        John Morrison
 *Purpose:
 *  Encode and decode the WBN key wire convention shared by
 *  PACKET_WBN_REKEY (server -> client server_key) and the
 *  wbnJoinKey field on JOIN_REQUEST (client -> server
 *  player_key). The wire envelope is WBN_JOIN_KEY_WIRE_LEN
 *  bytes (65); the payload is a NUL-terminated string whose
 *  NUL must fall within the first WINBOLONET_KEY_LEN bytes
 *  (33). Anything past the terminator is zero-pad.
 *
 *  Header lives in internal/ because it is a wire-format
 *  detail; no frontend should include it.
 *********************************************************/

#ifndef WBN_KEY_CODEC_H
#define WBN_KEY_CODEC_H

#include <stdint.h>

#include "global.h"

/*********************************************************
 *NAME:          wbnKeyEncode
 *PURPOSE:
 * Encode a NUL-terminated key into a WBN_JOIN_KEY_WIRE_LEN
 * destination buffer. The full envelope is zeroed; then up
 * to (WINBOLONET_KEY_LEN - 1) bytes of src are copied into
 * the prefix. Sources longer than (WINBOLONET_KEY_LEN - 1)
 * are silently truncated and the remaining bytes stay zero.
 *
 *ARGUMENTS:
 * dest - Wire buffer (WBN_JOIN_KEY_WIRE_LEN bytes)
 * src  - NUL-terminated source key
 *********************************************************/
void wbnKeyEncode(uint8_t *dest, const char *src);

/*********************************************************
 *NAME:          wbnKeyDecode
 *PURPOSE:
 * Decode a key out of a WBN_JOIN_KEY_WIRE_LEN source buffer
 * into a NUL-terminated destination buffer of at least
 * WINBOLONET_KEY_LEN bytes. Walks src looking for a NUL in
 * the first WINBOLONET_KEY_LEN bytes; if none is found the
 * payload is malformed and the function returns FALSE
 * without writing to dest. Returns TRUE on success.
 *
 *ARGUMENTS:
 * dest - Destination string buffer (>= WINBOLONET_KEY_LEN)
 * src  - Wire buffer (WBN_JOIN_KEY_WIRE_LEN bytes)
 *********************************************************/
bool wbnKeyDecode(char *dest, const uint8_t *src);

#endif /* WBN_KEY_CODEC_H */
