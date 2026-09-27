/* Fuzz target — the log viewer loading and playing a .wbv recording.
 *
 * A .wbv is a zip anyone can hand the viewer: a file the user opened, a round
 * a game server sent for the post-game reel, or one from WinBolo.net. Every
 * byte of it is the sender's. lv_screenLoadMapFromMemory reads the zip's
 * log.dat and scripts.json members, decodes the header and the opening
 * snapshot, then walks the whole log for the total time, the game start, the
 * slot names, the rule changes, the presentation index and the server ticks.
 * Playback then applies the records one tick at a time, and a seek backwards
 * restores a snapshot and rebuilds the rules and the presentation stores.
 *
 * Input format: [u16 big-endian scriptsLen][scripts.json bytes][log.dat bytes].
 * scriptsLen is clamped to what follows it, and 0 leaves the member out, as a
 * plain round's recording does. The target stores the two members in a zip it
 * builds in memory and hands that to lv_screenLoadMapFromMemory, so the
 * fuzzer mutates the plaintext log rather than a deflate stream it could
 * rarely keep valid, and the viewer's own zip reading still runs on every
 * input. The members are stored rather than deflated: the inflate is zlib's,
 * and compressing each input would cost more than decoding it.
 *
 * Each input is then played for at most FUZZ_REPLAY_STEP_CAP ticks, seeked
 * back to halfway through what was played and then to the start, and the
 * server-tick lookup, the scripts holder, the rule changes and the slot names
 * are read into a volatile sink. Nothing is written to disk.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lv_global.h"
#include "backend.h"
#include "logviewer.h"
#include "lv_presentation.h"
#include "zip.h"

/* Playback ticks per input. The committed recordings end within 1505; the
 * hand-built viewer tests use the same 4096. */
#define FUZZ_REPLAY_STEP_CAP 4096

/* Largest input taken. The committed recordings' log.dat members are far
 * smaller; this only keeps one input's zip and walks bounded. */
#define FUZZ_REPLAY_MAX_INPUT (1u << 20)

static volatile uint32_t g_sink;

/* The buffer minizip writes the zip into. It grows on demand and takes writes
 * behind the end of what has been written: minizip seeks back to patch each
 * local header with the CRC and sizes once the member's data has gone out. */
typedef struct {
    uint8_t *data;
    size_t   size;
    size_t   cap;
    size_t   pos;
    int      oom;
} FuzzZipWriter;

static voidpf ZCALLBACK fzOpen(voidpf opaque, const char *filename, int mode) {
    FuzzZipWriter *w = (FuzzZipWriter *)opaque;
    (void)filename;
    (void)mode;
    w->pos = 0;
    return w;
}

static uLong ZCALLBACK fzWrite(voidpf opaque, voidpf stream, const void *buf,
                               uLong size) {
    FuzzZipWriter *w = (FuzzZipWriter *)stream;
    size_t need;
    (void)opaque;

    if (size == 0) return 0;
    need = w->pos + (size_t)size;
    if (need > w->cap) {
        size_t   cap = w->cap == 0 ? 4096 : w->cap;
        uint8_t *grown;
        while (cap < need) cap *= 2;
        grown = (uint8_t *)realloc(w->data, cap);
        if (grown == NULL) {
            w->oom = 1;
            return 0;
        }
        w->data = grown;
        w->cap  = cap;
    }
    memcpy(w->data + w->pos, buf, (size_t)size);
    w->pos = need;
    if (w->pos > w->size) w->size = w->pos;
    return size;
}

static uLong ZCALLBACK fzRead(voidpf opaque, voidpf stream, void *buf,
                              uLong size) {
    FuzzZipWriter *w = (FuzzZipWriter *)stream;
    size_t avail;
    size_t n;
    (void)opaque;

    avail = w->size > w->pos ? w->size - w->pos : 0;
    n = (size_t)size < avail ? (size_t)size : avail;
    if (n > 0) {
        memcpy(buf, w->data + w->pos, n);
        w->pos += n;
    }
    return (uLong)n;
}

