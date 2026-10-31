/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * A brainrec session keeps its own map, and the bases in it stay where the
 * recorded terrain has them.
 *
 * brainrec.btr frames carry each base's owner and stocks but not its
 * position, so BrainTest takes base positions from the map it loads. The
 * header only names the map; a generated map has no file under that name,
 * and BrainTest fell back to Everard Island and drew Everard's bases on the
 * recorded terrain, most of them in the sea. The recorder now saves the map
 * as BRAINREC_MAP_FILENAME beside the .btr and BrainTest loads that first.
 *
 * The case writes a map through brainRecWriteSessionMap, reads it back
 * through brainRecReadSessionMap and loads it the way BrainTest does, then
 * checks each base and the terrain under it. The map is moved off the
 * centre mapRead would put it on first: a generated map sits where the
 * generator put it, and a loader that recentred would move the bases off
 * the recorded terrain the same way the fallback did.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "types.h"
#include "bolo_map.h"
#include "pillbox.h"
#include "bases.h"
#include "starts.h"
#include "everard_map.h"
#include "brain_record.h"
#include "test_harness.h"

#define BRSM_SHIFT 2  /* how far off centre the map is moved */

static void brsmFree(map *mp, pillboxes *pb, bases *bs, starts *ss) {
    mapDestroy(mp);
    pillsDestroy(pb);
    basesDestroy(bs);
    startsDestroy(ss);
}

int run_brainrec_session_map_roundtrip(void) {
    static BYTE emap[6000] = E_MAP;
    static BYTE blob[MAP_COMPRESSED_MAX_SIZE];
    static BYTE back[MAP_COMPRESSED_MAX_SIZE];
    static BYTE tiny[16];
    map mp, mp2, shifted;
    pillboxes pb, pb2;
    bases bs, bs2;
    starts ss, ss2;
    char dir[1024];
    char missing[1024];
    int x, y, i, n, got;

    UT_ASSERT_MSG(utScratchPath(dir, sizeof dir, NULL), "no scratch dir");

    mapCreate(&mp);
    pillsCreate(&pb);
    basesCreate(&bs);
    startsCreate(&ss);
    UT_ASSERT_MSG(mapLoadCompressedMap(&mp, &pb, &bs, &ss, emap, E_MAP_LEN),
                  "embedded Everard map failed to decode");
    UT_ASSERT_MSG(basesGetNumBases(&bs) > 0, "Everard has no bases");

    /* Move the whole map, terrain and items together, off centre. */
    mapCreate(&shifted);
    for (x = 0; x + BRSM_SHIFT < MAP_ARRAY_SIZE; x++) {
        for (y = 0; y + BRSM_SHIFT < MAP_ARRAY_SIZE; y++) {
            shifted->mapItem[x + BRSM_SHIFT][y + BRSM_SHIFT] = mp->mapItem[x][y];
        }
    }
    mapDestroy(&mp);
    mp = shifted;
    pillsMoveAll(&pb, BRSM_SHIFT, BRSM_SHIFT);
    basesMoveAll(&bs, BRSM_SHIFT, BRSM_SHIFT);
    startsMoveAll(&ss, BRSM_SHIFT, BRSM_SHIFT);

    n = mapSaveCompressedMap(&mp, &pb, &bs, &ss, blob, (int)sizeof blob);
    UT_ASSERT_MSG(n > 0, "mapSaveCompressedMap returned %d", n);
    UT_ASSERT_MSG(brainRecWriteSessionMap(dir, blob, n),
                  "brainRecWriteSessionMap failed in %s", dir);

    got = brainRecReadSessionMap(dir, back, (int)sizeof back);
    UT_ASSERT_MSG(got == n, "read back %d bytes, wrote %d", got, n);
    UT_ASSERT_MSG(memcmp(back, blob, (size_t)n) == 0, "bytes read back differ");

    mapCreate(&mp2);
    pillsCreate(&pb2);
    basesCreate(&bs2);
    startsCreate(&ss2);
    UT_ASSERT_MSG(mapLoadCompressedMap(&mp2, &pb2, &bs2, &ss2, back, got),
                  "the saved session map does not load");

    UT_ASSERT_MSG(basesGetNumBases(&bs2) == basesGetNumBases(&bs),
                  "base count %u, saved %u",
                  (unsigned)basesGetNumBases(&bs2), (unsigned)basesGetNumBases(&bs));
    for (i = 0; i < basesGetNumBases(&bs); i++) {
        BYTE bx = bs->item[i].x, by = bs->item[i].y;
        UT_ASSERT_MSG(bs2->item[i].x == bx && bs2->item[i].y == by,
                      "base %d at (%u,%u), saved at (%u,%u)", i,
                      (unsigned)bs2->item[i].x, (unsigned)bs2->item[i].y,
                      (unsigned)bx, (unsigned)by);
        UT_ASSERT_MSG(mp2->mapItem[bx][by] == mp->mapItem[bx][by],
                      "terrain under base %d is %u, saved %u", i,
                      (unsigned)mp2->mapItem[bx][by], (unsigned)mp->mapItem[bx][by]);
        UT_ASSERT_MSG(mp2->mapItem[bx][by] != DEEP_SEA,
                      "base %d at (%u,%u) is on deep sea", i,
                      (unsigned)bx, (unsigned)by);
    }

    /* Too big for the buffer is refused, not cut short. */
    UT_ASSERT_MSG(brainRecReadSessionMap(dir, tiny, (int)sizeof tiny) == 0,
                  "an oversize session map was read into a short buffer");

    /* No file: nothing read, so BrainTest falls through to data/maps. */
    UT_ASSERT_MSG(utScratchPath(missing, sizeof missing, "no-session-here"),
                  "no scratch path");
    UT_ASSERT_MSG(brainRecReadSessionMap(missing, back, (int)sizeof back) == 0,
                  "a missing session map read as present");

    brsmFree(&mp, &pb, &bs, &ss);
    brsmFree(&mp2, &pb2, &bs2, &ss2);
    return 0;
}
