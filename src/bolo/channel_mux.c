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
 *   repeat ackCount:  channelId u8, ackedSeq u32   (exclusive: peer has
 *                                                    received every seq <
 *                                                    ackedSeq on that channel)
 *   segCount : u8
 *   repeat segCount:  channelId u8, seq u32, len u16, payload[len]
 *
 * Per-record sizes used by the parser bounds checks. */
#define CHANNEL_ACK_RECORD_SIZE 5  /* channelId(1) + ackedSeq(4)           */
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
    if (m->streamCount + len > CHANNEL_STREAM_BUF) {
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