static long ZCALLBACK fzTell(voidpf opaque, voidpf stream) {
    (void)opaque;
    return (long)((FuzzZipWriter *)stream)->pos;
}

static long ZCALLBACK fzSeek(voidpf opaque, voidpf stream, uLong offset,
                             int origin) {
    FuzzZipWriter *w = (FuzzZipWriter *)stream;
    size_t pos;
    (void)opaque;

    switch (origin) {
    case ZLIB_FILEFUNC_SEEK_SET: pos = (size_t)offset; break;
    case ZLIB_FILEFUNC_SEEK_CUR: pos = w->pos + (size_t)offset; break;
    case ZLIB_FILEFUNC_SEEK_END: pos = w->size + (size_t)offset; break;
    default: return -1;
    }
    if (pos > w->size) return -1;
    w->pos = pos;
    return 0;
}

static int ZCALLBACK fzClose(voidpf opaque, voidpf stream) {
    (void)opaque;
    (void)stream;
    return 0;
}

static int ZCALLBACK fzError(voidpf opaque, voidpf stream) {
    (void)opaque;
    return ((FuzzZipWriter *)stream)->oom ? -1 : 0;
}

/* One stored member. */
static int fzAddMember(zipFile zf, const char *name, const uint8_t *bytes,
                       size_t len) {
    zip_fileinfo zi;
    int ok;

    memset(&zi, 0, sizeof(zi));
    if (zipOpenNewFileInZip(zf, name, &zi, NULL, 0, NULL, 0, NULL, 0, 0)
        != ZIP_OK) {
        return 0;
    }
    ok = len == 0 || zipWriteInFileInZip(zf, bytes, (unsigned)len) == ZIP_OK;
    if (zipCloseFileInZip(zf) != ZIP_OK) ok = 0;
    return ok;
}

/* The zip holding log.dat and, when scriptsLen is not 0, scripts.json. The
 * caller owns *out on success. */
static int fzBuildZip(const uint8_t *log, size_t logLen,
                      const uint8_t *scripts, size_t scriptsLen,
                      uint8_t **out, size_t *outLen) {
    zlib_filefunc_def ff;
    FuzzZipWriter w;
    zipFile zf;
    int ok;

    memset(&w, 0, sizeof(w));
    ff.zopen_file  = fzOpen;
    ff.zread_file  = fzRead;
    ff.zwrite_file = fzWrite;
    ff.ztell_file  = fzTell;
    ff.zseek_file  = fzSeek;
    ff.zclose_file = fzClose;
    ff.zerror_file = fzError;
    ff.opaque      = &w;

    zf = zipOpen2(NULL, APPEND_STATUS_CREATE, NULL, &ff);
    if (zf == NULL) {
        free(w.data);
        return 0;
    }
    ok = fzAddMember(zf, "log.dat", log, logLen);
    if (ok && scriptsLen > 0) {
        ok = fzAddMember(zf, "scripts.json", scripts, scriptsLen);
    }
    if (zipClose(zf, NULL) != ZIP_OK || w.oom) ok = 0;
    if (!ok) {
        free(w.data);
        return 0;
    }
    *out    = w.data;
    *outLen = w.size;
    return 1;
}

/* Is the buffer terminated within its length? */
static int fzTerminated(const char *s, size_t cap) {
    return memchr(s, '\0', cap) != NULL;
}

/* The scripts holder, read field by field. lv_scripts.h says every count is
 * clamped to its array and every string terminated in its buffer; either one
 * broken aborts, which libFuzzer reports as a crash with the input. */
