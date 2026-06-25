/*
 * Spectator forward-record pump gate (lv_specRecordPump in
 * src/logviewer/spec_seed_load.c).
 *
 * After a spectator seeds its decoder from the ring keyframe (lv_specSeedLoad),
 * every following ring record is fed through lv_specRecordPump: a keyframe
 * record re-syncs the decoder (a fresh LOG_SNAPSHOT) and refreshes the stashed
 * control slice; an event-tick record (or an empty idle tick) advances the
 * delayed view. lv_specRecordPump translates the raw record per its kind, then
 * pumps the result through lv_screenStreamPump.
 *
 * A test producing *real* ring records from the live sim would need the bolo log
 * / server_sim headers (-> global.h), which cannot share a TU with the decoder
 * headers. So this test derives ring-shaped records from the committed v2
 * fixture instead. The fixture's plaintext log.dat is
 * [header][LOG_SNAPSHOT][world body][event records...][LOG_QUIT]. The bytes
 * after the opening snapshot are exactly the world body a ring keyframe carries;
 * each LOG_EVENT/LOG_EVENT_LONG/LOG_NOEVENTS record's payload (marker + count
 * stripped) is exactly the [type][u16 len][body] event framing a ring event-tick
 * record carries — which lv_specRecordPump re-frames. So:
 *   - seed the decoder from a ring-shaped seed built from the opening snapshot,
 *   - replay each fixture event record as a ring event-tick record,
 * and the decoder must reach the same end state a one-shot load to end-of-log
 * does (slot-1 "Joiner"). The LOG_QUIT terminator has no ring analogue (a live
 * feed never marks EOF) and is skipped.
 *
 * Three things are asserted directly: the event-record replay advances the
 * decoder to the one-shot end state; an empty tick (payloadLen 0) advances
 * cleanly without disturbing the roster; and a mid-stream keyframe re-syncs the
 * decoder back to the opening snapshot and refreshes the stashed control slice.
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

/* Build a ring-shaped keyframe blob [u32 bodyLen][world body][u32 ctrlLen][ctrl]
 * from a world-body slice and a control slice. Returns the heap blob (caller
 * frees) or NULL on alloc failure; sets *outLen to its length. */
static uint8_t *buildKeyframe(const uint8_t *body, size_t bodyLen,
                              const uint8_t *ctrl, size_t ctrlLen,
                              size_t *outLen) {
  size_t n = 4 + bodyLen + 4 + ctrlLen;
  uint8_t *p = (uint8_t *) malloc(n);
  if (p == NULL) {
    return NULL;
  }
  packU32BE(p, (uint32_t) bodyLen);
  memcpy(p + 4, body, bodyLen);
  packU32BE(p + 4 + bodyLen, (uint32_t) ctrlLen);
  if (ctrlLen > 0) memcpy(p + 4 + bodyLen + 4, ctrl, ctrlLen);
  *outLen = n;
  return p;
}

