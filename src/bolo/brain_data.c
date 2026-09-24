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

/*********************************************************
 *Name:          Brain Data
 *Filename:      brain_data.c
 *Purpose:
 *  Builds the data the Lua brain reads each tick — terrain
 *  view buffer, BrainInfo struct, brain-object list — and
 *  reads decisions back out. Pure data shaping; no I/O,
 *  rendering, or network code.
 *
 *  Contents (declared in brain_data.h):
 *    brainDataMakeViewData  - terrain rect for the brain's view
 *    brainDataMakeInfo      - populate BrainInfo before think()
 *    brainDataExtractInfo   - read brain decisions back, free buffers
 *    brainDataAddObject     - append an object to the brain object list
 *
 *  Companion file: client_snapshot.c (server snapshot apply +
 *  input packet building).
 *********************************************************/

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <SDL3/SDL.h>

#include "../common/wb_log.h"
#include "global.h"
#include "bolo_map.h"
#include "pillbox.h"
#include "bases.h"
#include "starts.h"
#include "tank.h"
#include "shells.h"
#include "rubble.h"
#include "explosions.h"
#include "screenbullet.h"
#include "frontend.h"
#include "../gui/lang.h"
#include "sounddist.h"
#include "messages.h"
#include "grass.h"
#include "swamp.h"
#include "building.h"
#include "tilenum.h"
#include "tankexp.h"
#include "scroll.h"
#include "lgm.h"
#include "log.h"
#include "floodfill.h"
#include "minesexp.h"
#include "treegrow.h"
#include "mines.h"
#include "labels.h"
#include "players.h"
#include "screenbrainmap.h"
#include "brain_data.h"
#include "client_state.h"
#include "interpolation.h"
#include "util.h"
#include "client_sim.h"
#include "client_sim_internal.h"
#include "server_sim.h"
#include "bot_manager.h"
#include "../steam/steam_wrapper.h"

/*********************************************************
*NAME:          brainDataMakeViewData
*AUTHOR:        John Morrison
*CREATION DATE: 25/11/99
*LAST MODIFIED: 26/11/99
*PURPOSE:
*  Makes the view information including base and pills
*  for the brain.
*
*ARGUMENTS:
*  buff      - Pointer to the buffer to hold the data
*  leftPos   - left position on the map to get data from
*  rightPos  - Right position on the map to get data from
*  topPos    - top position on the map to get data from
*  bottomPos - bottom position on the map to get data from
*********************************************************/
void brainDataMakeViewData(ClientSim *cs, BYTE *buff, BYTE leftPos, BYTE rightPos, BYTE topPos, BYTE bottomPos) {
  int count1;  /* Looping variable */
  int count2;  /* Looping variable */
  int pos;     /* Upto position    */
  GameSim *gs = clientSimGetGameSim(cs);

  /* The counters are int because both bounds are inclusive and a rect that
   * reaches the map edge arrives here as 255: brainDataMakeInfo clamps the
   * 29x29 window's origin to 227, so a tank on map row 241 or below is passed
   * bottomPos == 255, and a BYTE counter tested `<= 255` holds for every value
   * it can take — the loop never ended. The far column did the same to the
   * inner loop.
   *
   * Play never puts a tank out there; the reachable part of a map stops well
   * short of the edge. What reaches it is the brain's FIRST pass, which runs
   * before the tank has an authoritative position. A client that creates its
   * tank before its map has arrived has no starts list, startsGetStart returns
   * without writing, and tankCreate places the tank on the uninitialised
   * locals it passed in. That is leftover stack, so which square it names
   * depends on what ran before: about one join in eight was out at the edge
   * under the baseline harness, every join under a debugger. A client that
   * drew one spun at 100% of a core inside brainsHandlerStart rather than
   * reaching its game loop, socket unread, deaf to the server leaving.
   *
   * pos is an int for a duller reason: the rect is view_width by view_height
   * squares, 841 of them for the tank window, and a BYTE index wrapped at 256
   * so five sixths of the buffer kept the zeros memset put there while the
   * first 256 bytes were overwritten three times over. */
  pos = 0;
  for (count1 = topPos; count1 <= bottomPos; count1++) {
    for (count2 = leftPos; count2 <= rightPos; count2++) {
      if (basesExistPos(&gs->bs, (BYTE)count2, (BYTE)count1) == TRUE) {
        buff[pos] = BREFBASE_T;
      } else if (pillsViewExistPos(&gs->pb, (BYTE)count2, (BYTE)count1) == TRUE) {
        /* The brain's view of the map is the bot's screen, so it shows a pill
         * at the square it was last seen on, exactly as a human's does. The
         * bot's movement still asks pillsExistPos through mapGetSpeed and
         * friends, so it is no more blocked by one than a human is. */
        buff[pos] = BPILLBOX_T;
      } else {
        buff[pos] = mapGetPos(&gs->mp, (BYTE)count2, (BYTE)count1);
        if (buff[pos] == DEEP_SEA) {
          buff[pos] = BDEEPSEA;
        } else if (buff[pos] >= MINE_START && buff[pos] <= MINE_END) {
          buff[pos] = buff[pos] - MINE_SUBTRACT;
        }
        if (minesExistPos(&gs->mns, &gs->mp, (BYTE)count2, (BYTE)count1) == TRUE) {
          buff[pos] |= TERRAIN_MINE;
        }
      }
      pos++;
    }
  }
}
/* Combined brain view rects: [0] is the tank-centered 29x29 window, then
 * one 15x15 (+/-7 — Bolo's pill-view size) per DEPLOYED team pillbox (own
 * or allied). Brains at EVERY ai level get the pill rects, as if watching
 * all their pill views simultaneously. Consumed by the brain event filter,
 * the pill-view object sweep, and the bot shell mirror in
 * brainDataMakeInfo. Terrain viewdata stays tank-centered. */
typedef struct {
  BYTE left;
  BYTE right;
  BYTE top;
  BYTE bottom;
} BrainViewRect;
#define BRAIN_VIEW_MAX_RECTS (MAX_PILLS + 1)

