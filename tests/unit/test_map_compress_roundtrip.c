/* test_map_compress_roundtrip.c — the map compressed-codec must preserve
 * terrain across mapSaveCompressedMap -> mapLoadCompressedMap.
 *
 * This is the deterministic reproducer behind the client map-resync loop: the
 * server hands the client a compressed map blob on a resync, and if that blob
 * does not decode back to the exact live terrain the client's checksum can
 * never match and it requests resync after resync until it self-kicks. A
 * lossy/asymmetric compressor — especially on mine-range terrain values
 * ([MINE_START, MINE_END]) or terrain adjacent to pills/bases — is the kind of
 * defect this guards against.
 *
 *   stock   : load a real map, round-trip it, assert checksum + every tile is
 *             byte-identical.
 *   mutated : seed a real map, overwrite a spread of interior land tiles
 *             (cycling through mine-range and ordinary terrains, skipping
 *             pill/base tiles that the loader intentionally normalises to
 *             ROAD), round-trip, assert the live and decoded maps match
 *             tile-for-tile.  On a mismatch the failure names the first
 *             differing (x, y, before, after) tile — the localized bug signal.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "bolo_map.h"
#include "pillbox.h"
#include "bases.h"
#include "starts.h"
#include "game_sim.h"
#include "gametype.h"
#include "server_sim.h"
#include "server_sim_lifecycle.h"
#include "everard_map.h"
#include "test_harness.h"

#define EMAP_LEN 5097 /* compressed length of E_MAP (matches the harness) */

/* Assert two maps are tile-for-tile identical across the whole 256x256 grid
 * (mapGetPos returns DEEP_SEA outside the mineable edges, so the boundary is
 * compared too). Returns 0 on equality; on the first mismatch reports the tile
 * and returns non-zero. */
static int assert_maps_equal(map *a, map *b, const char *what) {
    int x, y;
    for (y = 0; y < MAP_ARRAY_SIZE; y++) {
        for (x = 0; x < MAP_ARRAY_SIZE; x++) {
            BYTE av = mapGetPos(a, (BYTE)x, (BYTE)y);
            BYTE bv = mapGetPos(b, (BYTE)x, (BYTE)y);
            if (av != bv) {
                UT_FAIL("%s: tile (%d,%d) differs before=%u after=%u",
                        what, x, y, (unsigned)av, (unsigned)bv);
            }
        }
    }
    return 0;
}

/* Round-trip the live map (mp/pb/bs/ss) through the compressed codec into a
 * fresh scratch map and assert checksum + full tile equality. */
static int roundtrip_and_check(map *mp, pillboxes *pb, bases *bs, starts *ss,
                               const char *what) {
    static BYTE blob[131072];
    int n;
    uint16_t preSum, postSum;
    map mp2;
    pillboxes pb2;
    bases bs2;
    starts ss2;
    int rc;

    preSum = mapCalcChecksum(mp);
    n = mapSaveCompressedMap(mp, pb, bs, ss, blob);
    UT_ASSERT_MSG(n > 0, "%s: mapSaveCompressedMap returned %d", what, n);

    mapCreate(&mp2);
    pillsCreate(&pb2);
    basesCreate(&bs2);
    startsCreate(&ss2);
    if (!mapLoadCompressedMap(&mp2, &pb2, &bs2, &ss2, blob, n)) {
        mapDestroy(&mp2);
        pillsDestroy(&pb2);
        basesDestroy(&bs2);
        startsDestroy(&ss2);
        UT_FAIL("%s: mapLoadCompressedMap rejected the codec's own output", what);
    }

    postSum = mapCalcChecksum(&mp2);
    if (postSum != preSum) {
        rc = assert_maps_equal(mp, &mp2, what); /* names the differing tile */
        mapDestroy(&mp2);
        pillsDestroy(&pb2);
        basesDestroy(&bs2);
        startsDestroy(&ss2);
        /* assert_maps_equal fails the test if any tile differs; if it somehow
         * returned 0 the checksum disagreed without a tile diff — still a bug. */
        UT_ASSERT_MSG(rc != 0,
                      "%s: checksum changed %u -> %u with no differing tile",
                      what, (unsigned)preSum, (unsigned)postSum);
    }

    rc = assert_maps_equal(mp, &mp2, what);
    mapDestroy(&mp2);
    pillsDestroy(&pb2);
    basesDestroy(&bs2);
    startsDestroy(&ss2);
    return rc;
}

