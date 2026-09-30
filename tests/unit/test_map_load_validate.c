/*
 * Pins the clamps a map handed to mapLoadCompressedMap comes out with.
 *
 * A compressed map arrives from whatever server the player joined, and the
 * three *SetCompressData calls that unpack its bases, pillboxes and starts
 * memcpy the wire structs wholesale, so nothing has looked at a field before
 * the loader's own clamps: a downloaded map could otherwise seat an owner of
 * 200, a pill armour of 255 or a start dir of 99 in live game state.
 *
 * The clamps come in two halves and this walks both, because a sim sees the
 * result of both. basesValidate / pillsValidate / startsValidate settle what
 * the file may say — the counts, the owners, the start dir — and run without
 * a sim, so the preview and the map editor get them too. mapClampToRules
 * settles what this sim lets a pill or a base hold, and every caller that
 * owns a sim runs it straight after the load.
 *
 * Coordinates need no clamp and are deliberately not asserted here: x and y
 * are BYTE against a 256x256 map, so every value they can hold is in range.
 *
 * The blob these tests build is a hostile struct region over an all-DEEP_SEA
 * terrain, compressed the way mapSaveCompressedMap does it, so the load goes
 * all the way through. The assertions are about the structs, not the return.
 */
#include <stdint.h>
#include <string.h>
#include <zlib.h>

#include "global.h"
#include "types.h"
#include "bolo_map.h"
#include "bases.h"
#include "pillbox.h"
#include "starts.h"
#include "game_sim.h"
#include "test_harness.h"

/* bases | pillboxes | starts | terrain, before compression */
#define BLOB_STRUCTS (SIZEOF_BASES + SIZEOF_PILLS + SIZEOF_STARTS)

/* Build the blob the way the loader reads it: *SetCompressData memcpys
 * SIZEOF_* bytes straight over the object struct, so the struct region is
 * that struct's leading bytes. Populating real objects and copying them out keeps
 * this symmetric with the loader instead of guessing field offsets, which the
 * mixed pack(4)/pack(1) regions in types.h would make brittle.
 *
 * owner is 200 rather than 0xFF on purpose: 0xFF is NEUTRAL, which is a legal
 * owner, so poisoning with it would not exercise the clamp at all. */
#define HOSTILE_OWNER 200
#define HOSTILE_DIR   99

static void build_hostile_raw(BYTE *blob) {
    bases      bsSrc;
    pillboxes  pbSrc;
    starts     ssSrc;
    BYTE i;

    memset(blob, 0, BLOB_STRUCTS);
    memset(blob + BLOB_STRUCTS, DEEP_SEA, MAP_UNCOMPRESSED_SIZE - BLOB_STRUCTS);

    basesCreate(&bsSrc);
    basesSetNumBases(&bsSrc, MAX_BASES);
    for (i = 0; i < MAX_BASES; i++) {
        bsSrc->item[i].x      = i;
        bsSrc->item[i].y      = i;
        bsSrc->item[i].owner  = HOSTILE_OWNER;
        bsSrc->item[i].armour = 255;
        bsSrc->item[i].shells = 255;
        bsSrc->item[i].mines  = 255;
    }
    memcpy(blob, &(*bsSrc), SIZEOF_BASES);
    basesDestroy(&bsSrc);

    pillsCreate(&pbSrc);
    pillsSetNumPills(&pbSrc, MAX_PILLS);
    for (i = 0; i < MAX_PILLS; i++) {
        pbSrc->item[i].x      = i;
        pbSrc->item[i].y      = i;
        pbSrc->item[i].owner  = HOSTILE_OWNER;
        pbSrc->item[i].armour = 255;
        pbSrc->item[i].speed  = 255;
    }
    memcpy(blob + SIZEOF_BASES, &(*pbSrc), SIZEOF_PILLS);
    pillsDestroy(&pbSrc);

    startsCreate(&ssSrc);
    startsSetNumStarts(&ssSrc, MAX_STARTS);
    for (i = 0; i < MAX_STARTS; i++) {
        ssSrc->item[i].x   = i;
        ssSrc->item[i].y   = i;
        ssSrc->item[i].dir = HOSTILE_DIR;
    }
    memcpy(blob + SIZEOF_BASES + SIZEOF_PILLS, &(*ssSrc), SIZEOF_STARTS);
    startsDestroy(&ssSrc);
}

