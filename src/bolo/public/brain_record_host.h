/* SPDX-License-Identifier: GPL-3.0-or-later */
/*********************************************************
 *NAME:          brain_record_host.h
 *PURPOSE:       Bot Host brain recordings and their merge into the
 *               server's brainrec.btr.
 *
 *  A bot whose brain runs on a Bot Host has no brain on the server, so the
 *  server's brainrec.btr can hold no goal info, overlays or pool JSON for
 *  it. The server writes a HOSTED MARKER in that bot's per-bot entry
 *  instead: the bot's place on the host (placeGen, the gen of the assign
 *  that carried the token), the host's name and the last input the server
 *  applied from it (lastInput). The host records the same per-bot entry
 *  body for each think, stamped with the input sequence the think's first
 *  input packet carries, into one .hbr file per (seat, placeGen). After
 *  the game the host uploads its files and the server merges them: each
 *  marker is replaced by the host record whose inputs the server was about
 *  to apply at that frame. The result is a normal brainrec.btr that
 *  BrainTest loads as if every bot ran in-process.
 *
 *  Per-bot entry body (everything after the u8 slot in a frame):
 *    f32 thinkMs
 *    char kind[32], u32 mx, u32 my, u32 target_id, char substate[32]
 *    u16 nc, nc x BrainRecCandidate
 *    u32 overlayCount, packed overlays (28 bytes + text each)
 *    u32 poolLen, pool JSON
 *
 *  .hbr file (in the brainrec container, brain_record_io.c):
 *    HostRecHeader
 *    u32 legendLen, legend JSON (the seat's brain.viz_legend_json())
 *    records: u32 HOSTREC_RECORD_MAGIC, u32 seq, u32 estTick,
 *             u32 bodyLen, body
 *  A file cut short (the host was killed) reads up to its last whole
 *  record.
 *********************************************************/

#ifndef BRAIN_RECORD_HOST_H
#define BRAIN_RECORD_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "server_sim.h"    /* BrainGoalInfo */
#include "brain_overlay.h" /* OverlayCmdBuffer */
#include "brain_record.h"
#include "brain_record_io.h"  /* BrainRecOut / BrainRecIn: the .btr and .hbr container */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Growable byte buffer --------------------------------------------- */

typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
    bool     failed;   /* an allocation failed; the contents are short */
} BrainRecBuf;

void brainRecBufInit(BrainRecBuf *b);
void brainRecBufFree(BrainRecBuf *b);
void brainRecBufReset(BrainRecBuf *b);
bool brainRecBufPut(BrainRecBuf *b, const void *p, size_t n);

/* ---- Per-bot entry body ----------------------------------------------- */

/* Encode one bot's body. gi NULL writes an empty goal block, ovl NULL no
 * overlays, poolJson NULL or "" no pool. skip (256 flags, may be NULL)
 * drops overlay commands by viz_idx. Returns the overlay commands kept. */
uint32_t brainRecBodyEncode(BrainRecBuf *b, float thinkMs,
                            const BrainGoalInfo *gi,
                            const OverlayCmdBuffer *ovl, const bool *skip,
                            const char *poolJson);

/* The hosted marker. */
#define BRAINREC_HOSTED_KIND   "(on Bot Host)"
#define BRAINREC_HOSTED_TAG    0x54534F48u   /* "HOST", in the my field */
void brainRecBodyEncodeHosted(BrainRecBuf *b, uint16_t placeGen,
                              const char *hostName, uint32_t lastInput);

/* True when body is a hosted marker; fills the out fields (any may be
 * NULL). hostOut takes 32 bytes. */
bool brainRecBodyIsHosted(const uint8_t *body, size_t len,
                          uint16_t *placeGen, uint32_t *lastInput,
                          char *hostOut);

/* The byte length of the body that starts at p, or 0 when avail bytes do
 * not hold a whole body. */
size_t brainRecBodyMeasure(const uint8_t *p, size_t avail);

/* The length of the body at p (len bytes, a whole body) once its pool JSON
 * is dropped: the pool length field is set to 0 in place. */
