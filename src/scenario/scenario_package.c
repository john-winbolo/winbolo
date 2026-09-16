/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Scenario Package
 *Filename:      scenario_package.c
 *Author:        John Morrison
 *Purpose:
 *  Reads and writes the WBSC container: the ten framing
 *  bytes, then a ZIP archive driven through minizip's
 *  custom I/O functions so neither end ever touches a file.
 *
 *  Each handle carries its own reader context, held in the
 *  filefunc opaque field. A map's appended chunk and a
 *  package file can therefore be open at the same time,
 *  which is what the callers that compare the two need.
 *
 *  An entry name is a path once a brain is written to disk,
 *  so a name that could climb out of the directory it is
 *  extracted into is refused while the container is being
 *  opened, before any caller can ask for it.
 *
 *  Nothing here parses an entry. manifest.json is checked
 *  for and then handed back as bytes; the rest of the entry
 *  list is passed through untouched so a caller can warn
 *  about names it does not recognise.
 *********************************************************/

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zlib.h"
#include "unzip.h"
#include "zip.h"
#include "ioapi.h"

#include "bolo_map_validate.h" /* boloMapBodyLength — where a map file ends */

#include "scenario_package.h"

/* The most one minizip read or write call is handed. Both take an unsigned,
 * so a brain larger than that goes through several calls rather than being
 * refused. */
#define PKG_IO_CHUNK 0x40000u

/* What the entry-name array starts at and grows by doubling from. */
#define PKG_ENTRIES_FIRST 16

/* ------------------------------------------------------------------ */
/* Error text                                                          */
/* ------------------------------------------------------------------ */

static void pkgErr(char *err, size_t errLen, const char *fmt, ...) {
    va_list ap;
    if (err == NULL || errLen == 0) return;
    va_start(ap, fmt);
    vsnprintf(err, errLen, fmt, ap);
    va_end(ap);
    err[errLen - 1] = '\0';
}

/* ------------------------------------------------------------------ */
/* Reading a container                                                 */
/* ------------------------------------------------------------------ */

/* The bytes minizip reads a container's archive out of. One per handle,
 * never shared: it is the position as well as the buffer, so two handles
 * sharing one would read each other's archives. */
typedef struct {
    const uint8_t *data;
    size_t         size;
    size_t         pos;
} PkgReader;

struct ScnPackage {
    PkgReader  reader;
    unzFile    zip;
    char     **entryNames;
    int        entryCount;
    char     **brainNames;
    int        brainCount;
};

static voidpf ZCALLBACK pkgReadOpen(voidpf opaque, const char *filename,
                                    int mode) {
    PkgReader *mr = (PkgReader *)opaque;
    (void)filename;
    (void)mode;
    mr->pos = 0;
    return mr; /* non-NULL means the open succeeded */
}

static uLong ZCALLBACK pkgReadRead(voidpf opaque, voidpf stream, void *buf,
                                   uLong size) {
    PkgReader *mr = (PkgReader *)stream;
    size_t avail;
    size_t toRead;
    (void)opaque;
    avail = mr->size > mr->pos ? mr->size - mr->pos : 0;
    toRead = (size_t)size < avail ? (size_t)size : avail;
    if (toRead > 0) {
        memcpy(buf, mr->data + mr->pos, toRead);
        mr->pos += toRead;
    }
    return (uLong)toRead;
}

static uLong ZCALLBACK pkgReadWrite(voidpf opaque, voidpf stream,
                                    const void *buf, uLong size) {
    (void)opaque;
    (void)stream;
    (void)buf;
    (void)size;
    return 0; /* the reader never writes */
}

static long ZCALLBACK pkgReadTell(voidpf opaque, voidpf stream) {
    PkgReader *mr = (PkgReader *)stream;
    (void)opaque;
    return (long)mr->pos;
}

static long ZCALLBACK pkgReadSeek(voidpf opaque, voidpf stream, uLong offset,
                                  int origin) {
    PkgReader *mr = (PkgReader *)stream;
    size_t newPos;
    (void)opaque;
    switch (origin) {
    case ZLIB_FILEFUNC_SEEK_SET: newPos = (size_t)offset; break;
    case ZLIB_FILEFUNC_SEEK_CUR: newPos = mr->pos + (size_t)offset; break;
    case ZLIB_FILEFUNC_SEEK_END: newPos = mr->size + (size_t)offset; break;
    default: return -1;
    }
    if (newPos > mr->size) return -1;
    mr->pos = newPos;
    return 0;
}

