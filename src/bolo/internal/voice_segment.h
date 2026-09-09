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
 *Name:          Voice Segment
 *Filename:      voice_segment.h
 *Author:        John Morrison
 *Purpose:
 *  Framing for the payloads carried on CHANNEL_VOICE: a
 *  sequence number, a flags byte, and an opaque encoded
 *  audio frame the server forwards without looking at.
 *
 *  Deliberately free of any codec dependency, so the
 *  dedicated server - which forwards voice but never runs a
 *  codec - links this without libopus.
 *
 *  Every unpack here is a parse of bytes from the network:
 *  each one is total, rejects anything short or malformed,
 *  and never reads past the length it was handed.
 *
 *  T2 (sim internals): includable within src/bolo/,
 *  src/server/, and tests/unit/ only.
 *********************************************************/

#ifndef VOICE_SEGMENT_H
#define VOICE_SEGMENT_H

#include <stdbool.h>
#include <stdint.h>

#include "channel_mux.h"

#define VOICE_SEG_UP_HEADER    2   /* seq, flags                    */
#define VOICE_SEG_DOWN_HEADER  3   /* fromPlayer, seq, flags        */
#define VOICE_SEG_MAX_OPUS   (CHANNEL_VOICE_SEG - VOICE_SEG_DOWN_HEADER)

/* Bit 0: the talker stopped after this frame, so a receiver can drop its
 * jitter buffer instead of playing the tail late. */
#define VOICE_FLAG_END_OF_UTTERANCE 0x01

/* client -> server: [seq u8][flags u8][opus...]
 * Returns the total segment length, or 0 if it will not fit / bad args. */
int voiceSegmentPackUp(uint8_t *out, int outCap, uint8_t seq, uint8_t flags,
                       const uint8_t *opus, int opusLen);

/* Parses a client->server segment. opus points into in; no copy.
 * Returns false on any short or malformed input. */
bool voiceSegmentUnpackUp(const uint8_t *in, int inLen, uint8_t *seq,
                          uint8_t *flags, const uint8_t **opus, int *opusLen);

/* server -> client: [fromPlayer u8][seq u8][flags u8][opus...]
 * Returns the total segment length, or 0 if it will not fit / bad args. */
int voiceSegmentPackDown(uint8_t *out, int outCap, uint8_t fromPlayer,
                         uint8_t seq, uint8_t flags,
                         const uint8_t *opus, int opusLen);

/* Parses a server->client segment. opus points into in; no copy.
 * Returns false on any short or malformed input, including a fromPlayer
 * outside the tank slots. */
bool voiceSegmentUnpackDown(const uint8_t *in, int inLen, uint8_t *fromPlayer,
                            uint8_t *seq, uint8_t *flags,
                            const uint8_t **opus, int *opusLen);

#endif /* VOICE_SEGMENT_H */
