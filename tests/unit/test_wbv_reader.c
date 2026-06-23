/*
 * Loads the committed v1 .wbv fixture through the production log-viewer
 * reader (lv_screenLoadMapFromMemory in src/logviewer/screen.c), then
 * steps playback to end-of-log so both decode paths are exercised. This is
 * the "v1 files still load" gate: the fixture is a genuine current-format
 * log produced by test_wbv_v1_capture.
 *
 * lv_screenLoadMapFromMemory decodes the header and the opening snapshot
 * only — per-event records are applied later, during playback. So the test
 * first pins the header map name and the snapshot's slot-0 player, then
 * drives lv_screenLogTick to end-of-log to run the per-event decode switch
 * (lv_screenProcessLog) and asserts the fixture's slot-1 join ("Joiner")
 * decoded through it.
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

int run_wbv_reader_v1(void) {
  const char *dir = getenv("WB_WBV_FIXTURE_DIR");
  char path[512];
  FILE *f;
  long sz;
  uint8_t *buf;
  size_t got;
  LogViewerState *lv;
  bool loaded;
  char mapName[64];
  char joinName[64];
  int steps;

  if (dir == NULL || dir[0] == '\0') dir = WB_WBV_FIXTURE_DIR;
  snprintf(path, sizeof(path), "%s/spectator_v1.wbv", dir);

  f = fopen(path, "rb");
  if (f == NULL) {
    UT_FAIL("fixture missing: %s — generate it with: "
            "WB_WBV_FIXTURE_DIR=%s ./WinBoloUnitTests --test wbv_v1_capture",
            path, dir);
  }
  fseek(f, 0, SEEK_END);
  sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0) {
    fclose(f);
    UT_FAIL("fixture empty: %s — regenerate with: "
            "WB_WBV_FIXTURE_DIR=%s ./WinBoloUnitTests --test wbv_v1_capture",
            path, dir);
  }

  buf = (uint8_t *)malloc((size_t)sz);
  UT_ASSERT_MSG(buf != NULL, "malloc %ld failed", sz);
  got = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  UT_ASSERT_MSG(got == (size_t)sz, "short read: %zu of %ld", got, sz);

  /* The reader operates on the file-scope LogViewerState injected via
   * lv_screenSetState — mirror logViewerRun's allocation and screen-size
   * defaults so lv_screenSetup sizes its view buffers. */
  lv = (LogViewerState *)calloc(1, sizeof(LogViewerState));
  UT_ASSERT_MSG(lv != NULL, "calloc LogViewerState failed");
  lv_screenSetState(lv);
  lv_screenSetSizeX(30);
  lv_screenSetSizeY(30);

  /* Takes ownership of buf (freed when the log is closed). */
  loaded = lv_screenLoadMapFromMemory(buf, (size_t)sz);
  UT_ASSERT_MSG(loaded == TRUE, "lv_screenLoadMapFromMemory returned FALSE");

  /* Decoded-content assertions: the header map name round-trips and the
   * snapshot's player section registered the running-game player. */
  lv_screenGetMapName(mapName);
  UT_ASSERT_MSG(strcmp(mapName, "Everard Island") == 0,
                "map name = '%s' (want 'Everard Island')", mapName);
  UT_ASSERT_MSG(lv_screenGetNumPlayers() >= 1,
                "expected >= 1 decoded player, got %d",
                (int)lv_screenGetNumPlayers());

  /* Step playback to end-of-log so the per-event decode switch
   * (lv_screenProcessLog) runs — the load only decodes the opening
   * snapshot. lv_screenLogTick returns TRUE at the LOG_QUIT terminator
   * (and on a snapshot); the fixture has no mid-stream snapshot, so the
   * first TRUE is end-of-log. The cap guards a malformed stream that
   * never terminates. */
  steps = 0;
  while (lv_screenLogTick() != TRUE && steps < 256) {
    steps++;
  }
  UT_ASSERT_MSG(steps < 256, "playback did not reach end-of-log within cap");

  /* The slot-1 join (name "Joiner") is a distinct event tick, separate
   * from the snapshot's slot-0 "Tester"; decoding it back proves the
   * variable-length player-join event ran through lv_screenProcessLog.
   * That decode marks the slot in use, so the player count also rises. */
  lv_screenGetPlayerName(joinName, 1);
  UT_ASSERT_MSG(strcmp(joinName, "Joiner") == 0,
                "slot-1 player name = '%s' (want 'Joiner')", joinName);
  UT_ASSERT_MSG(lv_screenGetNumPlayers() >= 2,
                "expected >= 2 players after join, got %d",
                (int)lv_screenGetNumPlayers());

  lv_screenCloseLog();   /* frees the zip buffer + screen structures */
  free(lv);
  lv_screenSetState(NULL);
  return 0;
}