static int ZCALLBACK pkgReadClose(voidpf opaque, voidpf stream) {
    (void)opaque;
    (void)stream;
    return 0;
}

static int ZCALLBACK pkgReadError(voidpf opaque, voidpf stream) {
    (void)opaque;
    (void)stream;
    return 0;
}

static void pkgFillReadFuncs(zlib_filefunc_def *ff, PkgReader *reader) {
    ff->zopen_file  = pkgReadOpen;
    ff->zread_file  = pkgReadRead;
    ff->zwrite_file = pkgReadWrite;
    ff->ztell_file  = pkgReadTell;
    ff->zseek_file  = pkgReadSeek;
    ff->zclose_file = pkgReadClose;
    ff->zerror_file = pkgReadError;
    ff->opaque      = reader;
}

/* True when the name stays inside the directory the archive is extracted
 * into: relative, no ".." segment, no drive letter. Both separators are
 * checked because a container can be written anywhere and read anywhere. */
static bool pkgNameIsContained(const char *name) {
    size_t i;

    if (name == NULL || name[0] == '\0') return false;
    if (name[0] == '/' || name[0] == '\\') return false;
    if (((name[0] >= 'A' && name[0] <= 'Z') ||
         (name[0] >= 'a' && name[0] <= 'z')) && name[1] == ':') {
        return false;
    }

    for (i = 0; name[i] != '\0'; i++) {
        bool atSegmentStart;
        char after;
        if (name[i] != '.' || name[i + 1] != '.') continue;
        atSegmentStart = (i == 0) || name[i - 1] == '/' || name[i - 1] == '\\';
        after = name[i + 2];
        if (atSegmentStart &&
            (after == '\0' || after == '/' || after == '\\')) {
            return false;
        }
    }
    return true;
}

/* Add one name to a growable array of owned strings. */
static bool pkgAddName(char ***names, int *count, int *cap, const char *name) {
    char *copy;
    size_t len;

    if (*count == *cap) {
        int grown = (*cap == 0) ? PKG_ENTRIES_FIRST : (*cap * 2);
        char **bigger = (char **)realloc(*names, (size_t)grown * sizeof(char *));
        if (bigger == NULL) return false;
        *names = bigger;
        *cap = grown;
    }
    len = strlen(name);
    copy = (char *)malloc(len + 1);
    if (copy == NULL) return false;
    memcpy(copy, name, len + 1);
    (*names)[*count] = copy;
    (*count)++;
    return true;
}

static void pkgFreeNames(char **names, int count) {
    int i;
    for (i = 0; i < count; i++) free(names[i]);
    free(names);
}

/* The NAME in brains/NAME/..., or 0 when the entry is not one of a brain's
 * files. Writes the name into out, which holds outLen bytes. */
static size_t pkgBrainOf(const char *entry, char *out, size_t outLen) {
    const size_t prefix = sizeof(SCN_PACKAGE_BRAIN_PREFIX) - 1;
    const char *slash;
    size_t len;

    if (strncmp(entry, SCN_PACKAGE_BRAIN_PREFIX, prefix) != 0) return 0;
    slash = strchr(entry + prefix, '/');
    if (slash == NULL) return 0; /* brains/x with no directory names no brain */
    len = (size_t)(slash - (entry + prefix));
    if (len == 0 || len + 1 > outLen) return 0;
    memcpy(out, entry + prefix, len);
    out[len] = '\0';
    return len;
}

static bool pkgHasName(char *const *names, int count, const char *name) {
    int i;
    for (i = 0; i < count; i++) {
        if (strcmp(names[i], name) == 0) return true;
    }
    return false;
}

/* Walk the central directory once, keeping every entry name in archive order
 * and the distinct brain names beside it. */