size_t brainRecBodyDropPool(uint8_t *p, size_t len);

/* ---- .hbr files --------------------------------------------------------- */

#define HOSTREC_MAGIC          "WBNHREC"
#define HOSTREC_MAGIC_LEN      8
#define HOSTREC_VERSION        1u
#define HOSTREC_RECORD_MAGIC   0xB07EC0DFu
#define HOSTREC_EXT            ".hbr"
#define HOSTREC_HOST_LEN       32
#define HOSTREC_BLOCK_LEN      64

typedef struct {
    char     magic[HOSTREC_MAGIC_LEN];
    uint32_t version;          /* HOSTREC_VERSION */
    uint32_t brainrecVersion;  /* BRAINREC_VERSION the bodies follow */
    char     hostName[HOSTREC_HOST_LEN];
    char     block[HOSTREC_BLOCK_LEN];  /* the server's session folder name */
    uint8_t  slot;
    uint8_t  pad;
    uint16_t placeGen;
    uint32_t reserved[4];
} HostRecHeader;

/* The file name of a seat's recording: "bot<slot>_g<placeGen>.hbr". */
void hostRecFileName(char *out, size_t sz, uint8_t slot, uint16_t placeGen);
/* Part 0 is the name above. A seat whose file is opened again in the same
 * block (its brain was built again) writes "bot<slot>_g<placeGen>_p<part>.hbr"
 * with part 1, 2, ...; the merge reads every part of a (slot, placeGen). */
void hostRecPartFileName(char *out, size_t sz, uint8_t slot, uint16_t placeGen,
                         unsigned part);

typedef struct {
    BrainRecOut *file;
    uint32_t sinceFlush;
    uint32_t records;
    uint64_t rawBytes;
} HostRecWriter;

/* True when exactly n bytes were read from the container. */
bool hostRecReadExact(BrainRecIn *in, void *p, size_t n);

bool hostRecWriterOpen(HostRecWriter *w, const char *path,
                       const HostRecHeader *hdr, const char *legend);
bool hostRecWriterAppend(HostRecWriter *w, uint32_t seq, uint32_t estTick,
                         const uint8_t *body, size_t len);
void hostRecWriterClose(HostRecWriter *w);

typedef struct {
    BrainRecIn   *file;
    HostRecHeader hdr;
    char         *legend;      /* malloc'd, NUL-terminated, may be "" */
} HostRecReader;

bool hostRecReaderOpen(HostRecReader *r, const char *path);
/* 1: a record (body in *body, replaced), 0: end of file (or a cut-short
 * tail), -1: a damaged record. */
int  hostRecReaderNext(HostRecReader *r, uint32_t *seq, uint32_t *estTick,
                       BrainRecBuf *body);
void hostRecReaderClose(HostRecReader *r);

/* ---- Merge -------------------------------------------------------------- */

typedef struct {
    uint32_t frames;       /* frames in the .btr */
    uint32_t markers;      /* hosted markers seen */
    uint32_t filled;       /* markers given the matching host record */
    uint32_t stale;        /* given a host record a few thinks old */
    uint32_t missing;      /* left as markers: no host record */
    uint32_t hostRecords;  /* host records read */
    uint32_t hostFiles;    /* host files used */
    uint32_t hostFilesSkipped; /* host files that could not be read */
    uint32_t hostFilesUnused;  /* host files read that filled no entry */
    uint32_t btrMissing;   /* 1: host files came, the session has no .btr */
} BrainRecMergeStats;

/* The ACK a merge earns: BOT_HOST_REC_FAIL_NONE (merged) only when the
 * merge worked, every host file was read and each one filled entries;
 * else the BOT_HOST_REC_FAIL_* reason. ok: what brainRecMergeDir gave. */
uint8_t brainRecMergeVerdict(bool ok, const BrainRecMergeStats *st);
/* A short English text for a BOT_HOST_REC_FAIL_* reason. */
const char *brainRecMergeFailText(uint8_t reason);

