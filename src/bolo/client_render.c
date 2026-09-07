/*
 * $Id$
 *
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


/*********************************************************
*Name:          ClientRender
*Filename:      client_render.c
*Author:        John Morrison
*Purpose:
*  Per-frame render entry point and small screen/mine-view
*  buffer accessors. Extracted from screen.c so the render
*  pipeline lives in its own translation unit.
*********************************************************/

/* Includes */
#include <stdio.h>

#include "global.h"
#include "client_render.h"
#include "client_sim.h"
#include "client_sim_internal.h"
#include "tank.h"
#include "shells.h"
#include "explosions.h"
#include "tankexp.h"
#include "scroll.h"
#include "screenbullet.h"
#include "screentank.h"
#include "screenlgm.h"
#include "frontend.h"
#include "interpolation.h"
#include "overview_map.h"  /* overviewFogSightGet — is sight being worked out */
#include "sight.h"         /* sightBuildMask — which squares the tank can see */
#include "util.h"

/* Returns TRUE iff the proposed view origin (newXOff, newYOff) leaves
 * the entire tank sprite on screen.
 *
 * The renderer draws everything offset by one tile (originX - tileW),
 * so the back-buffer's leftmost/topmost column is an off-screen border
 * and the first *visible* map column/row is newXOff+1 / newYOff+1. The
 * visible world rectangle is therefore:
 *   x: [(newXOff+1)*256 , (newXOff+1+MAIN_SCREEN_SIZE_X)*256]
 *   y: [(newYOff+1)*256 , (newYOff+1+MAIN_SCREEN_SIZE_Y)*256]
 * in world units (256 per tile).
 *
 * The tank sprite is one tile wide, centred on the tank's world
 * position: it spans [tank-TANK_SUBTRACT, tank+TANK_SUBTRACT]
 * (TANK_SUBTRACT = 128 = half a tile). The check is pixel-precise, so
 * an arrow press that would push any part of the sprite past a visible
 * edge is ignored rather than firing and bouncing back next tick. */
static bool manualScrollKeepsTankOnScreen(ClientSim *csPtr, int newXOff, int newYOff) {
  WORLD tx, ty;
  int viewLeft, viewRight, viewTop, viewBot;

  if (newXOff < 0 || newYOff < 0) return FALSE;
  if (newXOff + MAIN_SCREEN_SIZE_X > 255) return FALSE;
  if (newYOff + MAIN_SCREEN_SIZE_Y > 255) return FALSE;

  tankGetWorld(&MY_TANK(csPtr), &tx, &ty);
  viewLeft  = (newXOff + 1) * 256;
  viewRight = (newXOff + 1 + MAIN_SCREEN_SIZE_X) * 256;
  viewTop   = (newYOff + 1) * 256;
  viewBot   = (newYOff + 1 + MAIN_SCREEN_SIZE_Y) * 256;

  if ((int)tx - TANK_SUBTRACT < viewLeft)  return FALSE;
  if ((int)tx + TANK_SUBTRACT > viewRight) return FALSE;
  if ((int)ty - TANK_SUBTRACT < viewTop)   return FALSE;
  if ((int)ty + TANK_SUBTRACT > viewBot)   return FALSE;

  return TRUE;
}

bool clientRenderCanScroll(ClientSim *csPtr, updateType value) {
  int xOff, yOff;

  if (MY_TANK(csPtr) == NULL) return FALSE;
  /* The same WinBolo-style edge block applies whenever the player is
   * manually scrolling, in both modes:
   *   - autoscroll OFF: manual scrolling is the only follow.
   *   - autoscroll ON:  the scroll keys act as a manual override pan, and
   *     that pan is clamped exactly like classic so it can never push the
   *     tank off-screen (and the input-side smooth-scroll accumulator gets
   *     zeroed at the edge instead of building up against it).
   * The autoscroll auto-follow itself does not go through here, so its
   * behaviour is unaffected. */
  if (clientSimIsInItemView(csPtr)) return TRUE; /* item views scroll freely */

  xOff = (int)clientSimGetXOffset(csPtr);
  yOff = (int)clientSimGetYOffset(csPtr);
  switch (value) {
  case left:  return manualScrollKeepsTankOnScreen(csPtr, xOff - 1, yOff);
  case right: return manualScrollKeepsTankOnScreen(csPtr, xOff + 1, yOff);
  case up:    return manualScrollKeepsTankOnScreen(csPtr, xOff, yOff - 1);
  case down:  return manualScrollKeepsTankOnScreen(csPtr, xOff, yOff + 1);
  default:    return TRUE;
  }
}

