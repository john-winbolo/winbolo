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
 *Filename:      bulk_transfer.h
 *Author:        John Morrison
 *Purpose:
 *  Framing for a sized blob carried over the stream-flavor
 *  bulk channel (CHANNEL_BULK). A small app-level header
 *  precedes the bytes; one receive-side state machine
 *  reassembles header-then-body out of the in-order stream
 *  fragments the channel delivers; one send-side helper
 *  feeds the blob into the channel's staging buffer as the
 *  window drains, serialized so two transfers never
 *  interleave on one peer's byte stream.
 *
 *  Recipient-agnostic: the receiver hands a parsed header to
 *  a sink that decides where the blob lands and what to do
 *  on completion, so map preview, upload, and download can
 *  all ride the same machinery.
 *
 *  T2 (sim internals): includable within src/bolo/,
 *  src/server/, and tests/unit/ only.
 *********************************************************/

#ifndef BULK_TRANSFER_H
#define BULK_TRANSFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "channel_mux.h"   /* ChannelMux, CHANNEL_BULK, CHANNEL_STREAM_BUF */
#include "scenario_details.h"  /* SCN_DETAILS_MAX */
#include "scenario_settings.h" /* SCN_SETTINGS_BLOB_MAX */
#include "scenario_identity.h" /* SCN_IDENTITY_BLOB_MAX */
#include "brain_list.h"        /* BRAIN_DOCS_Z_MAX */

/* Transfer kinds carried in the stream header's first byte: a map preview
 * (server->client, lobby chooser), an upload (client->server), a join map
 * download (server->client), and a live map resync (server->client, desync
 * recovery — distinguished from DOWNLOAD so the receiver routes it to the
 * parallel resync buffer and applies the generation gate). A spectator seed
 * (server->client) carries the delayed keyframe blob that seeds a spectator's
 * view before the forward event feed begins; a spectator record (server->client)
 * carries one delayed ring record (event or keyframe) of the forward feed that
 * follows the seed. */
enum {
    BULK_KIND_UPLOAD      = 1,
    BULK_KIND_DOWNLOAD    = 2,
    BULK_KIND_PREVIEW     = 3,
    BULK_KIND_RESYNC      = 4,
    BULK_KIND_SPEC_SEED   = 5,
    BULK_KIND_SPEC_RECORD = 6,
    /* Server->spectator one-shot at the delayed->live drain-flip: the
     * current-session lobby-chat backlog, serialized as a run of
     * [type u8][bodyLen u16 BE][body] control-event records. Sent on
     * CHANNEL_BULK so the ≤200-event burst never pressures the reliable
     * control window (which the same-tick sync replay already fills). */
    BULK_KIND_LOBBY_CHAT_BACKLOG = 7,
    /* Server->client one-shot, sent in answer to PACKET_ROUND_LOG_REQ: the
     * last completed round's .wbv bytes, so a joined client's lobby recap can
     * replay a round it did not host and therefore has no local copy of. The
     * header's gen echoes the request's reqSeq (a stale reply is droppable),
     * its path carries the log file's basename, and its totalSize is bounded
     * by ROUND_LOG_MAX_BYTES. */
    BULK_KIND_ROUND_LOG = 8,
    /* Server->client, sent in answer to PACKET_LOBBY_SCENARIO_DETAILS_REQ:
     * one script file's details. The path is the file the request named and
     * the blob is [status 1] then, for BULK_SCN_DETAILS_FOUND, the details
     * as scenario_details.h lays them out (which may be no bytes), and
     * nothing more for BULK_SCN_DETAILS_NOT_FOUND. So totalSize is 1 to
     * 1 + SCN_DETAILS_MAX, and a file the server does not know still gets an
     * answer the client can stop waiting on. */
    BULK_KIND_SCENARIO_DETAILS = 9,
    /* Server->client, sent in answer to PACKET_LOBBY_SCRIPT_FETCH_REQ: a copy
     * of one of the server's script files. The header's gen echoes the
     * request's reqSeq and its path is the file the request named. The blob
     * is [status 1] then, for BULK_SCRIPT_FOUND, the file's raw bytes (a
     * .scenario as its ZIP bytes, a .lua as its source), and nothing more
     * for any other status. So totalSize is 1 to
     * 1 + LOBBY_PACKAGE_UPLOAD_MAX_BYTES. */
    BULK_KIND_SCRIPT_PACKAGE = 10,
    /* Server->client, sent in answer to PACKET_LOBBY_BRAIN_DOCS_REQ: one
     * brain's commands.txt, zlib-compressed. The header's gen is the docs
     * generation the server holds, the one CTRL_LOBBY_BRAIN_ANNOUNCE
     * carries, and its path is the brain's index in decimal. The blob is
     * [status 1] then, for BULK_BRAIN_DOCS_FOUND, [rawLen 2 BE] and the
     * compressed bytes; nothing more for BULK_BRAIN_DOCS_NOT_FOUND. So
     * totalSize is 1 to BULK_BRAIN_DOCS_BLOB_MAX. */
    BULK_KIND_BRAIN_DOCS = 11,
    /* Server->client, sent in answer to PACKET_LOBBY_BOT_POOL_REQ: the
     * server's bot-name catalogue, as lobbyBotPoolsSerialize wrote it
     * (already zlib-compressed). The header's gen is the catalogue id
     * CTRL_LOBBY_BOT_POOL_INFO carries and its path is empty. The blob is
     * [status 1] then, for BULK_BOT_POOL_FOUND, the catalogue; nothing more
     * for BULK_BOT_POOL_NONE. So totalSize is 1 to
     * 1 + LOBBY_BOT_CATALOG_WIRE_MAX. */
    BULK_KIND_BOT_POOL = 12
};