static void fzReadScripts(void) {
    const LvScripts *s = lv_screenGetScripts();
    int i;

    if (s == NULL) return;
    if (s->count < 0 || s->count > LV_SCRIPTS_MAX ||
        s->ruleCount < 0 || s->ruleCount > SIM_RULE_COUNT ||
        s->regionCount < 0 || s->regionCount > LV_SCRIPTS_REGIONS_MAX ||
        !fzTerminated(s->map, sizeof(s->map))) {
        abort();
    }
    g_sink += (uint32_t)s->present + (uint32_t)s->modsEnabled;
    for (i = 0; i < s->count; i++) {
        const LvScriptRow *r = &s->scripts[i];
        if (!fzTerminated(r->file, sizeof(r->file)) ||
            !fzTerminated(r->source, sizeof(r->source)) ||
            !fzTerminated(r->kind, sizeof(r->kind)) ||
            !fzTerminated(r->name, sizeof(r->name)) ||
            !fzTerminated(r->description, sizeof(r->description))) {
            abort();
        }
        g_sink += (uint32_t)strlen(r->name) + (uint32_t)strlen(r->description);
    }
    for (i = 0; i < s->ruleCount; i++) {
        g_sink += (uint32_t)s->rules[i].index;
    }
    for (i = 0; i < s->regionCount; i++) {
        const LvScriptRegion *g = &s->regions[i];
        if (!fzTerminated(g->name, sizeof(g->name)) ||
            !fzTerminated(g->file, sizeof(g->file))) {
            abort();
        }
        g_sink += (uint32_t)g->x + g->y + g->w + g->h;
    }
}

/* Everything past the load: playback, the two seeks and the lookups. */
static void fzPlay(void) {
    const LvRuleChange *changes = NULL;
    char     name[64];
    uint32_t total = lv_screenGetState()->totalTimeMs;
    uint32_t start = lv_screenWindowStartMs();
    uint32_t now;
    uint32_t mid;
    int      steps = 0;
    int      n;
    int      i;

    fzReadScripts();

    while (lv_screenIsPlaying() == TRUE && steps < FUZZ_REPLAY_STEP_CAP) {
        lv_screenLogTick();
        steps++;
    }

    /* Halfway through what was played, then the start: both are at or behind
     * the playhead, so each restores a snapshot and rebuilds, and neither can
     * play further forward than playback already went. */
    now = lv_screenGetTimeRunning();
    mid = start + (now > start ? (now - start) / 2u : 0u);
    lv_screenSeekToTimeMs(mid);
    lv_screenSeekToTimeMs(0);

    g_sink += lv_screenServerTickAt(0) + lv_screenServerTickAt(start) +
              lv_screenServerTickAt(mid) + lv_screenServerTickAt(now) +
              lv_screenServerTickAt(total) +
              lv_screenServerTickAt(UINT32_MAX);
    g_sink += lv_screenGameStartMs();

    n = lv_screenGetRuleChanges(&changes);
    if (n < 0 || n > LV_RULE_CHANGES_MAX) abort();
    for (i = 0; i < n; i++) {
        g_sink += changes[i].ms + (uint32_t)changes[i].index;
    }

    for (i = 0; i < MAX_TANKS; i++) {
        if (lv_screenGetLoggedPlayerName((BYTE)i, name, sizeof(name)) == TRUE) {
            g_sink += (uint32_t)strlen(name);
        }
    }

    fzReadScripts();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    const uint8_t  *scripts;
    const uint8_t  *log;
    size_t          scriptsLen;
    size_t          logLen;
    uint8_t        *zipData = NULL;
    size_t          zipLen = 0;
    LogViewerState *lv;

    if (size < 2 || size > FUZZ_REPLAY_MAX_INPUT) {
        return 0;
    }
    scriptsLen = ((size_t)data[0] << 8) | (size_t)data[1];
    if (scriptsLen > size - 2) {
        scriptsLen = size - 2;
    }
    scripts = data + 2;
    log     = scripts + scriptsLen;
    logLen  = size - 2 - scriptsLen;

    if (!fzBuildZip(log, logLen, scripts, scriptsLen, &zipData, &zipLen)) {
        return 0;
    }

    lv = lv_decoderCreate(false);
    if (lv == NULL) {
        free(zipData);
        return 0;
    }
    lv_screenSetSizeX(30);
    lv_screenSetSizeY(30);

    /* Takes the buffer whether or not the load works; the destroy frees it. */
    if (lv_screenLoadMapFromMemory(zipData, zipLen) == TRUE) {
        fzPlay();
    }
    lv_decoderDestroy(lv);
    return 0;
}
