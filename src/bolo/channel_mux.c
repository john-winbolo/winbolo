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
 *Filename:      channel_mux.c
 *Author:        John Morrison
 *Purpose:
 *  Implementation of the reliable-ordered channel multiplexer.
 *  See channel_mux.h for the model. Reuses the big-endian
 *  packU16/packU32 helpers from transport_udp_common.c so the
 *  channel frame shares the project's one serialization style.
 *********************************************************/

#include "channel_mux.h"
#include "transport_udp_internal.h"

/* Channel frame layout (one shared codec):
 *
 *   ackCount : u8
 *   repeat ackCount:  channelId u8, ackedSeq u32, highestSeen u32
 *                       ackedSeq  (exclusive): peer has received every seq <
 *                                  ackedSeq in order on that channel.
 *                       highestSeen (exclusive): one past the highest seq the
 *                                  peer has buffered. highestSeen > ackedSeq
 *                                  means a gap exists past the in-order point,
 *                                  which the sender fast-retransmits.
 *   segCount : u8
 *   repeat segCount:  channelId u8, seq u32, len u16, payload[len]
 *
 * Per-record sizes used by the parser bounds checks. */
#define CHANNEL_ACK_RECORD_SIZE 9  /* channelId(1) + ackedSeq(4) + highestSeen(4) */
#define CHANNEL_SEG_HEADER_SIZE 7  /* channelId(1) + seq(4) + len(2)        */

