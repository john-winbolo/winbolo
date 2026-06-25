/*
 * Real spectator seed-load reproduction (lv_specSeedLoad, logviewer-world).
 *
 * Loads the committed ring-seed fixture (tests/fixtures/wbv/spectator_seed.bin,
 * produced by the spectator_seed_capture dispatch test) — the exact
 * [u32 bodyLen][world body][u32 ctrlLen][control] blob the server sends a
 * connecting spectator, with NO trailing data. Unlike the .wbv-derived seed in
 * test_spec_seed_load (whose trailing event records mask an over-read in
 * lv_processSnapshot), this seed ends exactly at the snapshot, so the over-read
 * trips the reader's length/bounds check and lv_specSeedLoad returns FALSE.
 *
 * The test asserts TRUE: it exercises the production spectate path (no logStart,
 * NULL info) that the existing seed-load test missed, and passes once the
 * empty-map-name seed loads.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lv_global.h"
#include "backend.h"
#include "logviewer.h"
#include "lv_log.h"
#include "test_harness.h"

#ifndef WB_WBV_FIXTURE_DIR
#define WB_WBV_FIXTURE_DIR "tests/fixtures/wbv"
#endif

int run_spec_seed_load_real(void) {
  const char *dir = getenv("WB_WBV_FIXTURE_DIR");
  char path[512];
  FILE *f;
  long sz;
  uint8_t *seed;
  size_t seedLen;
  LogViewerState *lv;
  bool loaded;

  if (dir == NULL || dir[0] == '\0') dir = WB_WBV_FIXTURE_DIR;
  snprintf(path, sizeof(path), "%s/spectator_seed.bin", dir);

  f = fopen(path, "rb");
  if (f == NULL) {
    UT_FAIL("fixture missing: %s — generate it with: "
            "WB_WBV_FIXTURE_DIR=%s ./WinBoloUnitTests --test spectator_seed_capture",
            path, dir);
  }
  fseek(f, 0, SEEK_END);
  sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  UT_ASSERT_MSG(sz > 8, "fixture too short: %ld bytes", sz);
  seedLen = (size_t) sz;
  seed = (uint8_t *) malloc(seedLen);
  UT_ASSERT_MSG(seed != NULL, "seed alloc failed");
  UT_ASSERT_MSG(fread(seed, 1, seedLen, f) == seedLen, "short read of %s", path);
  fclose(f);

  /* Mirror logViewerRun / stream_load: create + size the decoder before load. */
  lv = lv_decoderCreate(false);
  UT_ASSERT_MSG(lv != NULL, "lv_decoderCreate returned NULL");
  lv_screenSetSizeX(30);
  lv_screenSetSizeY(30);

  /* NULL info — the production spectatorRun path passes NULL (the live game-info
     handshake is a later slice). */
  loaded = lv_specSeedLoad(NULL, seed, seedLen);
  free(seed);
  UT_ASSERT_MSG(loaded == TRUE,
                "lv_specSeedLoad returned FALSE on a real ring seed "
                "(reproduces the lv_processSnapshot over-read)");

  lv_specSeedControlClear();
  lv_decoderDestroy(lv); /* closes the log (frees blocks + screen structures) */
  return 0;
}
