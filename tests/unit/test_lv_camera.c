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
 * Replay camera range. The viewer's tile fetch casts the column and row to a
 * BYTE, so a view whose left edge plus width passes 255 wraps round and draws
 * column 0 beside column 255: the map's two mined borders appear back to back
 * in the middle of the screen. The embedded lobby reel sizes its grid to the
 * panel before the load puts the camera at 127,127, and at 0.5x a wide panel
 * is more than 128 tiles across, so the reel opened wrapped and stayed that
 * way until something else happened to move the camera.
 *
 * Every writer of the offset now ends in one clamp that holds the whole-tile
 * offset in [0, 255 - screenSize]. Each case here drives one writer through a
 * grid wider than half the map and checks the view's far edge stays at 255.
 * No log is needed: the camera is decoder state, and the setup call every
 * load runs is public.
 */
#include <stdint.h>
#include <string.h>

#include "lv_global.h"
#include "backend.h"
#include "logviewer.h"
#include "test_harness.h"

/* A decoder sized the way a host sizes it (grid first) and then taken through
 * the setup every load path runs, which is where 127,127 comes from. */
static LogViewerState *cameraDecoder(BYTE sizeX, BYTE sizeY) {
  LogViewerState *lv = lv_decoderCreate(false);
  if (lv == NULL) return NULL;
  lv_screenSetSizeX(sizeX);
  lv_screenSetSizeY(sizeY);
  lv_screenDestroy();
  lv_screenSetup();
  return lv;
}

/* The reel's own case: a 253 by 84 grid (a 2025 px wide panel at 0.5x) and
 * the load's 127,127 default. x has to come down to 2; y fits and stays. */
int run_lv_camera_load_fits_wide_grid(void) {
  LogViewerState *lv = cameraDecoder(253, 84);
  BYTE ox = 0, oy = 0;
  int sx = -1, sy = -1;

  UT_ASSERT_MSG(lv != NULL, "decoder failed to create");
  lv_screenGetOffsets(&ox, &oy);
  lv_screenGetSubOffset(&sx, &sy);
  UT_ASSERT_MSG(ox == 2, "x offset after load = %u (want 2)", ox);
  UT_ASSERT_MSG(oy == 127, "y offset after load = %u (want 127)", oy);
  UT_ASSERT_MSG(sx == 0 && sy == 0, "sub-pixel pan after load = %d,%d (want 0,0)",
                sx, sy);
  lv_decoderDestroy(lv);
  return 0;
}

/* A highlight jump to a cell on the right of the map. The old cap was 255
 * alone, which left the view's right edge past the map on any grid wider than
 * a few tiles. The partial drag in flight is dropped as well, as it is on a
 * zoom, so the jump lands on whole tiles. */
int run_lv_camera_centre_on_cell_stays_on_map(void) {
  LogViewerState *lv = cameraDecoder(253, 84);
  BYTE ox = 0, oy = 0;
  int sx = -1, sy = -1;

  UT_ASSERT_MSG(lv != NULL, "decoder failed to create");
  lv->logLoaded = TRUE;
  lv_screenSetSubOffset(5, 7);

  lv_screenCentreOnCell(200, 200);
  lv_screenGetOffsets(&ox, &oy);
  lv_screenGetSubOffset(&sx, &sy);
  UT_ASSERT_MSG(ox == 2, "x offset centred on 200 = %u (want 2)", ox);
  UT_ASSERT_MSG(oy == 158, "y offset centred on 200 = %u (want 158)", oy);
  UT_ASSERT_MSG(sx == 0 && sy == 0, "sub-pixel pan after centre = %d,%d (want 0,0)",
                sx, sy);

  lv_screenCentreOnCell(10, 10);
  lv_screenGetOffsets(&ox, &oy);
  UT_ASSERT_MSG(ox == 0 && oy == 0, "offset centred on 10,10 = %u,%u (want 0,0)",
                ox, oy);
  lv_decoderDestroy(lv);
  return 0;
}

/* A grid that grows (the panel widened, or the reel zoomed out) shrinks the
 * range the offset may sit in, and a caller handing the setter a pair past the
 * edge gets it pulled back with the sub-pixel pan on the moved edges zeroed. */
int run_lv_camera_resize_and_set_offset_clamp(void) {
  LogViewerState *lv = cameraDecoder(30, 30);
  BYTE ox = 0, oy = 0;
  int sx = -1, sy = -1;

  UT_ASSERT_MSG(lv != NULL, "decoder failed to create");
  lv_screenGetOffsets(&ox, &oy);
  UT_ASSERT_MSG(ox == 127 && oy == 127,
                "offset on a 30x30 grid = %u,%u (want 127,127)", ox, oy);

  lv_screenSetSizeX(200);
  lv_screenGetOffsets(&ox, &oy);
  UT_ASSERT_MSG(ox == 55, "x offset after widening to 200 = %u (want 55)", ox);
  UT_ASSERT_MSG(oy == 127, "y offset after widening = %u (want 127)", oy);

  lv_screenSetSubOffset(3, 3);
  lv_screenSetOffset(250, 250);
  lv_screenGetOffsets(&ox, &oy);
  lv_screenGetSubOffset(&sx, &sy);
  UT_ASSERT_MSG(ox == 55 && oy == 225,
                "offset after setting 250,250 = %u,%u (want 55,225)", ox, oy);
  UT_ASSERT_MSG(sx == 0 && sy == 0,
                "sub-pixel pan after clamped set = %d,%d (want 0,0)", sx, sy);
  lv_decoderDestroy(lv);
  return 0;
}

/* The standalone viewer's centre-on-click did its arithmetic in BYTE, so a
 * click near the top-left wrapped to the far side of the map and one near the
 * bottom-right ran past column 255. */
int run_lv_camera_mouse_centre_click_stays_on_map(void) {
  LogViewerState *lv = cameraDecoder(253, 84);
  BYTE ox = 0, oy = 0;

  UT_ASSERT_MSG(lv != NULL, "decoder failed to create");
  lv->logLoaded = TRUE;

  /* Column 250 of the view, top row: x would be 2 + 250 - 126 = 126. */
  lv_screenMouseCentreClick(250 * 16, 0);
  lv_screenGetOffsets(&ox, &oy);
  UT_ASSERT_MSG(ox == 2, "x offset after right click = %u (want 2)", ox);
  UT_ASSERT_MSG(oy == 85, "y offset after right click = %u (want 85)", oy);

  /* Top-left corner: x would be 2 - 126, which the BYTE maths wrapped. */
  lv_screenMouseCentreClick(0, 0);
  lv_screenGetOffsets(&ox, &oy);
  UT_ASSERT_MSG(ox == 0, "x offset after corner click = %u (want 0)", ox);
  UT_ASSERT_MSG(oy == 43, "y offset after corner click = %u (want 43)", oy);
  lv_decoderDestroy(lv);
  return 0;
}