static bool pkgCollectEntries(ScnPackage *p, char *err, size_t errLen) {
    unz_global_info gi;
    int entryCap = 0;
    int brainCap = 0;
    int rc;
    uLong remaining;

    if (unzGetGlobalInfo(p->zip, &gi) != UNZ_OK) {
        pkgErr(err, errLen, "the WBSC archive has no readable directory");
        return false;
    }
    if (gi.number_entry == 0) return true;

    rc = unzGoToFirstFile(p->zip);
    if (rc != UNZ_OK) {
        pkgErr(err, errLen, "the WBSC archive directory ends early");
        return false;
    }
    for (remaining = gi.number_entry; rc == UNZ_OK && remaining > 0;
         remaining--) {
        unz_file_info info;
        char *name;
        char brain[256];

        if (unzGetCurrentFileInfo(p->zip, &info, NULL, 0, NULL, 0, NULL, 0)
            != UNZ_OK) {
            pkgErr(err, errLen, "the WBSC archive has an unreadable entry");
            return false;
        }
        name = (char *)malloc((size_t)info.size_filename + 1);
        if (name == NULL) {
            pkgErr(err, errLen, "out of memory reading the WBSC entry list");
            return false;
        }
        if (unzGetCurrentFileInfo(p->zip, &info, name,
                                  (uLong)info.size_filename + 1,
                                  NULL, 0, NULL, 0) != UNZ_OK) {
            free(name);
            pkgErr(err, errLen, "the WBSC archive has an unreadable entry");
            return false;
        }

        if (!pkgNameIsContained(name)) {
            pkgErr(err, errLen,
                   "entry \"%s\" is not a relative path inside the container",
                   name);
            free(name);
            return false;
        }
        if (!pkgAddName(&p->entryNames, &p->entryCount, &entryCap, name)) {
            free(name);
            pkgErr(err, errLen, "out of memory reading the WBSC entry list");
            return false;
        }
        if (pkgBrainOf(name, brain, sizeof(brain)) > 0 &&
            !pkgHasName(p->brainNames, p->brainCount, brain) &&
            !pkgAddName(&p->brainNames, &p->brainCount, &brainCap, brain)) {
            free(name);
            pkgErr(err, errLen, "out of memory reading the WBSC brain list");
            return false;
        }
        free(name);

        rc = unzGoToNextFile(p->zip);
        if (rc != UNZ_OK && rc != UNZ_END_OF_LIST_OF_FILE) {
            pkgErr(err, errLen, "the WBSC archive directory ends early");
            return false;
        }
    }
    return true;
}

ScnPackage *scnPackageOpen(const uint8_t *bytes, size_t len,
                           char *err, size_t errLen) {
    zlib_filefunc_def ff;
    ScnPackage *p;
    unsigned version;
    uint32_t archiveLen;

    if (err != NULL && errLen > 0) err[0] = '\0';

    if (bytes == NULL) {
        pkgErr(err, errLen, "scnPackageOpen was handed no bytes");
        return NULL;
    }
    if (len < SCN_PACKAGE_HEADER_LEN) {
        pkgErr(err, errLen,
               "the buffer is %lu bytes, shorter than the %d-byte WBSC header",
               (unsigned long)len, SCN_PACKAGE_HEADER_LEN);
        return NULL;
    }
    if (memcmp(bytes, SCN_PACKAGE_MAGIC, 4) != 0) {
        pkgErr(err, errLen, "the first four bytes are not the WBSC magic");
        return NULL;
    }
    version = (unsigned)bytes[4] | ((unsigned)bytes[5] << 8);
    if (version != SCN_PACKAGE_VERSION) {
        pkgErr(err, errLen,
               "WBSC version %u; this build reads version %d only",
               version, SCN_PACKAGE_VERSION);
        return NULL;
    }
    archiveLen = (uint32_t)bytes[6] | ((uint32_t)bytes[7] << 8) |
                 ((uint32_t)bytes[8] << 16) | ((uint32_t)bytes[9] << 24);
    if ((size_t)archiveLen > len - SCN_PACKAGE_HEADER_LEN) {
        pkgErr(err, errLen,
               "the WBSC length %lu runs past the %lu bytes after the header",
               (unsigned long)archiveLen,
               (unsigned long)(len - SCN_PACKAGE_HEADER_LEN));
        return NULL;
    }

    p = (ScnPackage *)calloc(1, sizeof(ScnPackage));
    if (p == NULL) {
        pkgErr(err, errLen, "out of memory opening the WBSC container");
        return NULL;
    }
    p->reader.data = bytes + SCN_PACKAGE_HEADER_LEN;
    p->reader.size = (size_t)archiveLen;
    p->reader.pos  = 0;

    pkgFillReadFuncs(&ff, &p->reader);
    p->zip = unzOpen2(NULL, &ff);
    if (p->zip == NULL) {
        pkgErr(err, errLen, "the WBSC payload does not open as a ZIP archive");
        scnPackageClose(p);
        return NULL;
    }

    if (!pkgCollectEntries(p, err, errLen)) {
        scnPackageClose(p);
        return NULL;
    }

    if (!scnPackageHasEntry(p, SCN_PACKAGE_MANIFEST_ENTRY)) {
        pkgErr(err, errLen, "the WBSC container has no %s",
               SCN_PACKAGE_MANIFEST_ENTRY);
        scnPackageClose(p);
        return NULL;
    }
    return p;
}

