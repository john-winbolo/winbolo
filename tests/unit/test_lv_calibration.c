/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*
 * Calibration anchor walk (lv_walkFindBaseOwnerTimes): the highlight
 * clip-time fit pairs the attribution track's first/last base captures
 * with their real base-ownership changes in the replay stream. Base
 * index -> cell resolution must come from the log's OWN snapshots: a
 * lobby-started log's opening snapshot has an empty base table (the game
 * world doesn't exist yet), so the table only appears in the first
 * in-game snapshot. Resolving through the load-time g_lv->bs instead
 * made the walker miss every anchor on every lobby-started log and the
 * clip times silently fell back to the uncalibrated mapping.
 *
 * Each scenario builds a synthetic v2 log.dat byte-for-byte, wraps it in
 * a .wbv zip with the same minizip writer the server uses, loads it
 * through the production reader (lv_screenLoadMapFromMemory), and
 * asserts the walker returns the exact playback times of the two
 * ownership changes.
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
#include "zip.h"

/* ---- synthetic v2 log builder ------------------------------------------- */

typedef struct {
  uint8_t buf[4096];
  size_t len;
} LogBuf;

static void put(LogBuf *b, const void *bytes, size_t n) {
  memcpy(b->buf + b->len, bytes, n);
  b->len += n;
}

static void putU8(LogBuf *b, uint8_t v) { put(b, &v, 1); }

static void putZeros(LogBuf *b, size_t n) {
  memset(b->buf + b->len, 0, n);
  b->len += n;
}

/* File header, as lv_logLoad reads it: magic, version, map name, seven
 * settings bytes, ip4 + port, create time, WBN key. */
static void putHeader(LogBuf *b) {
  put(b, "WBOLOMOV", 8);
  putU8(b, LOG_VERSION_V2);
  putU8(b, 4);
  put(b, "Test", 4);
  putU8(b, 1);            /* game type */
  putU8(b, 0);            /* hidden mines */
  putU8(b, 0);            /* ai */
  putU8(b, 0);            /* password */
  putU8(b, MAX_TANKS);    /* max players */
  putU8(b, 2); putU8(b, 0); putU8(b, 2);   /* game version */
  putZeros(b, 4 + 2);     /* ip + port */
  putZeros(b, 4);         /* create time (v2: no XOR seed) */
  putZeros(b, 32);        /* WBN key */
}

/* One LOG_SNAPSHOT frame. withWorld=false is the lobby form the recorder
 * writes while the game world doesn't exist yet: no bases and no map runs.
 * withWorld=true is a running-round snapshot — two bases at (10,20) and
 * (30,40) in the 10-byte-per-base blob lv_basesSetBaseNetData consumes, plus
 * one real map run. The run is what marks a snapshot as carrying a world:
 * a lobby body writes only the terminator, a running one always writes at
 * least one run. */
static void putSnapshot(LogBuf *b, bool withWorld) {
  static const uint8_t baseBlob[1 + 2 * 10] = {
      2,
      10, 20, 255, 90, 10, 10, 0, 0, 0, 0,
      30, 40, 255, 90, 10, 10, 0, 0, 0, 0};
  /* datalen (header + data), y, startx, endx, then the nibble RLE
   * lv_mapProcessRun reads: a high nibble of 8 is >= MAP_RUN_DIFF, so it is a
   * "same" run of 8 - MAP_RUN_SAME = 2 cells of the low nibble's terrain,
   * landing on endx 2. */
  static const uint8_t mapRun[5] = {5, 0, 0, 2, (8 << 4) | GRASS};
  static const uint8_t mapTerminator[4] = {4, 255, 255, 255};
  int i;

  putU8(b, LOG_SNAPSHOT);
  putZeros(b, 8);                    /* gmeStartDelay + gmeLength */
  putU8(b, 1); putU8(b, 0);          /* pills: count 0 */
  if (withWorld) {
    putU8(b, (uint8_t)sizeof(baseBlob));
    put(b, baseBlob, sizeof(baseBlob));
  } else {
    putU8(b, 1); putU8(b, 0);        /* bases: count 0 */
  }
  putU8(b, 1); putU8(b, 0);          /* starts: count 0 */
  if (withWorld) {
    put(b, mapRun, sizeof(mapRun));
  }
  put(b, mapTerminator, 4);
  for (i = 0; i < MAX_TANKS; i++) {  /* every slot "not in use" */
    putU8(b, 2); putU8(b, 0); putU8(b, 0);
  }
}

