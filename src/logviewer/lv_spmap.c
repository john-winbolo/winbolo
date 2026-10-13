/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Writes the map for "Open map at this time in single player". See
 * lv_spmap.h for what the owners become.
 *
 * The file is the usual .map: header, version, the three counts, pillboxes,
 * bases, starts, then the terrain runs. The terrain is the decoder's own,
 * written by lv_mapWriteRuns as File > Save Map writes it. The items are
 * written here because their owners and the start list are not the
 * decoder's.
 */

#include <stdio.h>
#include <string.h>

#include "lv_global.h"
#include "backend.h"
#include "logviewer.h"
#include "lv_bolo_map.h"
#include "lv_pillbox.h"
#include "lv_bases.h"
#include "lv_starts.h"
#include "lv_players.h"
#include "lv_spmap.h"

#define SPMAP_STEP_MS 20u   /* one lv_screenLogTick of playback */

int lvSpmapWrite(const char *path, int self, LvSpmapInfo *info) {
  LogViewerState *lv = lv_screenGetState();
  unsigned char   allied[LV_SPMAP_SLOTS];
  unsigned char   owns[LV_SPMAP_SLOTS];
  unsigned char   table[LV_SPMAP_SLOTS];
  LvSpmapInfo     done;
  FILE           *fp;
  BYTE            total, count, written;
  BYTE            mx = 0, my = 0, px = 0, py = 0, frame = 0;
  bool            boat = FALSE;
  bool            ok = TRUE;
  long            countsAt;
  int             i;

  memset(&done, 0, sizeof(done));
  done.self = -1;
  if (info != NULL) {
    *info = done;
  }
  if (lv == NULL || path == NULL || path[0] == '\0') {
    return 0;
  }
  if (self < 0 || self >= LV_SPMAP_SLOTS || lv_playersIsInUse((BYTE)self) == FALSE) {
    self = -1;
  }
  done.self = self;

  /* Who is on the selected side, and who owns anything. */
  memset(allied, 0, sizeof(allied));
  memset(owns, 0, sizeof(owns));
  for (i = 0; i < LV_SPMAP_SLOTS && self >= 0; i++) {
    if (i == self || (lv_playersIsInUse((BYTE)i) == TRUE &&
                      lv_playersIsAllie((BYTE)i, (BYTE)self) == TRUE)) {
      allied[i] = 1;
    }
  }
  total = lv_pillsGetNumPills(&lv->pb);
  for (count = 1; count <= total; count++) {
    pillbox p;
    memset(&p, 0, sizeof(p));
    p.owner = NEUTRAL;
    lv_pillsGetPill(&lv->pb, &p, count);
    if (p.owner < LV_SPMAP_SLOTS) owns[p.owner] = 1;
  }
  total = lv_basesGetNumBases(&lv->bs);
  for (count = 1; count <= total; count++) {
    base b;
    memset(&b, 0, sizeof(b));
    b.owner = NEUTRAL;
    lv_basesGetBase(&lv->bs, &b, count);
    if (b.owner < LV_SPMAP_SLOTS) owns[b.owner] = 1;
  }
  lvSpmapOwnerTable(self, allied, owns, table);

  fp = fopen(path, "wb");
  if (fp == NULL) {
    return 0;
  }
  ok = fputs(MAP_HEADER, fp) != EOF && fputc(CURRENT_MAP_VERSION, fp) != EOF;
  /* The three counts are known only after the items are written: an item
     that is not on the map is left out. Hold their place. */
  countsAt = ftell(fp);
  ok = ok && fputc(0, fp) != EOF && fputc(0, fp) != EOF && fputc(0, fp) != EOF;

  /* Pillboxes. One a tank carries is not on the map at this time, and the
     place the decoder holds for it is where it was picked up, so it is left
     out. */
  total   = lv_pillsGetNumPills(&lv->pb);
  written = 0;
  for (count = 1; count <= total && ok; count++) {
    pillbox p;
    memset(&p, 0, sizeof(p));
    p.owner = NEUTRAL;
    lv_pillsGetPill(&lv->pb, &p, count);
    if (lv_pillsIsActive(&lv->pb, count) == FALSE) {
      continue;
    }
    if (p.inTank) {
      done.pillsCarried++;
      continue;
    }
    if (done.havePill == 0) {
      done.havePill   = 1;
      done.firstPillX = p.x;
      done.firstPillY = p.y;
    }
    ok = fputc(p.x, fp) != EOF && fputc(p.y, fp) != EOF &&
         fputc(lvSpmapOwner(table, p.owner), fp) != EOF &&
         fputc(p.armour, fp) != EOF && fputc(p.speed, fp) != EOF;
    written++;
  }
  done.pills = written;

  /* Bases. */
  total   = lv_basesGetNumBases(&lv->bs);
  written = 0;
  for (count = 1; count <= total && ok; count++) {
    base b;
    memset(&b, 0, sizeof(b));
    b.owner = NEUTRAL;
    lv_basesGetBase(&lv->bs, &b, count);
    if (lv_basesIsActive(&lv->bs, count) == FALSE) {
      continue;
    }
    ok = fputc(b.x, fp) != EOF && fputc(b.y, fp) != EOF &&
         fputc(lvSpmapOwner(table, b.owner), fp) != EOF &&
         fputc(b.armour, fp) != EOF && fputc(b.shells, fp) != EOF &&
         fputc(b.mines, fp) != EOF;
    written++;
  }
  done.bases = written;

  /* Starts. When the selected player's tank is alive and on the map, its
     square is the only start, so the user's tank is placed there facing the
     same way. Otherwise the map keeps its own starts. */
  written = 0;
  if (self >= 0) {
    lv_playersGetTankDetails((BYTE)self, &mx, &my, &px, &py, &frame, &boat);
    if (lv->gameViewHud[self].alive && (mx != 0 || my != 0)) {
      done.tankStart = 1;
      done.startX    = mx;
      done.startY    = my;
      done.startDir  = lvSpmapStartDir(frame);
      ok = ok && fputc(mx, fp) != EOF && fputc(my, fp) != EOF &&
           fputc(done.startDir, fp) != EOF;
      written = 1;
    }
  }
  if (done.tankStart == 0) {
    total = lv_startsGetNumStarts(&lv->ss);
    for (count = 1; count <= total && ok; count++) {
      start s;
      memset(&s, 0, sizeof(s));
      lv_startsGetStartStruct(&lv->ss, &s, count);
      ok = fputc(s.x, fp) != EOF && fputc(s.y, fp) != EOF && fputc(s.dir, fp) != EOF;
      written++;
    }
  }
  done.starts = written;

  if (ok) {
    ok = lv_mapWriteRuns(fp, &lv->mp) == TRUE;
  }
  if (ok) {
    ok = fseek(fp, countsAt, SEEK_SET) == 0 && fputc(done.pills, fp) != EOF &&
         fputc(done.bases, fp) != EOF && fputc(done.starts, fp) != EOF;
  }
  if (fclose(fp) != 0) {
    ok = FALSE;
  }
  if (!ok) {
    remove(path);
    return 0;
  }
  if (info != NULL) {
    *info = done;
  }
  return 1;
}

