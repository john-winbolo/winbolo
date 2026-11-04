/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Brain sync
 *Filename:      brain_sync.h
 *Purpose:
 *  Lets a Bot Host run exactly the brain code its server
 *  runs. The server lists, for each brain it may assign,
 *  every file its own brain loader reads (a manifest: the
 *  path, size and hash of each file and a version hash of
 *  the whole brain). The Bot Host keeps one cache folder per
 *  brain version, bothost_cache/<brain>/<version>/, and
 *  downloads only the files it does not already hold in
 *  some version folder of that brain.
 *
 *  Which files a brain is made of: the files the server's
 *  own loader reads. Without brain debug mode a brain that
 *  ships opt/init.lua runs from opt/, so it is every file
 *  under opt/ plus the files at the top of the brain folder
 *  (data such as shield_stamp_cache.bin that the brain opens
 *  from there), less the top-level .lua files that opt/
 *  holds its own copy of. Any other brain, or any brain in
 *  debug mode, is every file under its folder. Names that
 *  open with a dot are left out, as are files and folders
 *  the path check refuses.
 *
 *  Paths in a manifest use '/' and are relative to the brain
 *  folder. brainSyncPathSafe refuses anything that could
 *  name a place outside the folder: an absolute path, a
 *  drive letter, "..", a backslash, and the Windows device
 *  names.
 *
 *  The hashes are 64-bit FNV-1a. They find changed and
 *  damaged files; they are not a defence against a server
 *  that lies, which the host trusts anyway since it runs the
 *  server's code.
 *********************************************************/

#ifndef BRAIN_SYNC_H
#define BRAIN_SYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "brain_list.h"   /* BRAIN_LIST_MAX, BRAIN_LIST_NAME_LEN */

#define BRAIN_SYNC_MAX_BRAINS      BRAIN_LIST_MAX
#define BRAIN_SYNC_MAX_FILES       1024        /* per brain */
#define BRAIN_SYNC_PATH_MAX        160         /* relative path, NUL included */
#define BRAIN_SYNC_FILE_MAX_BYTES  (64u * 1024u * 1024u)
#define BRAIN_SYNC_TOTAL_MAX_BYTES (512u * 1024u * 1024u)
/* The largest encoded manifest a reader accepts. */
#define BRAIN_SYNC_MANIFEST_MAX    (2u * 1024u * 1024u)
/* Files go to the host in pieces of this many raw bytes, each compressed on
 * its own, so the server never compresses a whole large file in one tick. */
#define BRAIN_SYNC_CHUNK_BYTES     (128u * 1024u)

typedef struct {
    char     path[BRAIN_SYNC_PATH_MAX];  /* relative, '/' separated */
    uint32_t size;
    uint64_t hash;
    int64_t  mtime;    /* local only: lets a rescan skip a file it hashed */
} BrainSyncFile;

typedef struct {
    char           name[BRAIN_LIST_NAME_LEN];
    uint64_t       version;    /* hash over every file's path, size, hash */
    uint32_t       totalBytes;
    int            fileCount;
    BrainSyncFile *files;      /* fileCount entries, sorted by path */
    char           dir[512];   /* local only: the folder the files are in */
} BrainSyncBrain;

typedef struct {
    int            brainCount;
    bool           debugMode;  /* the server loads brains in debug mode */
    uint64_t       version;    /* hash over every brain's name and version */
    BrainSyncBrain brains[BRAIN_SYNC_MAX_BRAINS];
} BrainSyncManifest;

/* 64-bit FNV-1a over len bytes, carrying on from h (start from
 * BRAIN_SYNC_HASH_SEED). */
#define BRAIN_SYNC_HASH_SEED 0xcbf29ce484222325ull
uint64_t brainSyncHash(uint64_t h, const void *data, size_t len);

/* True when rel is a safe relative path: 1..BRAIN_SYNC_PATH_MAX-1 bytes of
 * printable ASCII, '/' separated, no empty, "." or ".." part, no part that
 * opens with a dot or ends with a dot or a space, no backslash or colon, and
 * no Windows device name (CON, NUL, COM1 and so on, with or without an
 * extension). */
bool brainSyncPathSafe(const char *rel);

/* True when name is one safe path part (a brain name or a version folder). */
bool brainSyncNameSafe(const char *name);

