/*
 * Stream-pump gate (lv_screenStreamPump in src/logviewer/screen.c).
 *
 * A live spectator stream never terminates and arrives a record at a time. The
 * pump must append newly-arrived bytes and advance the decoder over only the
 * records that are fully buffered, parking cleanly when it catches up — never
 * reading past the buffer and never treating "caught up" as end-of-log.
 *
 * This test drives the committed v2 fixture's inner log.dat (the same bytes the
 * zip reader and stream-load gates use) through the pump one record at a time,
 * with a deliberate caught-up pump (no new bytes) partway through, and proves
 * the chunked feed reaches the same decoded state as a one-shot load to
 * end-of-log (slot-1 "Joiner", >= 2 players), and that pumping while caught up
 * neither advances the cursor nor finishes playback.
 *
 * Record boundaries are discovered with the decoder itself — a one-shot dry run
 * recording the read cursor after each reading tick — rather than re-parsing
 * the stream, so the chunked feed is record-aligned without duplicating decode
 * logic. (The cursor only moves on a record-reading tick; idle wait ticks leave
 * it put, so the distinct positions are exactly the record boundaries.)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lv_global.h"
#include "backend.h"
#include "blocks.h"
#include "logviewer.h"
#include "test_harness.h"

#ifndef WB_WBV_FIXTURE_DIR
#define WB_WBV_FIXTURE_DIR "tests/fixtures/wbv"
#endif

int run_stream_pump(void) {
  const char *dir = getenv("WB_WBV_FIXTURE_DIR");
  char path[512];
  uint8_t *full;
  size_t fullLen;
  LogViewerState *lv;
  size_t *bound;
  size_t nb;
  size_t prev;
  int steps;
  char osName[64];
  int osNum;
  char chName[64];
  int chNum;
  size_t i;

  if (dir == NULL || dir[0] == '\0') dir = WB_WBV_FIXTURE_DIR;
  snprintf(path, sizeof(path), "%s/spectator_v2.wbv", dir);

  full = NULL;
  fullLen = 0;
  if (!extractLogDat(path, &full, &fullLen)) {
    UT_FAIL("fixture missing or unreadable: %s — generate it with: "
            "WB_WBV_FIXTURE_DIR=%s ./WinBoloUnitTests --test wbv_v2_capture",
            path, dir);
  }
  UT_ASSERT_MSG(fullLen > 0, "extracted log.dat is empty");

  /* ---- One-shot reference + record-boundary discovery --------------------
   * Load the whole stream, then step to end-of-log recording the read cursor
   * after each reading tick. The final decoded state is the one-shot result the
   * chunked feed must match; the recorded positions are the record boundaries
   * the chunked feed appends on. */
  lv = lv_decoderCreate(false);
  UT_ASSERT_MSG(lv != NULL, "lv_decoderCreate returned NULL");
  lv_screenSetSizeX(30);
  lv_screenSetSizeY(30);

  UT_ASSERT_MSG(lv_screenLoadFromStream(full, fullLen) == TRUE,
                "one-shot lv_screenLoadFromStream returned FALSE");

  /* Each record is >= 1 byte, so there can be at most fullLen reading ticks;
   * fullLen + 2 entries covers the seed boundary plus every record end. */
  bound = (size_t *)malloc((fullLen + 2) * sizeof(size_t));
  UT_ASSERT_MSG(bound != NULL, "boundary buffer alloc failed");
  nb = 0;
  prev = lv_logGetCurrentPosition();   /* after header + opening snapshot */
  bound[nb++] = prev;

  steps = 0;
  while (lv_screenIsPlaying() == TRUE && steps < 100000) {
    size_t cur;
    lv_screenLogTick();
    cur = lv_logGetCurrentPosition();
    if (cur != prev) {
      bound[nb++] = cur;
      prev = cur;
    }
    steps++;
  }
  UT_ASSERT_MSG(steps < 100000, "one-shot playback did not reach end-of-log");
  UT_ASSERT_MSG(lv_screenIsPlaying() == FALSE, "one-shot did not finish");
  UT_ASSERT_MSG(nb >= 2, "expected at least one record after the snapshot");
  /* The fixture ends with a LOG_QUIT record, which finishes playback; the .wbv
   * carries a few bytes after it that are never played, so the last record
   * boundary sits at or before the buffer end. (A live stream carries no
   * LOG_QUIT and the pump would just keep parking; the fixture's QUIT lets the
   * test reach a stable final state to compare the chunked feed against.) The
   * chunked feed below appends only up to bound[nb-1], so the trailing bytes
   * are never fed. */
  UT_ASSERT_MSG(bound[nb - 1] <= fullLen, "playback ran past the buffer end");

  lv_screenGetPlayerName(osName, 1, sizeof(osName));
  osNum = (int)lv_screenGetNumPlayers();
  UT_ASSERT_MSG(strcmp(osName, "Joiner") == 0,
                "one-shot slot-1 name = '%s' (want 'Joiner')", osName);
  UT_ASSERT_MSG(osNum >= 2, "one-shot players = %d (want >= 2)", osNum);

  lv_decoderDestroy(lv);

  /* ---- Chunked feed through the pump -------------------------------------
   * Seed the decoder with just the header + opening snapshot (bound[0] bytes),
   * which leaves it caught up (cursor == buffer end). Then feed each following
   * record as its own append+pump, so every step appends record-aligned bytes
   * and the pump parks caught-up after consuming them. A no-new-bytes pump is
   * injected mid-stream to prove the caught-up path neither advances nor
   * finishes. Every record before the last is an event / no-events record
   * (never end-of-log), so the pump stays playing until the final record. */
  lv = lv_decoderCreate(false);
  UT_ASSERT_MSG(lv != NULL, "lv_decoderCreate (chunked) returned NULL");
  lv_screenSetSizeX(30);
  lv_screenSetSizeY(30);

  UT_ASSERT_MSG(lv_screenLoadFromStream(full, bound[0]) == TRUE,
                "chunked seed lv_screenLoadFromStream returned FALSE");
  UT_ASSERT_MSG(lv_logGetCurrentPosition() == lv_logGetTotalSize(),
                "seed left buffered bytes unread");

  for (i = 1; i < nb; i++) {
    bool playing;
    bool isLast = (i == nb - 1);

    /* Deliberate caught-up pump with no new bytes, partway through: it must
     * not advance the cursor or finish playback. */
    if (i == nb / 2) {
      bool p = lv_screenStreamPump(NULL, 0);
      UT_ASSERT_MSG(p == TRUE, "caught-up pump finished playback early");
      UT_ASSERT_MSG(lv_logGetCurrentPosition() == lv_logGetTotalSize(),
                    "caught-up pump advanced the cursor");
    }

    playing = lv_screenStreamPump(full + bound[i - 1], bound[i] - bound[i - 1]);

    /* After each record-aligned append the pump consumes exactly that record
     * (bursting any pending wait) and parks caught-up. */
    UT_ASSERT_MSG(lv_logGetCurrentPosition() == lv_logGetTotalSize(),
                  "pump left cursor short of buffer at record %u", (unsigned)i);
    if (!isLast) {
      UT_ASSERT_MSG(playing == TRUE,
                    "pump finished before the final record (at %u)",
                    (unsigned)i);
    }
  }

  UT_ASSERT_MSG(lv_screenIsPlaying() == FALSE,
                "stream did not finish after the final record");

  lv_screenGetPlayerName(chName, 1, sizeof(chName));
  chNum = (int)lv_screenGetNumPlayers();
  UT_ASSERT_MSG(strcmp(chName, osName) == 0,
                "chunked slot-1 name '%s' != one-shot '%s'", chName, osName);
  UT_ASSERT_MSG(chNum == osNum,
                "chunked players %d != one-shot %d", chNum, osNum);

  free(bound);
  free(full);
  lv_decoderDestroy(lv);
  return 0;
}
