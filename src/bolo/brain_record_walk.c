/*********************************************************
 *NAME:          brain_record_walk.c
 *PURPOSE:
 *  Frame walker for brainrec.btr. Mirrors the field order
 *  brain_record.c writes, one field at a time, and every size it needs
 *  comes from the same headers the writer uses — so the walk can't
 *  drift from the writer without a compile error.
 *
 *  Nothing here allocates. Every variable-length block is handed to the
 *  reader's skip() instead of being read into memory, which is what
 *  lets a 3 GB recording be walked in a few hundred KB of RAM.
 *********************************************************/
#include "brain_record_walk.h"

#include <string.h>

#include "brain_record.h"  /* BrainRecHeader / BrainRecCandidate / magics */
#include "input_packet.h"  /* TankSnapshot / ShellSnapshot / BaseSnapshot / PillSnapshot */

/* Read a fixed-size field. Anything short of the full length is a
 * truncated file, not an end of stream — the caller only treats a
 * zero-byte read on the frame magic as a clean EOF. */
static bool rdExact(BrainRecReader *r, void *dst, size_t len) {
    return r->read(r->ctx, dst, len) == len;
}

static bool rdU8(BrainRecReader *r, uint8_t *v) { return rdExact(r, v, 1); }
static bool rdU16(BrainRecReader *r, uint16_t *v) { return rdExact(r, v, 2); }
static bool rdU32(BrainRecReader *r, uint32_t *v) { return rdExact(r, v, 4); }

size_t brainRecWalkHeaderSize(void) {
    return sizeof(BrainRecHeader);
}

uint32_t brainRecWalkFormatVersion(void) {
    return BRAINREC_VERSION;
}

BrainRecWalkStatus brainRecWalkPreamble(BrainRecReader *r, char mapNameOut[64],
                                        uint32_t *legendLenOut) {
    BrainRecHeader hdr;
    if (!rdExact(r, &hdr, sizeof hdr)) return BRAINREC_WALK_BAD;
    if (memcmp(hdr.magic, BRAINREC_MAGIC, BRAINREC_MAGIC_LEN) != 0) return BRAINREC_WALK_BAD;
    if (hdr.version != BRAINREC_VERSION) return BRAINREC_WALK_BAD;
    if (mapNameOut) {
        memcpy(mapNameOut, hdr.mapName, 64);
        mapNameOut[63] = '\0';
    }

    uint32_t llen = 0;
    if (!rdU32(r, &llen)) return BRAINREC_WALK_BAD;
    if (llen && !r->skip(r->ctx, llen)) return BRAINREC_WALK_BAD;
    if (legendLenOut) *legendLenOut = llen;
    return BRAINREC_WALK_OK;
}

BrainRecWalkStatus brainRecWalkFrame(BrainRecReader *r, BrainRecFrameInfo *out) {
    BrainRecFrameInfo info;
    memset(&info, 0, sizeof info);
    info.maxBotSlot = -1;

    /* A zero-byte read here is the normal way a recording ends; anything
     * else short means the writer was killed mid-frame. */
    uint32_t fm = 0;
    size_t got = r->read(r->ctx, &fm, sizeof fm);
    if (got == 0) return BRAINREC_WALK_EOF;
    if (got != sizeof fm || fm != BRAINREC_FRAME_MAGIC) return BRAINREC_WALK_BAD;

    if (!rdU32(r, &info.tick)) return BRAINREC_WALK_BAD;

    /* God-view snapshot. Tanks and shells are written in full every
     * frame; so are bases and pills (the writer bypasses the snapshot's
     * delta encoding for those), so none of these carry a cross-frame
     * dependency and a part may start at any of them. */
    uint8_t tc = 0, sc = 0, bc = 0, pc = 0;
    if (!rdU8(r, &tc) || !r->skip(r->ctx, (size_t)tc * sizeof(TankSnapshot)))  return BRAINREC_WALK_BAD;
    if (!rdU8(r, &sc) || !r->skip(r->ctx, (size_t)sc * sizeof(ShellSnapshot))) return BRAINREC_WALK_BAD;
    if (!rdU8(r, &bc) || !r->skip(r->ctx, (size_t)bc * sizeof(BaseSnapshot)))  return BRAINREC_WALK_BAD;
    if (!rdU8(r, &pc) || !r->skip(r->ctx, (size_t)pc * sizeof(PillSnapshot)))  return BRAINREC_WALK_BAD;

    /* Map track: the one delta-encoded stream in the format. A keyframe
     * carries the whole map; every other frame only lists changed tiles
     * as (uint32 offset, uint8 value) pairs. */
    uint8_t kf = 0;
    if (!rdU8(r, &kf)) return BRAINREC_WALK_BAD;
    if (kf) {
        if (!r->skip(r->ctx, (size_t)MAP_ARRAY_SIZE * MAP_ARRAY_SIZE)) return BRAINREC_WALK_BAD;
    } else {
        uint32_t nd = 0;
        if (!rdU32(r, &nd)) return BRAINREC_WALK_BAD;
        if (nd > (uint32_t)(MAP_ARRAY_SIZE * MAP_ARRAY_SIZE)) return BRAINREC_WALK_BAD;
        if (nd && !r->skip(r->ctx, (size_t)nd * 5)) return BRAINREC_WALK_BAD;
    }
    info.mapKeyframe = (kf != 0);

    uint8_t nbot = 0;
    if (!rdU8(r, &nbot)) return BRAINREC_WALK_BAD;
    info.botCount = nbot;
    for (int b = 0; b < nbot; b++) {
        uint8_t slot = 0;
        if (!rdU8(r, &slot)) return BRAINREC_WALK_BAD;
        if ((int)slot > info.maxBotSlot) info.maxBotSlot = slot;

        if (!r->skip(r->ctx, 4)) return BRAINREC_WALK_BAD;           /* thinkMs */
        if (!r->skip(r->ctx, 32 + 12 + 32)) return BRAINREC_WALK_BAD; /* goal head */
        uint16_t nc = 0;
        if (!rdU16(r, &nc)) return BRAINREC_WALK_BAD;
        if (nc && !r->skip(r->ctx, (size_t)nc * sizeof(BrainRecCandidate))) return BRAINREC_WALK_BAD;

        /* Packed overlay commands: 28 bytes each, the last of which is
         * the label length that a variable-length text tail follows. */
        uint32_t oc = 0;
        if (!rdU32(r, &oc)) return BRAINREC_WALK_BAD;
        for (uint32_t k = 0; k < oc; k++) {
            uint8_t tl = 0;
            if (!r->skip(r->ctx, 27)) return BRAINREC_WALK_BAD;
            if (!rdU8(r, &tl)) return BRAINREC_WALK_BAD;
            if (tl && !r->skip(r->ctx, tl)) return BRAINREC_WALK_BAD;
        }

        uint32_t pl = 0;
        if (!rdU32(r, &pl)) return BRAINREC_WALK_BAD;
        if (pl && !r->skip(r->ctx, pl)) return BRAINREC_WALK_BAD;
    }

    /* v5 tail: one alliance bitmap per player slot, written every frame. */
    if (!r->skip(r->ctx, (size_t)MAX_TANKS * 4)) return BRAINREC_WALK_BAD;

    if (out) *out = info;
    return BRAINREC_WALK_OK;
}
