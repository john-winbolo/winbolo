/*********************************************************
 *NAME:          brain_record_walk.h
 *PURPOSE:
 *  The one frame walker for the brainrec.btr format, shared by every
 *  reader. BrainTest's session browser walks frames to learn the bot
 *  roster; brainrec_split walks them to copy whole frames into smaller
 *  session dirs. A second hand-written parser would drift the moment
 *  the frame layout (and BRAINREC_VERSION) moves, so the walk lives
 *  here once and both callers go through it.
 *
 *  The walker never materialises a frame. It pulls only the length
 *  fields it must understand and hands every other byte back to a
 *  caller-supplied BrainRecReader to dispose of: gzseek past them
 *  (BrainTest's peek) or copy them into another gzFile (the splitter).
 *  Resident memory is therefore whatever the reader's own buffer is,
 *  not the size of a frame — which matters, because a full-pool
 *  16-bot recording is gigabytes.
 *
 *  See brain_record.{c,h} for the on-disk layout this mirrors.
 *********************************************************/
#ifndef BRAIN_RECORD_WALK_H
#define BRAIN_RECORD_WALK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Byte source the walker pulls through. read() returns the number of
 * bytes actually delivered (0 = clean end of stream, short = truncated
 * file); skip() consumes len bytes and returns false if it can't. A
 * copying reader implements both as "forward these bytes to my output". */
typedef struct {
    void  *ctx;
    size_t (*read)(void *ctx, void *dst, size_t len);
    bool   (*skip)(void *ctx, size_t len);
    /* The file's format version. brainRecWalkPreamble sets it from the
     * header; a caller that walks frames without the preamble (it copied the
     * preamble some other way) sets it itself. 0 counts as the version this
     * build writes. */
    uint32_t version;
} BrainRecReader;

/* What a walked frame turned out to be. mapKeyframe is the split-safe
 * flag: the map track is delta-encoded, so a run of frames only replays
 * the right terrain if it starts on a frame that carries the full map. */
typedef struct {
    uint32_t tick;         /* engine tick this frame was recorded at */
    bool     mapKeyframe;  /* true = full map, false = changed tiles only */
    int      botCount;     /* per-bot blocks in this frame */
    int      maxBotSlot;   /* highest bot slot seen, or -1 */
    uint32_t pillsOnMap;   /* bit n: pill n is on the map (all set before v7) */
    uint32_t basesOnMap;   /* bit n: base n is on the map (all set before v7) */
} BrainRecFrameInfo;

typedef enum {
    BRAINREC_WALK_OK = 0,   /* one frame (or the preamble) consumed */
    BRAINREC_WALK_EOF,      /* clean end of stream on a frame boundary */
    BRAINREC_WALK_BAD       /* wrong magic/version, or truncated mid-frame */
} BrainRecWalkStatus;

/* Size of the on-disk BrainRecHeader, so a caller can buffer the
 * preamble without including brain_record.h (which drags in the whole
 * server_sim.h writer-side chain). */
size_t brainRecWalkHeaderSize(void);

/* The BRAINREC_VERSION this translation unit was built against. A tool
 * refuses to touch a file that doesn't match. */
uint32_t brainRecWalkFormatVersion(void);

/* The format version stored in a header already read into memory (at least
 * brainRecWalkHeaderSize() bytes). For a caller that copies the preamble as
 * bytes and then walks frames: it sets BrainRecReader.version from this. */
uint32_t brainRecWalkHeaderVersion(const void *header);

/* Consume the file header plus the one-time legend block, i.e. everything
 * before the first frame. Fails on a bad magic or a version mismatch —
 * the format is platform-native binary, so a mismatch is unreadable, not
 * merely inconvenient. mapNameOut/legendLenOut may be NULL. */
BrainRecWalkStatus brainRecWalkPreamble(BrainRecReader *r,
                                        char mapNameOut[64],
                                        uint32_t *legendLenOut);

/* Consume exactly one frame, starting at its BRAINREC_FRAME_MAGIC. Call
 * repeatedly after brainRecWalkPreamble until it returns EOF. */
BrainRecWalkStatus brainRecWalkFrame(BrainRecReader *r, BrainRecFrameInfo *out);

#ifdef __cplusplus
}
#endif

#endif /* BRAIN_RECORD_WALK_H */