/* Stock: a real map round-trips losslessly. Uses the embedded Everard stock
 * map (always available in-process), and additionally the on-disk
 * "Big island.map" when the test's working directory exposes it. */
int run_map_compress_roundtrip_stock(void) {
    static BYTE emap[6000] = E_MAP;
    map mp;
    pillboxes pb;
    bases bs;
    starts ss;
    char bigIsland[] = "data/maps/Big island.map";

    mapCreate(&mp);
    pillsCreate(&pb);
    basesCreate(&bs);
    startsCreate(&ss);
    UT_ASSERT_MSG(mapLoadCompressedMap(&mp, &pb, &bs, &ss, emap, EMAP_LEN),
                  "embedded Everard map failed to decode");
    UT_ASSERT(roundtrip_and_check(&mp, &pb, &bs, &ss, "everard") == 0);
    mapDestroy(&mp);
    pillsDestroy(&pb);
    basesDestroy(&bs);
    startsDestroy(&ss);

    /* Opportunistic on-disk map: covered when present, skipped (not failed)
     * when the working directory does not carry data/maps. */
    mapCreate(&mp);
    pillsCreate(&pb);
    basesCreate(&bs);
    startsCreate(&ss);
    if (mapRead(bigIsland, &mp, &pb, &bs, &ss)) {
        UT_ASSERT(roundtrip_and_check(&mp, &pb, &bs, &ss, "big-island") == 0);
    } else {
        fprintf(stderr, "  (skipped '%s' — not reachable from cwd)\n", bigIsland);
    }
    mapDestroy(&mp);
    pillsDestroy(&pb);
    basesDestroy(&bs);
    startsDestroy(&ss);
    return 0;
}

/* Mutated: overwrite a spread of interior land tiles with mine-range and
 * ordinary terrain, then round-trip the live server map. A ServerSim provides
 * the GameSim mapSetPos needs (isServer terrain writes). */
int run_map_compress_roundtrip_mutated(void) {
    static BYTE emap[6000] = E_MAP;
    static const BYTE palette[] = {
        MINE_START, MINE_START + 2, MINE_END, /* mine-range terrain */
        GRASS, SWAMP, CRATER, RUBBLE, FOREST
    };
    ServerSim *sim;
    GameSim *gs;
    int x, y, idx = 0, mutated = 0;

    sim = serverSimCreateCompressed(emap, EMAP_LEN, "Everard Island",
                                    gameOpen, false, 0, -1);
    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);

    /* Sparse grid over the mineable interior. Skip deep sea (keep edits on
     * real terrain, where pills/bases sit, so adjacent tiles get mutated too)
     * and skip pill/base tiles themselves — the loader normalises those to
     * ROAD, an intended asymmetry that is out of scope here. */
    for (y = MAP_MINE_EDGE_TOP + 1; y < MAP_MINE_EDGE_BOTTOM; y += 5) {
        for (x = MAP_MINE_EDGE_LEFT + 1; x < MAP_MINE_EDGE_RIGHT; x += 7) {
            BYTE bx = (BYTE)x, by = (BYTE)y;
            if (mapGetPos(&gs->mp, bx, by) == DEEP_SEA) continue;
            if (pillsExistPos(&gs->pb, bx, by)) continue;
            if (basesExistPos(&gs->bs, bx, by)) continue;
            mapSetPos(gs, &gs->mp, bx, by,
                      palette[idx % (int)(sizeof(palette))],
                      /*needSend=*/false, /*mineClear=*/false);
            idx++;
            mutated++;
        }
    }
    UT_ASSERT_MSG(mutated > 0, "mutation pass changed no tiles");
    fprintf(stderr, "  mutated %d interior tiles (incl. mine-range)\n", mutated);

    UT_ASSERT(roundtrip_and_check(&gs->mp, &gs->pb, &gs->bs, &gs->ss,
                                  "mutated") == 0);

    serverSimDestroy(sim);
    return 0;
}