int lvSpmapWriteFromLog(const char *logPath, uint32_t ms, int self,
                        const char *mapPath, LvSpmapInfo *info) {
  LogViewerState  *lv;
  LvDecodeObserver quiet;
  char             path[FILENAME_MAX];
  int             hideLobby;
  int             result = 0;
  int             n;

  if (logPath == NULL || mapPath == NULL) {
    return 0;
  }
  /* A path too long for the buffer is refused: cut short it would name
     some other file. */
  n = snprintf(path, sizeof(path), "%s", logPath);
  if (n < 0 || (size_t)n >= sizeof(path)) {
    return 0;
  }
  lv = lv_decoderCreate(false);
  if (lv == NULL) {
    return 0;
  }
  /* From the log's first byte, not from the round's start. */
  hideLobby = lv_screenGetHideLobby();
  lv_screenSetHideLobby(0);
  /* An observer that takes nothing: with one set the decoder leaves the
     host alone (no event rows, no end-of-log calls), as the dump does. */
  memset(&quiet, 0, sizeof(quiet));
  lv_screenSetObserver(&quiet);
  if (lv_screenLoadMap(path, 4) == TRUE) {
    lv->fastForwarding = TRUE;   /* no sounds */
    while (lv->isPlaying == TRUE && lv->timeRunning + SPMAP_STEP_MS <= ms) {
      lv_screenLogTick();
    }
    result = lvSpmapWrite(mapPath, self, info);
  }
  lv_screenSetObserver(NULL);
  lv_decoderDestroy(lv);
  lv_screenSetHideLobby(hideLobby);
  return result;
}