/* One LOG_EVENT frame holding a single [type][u16 BE len][payload] event. */
static void putEventFrame(LogBuf *b, uint8_t evType, const uint8_t *payload,
                          uint16_t payloadLen) {
  putU8(b, LOG_EVENT);
  putU8(b, 1);
  putU8(b, evType);
  putU8(b, (uint8_t)(payloadLen >> 8));
  putU8(b, (uint8_t)(payloadLen & 0xFF));
  if (payloadLen > 0) put(b, payload, payloadLen);
}

static void putBaseSetOwner(LogBuf *b, uint8_t baseIdx, uint8_t owner) {
  uint8_t payload[2];
  payload[0] = baseIdx;
  payload[1] = owner;
  putEventFrame(b, log_BaseSetOwner, payload, 2);
}

static void putNoEvents(LogBuf *b, uint8_t waitLen) {
  putU8(b, LOG_NOEVENTS);
  putU8(b, waitLen);
}

/* Wrap log.dat in a .wbv zip on disk, read it back into a heap buffer
 * (lv_screenLoadMapFromMemory takes ownership). */
static uint8_t *zipToMemory(const LogBuf *b, const char *path, size_t *outLen) {
  zipFile zf;
  zip_fileinfo zi;
  FILE *f;
  long sz;
  uint8_t *out;

  remove(path);
  memset(&zi, 0, sizeof(zi));
  zf = zipOpen(path, 0);
  if (zf == NULL) return NULL;
  if (zipOpenNewFileInZip(zf, "log.dat", &zi, NULL, 0, NULL, 0, "",
                          Z_DEFLATED, Z_DEFAULT_COMPRESSION) != ZIP_OK ||
      zipWriteInFileInZip(zf, b->buf, (unsigned)b->len) != ZIP_OK ||
      zipCloseFileInZip(zf) != ZIP_OK) {
    zipClose(zf, NULL);
    return NULL;
  }
  if (zipClose(zf, NULL) != ZIP_OK) return NULL;

  f = fopen(path, "rb");
  if (f == NULL) return NULL;
  fseek(f, 0, SEEK_END);
  sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  out = (uint8_t *)malloc((size_t)sz);
  if (out == NULL || fread(out, 1, (size_t)sz, f) != (size_t)sz) {
    fclose(f);
    free(out);
    return NULL;
  }
  fclose(f);
  *outLen = (size_t)sz;
  return out;
}

static LogViewerState *loadSynthetic(const LogBuf *b, const char *path) {
  size_t zipLen = 0;
  uint8_t *zipData = zipToMemory(b, path, &zipLen);
  LogViewerState *lv;
  if (zipData == NULL) return NULL;
  lv = lv_decoderCreate(false);
  if (lv == NULL) {
    free(zipData);
    return NULL;
  }
  lv_screenSetSizeX(30);
  lv_screenSetSizeY(30);
  if (lv_screenLoadMapFromMemory(zipData, zipLen) != TRUE) {
    lv_decoderDestroy(lv);
    return NULL;
  }
  return lv;
}

/*
 * Lobby-started log: opening snapshot has NO bases; the base table appears
 * only in the mid-stream world rewrite that ends the lobby. Playback tick
 * accounting (per the walker: snapshot/event frames +1, NOEVENTS 1+wait):
 *
 *   snapshot (with world) -> tick   1  (gameStartMs 20)
 *   NOEVENTS 101          -> tick 103
 *   BaseSetOwner idx0     -> tick 104  (ms 2080)
 *   NOEVENTS 50           -> tick 155
 *   BaseSetOwner idx1     -> tick 156  (ms 3120)
 */