static bool brainViewRectsContain(const BrainViewRect *rects, int numRects, BYTE mx, BYTE my) {
  int i;
  for (i = 0; i < numRects; i++) {
    if (mx >= rects[i].left && mx <= rects[i].right
        && my >= rects[i].top && my <= rects[i].bottom) {
      return TRUE;
    }
  }
  return FALSE;
}

/*********************************************************
*NAME:          brainDataMakeInfo
*AUTHOR:        John Morrison
*CREATION DATE: 25/11/99
*LAST MODIFIED: 28/11/99
*PURPOSE:
*  Makes the information for a particular brain pass
*
*ARGUMENTS:
*  value - Pointer to the brain info structure
*  first - TRUE if this is the first time we have been
*          called
*********************************************************/
void brainDataMakeInfo(ClientSim *csPtr, BrainInfo *value, bool first, aiType aiMode) {
  BYTE tx;        /* Tank X and Y Co-ordinates */
  BYTE ty;
  BYTE closeBase; /* The closest base to our current position */
  GameSim *gs = clientSimGetGameSim(csPtr);
  BrainViewRect viewRects[BRAIN_VIEW_MAX_RECTS];
  int numViewRects;


  if (MY_TANK(csPtr) == NULL) {
    return;
  }

  tx = tankGetMX(&MY_TANK(csPtr));
  ty = tankGetMY(&MY_TANK(csPtr));

  /* Build the view-rect list: tank window first, then every deployed team
   * pill's view. Clamped at map edges (BYTE wrap would invert the rect). */
  numViewRects = 0;
  viewRects[numViewRects].left   = (BYTE)((tx >= 14) ? tx - 14 : 0);
  viewRects[numViewRects].right  = (BYTE)((tx <= 241) ? tx + 14 : 255);
  viewRects[numViewRects].top    = (BYTE)((ty >= 14) ? ty - 14 : 0);
  viewRects[numViewRects].bottom = (BYTE)((ty <= 241) ? ty + 14 : 255);
  numViewRects++;
  {
    BYTE myPN  = clientSimGetMyPlayerNum(csPtr);
    BYTE numPb = pillsGetNumPills(&gs->pb);
    BYTE pi;
    for (pi = 0; pi < numPb && numViewRects < BRAIN_VIEW_MAX_RECTS; pi++) {
      if ((*gs->pb).item[pi].inTank == FALSE
          && (*gs->pb).item[pi].owner < MAX_TANKS
          && ((*gs->pb).item[pi].owner == myPN
              || playersIsAllie(&gs->plyrs, myPN, (*gs->pb).item[pi].owner) == TRUE)) {
        BYTE px = (*gs->pb).item[pi].x;
        BYTE py = (*gs->pb).item[pi].y;
        viewRects[numViewRects].left   = (BYTE)((px >= 7) ? px - 7 : 0);
        viewRects[numViewRects].right  = (BYTE)((px <= 248) ? px + 7 : 255);
        viewRects[numViewRects].top    = (BYTE)((py >= 7) ? py - 7 : 0);
        viewRects[numViewRects].bottom = (BYTE)((py <= 248) ? py + 7 : 255);
        numViewRects++;
      }
    }
  }

  /* Dead-tick hook: TRUE while the tank is waiting to respawn. The think is
   * still invoked (so the brain can reset its own state for a clean respawn)
   * but its outputs are ignored by the caller. */
  value->dead = (tankGetDeathWait(&MY_TANK(csPtr)) > 0) ? TRUE : FALSE;

  /* Max's */
  value->max_players = MAX_TANKS;//-1; /* FIXME: Huh? */
  value->max_refbases = basesGetNumBases(&gs->bs);//-1;
  value->max_pillboxes = pillsGetNumPills(&gs->pb);//-1;
  value->player_number = clientSimGetMyPlayerNum(csPtr);
  value->num_players = playersGetNumPlayers(&gs->plyrs);
  value->playernames = playersGetBrainsNamesArray(&gs->plyrs);
  value->allies = malloc(sizeof(PlayerBitMap));
  *(value->allies) = playersGetAlliesBitMap(&gs->plyrs, clientSimGetMyPlayerNum(csPtr));

  /* Per-slot PLAYER_FLAG_BOT bitmap. Brains pick out allied humans
   * via (allies & ~player_bots) — needed for the human-only chat
   * path where the internal bot-coordination slate would otherwise
   * spam every teammate's chat panel. */
  value->player_bots = malloc(sizeof(PlayerBitMap));
  {
    PlayerBitMap botBits = 0;
    BYTE i;
    for (i = 0; i < MAX_TANKS; i++) {
      if (playersGetClientFlags(&gs->plyrs, i) & PLAYER_FLAG_BOT) {
        botBits |= ((PlayerBitMap)1u << i);
      }
    }
    *(value->player_bots) = botBits;
  }

  /* Tank */
  tankGetWorld(&MY_TANK(csPtr), &(value->tankx), &(value->tanky));
  value->direction = tankGet256Dir(&MY_TANK(csPtr));
  /* Float tank angle for sub-brad-precision brains. tank->angle is
   * the float the engine fires shells at; `direction` above floors
   * it for legacy BYTE consumers. */
  value->tank_angle = (MY_TANK(csPtr) != NULL) ? (float)MY_TANK(csPtr)->angle : 0.0f;
  value->speed = (BYTE) (tankGetSpeed(&MY_TANK(csPtr)) * 4);
  value->inboat = tankIsOnBoat(&MY_TANK(csPtr));
  value->hidden = utilIsTankInTrees(&gs->mp, &gs->pb, &gs->bs, value->tankx, value->tanky);

  tankGetStats(&MY_TANK(csPtr), &(value->shells), &(value->mines), &(value->armour), &(value->trees));
  value->destroyed = tankIsDestroyed(&MY_TANK(csPtr)) ? TRUE : FALSE;

  /* The rules view: the gameplay numbers this sim is running on, copied
     once per tick so a brain reads the same values the engine acts on
     rather than the classic ones it used to be able to assume. */
  value->rules.tank_reload_ticks  = gs->rules.tank_reload_ticks;
  value->rules.tank_full_shells   = gs->rules.tank_full_shells;
  value->rules.tank_full_mines    = gs->rules.tank_full_mines;
  value->rules.tank_full_trees    = gs->rules.tank_full_trees;
  value->rules.tank_full_armour   = gs->rules.tank_full_armour;
  value->rules.tank_death_ticks   = gs->rules.tank_death_ticks;
  value->rules.tank_water_ticks   = gs->rules.tank_water_ticks;
  value->rules.mine_damage        = gs->rules.mine_damage;
  value->rules.just_fired_ticks   = gs->rules.just_fired_ticks;
  value->rules.tank_min_move      = gs->rules.tank_min_move;
  value->rules.tank_accel_rate    = gs->rules.tank_accel_rate;
  value->rules.tank_decel_rate    = gs->rules.tank_decel_rate;
  value->rules.tank_brake_rate    = gs->rules.tank_brake_rate;
  value->rules.tank_autoslow_rate = gs->rules.tank_autoslow_rate;
  value->rules.speed_road         = gs->rules.speed_road;
  value->rules.speed_grass        = gs->rules.speed_grass;
  value->rules.speed_forest       = gs->rules.speed_forest;
  value->rules.speed_river        = gs->rules.speed_river;
  value->rules.speed_swamp        = gs->rules.speed_swamp;
  value->rules.speed_crater       = gs->rules.speed_crater;
  value->rules.speed_rubble       = gs->rules.speed_rubble;
  value->rules.speed_boat         = gs->rules.speed_boat;
  value->rules.speed_deep_sea     = gs->rules.speed_deep_sea;
  value->rules.speed_refuel_base  = gs->rules.speed_refuel_base;
  value->rules.turn_road          = gs->rules.turn_road;
  value->rules.turn_grass         = gs->rules.turn_grass;
  value->rules.turn_forest        = gs->rules.turn_forest;
  value->rules.turn_river         = gs->rules.turn_river;
  value->rules.turn_swamp         = gs->rules.turn_swamp;
  value->rules.turn_crater        = gs->rules.turn_crater;
  value->rules.turn_rubble        = gs->rules.turn_rubble;
  value->rules.turn_boat          = gs->rules.turn_boat;
  value->rules.turn_deep_sea      = gs->rules.turn_deep_sea;
  value->rules.turn_refuel_base   = gs->rules.turn_refuel_base;
  value->rules.pill_max_armour       = gs->rules.pill_max_armour;
  value->rules.pill_attack_ticks     = gs->rules.pill_attack_ticks;
  value->rules.pill_attack_min_ticks = gs->rules.pill_attack_min_ticks;
  value->rules.base_full_armour      = gs->rules.base_full_armour;
  value->rules.base_full_shells      = gs->rules.base_full_shells;
  value->rules.base_full_mines       = gs->rules.base_full_mines;
  value->rules.shell_damage          = gs->rules.shell_damage;
  value->rules.shell_life            = gs->rules.shell_life;
  value->rules.shell_speed           = gs->rules.shell_speed;
  value->rules.gunsight_max          = gs->rules.gunsight_max;
  value->rules.tree_hide_distance    = gs->rules.tree_hide_distance;
  value->rules.pill_cooldown_ticks   = gs->rules.pill_cooldown_ticks;
  value->rules.pill_repair_amount    = gs->rules.pill_repair_amount;
  value->rules.pill_range            = gs->rules.pill_range;
  value->rules.pill_shell_damage     = gs->rules.pill_shell_damage;
  value->rules.pill_angry_divisor    = gs->rules.pill_angry_divisor;
  value->rules.base_capture_armour   = gs->rules.base_capture_armour;
  value->rules.base_hit_armour       = gs->rules.base_hit_armour;
  value->rules.base_regen_ticks      = gs->rules.base_regen_ticks;
  value->rules.lgm_build_ticks       = gs->rules.lgm_build_ticks;
  value->rules.lgm_cost_road         = gs->rules.lgm_cost_road;
  value->rules.lgm_cost_building     = gs->rules.lgm_cost_building;
  value->rules.lgm_cost_pill_repair  = gs->rules.lgm_cost_pill_repair;
  value->rules.lgm_cost_boat         = gs->rules.lgm_cost_boat;
  value->rules.lgm_cost_pill_new     = gs->rules.lgm_cost_pill_new;
  value->rules.lgm_cost_mine         = gs->rules.lgm_cost_mine;
  value->rules.lgm_gather_trees      = gs->rules.lgm_gather_trees;
  value->rules.lgm_helicopter_speed  = gs->rules.lgm_helicopter_speed;
  value->rules.man_speed_road        = gs->rules.man_speed_road;
  value->rules.man_speed_grass       = gs->rules.man_speed_grass;
  value->rules.man_speed_forest      = gs->rules.man_speed_forest;
  value->rules.man_speed_river       = gs->rules.man_speed_river;
  value->rules.man_speed_swamp       = gs->rules.man_speed_swamp;
  value->rules.man_speed_crater      = gs->rules.man_speed_crater;
  value->rules.man_speed_rubble      = gs->rules.man_speed_rubble;
  value->rules.man_speed_boat        = gs->rules.man_speed_boat;
  value->rules.man_speed_deep_sea    = gs->rules.man_speed_deep_sea;
  value->rules.man_speed_refuel_base = gs->rules.man_speed_refuel_base;

  /* This tank's own modifiers, 0 ("classic") resolved to 100, and the
     reload interval the engine gives it. The owning client receives its
     own modifiers in the snapshot, so the client sim holds the live set. */
  {
    TankModifiers mods;
    tankGetModifiers(MY_TANK(csPtr), &mods);
    value->mods.speed  = (u_short) tankModPct(mods.speed);
    value->mods.accel  = (u_short) tankModPct(mods.accel);
    value->mods.turn   = (u_short) tankModPct(mods.turn);
    value->mods.reload = (u_short) tankModPct(mods.reload);
    value->mods.dealt  = (u_short) tankModPct(mods.dealt);
    value->mods.taken  = (u_short) tankModPct(mods.taken);
    value->reload_ticks = tankReloadTicks(gs, MY_TANK(csPtr));
  }


  /* Count carried pills from pillbox state (server syncs inTank via snapshots/events) */
  {
    BYTE selfPlayer = clientSimGetMyPlayerNum(csPtr);
    BYTE numPb = pillsGetNumPills(&gs->pb);
    BYTE carried = 0;
    for (BYTE pi = 0; pi < numPb; pi++) {
      if (pillsIsActive(&gs->pb, (BYTE)(pi + 1)) &&
          (*gs->pb).item[pi].inTank && (*gs->pb).item[pi].owner == selfPlayer) {
        carried++;
      }
    }
    value->carriedpills = carried;
  }
  value->carriedbases = 0;

  value->gunrange = tankGetGunsightLength(&MY_TANK(csPtr));
  value->reload = tankGetReloadTime(&MY_TANK(csPtr));
  if (first == TRUE) {
    /* Reset stuff */
    *clientSimGetBrainHoldKeys(csPtr) = 0;
    *clientSimGetBrainTapKeys(csPtr) = 0;
    *clientSimGetBrainsWantAllies(csPtr) = 0;
    *clientSimGetBrainsMessageDest(csPtr) = 0;
    clientSimGetBrainsMessage(csPtr)[0] = '\0';
    /* Set tank */
    value->newtank = TRUE;
  } else {
    value->newtank = tankIsNewTank(&MY_TANK(csPtr));
  }
  value->tankobstructed = tankIsObstructed(&MY_TANK(csPtr));

  /* Server tick and assistant message */
  value->server_tick = clientSimGetLastServerTick(csPtr);
  value->assistant_msg = clientSimGetBrainLastAssistMsg(csPtr);
  clientSimSetBrainLastAssistMsg(csPtr, 0);  /* consume once */

  /* Filter brain events based on aiMode */
  {
    int ei;
    int filtered = 0;
    int evCount = clientSimGetBrainEventCount(csPtr);
    const GameEvent *brainEvents = clientSimGetBrainEvents(csPtr);
    GameEvent *buf = evCount > 0 ? malloc(sizeof(GameEvent) * evCount) : NULL;
    for (ei = 0; ei < evCount; ei++) {
      const GameEvent *e = &brainEvents[ei];
      switch (e->type) {
      case EVENT_PILL_CAPTURED:
      case EVENT_BASE_CAPTURED:
      case EVENT_TANK_KILLED:
      case EVENT_LGM_LOST:
      case EVENT_PLAYER_LEAVE:
      case EVENT_SOUND:
      case EVENT_SOUND_SHOOT:
      case EVENT_SOUND_TANK_HIT:
      case EVENT_ASSISTANT_MSG:
      case EVENT_PING:
        buf[filtered++] = *e;
        break;
      case EVENT_PILL_UPDATE:
        if (aiMode == aiYesAdvantage || aiMode == aiFull) {
          buf[filtered++] = *e;
        } else {
          /* Only include if pill is within the tank view rect OR any team
           * pill's view rect. (A team pill trivially contains itself, so
           * "our pill is being hurt" events always reach the brain.) */
          BYTE px = e->data[1], py = e->data[2];
          if (brainViewRectsContain(viewRects, numViewRects, px, py)) {
            buf[filtered++] = *e;
          }
        }
        break;
      case EVENT_BASE_UPDATE:
      case EVENT_BASE_STOCK:
        if (aiMode == aiYesAdvantage || aiMode == aiFull) {
          buf[filtered++] = *e;
        } else {
          /* Look up base position and check the tank + team pill rects */
          BYTE idx = e->data[0];
          if (idx < MAX_BASES && gs->bs != NULL) {
            BYTE bx = (*gs->bs).item[idx].x;
            BYTE by = (*gs->bs).item[idx].y;
            if (brainViewRectsContain(viewRects, numViewRects, bx, by)) {
              buf[filtered++] = *e;
            }
          }
        }
        break;
      default:
        break;
      }
    }
    value->events = buf;
    value->num_events = (u_short)filtered;
    clientSimSetBrainEventCount(csPtr, 0);
  }

  /* Base nearby */
  closeBase = basesGetClosest(gs, value->tankx, value->tanky);
  if (closeBase == BASE_NOT_FOUND) {
    value->base = NULL;
  } else {
    value->base = (ObjectInfo*) malloc(sizeof(ObjectInfo));
    value->base->object = OBJECT_REFBASE;
    /* basesGetClosest returns a 1-BASED base number (not-found = 254 works
     * out of returnValue+1); basesGetBrainBaseItem below expects that and
     * decrements internally. The idnum handed to the brain, however, must be
     * 0-based like every other object id in the feed — passing it through
     * unconverted made the brain stamp "seen first-hand" stock/last_seen
     * onto base N+1: parked at base #10, bot0 refreshed base #11 across the
     * map every tick, so capture_base #11 appeared out of nowhere and the
     * phantom sighting was even KW-broadcast to allies (20260704_092654
     * t=1665/1710). */
    value->base->idnum = (BYTE)(closeBase - 1);
    basesGetBrainBaseItem(gs, closeBase, &(value->base->x), &(value->base->y), &(value->base->info), &(value->base_shells), &(value->base_mines), &(value->base_armour));
    value->base->direction = value->base_armour;
  }

  /* Lgm */
  value->man_status = lgmGetBrainState(&MY_LGM(csPtr));
  value->man_direction = lgmGetDir(&MY_LGM(csPtr), &MY_TANK(csPtr));
  value->man_x = lgmGetWX(&MY_LGM(csPtr));
  value->man_y = lgmGetWY(&MY_LGM(csPtr));
  value->manobstructed = lgmGetBrainObstructed(&MY_LGM(csPtr));

  /* Pillview — bots always use tank-centered view (no pill view) */
  value->pillview = malloc(sizeof(WORD));
  if (clientSimIsInPillView(csPtr) == TRUE) {
    *(value->pillview) = pillsGetViewPillNum(&gs->pb, clientSimGetPillViewX(csPtr), clientSimGetPillViewY(csPtr), FALSE, FALSE) -1;
    value->view_left = clientSimGetPillViewX(csPtr)-7;
    value->view_width = 15;
    value->view_top = clientSimGetPillViewY(csPtr)-7;
    value->view_height = 15;
  } else {
    /* Keep the 29x29 tank-centred window on the map.
     *
     * BUG THIS FIXES: view_left/view_top are MAP_X (uint8_t). With tx < 14 the
     * old `tx-14` wrapped to 242..255, and the right edge passed below
     * (view_left+view_width) wrapped back round to 15..28. That made
     * leftPos > rightPos, so brainDataMakeViewData's inclusive
     * `count2 <= rightPos` loop never ran for ANY row and the malloc'd buffer
     * was handed to the brain completely unwritten — 900 bytes of raw heap
     * read as terrain. Heap contents vary with allocation history, so on a
     * threaded bot host this was also a source of run-to-run divergence.
     *
     * SHIFT the origin rather than shrinking the window: shifting keeps every
     * row full, keeps view_width / view_height at their long-standing 29, and
     * still contains the tank (which is at most 14 tiles from an edge).
     * Behaviour for a tank away from the edges is bit-for-bit unchanged. */
    *(value->pillview) = 0x8000;
    {
      BYTE vleft = (tx > 14) ? (BYTE)(tx - 14) : 0;
      BYTE vtop  = (ty > 14) ? (BYTE)(ty - 14) : 0;
      /* 255 - 28 = 227: the largest origin whose inclusive right/bottom edge
       * (origin + view_width - 1) still fits in a BYTE without wrapping. */
      if (vleft > 227) vleft = 227;
      if (vtop  > 227) vtop  = 227;
      value->view_left = vleft;
      value->view_top  = vtop;
    }
    value->view_width = 29;
    value->view_height = 29;
  }
  /* The buffer is the largest window any branch above asks for; the fill
   * writes view_width * view_height of it and braincore hands the brain
   * exactly that many bytes, which is the length brain.h documents. The far
   * edges below are inclusive, hence the -1: passing view_left + view_width
   * described a 30-square row for a 29-square window, so the rows the brain
   * read were a square out of step with the ones written, and the last row
   * ran off the end of what it was given. */
  value->viewdata = malloc(30 * 30);
  /* Belt-and-braces: no path may ever hand the brain uninitialised heap as
   * terrain, even if some future view rect comes out empty. */
  if (value->viewdata != NULL) {
    memset(value->viewdata, 0, 30 * 30);
  }
  brainDataMakeViewData(csPtr, value->viewdata,
                        value->view_left,
                        (BYTE) (value->view_left + value->view_width - 1),
                        value->view_top,
                        (BYTE) (value->view_top + value->view_height - 1));

  /* From Bolo Version History:
  Added option to give Brains an advantage to make them more
  challenging opponents. With this option enabled, Bolo tells
  Brains the location of every base, pillbox and tank on the map
  even if they are out of visual range. (Use with caution in
  public games -- cyborgs currently get the same advantage.) */

  value->gameinfo.allow_AI = TRUE;
  value->gameinfo.assist_AI = FALSE;
  value->objects = clientSimGetBrainObjects(csPtr);
  if (aiMode == aiYesAdvantage || aiMode == aiFull) {
    value->gameinfo.assist_AI = TRUE;
    if (aiMode == aiFull && first == TRUE) {
      screenBrainMapFillFromMap(csPtr, &gs->mp, &gs->mns);
    }
    basesGetBrainBaseInRect(csPtr, gs, 0, 255, 0, 255);
    pillsGetBrainPillsInRect(csPtr, gs, &gs->pb, 0, 255, 0, 255);
    shellsGetBrainShellsInRect(csPtr, gs, &gs->shs, value->view_left, (BYTE) (value->view_left+value->view_width), value->view_top, (BYTE) (value->view_top+value->view_height));
    playersGetBrainTanksInRect(csPtr, &gs->plyrs, value->view_left, (BYTE) (value->view_left+value->view_width), value->view_top, (BYTE) (value->view_top+value->view_height), value->tankx, value->tanky);
    playersGetBrainLgmsInRect(csPtr, &gs->plyrs, value->view_left, (BYTE) (value->view_left+value->view_width), value->view_top, (BYTE) (value->view_top+value->view_height));
  } else {
    /* Must be aiYes else we wouldn't be called would we? */
    basesGetBrainBaseInRect(csPtr, gs, value->view_left, (BYTE) (value->view_left+value->view_width), value->view_top, (BYTE) (value->view_top+value->view_height));
    pillsGetBrainPillsInRect(csPtr, gs, &gs->pb, value->view_left, (BYTE) (value->view_left+value->view_width), value->view_top, (BYTE) (value->view_top+value->view_height));
    shellsGetBrainShellsInRect(csPtr, gs, &gs->shs, value->view_left, (BYTE) (value->view_left+value->view_width), value->view_top, (BYTE) (value->view_top+value->view_height));
    playersGetBrainTanksInRect(csPtr, &gs->plyrs, value->view_left, (BYTE) (value->view_left+value->view_width), value->view_top, (BYTE) (value->view_top+value->view_height), value->tankx, value->tanky);
    playersGetBrainLgmsInRect(csPtr, &gs->plyrs, value->view_left, (BYTE) (value->view_left+value->view_width), value->view_top, (BYTE) (value->view_top+value->view_height));
  }

  /* Team pill view sweep: pull objects from every deployed team pill's
   * 15x15 view rect, at EVERY ai level — brains effectively watch all
   * their pill views simultaneously. brainDataAddObject dedups by
   * type+id, so an entity inside the tank rect AND a pill rect (or two
   * overlapping pill rects) is added exactly once. Bases/pills are
   * already delivered map-wide in advantage/full mode, so only aiYes
   * needs them per pill rect. Shells for bots come from the snapshot
   * mirror below, which tests the whole rect union in one pass. */
  {
    int ri;
    for (ri = 1; ri < numViewRects; ri++) {
      BYTE pl  = viewRects[ri].left;
      BYTE pr  = viewRects[ri].right;
      BYTE pt  = viewRects[ri].top;
      BYTE pbt = viewRects[ri].bottom;
      if (aiMode == aiYes) {
        basesGetBrainBaseInRect(csPtr, gs, pl, pr, pt, pbt);
        pillsGetBrainPillsInRect(csPtr, gs, &gs->pb, pl, pr, pt, pbt);
      }
      playersGetBrainTanksInRect(csPtr, &gs->plyrs, pl, pr, pt, pbt, value->tankx, value->tanky);
      playersGetBrainLgmsInRect(csPtr, &gs->plyrs, pl, pr, pt, pbt);
    }
  }
  /* Bots have no client-side prediction layer that fills sim.shs, so
   * shellsGetBrainShellsInRect above adds nothing for bot players.
   * Mirror the snapshot shells (now retained for bots — see
   * clientApplySnapshot) into the brain object array so
   * info.objects actually contains type=OBJECT_SHOT entries the
   * brain (and BrainTest's shell-hitbox overlay) can render. */
  if (clientSimIsBot(csPtr)) {
    BYTE myPN      = clientSimGetMyPlayerNum(csPtr);
    int shellCount = clientSimGetServerShellCount(csPtr);
    const ShellSnapshot *shellSnaps = clientSimGetServerShellSnaps(csPtr);
    for (int i = 0; i < shellCount; i++) {
      const ShellSnapshot *s = &shellSnaps[i];
      BYTE smx = (BYTE)(s->worldX >> TANK_SHIFT_MAPSIZE);
      BYTE smy = (BYTE)(s->worldY >> TANK_SHIFT_MAPSIZE);
      /* Union of the tank view rect + every team pill's view rect */
      if (!brainViewRectsContain(viewRects, numViewRects, smx, smy))
        continue;
      BYTE owner;
      if (s->owner == NEUTRAL) owner = SHELLS_BRAIN_NEUTRAL;
      else if (playersIsAllie(&gs->plyrs, myPN, s->owner) == TRUE)
        owner = SHELLS_BRAIN_FRIENDLY;
      else
        owner = SHELLS_BRAIN_HOSTILE;
      /* THE THREE SHELL FACTS THE CLASSIC ObjectInfo HAS NO ROOM FOR.
       *
       * ObjectInfo (public/brain.h) is the 1998 brain API: object / x / y /
       * idnum / direction / info / speed. For a shell `direction` is the
       * utilGet16Dir SNAP of the true angle — 16 compass points, so up to
       * +-11.25 degrees of error, which over a shell's full 2016-WU flight is
       * ~400 WU (a tile and a half) of lateral drift. `idnum` and `speed` are
       * both dead for a shell (0 and 0). The snapshot the server sent us
       * (ShellSnapshot, input_packet.h) carries all three of the things the
       * struct drops: the exact 8-bit angle, the owner's player number, and
       * `length`, the shell's REMAINING LIFE in engine ticks.
       *
       * So carry them on the two dead fields and leave `direction` exactly as
       * it has always been — nothing that reads it changes behaviour:
       *
       *   idnum = (exact angle << 8) | owner player number  (NEUTRAL = 0xFF)
       *   speed = remaining life, engine ticks (0..63)
       *
       * braincore.c unpacks these into ob.angle / ob.owner / ob.life on the
       * Lua object table, which is where a brain should read them.
       *
       * The shell idnum is no longer always 0, but shells are still exempt
       * from brainDataAddObject's identity dedup (it is keyed on the object
       * TYPE), so two shells from the same pill still both arrive. */
      brainDataAddObject(csPtr, SHELLS_BRAIN_OBJECT_TYPE,
                         s->worldX, s->worldY,
                         (unsigned short)(((unsigned short)s->angle << 8)
                                          | (unsigned short)s->owner),
                         utilGet16Dir((TURNTYPE)s->angle),
                         owner, s->length);
    }
  }

  value->num_objects = *clientSimGetBrainsNumObjects(csPtr);
  *clientSimGetBrainsNumObjects(csPtr) = 0;

  /* Messages — drain the full per-tick inbox into a contiguous array
   * of MessageInfo. Pre-change behavior was a single-slot pull that
   * silently dropped extras when N ally bots all chatted on the same
   * tick. value->message stays valid as an alias to messages[0] so
   * legacy C consumers don't change. */
  {
    MessageState *ms = clientSimGetMessages(csPtr);
    int n = messageInboxCount(ms);
    value->messages = NULL;
    value->num_messages = 0;
    value->message = NULL;
    if (n > 0) {
      /* Receive-side audit: this bot pulled n messages from its inbox this tick.
       * Pairs with the BOTMSG fan-out log on the sender to confirm end-to-end
       * delivery (sender delivered=K should show up as receives across allies). */
      botMsgDebugLog("BOTMSG p%d inbox: %d msg(s) this tick",
                     (int)clientSimGetMyPlayerNum(csPtr), n);
      value->messages = (MessageInfo *)malloc((size_t)n * sizeof(MessageInfo));
      for (int i = 0; i < n; i++) {
        char  pbuf[BRAIN_INBOX_MSG_LEN];
        BYTE  from = messageInboxPeek(ms, i, pbuf);
        size_t plen = (size_t)((unsigned char)pbuf[0]);
        value->messages[i].sender    = from;
        value->messages[i].receivers = (uint32_t *)malloc(sizeof(uint32_t));
        if (value->messages[i].receivers != NULL) {
          *(value->messages[i].receivers) = 0;
        }
        value->messages[i].message = (u_char *)malloc(BRAIN_INBOX_MSG_LEN);
        if (value->messages[i].message != NULL) {
          memcpy(value->messages[i].message, pbuf, plen + 1);
          value->messages[i].message[plen + 1] = '\0';
        }
      }
      value->num_messages = (u_short)n;
      value->message      = &value->messages[0];
      messageInboxClear(ms);
    }
  }

  /* Controling the tank */
  value->holdkeys = clientSimGetBrainHoldKeys(csPtr);
  value->tapkeys = clientSimGetBrainTapKeys(csPtr);

  /* Building */
  value->build = *clientSimGetBrainBuildInfo(csPtr);

  /* Allies */
  //FIXME!!!!
  *clientSimGetBrainsWantAllies(csPtr) = *(value->allies);
  value->wantallies = clientSimGetBrainsWantAllies(csPtr);

  /* Smart ping request. Cleared on every think, so a brain that returns no
   * ping fields places nothing and last think's request cannot fire twice. */
  value->ping_pending = 0;
  value->ping_kind = 0;
  value->ping_x = 0;
  value->ping_y = 0;

  /* Message Sending */
  value->messagedest = clientSimGetBrainsMessageDest(csPtr);
  clientSimGetBrainsMessage(csPtr)[0] = '\0';
  value->sendmessage = (u_char *) clientSimGetBrainsMessage(csPtr);

  /* Game World */
  value->theWorld = screenBrainMapGetPointer(csPtr);

  /* Game Info */
  strcpy(((char *) &(value->gameinfo.mapname)), clientSimGetMapName(csPtr));
  value->gameinfo.gametype = gameTypeGet(&gs->game);
  value->gameinfo.start_delay = clientSimGetGmeStartDelay(csPtr);
  value->gameinfo.time_limit = clientSimGetGmeLength(csPtr);

  if (minesGetAllowHiddenMines(&gs->mns) == TRUE) {
    value->gameinfo.hidden_mines = GAMEINFO_HIDDENMINES;
  } else {
    value->gameinfo.hidden_mines = GAMEINFO_ALLMINES_VISIBLE;
  }
  value->gameinfo.gameid.start_time = (unsigned long) clientSimGetTimeStart(csPtr);
  value->gameinfo.gameid.serveraddress = clientSimGetServerAddress(csPtr);
  value->gameinfo.gameid.serverport = clientSimGetServerPort(csPtr);
}
/*********************************************************
*NAME:          brainDataExtractInfo
*AUTHOR:        John Morrison
*CREATION DATE: 26/11/99
*LAST MODIFIED: 13/12/99
*PURPOSE:
*  Called after the brain has executed. Extract the data
*  and cleanup
*
*ARGUMENTS:
*  value - Pointer to the brain info structure
*********************************************************/
void brainDataExtractInfo(ClientSim *csPtr, BrainInfo *value) {
  BYTE pillNum;

  free(value->allies);
  free(value->player_bots);
  if (value->base != NULL) {
    free(value->base);
  }

  /* Pill view manipulation */
  if (*(value->pillview) != 0x8000) {
    GameSim *gs = clientSimGetGameSim(csPtr);
    pillNum = (BYTE) (*(value->pillview));
    if (pillNum != (pillsGetViewPillNum(&gs->pb, clientSimGetPillViewX(csPtr), clientSimGetPillViewY(csPtr), FALSE, FALSE)-1)) {
      if (pillsSetView(gs, &gs->pb, pillNum, clientSimGetMyPlayerNum(csPtr)) == TRUE) {
        /* We can set the new view */
        pillbox p;
        pillsGetPill(&gs->pb, &p, (BYTE) (pillNum+ 1));
        clientSimSetInPillView(csPtr, TRUE);
        clientSimSetPillViewX(csPtr, p.x);
        clientSimSetPillViewY(csPtr, p.y);
        scrollCenterObject(clientSimGetScroll(csPtr), clientSimGetXOffsetPtr(csPtr), clientSimGetYOffsetPtr(csPtr), clientSimGetPillViewX(csPtr), clientSimGetPillViewY(csPtr));
        clientSimRecalc(csPtr);
      }
    }
  }

  free(value->pillview);
  free(value->viewdata);
  if (value->events != NULL) {
    free(value->events);
    value->events = NULL;
  }
  /* Free the per-tick messages array. value->message is just an alias
   * into messages[0]; freeing it separately would be a double-free. */
  if (value->messages != NULL) {
    for (u_short mi = 0; mi < value->num_messages; mi++) {
      free(value->messages[mi].receivers);
      free(value->messages[mi].message);
    }
    free(value->messages);
    value->messages = NULL;
    value->message  = NULL;
    value->num_messages = 0;
  }

  /* Controling the tank.
   *
   * NOTE these two lines are self-assignments and have been for a long time:
   * brainDataMakeInfo points value->holdkeys AT clientSimGetBrainHoldKeys(cs)
   * (see :517), so this copies *&cs->brainHoldKeys onto itself. The write that
   * actually drives the tank happens earlier, when the Lua result table is
   * read in extract_brain_output(), straight through that alias.
   *
   * Kept because the legacy brain API shape (a BrainInfo full of pointers the
   * brain writes through) is what other frontends still expect, but do not
   * add conditions here expecting them to gate anything -- the dead-tank gate
   * lives in extract_brain_output, where the real write is. */
  *clientSimGetBrainHoldKeys(csPtr) = *(value->holdkeys);
  *clientSimGetBrainTapKeys(csPtr) = *(value->tapkeys);

  /* Build requests are routed through InputPacket so the server sim
   * processes them authoritatively.  brainBuildInfo->action is 1-based
   * (0=none, 1=BsTrees, 2=BsRoad, ...) and clientBuildInputPacket
   * will pick it up and put it in the InputPacket as-is.  The server
   * sim decrements to 0-based before passing to lgmAddRequest. */
  if (value->build->action != 0) {
    if (tankIsDestroyed(&MY_TANK(csPtr))) {
      /* Tank is dead, cancel build */
      value->build->action = 0;
    }
    /* Otherwise leave action set for clientBuildInputPacket to read */
  }

  /* Allies */
  /* FIXME!!!! Get want allies stuff */
/*  brainsWantAllies = *(value->allies);
    value->wantallies = &brainsWantAllies; */

  /* Message Sending. Two dispatch modes share the same brain API:
   *   messagedest == 0 → internal channel (never crosses the chat wire)
   *     - Local brain (Brains menu on a human client): surface on the
   *       host's AI message channel so the brain author can read what
   *       their brain is broadcasting.
   *     - Bot-manager bot: fan the message into every allied bot's
   *       MessageState inbox so their brains can parse it via the
   *       normal incoming-message path. Never reaches a human's chat.
   *   messagedest != 0 → real player-to-player chat (unchanged). */
  if (value->sendmessage[0] != 0) {
    /* 255 characters plus the terminator: the pascal length byte can say
     * 255, and the writers that fill this buffer are free to grow to it. */
    char msg[256];
    GameSim *gs = clientSimGetGameSim(csPtr);
    utilPtoCString((char *) value->sendmessage, msg);
    if (*(value->messagedest) == 0) {
      struct ServerSim *bound = clientSimGetBoundServerSim(csPtr);
      if (bound == NULL) {
        /* No bound ServerSim → this is a local/human-menu brain, NOT a hosted
         * bot. The internal message can't fan out to teammates; it only lands on
         * the AI message channel. On a dedicated server this WARN means bot comms
         * are silently broken (every bot's info.messages stays empty). */
        botMsgDebugLog("BOTMSG p%d dest=0 but NO bound ServerSim -> NOT delivered "
                       "to teammates (AI-channel only): %.48s",
                       (int)clientSimGetMyPlayerNum(csPtr), msg);
        clientMessageAdd(clientSimGetMessages(csPtr), AIMessage,
                         langGetText(MESSAGE_AI), msg);
      } else {
        botMsgDebugLog("BOTMSG p%d send dest=0 (internal): %.48s",
                       (int)clientSimGetMyPlayerNum(csPtr), msg);
        /* QUEUE, don't deliver. This runs on the bot's worker thread inside
         * the parallel brain-think stage; delivering here would push into
         * every allied bot's MessageState inbox while those bots' own
         * workers may be reading and clearing the same unlocked ring, so
         * whether a message arrived before or after the receiver looked was
         * decided by thread interleaving. Queueing writes only into this
         * bot's own job slot; Stage 3 of botManagerTick fans it out
         * serially. Consequence: internal messages now always arrive on the
         * NEXT tick for every receiver (previously same-tick for
         * higher-numbered slots under -threads 1). */
        botManagerQueueInternalMessage(bound,
                                       clientSimGetMyPlayerNum(csPtr), msg);
      }
    } else {
      /* Send this message to the appropriate players */
      playersSendAiMessage(csPtr, gs, &gs->plyrs, *(value->messagedest), msg);
    }
    clientSimGetBrainsMessage(csPtr)[0] = '\0';
  }

  /* Smart ping. The brain asks; the engine places it from the brain's own
   * player slot, so allies see it on the map, the team filter is the one
   * every ping goes through, and a replay keeps it. Queued rather than
   * applied: this runs on a bot worker thread during the parallel think
   * stage, and serverSimApplyCommand belongs to the producer thread, which
   * drains the queue in Stage 3 of botManagerTick. Same deferral the bot's
   * chat takes, for the same reason. A local brain in a human's Brains menu
   * has no bound ServerSim and no player slot of its own to ping from, so
   * its request is dropped. */
  if (value->ping_pending != 0) {
    struct ServerSim *bound = clientSimGetBoundServerSim(csPtr);
    if (bound != NULL) {
      botManagerQueuePing(bound, clientSimGetMyPlayerNum(csPtr),
                          value->ping_kind, value->ping_x, value->ping_y);
    }
    value->ping_pending = 0;
  }
}
/*********************************************************
*NAME:          brainDataAddObject
*AUTHOR:        John Morrison
*CREATION DATE: 28/11/99
*LAST MODIFIED: 28/11/99
*PURPOSE:
*  Adds a brain object to the list of brain objects
*
*ARGUMENTS:
*  object - The type of the object
*  wx     - X position of the object
*  wy     - Y position of the object
*  idNum  - Objects identifier number
*  dir    - Direction of the object
*  info   - Object info
*********************************************************/
void brainDataAddObject(ClientSim *cs, unsigned short object, WORLD wx, WORLD wy, unsigned short idNum, BYTE dir, BYTE info, BYTE speed) {
  unsigned short *numObjects;
  ObjectInfo *objects;

  numObjects = clientSimGetBrainsNumObjects(cs);
  objects = clientSimGetBrainObjects(cs);
  /* Capacity guard — brainObjects is a fixed 1024-slot array
   * (client_sim_internal.h) that historically had no overflow check. */
  if (*numObjects >= 1024) {
    return;
  }
  /* Dedup identifiable objects (tanks / LGMs / pills / bases): with the
   * team-pill-view sweep the same entity can sit inside the tank view
   * rect AND one or more pill view rects — add it exactly once. Shells
   * are exempt: their idnum is not an identity (for a snapshot shell it
   * packs angle+owner, see the bot branch of brainDataGetInfo) and each
   * shell source runs a single pass per tick. */
  if (object != SHELLS_BRAIN_OBJECT_TYPE) {
    unsigned short i;
    for (i = 0; i < *numObjects; i++) {
      if (objects[i].object == object && objects[i].idnum == idNum) {
        return;
      }
    }
  }
  objects[*numObjects].object = object;
  objects[*numObjects].x = wx;
  objects[*numObjects].y = wy;
  objects[*numObjects].idnum = idNum;
  objects[*numObjects].direction = dir;
  objects[*numObjects].info = info;
  objects[*numObjects].speed = speed;
  (*numObjects)++;
}