/* The status byte that opens a BULK_KIND_BOT_POOL blob. */
#define BULK_BOT_POOL_FOUND 0
#define BULK_BOT_POOL_NONE  1   /* the server holds no themed pools */

/* The status byte that opens a BULK_KIND_BRAIN_DOCS blob. */
#define BULK_BRAIN_DOCS_FOUND     0
#define BULK_BRAIN_DOCS_NOT_FOUND 1   /* no such brain, or it ships no docs */

/* The largest BULK_KIND_BRAIN_DOCS blob. */
#define BULK_BRAIN_DOCS_BLOB_MAX (1 + 2 + BRAIN_DOCS_Z_MAX)

/* The status byte that opens a BULK_KIND_SCENARIO_DETAILS blob. */
#define BULK_SCN_DETAILS_FOUND     0
#define BULK_SCN_DETAILS_NOT_FOUND 1
/* Found, with the file's settings block (scenario_settings.h) behind the
 * details: [status 1][detailsLen 2 BE][details][settings to the end]. Sent
 * only to a client whose request ended with a flags byte that has
 * BULK_SCN_DETAILS_WANT_SETTINGS set, because an older client reads every
 * byte after the status as details and would refuse the blob. An older
 * server ignores the flags byte and answers BULK_SCN_DETAILS_FOUND. */
#define BULK_SCN_DETAILS_FOUND_V2  2
/* Found, with the settings block and who wrote the file behind the details:
 * [status 1][detailsLen 2 BE][details][settingsLen 2 BE][settings]
 * [identity blob (scenario_identity.h)]. A reader ignores any bytes after
 * the identity blob, so a later field can follow it. The settings length is
 * 0 when the request did not ask for them. Sent only to a client whose
 * flags byte has BULK_SCN_DETAILS_WANT_IDENTITY set; a server from before it
 * ignores that bit and answers V2 or FOUND, which such a client reads as
 * identity not known. */
#define BULK_SCN_DETAILS_FOUND_V3  3

/* The flags byte a request may end with, after the file name. */
#define BULK_SCN_DETAILS_WANT_SETTINGS 0x01u
#define BULK_SCN_DETAILS_WANT_IDENTITY 0x02u

/* The largest BULK_KIND_SCENARIO_DETAILS blob: the V3 shape, the longest. */
#define BULK_SCN_DETAILS_BLOB_MAX \
    (1 + 2 + SCN_DETAILS_MAX + 2 + SCN_SETTINGS_BLOB_MAX + \
     SCN_IDENTITY_BLOB_MAX)

/* The status byte that opens a BULK_KIND_SCRIPT_PACKAGE blob. */
#define BULK_SCRIPT_FOUND     0   /* the file's bytes follow               */
#define BULK_SCRIPT_NOT_FOUND 1   /* no such file, or the map's own script */
#define BULK_SCRIPT_DISABLED  2   /* the server does not share its scripts */
#define BULK_SCRIPT_TOO_LARGE 3   /* over LOBBY_PACKAGE_UPLOAD_MAX_BYTES    */
#define BULK_SCRIPT_BUSY      4   /* asked too soon after the last request */

/* App-level stream header that precedes a blob on CHANNEL_BULK. Big-endian on
 * the wire: kind(1) gen(4) totalSize(4) pathLen(1) path[pathLen]. */
#define BULK_PATH_MAX            255
#define BULK_STREAM_HEADER_FIXED 10   /* kind + gen + totalSize + pathLen */
#define BULK_STREAM_HEADER_MAX   (BULK_STREAM_HEADER_FIXED + BULK_PATH_MAX)