int run_lv_walk_base_anchor_lobby_log(void) {
  const char *path = "lv_calib_synth_lobby.wbv";
  LogBuf b;
  LogViewerState *lv;
  uint32_t msE = 0, msL = 0;
  bool found;

  b.len = 0;
  putHeader(&b);
  putSnapshot(&b, false);   /* lobby: empty world, empty base table */
  putSnapshot(&b, true);    /* the world rewrite carries the bases */
  putNoEvents(&b, 101);
  putBaseSetOwner(&b, 0, 2);
  putNoEvents(&b, 50);
  putBaseSetOwner(&b, 1, 3);
  putU8(&b, LOG_QUIT);

  lv = loadSynthetic(&b, path);
  UT_ASSERT_MSG(lv != NULL, "synthetic lobby log failed to build/load");

  UT_ASSERT_MSG(lv_screenGameStartMs() == 20,
                "gameStartMs = %u (want 20)", lv_screenGameStartMs());

  /* The load-time base table is the empty lobby snapshot — the walker must
   * pick the cells up from the mid-stream world rewrite instead. */
  found = lv_walkFindBaseOwnerTimes(10, 20, 2, 1, 30, 40, 3, 1, &msE, &msL);
  UT_ASSERT_MSG(found == TRUE, "anchor walk found nothing on a lobby log");
  UT_ASSERT_MSG(msE == 2080, "first anchor ms = %u (want 2080)", msE);
  UT_ASSERT_MSG(msL == 3120, "last anchor ms = %u (want 3120)", msL);

  /* A cell no base occupies must not resolve. */
  found = lv_walkFindBaseOwnerTimes(99, 99, 2, 1, 30, 40, 3, 1, &msE, &msL);
  UT_ASSERT_MSG(found == FALSE, "walk matched a nonexistent base cell");

  lv_decoderDestroy(lv);
  remove(path);
  return 0;
}

/*
 * No-lobby log: the ONLY base table is the opening snapshot, consumed by the
 * loader before the event-stream start. The walker must seed its table from
 * the loaded g_lv->bs. Ticks: BaseSetOwner idx0 -> 1 (ms 20), NOEVENTS 50
 * -> 52 (a wait frame counts 1 + waitLen), BaseSetOwner idx1 -> 53 (ms 1060).
 */
int run_lv_walk_base_anchor_opening_snapshot(void) {
  const char *path = "lv_calib_synth_nolobby.wbv";
  LogBuf b;
  LogViewerState *lv;
  uint32_t msE = 0, msL = 0;
  bool found;

  b.len = 0;
  putHeader(&b);
  putSnapshot(&b, true);    /* opening snapshot carries the bases */
  putBaseSetOwner(&b, 0, 2);
  putNoEvents(&b, 50);
  putBaseSetOwner(&b, 1, 3);
  putU8(&b, LOG_QUIT);

  lv = loadSynthetic(&b, path);
  UT_ASSERT_MSG(lv != NULL, "synthetic no-lobby log failed to build/load");

  found = lv_walkFindBaseOwnerTimes(10, 20, 2, 1, 30, 40, 3, 1, &msE, &msL);
  UT_ASSERT_MSG(found == TRUE, "anchor walk found nothing on a no-lobby log");
  UT_ASSERT_MSG(msE == 20, "first anchor ms = %u (want 20)", msE);
  UT_ASSERT_MSG(msL == 1060, "last anchor ms = %u (want 1060)", msL);

  lv_decoderDestroy(lv);
  remove(path);
  return 0;
}

/*
 * Ordinal matching: at game over every base is handed to the winner, and a
 * base can also be recaptured by the same player later in a round — either
 * way the log carries a LATER ownership gain at the anchor's cell than the
 * capture the attribution track recorded, and matching "the last gain at
 * this cell" stretches the fitted slope (observed +10%, pushing clip times
 * past the end of the log). The k-th (cell, owner) gain is the exact event.
 * Stream: idx0/owner2 (ms 20), idx1/owner3 (ms 1060), idx1/owner3 again
 * (ms 1500 — the handover/recapture).
 */