/* The session subfolder that keeps every file a host uploaded, as it came:
 * <session>/bothost_raw/<host>_<name>. */
#define BRAINREC_HOST_RAW_DIR "bothost_raw"


/* The input sequence distance a host record may lag the marker's last
 * input and still fill it (8 thinks). */
#define BRAINREC_MERGE_STALE_MAX 16u

/* Merge the host files into the .btr at btrPath, writing outPath. The
 * output is checked with the frame walker; false (and err) on any
 * failure, with outPath removed. A host file that cannot be read is
 * skipped and its markers stay. */
bool brainRecMergeFiles(const char *btrPath, const char *const *hostFiles,
                        int hostCount, const char *outPath,
                        BrainRecMergeStats *st, char *err, size_t errSz);

/* Add a slot's legend to the .btr legend blob ({..., "slots":{...}}),
 * unless the blob has that slot already. A blob with an empty global
 * legend takes the slot's legend as its global one too. Returns a malloc'd
 * string. */
char *brainRecLegendAddSlot(const char *legend, int slot,
                            const char *slotLegend);

/* Merge print2 logs: both files are runs of "===TICK n===" blocks (any
 * lines before the first block are kept at the top). The blocks of both
 * are written to outPath in tick order, a's first on equal ticks. Each
 * input may be plain, gzip or zstd (brainRecInOpen detects it); outFmt
 * is the form of outPath. */
bool brainRecMergePrint2(const char *a, const char *b, const char *outPath,
                         BrainLogFormat outFmt);

/* Merge one host's uploaded folder into a session folder:
 *   - its .hbr files into the session's brainrec.btr (checked, then
 *     replaced);
 *   - print2_bot*.log present on both sides by ticks;
 *   - any other print2_bot*.log or print2_bot*.log.gz moved in, renamed
 *     "bothost_<host>_<name>" when the session has a file of that name;
 *   - every other file kept only as a raw copy in
 *     <session>/BRAINREC_HOST_RAW_DIR/<host>_<name>, never in the session
 *     folder itself; deleted, with a log line, when it cannot be kept.
 * The .hbr files and the host's copies of the print2 logs it merged are
 * kept as they came in <session>/BRAINREC_HOST_RAW_DIR/, merged or not.
 * hostDir is removed when every step worked. log (may be NULL) gets one
 * line per notable step. */
typedef void (*BrainRecMergeLogFn)(void *ctx, const char *line);
/* Make path a full path: a relative one is joined to the current folder,
 * '\\' becomes '/', and "." and ".." parts are folded. False when it
 * does not fit. */
bool brainRecMakeAbsolute(char *path, size_t sz);

bool brainRecMergeDir(const char *sessionDir, const char *hostDir,
                      const char *hostName, BrainRecMergeStats *st,
                      BrainRecMergeLogFn log, void *logCtx);

/* ---- Folder helpers ------------------------------------------------------ */

/* Copy in to out with every character outside [A-Za-z0-9_-] (and '.' when
 * allowDot) made '_'. Never empty, never only dots. */
void brainRecSafeName(const char *in, char *out, size_t outSz, bool allowDot);

/* True when s is a name a peer may use as one path part: not empty, no
 * leading '.', no "..", only [A-Za-z0-9_-] (and '.' when allowDot). */
bool brainRecNameOk(const char *s, bool allowDot);

/* True when path is below root (not root itself): it starts with root and a
 * separator, and no part after root is empty, "." or "..". The check is on
 * the text only. */
bool brainRecPathUnder(const char *root, const char *path);

/* Delete the file or folder path and everything below it, but only when
 * brainRecPathUnder(root, path). bytes and files (may be NULL) get what was
 * deleted. False when the path check failed or something was not deleted;
 * a path that does not exist is true with 0 deleted. */
bool brainRecRemoveTree(const char *root, const char *path, uint64_t *bytes,
                        uint32_t *files);

#ifdef __cplusplus
}
#endif

#endif /* BRAIN_RECORD_HOST_H */