/* DEBUG: log every arrow-key pan that sets autoScrollOverRide. Pairs
 * with the OVERRIDE_HOLD / OVERRIDE_CLEAR entries autoscroll.log gets
 * from scroll.c so we can tell whether the producer side is actually
 * firing the flag and whether the engine tick sees it. */
/* Debug file logging. Off for shipping builds — flip to 1 to re-enable
 * the manual_scroll.log trace. When 0 no file is opened or written. */
#define WB_DEBUG_FILE_LOG 0

static FILE *gManScrollLog = NULL;
static bool  gManScrollTried = FALSE;
static int   gManScrollEntries = 0;
static void manScrollLog(const char *dir, ClientSim *cs) {
  if (!WB_DEBUG_FILE_LOG) return;
  if (gManScrollEntries >= 2000) return;
  if (!gManScrollLog && !gManScrollTried) {
    gManScrollTried = TRUE;
    gManScrollLog = fopen("manual_scroll.log", "a");
    if (gManScrollLog) fprintf(gManScrollLog, "--- manual_scroll session start ---\n");
  }
  if (!gManScrollLog) return;
  fprintf(gManScrollLog, "[man] dir=%s view=(%u,%u) override_set=TRUE\n",
          dir,
          (unsigned)clientSimGetXOffset(cs),
          (unsigned)clientSimGetYOffset(cs));
  fflush(gManScrollLog);
  gManScrollEntries++;
}

/* Where the tank can see from the square it is standing on, over the whole
 * back buffer, in the form the view substitutes from. Returns NULL - and
 * builds nothing - while nothing blocks sight, while there is no live tank to
 * look from, or with no memory of what was seen to fall back on; the view then
 * draws every square exactly as it has always drawn it.
 *
 * out and vis belong to the caller and have to outlive the returned pointer.
 * vis is filled as all seen first, so a block sightBuildMask refuses reads as
 * nothing hidden rather than as everything hidden. */
static const ViewSight *clientRenderBuildSight(ClientSim *csPtr, ViewSight *out,
                                               BYTE *vis) {
  BYTE mx; /* The square the tank is standing on */
  BYTE my;
  FogSightMode mode; /* What is blocking sight this tick */
  SightMode rule;    /* Which blockers the walk is to count */

  mode = overviewFogSightGet();
  if (mode == fogSightOff) {
    return NULL;
  }
  rule = (mode == fogSightBuildingsAndTrees) ? sightModeBuildingsAndTrees
                                             : sightModeBuildings;
  if (clientSimGetMyTankMapPos(csPtr, &mx, &my) == FALSE) {
    return NULL;
  }
  memset(out, 0, sizeof(*out));
  out->memory = clientSimGetOverviewMap(csPtr);
  if (out->memory == NULL) {
    return NULL;
  }
  out->block.left = (int)clientSimGetXOffset(csPtr);
  out->block.top = (int)clientSimGetYOffset(csPtr);
  out->block.right = out->block.left + MAIN_BACK_BUFFER_SIZE_X - 1;
  out->block.bottom = out->block.top + MAIN_BACK_BUFFER_SIZE_Y - 1;
  memset(vis, 1, SIGHT_MASK_BYTES);
  sightBuildMask(&clientSimGetGameSim(csPtr)->mp, mx, my, rule, &out->block,
                 vis);
  out->vis = vis;
  return out;
}

/* Whether a back-buffer square is one the player cannot see into. A square off
 * the buffer reads as seen, so nothing is dropped for a square that is not on
 * screen anyway. */
static bool clientRenderSquareHidden(const screenHidden *hidden, BYTE x,
                                     BYTE y) {
  if (hidden == NULL || *hidden == NULL) {
    return FALSE;
  }
  if (x >= MAIN_BACK_BUFFER_SIZE_X || y >= MAIN_BACK_BUFFER_SIZE_Y) {
    return FALSE;
  }
  return (*hidden)->hiddenItem[x][y];
}

/* Drops every tank standing on a square out of sight, the local player's own
 * excepted: it stands on a square it can always see, and its reticle must not
 * blink out if that ever stops being true. The list is an array with a count,
 * so the ones kept are moved down over the ones dropped. */
