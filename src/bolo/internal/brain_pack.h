/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Brain pack
 *Filename:      brain_pack.h
 *Purpose:
 *  The fast way a Bot Host gets a server's brain files
 *  (brain_sync.h has the manifest and the cache).
 *
 *  A host that can take it says so with
 *  CTRL_BOT_HOST_SYNC_CAPS. The server then sends it the v2
 *  manifest, which carries each file's SHA-256. The host
 *  takes every file it can from its own install of the
 *  brain (only read, never written) and from its cache, and
 *  asks for the rest with one CTRL_BOT_HOST_PACK_REQ per
 *  brain: a bitmap of the files it needs. The server sends
 *  those files as one zstd stream per brain, in
 *  BULK_KIND_BRAIN_PACK pieces. The host checks each file's
 *  size, hash and SHA-256 before it uses it; a file that
 *  fails is asked for again the old way, one file at a
 *  time. A host or server without this keeps the old way.
 *
 *  The raw stream, before zstd:
 *    'B' 'P' 1 [brain 1][count 2]
 *    per file, in rising file order: [file 2][size 4][bytes]
 *
 *  A player's own Bot Host (the one inside a client) keeps
 *  the files it receives in a session folder for that
 *  server connection only, bothost_cache/session/<id>/, and
 *  removes it when the connection ends. The marker file in
 *  it is what lets a later start remove a folder a crash
 *  left: only a folder with the marker is ever removed, and
 *  only when no running program holds its lock file (two
 *  clients can run on one computer at once).
 *********************************************************/

#ifndef BRAIN_PACK_H
#define BRAIN_PACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "brain_sync.h"

/* zstd level of the stream. */
#define BRAIN_PACK_LEVEL        9
/* The most compressed bytes in one BULK_KIND_BRAIN_PACK piece. */
#define BRAIN_PACK_PIECE_MAX    (128u * 1024u)
/* The file bitmap of a CTRL_BOT_HOST_PACK_REQ: one bit per file. */
#define BRAIN_PACK_BITMAP_BYTES (BRAIN_SYNC_MAX_FILES / 8)

/* CTRL_BOT_HOST_SYNC_CAPS bits. */
#define BRAIN_PACK_CAP_PACKED   0x01u
/* The host runs only the brains of its own install (a player's built-in
 * host built with WINBOLO_BOTHOST_OFFICIAL_ONLY): the server places a bot
 * there only when the host's pack request for its brain asked for no file. */
#define BRAIN_PACK_CAP_OFFICIAL_ONLY 0x02u
/* Sent again with the other bits when the host drops a download (its
 * player set Max slots 0): the server drops the files it was sending
 * that host and the pack requests it holds. */
#define BRAIN_PACK_CAP_RESET    0x04u
/* The host takes bots that see the whole map (an AI level other than
 * aiYes): sent when it is built with WINBOLO_BOTHOST_ALLOW_FULL_MAP. A
 * player's built-in host gets such a bot only with this bit; a host that
 * does not send it (an older one, or one built with the option OFF) never
 * gets one. */
#define BRAIN_PACK_CAP_FULL_MAP 0x08u
/* Ticks (50 a second) the server waits for a new host's caps before it
 * sends the v1 manifest, so an older host is not kept waiting long. */
#define BRAIN_PACK_CAPS_WAIT_TICKS 100

typedef enum {
    BRAIN_PACK_MANIFEST_WAIT,   /* no caps yet; ask again next tick */
    BRAIN_PACK_MANIFEST_V1,     /* the old way */
    BRAIN_PACK_MANIFEST_V2      /* the packed way */
} BrainPackManifestChoice;

/* Which manifest a server sends a host: v2 once the host's caps say
 * BRAIN_PACK_CAP_PACKED, v1 when the caps say no or none came in
 * BRAIN_PACK_CAPS_WAIT_TICKS (an older host sends none). */
BrainPackManifestChoice brainPackManifestChoice(bool capsSeen, uint8_t caps,
                                                uint32_t ticksSinceJoin);

/* ── Bitmaps ───────────────────────────────────────────────────────────── */

void brainPackBitSet(uint8_t *bits, int i);
bool brainPackBitGet(const uint8_t *bits, int i);
int  brainPackBitCount(const uint8_t *bits, int n);