void scnPackageClose(ScnPackage *p) {
    if (p == NULL) return;
    if (p->zip != NULL) unzClose(p->zip);
    pkgFreeNames(p->entryNames, p->entryCount);
    pkgFreeNames(p->brainNames, p->brainCount);
    free(p);
}

bool scnPackageReadEntry(ScnPackage *p, const char *name,
                         uint8_t **outBytes, size_t *outLen) {
    unz_file_info info;
    uint8_t *buf;
    size_t want;
    size_t done = 0;
    bool ok = true;

    if (outBytes != NULL) *outBytes = NULL;
    if (outLen != NULL) *outLen = 0;
    if (p == NULL || name == NULL || outBytes == NULL || outLen == NULL) {
        return false;
    }

    /* Case sensitive, so a container reads the same on Windows as it does
     * everywhere else. */
    if (unzLocateFile(p->zip, name, 1) != UNZ_OK) return false;
    if (unzGetCurrentFileInfo(p->zip, &info, NULL, 0, NULL, 0, NULL, 0)
        != UNZ_OK) {
        return false;
    }
    if (unzOpenCurrentFile(p->zip) != UNZ_OK) return false;

    want = (size_t)info.uncompressed_size;
    /* One byte past the content, set to 0 below: an empty entry still gets a
     * pointer, and a caller handing a script or a manifest to something that
     * wants a C string can use the buffer as it stands. The length reported
     * back does not count it. */
    buf = (uint8_t *)malloc(want + 1);
    if (buf == NULL) {
        unzCloseCurrentFile(p->zip);
        return false;
    }

    while (done < want) {
        size_t left = want - done;
        unsigned ask = left > PKG_IO_CHUNK ? PKG_IO_CHUNK
                                              : (unsigned)left;
        int got = unzReadCurrentFile(p->zip, buf + done, ask);
        if (got <= 0) {
            ok = false;
            break;
        }
        done += (size_t)got;
    }
    /* The close is where minizip checks the CRC, so a truncated or corrupt
     * entry fails here rather than being handed back. */
    if (unzCloseCurrentFile(p->zip) != UNZ_OK) ok = false;

    if (!ok) {
        free(buf);
        return false;
    }
    buf[want] = '\0';
    *outBytes = buf;
    *outLen = want;
    return true;
}

bool scnPackageHasEntry(const ScnPackage *p, const char *name) {
    if (p == NULL || name == NULL) return false;
    return pkgHasName(p->entryNames, p->entryCount, name);
}

int scnPackageEntryCount(const ScnPackage *p) {
    return p == NULL ? 0 : p->entryCount;
}

const char *scnPackageEntryName(const ScnPackage *p, int i) {
    if (p == NULL || i < 0 || i >= p->entryCount) return NULL;
    return p->entryNames[i];
}

int scnPackageBrainCount(const ScnPackage *p) {
    return p == NULL ? 0 : p->brainCount;
}

const char *scnPackageBrainName(const ScnPackage *p, int i) {
    if (p == NULL || i < 0 || i >= p->brainCount) return NULL;
    return p->brainNames[i];
}

/* Where the map stops is the only thing that says where the chunk starts, so
 * this asks boloMapBodyLength and then looks for the magic at the byte after.
 * The container is not opened here: a caller that wants what is inside it
 * calls scnPackageOpen on what comes back. */