static void clientRenderDropHiddenTanks(screenTanks *tks,
                                        const screenHidden *hidden,
                                        BYTE myPlayerNum) {
  BYTE kept; /* How many have been written back so far */
  BYTE i;    /* Looping variable */

  kept = 0;
  for (i = 0; i < tks->numTanksScreen; i++) {
    if (tks->pos[i].playerNum != myPlayerNum &&
        clientRenderSquareHidden(hidden, tks->pos[i].mx, tks->pos[i].my) ==
            TRUE) {
      continue;
    }
    if (kept != i) {
      tks->pos[kept] = tks->pos[i];
    }
    kept++;
  }
  tks->numTanksScreen = kept;
}

/* The same for the men, which are a linked list: each one dropped is unlinked
 * and freed the way the list's own destroy frees it. */
static void clientRenderDropHiddenLgms(screenLgm *lgms,
                                       const screenHidden *hidden) {
  screenLgm *link; /* Where the entry being looked at is linked from */
  screenLgm q;     /* That entry */

  link = lgms;
  while (NonEmpty(*link)) {
    q = *link;
    if (clientRenderSquareHidden(hidden, q->mx, q->my) == TRUE) {
      *link = q->next;
      Dispose(q);
    } else {
      link = &q->next;
    }
  }
}

/* And the shells, on the same kind of list. */
static void clientRenderDropHiddenShells(screenBullets *sBullets,
                                         const screenHidden *hidden) {
  screenBullets *link; /* Where the entry being looked at is linked from */
  screenBullets q;     /* That entry */

  link = sBullets;
  while (NonEmpty(*link)) {
    q = *link;
    if (clientRenderSquareHidden(hidden, q->mx, q->my) == TRUE) {
      *link = q->next;
      Dispose(q);
    } else {
      link = &q->next;
    }
  }
}

