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
 *   capacity: the encoder must stay inside the output capacity it is handed
 *             and return 0 rather than run off the end of a buffer that is
 *             too small for the map.
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

    preSum = mapCalcChecksum(mp, bs, pb);
    n = mapSaveCompressedMap(mp, pb, bs, ss, blob, (int)sizeof(blob));
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

    postSum = mapCalcChecksum(&mp2, &bs2, &pb2);
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

/* Bytes past `from` in buf must still hold the canary pattern. Reports the
 * first byte the compressor touched. Returns 0 when the tail is clean. */
static int assert_tail_untouched(const BYTE *buf, int from, int to,
                                 BYTE canary, const char *what) {
    int i;
    for (i = from; i < to; i++) {
        if (buf[i] != canary) {
            UT_FAIL("%s: wrote %u at offset %d, past the capacity given",
                    what, (unsigned)buf[i], i);
        }
    }
    return 0;
}

/* The compressor must honour the output capacity it is given. An
 * incompressible map encodes larger than its input, and several destinations
 * in the tree are exactly MAP_DOWNLOAD_MAX_SIZE, so refusing has to be a
 * property of mapSaveCompressedMap rather than of the buffer it is handed.
 *
 *   generous capacity  -> the same length and the same bytes as always
 *   exactly enough     -> unchanged, and not one byte more
 *   one byte short     -> 0, tail untouched
 *   short of the fixed header -> 0, nothing written at all
 */
int run_map_compress_capacity_refuses(void) {
    static BYTE emap[6000] = E_MAP;
    static BYTE reference[131072];
    static BYTE dest[131072];
    const BYTE canary = 0xA5;
    const int headerLen = SIZEOF_BASES + SIZEOF_PILLS + SIZEOF_STARTS;
    map mp;
    pillboxes pb;
    bases bs;
    starts ss;
    int refLen, n, cap;

    mapCreate(&mp);
    pillsCreate(&pb);
    basesCreate(&bs);
    startsCreate(&ss);
    UT_ASSERT_MSG(mapLoadCompressedMap(&mp, &pb, &bs, &ss, emap, EMAP_LEN),
                  "embedded Everard map failed to decode");

    /* Generous capacity: the reference bytes every other case is measured
     * against. This is what the compressor produced before it took a
     * capacity at all. */
    refLen = mapSaveCompressedMap(&mp, &pb, &bs, &ss, reference,
                                  (int)sizeof(reference));
    UT_ASSERT_MSG(refLen > headerLen,
                  "generous capacity produced %d bytes, expected > %d",
                  refLen, headerLen);

    /* Exactly enough: same length, same bytes, and the byte after the blob
     * is still the caller's. */
    memset(dest, canary, sizeof(dest));
    n = mapSaveCompressedMap(&mp, &pb, &bs, &ss, dest, refLen);
    UT_ASSERT_MSG(n == refLen,
                  "exact capacity returned %d, expected %d", n, refLen);
    UT_ASSERT_MSG(memcmp(dest, reference, (size_t)refLen) == 0,
                  "exact capacity changed the compressed bytes");
    UT_ASSERT(assert_tail_untouched(dest, refLen, (int)sizeof(dest), canary,
                                    "exact capacity") == 0);

    /* One byte short: refused, and nothing written past the capacity. */
    cap = refLen - 1;
    memset(dest, canary, sizeof(dest));
    n = mapSaveCompressedMap(&mp, &pb, &bs, &ss, dest, cap);
    UT_ASSERT_MSG(n == 0, "a capacity of %d (map needs %d) returned %d, "
                          "expected 0", cap, refLen, n);
    UT_ASSERT(assert_tail_untouched(dest, cap, (int)sizeof(dest), canary,
                                    "one byte short") == 0);

    /* Short of the fixed bases/pills/starts header: refused before the first
     * struct copy, so the whole buffer is untouched. */
    cap = headerLen - 1;
    memset(dest, canary, sizeof(dest));
    n = mapSaveCompressedMap(&mp, &pb, &bs, &ss, dest, cap);
    UT_ASSERT_MSG(n == 0, "a capacity of %d (header is %d) returned %d, "
                          "expected 0", cap, headerLen, n);
    UT_ASSERT(assert_tail_untouched(dest, 0, (int)sizeof(dest), canary,
                                    "short of the header") == 0);

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
    sumMined = mapCalcChecksum(&gs->mp, &gs->bs, &gs->pb);

    for (i = 0; i < 3; i++) {
        mapSetPos(gs, &gs->mp, tx[i], ty[i], base[i], false, false);
    }
    sumBase = mapCalcChecksum(&gs->mp, &gs->bs, &gs->pb);

    UT_ASSERT_MSG(sumMined == sumBase,
                  "mined tiles changed the checksum: mined=%u base=%u",
                  (unsigned)sumMined, (unsigned)sumBase);

    /* Control: a normal terrain change (GRASS -> ROAD, neither in the mine
     * range) must still move the checksum, proving the mask isn't over-broad. */
    mapSetPos(gs, &gs->mp, tx[0], ty[0], GRASS, false, false);
    sumA = mapCalcChecksum(&gs->mp, &gs->bs, &gs->pb);
    mapSetPos(gs, &gs->mp, tx[0], ty[0], ROAD, false, false);
    sumB = mapCalcChecksum(&gs->mp, &gs->bs, &gs->pb);
    UT_ASSERT_MSG(sumA != sumB,
                  "a non-mine terrain change did not move the checksum (%u)",
                  (unsigned)sumA);

    serverSimDestroy(sim);
    return 0;
}