typedef struct {
    uint8_t  kind;
    uint32_t gen;        /* transfer generation / sequence id           */
    uint32_t totalSize;  /* blob byte count following the header         */
    uint8_t  pathLen;
    char     path[BULK_PATH_MAX + 1];  /* NUL-terminated                 */
} BulkStreamHeader;

/* Serialize h into buf (which must hold BULK_STREAM_HEADER_FIXED + h->pathLen
 * bytes). Returns the number of bytes written. */
int bulkPackStreamHeader(uint8_t *buf, const BulkStreamHeader *h);

/* Parse a stream header from buf. Returns the number of bytes consumed
 * (BULK_STREAM_HEADER_FIXED + pathLen) or 0 when avail is too short for the
 * fixed prefix or the announced path — never reads past avail. */
int bulkParseStreamHeader(const uint8_t *buf, size_t avail,
                          BulkStreamHeader *h);

/* ---- Per-direction sender: serializer guard + incremental feed ---- */

typedef struct {
    bool      busy;
    uint8_t   kind;     /* in-flight transfer kind (for priority decisions) */
    uint8_t  *buf;      /* malloc'd header+blob, freed at completion/reset  */
    uint32_t  total;    /* buf length                                       */
    uint32_t  offset;   /* bytes already handed to channelStreamSend        */
} BulkSender;

void bulkSenderInit(BulkSender *s);
/* Drop any in-flight transfer and free its buffer (disconnect / re-init). */
void bulkSenderReset(BulkSender *s);
bool bulkSenderBusy(const BulkSender *s);

/* Stage one transfer (header + blob) for incremental send. Rejected (returns
 * false, no state change) when a transfer is already in flight — the
 * serializer guard. Also returns false on allocation failure. */
bool bulkSenderBegin(BulkSender *s, const BulkStreamHeader *h,
                     const uint8_t *blob, uint32_t blobLen);

/* Feed as many staged bytes as the mux's stream buffer accepts, advancing on
 * success; frees the buffer and clears busy once the whole transfer has been
 * handed to the channel. Safe to call every tick; a no-op when idle. */
void bulkSenderPump(BulkSender *s, ChannelMux *m);

/* ---- Per-direction receiver: header + body state machine ---- */

/* The receiver is recipient-agnostic: on a fully parsed header it asks the
 * sink where the blob should land, and tells the sink when the blob is
 * complete. */
typedef struct {
    /* A full header was parsed (IDLE -> BODY). Return a buffer of at least
     * h->totalSize bytes to receive the blob, or NULL to reject this transfer
     * (its body is consumed and discarded so the stream stays aligned).
     *
     * SECURITY: h->totalSize is WIRE-SUPPLIED and ATTACKER-CONTROLLED (up to
     * 4 GB). The receiver's body fill trusts it as the buffer bound, so the
     * sink MUST validate and bound totalSize itself and return NULL to reject
     * anything it will not allocate (e.g. the upload sink caps it at
     * LOBBY_MAP_UPLOAD_MAX_BYTES). A sink that allocates blindly off totalSize
     * hands a remote peer the allocation size. */
    uint8_t *(*onBegin)(void *ctx, const BulkStreamHeader *h);
    /* h->totalSize body bytes have been written into the buffer onBegin
     * returned. */
    void     (*onComplete)(void *ctx, const BulkStreamHeader *h, uint8_t *buf);
    void     *ctx;
} BulkRecvSink;

typedef enum { BULK_RECV_IDLE = 0, BULK_RECV_BODY } BulkRecvState;

typedef struct {
    BulkRecvState    state;
    uint8_t          hdrBuf[BULK_STREAM_HEADER_MAX];
    uint32_t         hdrLen;        /* header bytes accumulated in IDLE */
    BulkStreamHeader hdr;           /* parsed header, valid in BODY     */
    uint8_t         *dst;           /* onBegin's buffer; NULL = rejected */
    uint32_t         bodyReceived;  /* blob bytes accumulated in BODY    */
} BulkReceiver;

void bulkReceiverInit(BulkReceiver *r);

/* Feed one stream fragment (the bytes channelReceive popped from CHANNEL_BULK)
 * through the state machine, firing sink callbacks as headers and blobs
 * complete. Handles a header that spans fragments, a fragment carrying the
 * tail of one transfer and the head of the next, and rejected transfers. */
void bulkReceiverFeed(BulkReceiver *r, const uint8_t *data, uint32_t len,
                      const BulkRecvSink *sink);

#endif /* BULK_TRANSFER_H */