/*********************************************************
*NAME:          clientRenderFrame
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 21/01/01
*PURPOSE:
*  Updates the screen. Takes numerous directions
*
*ARGUMENTS:
*  value - Pointer to the starts structure
*********************************************************/
void clientRenderFrame(ClientSim *csPtr, updateType value) {
  static bool b = FALSE;
  screenGunsight gs;
  screenBullets sBullets;
  screenLgm lgms;
  screenTanks scnTnk;
  BYTE gsX;
  netStatus ns;
  BYTE oldXOffset = clientSimGetXOffset(csPtr);
  BYTE oldYOffset = clientSimGetYOffset(csPtr);

  if (b == TRUE) {
    return;
  }
  ns = clientSimGetNetStatus(csPtr);
  if (ns != netRunning && ns != netFailed) {
    /* netStat=netLobby with inLobby=false is the brief post-game window
     * between CTRL_GAME_PHASE_GAME_OVER and inLobby flipping. Falling
     * through to frontEndDrawDownload here used to paint the download
     * progress bar at 100%, which presented as a multi-second white
     * playfield. Separate path renders a "Returning to lobby" caption
     * over a black playfield instead. */
    if ((ns == netLobby || ns == netLobbyCountdown) && !clientSimIsInLobby(csPtr)) {
      frontEndDrawReturningToLobby(csPtr);
    } else {
      frontEndDrawDownload(csPtr, FALSE);
    }
    return;
  }
  if (clientSimGetGameSim(csPtr)->inStartFind == TRUE) {
    frontEndDrawDownload(csPtr, TRUE);
    return;
  }
  if (clientSimIsNeedScreenReCalc(csPtr) == TRUE) {
    ViewSight sight;                  /* What the tank can see from where it is */
    BYTE sightMask[SIGHT_MASK_BYTES]; /* One byte a square of the back buffer */

    clientSimSetNeedScreenReCalc(csPtr, FALSE);
    clientSimUpdateView(csPtr, (updateType) 0,
                        clientRenderBuildSight(csPtr, &sight, sightMask));
  }

  b = TRUE;

  if (MY_TANK(csPtr) == NULL) {
    b = FALSE;
    return;
  }

  screenLgmCreate(&lgms);
  sBullets = screenBulletsCreate();
  screenTanksCreate(&scnTnk);


  switch (value) {
  case redraw:
    break;
  case left:
    if (clientSimIsInItemView(csPtr) == FALSE) {
      if (manualScrollKeepsTankOnScreen(csPtr, (int)clientSimGetXOffset(csPtr) - 1, clientSimGetYOffset(csPtr))) {
        clientSimSetXOffset(csPtr, clientSimGetXOffset(csPtr) - 1);
        if (clientSimGetXOffset(csPtr) != oldXOffset) {
          clientSimSetCursorPosX(csPtr, clientSimGetCursorPosX(csPtr) + 1);
        }
        clientSimGetScroll(csPtr)->autoScrollOverRide = TRUE;
        clientSimGetScroll(csPtr)->subPosX = 0;
        clientSimGetScroll(csPtr)->subPosY = 0;
        manScrollLog("left", csPtr);
      }
    } else {
      clientSimStepView(csPtr, -1, 0);
    }
    clientSimSetNeedScreenReCalc(csPtr, TRUE);
    break;
  case right:
    if (clientSimIsInItemView(csPtr) == FALSE) {
      if (manualScrollKeepsTankOnScreen(csPtr, (int)clientSimGetXOffset(csPtr) + 1, clientSimGetYOffset(csPtr))) {
        clientSimSetXOffset(csPtr, clientSimGetXOffset(csPtr) + 1);
        if (clientSimGetXOffset(csPtr) != oldXOffset) {
          clientSimSetCursorPosX(csPtr, clientSimGetCursorPosX(csPtr) - 1);
        }
        clientSimGetScroll(csPtr)->autoScrollOverRide = TRUE;
        clientSimGetScroll(csPtr)->subPosX = 0;
        clientSimGetScroll(csPtr)->subPosY = 0;
        manScrollLog("right", csPtr);
      }
    } else {
      clientSimStepView(csPtr, 1, 0);
    }
    clientSimSetNeedScreenReCalc(csPtr, TRUE);
    break;
  case up:
    if (clientSimIsInItemView(csPtr) == FALSE) {
      if (manualScrollKeepsTankOnScreen(csPtr, clientSimGetXOffset(csPtr), (int)clientSimGetYOffset(csPtr) - 1)) {
        clientSimSetYOffset(csPtr, clientSimGetYOffset(csPtr) - 1);
        if (clientSimGetYOffset(csPtr) != oldYOffset) {
          clientSimSetCursorPosY(csPtr, clientSimGetCursorPosY(csPtr) + 1);
        }
        clientSimGetScroll(csPtr)->autoScrollOverRide = TRUE;
        clientSimGetScroll(csPtr)->subPosX = 0;
        clientSimGetScroll(csPtr)->subPosY = 0;
        manScrollLog("up", csPtr);
      }
    } else {
      clientSimStepView(csPtr, 0, -1);
    }
    clientSimSetNeedScreenReCalc(csPtr, TRUE);
    break;
  case down:
  default:
    if (clientSimIsInItemView(csPtr) == FALSE) {
      if (manualScrollKeepsTankOnScreen(csPtr, clientSimGetXOffset(csPtr), (int)clientSimGetYOffset(csPtr) + 1)) {
        clientSimSetYOffset(csPtr, clientSimGetYOffset(csPtr) + 1);
        if (clientSimGetYOffset(csPtr) != oldYOffset) {
          clientSimSetCursorPosY(csPtr, clientSimGetCursorPosY(csPtr) - 1);
        }
        clientSimGetScroll(csPtr)->autoScrollOverRide = TRUE;
        clientSimGetScroll(csPtr)->subPosX = 0;
        clientSimGetScroll(csPtr)->subPosY = 0;
        manScrollLog("down", csPtr);
      }
    } else {
      clientSimStepView(csPtr, 0, 1);
    }
    clientSimSetNeedScreenReCalc(csPtr, TRUE);
    break;
  }


  if (value == redraw) {
    screenTanksPrepare(csPtr, &scnTnk, &MY_TANK(csPtr), clientSimGetXOffset(csPtr), (BYTE) (clientSimGetXOffset(csPtr) + MAIN_BACK_BUFFER_SIZE_X), clientSimGetYOffset(csPtr), (BYTE) (clientSimGetYOffset(csPtr) + MAIN_BACK_BUFFER_SIZE_Y));
    /* Render-only error smoothing: redraw the own-tank hull (slot 0 when
     * on screen) at the smoothed rendered pose so reconciliation
     * corrections slide instead of snapping. screenTanksPrepare only puts
     * the local tank in slot 0, so a slot-0 entry whose playerNum is ours
     * is unambiguously the own hull. With err == 0 the rendered pose
     * equals the sim pose, so this is byte-identical to the raw prepare. */
    if (scnTnk.numTanksScreen >= 1 &&
        scnTnk.pos[0].playerNum == clientSimGetMyPlayerNum(csPtr)) {
      WORLD rwx, rwy;
      float rang;
      WORLD cx, cy;
      clientSimGetRenderedTankPos(csPtr, &rwx, &rwy, &rang);
      /* Same shift math as tankGetScreenMX/PX/MY/PY. */
      cx = (WORLD)(rwx - TANK_SUBTRACT);
      cy = (WORLD)(rwy - TANK_SUBTRACT);
      scnTnk.pos[0].mx = (BYTE)((cx >> TANK_SHIFT_MAPSIZE) - clientSimGetXOffset(csPtr));
      scnTnk.pos[0].my = (BYTE)((cy >> TANK_SHIFT_MAPSIZE) - clientSimGetYOffset(csPtr));
      scnTnk.pos[0].px = (BYTE)((WORLD)(cx << TANK_SHIFT_MAPSIZE) >> TANK_SHIFT_PIXELSIZE);
      scnTnk.pos[0].py = (BYTE)((WORLD)(cy << TANK_SHIFT_MAPSIZE) >> TANK_SHIFT_PIXELSIZE);
      /* Low byte of the same shifted co-ord, so wx >> 4 is px. */
      scnTnk.pos[0].wx = (BYTE)cx;
      scnTnk.pos[0].wy = (BYTE)cy;
      scnTnk.pos[0].angle = (BYTE)rang;
      scnTnk.pos[0].frame = tankGetFrameAt(&MY_TANK(csPtr), rang);
    }
    screenLgmPrepare(csPtr, &lgms, clientSimGetXOffset(csPtr), (BYTE) (clientSimGetXOffset(csPtr) + MAIN_BACK_BUFFER_SIZE_X-1 ), clientSimGetYOffset(csPtr), (BYTE) (clientSimGetYOffset(csPtr) + MAIN_BACK_BUFFER_SIZE_Y-1));
    if (tankIsGunsightShow(&MY_TANK(csPtr)) == TRUE) {
      WORLD gsrx, gsry;
      float gsra;
      clientSimGetRenderedTankPos(csPtr, &gsrx, &gsry, &gsra);
      tankGetGunsightAt(&MY_TANK(csPtr), gsrx, gsry, gsra, &gsX, &(gs.mapY), &(gs.pixelX), &(gs.pixelY));
      gs.mapX = gsX;

      if (gs.mapX >= clientSimGetXOffset(csPtr) && gs.mapX < (clientSimGetXOffset(csPtr) + MAIN_BACK_BUFFER_SIZE_X-1) && gs.mapY >= clientSimGetYOffset(csPtr) && gs.mapY < (clientSimGetYOffset(csPtr) + MAIN_BACK_BUFFER_SIZE_Y -1 )) {
        gs.mapX -= clientSimGetXOffset(csPtr);
        gs.mapY -= clientSimGetYOffset(csPtr);
      } else {
        gs.mapX = NO_GUNSIGHT;
      }
    } else {
      gs.mapX = NO_GUNSIGHT;
    }

    /* Other players' shells. Humans draw the forward-projected layer so
     * incoming shells appear at their true present position rather than
     * ~RTT/2 in the past; bots (e.g. BrainTest overlay) draw the raw
     * serverShellSnaps the brain perceives. Own shells are filtered during
     * snapshot sync, so neither source contains them. */
    if (clientSimIsBot(csPtr)) {
      int si;
      BYTE myPlayer = clientSimGetInterpCtx(csPtr)->localPlayer;
      for (si = 0; si < clientSimGetServerShellCount(csPtr); si++) {
        BYTE smx = (BYTE)(clientSimGetServerShellSnaps(csPtr)[si].worldX >> TANK_SHIFT_MAPSIZE);
        BYTE smy = (BYTE)(clientSimGetServerShellSnaps(csPtr)[si].worldY >> TANK_SHIFT_MAPSIZE);
        if (clientSimGetServerShellSnaps(csPtr)[si].owner == myPlayer) {
          continue;
        }
        if (smx >= clientSimGetXOffset(csPtr) && smx < (BYTE)(clientSimGetXOffset(csPtr) + MAIN_BACK_BUFFER_SIZE_X - 1) &&
            smy >= clientSimGetYOffset(csPtr) && smy < (BYTE)(clientSimGetYOffset(csPtr) + MAIN_BACK_BUFFER_SIZE_Y - 1)) {
          WORLD conv;
          BYTE spx, spy, swx, swy, sframe;
          swx = (BYTE)clientSimGetServerShellSnaps(csPtr)[si].worldX;
          swy = (BYTE)clientSimGetServerShellSnaps(csPtr)[si].worldY;
          conv = clientSimGetServerShellSnaps(csPtr)[si].worldX;
          conv <<= TANK_SHIFT_MAPSIZE;
          conv >>= TANK_SHIFT_PIXELSIZE;
          spx = (BYTE)conv;
          conv = clientSimGetServerShellSnaps(csPtr)[si].worldY;
          conv <<= TANK_SHIFT_MAPSIZE;
          conv >>= TANK_SHIFT_PIXELSIZE;
          spy = (BYTE)conv;
          sframe = (BYTE)(utilGetDir((TURNTYPE)clientSimGetServerShellSnaps(csPtr)[si].angle) + SHELL_START_EXPLODE + 1);
          screenBulletsAddItem(&sBullets, (BYTE)(smx - clientSimGetXOffset(csPtr)), (BYTE)(smy - clientSimGetYOffset(csPtr)), spx, spy, sframe, swx, swy);
        }
      }
    } else {
      int si;
      BYTE myPlayer = clientSimGetInterpCtx(csPtr)->localPlayer;
      for (si = 0; si < clientSimGetProjectedShellCount(csPtr); si++) {
        const ProjectedShell *ps = &clientSimGetProjectedShells(csPtr)[si];
        WORLD sx = (WORLD)(int)ps->fx;
        WORLD sy = (WORLD)(int)ps->fy;
        BYTE smx = (BYTE)(sx >> TANK_SHIFT_MAPSIZE);
        BYTE smy = (BYTE)(sy >> TANK_SHIFT_MAPSIZE);
        if (ps->owner == myPlayer) {
          continue;
        }
        if (smx >= clientSimGetXOffset(csPtr) && smx < (BYTE)(clientSimGetXOffset(csPtr) + MAIN_BACK_BUFFER_SIZE_X - 1) &&
            smy >= clientSimGetYOffset(csPtr) && smy < (BYTE)(clientSimGetYOffset(csPtr) + MAIN_BACK_BUFFER_SIZE_Y - 1)) {
          WORLD conv;
          BYTE spx, spy, sframe;
          conv = sx;
          conv <<= TANK_SHIFT_MAPSIZE;
          conv >>= TANK_SHIFT_PIXELSIZE;
          spx = (BYTE)conv;
          conv = sy;
          conv <<= TANK_SHIFT_MAPSIZE;
          conv >>= TANK_SHIFT_PIXELSIZE;
          spy = (BYTE)conv;
          sframe = (BYTE)(utilGetDir((TURNTYPE)ps->angle) + SHELL_START_EXPLODE + 1);
          screenBulletsAddItem(&sBullets, (BYTE)(smx - clientSimGetXOffset(csPtr)), (BYTE)(smy - clientSimGetYOffset(csPtr)), spx, spy, sframe, (BYTE)sx, (BYTE)sy);
        }
      }
    }
    /* Client-predicted shells (local player only) */
    {
      int pi;
      for (pi = 0; pi < clientSimGetPredictedShellCount(csPtr); pi++) {
        const PredictedShell *ps = &clientSimGetPredictedShells(csPtr)[pi];
        /* Don't render shells that have expired — prevents ghost frame alongside explosion */
        if (ps->length <= SHELL_DEATH) continue;
        BYTE pmx = (BYTE)(ps->x >> TANK_SHIFT_MAPSIZE);
        BYTE pmy = (BYTE)(ps->y >> TANK_SHIFT_MAPSIZE);
        if (pmx >= clientSimGetXOffset(csPtr) && pmx < (BYTE)(clientSimGetXOffset(csPtr) + MAIN_BACK_BUFFER_SIZE_X - 1) &&
            pmy >= clientSimGetYOffset(csPtr) && pmy < (BYTE)(clientSimGetYOffset(csPtr) + MAIN_BACK_BUFFER_SIZE_Y - 1)) {
          WORLD conv;
          BYTE ppx, ppy, pframe;
          conv = ps->x;
          conv <<= TANK_SHIFT_MAPSIZE;
          conv >>= TANK_SHIFT_PIXELSIZE;
          ppx = (BYTE)conv;
          conv = ps->y;
          conv <<= TANK_SHIFT_MAPSIZE;
          conv >>= TANK_SHIFT_PIXELSIZE;
          ppy = (BYTE)conv;
          pframe = (BYTE)(utilGetDir(ps->angle) + SHELL_START_EXPLODE + 1);
          screenBulletsAddItem(&sBullets, (BYTE)(pmx - clientSimGetXOffset(csPtr)), (BYTE)(pmy - clientSimGetYOffset(csPtr)), ppx, ppy, pframe, (BYTE)ps->x, (BYTE)ps->y);
        }
      }
    }
    /* Nothing moving is drawn on a square the player cannot see into. The view
     * fill above decided which squares those are; with sight off none of them
     * is, and the three lists go through untouched. */
    if (overviewFogSightGet() != fogSightOff) {
      const screenHidden *hidden = clientSimGetHiddenView(csPtr);
      clientRenderDropHiddenTanks(&scnTnk, hidden,
                                  clientSimGetMyPlayerNum(csPtr));
      clientRenderDropHiddenLgms(&lgms, hidden);
      clientRenderDropHiddenShells(&sBullets, hidden);
    }
    explosionsCalcScreenBullets(&clientSimGetGameSim(csPtr)->expl, &sBullets, clientSimGetXOffset(csPtr), (BYTE) (clientSimGetXOffset(csPtr) + MAIN_BACK_BUFFER_SIZE_X-1), clientSimGetYOffset(csPtr), (BYTE) (clientSimGetYOffset(csPtr) + MAIN_BACK_BUFFER_SIZE_Y-1));
    tkExplosionCalcScreenBullets(&clientSimGetGameSim(csPtr)->tankExplosions, &sBullets, clientSimGetXOffset(csPtr), (BYTE) (clientSimGetXOffset(csPtr) + MAIN_BACK_BUFFER_SIZE_X-1), clientSimGetYOffset(csPtr), (BYTE) (clientSimGetYOffset(csPtr) + MAIN_BACK_BUFFER_SIZE_Y-1));
    frontEndDrawMainScreen(csPtr, clientSimGetView(csPtr), clientSimGetMineView(csPtr), &scnTnk, &gs, &sBullets, &lgms, clientSimGetGmeStartDelay(csPtr), clientSimIsInItemView(csPtr), 0, 0);
  }
  screenBulletsDestroy(&sBullets);
  screenLgmDestroy(&lgms);
  screenTanksDestroy(&scnTnk);
  b = FALSE;
}