bool scnPackageFindInMap(const uint8_t *file, size_t len,
                         const uint8_t **outChunk, size_t *outChunkLen) {
    size_t body = 0;

    if (outChunk != NULL) *outChunk = NULL;
    if (outChunkLen != NULL) *outChunkLen = 0;
    if (file == NULL || outChunk == NULL || outChunkLen == NULL) return false;

    if (!boloMapBodyLength((const unsigned char *)file, len, &body)) {
        return false;
    }
    if (len - body < SCN_PACKAGE_HEADER_LEN) {
        return false;
    }
    if (memcmp(file + body, SCN_PACKAGE_MAGIC, 4) != 0) {
        return false;
    }
    *outChunk = file + body;
    *outChunkLen = len - body;
    return true;
}

/* ------------------------------------------------------------------ */
/* Writing a container                                                 */
/* ------------------------------------------------------------------ */

/* The buffer minizip writes an archive into. It grows on demand and takes
 * writes behind the end of what has been written: minizip seeks back to
 * patch each local header with the CRC and sizes once the entry's data has
 * gone out. */
typedef struct {
    uint8_t *data;
    size_t   size; /* bytes written, which is the archive's length */
    size_t   cap;
    size_t   pos;
    bool     oom;
} PkgWriter;

static voidpf ZCALLBACK pkgWriteOpen(voidpf opaque, const char *filename,
                                     int mode) {
    PkgWriter *mw = (PkgWriter *)opaque;
    (void)filename;
    (void)mode;
    mw->pos = 0;
    return mw;
}