int run_lv_walk_base_anchor_ordinal(void) {
  const char *path = "lv_calib_synth_ordinal.wbv";
  LogBuf b;
  LogViewerState *lv;
  uint32_t msE = 0, msL = 0;
  bool found;

  b.len = 0;
  putHeader(&b);
  putSnapshot(&b, true);
  putBaseSetOwner(&b, 0, 2);   /* tick 1  -> ms 20 */
  putNoEvents(&b, 50);         /* ticks 2..52 */
  putBaseSetOwner(&b, 1, 3);   /* tick 53 -> ms 1060: the tracked capture */
  putNoEvents(&b, 20);         /* ticks 54..74 */
  putBaseSetOwner(&b, 1, 3);   /* tick 75 -> ms 1500: handover, no record */
  putU8(&b, LOG_QUIT);

  lv = loadSynthetic(&b, path);
  UT_ASSERT_MSG(lv != NULL, "synthetic ordinal log failed to build/load");

  /* Ordinal 1 must latch the tracked capture, not the later gain. */
  found = lv_walkFindBaseOwnerTimes(10, 20, 2, 1, 30, 40, 3, 1, &msE, &msL);
  UT_ASSERT_MSG(found == TRUE, "ordinal-1 walk found nothing");
  UT_ASSERT_MSG(msL == 1060, "ordinal-1 anchor ms = %u (want 1060)", msL);

  /* Ordinal 2 reaches the later gain. */
  found = lv_walkFindBaseOwnerTimes(10, 20, 2, 1, 30, 40, 3, 2, &msE, &msL);
  UT_ASSERT_MSG(found == TRUE, "ordinal-2 walk found nothing");
  UT_ASSERT_MSG(msL == 1500, "ordinal-2 anchor ms = %u (want 1500)", msL);

  /* A different owner at the anchor cell never matches. */
  found = lv_walkFindBaseOwnerTimes(10, 20, 2, 1, 30, 40, 4, 1, &msE, &msL);
  UT_ASSERT_MSG(found == FALSE, "walk matched the wrong owner");

  lv_decoderDestroy(lv);
  remove(path);
  return 0;
}

/*
 * Presentation window (Hide Lobby): a two-minute lobby in front of a
 * one-minute game, so the minutes field is non-zero on both sides of the
 * window. Playback ticks, per the walker (a snapshot or event frame counts
 * +1, NOEVENTS n counts 1 + n, 20 ms a tick; the opening snapshot is consumed
 * at load and counts 0):
 *
 *   23 x NOEVENTS 249  -> tick 5750
 *   NOEVENTS 248       -> tick 5999
 *   LobbyExit frame    -> tick 6000
 *   snapshot           -> tick 6001  (gameStartMs 120020)
 *   12 x NOEVENTS 249  -> tick 9001
 *   NOEVENTS 100       -> tick 9102
 *   LOG_QUIT           -> tick 9103  (totalTimeMs 182060)
 *
 * The marker and the world rewrite sit one tick apart, the order the recorder
 * writes them in: log_LobbyExit is queued as an event and the snapshot that
 * follows flushes it. The anchor is the snapshot, so the window opens on the
 * round's first real frame rather than on the empty frame before it.
 *
 * The two walkers differ on that last frame: lv_walkComputeTotalTimeMs counts
 * LOG_QUIT as a tick and lv_walkComputeGameStartMs does not. That matches the
 * decoder, which advances timeRunning before reading the byte, so the tick
 * that consumes LOG_QUIT is the one that ends playback and the end of the log
 * is 182060, not the 182040 of the last wait.
 *
 * Window [120020, 182060), length 62040. While it is on, progress, both seeks
 * and the clock run on the game's own 0..62040; the decoder's timeRunning and
 * the times the seeks are handed stay absolute throughout.
 */