int run_spec_record_pump(void) {
  static const uint8_t kCtrl1[] = { 0xB1, 0xB2 };
  static const uint8_t kCtrl2[] = { 0xC1, 0xC2, 0xC3, 0xC4, 0xC5 };
  const char *dir = getenv("WB_WBV_FIXTURE_DIR");
  char path[512];
  uint8_t *full;
  size_t fullLen;
  size_t headerLen;
  const uint8_t *snapBody;
  size_t snapBodyLen;
  LogViewerState *lv;
  size_t *bound;
  size_t nb;
  size_t prev;
  size_t snapEnd;
  int steps;
  char oneShotName[64];
  int oneShotNum;
  uint8_t *seed;
  size_t seedLen;
  LvSpecSeedInfo info;
  const uint8_t *stash;
  size_t stashLen;
  int openNum;
  char name[64];
  uint8_t *kf;
  size_t kfLen;
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
  UT_ASSERT_MSG(fullLen > 10, "fixture too short: %d bytes", (int) fullLen);

  /* Header length (mirrors test_spec_seed_load): 8 id + 1 version + 1 map-length
   * byte + mapLen + 8 game-info + 4 addr + 2 port + 4 time + 32 key. */
  headerLen = 8 + 1 + 1 + (size_t) full[9] + 8 + 4 + 2 + 4 + 32;
  UT_ASSERT_MSG(headerLen < fullLen, "parsed header length %d >= file %d",
                (int) headerLen, (int) fullLen);
  UT_ASSERT_MSG(full[headerLen] == LOG_SNAPSHOT,
                "byte after header = %d (want LOG_SNAPSHOT %d)",
                (int) full[headerLen], LOG_SNAPSHOT);

  /* ---- One-shot reference + record-boundary discovery (as test_stream_pump):
   * load the whole stream, then step to end-of-log recording the read cursor
   * after each reading tick. bound[0] is the end of the opening snapshot (where
   * events begin); the later entries are the event-record boundaries. */
  lv = lv_decoderCreate(false);
  UT_ASSERT_MSG(lv != NULL, "lv_decoderCreate (dry run) returned NULL");
  lv_screenSetSizeX(30);
  lv_screenSetSizeY(30);
  UT_ASSERT_MSG(lv_screenLoadFromStream(full, fullLen) == TRUE,
                "one-shot lv_screenLoadFromStream returned FALSE");

  bound = (size_t *) malloc((fullLen + 2) * sizeof(size_t));
  UT_ASSERT_MSG(bound != NULL, "boundary buffer alloc failed");
  nb = 0;
  prev = lv_logGetCurrentPosition();   /* after header + opening snapshot */
  bound[nb++] = prev;
  snapEnd = prev;

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
  UT_ASSERT_MSG(nb >= 2, "expected at least one record after the snapshot");

  lv_screenGetPlayerName(oneShotName, 1);
  oneShotNum = (int) lv_screenGetNumPlayers();
  UT_ASSERT_MSG(strcmp(oneShotName, "Joiner") == 0,
                "one-shot slot-1 name = '%s' (want 'Joiner')", oneShotName);
  UT_ASSERT_MSG(oneShotNum >= 2, "one-shot players = %d (want >= 2)", oneShotNum);
  lv_decoderDestroy(lv);

  /* Opening snapshot world body = bytes between the snapshot marker and the
   * first event record. */
  snapBody = full + headerLen + 1;
  snapBodyLen = snapEnd - (headerLen + 1);

  /* ---- Seed the decoder from a ring-shaped opening keyframe (the 4.3 path),
   * carrying kCtrl1 so the keyframe re-sync below can prove the slice changes. */
  seed = buildKeyframe(snapBody, snapBodyLen, kCtrl1, sizeof(kCtrl1), &seedLen);
  UT_ASSERT_MSG(seed != NULL, "seed alloc failed");

  lv = lv_decoderCreate(false);
  UT_ASSERT_MSG(lv != NULL, "lv_decoderCreate (feed) returned NULL");
  lv_screenSetSizeX(30);
  lv_screenSetSizeY(30);

  memset(&info, 0, sizeof(info));
  info.mapName = "Seed Island";
  info.maxPlayers = 16;
  UT_ASSERT_MSG(lv_specSeedLoad(&info, seed, seedLen) == TRUE,
                "lv_specSeedLoad returned FALSE");
  free(seed);
  UT_ASSERT_MSG(lv_screenIsPlaying() == TRUE, "seed left the decoder not playing");
  UT_ASSERT_MSG(lv_logGetCurrentPosition() == lv_logGetTotalSize(),
                "seed left buffered bytes unread");

  stash = lv_specSeedControl(&stashLen);
  UT_ASSERT_MSG(stash != NULL && stashLen == sizeof(kCtrl1) &&
                    memcmp(stash, kCtrl1, sizeof(kCtrl1)) == 0,
                "seed control slice not stashed");
  openNum = (int) lv_screenGetNumPlayers();

  /* Empty idle tick: payloadLen 0 -> LOG_NOEVENTS. Advances and parks without
   * disturbing the roster. */
  UT_ASSERT_MSG(lv_specRecordPump(FALSE, NULL, 0) == TRUE,
                "empty-tick pump finished playback");
  UT_ASSERT_MSG(lv_logGetCurrentPosition() == lv_logGetTotalSize(),
                "empty-tick pump left the cursor short of the buffer");
  UT_ASSERT_MSG((int) lv_screenGetNumPlayers() == openNum,
                "empty-tick pump changed the roster (%d -> %d)", openNum,
                (int) lv_screenGetNumPlayers());

  /* ---- Replay each fixture event record as a ring event-tick record. Strip
   * the LOG_* marker (+ count) back to the raw [type][u16 len][body] framing the
   * ring carries; lv_specRecordPump re-frames it. The trailing LOG_QUIT has no
   * ring analogue (a live feed never marks EOF) — skip it. */
  for (i = 1; i < nb; i++) {
    const uint8_t *rec = full + bound[i - 1];
    size_t recLen = bound[i] - bound[i - 1];
    uint8_t marker = rec[0];
    bool played;

    if (marker == LOG_QUIT) {
      break; /* terminator; not a ring record */
    }

    if (marker == LOG_NOEVENTS || marker == LOG_NOEVENTS_LONG) {
      /* Idle tick -> empty ring payload (the wait count is a decoder-paced
       * detail; one empty record per idle tick reaches the same end state). */
      played = lv_specRecordPump(FALSE, NULL, 0);
    } else if (marker == LOG_EVENT) {
      /* [LOG_EVENT][count][events]: payload is the events verbatim. */
      played = lv_specRecordPump(FALSE, rec + 2, recLen - 2);
    } else if (marker == LOG_EVENT_LONG) {
      /* [LOG_EVENT_LONG][u16 count][events]. */
      played = lv_specRecordPump(FALSE, rec + 3, recLen - 3);
    } else if (marker == LOG_SNAPSHOT) {
      /* Mid-stream keyframe -> ring keyframe record (no control slice here). */
      uint8_t *midKf;
      size_t midKfLen;
      midKf = buildKeyframe(rec + 1, recLen - 1, NULL, 0, &midKfLen);
      UT_ASSERT_MSG(midKf != NULL, "mid-stream keyframe alloc failed");
      played = lv_specRecordPump(TRUE, midKf, midKfLen);
      free(midKf);
    } else {
      UT_FAIL("unexpected record marker %d at boundary %u", (int) marker,
              (unsigned) i);
      played = FALSE; /* unreachable; UT_FAIL returns */
    }

    UT_ASSERT_MSG(played == TRUE, "forward pump finished early at record %u",
                  (unsigned) i);
    UT_ASSERT_MSG(lv_logGetCurrentPosition() == lv_logGetTotalSize(),
                  "forward pump left the cursor short at record %u", (unsigned) i);
  }

  /* The event-record replay reached the one-shot end state: the "Joiner" join
   * applied and the roster matches. */
  lv_screenGetPlayerName(name, 1);
  UT_ASSERT_MSG(strcmp(name, "Joiner") == 0,
                "after replay slot-1 = '%s' (want 'Joiner')", name);
  UT_ASSERT_MSG((int) lv_screenGetNumPlayers() == oneShotNum,
                "after replay players = %d (want %d)",
                (int) lv_screenGetNumPlayers(), oneShotNum);

  /* ---- Mid-stream keyframe re-sync: feed the opening world body again as a
   * keyframe record carrying a fresh control slice. The decoder re-syncs (its
   * roster reverts to the opening snapshot's) and the stashed control slice is
   * refreshed to kCtrl2. */
  kf = buildKeyframe(snapBody, snapBodyLen, kCtrl2, sizeof(kCtrl2), &kfLen);
  UT_ASSERT_MSG(kf != NULL, "re-sync keyframe alloc failed");
  UT_ASSERT_MSG(lv_specRecordPump(TRUE, kf, kfLen) == TRUE,
                "keyframe re-sync finished playback");
  free(kf);
  UT_ASSERT_MSG(lv_logGetCurrentPosition() == lv_logGetTotalSize(),
                "keyframe re-sync left the cursor short of the buffer");
  UT_ASSERT_MSG((int) lv_screenGetNumPlayers() == openNum,
                "keyframe did not re-sync the roster to the opening snapshot "
                "(%d, want %d)", (int) lv_screenGetNumPlayers(), openNum);

  stash = lv_specSeedControl(&stashLen);
  UT_ASSERT_MSG(stash != NULL && stashLen == sizeof(kCtrl2) &&
                    memcmp(stash, kCtrl2, sizeof(kCtrl2)) == 0,
                "keyframe did not refresh the stashed control slice");

  lv_specSeedControlClear();
  free(bound);
  free(full);
  lv_decoderDestroy(lv);
  return 0;
}
