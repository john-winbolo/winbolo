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
 *Name:          Transport Control Codec
 *Filename:      transport_control_codec.h
 *Author:        John Morrison
 *Purpose:
 *  Per-variant encoder/decoder table for ControlEvent.
 *  Encoders run inside the server's per-client subscriber
 *  deliver callback, taking the recipient's UdpServerClient
 *  context so they can filter single-target events and stamp
 *  per-recipient state. Decoders run inside the client's wire
 *  packet dispatcher and build a ControlEvent that gets fed
 *  to clientSimApplyControl.
 *
 *  Co-located so the server (encode) and client (decode)
 *  sides of each variant sit next to each other.
 *********************************************************/

#ifndef TRANSPORT_CONTROL_CODEC_H
#define TRANSPORT_CONTROL_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "control_event.h"

/* Encoder result. ENCODE_OK means buf and outLen are populated and
 * the caller should deliver the bytes. ENCODE_SKIP means this event
 * has no wire form for this recipient (e.g. an alliance request to
 * a non-target client) — not an error. ENCODE_OVERFLOW means the
 * encoder needed more than bufCap bytes and is a programmer bug;
 * the caller asserts in debug builds and silently drops in release. */
typedef enum {
    ENCODE_OK,
    ENCODE_SKIP,
    ENCODE_OVERFLOW
} EncodeResult;

/* Forward decl — the encoder takes the recipient's UdpServerClient
 * by const pointer so it can read addr/sequence/etc. without
 * pulling the full struct (and its transport_udp.h include chain)
 * into every codec consumer. */
struct UdpServerClient;

typedef EncodeResult (*ControlEncodeFn)(const ControlEvent *evt,
                                        const struct UdpServerClient *recipient,
                                        uint8_t *buf, size_t bufCap,
                                        size_t *outLen);

typedef bool (*ControlDecodeFn)(const uint8_t *buf, size_t len,
                                ControlEvent *outEvt);

/* Stack-allocation upper bound for any single ControlEvent's wire
 * encoding. Sized for the brain-list worst case
 *   8 hdr + 1 count + BRAIN_LIST_MAX (16) * (3 length bytes +
 *     BRAIN_LIST_NAME_LEN-1 (31) + BRAIN_LIST_VER_LEN-1 (23) +
 *     BRAIN_LIST_PATH_LEN-1 (255)) = 5001 bytes
 * — rounded to 8192 for headroom. All other variants fit comfortably
 * (the chat-localized case is 282 bytes). */
#define MAX_CONTROL_PACKET 8192

/* Returns the encoder for the given variant, or NULL if the
 * variant has no wire form (CTRL_MAP_DOWNLOAD_COMPLETE is
 * client-internal) or the type is out of range. */
ControlEncodeFn transportControlCodecEncoder(ControlEventType type);

/* Returns the decoder for the given wire packet type, or NULL if
 * the packet isn't backed by a control event (snapshot, handshake,
 * ping, etc). */
ControlDecodeFn transportControlCodecDecoder(uint16_t packetType);

#endif /* TRANSPORT_CONTROL_CODEC_H */