int run_lv_hide_lobby_window_mapping(void) {
  const char *path = "lv_hide_lobby_window.wbv";
  LogBuf b;
  LogViewerState *lv;
  size_t curPos, totSize;
  uint32_t curTime, totTime;
  char buf[16];
  int savedHide = lv_screenGetHideLobby();
  int i;

  b.len = 0;
  putHeader(&b);
  putSnapshot(&b, false);          /* lobby: empty world, empty base table */
  for (i = 0; i < 23; i++) {
    putNoEvents(&b, 249);          /* waitLen is a uint8_t, so loop the wait */
  }
  putNoEvents(&b, 248);
  putEventFrame(&b, log_LobbyExit, NULL, 0);
  putSnapshot(&b, true);
  for (i = 0; i < 12; i++) {
    putNoEvents(&b, 249);
  }
  putNoEvents(&b, 100);
  putU8(&b, LOG_QUIT);

  lv_screenSetHideLobby(1);
  lv = loadSynthetic(&b, path);
  UT_ASSERT_MSG(lv != NULL, "synthetic hide-lobby log failed to build/load");

  UT_ASSERT_MSG(lv_screenGameStartMs() == 120020,
                "gameStartMs = %u (want 120020)", lv_screenGameStartMs());

  /* Straight after load the park has landed on game start, so 00:00 and the
   * first rendered frame agree. */
  lv_screenGetLogProgress(&curPos, &totSize, &curTime, &totTime);
  UT_ASSERT_MSG(totTime == 62040, "window length = %u (want 62040)", totTime);
  UT_ASSERT_MSG(curTime == 0, "position after load = %u (want 0)", curTime);
  UT_ASSERT_MSG(lv_screenGetTimeRunning() == 120020,
                "parked at %u absolute (want 120020)",
                lv_screenGetTimeRunning());

  lv_screenSeekToPosition(1.0f);
  lv_screenGetLogProgress(&curPos, &totSize, &curTime, &totTime);
  UT_ASSERT_MSG(curTime == 62040, "seek to 1.0 = %u (want 62040)", curTime);
  UT_ASSERT_MSG(lv_screenGetTimeRunning() == 182060,
                "seek to 1.0 = %u absolute (want 182060)",
                lv_screenGetTimeRunning());

  lv_screenSeekToPosition(0.0f);
  lv_screenGetLogProgress(&curPos, &totSize, &curTime, &totTime);
  UT_ASSERT_MSG(curTime == 0, "seek to 0.0 = %u (want 0)", curTime);
  UT_ASSERT_MSG(lv_screenGetTimeRunning() == 120020,
                "seek to 0.0 = %u absolute (want 120020)",
                lv_screenGetTimeRunning());

  /* Highlight clip times are absolute; the seek maps them into the window. */
  lv_screenSeekToTimeMs(122040);
  lv_screenGetLogProgress(&curPos, &totSize, &curTime, &totTime);
  UT_ASSERT_MSG(curTime == 2020, "absolute seek = %u (want 2020)", curTime);

  /* An absolute time inside the lobby clamps forward to game start. */
  lv_screenSeekToTimeMs(1000);
  lv_screenGetLogProgress(&curPos, &totSize, &curTime, &totTime);
  UT_ASSERT_MSG(curTime == 0, "lobby-time seek = %u (want 0)", curTime);
  UT_ASSERT_MSG(lv_screenGetTimeRunning() == 120020,
                "lobby-time seek = %u absolute (want 120020)",
                lv_screenGetTimeRunning());

  lv_screenFormatTime(182060, buf, sizeof buf);
  UT_ASSERT_MSG(strcmp(buf, "01:02") == 0,
                "end of window formats \"%s\" (want 01:02)", buf);
  lv_screenFormatTime(122040, buf, sizeof buf);
  UT_ASSERT_MSG(strcmp(buf, "00:02") == 0,
                "mid-game formats \"%s\" (want 00:02)", buf);
  lv_screenFormatTime(1000, buf, sizeof buf);
  UT_ASSERT_MSG(strcmp(buf, "00:00") == 0,
                "lobby time formats \"%s\" (want 00:00)", buf);

  /* Un-tick: the whole-file numbers are back, the same instant reads three
   * minutes later (the window start), and the playhead has not moved. */
  lv_screenSetHideLobby(0);
  lv_screenGetLogProgress(&curPos, &totSize, &curTime, &totTime);
  UT_ASSERT_MSG(totTime == 182060, "whole-file length = %u (want 182060)",
                totTime);
  UT_ASSERT_MSG(curTime == lv_screenGetTimeRunning(),
                "whole-file position = %u (want %u)", curTime,
                lv_screenGetTimeRunning());
  UT_ASSERT_MSG(lv_screenGetTimeRunning() == 120020,
                "un-tick moved the playhead to %u (want 120020)",
                lv_screenGetTimeRunning());
  lv_screenFormatTime(182060, buf, sizeof buf);
  UT_ASSERT_MSG(strcmp(buf, "03:02") == 0,
                "whole-file end formats \"%s\" (want 03:02)", buf);

  lv_decoderDestroy(lv);
  lv_screenSetHideLobby(savedHide);
  remove(path);
  return 0;
}

/*
 * No log_LobbyExit (a pre-lobby log, or a server that never sat in one): the
 * window is the whole file, so the toggle is inert — the same progress numbers
 * either way, and no park at load.
 */