/* Find an interior land tile (not deep sea, not a pill/base) at or after the
 * scan cursor (*sx, *sy). Returns true and writes the tile + advances the
 * cursor past it; false if none remain. */
static bool next_land_tile(GameSim *gs, int *sx, int *sy, BYTE *outx, BYTE *outy) {
    int x, y;
    for (y = *sy; y < MAP_MINE_EDGE_BOTTOM; y++) {
        for (x = (y == *sy ? *sx : MAP_MINE_EDGE_LEFT + 1);
             x < MAP_MINE_EDGE_RIGHT; x++) {
            BYTE bx = (BYTE)x, by = (BYTE)y;
            if (mapGetPos(&gs->mp, bx, by) == DEEP_SEA) continue;
            if (pillsExistPos(&gs->pb, bx, by)) continue;
            if (basesExistPos(&gs->bs, bx, by)) continue;
            *outx = bx;
            *outy = by;
            *sx = x + 1;
            *sy = y;
            return true;
        }
    }
    return false;
}

/* The terrain checksum must ignore mine state: a mined tile (value in
 * [MINE_START, MINE_END]) checksums identically to its base terrain
 * (value - MINE_SUBTRACT), so a hidden-mines game can't mismatch forever. A
 * real (non-mine) terrain change must still move the checksum. */
int run_map_checksum_ignores_mines(void) {
    static BYTE emap[6000] = E_MAP;
    /* Each mined value paired with the base terrain it must reduce to. */
    static const BYTE mined[] = { MINE_GRASS, MINE_SWAMP, MINE_ROAD };
    static const BYTE base[]  = { GRASS,      SWAMP,      ROAD };
    ServerSim *sim;
    GameSim *gs;
    int sx = MAP_MINE_EDGE_LEFT + 1, sy = MAP_MINE_EDGE_TOP + 1;
    BYTE tx[3], ty[3];
    uint16_t sumMined, sumBase, sumA, sumB;
    int i;

    sim = serverSimCreateCompressed(emap, EMAP_LEN, "Everard Island",
                                    gameOpen, false, 0, -1);
    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);

    for (i = 0; i < 3; i++) {
        UT_ASSERT_MSG(next_land_tile(gs, &sx, &sy, &tx[i], &ty[i]),
                      "ran out of interior land tiles (i=%d)", i);
    }

    /* Mine the tiles, checksum; then strip each to its base terrain, checksum.
     * Masking must make the two identical. */
    for (i = 0; i < 3; i++) {
        mapSetPos(gs, &gs->mp, tx[i], ty[i], mined[i], false, false);
    }
    sumMined = mapCalcChecksum(&gs->mp);

    for (i = 0; i < 3; i++) {
        mapSetPos(gs, &gs->mp, tx[i], ty[i], base[i], false, false);
    }
    sumBase = mapCalcChecksum(&gs->mp);

    UT_ASSERT_MSG(sumMined == sumBase,
                  "mined tiles changed the checksum: mined=%u base=%u",
                  (unsigned)sumMined, (unsigned)sumBase);

    /* Control: a normal terrain change (GRASS -> ROAD, neither in the mine
     * range) must still move the checksum, proving the mask isn't over-broad. */
    mapSetPos(gs, &gs->mp, tx[0], ty[0], GRASS, false, false);
    sumA = mapCalcChecksum(&gs->mp);
    mapSetPos(gs, &gs->mp, tx[0], ty[0], ROAD, false, false);
    sumB = mapCalcChecksum(&gs->mp);
    UT_ASSERT_MSG(sumA != sumB,
                  "a non-mine terrain change did not move the checksum (%u)",
                  (unsigned)sumA);

    serverSimDestroy(sim);
    return 0;
}