static int load_hostile(map *mp, pillboxes *pb, bases *bs, starts *ss) {
    static BYTE raw[MAP_UNCOMPRESSED_SIZE];
    static BYTE blob[MAP_COMPRESSED_MAX_SIZE];
    uLongf blobLen = sizeof(blob);
    mapCreate(mp);
    pillsCreate(pb);
    basesCreate(bs);
    startsCreate(ss);
    build_hostile_raw(raw);
    UT_ASSERT_MSG(compress(blob, &blobLen, raw, sizeof(raw)) == Z_OK,
                  "could not compress the hostile blob");
    UT_ASSERT_MSG(mapLoadCompressedMap(mp, pb, bs, ss, blob, (int)blobLen),
                  "the hostile blob did not load");
    /* The second half, the one a sim does for itself. These lists are local
     * rather than a sim's, so the two halves of mapClampToRules are called
     * on them directly. */
    pillsClampToRules(ut_rules_only_sim(), pb);
    basesClampToRules(ut_rules_only_sim(), bs);
    return 0;
}

static void destroy_all(map *mp, pillboxes *pb, bases *bs, starts *ss) {
    mapDestroy(mp);
    pillsDestroy(pb);
    basesDestroy(bs);
    startsDestroy(ss);
}

int run_map_load_clamps_base_fields(void) {
    map mp; pillboxes pb; bases bs; starts ss;
    BYTE i, n;

    UT_ASSERT(load_hostile(&mp, &pb, &bs, &ss) == 0);
    n = basesGetNumBases(&bs);
    UT_ASSERT_MSG(n <= MAX_BASES, "numBases %u exceeds MAX_BASES", (unsigned)n);
    for (i = 0; i < n; i++) {
        base *b = &(bs->item[i]);
        UT_ASSERT_MSG(b->owner <= (MAX_TANKS - 1) || b->owner == NEUTRAL,
                      "base %u owner %u is neither a valid slot nor NEUTRAL",
                      (unsigned)i, (unsigned)b->owner);
        UT_ASSERT_MSG(b->armour <= BASE_FULL_ARMOUR,
                      "base %u armour %u over BASE_FULL_ARMOUR", (unsigned)i, (unsigned)b->armour);
        UT_ASSERT_MSG(b->shells <= BASE_FULL_SHELLS,
                      "base %u shells %u over BASE_FULL_SHELLS", (unsigned)i, (unsigned)b->shells);
        UT_ASSERT_MSG(b->mines <= BASE_FULL_MINES,
                      "base %u mines %u over BASE_FULL_MINES", (unsigned)i, (unsigned)b->mines);
    }
    destroy_all(&mp, &pb, &bs, &ss);
    return 0;
}

int run_map_load_clamps_pill_fields(void) {
    map mp; pillboxes pb; bases bs; starts ss;
    BYTE i, n;

    UT_ASSERT(load_hostile(&mp, &pb, &bs, &ss) == 0);
    n = pillsGetNumPills(&pb);
    UT_ASSERT_MSG(n <= MAX_PILLS, "numPills %u exceeds MAX_PILLS", (unsigned)n);
    for (i = 0; i < n; i++) {
        pillbox *p = &(pb->item[i]);
        UT_ASSERT_MSG(p->owner <= (MAX_TANKS - 1) || p->owner == NEUTRAL,
                      "pill %u owner %u is neither a valid slot nor NEUTRAL",
                      (unsigned)i, (unsigned)p->owner);
        /* armour indexes a 16-entry tile table in the map preview. */
        UT_ASSERT_MSG(p->armour <= PILLS_MAX_ARMOUR,
                      "pill %u armour %u over PILLS_MAX_ARMOUR", (unsigned)i, (unsigned)p->armour);
        UT_ASSERT_MSG(p->speed >= PILLBOX_MAX_FIRERATE && p->speed <= PILLBOX_ATTACK_NORMAL,
                      "pill %u speed %u outside the runtime range", (unsigned)i, (unsigned)p->speed);
    }
    destroy_all(&mp, &pb, &bs, &ss);
    return 0;
}

int run_map_load_clamps_start_dir(void) {
    map mp; pillboxes pb; bases bs; starts ss;
    BYTE i, n;

    UT_ASSERT(load_hostile(&mp, &pb, &bs, &ss) == 0);
    n = startsGetNumStarts(&ss);
    UT_ASSERT_MSG(n <= MAX_STARTS, "numStarts %u exceeds MAX_STARTS", (unsigned)n);
    for (i = 0; i < n; i++) {
        /* dir indexes the direction tables, so it must be 0-15. */
        UT_ASSERT_MSG(ss->item[i].dir <= 15,
                      "start %u dir %u is outside 0-15",
                      (unsigned)i, (unsigned)ss->item[i].dir);
    }
    destroy_all(&mp, &pb, &bs, &ss);
    return 0;
}