int run_lv_hide_lobby_no_lobby_fallback(void) {
  const char *path = "lv_hide_lobby_nolobby.wbv";
  LogBuf b;
  LogViewerState *lv;
  size_t onPos, onSize, offPos, offSize;
  uint32_t onTime, onTotal, offTime, offTotal;
  int savedHide = lv_screenGetHideLobby();

  b.len = 0;
  putHeader(&b);
  putSnapshot(&b, true);
  putBaseSetOwner(&b, 0, 2);
  putNoEvents(&b, 50);
  putBaseSetOwner(&b, 1, 3);
  putU8(&b, LOG_QUIT);

  lv_screenSetHideLobby(1);
  lv = loadSynthetic(&b, path);
  UT_ASSERT_MSG(lv != NULL, "synthetic no-lobby log failed to build/load");

  UT_ASSERT_MSG(lv_screenGameStartMs() == 0,
                "gameStartMs = %u (want 0)", lv_screenGameStartMs());
  UT_ASSERT_MSG(lv_screenGetTimeRunning() == 0,
                "a no-lobby log parked at %u (want 0)",
                lv_screenGetTimeRunning());

  lv_screenGetLogProgress(&onPos, &onSize, &onTime, &onTotal);
  lv_screenSetHideLobby(0);
  lv_screenGetLogProgress(&offPos, &offSize, &offTime, &offTotal);
  UT_ASSERT_MSG(onPos == offPos && onSize == offSize && onTime == offTime &&
                    onTotal == offTotal,
                "the toggle moved a no-lobby log: %u/%u on, %u/%u off",
                onTime, onTotal, offTime, offTotal);

  lv_decoderDestroy(lv);
  lv_screenSetHideLobby(savedHide);
  remove(path);
  return 0;
}

/*
 * Marker-less lobby log: the round sat in the lobby but nothing wrote
 * log_LobbyExit, which is what a single-player recording looks like. The
 * anchor is the world rewrite itself — the first snapshot whose body carries a
 * map run — so the window still opens on the round instead of playing the
 * lobby out. Ticks: 4 x NOEVENTS 249 -> 1000, snapshot -> 1001 (20020),
 * NOEVENTS 99 -> 1101, LOG_QUIT -> 1102 (totalTimeMs 22040).
 */
int run_lv_hide_lobby_no_marker_anchor(void) {
  const char *path = "lv_hide_lobby_no_marker.wbv";
  LogBuf b;
  LogViewerState *lv;
  size_t curPos, totSize;
  uint32_t curTime, totTime;
  int savedHide = lv_screenGetHideLobby();
  int i;

  b.len = 0;
  putHeader(&b);
  putSnapshot(&b, false);          /* lobby: empty world */
  for (i = 0; i < 4; i++) {
    putNoEvents(&b, 249);
  }
  putSnapshot(&b, true);           /* the world rewrite, with no marker in front */
  putNoEvents(&b, 99);
  putU8(&b, LOG_QUIT);

  lv_screenSetHideLobby(1);
  lv = loadSynthetic(&b, path);
  UT_ASSERT_MSG(lv != NULL, "synthetic marker-less log failed to build/load");

  UT_ASSERT_MSG(lv_screenGameStartMs() == 20020,
                "gameStartMs = %u (want 20020)", lv_screenGameStartMs());

  /* The park lands on the rewrite, so 00:00 is the round's first frame. */
  lv_screenGetLogProgress(&curPos, &totSize, &curTime, &totTime);
  UT_ASSERT_MSG(totTime == 2020, "window length = %u (want 2020)", totTime);
  UT_ASSERT_MSG(curTime == 0, "position after load = %u (want 0)", curTime);
  UT_ASSERT_MSG(lv_screenGetTimeRunning() == 20020,
                "parked at %u absolute (want 20020)",
                lv_screenGetTimeRunning());

  lv_decoderDestroy(lv);
  lv_screenSetHideLobby(savedHide);
  remove(path);
  return 0;
}

/*
 * No-lobby log carrying the periodic in-game snapshots the recorder writes
 * every FULL_SYNC_INTERVAL ticks. Those carry a world too, so "the first
 * snapshot with a map run" cannot be the anchor on its own: the opening
 * snapshot, which the loader consumes, is what says whether a lobby ran. It
 * carries a world here, so the round starts at tick 0 and the window is the
 * whole file.
 */