static uint32_t channelMin32(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

/* Retransmit timeout in ticks, derived from the RTT estimate. At the 50 Hz
 * tick (20 ms/tick) rttMs/10 is roughly two round trips; floored at 2 ticks
 * so a zero/unknown RTT still backs off rather than resending every tick. */
static uint32_t channelRtoTicks(const ChannelMux *m) {
    uint32_t rto = m->rttMs / 10;
    if (rto < 2) {
        rto = 2;
    }
    return rto;
}

void channelMuxInit(ChannelMux *m) {
    if (m == NULL) {
        return;
    }
    memset(m, 0, sizeof(*m));

    ChannelState *game = &m->ch[CHANNEL_GAME];
    game->window = CHANNEL_GAME_WINDOW;
    game->segSize = CHANNEL_GAME_SEG;
    game->sendLen = m->gameSendLen;
    game->sendData = &m->gameSendData[0][0];
    game->recvPresent = m->gameRecvPresent;
    game->recvLen = m->gameRecvLen;
    game->recvData = &m->gameRecvData[0][0];

    ChannelState *map = &m->ch[CHANNEL_MAP];
    map->window = CHANNEL_MAP_WINDOW;
    map->segSize = CHANNEL_MAP_SEG;
    map->sendLen = m->mapSendLen;
    map->sendData = &m->mapSendData[0][0];
    map->recvPresent = m->mapRecvPresent;
    map->recvLen = m->mapRecvLen;
    map->recvData = &m->mapRecvData[0][0];

    ChannelState *control = &m->ch[CHANNEL_CONTROL];
    control->window = CHANNEL_CONTROL_WINDOW;
    control->segSize = CHANNEL_CONTROL_SEG;
    control->sendLen = m->controlSendLen;
    control->sendData = &m->controlSendData[0][0];
    control->recvPresent = m->controlRecvPresent;
    control->recvLen = m->controlRecvLen;
    control->recvData = &m->controlRecvData[0][0];

    ChannelState *bulk = &m->ch[CHANNEL_BULK];
    bulk->window = CHANNEL_BULK_WINDOW;
    bulk->segSize = CHANNEL_BULK_SEG;
    bulk->sendLen = m->bulkSendLen;
    bulk->sendData = &m->bulkSendData[0][0];
    bulk->recvPresent = m->bulkRecvPresent;
    bulk->recvLen = m->bulkRecvLen;
    bulk->recvData = &m->bulkRecvData[0][0];

    ChannelState *gameEffect = &m->ch[CHANNEL_GAME_EFFECT];
    gameEffect->window = CHANNEL_GAME_EFFECT_WINDOW;
    gameEffect->segSize = CHANNEL_GAME_EFFECT_SEG;
    gameEffect->bestEffort = true;
    gameEffect->sendLen = m->gameEffectSendLen;
    gameEffect->sendData = &m->gameEffectSendData[0][0];
    gameEffect->recvPresent = m->gameEffectRecvPresent;
    gameEffect->recvLen = m->gameEffectRecvLen;
    gameEffect->recvData = &m->gameEffectRecvData[0][0];
}

void channelTick(ChannelMux *m, uint32_t tick, uint32_t rttMs) {
    if (m == NULL) {
        return;
    }
    m->curTick = tick;
    m->rttMs = rttMs;
}

bool channelSend(ChannelMux *m, uint8_t ch, const uint8_t *msg, uint16_t len) {
    if (m == NULL || ch >= CHANNEL_COUNT || ch == CHANNEL_BULK) {
        return false; /* usage error: bad id or wrong flavor */
    }
    ChannelState *c = &m->ch[ch];
    if (len > c->segSize) {
        return false; /* a message must fit one segment */
    }
    /* Window-bounded: never let more than `window` segments be unacked. */
    if ((c->nextSeq - c->ackedSeq) >= c->window) {
        return false; /* overflow signal — caller/transport disconnects */
    }
    uint32_t idx = c->nextSeq % c->window;
    c->sendLen[idx] = len;
    if (len > 0 && msg != NULL) {
        memcpy(c->sendData + idx * c->segSize, msg, len);
    }
    c->nextSeq++;
    return true;
}

bool channelSendBestEffort(ChannelMux *m, uint8_t ch, const uint8_t *msg,
                           uint16_t len) {
    if (m == NULL || ch >= CHANNEL_COUNT) {
        return false; /* usage error: bad id */
    }
    ChannelState *c = &m->ch[ch];
    if (!c->bestEffort) {
        return false; /* usage error: not a best-effort channel */
    }
    if (len > c->segSize) {
        return false; /* a message must fit one segment */
    }
    /* No ack ever drains this ring, so reclaim it here: when the window is
     * full, drop the oldest pending segment rather than block or fail. */
    if ((c->nextSeq - c->ackedSeq) >= c->window) {
        c->ackedSeq++;
        if (c->txNext < c->ackedSeq) {
            c->txNext = c->ackedSeq;
        }
    }
    uint32_t idx = c->nextSeq % c->window;
    c->sendLen[idx] = len;
    if (len > 0 && msg != NULL) {
        memcpy(c->sendData + idx * c->segSize, msg, len);
    }
    c->nextSeq++;
    return true;
}

/* Turn pending stream bytes into segments while the window has room. */
static void channelStreamRefill(ChannelMux *m) {
    ChannelState *c = &m->ch[CHANNEL_BULK];
    while (m->streamCount > 0 && (c->nextSeq - c->ackedSeq) < c->window) {
        uint32_t n = channelMin32(c->segSize, m->streamCount);
        uint32_t idx = c->nextSeq % c->window;
        uint8_t *seg = c->sendData + idx * c->segSize;
        uint32_t k;
        for (k = 0; k < n; k++) {
            seg[k] = m->streamBuf[(m->streamHead + k) % CHANNEL_STREAM_BUF];
        }
        c->sendLen[idx] = (uint16_t)n;
        m->streamHead = (m->streamHead + n) % CHANNEL_STREAM_BUF;
        m->streamCount -= n;
        c->nextSeq++;
    }
}

bool channelStreamSend(ChannelMux *m, uint8_t ch, const uint8_t *data,
                       uint32_t len) {
    if (m == NULL || ch != CHANNEL_BULK) {
        return false; /* usage error: only the bulk channel streams */
    }
    if (len == 0) {
        return true;
    }
    /* streamCount never exceeds CHANNEL_STREAM_BUF, so the subtraction stays
     * non-negative; phrasing the capacity check this way (rather than
     * streamCount + len) keeps it overflow-safe when len is large. Every
     * current caller streams bounded chunks, so this is defense-in-depth on a
     * generic primitive — the reject is the existing pending-buffer-full
     * signal, no state change. */
    if (len > CHANNEL_STREAM_BUF - m->streamCount) {
        return false; /* pending buffer full — overflow signal */
    }
    if (data != NULL) {
        uint32_t i;
        for (i = 0; i < len; i++) {
            m->streamBuf[(m->streamHead + m->streamCount + i) %
                         CHANNEL_STREAM_BUF] = data[i];
        }
    }
    m->streamCount += len;
    /* Form segments now so the first transmission can ride immediately. */
    channelStreamRefill(m);
    return true;
}

int channelBuildFrame(ChannelMux *m, uint8_t *buf, int budget) {
    if (m == NULL || buf == NULL || budget < 2) {
        return 0;
    }

    /* Bring any pending stream bytes into the window first. */
    channelStreamRefill(m);

    int pos = 0;
    int ackCountPos = pos;
    buf[pos++] = 0; /* ackCount placeholder */
    uint8_t ackCount = 0;

    int ch;
    for (ch = 0; ch < CHANNEL_COUNT; ch++) {
        ChannelState *c = &m->ch[ch];
        if (c->bestEffort) {
            continue; /* best-effort is never acked (ackDirty stays clear) */
        }
        if (!c->ackDirty) {
            continue;
        }
        /* Reserve one byte for the segCount field so the frame is always
         * well-formed even when the budget barely fits the acks. */
        if (ackCount == 255 || pos + CHANNEL_ACK_RECORD_SIZE + 1 > budget) {
            break; /* no room — leave ackDirty set, retry next frame */
        }
        buf[pos] = (uint8_t)ch;
        packU32(buf + pos + 1, c->expectedSeq);
        uint32_t hs = (c->recvHighestSeq < c->expectedSeq) ? c->expectedSeq
                                                           : c->recvHighestSeq;
        packU32(buf + pos + 5, hs); /* never below the cumulative ack */
        pos += CHANNEL_ACK_RECORD_SIZE;
        ackCount++;
        c->ackDirty = false;
    }
    buf[ackCountPos] = ackCount;

    int segCountPos = pos;
    buf[pos++] = 0; /* segCount placeholder — always fits (budget >= 2) */
    uint8_t segCount = 0;

    uint32_t rto = channelRtoTicks(m);
    for (ch = 0; ch < CHANNEL_COUNT; ch++) {
        ChannelState *c = &m->ch[ch];
        if (c->bestEffort) {
            /* Drain-and-clear: frame each pending segment once, free its ring
             * slot, advance the cursor. No ack listing, no RTO, no retransmit
             * retention. */
            uint32_t seq = c->txNext;
            for (; seq < c->nextSeq; seq++) {
                uint32_t idx = seq % c->window;
                uint16_t slen = c->sendLen[idx];
                if (segCount == 255 ||
                    pos + CHANNEL_SEG_HEADER_SIZE + slen > budget) {
                    break; /* tight budget — remaining drain next frame */
                }
                buf[pos] = (uint8_t)ch;
                packU32(buf + pos + 1, seq);
                packU16(buf + pos + 5, slen);
                pos += CHANNEL_SEG_HEADER_SIZE;
                if (slen > 0) {
                    memcpy(buf + pos, c->sendData + idx * c->segSize, slen);
                }
                pos += slen;
                segCount++;
                c->sendLen[idx] = 0; /* drained; nothing retained for retransmit */
            }
            c->txNext = seq;
            c->ackedSeq = seq; /* framed == done; recycle the ring without an ack */
            continue;
        }
        bool tailNonEmpty = (c->ackedSeq < c->nextSeq);

        /* A stalled tail (the transmit cursor has reached nextSeq but acks
         * have not caught up) past the RTO rewinds the cursor to ackedSeq:
         * the whole unacked window is retransmitted, walked out over as many
         * frames as the budget needs. An actively-advancing cursor keeps
         * lastTxTick fresh, so the RTO only fires once flow stalls — no
         * resend storm during steady transmission. */
        bool rtoExpired = tailNonEmpty && c->everSent &&
                          (m->curTick - c->lastTxTick) >= rto;
        if (rtoExpired) {
            c->txNext = c->ackedSeq;
        }
        if (c->txNext < c->ackedSeq) {
            c->txNext = c->ackedSeq; /* acks moved the window forward */
        }
        if (c->txNext >= c->nextSeq) {
            continue; /* caught up on transmission — nothing to send */
        }

        bool emitted = false;
        uint32_t seq = c->txNext;
        for (; seq < c->nextSeq; seq++) {
            uint32_t idx = seq % c->window;
            uint16_t slen = c->sendLen[idx];
            if (segCount == 255 ||
                pos + CHANNEL_SEG_HEADER_SIZE + slen > budget) {
                break; /* tight budget — continue from here next frame */
            }
            buf[pos] = (uint8_t)ch;
            packU32(buf + pos + 1, seq);
            packU16(buf + pos + 5, slen);
            pos += CHANNEL_SEG_HEADER_SIZE;
            if (slen > 0) {
                memcpy(buf + pos, c->sendData + idx * c->segSize, slen);
            }
            pos += slen;
            segCount++;
            emitted = true;
        }
        c->txNext = seq; /* first sequence not yet transmitted this pass */
        if (emitted) {
            c->lastTxTick = m->curTick;
            c->everSent = true;
        }
    }
    buf[segCountPos] = segCount;
    return pos;
}

/* Apply a received segment to the recv-side reorder buffer. */
static void channelApplySegment(ChannelMux *m, uint8_t ch, uint32_t seq,
                                const uint8_t *payload, uint16_t slen) {
    ChannelState *c = &m->ch[ch];
    if (c->bestEffort) {
        /* No ack for best-effort. Deliver-on-arrival with no reorder hold. */
        if (seq < c->expectedSeq) {
            return; /* stale / already delivered */
        }
        if (seq >= c->expectedSeq + c->window) {
            /* Jumped beyond the window — drop the oldest to make room rather
             * than wait (best-effort never stalls on a gap). */
            uint32_t newExpected = seq - c->window + 1;
            uint32_t s;
            for (s = c->expectedSeq;
                 s < newExpected && s < c->expectedSeq + c->window; s++) {
                c->recvPresent[s % c->window] = false;
            }
            c->expectedSeq = newExpected;
        }
        uint32_t idx = seq % c->window;
        if (c->recvPresent[idx]) {
            return; /* duplicate */
        }
        c->recvPresent[idx] = true;
        c->recvLen[idx] = slen;
        if (slen > 0) {
            memcpy(c->recvData + idx * c->segSize, payload, slen);
        }
        return;
    }
    /* Any segment (even a duplicate or out-of-window one) re-arms the ack,
     * so a dropped ack is recovered by the sender's next retransmit. */
    c->ackDirty = true;
    if (seq < c->expectedSeq) {
        return; /* already delivered — dedup */
    }
    if (seq >= c->expectedSeq + c->window) {
        return; /* beyond the reorder window — drop, sender will resend */
    }
    uint32_t idx = seq % c->window;
    if (c->recvPresent[idx]) {
        return; /* already buffered — dedup */
    }
    c->recvPresent[idx] = true;
    if (seq + 1 > c->recvHighestSeq) {
        c->recvHighestSeq = seq + 1; /* in-window accept lifts the gap bound */
    }
    c->recvLen[idx] = slen;
    if (slen > 0) {
        memcpy(c->recvData + idx * c->segSize, payload, slen);
    }
}

int channelRecvFrame(ChannelMux *m, const uint8_t *buf, int len) {
    if (m == NULL || buf == NULL || len < 1) {
        return -1;
    }
    int pos = 0;
    uint8_t ackCount = buf[pos++];

    uint8_t i;
    for (i = 0; i < ackCount; i++) {
        if (pos + CHANNEL_ACK_RECORD_SIZE > len) {
            return -1; /* truncated ack record */
        }
        uint8_t ch = buf[pos];
        uint32_t ackVal = unpackU32(buf + pos + 1);
        uint32_t highestSeen = unpackU32(buf + pos + 5);
        pos += CHANNEL_ACK_RECORD_SIZE;
        if (ch < CHANNEL_COUNT) {
            ChannelState *c = &m->ch[ch];
            /* Advance only forward and never past nextSeq — a stale or
             * hostile ack can neither rewind nor overrun the send window. */
            if (ackVal > c->ackedSeq && ackVal <= c->nextSeq) {
                c->ackedSeq = ackVal;
                if (c->txNext < c->ackedSeq) {
                    c->txNext = c->ackedSeq;
                }
                c->nakIssued = false; /* forward progress re-arms a future NAK */
            }
            /* A reported gap (the peer holds segments past the in-order point)
             * rewinds the transmit cursor once, so the existing send loop
             * resends the unacked tail a full RTO before the timeout would. */
            if (!c->bestEffort && c->ackedSeq < c->nextSeq &&
                highestSeen > c->ackedSeq && !c->nakIssued) {
                c->txNext = c->ackedSeq;   /* resend the unacked tail next frame */
                c->nakIssued = true;       /* one fast-retransmit per stall point */
            }
        }
    }

    if (pos + 1 > len) {
        return -1; /* missing segCount */
    }
    uint8_t segCount = buf[pos++];

    for (i = 0; i < segCount; i++) {
        if (pos + CHANNEL_SEG_HEADER_SIZE > len) {
            return -1; /* truncated segment header */
        }
        uint8_t ch = buf[pos];
        uint32_t seq = unpackU32(buf + pos + 1);
        uint16_t slen = unpackU16(buf + pos + 5);
        pos += CHANNEL_SEG_HEADER_SIZE;
        if (pos + (int)slen > len) {
            return -1; /* payload runs past the buffer */
        }
        if (ch < CHANNEL_COUNT && slen <= m->ch[ch].segSize) {
            channelApplySegment(m, ch, seq, buf + pos, slen);
        }
        pos += slen;
    }
    return pos;
}

bool channelReceive(ChannelMux *m, uint8_t ch, uint8_t *out, uint16_t *outLen) {
    if (m == NULL || ch >= CHANNEL_COUNT || out == NULL || outLen == NULL) {
        return false;
    }
    ChannelState *c = &m->ch[ch];
    uint32_t idx = c->expectedSeq % c->window;
    if (!c->recvPresent[idx]) {
        return false; /* gap not yet filled — nothing to deliver in order */
    }
    *outLen = c->recvLen[idx];
    if (c->recvLen[idx] > 0) {
        memcpy(out, c->recvData + idx * c->segSize, c->recvLen[idx]);
    }
    c->recvPresent[idx] = false;
    c->expectedSeq++;
    return true;
}

bool channelReceiveBestEffort(ChannelMux *m, uint8_t ch, uint8_t *out,
                              uint16_t *outLen) {
    if (m == NULL || ch >= CHANNEL_COUNT || out == NULL || outLen == NULL) {
        return false;
    }
    ChannelState *c = &m->ch[ch];
    if (!c->bestEffort) {
        return false; /* usage error: not a best-effort channel */
    }
    /* Return the lowest buffered seq at or above the cursor, advancing past
     * any skipped (never-arrived) seqs below it — no waiting on a gap. */
    uint32_t seq;
    for (seq = c->expectedSeq; seq < c->expectedSeq + c->window; seq++) {
        uint32_t idx = seq % c->window;
        if (c->recvPresent[idx]) {
            *outLen = c->recvLen[idx];
            if (c->recvLen[idx] > 0) {
                memcpy(out, c->recvData + idx * c->segSize, c->recvLen[idx]);
            }
            c->recvPresent[idx] = false;
            c->expectedSeq = seq + 1;
            return true;
        }
    }
    return false; /* nothing buffered in the window */
}

uint32_t channelResetSend(ChannelMux *m, uint8_t ch) {
    if (m == NULL || ch >= CHANNEL_COUNT) {
        return 0;
    }
    ChannelState *c = &m->ch[ch];
    /* Clear the live (unacked) ring entries, then collapse the window: the
     * tail is gone, so channelBuildFrame finds nothing to retransmit. The
     * loop spans at most one window since live seqs are always contiguous. */
    uint32_t seq;
    for (seq = c->ackedSeq; seq < c->nextSeq; seq++) {
        c->sendLen[seq % c->window] = 0;
    }
    c->ackedSeq = c->nextSeq;
    c->txNext = c->nextSeq;
    c->nakIssued = false; /* no outstanding stall after a send reset */
    /* The stream channel also carries un-segmentized bytes in the shared
     * staging buffer; collapsing the window without dropping them would let a
     * stale tail segmentize into the post-reset sequence space and corrupt the
     * re-based stream. Drop the pending bytes so the reset is complete. */
    if (ch == CHANNEL_BULK) {
        m->streamHead = 0;
        m->streamCount = 0;
    }
    return c->nextSeq;
}

void channelResetExpected(ChannelMux *m, uint8_t ch, uint32_t newExpected) {
    if (m == NULL || ch >= CHANNEL_COUNT) {
        return;
    }
    ChannelState *c = &m->ch[ch];
    if (newExpected <= c->expectedSeq) {
        return; /* no rewind: at or below the current baseline is a no-op */
    }
    /* Discard buffered segments below the new baseline. Only seqs in
     * [expectedSeq, expectedSeq + window) can be present, so capping the
     * clear at the window bounds the loop and never touches a slot holding a
     * still-deliverable seq >= newExpected. */
    uint32_t clearEnd = newExpected;
    if (clearEnd > c->expectedSeq + c->window) {
        clearEnd = c->expectedSeq + c->window;
    }
    uint32_t seq;
    for (seq = c->expectedSeq; seq < clearEnd; seq++) {
        c->recvPresent[seq % c->window] = false;
    }
    c->expectedSeq = newExpected;
    if (c->recvHighestSeq < c->expectedSeq) {
        c->recvHighestSeq = c->expectedSeq; /* no false gap across the reset */
    }
}
