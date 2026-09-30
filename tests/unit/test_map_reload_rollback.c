/*
 * serverSimReloadMap rollback on mapRead failure.
 *
 * serverSimReloadMap wipes the live map / pill / base / start
 * structures before calling mapRead so that cells left out of the
 * new map's RLE runs don't keep the previous map's values. Until
 * the rollback fix, mapRead failure left those structures wiped —
 * sim->cachedMapData still held the previous map's compressed bytes
 * (so the wire side was fine), but the in-memory simulation state
 * was empty: 0 pills, 0 bases, 0 starts, all DEEP_SEA. A host who
 * pressed "Start Game" without first picking a different map or
 * sending PREVIEW_CANCEL would have started a game with no spawn
 * points.
 *
 * The fix re-decodes cachedMapData into the live structures on the
 * failure path, making serverSimReloadMap transactional: either the
 * new map loads or nothing changed.
 *
 * This test serialises the live state both before and after a
 * failed reload via serverSimGetCompressedMap and asserts the
 * bytes are byte-for-byte identical. That covers mapItem AND
 * pills/bases/starts in one comparison — strictly stronger than
 * any single-structure checksum.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_lifecycle.h"
#include "everard_map.h"
#include "test_harness.h"

static const char *kBadMapPath = "data/maps/.test_reload_rollback_bad.map";

/* Write a .map file that passes the magic+version gate but fails
 * mapRead inside mapReadRuns. The trick: declare numPills /
 * numBases / numStarts then end the file before any actual pill /
 * base / start bytes — mapReadPills will fread() short and return
 * FALSE. That exercises a real failure path without depending on
 * any particular malformed-RLE shape. */
static bool write_malformed_map(void) {
    SDL_CreateDirectory("data/maps");
    FILE *fp = fopen(kBadMapPath, "wb");
    if (!fp) return false;
    static const uint8_t blob[] = {
        'B','M','A','P','B','O','L','O',  /* magic */
        0x01,                              /* version */
        0x10, 0x10, 0x10,                  /* numPills/Bases/Starts = 16 each */
        /* ... and nothing else. mapReadPills will fread 5 bytes
         * for the first pill, hit EOF, return FALSE. */
    };
    size_t w = fwrite(blob, 1, sizeof(blob), fp);
    fclose(fp);
    return w == sizeof(blob);
}

int run_map_reload_rollback(void) {
    /* Fresh lobby sim seeded from Everard. serverSimCreateCompressed
     * leaves the sim in lobby state and populates cachedMapData. */
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN,
                                                "Everard Island",
                                                gameOpen, false, 0, -1);
    UT_ASSERT(sim != NULL);
    serverSimSetLobbyEnabled(sim, true);

    /* Snapshot the live state (compressed serialisation) before the
     * failed reload. serverSimGetCompressedMap re-encodes from the
     * live mp/pb/bs/ss into the caller's buffer; 131,072 bytes is
     * the same headroom serverSimReloadMap allocates internally. */
    BYTE before[131072];
    int beforeLen = serverSimGetCompressedMap(sim, before, (int)sizeof(before));
    UT_ASSERT_MSG(beforeLen > 0, "pre-reload compressed map must be non-empty");

    /* Stage and attempt the malformed reload. */
    UT_ASSERT(write_malformed_map());
    bool ok = serverSimReloadMap(sim, kBadMapPath);
    UT_ASSERT_MSG(!ok, "serverSimReloadMap must report failure for a malformed map");

    /* Re-serialise the live state. If the rollback restored the
     * mp / pb / bs / ss structures the bytes must match
     * byte-for-byte; if the structures were left wiped (the bug
     * this test guards against) the encoding will differ — empty
     * pills/bases/starts shrink the payload, and the all-DEEP_SEA
     * mapItem compresses very differently from a real map. */
    BYTE after[131072];
    int afterLen = serverSimGetCompressedMap(sim, after, (int)sizeof(after));
    UT_ASSERT_MSG(afterLen == beforeLen,
                  "compressed length differs: before=%d after=%d",
                  beforeLen, afterLen);
    UT_ASSERT_MSG(memcmp(before, after, (size_t)beforeLen) == 0,
                  "compressed map bytes differ after failed reload — "
                  "rollback did not restore live structures");

    SDL_RemovePath(kBadMapPath);
    serverSimDestroy(sim);
    return 0;
}
