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
 * discrete messages; channel 3 carries a byte stream; channel 4 carries
 * best-effort (fire-and-forget) messages. */
enum {
    CHANNEL_GAME        = 0,
    CHANNEL_MAP         = 1,
    CHANNEL_CONTROL     = 2,
    CHANNEL_BULK        = 3,
    CHANNEL_GAME_EFFECT = 4,
    CHANNEL_COUNT       = 5
};

/* Per-channel window depth and segment size. Each channel sizes its rings
 * to its own traffic instead of one uniform pair, so the high-rate game
 * channel can be deep with tiny segments while control / bulk keep room for
 * large messages — without the cross-product memory blowup a uniform raise
 * would cost. window is the physical depth of that channel's send / receive
 * rings: live (unacked / buffered) sequence numbers are always contiguous
 * and fewer than the window, so indexing a ring by (seq % window) never
 * aliases two live entries. segSize is the largest payload one segment may
 * carry; a message-flavor send larger than it is rejected, a stream-flavor
 * send is split into pieces no larger than it. */
#define CHANNEL_GAME_WINDOW    512  /* game events <= GAME_EVENT_MAX_WIRE_SIZE */
#define CHANNEL_GAME_SEG        16
#define CHANNEL_MAP_WINDOW     128  /* map events are also GameEvents          */
#define CHANNEL_MAP_SEG         16
#define CHANNEL_CONTROL_WINDOW 64   /* covers the join sync-replay burst plus
                                     * concurrent publishes with margin        */
#define CHANNEL_CONTROL_SEG   1024  /* one control event = one segment = one
                                     * datagram. Must hold the worst-case
                                     * control message (the full BRAIN_LIST:
                                     * type(1) + bodyLen(2) + body(897) = 900)
                                     * yet stay budget-safe: a segment plus the
                                     * channel-frame overhead and packet header
                                     * must fit UDP_MAX_PAYLOAD with room for
                                     * coalesced acks. 1024 clears both.        */
#define CHANNEL_BULK_WINDOW     96  /* provisional: bandwidth-delay product set
                                     * when bulk transfer moves here           */
#define CHANNEL_BULK_SEG       256  /* stream segments                         */
#define CHANNEL_GAME_EFFECT_WINDOW 64  /* best-effort ephemeral game events;
                                        * sized to one tick's burst + margin,
                                        * not retransmit depth                */
#define CHANNEL_GAME_EFFECT_SEG    16  /* one GameEvent, like CHANNEL_GAME    */

/* Largest segSize over all channels, so a caller can size one scratch
 * receive buffer that fits a segment from any channel. */
#define CHANNEL_MAX_SEG 1024

/* Capacity of the stream channel's pending-byte staging buffer. Bytes
 * handed to channelStreamSend wait here until the window has room to turn
 * them into segments. */
#define CHANNEL_STREAM_BUF 65536

/* Per-channel reliability state. ackedSeq / expectedSeq are exclusive
 * upper bounds (matching the shipped queue model: "confirmed up to here,
 * exclusive"). The ring storage lives in the owning ChannelMux, sized to
 * this channel's {window, segSize}; channelMuxInit points window / segSize
 * and the five ring pointers below at it. A ring entry's payload starts at
 * (sendData + idx * segSize); sendLen[idx] gives its length. */
typedef struct {
    uint32_t window;       /* this channel's ring depth                */
    uint32_t segSize;      /* this channel's max segment payload        */
    bool     bestEffort;   /* signals the frame builder/applier to skip
                            * acks, RTO, and reorder-hold for this channel */

    /* Send side. */
    uint32_t nextSeq;      /* next sequence number to assign           */
    uint32_t ackedSeq;     /* peer has received every seq < ackedSeq   */
    uint32_t txNext;       /* next seq to (re)transmit; walks forward,
                            * rewound to ackedSeq on a retransmit timeout */
    uint32_t lastTxTick;   /* tick of the most recent transmission      */
    bool     everSent;     /* lastTxTick is meaningful once true        */
    uint16_t *sendLen;     /* [window]                                  */
    uint8_t  *sendData;    /* [window * segSize], row stride = segSize  */

    /* Receive side. */
    uint32_t expectedSeq;  /* next in-order seq to deliver (the ack we emit) */
    bool     ackDirty;     /* an ack for this channel is waiting to be sent  */
    bool     *recvPresent; /* [window]                                  */
    uint16_t *recvLen;     /* [window]                                  */
    uint8_t  *recvData;    /* [window * segSize], row stride = segSize  */

    /* Fast-retransmit (NAK) state. */
    uint32_t recvHighestSeq; /* exclusive upper bound on the highest seq
                              * accepted on the receive side; reported as
                              * highestSeen so the peer can spot a gap        */
    bool     nakIssued;    /* a fast-retransmit fired for the current stall;
                            * cleared when the cumulative ack advances, so each
                            * distinct gap rewinds the tail at most once     */
} ChannelState;