/* The version as 16 lower-case hex digits and a NUL. */
void brainSyncVersionHex(uint64_t version, char out[17]);

/* Fill b from the brain folder dir. prev, when not NULL, is the last list
 * made for the same brain: a file whose size and change time are the same
 * keeps its hash without being read again. False when the folder has too
 * many files or bytes, or a file cannot be read. */
bool brainSyncCollectBrain(BrainSyncBrain *b, const char *name,
                           const char *dir, bool debugMode,
                           const BrainSyncBrain *prev);

/* Build the manifest for every brain in the catalogue (brainListScan).
 * prev, when not NULL, is the last manifest, for the hash reuse above. A
 * brain whose files cannot be listed is left out. */
void brainSyncBuildManifest(BrainSyncManifest *out, bool debugMode,
                            const BrainSyncManifest *prev);

/* Work out a brain's version and the manifest's version from the file
 * lists. brainSyncCollectBrain and the decoder call this; a test that fills
 * a manifest by hand calls it too. */
void brainSyncComputeVersions(BrainSyncManifest *m);

/* Free the file lists and zero the manifest. */
void brainSyncFree(BrainSyncManifest *m);

/* The brain with this name, or NULL. */
const BrainSyncBrain *brainSyncFindBrain(const BrainSyncManifest *m,
                                         const char *name);

/* Encode the manifest. Returns a malloc'd buffer the caller frees, with its
 * length in *outLen, or NULL.
 *
 * Wire form, big-endian:
 *   'B' 'S' 1  [flags 1: bit 0 debug mode]  [brainCount 1]  [version 8]
 *   per brain: [nameLen 1][name][version 8][fileCount 2]
 *     per file: [pathLen 1][path][size 4][hash 8] */
uint8_t *brainSyncManifestEncode(const BrainSyncManifest *m, size_t *outLen);

/* Decode a manifest. Every name and path must pass the checks above, the
 * counts and sizes must be in range, the files of a brain must be in
 * strictly rising path order, and each version must match the one worked
 * out from the lists. On false *out is left empty. */
bool brainSyncManifestDecode(const uint8_t *buf, size_t len,
                             BrainSyncManifest *out);

/* ── Bot Host cache ────────────────────────────────────────────────────── */

/* "<root>/<brain>/<version hex>" into out. False when the brain name is not
 * safe or the path does not fit. */
bool brainSyncCacheDir(char *out, size_t outSz, const char *root,
                       const char *brain, uint64_t version);

/* "<root>/<brain>/<version hex>/<rel>" into out, with the same checks and a
 * path check on rel. */
bool brainSyncCacheFile(char *out, size_t outSz, const char *root,
                        const char *brain, uint64_t version, const char *rel);

/* True when the file at path has this size and hash. */
bool brainSyncFileMatches(const char *path, uint32_t size, uint64_t hash);

/* Write len bytes to path through a ".part" file and a rename, making the
 * folders on the way. The caller has checked the path. */
bool brainSyncWriteFile(const char *path, const void *data, size_t len);

/* What a host must do to hold every file of a manifest. */
typedef struct {
    int      haveCount;     /* files already in place */
    int      copyCount;     /* files copied from another version folder */
    int      needCount;     /* files to download */
    uint32_t needBytes;     /* their raw size */
    /* need[i] = (brain << 16) | file, for i < needCount. */
    uint32_t *need;
} BrainSyncPlan;

/* Check every file of every brain in m against the cache under root. A file
 * already in its version folder counts as held; a file with the same path,
 * size and hash in another version folder of the same brain is copied in;
 * everything else is listed to download. False only when a path is refused
 * or memory runs out. The caller frees plan->need. */
bool brainSyncPlanCache(const BrainSyncManifest *m, const char *root,
                        BrainSyncPlan *plan);

/* Compress one chunk for the wire: [rawLen 4 BE][zlib stream]. Returns a
 * malloc'd buffer the caller frees, or NULL. */
uint8_t *brainSyncChunkPack(const uint8_t *raw, uint32_t rawLen,
                            uint32_t *outLen);

/* Inflate a packed chunk into dst, which holds dstCap bytes. Returns the raw
 * length, or -1 when the chunk is damaged or larger than dstCap. */
int64_t brainSyncChunkUnpack(const uint8_t *buf, uint32_t len,
                             uint8_t *dst, uint32_t dstCap);

#endif /* BRAIN_SYNC_H */
