/*
 * mapRead field-range clamping.
 *
 * The .map wire format has documented per-field ranges (pillbox
 * armour 0..15, base stocks 0..90, start dir 0..15, pillbox speed
 * 6..100 at runtime) but the parser historically accepted any byte
 * value. This test feeds mapRead a hand-built map blob whose
 * pillbox / base / start records hold values past every cap and
 * verifies the post-load structures sit inside the runtime range.
 *
 * It also exercises in-range values to confirm legitimate maps
 * survive the clamps untouched.
 *
 * Coverage is at the pillsSetPill / basesSetBase / startsSetStart
 * boundary — those are the single entry points the parser uses,
 * so this catches every wire path that funnels through them
 * (mapRead, snapshot decode, network state restore).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "bolo_map.h"
#include "pillbox.h"
#include "bases.h"
#include "starts.h"
#include "test_harness.h"

/* The malicious blob: BMAPBOLO header, version 1, then one pillbox,
 * one base, one start — each with every field cranked past its
 * documented cap — and a single run-terminator. The structures
 * read are 5/6/3 bytes respectively (SIZEOFBMAP_PILL_INFO etc).
 * Field order matches the on-disk layout (which is also the
 * first-N-bytes prefix of the struct thanks to the layout
 * static_asserts in types.h:233-239). */
static const uint8_t kEvilMap[] = {
    /* magic */
    'B','M','A','P','B','O','L','O',
    /* version */
    0x01,
    /* numPills, numBases, numStarts */
    0x01, 0x01, 0x01,
    /* pillbox: x=50 y=50 owner=NEUTRAL armour=255 speed=2 (under floor) */
    50, 50, 0xFF, 255, 2,
    /* base: x=60 y=60 owner=NEUTRAL armour=200 shells=150 mines=255 */
    60, 60, 0xFF, 200, 150, 255,
    /* start: x=70 y=70 dir=200 */
    70, 70, 200,
    /* run terminator */
    0x04, 0xFF, 0xFF, 0xFF,
};

/* A second blob with every field at the *legitimate* maximum, to
 * make sure the clamps don't truncate values a real editor would
 * produce. PILLS_MAX_ARMOUR=15, BASE_FULL_*=90, dir 0..15 nibble. */
static const uint8_t kLegitMap[] = {
    'B','M','A','P','B','O','L','O',
    0x01,
    0x01, 0x01, 0x01,
    /* pillbox: armour=15 (cap) speed=100 (cap) */
    50, 50, 0xFF, 15, 100,
    /* base: stocks=90 (cap) */
    60, 60, 0xFF, 90, 90, 90,
    /* start: dir=15 (cap) */
    70, 70, 15,
    0x04, 0xFF, 0xFF, 0xFF,
};

/* A third blob with pillbox speed at the lower bound (6) to make
 * sure the floor clamp doesn't trip on a legitimate "angry start"
 * pill. */
static const uint8_t kAngryStartMap[] = {
    'B','M','A','P','B','O','L','O',
    0x01,
    0x01, 0x00, 0x00,  /* one pill, no bases, no starts */
    50, 50, 0xFF, 10, 6,  /* speed = PILLBOX_MAX_FIRERATE */
    0x04, 0xFF, 0xFF, 0xFF,
};

static const char *kTempPath = "data/maps/.test_field_clamps.map";

static bool write_blob(const void *bytes, size_t len) {
    SDL_CreateDirectory("data/maps");
    FILE *fp = fopen(kTempPath, "wb");
    if (!fp) return false;
    size_t w = fwrite(bytes, 1, len, fp);
    fclose(fp);
    return w == len;
}

static bool load_and_inspect(const void *blob, size_t blobLen,
                              pillbox *outPill,
                              base    *outBase,
                              start   *outStart,
                              bool wantPill, bool wantBase, bool wantStart) {
    if (!write_blob(blob, blobLen)) return false;

    map mp = NULL;
    pillboxes pb = NULL;
    bases bs = NULL;
    starts ss = NULL;
    mapCreate(&mp);
    pillsCreate(&pb);
    basesCreate(&bs);
    startsCreate(&ss);

    bool ok = mapRead((char *)kTempPath, &mp, &pb, &bs, &ss);
    if (ok) {
        if (wantPill)  pillsGetPill(&pb, outPill, 1);
        if (wantBase)  basesGetBase(&bs, outBase, 1);
        if (wantStart) startsGetStartStruct(&ss, outStart, 1);
    }

    mapDestroy(&mp);
    pillsDestroy(&pb);
    basesDestroy(&bs);
    startsDestroy(&ss);
    return ok;
}

