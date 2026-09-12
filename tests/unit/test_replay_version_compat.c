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
 * Recordings made before LOG_VERSION 3, read by the reader that came after.
 *
 * log_PillSetHealth grew from one payload byte to two: up to v2 the pillbox
 * index sat in the byte's high nibble and its armour in the low, and from v3
 * each has a byte of its own. The record kept its type code through the
 * change, so nothing inside an event says which shape it holds — only the
 * file's version byte does.
 *
 * That makes the length a reader gets wrong an alignment bug rather than a
 * wrong armour value: a reader that takes two bytes from a v2 file swallows
 * the next record's type code as the armour and reads everything after it off
 * its boundaries. So each case here puts a second, different record straight
 * behind the health one and asserts what that second record did, which is
 * what a reader that lost its place cannot get right.
 *
 * Both cases build the bytes by hand — the writer can only produce the
 * current version, and an old file is the whole point — and run them through
 * the production reader, lv_screenLoadMapFromMemory and lv_screenLogTick.
 *
 * The v1 case is here for the other reader: the byte walk the load runs to
 * size the scrubber. v2 and later frame every event with its own length, so
 * the walk steps over a record it does not understand; a v0/v1 file has no
 * framing at all, so the walk has to know each record's length itself, and a
 * walk that loses its place stops where it stands and reports a duration
 * short of the file.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lv_global.h"
#include "backend.h"
#include "blocks.h"
#include "logviewer.h"
#include "lv_log.h"
#include "lv_pillbox.h"
#include "test_harness.h"
#include "zip.h"

/* Playback advances one 20ms tick a call; these logs are a handful of ticks
 * long, so reaching the cap means the stream never terminated. */
#define VC_TICK_CAP 512

/* The pillbox the records name, 0-based the way the log counts, and the
 * armour and square they set. Armour 9 fits a nibble, which a v2 record has
 * to, and is not the armour the snapshot handed out; every byte here differs
 * from every other so a value read out of the wrong place cannot match by
 * luck. */
#define VC_PILL_INDEX 0
#define VC_PILL_ARMOUR 9
#define VC_PILL_X 77
#define VC_PILL_Y 88

/* What the snapshot gives the two pillboxes, so the case can tell a record
 * that applied from one that did not. */
#define VC_SNAP_ARMOUR 4
#define VC_SNAP_X 40
#define VC_SNAP_Y 41

typedef struct {
  uint8_t buf[4096];
  size_t len;
  /* v0/v1 only: the rolling XOR key the reader will be holding when it
     reaches the next byte written. 0 for a plaintext file. */
  uint8_t key;
} VcLog;

static void vcPut(VcLog *b, const void *bytes, size_t n) {
  const uint8_t *src = (const uint8_t *)bytes;
  size_t i;
  for (i = 0; i < n; i++) {
    b->buf[b->len++] = (uint8_t)(src[i] ^ b->key);
  }
}

static void vcPutU8(VcLog *b, uint8_t v) { vcPut(b, &v, 1); }

static void vcPutZeros(VcLog *b, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    vcPutU8(b, 0);
  }
}

/* The header, as the loader reads it: magic, version, map name, the seven
 * settings bytes, address and port, create time, WBN key. Always plaintext —
 * the key a v0/v1 body is read with is seeded from the create time, which is
 * written here as zero so the body starts in the clear and the case can roll
 * the key itself from the first event on. */
static void vcPutHeader(VcLog *b, uint8_t version) {
  b->key = 0;
  vcPut(b, "WBOLOMOV", 8);
  vcPutU8(b, version);
  vcPutU8(b, 4);
  vcPut(b, "Test", 4);
  vcPutU8(b, 1);            /* game type */
  vcPutU8(b, 0);            /* hidden mines */
  vcPutU8(b, 0);            /* ai */
  vcPutU8(b, 0);            /* password */
  vcPutU8(b, MAX_TANKS);    /* max players */
  vcPutU8(b, 2); vcPutU8(b, 0); vcPutU8(b, 2);   /* game version */
  vcPutZeros(b, 4 + 2);     /* ip + port */
  vcPutZeros(b, 4);         /* create time: the v0/v1 key seed, kept 0 */
  vcPutZeros(b, 32);        /* WBN key */
}

/* One LOG_SNAPSHOT frame carrying two pillboxes on the map, no bases, no
 * starts and one map run. Nine bytes a pillbox: x, y, armour, owner, speed,
 * in-tank, and three the viewer reads past. */
