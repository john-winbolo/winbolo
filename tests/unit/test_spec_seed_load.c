/*
 * Spectator seed-load gate (lv_specSeedLoad in src/logviewer/spec_seed_load.c).
 *
 * lv_specSeedLoad takes a raw spectator-ring keyframe seed blob
 * ([u32 bodyLen BE][world body][u32 ctrlLen BE][control snapshot]), synthesizes
 * the v2 header, translates the keyframe and loads it into the decoder.
 *
 * A test that produced a *real* seed from the live sim would need the bolo log /
 * server_sim headers (-> global.h), which cannot share a TU with the decoder
 * headers. So this test fabricates a ring-shaped seed from the committed v2
 * fixture instead. The fixture's plaintext log.dat is
 * [header][LOG_SNAPSHOT][world body][events][LOG_QUIT]; the bytes after the
 * snapshot marker are exactly the world body a ring keyframe carries. Wrapping
 * them as [bodyLen][body][ctrlLen][synthetic control] yields a seed byte-shaped
 * like the ring's, and lv_specSeedLoad must rebuild the same opening-snapshot
 * world stream_load decodes — plus stash the control slice for the HUD.
 *
 * (The trailing event records ride along inside the body harmlessly: the load
 * decodes only the header + opening snapshot, exactly as stream_load does when
 * it feeds the whole plaintext in one call.)
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

static void packU32BE(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t) (v >> 24);
  p[1] = (uint8_t) (v >> 16);
  p[2] = (uint8_t) (v >> 8);
  p[3] = (uint8_t) v;
}

int run_spec_seed_load(void) {
  static const uint8_t kCtrl[] = { 0xA1, 0xA2, 0xA3, 0xA4 };
  const size_t ctrlLen = sizeof(kCtrl);
  const char *dir = getenv("WB_WBV_FIXTURE_DIR");
  char path[512];
  uint8_t *plaintext;
  size_t len;
  size_t headerLen;
  size_t bodyLen;
  const uint8_t *body;
  uint8_t *seed;
  size_t seedLen;
  LvSpecSeedInfo info;
  LogViewerState *lv;
  bool loaded;
  char mapName[64];
  const uint8_t *stashed;
  size_t stashedLen;

  if (dir == NULL || dir[0] == '\0') dir = WB_WBV_FIXTURE_DIR;
  snprintf(path, sizeof(path), "%s/spectator_v2.wbv", dir);

  plaintext = NULL;
  len = 0;
  if (!extractLogDat(path, &plaintext, &len)) {
    UT_FAIL("fixture missing or unreadable: %s — generate it with: "
            "WB_WBV_FIXTURE_DIR=%s ./WinBoloUnitTests --test wbv_v2_capture",
            path, dir);
  }

  /* Parse the fixture's v2 header length to isolate the snapshot body:
   * 8 id + 1 version + 1 map-length byte + mapLen + 8 game-info + 4 addr +
   * 2 port + 4 time + 32 key. The map-length byte sits at offset 9. */
  UT_ASSERT_MSG(len > 10, "fixture too short: %d bytes", (int) len);
  headerLen = 8 + 1 + 1 + (size_t) plaintext[9] + 8 + 4 + 2 + 4 + 32;
  UT_ASSERT_MSG(headerLen < len, "parsed header length %d >= file %d",
                (int) headerLen, (int) len);
  UT_ASSERT_MSG(plaintext[headerLen] == LOG_SNAPSHOT,
                "byte after header = %d (want LOG_SNAPSHOT %d)",
                (int) plaintext[headerLen], LOG_SNAPSHOT);

  /* World body = everything after the snapshot marker (trailing records ride
   * along; see the file header). */
  body = plaintext + headerLen + 1;
  bodyLen = len - headerLen - 1;

  /* Fabricate the ring-shaped seed: [bodyLen][body][ctrlLen][synthetic ctrl]. */
  seedLen = 4 + bodyLen + 4 + ctrlLen;
  seed = (uint8_t *) malloc(seedLen);
  UT_ASSERT_MSG(seed != NULL, "seed alloc failed");
  packU32BE(seed, (uint32_t) bodyLen);
  memcpy(seed + 4, body, bodyLen);
  packU32BE(seed + 4 + bodyLen, (uint32_t) ctrlLen);
  memcpy(seed + 4 + bodyLen + 4, kCtrl, ctrlLen);
  free(plaintext);

  /* Mirror logViewerRun / stream_load: create + size the decoder before load. */
  lv = lv_decoderCreate(false);
  UT_ASSERT_MSG(lv != NULL, "lv_decoderCreate returned NULL");
  lv_screenSetSizeX(30);
  lv_screenSetSizeY(30);

  memset(&info, 0, sizeof(info));
  info.mapName = "Seed Island";
  info.maxPlayers = 16;

  loaded = lv_specSeedLoad(&info, seed, seedLen);
  free(seed);
  UT_ASSERT_MSG(loaded == TRUE, "lv_specSeedLoad returned FALSE");

  /* The synthesized header drove the decode: the supplied map name is what the
   * decoder now reports, and the opening snapshot's running-game player loaded. */
  lv_screenGetMapName(mapName);
  UT_ASSERT_MSG(strcmp(mapName, "Seed Island") == 0,
                "map name = '%s' (want 'Seed Island')", mapName);
  UT_ASSERT_MSG(lv_screenGetNumPlayers() >= 1,
                "expected >= 1 decoded player, got %d",
                (int) lv_screenGetNumPlayers());

  /* The keyframe's control slice was stashed (not discarded) for the HUD. */
  stashed = lv_specSeedControl(&stashedLen);
  UT_ASSERT_MSG(stashed != NULL, "control slice not stashed");
  UT_ASSERT_MSG(stashedLen == ctrlLen,
                "stashed control len = %d (want %d)",
                (int) stashedLen, (int) ctrlLen);
  UT_ASSERT_MSG(memcmp(stashed, kCtrl, ctrlLen) == 0,
                "stashed control bytes mismatch");

  lv_specSeedControlClear();
  lv_decoderDestroy(lv); /* closes the log (frees blocks + screen structures) */
  return 0;
}
