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
 *Name:          Channel Mux
 *Filename:      channel_mux.h
 *Author:        John Morrison
 *Purpose:
 *  One reliability primitive: N independent reliable-ordered
 *  substreams (channels) carried over UDP, built and tested
 *  entirely off-socket. The transport shuttles the byte frames
 *  this module builds and consumes; it owns no socket itself.
 *
 *  Channels 0-2 are message-flavor (each send is one whole
 *  logical message delivered in order, exactly once). Channel 3
 *  is stream-flavor (a byte stream split into segments and
 *  reassembled in order on the far side — the bulk transfer
 *  fragmentation layer, no separate chunker).
 *
 *  One cumulative-ack / full-tail-resend / window-bounded core
 *  with a receive-side reorder buffer serves both flavors. One
 *  shared channel-frame codec (ack list + segment list) carries
 *  the bytes; channelRecvFrame is the single bounds-checked
 *  parse site.
 *
 *  T2 (sim internals): includable within src/bolo/, src/server/,
 *  and tests/unit/ only.
 *********************************************************/

#ifndef CHANNEL_MUX_H
#define CHANNEL_MUX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Fixed channel identities and the channel count. Channels 0-2 carry
 * discrete messages; channel 3 carries a byte stream. */
enum {
    CHANNEL_GAME    = 0,
    CHANNEL_MAP     = 1,
    CHANNEL_CONTROL = 2,
    CHANNEL_BULK    = 3,
    CHANNEL_COUNT   = 4
};

/* Largest payload one segment may carry (one datagram-sized chunk). A
 * message-flavor send larger than this is rejected; a stream-flavor send
 * is split into pieces no larger than this. */
#define CHANNEL_MAX_SEG 256

/* Physical depth of the per-channel send / receive rings. The runtime
 * flow-control window passed to channelMuxInit is capped to this. Live
 * (unacked / buffered) sequence numbers are always contiguous and fewer
 * than the window, so indexing a ring by (seq % CHANNEL_MAX_WINDOW) never
 * aliases two live entries. */
#define CHANNEL_MAX_WINDOW 64

/* Capacity of the stream channel's pending-byte staging buffer. Bytes
 * handed to channelStreamSend wait here until the window has room to turn
 * them into segments. */
#define CHANNEL_STREAM_BUF 65536

/* Per-channel reliability state. ackedSeq / expectedSeq are exclusive
 * upper bounds (matching the shipped queue model: "confirmed up to here,
 * exclusive"). */
typedef struct {
    /* Send side. */
    uint32_t nextSeq;      /* next sequence number to assign           */
    uint32_t ackedSeq;     /* peer has received every seq < ackedSeq   */
    uint32_t txNext;       /* next seq to (re)transmit; walks forward,
                            * rewound to ackedSeq on a retransmit timeout */
    uint32_t lastTxTick;   /* tick of the most recent transmission      */
    bool     everSent;     /* lastTxTick is meaningful once true        */
    uint16_t sendLen[CHANNEL_MAX_WINDOW];
    uint8_t  sendData[CHANNEL_MAX_WINDOW][CHANNEL_MAX_SEG];

    /* Receive side. */
    uint32_t expectedSeq;  /* next in-order seq to deliver (the ack we emit) */
    bool     ackDirty;     /* an ack for this channel is waiting to be sent  */
    bool     recvPresent[CHANNEL_MAX_WINDOW];
    uint16_t recvLen[CHANNEL_MAX_WINDOW];
    uint8_t  recvData[CHANNEL_MAX_WINDOW][CHANNEL_MAX_SEG];
} ChannelState;

typedef struct ChannelMux {
    uint32_t window;       /* flow-control cap, <= CHANNEL_MAX_WINDOW */
    uint32_t curTick;      /* last tick handed to channelTick         */
    uint32_t rttMs;        /* last RTT estimate handed to channelTick */
    ChannelState ch[CHANNEL_COUNT];

    /* Stream channel (CHANNEL_BULK) pending-byte ring. */
    uint8_t  streamBuf[CHANNEL_STREAM_BUF];
    uint32_t streamHead;   /* ring read index                  */
    uint32_t streamCount;  /* bytes waiting to be segmentized   */
} ChannelMux;

/* Initialise a caller-owned ChannelMux. windowPerChannel is clamped to
 * [1, CHANNEL_MAX_WINDOW]. */
void channelMuxInit(ChannelMux *m, uint32_t windowPerChannel);

/* Queue one whole message on a message-flavor channel (0-2). Returns
 * false on a usage error (stream channel, oversized message, bad id) or
 * when the window is full without room for another in-flight segment. */
bool channelSend(ChannelMux *m, uint8_t ch, const uint8_t *msg, uint16_t len);

/* Append bytes to the stream-flavor channel (3). Returns false on a usage
 * error (a message channel, bad id) or when the pending-byte buffer cannot
 * hold the data. The core splits the stream into segment-sized pieces. */
bool channelStreamSend(ChannelMux *m, uint8_t ch, const uint8_t *data,
                       uint32_t len);

/* Build one channel frame into buf: the pending acks, then as many queued
 * segments as fit under budget. Returns the number of bytes written
 * (>= 2: the two count bytes are always present). */
int channelBuildFrame(ChannelMux *m, uint8_t *buf, int budget);

/* Consume one channel frame. Every field read is bounds-checked against
 * len; a malformed or truncated frame is rejected with a negative return
 * and no over-read. Returns the number of bytes consumed on success. */
int channelRecvFrame(ChannelMux *m, const uint8_t *buf, int len);

/* Pop the next in-order payload from a channel into out (which must hold at
 * least CHANNEL_MAX_SEG bytes). For message channels each pop is one whole
 * message; for the stream channel each pop is the next stream fragment, to
 * be concatenated by the caller. Returns false when nothing is ready. */
bool channelReceive(ChannelMux *m, uint8_t ch, uint8_t *out, uint16_t *outLen);

/* Advance the retransmit clock. tick is the current tick; rttMs is the
 * current RTT estimate used to derive the retransmit timeout. */
void channelTick(ChannelMux *m, uint32_t tick, uint32_t rttMs);

#endif /* CHANNEL_MUX_H */