/*********************************************************
*NAME:          screenGetPos
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 30/12/2008
*PURPOSE:
*  Gets the value of a square in the structure
*  Return RIVER if out of range
*
*ARGUMENTS:
*  value  - Pointer to the screen structure
*  xValue - The X co-ordinate
*  yValue - The Y co-ordinate
*********************************************************/
BYTE screenGetPos(const screen *value,BYTE xValue, BYTE yValue) {
  BYTE returnValue = RIVER; /* Value to return */

  if (xValue < MAIN_BACK_BUFFER_SIZE_X && yValue < MAIN_BACK_BUFFER_SIZE_Y) {
    returnValue = (*value)->screenItem[xValue][yValue];
  }
  return returnValue;
}

/*********************************************************
*NAME:          screenIsMine
*AUTHOR:        John Morrison
*CREATION DATE: 6/11/98
*LAST MODIFIED: 6/11/98
*PURPOSE:
*  Returns if a square on the screen should have a mine
*  drawn on it.
*  If value is out of range returns FALSE
*
*ARGUMENTS:
*  value  - Pointer to the screenMines structure
*  xValue - The X co-ordinate
*  yValue - The Y co-ordinate
*********************************************************/
bool screenIsMine(const screenMines *value,BYTE xValue, BYTE yValue) {
  bool returnValue = FALSE; /* Value to return */

  if (xValue <= MAIN_SCREEN_SIZE_X && yValue <= MAIN_SCREEN_SIZE_Y) {
    returnValue = ((*value)->mineItem[xValue][yValue]);
  }
  return returnValue;
}