static void vcPutSnapshot(VcLog *b) {
  uint8_t pills[1 + 2 * 9];
  static const uint8_t mapRun[5] = {5, 0, 0, 2, (8 << 4) | GRASS};
  static const uint8_t mapTerminator[4] = {4, 255, 255, 255};
  int i;

  memset(pills, 0, sizeof(pills));
  pills[0] = 2;
  for (i = 0; i < 2; i++) {
    pills[1 + i * 9] = (uint8_t)(VC_SNAP_X + i);   /* x */
    pills[2 + i * 9] = VC_SNAP_Y;                  /* y */
    pills[3 + i * 9] = VC_SNAP_ARMOUR;             /* armour */
    pills[4 + i * 9] = 0xFF;                       /* owner: neutral */
  }

  vcPutU8(b, LOG_SNAPSHOT);
  vcPutZeros(b, 8);                       /* gmeStartDelay + gmeLength */
  vcPutU8(b, (uint8_t)sizeof(pills));
  vcPut(b, pills, sizeof(pills));
  vcPutU8(b, 1); vcPutU8(b, 0);           /* bases: count 0 */
  vcPutU8(b, 1); vcPutU8(b, 0);           /* starts: count 0 */
  vcPut(b, mapRun, sizeof(mapRun));
  vcPut(b, mapTerminator, 4);
  for (i = 0; i < MAX_TANKS; i++) {       /* every slot "not in use" */
    vcPutU8(b, 2); vcPutU8(b, 0); vcPutU8(b, 0);
  }
}

/* One event inside a v2-or-later frame: [type][u16 BE length][payload]. */
static void vcPutFramedEvent(VcLog *b, uint8_t evType, const uint8_t *payload,
                             uint16_t payloadLen) {
  vcPutU8(b, evType);
  vcPutU8(b, (uint8_t)(payloadLen >> 8));
  vcPutU8(b, (uint8_t)(payloadLen & 0xFF));
  if (payloadLen > 0) vcPut(b, payload, payloadLen);
}

/* One event inside a v0/v1 frame: [type][payload], no length, and the key
 * rolls to the type code once the event is behind us — the rotation the
 * reader performs after each event it processes. */
static void vcPutKeyedEvent(VcLog *b, uint8_t evType, const uint8_t *payload,
                            size_t payloadLen) {
  vcPutU8(b, evType);
  if (payloadLen > 0) vcPut(b, payload, payloadLen);
  b->key = evType;
}

/* Wrap the bytes as log.dat in a .wbv zip on disk and read the zip back into
 * a heap buffer, which the loader takes ownership of. */
