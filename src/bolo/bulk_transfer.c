/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*********************************************************
 *Name:          Bulk Transfer
 *Filename:      bulk_transfer.c
 *Author:        John Morrison
 *Purpose:       Stream header framing, send-side serializer with
 *               incremental feed, and the receive-side reassembly
 *               state machine for blobs carried on CHANNEL_BULK.
 *********************************************************/

#include <stdlib.h>
#include <string.h>

#include "bulk_transfer.h"
#include "transport_udp_internal.h"   /* packU16/packU32/unpackU16/unpackU32 */

int bulkPackStreamHeader(uint8_t *buf, const BulkStreamHeader *h) {
    int pos = 0;
    buf[pos++] = h->kind;
    packU32(buf + pos, h->gen);
    pos += 4;
    packU32(buf + pos, h->totalSize);
    pos += 4;
    buf[pos++] = h->pathLen;
    if (h->pathLen > 0) {
        memcpy(buf + pos, h->path, h->pathLen);
        pos += h->pathLen;
    }
    return pos;
}

int bulkParseStreamHeader(const uint8_t *buf, size_t avail,
                          BulkStreamHeader *h) {
    if (avail < BULK_STREAM_HEADER_FIXED) {
        return 0;
    }
    uint8_t pathLen = buf[9];
    size_t need = (size_t)BULK_STREAM_HEADER_FIXED + pathLen;
    if (avail < need) {
        return 0;
    }
    h->kind = buf[0];
    h->gen = unpackU32(buf + 1);
    h->totalSize = unpackU32(buf + 5);
    h->pathLen = pathLen;
    if (pathLen > 0) {
        memcpy(h->path, buf + BULK_STREAM_HEADER_FIXED, pathLen);
    }
    h->path[pathLen] = '\0';
    return (int)need;
}

/* ---- sender ---- */

void bulkSenderInit(BulkSender *s) {
    memset(s, 0, sizeof(*s));
}

void bulkSenderReset(BulkSender *s) {
    if (s->buf != NULL) {
        free(s->buf);
    }
    memset(s, 0, sizeof(*s));
}

bool bulkSenderBusy(const BulkSender *s) {
    return s->busy;
}

bool bulkSenderBegin(BulkSender *s, const BulkStreamHeader *h,
                     const uint8_t *blob, uint32_t blobLen) {
    if (s->busy) {
        return false;   /* serializer guard: a transfer is already in flight */
    }
    uint32_t headerLen = (uint32_t)BULK_STREAM_HEADER_FIXED + h->pathLen;
    /* Defense-in-depth on a generic primitive: every current caller bounds its
     * blob (e.g. the upload sink clamps to LOBBY_MAP_UPLOAD_MAX_BYTES), so this
     * never trips today. Reject rather than wrap if header+blob would overflow
     * the 32-bit allocation size — same no-state-change failure path as the
     * serializer guard above. */
    if (blobLen > UINT32_MAX - headerLen) {
        return false;
    }
    uint32_t total = headerLen + blobLen;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (buf == NULL) {
        return false;
    }
    bulkPackStreamHeader(buf, h);
    if (blobLen > 0 && blob != NULL) {
        memcpy(buf + headerLen, blob, blobLen);
    }
    s->busy = true;
    s->kind = h->kind;
    s->buf = buf;
    s->total = total;
    s->offset = 0;
    return true;
}

void bulkSenderPump(BulkSender *s, ChannelMux *m) {
    if (!s->busy) {
        return;
    }
    uint32_t avail = (m->streamCount < CHANNEL_STREAM_BUF)
                         ? (CHANNEL_STREAM_BUF - m->streamCount)
                         : 0u;
    uint32_t remaining = s->total - s->offset;
    uint32_t n = (remaining < avail) ? remaining : avail;
    if (n > 0) {
        if (channelStreamSend(m, CHANNEL_BULK, s->buf + s->offset, n)) {
            s->offset += n;
        }
    }
    if (s->offset >= s->total) {
        free(s->buf);
        s->buf = NULL;
        s->busy = false;
        s->total = 0;
        s->offset = 0;
    }
}

/* ---- receiver ---- */

void bulkReceiverInit(BulkReceiver *r) {
    memset(r, 0, sizeof(*r));
    r->state = BULK_RECV_IDLE;
}

void bulkReceiverFeed(BulkReceiver *r, const uint8_t *data, uint32_t len,
                      const BulkRecvSink *sink) {
    uint32_t i = 0;
    while (i < len) {
        if (r->state == BULK_RECV_IDLE) {
            /* Accumulate the fixed prefix first so pathLen is known. */
            if (r->hdrLen < BULK_STREAM_HEADER_FIXED) {
                uint32_t want = (uint32_t)BULK_STREAM_HEADER_FIXED - r->hdrLen;
                uint32_t take = (want < len - i) ? want : len - i;
                memcpy(r->hdrBuf + r->hdrLen, data + i, take);
                r->hdrLen += take;
                i += take;
                if (r->hdrLen < BULK_STREAM_HEADER_FIXED) {
                    break;   /* fragment exhausted mid prefix */
                }
            }
            /* Then the variable-length path. */
            uint32_t need = (uint32_t)BULK_STREAM_HEADER_FIXED + r->hdrBuf[9];
            if (r->hdrLen < need) {
                uint32_t want = need - r->hdrLen;
                uint32_t take = (want < len - i) ? want : len - i;
                memcpy(r->hdrBuf + r->hdrLen, data + i, take);
                r->hdrLen += take;
                i += take;
                if (r->hdrLen < need) {
                    break;   /* header spans into a later fragment */
                }
            }
            /* Full header in hand. */
            bulkParseStreamHeader(r->hdrBuf, r->hdrLen, &r->hdr);
            r->hdrLen = 0;
            r->bodyReceived = 0;
            r->dst = (sink != NULL && sink->onBegin != NULL)
                         ? sink->onBegin(sink->ctx, &r->hdr)
                         : NULL;
            if (r->hdr.totalSize == 0) {
                /* Zero-length blob: complete immediately. */
                if (r->dst != NULL && sink != NULL && sink->onComplete != NULL) {
                    sink->onComplete(sink->ctx, &r->hdr, r->dst);
                }
                r->dst = NULL;
                r->state = BULK_RECV_IDLE;
            } else {
                r->state = BULK_RECV_BODY;
            }
        } else { /* BULK_RECV_BODY */
            uint32_t remaining = r->hdr.totalSize - r->bodyReceived;
            uint32_t take = (remaining < len - i) ? remaining : len - i;
            if (r->dst != NULL) {
                memcpy(r->dst + r->bodyReceived, data + i, take);
            }
            r->bodyReceived += take;
            i += take;
            if (r->bodyReceived >= r->hdr.totalSize) {
                if (r->dst != NULL && sink != NULL && sink->onComplete != NULL) {
                    sink->onComplete(sink->ctx, &r->hdr, r->dst);
                }
                r->dst = NULL;
                r->bodyReceived = 0;
                r->state = BULK_RECV_IDLE;
            }
        }
    }
}
