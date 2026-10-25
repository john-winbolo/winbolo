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
#include "lv_event_flash.h"
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

/* The standalone viewer's arrow keys moved the offset one tile with a bare
 * BYTE increment or decrement: left at column 0 jumped the view to column
 * 255, and right at the far edge drew the wrapped borders. Each press now
 * stops at the edge, and a press that has room still moves one tile. */
int run_lv_camera_arrow_nudge_stays_on_map(void) {
  LogViewerState *lv = cameraDecoder(253, 84);
  BYTE ox = 0, oy = 0;

  UT_ASSERT_MSG(lv != NULL, "decoder failed to create");
  lv->logLoaded = TRUE;

  /* The load put x at its far edge, 2. A right press has nowhere to go. */
  lv_screenUpdate(right);
  lv_screenGetOffsets(&ox, &oy);
  UT_ASSERT_MSG(ox == 2, "x offset after right at the edge = %u (want 2)", ox);
  UT_ASSERT_MSG(oy == 127, "y offset after right = %u (want 127)", oy);

  lv_screenUpdate(left);
  lv_screenGetOffsets(&ox, &oy);
  UT_ASSERT_MSG(ox == 1, "x offset after left = %u (want 1)", ox);

  lv_screenSetOffset(0, 0);
  lv_screenUpdate(left);
  lv_screenUpdate(up);
  lv_screenGetOffsets(&ox, &oy);
  UT_ASSERT_MSG(ox == 0 && oy == 0,
                "offset after left and up at the corner = %u,%u (want 0,0)",
                ox, oy);

  lv_screenUpdate(down);
  lv_screenGetOffsets(&ox, &oy);
  UT_ASSERT_MSG(oy == 1, "y offset after down = %u (want 1)", oy);
  lv_decoderDestroy(lv);
  return 0;
}

/* 1 when a map square is inside the view. */
static int cameraShows(int mapX, int mapY) {
  BYTE ox = 0, oy = 0;
  lv_screenGetOffsets(&ox, &oy);
  return mapX >= (int)ox && mapX < (int)ox + lv_screenGetSizeX() &&
         mapY >= (int)oy && mapY < (int)oy + lv_screenGetSizeY();
}

/* A jump to an event puts the event's place in the middle of the view and
 * leaves it there.
 *
 * The fault this pins: after the jump the viewer's window took the view to
 * the camera slot's tank in every mode (as the game view needs), so in the
 * overview the map left the event's place: the place was not centred and
 * the ring round it was drawn off the view. The window's steps are run here
 * as it runs them: centre the place (lv_screenShowCell), then go to the tank
 * only when lvFlashCameraFollowsTank says so. The tank is at 40,40 and the
 * event at 200,180, on a 30 by 30 view (480 map pixels each way). */
int run_lv_event_jump_keeps_place_in_view(void) {
  LogViewerState *lv = cameraDecoder(30, 30);
  const int tankPxX = 40 * 16 - 120, tankPxY = 40 * 16 - 120;
  int shownAfterShow, shownOverview, shownOldWay, movedGameView;
  int gameViewShows, placedOverview, placedDocked;
  BYTE ox = 0, oy = 0, dockOx = 0, dockOy = 0;
  int sx = -1, sy = -1, dockSx = -1, dockSy = -1;

  UT_ASSERT_MSG(lv != NULL, "decoder failed to create");
  lv->logLoaded = TRUE;
  lv->gameView = FALSE;
  lv->centredTank = FALSE;

  /* Overview: the place goes to the middle and stays there. */
  lv_screenCentreOnCell(40, 40);
  placedOverview = lv_screenShowCell(200, 180, 0.0f, 0.0f, 1.0f);
  shownAfterShow = cameraShows(200, 180);
  if (lvFlashCameraFollowsTank(lv->gameView ? 1 : 0)) {
    lv_screenPanToTotalPixels(tankPxX, tankPxY);
  }
  shownOverview = cameraShows(200, 180);
  lv_screenGetOffsets(&ox, &oy);
  lv_screenGetSubOffset(&sx, &sy);

  /* A visible map area of 400 by 300 points at 2x: 200 by 150 map pixels.
     The middle of square 200 is pixel 3208: less 100 is 3108, square 194
     and 4 pixels. Down: 2888 less 75 is 2813, square 175 and 13 pixels. */
  placedDocked = lv_screenShowCell(200, 180, 400.0f, 300.0f, 2.0f);
  lv_screenGetOffsets(&dockOx, &dockOy);
  lv_screenGetSubOffset(&dockSx, &dockSy);

  /* What the window did before: to the tank whatever the mode. The place is
     then outside the view; this is the fault. */
  lv_screenPanToTotalPixels(tankPxX, tankPxY);
  shownOldWay = cameraShows(200, 180);

  /* Game view: the view is the tank's, the jump does not move it, and the
     window puts the camera on the tank. */
  lv->gameView = TRUE;
  movedGameView = lv_screenShowCell(200, 180, 0.0f, 0.0f, 1.0f);
  if (lvFlashCameraFollowsTank(lv->gameView ? 1 : 0)) {
    lv_screenPanToTotalPixels(tankPxX, tankPxY);
  }
  gameViewShows = cameraShows(40, 40);
  lv->gameView = FALSE;

  lv_decoderDestroy(lv);

  UT_ASSERT_MSG(placedOverview == 1 && shownAfterShow,
                "the jump did not bring the event's place into view");
  UT_ASSERT_MSG(shownOverview, "in the overview the view left the event's "
                "place after the jump");
  /* The middle of square 200 is pixel 3208; less half of 480 is 2968:
     square 185 and 8 pixels. Down: 2888 less 240 is 2648: 165 and 8. */
  UT_ASSERT_MSG(ox == 185 && sx == 8 && oy == 165 && sy == 8,
                "the view is at %u+%d, %u+%d (want 185+8, 165+8: the place "
                "in the middle to the pixel)", ox, sx, oy, sy);
  UT_ASSERT_MSG(placedDocked == 1 && dockOx == 194 && dockSx == 4 &&
                dockOy == 175 && dockSy == 13,
                "for a 400 by 300 point area at 2x the view is at %u+%d, "
                "%u+%d (want 194+4, 175+13)", dockOx, dockSx, dockOy, dockSy);
  UT_ASSERT_MSG(!shownOldWay, "the old steps kept the place in view: the test "
                "no longer shows the fault");
  UT_ASSERT_MSG(movedGameView == 0, "the jump moved the game view's camera");
  UT_ASSERT_MSG(gameViewShows, "the game view is not on its tank after the jump");
  return 0;
}
