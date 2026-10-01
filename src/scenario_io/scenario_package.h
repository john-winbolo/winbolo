/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Scenario Package
 *Filename:      scenario_package.h
 *Author:        John Morrison
 *Purpose:
 *  The WBSC container: a four-byte magic, a version, a
 *  length and a ZIP archive holding the manifest, the
 *  script and whatever brain directories a scenario ships
 *  with.
 *
 *  Both ends work in memory. The reader takes a byte
 *  buffer rather than a path, so the chunk appended to a
 *  map file, a package file on its own and a blob that
 *  arrived over the network are all read through one call,
 *  and the writer hands back one buffer to append or send.
 *
 *  Nothing here reads what an entry means. manifest.json is
 *  a byte string to this file, which only insists that it
 *  is present; main.lua and the brain entries are the same.
 *  The whole entry list comes back so a caller can say
 *  which names it did not recognise.
 *********************************************************/

#ifndef SCENARIO_PACKAGE_H
#define SCENARIO_PACKAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The four bytes a container starts with. */
#define SCN_PACKAGE_MAGIC "WBSC"

/* The container version this build writes and the only one it reads. */
#define SCN_PACKAGE_VERSION 1

/* Magic, u16 version and u32 length, all little-endian, ahead of the
 * archive. A buffer shorter than this cannot be a container. */
#define SCN_PACKAGE_HEADER_LEN 10

/* The manifest every container carries, and the script one carries when it
 * has a script. Names inside the archive. */
#define SCN_PACKAGE_MANIFEST_ENTRY "manifest.json"
#define SCN_PACKAGE_SCRIPT_ENTRY   "main.lua"

/* The prefix a brain's files live under: brains/NAME/... */
#define SCN_PACKAGE_BRAIN_PREFIX "brains/"

/* The most manifest.json may inflate to. Every read of a manifest is held to
 * this, because a manifest is the one entry read before anything about the
 * container is known and the entry every reader opens.
 *
 * It is JSON naming a scenario, its lobby teams, its rules, its tags and its
 * regions; the reference manifests in this tree are a couple of kilobytes and
 * the shapes the parser will accept are bounded well inside that. 64 KiB is
 * far more than one can honestly need and small enough that a container
 * declaring a gigabyte of manifest is refused rather than allocated for. */
#define SCN_PACKAGE_MANIFEST_MAX_BYTES (64u * 1024u)

typedef struct ScnPackage ScnPackage;

/* Open a WBSC container held in memory. bytes must start at the magic.
   The caller's buffer must outlive the handle: nothing is copied. */
ScnPackage *scnPackageOpen(const uint8_t *bytes, size_t len,
                           char *err, size_t errLen);
void        scnPackageClose(ScnPackage *p);

/* One entry's content, up to maxBytes of it. *outBytes is malloc'd and the
   caller frees it. It carries a 0 byte past the content, which *outLen does
   not count, so text entries can be read as C strings without a copy.

   maxBytes is what the caller will do something with, and the entry is
   measured against it before a byte is allocated: an archive is a handful of
   bytes that can declare any size at all, so without the cap a container of
   forty bytes asks for whatever its header says. The inflate is held to the
   same figure, so a header that understates its entry cannot write past what
   was allocated for it either.

   False for an entry that is not there, that is above maxBytes, or whose data
   does not come back whole — the CRC is checked as the entry is closed. err
   carries the reason where there is one to give; it is left empty for an
   entry that is simply not in the container, which several callers treat as
   an ordinary answer rather than a fault. err may be NULL only when errLen
   is 0. */
bool scnPackageReadEntry(ScnPackage *p, const char *name, size_t maxBytes,
                         uint8_t **outBytes, size_t *outLen,
                         char *err, size_t errLen);
bool scnPackageHasEntry(const ScnPackage *p, const char *name);

/* Every entry the archive holds, in archive order, so a validator can
   warn about the ones nothing reads. */
int         scnPackageEntryCount(const ScnPackage *p);
const char *scnPackageEntryName(const ScnPackage *p, int i);

/* The distinct NAME in brains/NAME/, deduplicated, in first-seen order. */
int         scnPackageBrainCount(const ScnPackage *p);
const char *scnPackageBrainName(const ScnPackage *p, int i);

typedef struct {
    const char    *name;
    const uint8_t *bytes;
    size_t         len;
    bool           deflate;   /* false stores the entry */
} ScnPackageEntry;

/* Build a framed container. *outBytes is malloc'd and the caller frees it. */
bool scnPackageWrite(const ScnPackageEntry *entries, int count,
                     uint8_t **outBytes, size_t *outLen,
                     char *err, size_t errLen);

/* The WBSC chunk appended to a map file, if there is one. Points into the
   caller's buffer; nothing is copied, and the buffer must outlive the use.
   False when the file is not a map, or is a map with nothing after it. */
bool scnPackageFindInMap(const uint8_t *file, size_t len,
                         const uint8_t **outChunk, size_t *outChunkLen);

#endif /* SCENARIO_PACKAGE_H */
