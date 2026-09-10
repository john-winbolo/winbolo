/* Fuzz target — a downloaded map's fields, past the LZW gate.
 *
 * fuzz_map_load feeds mapLoadCompressedMap raw bytes, which is the right
 * shape for the decoder but cannot reach the interesting half: the loader
 * rejects anything whose terrain does not decode to exactly 65536 bytes, and
 * a mutated compressed stream almost never does. An hour of it plateaued in
 * 69 seconds having only ever loaded the one valid seed map.
 *
 * The space worth exploring is a map that loads successfully while carrying
 * attacker-chosen field values: that is what the bases/pillboxes/starts
 * clamps guard, and where baseTime (a wire int32_t), inTank (a wire bool) and
 * the terrain bytes live. The compressed path can seat terrain outside the
 * legal 0-15 + DEEP_SEA set, which the nibble-packed file format cannot.
 *
 * So this target builds a blob that always decodes: it takes the struct
 * region straight from the fuzzer, fills the terrain from the remaining
 * input, and compresses it with the same lzwencoding the map writer uses.
 * mapLoadCompressedMap is then called for real — no seam reimplementing the
 * loader, so this cannot drift from the code it is testing.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "types.h"
#include "bolo_map.h"
#include "bases.h"
#include "pillbox.h"
#include "starts.h"

int lzwencoding(unsigned char *src, unsigned char *dest, int len, int destCap);

#define STRUCT_REGION (SIZEOF_BASES + SIZEOF_PILLS + SIZEOF_STARTS)
#define TERRAIN_BYTES (MAP_ARRAY_SIZE * MAP_ARRAY_SIZE)
/* The RLE can expand: its worst input costs four output bytes per three in,
 * so 2x the terrain is comfortably above the bound. */
#define COMP_CAP      (TERRAIN_BYTES * 2)

static map       g_mp;
static pillboxes g_pb;
static bases     g_bs;
static starts    g_ss;
static BYTE     *g_terrain;
static BYTE     *g_comp;
static BYTE     *g_blob;

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
    mapCreate(&g_mp);
    pillsCreate(&g_pb);
    basesCreate(&g_bs);
    startsCreate(&g_ss);
    g_terrain = (BYTE *)malloc(TERRAIN_BYTES);
    g_comp    = (BYTE *)malloc(COMP_CAP);
    g_blob    = (BYTE *)malloc(STRUCT_REGION + COMP_CAP);
    if (g_terrain == NULL || g_comp == NULL || g_blob == NULL) {
        return -1;
    }
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    size_t terrainSeed;
    int compLen;

    /* Need the struct region plus at least one terrain byte to tile from. */
    if (size < STRUCT_REGION + 1) {
        return 0;
    }
    terrainSeed = size - STRUCT_REGION;

    /* Tile the remaining input across the terrain so a short input still
     * produces a full 256x256 map, and a long one supplies it directly. */
    {
        size_t filled = 0;
        while (filled < TERRAIN_BYTES) {
            size_t chunk = TERRAIN_BYTES - filled;
            if (chunk > terrainSeed) chunk = terrainSeed;
            memcpy(g_terrain + filled, data + STRUCT_REGION, chunk);
            filled += chunk;
        }
    }

    compLen = lzwencoding(g_terrain, g_comp, TERRAIN_BYTES, COMP_CAP);
    if (compLen <= 0 || compLen > COMP_CAP) {
        return 0;
    }

    memcpy(g_blob, data, STRUCT_REGION);
    memcpy(g_blob + STRUCT_REGION, g_comp, (size_t)compLen);
    (void)mapLoadCompressedMap(&g_mp, &g_pb, &g_bs, &g_ss,
                               g_blob, (int)(STRUCT_REGION + (size_t)compLen));
    return 0;
}