/* Stamp liveTerrain onto the base tile (bx,by) in gs's live map (mapSetPos
 * writes unconditionally, modelling a tank-explosion crater or a flood that
 * slipped under a base), round-trip the map through the compressed codec, then
 * assert (a) the decoded tile is forced back to ROAD and (b) the live and
 * round-tripped checksums converge. Returns 0 on success. */
static int assert_base_tile_converges(GameSim *gs, BYTE bx, BYTE by,
                                      BYTE liveTerrain, const char *what) {
    static BYTE blob[131072];
    int n;
    uint16_t liveSum, rtSum;
    BYTE decoded;
    map mp2;
    pillboxes pb2;
    bases bs2;
    starts ss2;

    mapSetPos(gs, &gs->mp, bx, by, liveTerrain, false, false);
    UT_ASSERT_MSG(mapGetPos(&gs->mp, bx, by) == liveTerrain,
                  "%s: failed to stamp live terrain under base", what);

    n = mapSaveCompressedMap(&gs->mp, &gs->pb, &gs->bs, &gs->ss, blob,
                             (int)sizeof(blob));
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

    /* The decode fixup forces every base tile back to ROAD. */
    decoded = mapGetPos(&mp2, bx, by);
    /* The checksum must fold base tiles to ROAD on both ends so the live map
     * (non-ROAD under the base) matches the round-tripped map (ROAD). Without
     * the fold these differ forever and the client resyncs endlessly. */
    liveSum = mapCalcChecksum(&gs->mp, &gs->bs, &gs->pb);
    rtSum   = mapCalcChecksum(&mp2, &bs2, &pb2);

    mapDestroy(&mp2);
    pillsDestroy(&pb2);
    basesDestroy(&bs2);
    startsDestroy(&ss2);

    UT_ASSERT_MSG(decoded == ROAD,
                  "%s: decoded base tile = %u, expected ROAD(%u)",
                  what, (unsigned)decoded, (unsigned)ROAD);
    UT_ASSERT_MSG(liveSum == rtSum,
                  "%s: live CRC %04x != round-trip CRC %04x — client cannot converge",
                  what, (unsigned)liveSum, (unsigned)rtSum);
    return 0;
}

/* Regression for the cratered-base resync loop. A tank exploding on (or next
 * to) a base craters the tile under it — CRATER, or RIVER once the flood
 * degrades a water-adjacent crater — but mapLoadCompressedMap forces base
 * tiles back to ROAD on decode. The terrain checksum must fold base/pill tiles
 * to ROAD too, otherwise the live map's CRC never matches any round-tripped
 * copy and clients request a fresh map forever (observed in-game on the west
 * column of bases on "Chewy somthin' or other"). */