typedef struct ChannelMux {
    uint32_t curTick;      /* last tick handed to channelTick         */
    uint32_t rttMs;        /* last RTT estimate handed to channelTick */
    ChannelState ch[CHANNEL_COUNT];

    /* Per-channel ring storage, each sized to its own {window, segSize}.
     * ChannelState pointers above are wired here in channelMuxInit, so the
     * whole mux stays a plain value type with no heap allocation. */
    uint16_t gameSendLen[CHANNEL_GAME_WINDOW];
    uint8_t  gameSendData[CHANNEL_GAME_WINDOW][CHANNEL_GAME_SEG];
    bool     gameRecvPresent[CHANNEL_GAME_WINDOW];
    uint16_t gameRecvLen[CHANNEL_GAME_WINDOW];
    uint8_t  gameRecvData[CHANNEL_GAME_WINDOW][CHANNEL_GAME_SEG];

    uint16_t mapSendLen[CHANNEL_MAP_WINDOW];
    uint8_t  mapSendData[CHANNEL_MAP_WINDOW][CHANNEL_MAP_SEG];
    bool     mapRecvPresent[CHANNEL_MAP_WINDOW];
    uint16_t mapRecvLen[CHANNEL_MAP_WINDOW];
    uint8_t  mapRecvData[CHANNEL_MAP_WINDOW][CHANNEL_MAP_SEG];

    uint16_t controlSendLen[CHANNEL_CONTROL_WINDOW];
    uint8_t  controlSendData[CHANNEL_CONTROL_WINDOW][CHANNEL_CONTROL_SEG];
    bool     controlRecvPresent[CHANNEL_CONTROL_WINDOW];
    uint16_t controlRecvLen[CHANNEL_CONTROL_WINDOW];
    uint8_t  controlRecvData[CHANNEL_CONTROL_WINDOW][CHANNEL_CONTROL_SEG];

    uint16_t bulkSendLen[CHANNEL_BULK_WINDOW];
    uint8_t  bulkSendData[CHANNEL_BULK_WINDOW][CHANNEL_BULK_SEG];
    bool     bulkRecvPresent[CHANNEL_BULK_WINDOW];
    uint16_t bulkRecvLen[CHANNEL_BULK_WINDOW];
    uint8_t  bulkRecvData[CHANNEL_BULK_WINDOW][CHANNEL_BULK_SEG];

    uint16_t gameEffectSendLen[CHANNEL_GAME_EFFECT_WINDOW];
    uint8_t  gameEffectSendData[CHANNEL_GAME_EFFECT_WINDOW][CHANNEL_GAME_EFFECT_SEG];
    bool     gameEffectRecvPresent[CHANNEL_GAME_EFFECT_WINDOW];
    uint16_t gameEffectRecvLen[CHANNEL_GAME_EFFECT_WINDOW];
    uint8_t  gameEffectRecvData[CHANNEL_GAME_EFFECT_WINDOW][CHANNEL_GAME_EFFECT_SEG];

    /* Stream channel (CHANNEL_BULK) pending-byte ring. */
    uint8_t  streamBuf[CHANNEL_STREAM_BUF];
    uint32_t streamHead;   /* ring read index                  */
    uint32_t streamCount;  /* bytes waiting to be segmentized   */
} ChannelMux;

/* Initialise a caller-owned ChannelMux: zero the reliability state and point
 * each channel's ring pointers at that channel's storage with its window /
 * segSize. */
void channelMuxInit(ChannelMux *m);

/* Queue one whole message on a message-flavor channel (0-2). Returns
 * false on a usage error (stream channel, oversized message, bad id) or
 * when the window is full without room for another in-flight segment. */
bool channelSend(ChannelMux *m, uint8_t ch, const uint8_t *msg, uint16_t len);

/* Queue one whole message on the best-effort channel. Never blocks and never
 * signals overflow by failing: if the ring is full, the oldest pending segment
 * is dropped to make room. Returns false only on a usage error (not a
 * best-effort channel, bad id, oversized message). */
bool channelSendBestEffort(ChannelMux *m, uint8_t ch, const uint8_t *msg,
                           uint16_t len);

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

/* Pop the next available best-effort payload: the lowest buffered seq at or
 * above the delivery cursor, advancing the cursor past any skipped seqs
 * (dropped, never waited on). Returns false when nothing is buffered. */
bool channelReceiveBestEffort(ChannelMux *m, uint8_t ch, uint8_t *out,
                              uint16_t *outLen);

/* Truncate the send side of a channel forward at a game boundary: drop the
 * unacked send tail (ackedSeq = txNext = nextSeq) and clear those ring
 * entries, so the previous game's undelivered segments are never resent. The
 * sequence space stays monotonic — low seqs are not reused, new channelSends
 * continue from nextSeq. Returns the post-reset nextSeq, the baseline the
 * peer must adopt with channelResetExpected. */
uint32_t channelResetSend(ChannelMux *m, uint8_t ch);

/* Truncate the receive side of a channel forward to match a peer's send
 * reset: advance expectedSeq to newExpected and discard any buffered segment
 * below it. A straggler with seq < newExpected is then dropped by the
 * existing dedup; a segment already buffered at seq >= newExpected becomes
 * deliverable. newExpected must not rewind — a value at or below the current
 * expectedSeq is a no-op. */
void channelResetExpected(ChannelMux *m, uint8_t ch, uint32_t newExpected);

/* Advance the retransmit clock. tick is the current tick; rttMs is the
 * current RTT estimate used to derive the retransmit timeout. */
void channelTick(ChannelMux *m, uint32_t tick, uint32_t rttMs);

#endif /* CHANNEL_MUX_H */
