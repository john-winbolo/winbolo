/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * What the log viewer learns about a log when it opens it, before playback:
 * the total time, the game start, every name each slot carried, the rule
 * changes, the server tick index and the scenario presentation index. Each
 * committed .wbv fixture is loaded and all of that is written out as one
 * line, which must match the line recorded for it. The per-feature tests
 * build small logs to pin each one's edge cases; this pins all of them
 * together on recordings the real writer made.
 *
 * Set WB_LOAD_WALK_EXTRA to the path of any other .wbv to print its line
 * too, unchecked, for comparing a large local log before and after a change.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lv_global.h"
#include "backend.h"
#include "logviewer.h"
#include "lv_presentation.h"
#include "lv_scripts.h"
#include "test_harness.h"

#ifndef WB_WBV_FIXTURE_DIR
#define WB_WBV_FIXTURE_DIR "tests/fixtures/wbv"
#endif

typedef struct {
  char  *text;
  size_t len;
  size_t cap;
} Digest;

static void digestAdd(Digest *d, const char *fmt, ...) {
  va_list ap;
  int n;

  for (;;) {
    size_t room = d->cap - d->len;
    va_start(ap, fmt);
    n = vsnprintf(d->text + d->len, room, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < room) {
      d->len += (size_t)n;
      return;
    }
    d->cap = d->cap * 2 + (size_t)n;
    d->text = (char *)realloc(d->text, d->cap);
    if (d->text == NULL) abort();
  }
}

static uint8_t *readFile(const char *path, size_t *outLen) {
  FILE *f = fopen(path, "rb");
  long sz;
  uint8_t *buf;

  if (f == NULL) return NULL;
  fseek(f, 0, SEEK_END);
  sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0) {
    fclose(f);
    return NULL;
  }
  buf = (uint8_t *)malloc((size_t)sz);
  if (buf != NULL && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
    free(buf);
    buf = NULL;
  }
  fclose(f);
  *outLen = (size_t)sz;
  return buf;
}

/* Load the log at path and describe what the load collected. Returns NULL if
   it would not load. fromFile opens it as the viewer's File > Open does, with
   the larger size limit; otherwise it goes through the in-memory path the
   recap reel uses. */
static char *describeLoad(const char *path, bool fromFile) {
  Digest d;
  LogViewerState *lv;
  const LvRuleChange *changes = NULL;
  uint8_t *buf;
  size_t len = 0;
  uint32_t total;
  char name[64];
  int n;
  int i;

  buf = NULL;
  if (!fromFile) {
    buf = readFile(path, &len);
    if (buf == NULL) return NULL;
  }

  lv = lv_decoderCreate(false);
  if (lv == NULL) {
    free(buf);
    return NULL;
  }
  lv_screenSetSizeX(30);
  lv_screenSetSizeY(30);
  /* The memory load takes the buffer whether or not it works. */
  if ((fromFile ? lv_screenLoadMap((char *)path, 0)
                : lv_screenLoadMapFromMemory(buf, len)) != TRUE) {
    lv_decoderDestroy(lv);
    return NULL;
  }

  d.cap = 1024;
  d.len = 0;
  d.text = (char *)malloc(d.cap);
  if (d.text == NULL) abort();
  d.text[0] = '\0';

  total = lv_screenGetState()->totalTimeMs;
  digestAdd(&d, "total=%u start=%u", total, lv_screenGameStartMs());

  digestAdd(&d, " names=");
  for (i = 0; i < MAX_TANKS; i++) {
    if (lv_screenGetLoggedPlayerName((BYTE)i, name, sizeof(name)) == TRUE) {
      digestAdd(&d, "%d:%s;", i, name);
    }
  }

  n = lv_screenGetRuleChanges(&changes);
  digestAdd(&d, " rules=");
  for (i = 0; i < n; i++) {
    digestAdd(&d, "%u:%d:%g;", changes[i].ms, changes[i].index, changes[i].value);
  }

  digestAdd(&d, " ticks=%d:", lv_screenHasServerTick() ? 1 : 0);
  for (i = 0; i <= 8; i++) {
    uint32_t ms = (uint32_t)(((uint64_t)total * (uint64_t)i) / 8u);
    digestAdd(&d, "%u;", lv_screenServerTickAt(ms));
  }

  /* The presentation index is read back through the state it rebuilds at a
     time: the teams at the end of the log. */
  lv_screenSeekToTimeMs(total);
  digestAdd(&d, " teams=");
  for (i = 0; i < MAX_TANKS; i++) {
    BYTE team;
    if (lv_screenGetSlotTeam((BYTE)i, &team) == TRUE) {
      digestAdd(&d, "%d:%u;", i, (unsigned)team);
    }
  }

  lv_decoderDestroy(lv);
  return d.text;
}

int run_lv_load_walk_fixtures(void) {
  static const struct {
    const char *file;
    const char *want;
  } cases[] = {
    { "boat_bank_road_fast.wbv",     
      "total=6000 start=0 names= rules= ticks=0:0;75;150;225;300;375;450;525;600; teams=" },
    { "builder_yard_pill_place.wbv", 
      "total=6640 start=0 names= rules= ticks=0:0;83;166;249;332;415;498;581;664; teams=" },
    { "road_spit_shell_open.wbv",    
      "total=30100 start=0 names= rules= ticks=0:0;376;752;1128;1505;1881;2257;2633;3010; teams=" },
    { "spectator_v1.wbv",            
      "total=80 start=0 names=1:Joiner; rules= ticks=0:0;1;2;3;4;5;6;7;8; teams=" },
    { "spectator_v2.wbv",            
      "total=80 start=0 names=1:Joiner; rules= ticks=0:0;1;2;3;4;5;6;7;8; teams=" },
  };
  const char *dir = getenv("WB_WBV_FIXTURE_DIR");
  const char *extra = getenv("WB_LOAD_WALK_EXTRA");
  char path[512];
  size_t i;
  int failed = 0;

  if (dir == NULL || dir[0] == '\0') dir = WB_WBV_FIXTURE_DIR;

  for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char *got;
    snprintf(path, sizeof(path), "%s/%s", dir, cases[i].file);
    got = describeLoad(path, false);
    UT_ASSERT_MSG(got != NULL, "%s did not load", path);
    if (strcmp(got, cases[i].want) != 0) {
      fprintf(stderr, "%s\n  got:  %s\n  want: %s\n", cases[i].file, got, cases[i].want);
      failed = 1;
    }
    free(got);
  }

  if (extra != NULL && extra[0] != '\0') {
    char *got = describeLoad(extra, true);
    printf("%s: %s\n", extra, got != NULL ? got : "(did not load)");
    free(got);
  }

  UT_ASSERT_MSG(failed == 0, "a fixture's load results changed");
  return 0;
}