static bool pkgWriterReserve(PkgWriter *mw, size_t need) {
    size_t cap;
    uint8_t *grown;

    if (need <= mw->cap) return true;
    cap = mw->cap == 0 ? 4096 : mw->cap;
    while (cap < need) {
        if (cap > (size_t)-1 / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    grown = (uint8_t *)realloc(mw->data, cap);
    if (grown == NULL) {
        mw->oom = true;
        return false;
    }
    mw->data = grown;
    mw->cap = cap;
    return true;
}

static uLong ZCALLBACK pkgWriteWrite(voidpf opaque, voidpf stream,
                                     const void *buf, uLong size) {
    PkgWriter *mw = (PkgWriter *)stream;
    size_t need;
    (void)opaque;

    if (size == 0) return 0;
    need = mw->pos + (size_t)size;
    if (!pkgWriterReserve(mw, need)) return 0;
    memcpy(mw->data + mw->pos, buf, (size_t)size);
    mw->pos = need;
    if (mw->pos > mw->size) mw->size = mw->pos;
    return size;
}

static uLong ZCALLBACK pkgWriteRead(voidpf opaque, voidpf stream, void *buf,
                                    uLong size) {
    PkgWriter *mw = (PkgWriter *)stream;
    size_t avail;
    size_t toRead;
    (void)opaque;

    avail = mw->size > mw->pos ? mw->size - mw->pos : 0;
    toRead = (size_t)size < avail ? (size_t)size : avail;
    if (toRead > 0) {
        memcpy(buf, mw->data + mw->pos, toRead);
        mw->pos += toRead;
    }
    return (uLong)toRead;
}

static long ZCALLBACK pkgWriteTell(voidpf opaque, voidpf stream) {
    PkgWriter *mw = (PkgWriter *)stream;
    (void)opaque;
    return (long)mw->pos;
}

static long ZCALLBACK pkgWriteSeek(voidpf opaque, voidpf stream, uLong offset,
                                   int origin) {
    PkgWriter *mw = (PkgWriter *)stream;
    size_t newPos;
    (void)opaque;

    switch (origin) {
    case ZLIB_FILEFUNC_SEEK_SET: newPos = (size_t)offset; break;
    case ZLIB_FILEFUNC_SEEK_CUR: newPos = mw->pos + (size_t)offset; break;
    case ZLIB_FILEFUNC_SEEK_END: newPos = mw->size + (size_t)offset; break;
    default: return -1;
    }
    if (newPos > mw->size) return -1;
    mw->pos = newPos;
    return 0;
}

static int ZCALLBACK pkgWriteClose(voidpf opaque, voidpf stream) {
    (void)opaque;
    (void)stream;
    return 0;
}

static int ZCALLBACK pkgWriteError(voidpf opaque, voidpf stream) {
    PkgWriter *mw = (PkgWriter *)stream;
    (void)opaque;
    return (mw != NULL && mw->oom) ? -1 : 0;
}

static bool pkgWriteOneEntry(zipFile zf, const ScnPackageEntry *e) {
    zip_fileinfo zi;
    size_t done = 0;

    /* Zeroed, so every container written from the same entries comes out the
     * same bytes rather than carrying the moment it was built. */
    memset(&zi, 0, sizeof(zi));

    if (zipOpenNewFileInZip(zf, e->name, &zi, NULL, 0, NULL, 0, NULL,
                            e->deflate ? Z_DEFLATED : 0,
                            e->deflate ? Z_DEFAULT_COMPRESSION : 0)
        != ZIP_OK) {
        return false;
    }
    while (done < e->len) {
        size_t left = e->len - done;
        unsigned put = left > PKG_IO_CHUNK ? PKG_IO_CHUNK
                                              : (unsigned)left;
        if (zipWriteInFileInZip(zf, e->bytes + done, put) != ZIP_OK) {
            zipCloseFileInZip(zf);
            return false;
        }
        done += put;
    }
    return zipCloseFileInZip(zf) == ZIP_OK;
}

bool scnPackageWrite(const ScnPackageEntry *entries, int count,
                     uint8_t **outBytes, size_t *outLen,
                     char *err, size_t errLen) {
    zlib_filefunc_def ff;
    PkgWriter mw;
    zipFile zf;
    uint8_t *framed;
    int i;

    if (err != NULL && errLen > 0) err[0] = '\0';
    if (outBytes != NULL) *outBytes = NULL;
    if (outLen != NULL) *outLen = 0;

    if (outBytes == NULL || outLen == NULL || count < 0 ||
        (count > 0 && entries == NULL)) {
        pkgErr(err, errLen, "scnPackageWrite was handed no entries to write");
        return false;
    }
    for (i = 0; i < count; i++) {
        if (entries[i].name == NULL || entries[i].name[0] == '\0' ||
            (entries[i].len > 0 && entries[i].bytes == NULL)) {
            pkgErr(err, errLen, "entry %d has no name or no bytes", i);
            return false;
        }
    }

    memset(&mw, 0, sizeof(mw));
    ff.zopen_file  = pkgWriteOpen;
    ff.zread_file  = pkgWriteRead;
    ff.zwrite_file = pkgWriteWrite;
    ff.ztell_file  = pkgWriteTell;
    ff.zseek_file  = pkgWriteSeek;
    ff.zclose_file = pkgWriteClose;
    ff.zerror_file = pkgWriteError;
    ff.opaque      = &mw;

    zf = zipOpen2(NULL, APPEND_STATUS_CREATE, NULL, &ff);
    if (zf == NULL) {
        free(mw.data);
        pkgErr(err, errLen, "the WBSC archive could not be started");
        return false;
    }
    for (i = 0; i < count; i++) {
        if (!pkgWriteOneEntry(zf, &entries[i])) {
            zipClose(zf, NULL);
            pkgErr(err, errLen, "entry \"%s\" could not be written",
                   entries[i].name);
            free(mw.data);
            return false;
        }
    }
    if (zipClose(zf, NULL) != ZIP_OK || mw.oom) {
        pkgErr(err, errLen, "the WBSC archive could not be finished");
        free(mw.data);
        return false;
    }
    if (mw.size > 0xFFFFFFFFu) {
        pkgErr(err, errLen,
               "the archive is %lu bytes, past what the WBSC length holds",
               (unsigned long)mw.size);
        free(mw.data);
        return false;
    }

    framed = (uint8_t *)malloc(SCN_PACKAGE_HEADER_LEN + mw.size);
    if (framed == NULL) {
        pkgErr(err, errLen, "out of memory framing the WBSC container");
        free(mw.data);
        return false;
    }
    memcpy(framed, SCN_PACKAGE_MAGIC, 4);
    framed[4] = (uint8_t)(SCN_PACKAGE_VERSION & 0xFF);
    framed[5] = (uint8_t)((SCN_PACKAGE_VERSION >> 8) & 0xFF);
    framed[6] = (uint8_t)(mw.size & 0xFF);
    framed[7] = (uint8_t)((mw.size >> 8) & 0xFF);
    framed[8] = (uint8_t)((mw.size >> 16) & 0xFF);
    framed[9] = (uint8_t)((mw.size >> 24) & 0xFF);
    if (mw.size > 0) memcpy(framed + SCN_PACKAGE_HEADER_LEN, mw.data, mw.size);
    free(mw.data);

    *outBytes = framed;
    *outLen = SCN_PACKAGE_HEADER_LEN + mw.size;
    return true;
}