int run_map_resync_base_crater_converges(void) {
    static BYTE emap[6000] = E_MAP;
    ServerSim *sim;
    GameSim *gs;
    BYTE bx = 0, by = 0;
    bool found = false;
    int x, y;

    sim = serverSimCreateCompressed(emap, EMAP_LEN, "Everard Island",
                                    gameOpen, false, 0, -1);
    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);

    /* Find a base tile by scanning (avoids depending on the bases struct
     * internals). Everard always has bases. */
    for (y = 0; y < MAP_ARRAY_SIZE && !found; y++) {
        for (x = 0; x < MAP_ARRAY_SIZE && !found; x++) {
            if (basesExistPos(&gs->bs, (BYTE)x, (BYTE)y)) {
                bx = (BYTE)x;
                by = (BYTE)y;
                found = true;
            }
        }
    }
    UT_ASSERT_MSG(found, "Everard map exposed no base tile to test");

    /* CRATER: the direct tank-explosion outcome. */
    UT_ASSERT(assert_base_tile_converges(gs, bx, by, CRATER, "base+crater") == 0);
    /* RIVER: the flood turns a water-adjacent cratered base into RIVER. */
    UT_ASSERT(assert_base_tile_converges(gs, bx, by, RIVER, "base+river") == 0);

    serverSimDestroy(sim);
    return 0;
}

/* The bound MAP_COMPRESSED_MAX_SIZE states has to be the encoder's actual
 * worst case, not a guess, because every buffer in the tree is sized to it.
 *
 * The RLE expands rather than compresses on its worst input: a three-byte
 * cycle of one literal followed by a two-byte run costs four output bytes,
 * two for the one-byte literal frame and two for the run. This drives a map
 * of exactly that shape through mapSaveCompressedMap and pins three things —
 * that the expansion is real, that the constant covers it, and that a buffer
 * sized to the uncompressed map does not.
 *
 * The last of those is the regression this guards: sizing a destination to
 * the 64 KiB a map occupies in memory looks right and is not, and the
 * compressor refuses on it silently rather than overrunning, so nothing
 * downstream says why the map never arrived. */
int run_map_compress_incompressible(void) {
    static BYTE emap[6000] = E_MAP;
    static BYTE dest[MAP_COMPRESSED_MAX_SIZE];
    const int headerLen = SIZEOF_BASES + SIZEOF_PILLS + SIZEOF_STARTS;
    const int terrainLen = MAP_ARRAY_SIZE * MAP_ARRAY_SIZE;
    map mp;
    pillboxes pb;
    bases bs;
    starts ss;
    int n, x, y;

    mapCreate(&mp);
    pillsCreate(&pb);
    basesCreate(&bs);
    startsCreate(&ss);
    UT_ASSERT_MSG(mapLoadCompressedMap(&mp, &pb, &bs, &ss, emap, EMAP_LEN),
                  "embedded Everard map failed to decode");

    /* ABB ABB ABB ... laid down in the order the encoder walks the array,
     * which is the flat mapItem order. Written straight into the array rather
     * than through mapSetPos: this is a compressor input, not a playable map,
     * and the point is the byte pattern. */
    for (y = 0; y < MAP_ARRAY_SIZE; y++) {
        for (x = 0; x < MAP_ARRAY_SIZE; x++) {
            int flat = (y * MAP_ARRAY_SIZE) + x;
            mp->mapItem[y][x] = (BYTE)((flat % 3 == 0) ? GRASS : SWAMP);
        }
    }

    /* Bigger than the terrain it came from — the case every 64 KiB buffer in
     * the tree used to assume away. */
    n = mapSaveCompressedMap(&mp, &pb, &bs, &ss, dest, (int)sizeof(dest));
    UT_ASSERT_MSG(n > terrainLen,
                  "the worst-case pattern encoded to %d bytes, which is not "
                  "larger than the %d it started as — the encoder's expansion "
                  "behaviour has changed and the bound needs re-deriving",
                  n, terrainLen);

    /* And inside the bound, which is what every caller is sized to. */
    UT_ASSERT_MSG(n <= (int)sizeof(dest),
                  "the worst-case pattern encoded to %d bytes, past the "
                  "MAP_COMPRESSED_MAX_SIZE of %d that every buffer in the "
                  "tree is sized to", n, (int)sizeof(dest));

    /* The 4/3 derivation, checked rather than trusted: the terrain half must
     * land on the bound the constant is built from. */
    UT_ASSERT_MSG(n - headerLen <= ((terrainLen * 4) / 3) + 1,
                  "the terrain encoded to %d bytes, past the 4/3 bound of %d "
                  "that MAP_COMPRESSED_MAX_SIZE is derived from",
                  n - headerLen, ((terrainLen * 4) / 3) + 1);

    /* A buffer sized to the uncompressed map is refused, not overrun. This is
     * the sizing bug itself: it is the obvious wrong number to pick.
     *
     * Refusing is not the same as writing nothing. The header goes down
     * before the terrain is encoded, and the encoder fills what it was given
     * before finding it has run out, so a refused call leaves the caller's
     * buffer partly written and returns 0 to say the contents mean nothing.
     * What it must never do is step past the capacity, so the slack after it
     * is what gets checked. */
    {
        const int cap = MAP_ARRAY_SIZE * MAP_ARRAY_SIZE;
        static BYTE tooSmall[(MAP_ARRAY_SIZE * MAP_ARRAY_SIZE) + 1024];
        const BYTE canary = 0xA5;
        memset(tooSmall, canary, sizeof(tooSmall));
        n = mapSaveCompressedMap(&mp, &pb, &bs, &ss, tooSmall, cap);
        UT_ASSERT_MSG(n == 0,
                      "a buffer the size of the uncompressed map (%d) took "
                      "this map in %d bytes — it should have been refused",
                      cap, n);
        UT_ASSERT(assert_tail_untouched(tooSmall, cap, (int)sizeof(tooSmall),
                                        canary, "refused at map size") == 0);
    }

    mapDestroy(&mp);
    pillsDestroy(&pb);
    basesDestroy(&bs);
    startsDestroy(&ss);
    return 0;
}

