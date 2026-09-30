/* Fuzz target — a downloaded map's fields, past the zlib gate.
 *
 * fuzz_map_load feeds mapLoadCompressedMap raw bytes, which is the right
 * shape for the decoder but cannot reach the interesting half: the loader
 * rejects anything that is not a whole zlib stream inflating to exactly the
 * struct region plus 65536 terrain bytes, and a mutated stream almost never
 * passes zlib's checksum.
 *
 * The space worth exploring is a map that loads successfully while carrying
 * attacker-chosen field values: that is what the bases/pillboxes/starts
 * clamps guard, and where baseTime (a wire int32_t), inTank (a wire bool) and
 * the terrain bytes live. The compressed path can seat terrain outside the
 * legal 0-15 + DEEP_SEA set, which the nibble-packed file format cannot.
 *
 * So this target builds a blob that always decodes: it takes the struct
 * region straight from the fuzzer, fills the terrain from the remaining
 * input, and compresses the two with zlib the way the map writer does.
 * mapLoadCompressedMap is then called for real — no seam reimplementing the
 * loader, so this cannot drift from the code it is testing.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "global.h"
#include "types.h"
#include "bolo_map.h"
#include "bases.h"
#include "pillbox.h"
#include "starts.h"

#define STRUCT_REGION (SIZEOF_BASES + SIZEOF_PILLS + SIZEOF_STARTS)
#define TERRAIN_BYTES (MAP_ARRAY_SIZE * MAP_ARRAY_SIZE)

static map       g_mp;
static pillboxes g_pb;
static bases     g_bs;
static starts    g_ss;
static BYTE     *g_raw;   /* struct region | terrain, before compression */
static BYTE     *g_comp;

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
    mapCreate(&g_mp);
    pillsCreate(&g_pb);
    basesCreate(&g_bs);
    startsCreate(&g_ss);
    g_raw  = (BYTE *)malloc(MAP_UNCOMPRESSED_SIZE);
    g_comp = (BYTE *)malloc(MAP_COMPRESSED_MAX_SIZE);
    if (g_raw == NULL || g_comp == NULL) {
        return -1;
    }
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    size_t terrainSeed;
    uLongf compLen;

    /* Need the struct region plus at least one terrain byte to tile from. */
    if (size < STRUCT_REGION + 1) {
        return 0;
    }
    terrainSeed = size - STRUCT_REGION;

    memcpy(g_raw, data, STRUCT_REGION);

    /* Tile the remaining input across the terrain so a short input still
     * produces a full 256x256 map, and a long one supplies it directly. */
    {
        size_t filled = 0;
        while (filled < TERRAIN_BYTES) {
            size_t chunk = TERRAIN_BYTES - filled;
            if (chunk > terrainSeed) chunk = terrainSeed;
            memcpy(g_raw + STRUCT_REGION + filled, data + STRUCT_REGION, chunk);
            filled += chunk;
        }
    }

    compLen = MAP_COMPRESSED_MAX_SIZE;
    if (compress(g_comp, &compLen, g_raw, MAP_UNCOMPRESSED_SIZE) != Z_OK) {
        return 0;
    }
    (void)mapLoadCompressedMap(&g_mp, &g_pb, &g_bs, &g_ss,
                               g_comp, (int)compLen);
    return 0;
}