static uint8_t *vcZipToMemory(const VcLog *b, const char *path,
                              size_t *outLen) {
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
      zipWriteInFileInZip(zf, (void *)b->buf, (unsigned)b->len) != ZIP_OK ||
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

static LogViewerState *vcLoad(const VcLog *b, const char *path) {
  size_t zipLen = 0;
  uint8_t *zipData = vcZipToMemory(b, path, &zipLen);
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

/* Play to end-of-log. Returns false if playback ran past the cap without the
 * stream ending, which is what a reader that lost its place does. */
static bool vcPlayToEnd(void) {
  int steps = 0;
  while (lv_screenIsPlaying() == TRUE && steps < VC_TICK_CAP) {
    lv_screenLogTick();
    steps++;
  }
  return lv_screenIsPlaying() == FALSE;
}

/* The two records every case writes, in the order that makes a length
 * mistake visible: the health record, then a placement record for the same
 * pillbox. Only a reader that sized the first one right reaches the second
 * on its own boundary. */
static void vcNibbleHealthPayload(uint8_t *out) {
  /* Up to v2: index in the high nibble, armour in the low. */
  out[0] = (uint8_t)((VC_PILL_INDEX << 4) | VC_PILL_ARMOUR);
}

static void vcPlacePayload(uint8_t *out) {
  out[0] = VC_PILL_INDEX;
  out[1] = VC_PILL_X;
  out[2] = VC_PILL_Y;
}

/* What the decoded world says about the two pillboxes, read out before the
 * decoder is torn down so the case can assert on it afterwards. */
typedef struct {
  uint8_t armour;       /* the pillbox both records name */
  uint8_t x;
  uint8_t y;
  uint8_t otherArmour;  /* the pillbox neither record names */
} VcPills;

/* lv_pillsGetPill counts from one; the records count from zero. */
static void vcReadPills(LogViewerState *lv, VcPills *out) {
  pillbox got;
  pillbox other;

  memset(&got, 0, sizeof(got));
  memset(&other, 0, sizeof(other));
  lv_pillsGetPill(&lv->pb, &got, (BYTE)(VC_PILL_INDEX + 1));
  lv_pillsGetPill(&lv->pb, &other, (BYTE)(VC_PILL_INDEX + 2));

  out->armour = got.armour;
  out->x = got.x;
  out->y = got.y;
  out->otherArmour = other.armour;
}

/*
 * A v2 file's health record is one byte of nibbles, and the reader has to
 * size it by the file's version rather than by the shape the writer produces
 * today. The placement record behind it proves the cursor stayed on its
 * boundary.
 */
int run_replay_v2_pill_health_nibble(void) {
  const char *path = "replay_v2_pill_health.wbv";
  VcLog b;
  LogViewerState *lv;
  uint8_t health[1];
  uint8_t place[3];
  VcPills pills;

  memset(&b, 0, sizeof(b));
  memset(&pills, 0, sizeof(pills));
  vcNibbleHealthPayload(health);
  vcPlacePayload(place);

  vcPutHeader(&b, LOG_VERSION_V2);
  vcPutSnapshot(&b);
  vcPutU8(&b, LOG_EVENT);
  vcPutU8(&b, 2);                 /* two events in this frame */
  vcPutFramedEvent(&b, (uint8_t)log_PillSetHealth, health, 1);
  vcPutFramedEvent(&b, (uint8_t)log_PillSetPlace, place, 3);
  vcPutU8(&b, LOG_QUIT);

  lv = vcLoad(&b, path);
  UT_ASSERT_MSG(lv != NULL, "the v2 log failed to load");
  UT_ASSERT_MSG(lv->loadedLogVersion == LOG_VERSION_V2,
                "loaded version is %u, wanted %u",
                (unsigned)lv->loadedLogVersion, (unsigned)LOG_VERSION_V2);

  /* The load's byte walk sizes the file before a tick is played: the opening
     snapshot is consumed by the load, then the event frame and LOG_QUIT are
     a tick each at 20ms. */
  UT_ASSERT_MSG(lv->totalTimeMs == 40,
                "the load walk made the file %u ms, wanted 40", lv->totalTimeMs);

  UT_ASSERT_MSG(vcPlayToEnd(), "playback never reached the end of the log");
  vcReadPills(lv, &pills);

  lv_decoderDestroy(lv);
  remove(path);

  UT_ASSERT_MSG(pills.armour == VC_PILL_ARMOUR,
                "pillbox armour is %u, wanted %u", (unsigned)pills.armour,
                (unsigned)VC_PILL_ARMOUR);
  UT_ASSERT_MSG(pills.x == VC_PILL_X && pills.y == VC_PILL_Y,
                "pillbox sits at %u,%u, wanted %u,%u — the record behind the "
                "health one was read off its boundary", (unsigned)pills.x,
                (unsigned)pills.y, (unsigned)VC_PILL_X, (unsigned)VC_PILL_Y);
  UT_ASSERT_MSG(pills.otherArmour == VC_SNAP_ARMOUR,
                "the pillbox no record names has armour %u, wanted the "
                "snapshot's %u", (unsigned)pills.otherArmour,
                (unsigned)VC_SNAP_ARMOUR);
  return 0;
}

/*
 * The same record in a v1 file, which has no framing to fall back on: the
 * playback reader and the load's sizing walk both have to know the record is
 * one byte long. The XOR key the body is read with rolls to each event's type
 * code, and the builder rolls with it.
 */
int run_replay_v1_pill_health_nibble(void) {
  const char *path = "replay_v1_pill_health.wbv";
  VcLog b;
  LogViewerState *lv;
  uint8_t health[1];
  uint8_t place[3];
  VcPills pills;

  memset(&b, 0, sizeof(b));
  memset(&pills, 0, sizeof(pills));
  vcNibbleHealthPayload(health);
  vcPlacePayload(place);

  vcPutHeader(&b, LOG_VERSION_V1);
  vcPutSnapshot(&b);
  vcPutU8(&b, LOG_EVENT);
  vcPutU8(&b, 2);
  vcPutKeyedEvent(&b, (uint8_t)log_PillSetHealth, health, 1);
  vcPutKeyedEvent(&b, (uint8_t)log_PillSetPlace, place, 3);
  vcPutU8(&b, LOG_QUIT);

  lv = vcLoad(&b, path);
  UT_ASSERT_MSG(lv != NULL, "the v1 log failed to load");
  UT_ASSERT_MSG(lv->loadedLogVersion == LOG_VERSION_V1,
                "loaded version is %u, wanted %u",
                (unsigned)lv->loadedLogVersion, (unsigned)LOG_VERSION_V1);

  /* A walk that took two bytes for the health record would read the
     placement record's type byte as armour, fail to size whatever it landed
     on next, and stop with the file part-measured. */
  UT_ASSERT_MSG(lv->totalTimeMs == 40,
                "the load walk made the file %u ms, wanted 40 — the walk lost "
                "its place on the health record", lv->totalTimeMs);

  UT_ASSERT_MSG(vcPlayToEnd(), "playback never reached the end of the log");
  vcReadPills(lv, &pills);

  lv_decoderDestroy(lv);
  remove(path);

  UT_ASSERT_MSG(pills.armour == VC_PILL_ARMOUR,
                "pillbox armour is %u, wanted %u", (unsigned)pills.armour,
                (unsigned)VC_PILL_ARMOUR);
  UT_ASSERT_MSG(pills.x == VC_PILL_X && pills.y == VC_PILL_Y,
                "pillbox sits at %u,%u, wanted %u,%u — the record behind the "
                "health one was read off its boundary", (unsigned)pills.x,
                (unsigned)pills.y, (unsigned)VC_PILL_X, (unsigned)VC_PILL_Y);
  UT_ASSERT_MSG(pills.otherArmour == VC_SNAP_ARMOUR,
                "the pillbox no record names has armour %u, wanted the "
                "snapshot's %u", (unsigned)pills.otherArmour,
                (unsigned)VC_SNAP_ARMOUR);
  return 0;
}