/* mapLoadCompressedMap dereferences both levels of all four world handles: the
 * parameters are pointers to pointer typedefs, and the compressed-data setters
 * and the (*value)->mapItem reads go straight in. A caller still holding a
 * handle that a teardown has nulled has to get a refusal back, not a fault
 * inside the loader.
 *
 * A control load brackets the refusals. The first proves the fixture decodes,
 * so a FALSE below is the null check talking and not a broken map; the last
 * proves the refused calls left the valid handles as they were. */
int run_map_compress_rejects_null_handles(void) {
    static BYTE emap[6000] = E_MAP;
    map mp;
    pillboxes pb;
    bases bs;
    starts ss;
    map nullMp = NULL;
    pillboxes nullPb = NULL;
    bases nullBs = NULL;
    starts nullSs = NULL;

    mapCreate(&mp);
    pillsCreate(&pb);
    basesCreate(&bs);
    startsCreate(&ss);

    UT_ASSERT_MSG(mapLoadCompressedMap(&mp, &pb, &bs, &ss, emap, EMAP_LEN),
                  "control: four valid handles failed to decode the embedded "
                  "Everard map");

    /* Outer level: the parameter itself is NULL. */
    UT_ASSERT_MSG(!mapLoadCompressedMap(NULL, &pb, &bs, &ss, emap, EMAP_LEN),
                  "map: a NULL parameter (outer level) was accepted");
    UT_ASSERT_MSG(!mapLoadCompressedMap(&mp, NULL, &bs, &ss, emap, EMAP_LEN),
                  "pillboxes: a NULL parameter (outer level) was accepted");
    UT_ASSERT_MSG(!mapLoadCompressedMap(&mp, &pb, NULL, &ss, emap, EMAP_LEN),
                  "bases: a NULL parameter (outer level) was accepted");
    UT_ASSERT_MSG(!mapLoadCompressedMap(&mp, &pb, &bs, NULL, emap, EMAP_LEN),
                  "starts: a NULL parameter (outer level) was accepted");

    /* Inner level: the parameter points at a handle that is itself NULL. */
    UT_ASSERT_MSG(!mapLoadCompressedMap(&nullMp, &pb, &bs, &ss, emap, EMAP_LEN),
                  "map: a NULL handle (inner level) was accepted");
    UT_ASSERT_MSG(!mapLoadCompressedMap(&mp, &nullPb, &bs, &ss, emap, EMAP_LEN),
                  "pillboxes: a NULL handle (inner level) was accepted");
    UT_ASSERT_MSG(!mapLoadCompressedMap(&mp, &pb, &nullBs, &ss, emap, EMAP_LEN),
                  "bases: a NULL handle (inner level) was accepted");
    UT_ASSERT_MSG(!mapLoadCompressedMap(&mp, &pb, &bs, &nullSs, emap, EMAP_LEN),
                  "starts: a NULL handle (inner level) was accepted");

    UT_ASSERT_MSG(mapLoadCompressedMap(&mp, &pb, &bs, &ss, emap, EMAP_LEN),
                  "the refused calls disturbed the valid handles: the control "
                  "load no longer succeeds");

    mapDestroy(&mp);
    pillsDestroy(&pb);
    basesDestroy(&bs);
    startsDestroy(&ss);
    return 0;
}
