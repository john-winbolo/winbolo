/*
 * Stream-load decode gate (lv_screenLoadFromStream in src/logviewer/screen.c).
 *
 * The committed v2 fixture (spectator_v2.wbv) is a zip whose inner log.dat is
 * the plaintext v2 byte stream (header + opening snapshot + per-event records +
 * LOG_QUIT). The .wbv reader (run_wbv_reader_v2) decodes that same content
 * through the zip source; this test extracts log.dat and feeds the identical
 * bytes through the no-zip stream source instead, proving lv_screenLoadFromStream
 * decodes exactly what the zip path does.
 *
 * As with the reader gate, the load decodes only the header + opening snapshot;
 * the per-event records (including the slot-1 "Joiner" join) are applied during
 * playback, so the test steps lv_screenLogTick to end-of-log before asserting
 * the joined player.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lv_global.h"
#include "backend.h"
#include "logviewer.h"
#include "test_harness.h"

#ifndef WB_WBV_FIXTURE_DIR
#define WB_WBV_FIXTURE_DIR "tests/fixtures/wbv"
#endif

int run_stream_load(void) {
  const char *dir = getenv("WB_WBV_FIXTURE_DIR");
  char path[512];
  uint8_t *plaintext;
  size_t len;
  LogViewerState *lv;
  bool loaded;
  char mapName[64];
  char joinName[64];
  int steps;

  if (dir == NULL || dir[0] == '\0') dir = WB_WBV_FIXTURE_DIR;
  snprintf(path, sizeof(path), "%s/spectator_v2.wbv", dir);

  /* Unzip the inner log.dat to get the plaintext v2 byte stream. The zip
   * reader (run_wbv_reader_v2) loads the same bytes via lv_blocksCreateFromMemory;
   * here we drive them through the append-fed stream source instead. */
  plaintext = NULL;
  len = 0;
  if (!extractLogDat(path, &plaintext, &len)) {
    UT_FAIL("fixture missing or unreadable: %s — generate it with: "
            "WB_WBV_FIXTURE_DIR=%s ./WinBoloUnitTests --test wbv_v2_capture",
            path, dir);
  }

  /* Mirror logViewerRun's allocation + screen-size defaults so lv_screenSetup
   * sizes its view buffers. */
  lv = lv_decoderCreate(false);
  UT_ASSERT_MSG(lv != NULL, "lv_decoderCreate returned NULL");
  lv_screenSetSizeX(30);
  lv_screenSetSizeY(30);

  loaded = lv_screenLoadFromStream(plaintext, len);
  free(plaintext);   /* the stream source copies appended bytes into blocks */
  UT_ASSERT_MSG(loaded == TRUE, "lv_screenLoadFromStream returned FALSE");

  /* Header map name + the snapshot's running-game player decoded on load. */
  lv_screenGetMapName(mapName);
  UT_ASSERT_MSG(strcmp(mapName, "Everard Island") == 0,
                "map name = '%s' (want 'Everard Island')", mapName);
  UT_ASSERT_MSG(lv_screenGetNumPlayers() >= 1,
                "expected >= 1 decoded player, got %d",
                (int)lv_screenGetNumPlayers());

  /* Step playback to end-of-log so the per-event decode switch runs over the
   * stream-served records — same bounded-loop cap as the reader gate. */
  steps = 0;
  while (lv_screenLogTick() != TRUE && steps < 256) {
    steps++;
  }
  UT_ASSERT_MSG(steps < 256, "playback did not reach end-of-log within cap");

  /* The slot-1 join ("Joiner") decoded back through the stream path proves it
   * served the same events the zip path does. */
  lv_screenGetPlayerName(joinName, 1, sizeof(joinName));
  UT_ASSERT_MSG(strcmp(joinName, "Joiner") == 0,
                "slot-1 player name = '%s' (want 'Joiner')", joinName);
  UT_ASSERT_MSG(lv_screenGetNumPlayers() >= 2,
                "expected >= 2 players after join, got %d",
                (int)lv_screenGetNumPlayers());

  lv_decoderDestroy(lv);   /* closes the log (frees blocks + screen structures) */
  return 0;
}