int run_map_field_clamps_evil(void) {
    pillbox p; memset(&p, 0, sizeof(p));
    base    b; memset(&b, 0, sizeof(b));
    start   s; memset(&s, 0, sizeof(s));

    UT_ASSERT_MSG(load_and_inspect(kEvilMap, sizeof(kEvilMap),
                                    &p, &b, &s, true, true, true),
                  "mapRead must accept a malformed-but-decodable map");

    /* Pill: armour clamped to 15, speed floored at 6. */
    UT_ASSERT_MSG(p.armour == PILLS_MAX_ARMOUR,
                  "pill armour 255 must clamp to %d, got %d",
                  PILLS_MAX_ARMOUR, p.armour);
    UT_ASSERT_MSG(p.speed == PILLBOX_MAX_FIRERATE,
                  "pill speed 2 must floor to %d, got %d",
                  PILLBOX_MAX_FIRERATE, p.speed);

    /* Base: all three stocks clamped to 90. */
    UT_ASSERT_MSG(b.armour == BASE_FULL_ARMOUR,
                  "base armour 200 must clamp to %d, got %d",
                  BASE_FULL_ARMOUR, b.armour);
    UT_ASSERT_MSG(b.shells == BASE_FULL_SHELLS,
                  "base shells 150 must clamp to %d, got %d",
                  BASE_FULL_SHELLS, b.shells);
    UT_ASSERT_MSG(b.mines == BASE_FULL_MINES,
                  "base mines 255 must clamp to %d, got %d",
                  BASE_FULL_MINES, b.mines);

    /* Start: dir > 15 clamped to 0. */
    UT_ASSERT_MSG(s.dir == 0,
                  "start dir 200 must clamp to 0, got %d", s.dir);

    SDL_RemovePath(kTempPath);
    return 0;
}

int run_map_field_clamps_passthrough(void) {
    pillbox p; memset(&p, 0, sizeof(p));
    base    b; memset(&b, 0, sizeof(b));
    start   s; memset(&s, 0, sizeof(s));

    UT_ASSERT(load_and_inspect(kLegitMap, sizeof(kLegitMap),
                                &p, &b, &s, true, true, true));

    /* Maxed-but-legitimate values must survive unchanged. */
    UT_ASSERT_MSG(p.armour == PILLS_MAX_ARMOUR,
                  "legitimate pill armour=15 must pass, got %d", p.armour);
    UT_ASSERT_MSG(p.speed == PILLBOX_ATTACK_NORMAL,
                  "legitimate pill speed=100 must pass, got %d", p.speed);
    UT_ASSERT_MSG(b.armour == BASE_FULL_ARMOUR, "got %d", b.armour);
    UT_ASSERT_MSG(b.shells == BASE_FULL_SHELLS, "got %d", b.shells);
    UT_ASSERT_MSG(b.mines  == BASE_FULL_MINES,  "got %d", b.mines);
    UT_ASSERT_MSG(s.dir == 15,
                  "legitimate start dir=15 must pass, got %d", s.dir);

    SDL_RemovePath(kTempPath);
    return 0;
}

int run_map_field_clamps_angry_start(void) {
    pillbox p; memset(&p, 0, sizeof(p));

    UT_ASSERT(load_and_inspect(kAngryStartMap, sizeof(kAngryStartMap),
                                &p, NULL, NULL, true, false, false));

    /* speed = PILLBOX_MAX_FIRERATE is the legitimate "starts angry"
     * floor — the clamp must accept it untouched, not bounce it
     * back up to ATTACK_NORMAL. */
    UT_ASSERT_MSG(p.speed == PILLBOX_MAX_FIRERATE,
                  "angry-start pill speed=%d must pass through, got %d",
                  PILLBOX_MAX_FIRERATE, p.speed);

    SDL_RemovePath(kTempPath);
    return 0;
}
