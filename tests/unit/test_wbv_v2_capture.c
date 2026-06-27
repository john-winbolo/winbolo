/*
 * On-demand generator for the committed v2 .wbv reader fixture. Not part
 * of the fast suite: it drives the dedicated-server replay writer the
 * same way the server does (logStart -> logAddEvent -> logWriteTick ->
 * logStop) to emit a small but representative current-format log, and
 * writes it to <WB_WBV_FIXTURE_DIR>/spectator_v2.wbv for test_wbv_reader
 * to load back through the production log-viewer reader.
 *
 * Regenerate with, from the repo root:
 *
 *   WB_WBV_FIXTURE_DIR=tests/fixtures/wbv \
 *       ./WinBoloUnitTests --test wbv_v2_capture
 *
 * then commit the resulting tests/fixtures/wbv/spectator_v2.wbv.
 *
 * Every test-controlled byte (the event opcodes and their fixed payloads
 * below) is a constant. The log header also carries the server creation
 * time and WBN key, which are runtime-derived in any genuine .wbv; the v2
 * stream is plaintext so they no longer seed an XOR key, and they are read
 * straight back out of the header, so the fixture loads regardless of
 * their value.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "log.h"
#include "server_sim.h"
#include "test_harness.h"

#ifndef WB_WBV_FIXTURE_DIR
#define WB_WBV_FIXTURE_DIR "tests/fixtures/wbv"
#endif

int run_wbv_v2_capture(void) {
  const char *dir = getenv("WB_WBV_FIXTURE_DIR");
  char path[512];
  char namePstr[64];
  const char *joinerName = "Joiner";
  size_t nameLen = strlen(joinerName);
  ServerSim *sim;
  FILE *f;
  long sz;

  if (dir == NULL || dir[0] == '\0') dir = WB_WBV_FIXTURE_DIR;
  snprintf(path, sizeof(path), "%s/spectator_v2.wbv", dir);
  remove(path);

  sim = ut_make_running_sim("Tester");
  UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim failed");

  logCreate();
  UT_ASSERT_MSG(logStart(path, sim, /*ai*/0, /*maxPlayers*/MAX_TANKS,
                         /*usePassword*/FALSE),
                "logStart failed for %s", path);
  /* First tick pins the log owner thread to this thread (matches
   * production — the owner check is otherwise a no-op while owner is 0). */
  logWriteTick();

  /* Variable-length event: a player join carries a pascal-string name.
   * v1/v2 layout: opt2/opt3 are the 2-char country code, opt4 is
   * accountFlags, opt5 (short1) is reserved. */
  namePstr[0] = (char)nameLen;
  memcpy(namePstr + 1, joinerName, nameLen);
  logAddEvent(log_PlayerJoined, /*slot*/1, /*'G'*/'G', /*'B'*/'B',
              /*accountFlags*/0, /*reserved*/0, namePstr);
  /* Fixed-length events: a tank location (5 bytes) and a shoot sound
   * (2 bytes). */
  logAddEvent(log_PlayerLocation, /*slot*/1, /*mx*/10, /*my*/12,
              /*pxy*/0x44, /*dir*/8, NULL);
  logAddEvent(log_SoundShoot, /*mx*/10, /*my*/12, 0, 0, 0, NULL);
  logWriteTick();

  logStop();   /* writes the LOG_QUIT terminator and closes the zip */
  logDestroy();
  serverSimDestroy(sim);

  f = fopen(path, "rb");
  UT_ASSERT_MSG(f != NULL, "fixture not written: %s", path);
  fseek(f, 0, SEEK_END);
  sz = ftell(f);
  fclose(f);
  UT_ASSERT_MSG(sz > 0, "fixture is empty: %s", path);

  fprintf(stderr, "wbv_v2_capture: wrote %ld bytes -> %s\n", sz, path);
  return 0;
}
