/*
 * Spectator-frame snapshot gate (specFrameFill in src/logviewer/spec_frame_fill.c).
 *
 * Drives the same v2 fixture as the stream-load gate: extract the inner
 * log.dat, feed it through lv_screenLoadFromStream, then step lv_screenLogTick
 * to end-of-log so the slot-1 "Joiner" join is applied. With the decoder at
 * that state, fill a SpecFramePOD and assert the snapshot mirrors the two
 * seeded tanks (present, names, slot) and exposes a defined followed slot —
 * proving the POD seam carries the decoder's reconstructed world without any
 * bolo/lv headers leaking into the consuming struct.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lv_global.h"
#include "backend.h"
#include "logviewer.h"
#include "spec_frame.h"
#include "test_harness.h"

#ifndef WB_WBV_FIXTURE_DIR
#define WB_WBV_FIXTURE_DIR "tests/fixtures/wbv"
#endif

int run_spec_frame_fill(void) {
  const char *dir = getenv("WB_WBV_FIXTURE_DIR");
  char path[512];
  uint8_t *plaintext;
  size_t len;
  LogViewerState *lv;
  bool loaded;
  int steps;
  SpecFramePOD frame;

  if (dir == NULL || dir[0] == '\0') dir = WB_WBV_FIXTURE_DIR;
  snprintf(path, sizeof(path), "%s/spectator_v2.wbv", dir);

  plaintext = NULL;
  len = 0;
  if (!extractLogDat(path, &plaintext, &len)) {
    UT_FAIL("fixture missing or unreadable: %s — generate it with: "
            "WB_WBV_FIXTURE_DIR=%s ./WinBoloUnitTests --test wbv_v2_capture",
            path, dir);
  }

  lv = lv_decoderCreate(false);
  UT_ASSERT_MSG(lv != NULL, "lv_decoderCreate returned NULL");
  lv_screenSetSizeX(30);
  lv_screenSetSizeY(30);

  loaded = lv_screenLoadFromStream(plaintext, len);
  free(plaintext);
  UT_ASSERT_MSG(loaded == TRUE, "lv_screenLoadFromStream returned FALSE");

  /* Step playback to end-of-log so the slot-1 join is applied before the
   * snapshot — same bounded-loop cap as the stream-load gate. */
  steps = 0;
  while (lv_screenLogTick() != TRUE && steps < 256) {
    steps++;
  }
  UT_ASSERT_MSG(steps < 256, "playback did not reach end-of-log within cap");

  /* Snapshot the reconstructed world. */
  specFrameFill(&frame);

  /* The snapshot's seeded slot-0 "Tester" and slot-1 "Joiner" prove
   * specFrameFill copied the decoder's slot-indexed player state. */
  UT_ASSERT_MSG(frame.tanks[0].present == 1, "tanks[0] not present");
  UT_ASSERT_MSG(frame.tanks[1].present == 1, "tanks[1] not present");
  UT_ASSERT_MSG(strcmp(frame.tanks[0].name, "Tester") == 0,
                "tanks[0].name = '%s' (want 'Tester')", frame.tanks[0].name);
  UT_ASSERT_MSG(strcmp(frame.tanks[1].name, "Joiner") == 0,
                "tanks[1].name = '%s' (want 'Joiner')", frame.tanks[1].name);
  UT_ASSERT_MSG(frame.tanks[1].slot == 1,
                "tanks[1].slot = %d (want 1)", (int)frame.tanks[1].slot);

  /* The camera follows a real slot; the fixture's default is in range. */
  UT_ASSERT_MSG(frame.followedSlot < SPEC_MAX_TANKS,
                "followedSlot = %d out of range", (int)frame.followedSlot);

  lv_decoderDestroy(lv);
  return 0;
}