/* The bitmap of brain b's files on plan->need. */
void brainPackBitsFromPlan(const BrainSyncPlan *plan, int b,
                           uint8_t bits[BRAIN_PACK_BITMAP_BYTES]);

/* ── Writer (server) ───────────────────────────────────────────────────── */

typedef struct BrainPackWriter BrainPackWriter;

/* A stream of count files of brain number brain. NULL when out of memory. */
BrainPackWriter *brainPackWriterNew(int level, uint8_t brain, uint16_t count);
/* The record header of the next file; then its size bytes with Put. */
bool brainPackWriterBeginFile(BrainPackWriter *w, uint16_t file, uint32_t size);
bool brainPackWriterPut(BrainPackWriter *w, const void *data, size_t len);
/* The end of the stream. */
bool brainPackWriterFinish(BrainPackWriter *w);
/* Compressed bytes ready to send. */
size_t brainPackWriterPending(const BrainPackWriter *w);
/* Take up to cap of them. Returns the count taken. */
size_t brainPackWriterTake(BrainPackWriter *w, uint8_t *dst, size_t cap);
/* Finish was called (bytes may still wait to be taken). */
bool brainPackWriterFinished(const BrainPackWriter *w);
/* Finished and every byte taken. */
bool brainPackWriterDone(const BrainPackWriter *w);
uint64_t brainPackWriterRawBytes(const BrainPackWriter *w);
uint64_t brainPackWriterOutBytes(const BrainPackWriter *w);
void brainPackWriterFree(BrainPackWriter *w);

/* ── Reader (host) ─────────────────────────────────────────────────────── */

/* A file of the stream, whole. Its size is the manifest's; the callee
 * checks the bytes (brainSyncDataIs) and returns false to refuse them. */
typedef bool (*BrainPackFileFn)(void *ctx, int file, const uint8_t *data,
                                uint32_t size);

typedef enum {
    BRAIN_PACK_MORE,   /* fine so far; feed the next piece */
    BRAIN_PACK_DONE,   /* every file came and the stream ended */
    BRAIN_PACK_BAD     /* damaged, or not the files asked for */
} BrainPackStatus;

typedef struct BrainPackReader BrainPackReader;

/* A reader for the files of br (brain number brain) set in bits. br must
 * live as long as the reader. */
BrainPackReader *brainPackReaderNew(const BrainSyncBrain *br, uint8_t brain,
                                    const uint8_t bits[BRAIN_PACK_BITMAP_BYTES]);
/* Feed the next compressed piece. fn is called for each whole file. After
 * BRAIN_PACK_BAD or BRAIN_PACK_DONE more input is refused. */
BrainPackStatus brainPackReaderFeed(BrainPackReader *r, const uint8_t *data,
                                    size_t len, BrainPackFileFn fn, void *ctx);
/* Files given to fn so far. */
int brainPackReaderGot(const BrainPackReader *r);
void brainPackReaderFree(BrainPackReader *r);

/* ── Session folders (the Bot Host inside a client) ────────────────────── */

#define BRAIN_SESSION_MARKER "session.marker"
#define BRAIN_SESSION_LOCK   "session.lock"

/* A session folder and the lock its program holds while it runs. */
typedef struct {
    char     dir[600];   /* "<cacheRoot>/session/<16 hex>"; empty: none */
    intptr_t lock;       /* the open lock file, or -1 */
} BrainSession;

/* An empty session (no folder, no lock). */
void brainSessionInit(BrainSession *s);
/* Make "<cacheRoot>/session/<16 hex random>": the folder, then its lock
 * file opened and held, then the marker. */
bool brainSessionCreate(const char *cacheRoot, BrainSession *s);
/* Let go of the lock and keep the folder (a test does this to stand in for
 * a program that ended without its clean-up). */
void brainSessionUnlock(BrainSession *s);
/* Let go of the lock and remove the folder. True when it is gone. */
bool brainSessionEnd(const char *cacheRoot, BrainSession *s);
/* Remove each session folder under cacheRoot that has the marker and whose
 * lock no running program holds: the lock can be taken, so its owner is
 * gone. A folder without the marker is kept, a folder whose lock is held
 * is kept, and nothing outside "<cacheRoot>/session" is looked at.
 * Returns the folders removed. */
int brainSessionSweep(const char *cacheRoot);

#endif /* BRAIN_PACK_H */
