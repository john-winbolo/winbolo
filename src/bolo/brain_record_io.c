/* SPDX-License-Identifier: GPL-3.0-or-later */
/*********************************************************
 *NAME:          brain_record_io.c
 *PURPOSE:       The container the brain recordings are stored in: the
 *               server's brainrec.btr and the Bot Host's .hbr files. Every
 *               reader and writer of those files goes through these calls,
 *               so a change of container (compression) is made here only.
 *               Today the container is gzip.
 *********************************************************/

#include <stdlib.h>
#include <zlib.h>

#include "brain_record.h"

struct BrainRecFile {
    gzFile gz;
    bool   writing;
};

static BrainRecFile *recFileOpen(const char *path, const char *mode,
                                 bool writing) {
    BrainRecFile *f;
    gzFile gz;
    if (path == NULL) return NULL;
    gz = gzopen(path, mode);
    if (gz == NULL) return NULL;
    f = (BrainRecFile *)malloc(sizeof(*f));
    if (f == NULL) {
        gzclose(gz);
        return NULL;
    }
    f->gz = gz;
    f->writing = writing;
    return f;
}

BrainRecFile *brainRecFileOpenWrite(const char *path) {
    return recFileOpen(path, "wb1", true);
}

BrainRecFile *brainRecFileOpenRead(const char *path) {
    return recFileOpen(path, "rb", false);
}

bool brainRecFileWrite(BrainRecFile *f, const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    if (f == NULL || !f->writing) return false;
    while (n > 0) {
        unsigned part = n > (1u << 30) ? (1u << 30) : (unsigned)n;
        if (gzwrite(f->gz, b, part) != (int)part) return false;
        b += part;
        n -= part;
    }
    return true;
}

long brainRecFileRead(BrainRecFile *f, void *p, size_t n) {
    int got;
    if (f == NULL || f->writing) return -1;
    if (n == 0) return 0;
    if (n > (1u << 30)) n = (1u << 30);
    got = gzread(f->gz, p, (unsigned)n);
    return got < 0 ? -1 : (long)got;
}

bool brainRecFileReadExact(BrainRecFile *f, void *p, size_t n) {
    unsigned char *b = (unsigned char *)p;
    while (n > 0) {
        long got = brainRecFileRead(f, b, n);
        if (got <= 0) return false;
        b += got;
        n -= (size_t)got;
    }
    return true;
}

bool brainRecFileFlush(BrainRecFile *f) {
    if (f == NULL || !f->writing) return false;
    return gzflush(f->gz, Z_SYNC_FLUSH) == Z_OK;
}

bool brainRecFileClose(BrainRecFile *f) {
    int rc;
    if (f == NULL) return true;
    rc = gzclose(f->gz);
    free(f);
    return rc == Z_OK;
}