int run_lv_hide_lobby_periodic_snapshot(void) {
  const char *path = "lv_hide_lobby_periodic.wbv";
  LogBuf b;
  LogViewerState *lv;
  size_t onPos, onSize, offPos, offSize;
  uint32_t onTime, onTotal, offTime, offTotal;
  int savedHide = lv_screenGetHideLobby();
  int i;

  b.len = 0;
  putHeader(&b);
  putSnapshot(&b, true);           /* opening: the world is already there */
  for (i = 0; i < 2; i++) {
    putNoEvents(&b, 249);
  }
  putSnapshot(&b, true);           /* periodic in-game resync, not a game start */
  putNoEvents(&b, 99);
  putU8(&b, LOG_QUIT);

  lv_screenSetHideLobby(1);
  lv = loadSynthetic(&b, path);
  UT_ASSERT_MSG(lv != NULL, "synthetic periodic-snapshot log failed to build/load");

  UT_ASSERT_MSG(lv_screenGameStartMs() == 0,
                "gameStartMs = %u (want 0)", lv_screenGameStartMs());
  UT_ASSERT_MSG(lv_screenGetTimeRunning() == 0,
                "a no-lobby log parked at %u (want 0)",
                lv_screenGetTimeRunning());

  lv_screenGetLogProgress(&onPos, &onSize, &onTime, &onTotal);
  lv_screenSetHideLobby(0);
  lv_screenGetLogProgress(&offPos, &offSize, &offTime, &offTotal);
  UT_ASSERT_MSG(onPos == offPos && onSize == offSize && onTime == offTime &&
                    onTotal == offTotal,
                "the toggle moved a no-lobby log: %u/%u on, %u/%u off",
                onTime, onTotal, offTime, offTotal);

  lv_decoderDestroy(lv);
  lv_screenSetHideLobby(savedHide);
  remove(path);
  return 0;
}

/* One log_PlayerJoined event: slot, 2-char country, account flags, reserved,
 * then the name as a pascal string (the layout screen.c's decode reads). */
static void putPlayerJoined(LogBuf *b, uint8_t slot, const char *name) {
  uint8_t payload[5 + 1 + 255];
  size_t nameLen = strlen(name);
  payload[0] = slot;
  payload[1] = 'G';
  payload[2] = 'B';
  payload[3] = 0;
  payload[4] = 0;
  payload[5] = (uint8_t)nameLen;
  memcpy(payload + 6, name, nameLen);
  putEventFrame(b, log_PlayerJoined, payload, (uint16_t)(6 + nameLen));
}

/*
 * A player who joins after the round starts is not in the live roster while the
 * playhead is parked at game start, which is where the round summary is emitted
 * from. The load-time name walk reads the join event out of the log itself, so
 * the name is available regardless of the playhead.
 *
 *   LobbyExit frame  -> tick   1
 *   snapshot         -> tick   2  (gameStartMs 40, where the load parks)
 *   NOEVENTS 100     -> tick 103
 *   PlayerJoined     -> tick 104
 */
int run_lv_logged_name_from_join_event(void) {
  const char *path = "lv_logged_name_join.wbv";
  LogBuf b;
  LogViewerState *lv;
  char logged[PLAYER_NAME_LEN];
  char live[PLAYER_NAME_LEN];
  int savedHide = lv_screenGetHideLobby();

  b.len = 0;
  putHeader(&b);
  putSnapshot(&b, false);   /* lobby: every slot not in use */
  putEventFrame(&b, log_LobbyExit, NULL, 0);
  putSnapshot(&b, true);    /* the world rewrite the round starts on */
  putNoEvents(&b, 100);
  putPlayerJoined(&b, 3, "Chapu");
  putU8(&b, LOG_QUIT);

  lv_screenSetHideLobby(1);
  lv = loadSynthetic(&b, path);
  UT_ASSERT_MSG(lv != NULL, "synthetic join log failed to build/load");

  UT_ASSERT_MSG(lv_screenGetTimeRunning() == 40,
                "parked at %u (want 40, the game start)",
                lv_screenGetTimeRunning());

  /* The playhead has not reached the join, so the live roster is empty here. */
  lv_screenGetPlayerName(live, 3, sizeof(live));
  UT_ASSERT_MSG(strcmp(live, NO_TANK) == 0,
                "live roster named slot 3 '%s' at game start (want %s)", live,
                NO_TANK);

  UT_ASSERT_MSG(lv_screenGetLoggedPlayerName(3, logged, sizeof(logged)) == TRUE,
                "the log's own join event did not name slot 3");
  UT_ASSERT_MSG(strcmp(logged, "Chapu") == 0,
                "logged name = '%s' (want 'Chapu')", logged);

  /* A slot the log never named has nothing to report. */
  UT_ASSERT_MSG(lv_screenGetLoggedPlayerName(5, logged, sizeof(logged)) == FALSE,
                "slot 5 reported a name '%s' from a log that never joined it",
                logged);

  lv_decoderDestroy(lv);
  lv_screenSetHideLobby(savedHide);
  remove(path);
  return 0;
}
