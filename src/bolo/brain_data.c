/*
 * Copyright (c) 1998-2008 John Morrison.
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
 *  Contents (declared in screen.h):
 *    screenMakeBrainViewDataCS  - terrain rect for the brain's view
 *    screenMakeBrainInfoCS      - populate BrainInfo before think()
 *    screenExtractBrainInfoCS   - read brain decisions back, free buffers
 *    screenAddBrainObject       - append an object to the brain object list
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
#include "screen.h"
#include "client_state.h"
#include "interpolation.h"
#include "util.h"
#include "client_sim.h"
#include "../server/server_sim.h"
#include "../steam/steam_wrapper.h"

/*********************************************************
*NAME:          screenMakeBrainViewData
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
void screenMakeBrainViewDataCS(ClientSim *cs, BYTE *buff, BYTE leftPos, BYTE rightPos, BYTE topPos, BYTE bottomPos) {
  BYTE count1; /* Looping variable */
  BYTE count2; /* Looping variable */
  BYTE pos;    /* Upto position    */

  pos = 0;
  for (count1=topPos;count1<=bottomPos;count1++) {
    for (count2=leftPos;count2<=rightPos;count2++) {
      if (basesExistPos(&cs->sim.bs, count2, count1) == TRUE) {
        buff[pos] = BREFBASE_T;
      } else if (pillsExistPos(&cs->sim.pb, count2, count1) == TRUE) {
        buff[pos] = BPILLBOX_T;
      } else {
        buff[pos] = mapGetPos(&cs->sim.mp, count2, count1);
        if (buff[pos] == DEEP_SEA) {
          buff[pos] = BDEEPSEA;
        } else if (buff[pos] >= MINE_START && buff[pos] <= MINE_END) {
          buff[pos] = buff[pos] - MINE_SUBTRACT;
        }
        if (minesExistPos(&cs->sim.mns, &cs->sim.mp, count2, count1) == TRUE) {
          buff[pos] |= TERRAIN_MINE;
        }
      }
      pos++;
    }
  }
}
/*********************************************************
*NAME:          screenMakeBrainInfo
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
void screenMakeBrainInfoCS(ClientSim *csPtr, BrainInfo *value, bool first, aiType aiMode) {
  BYTE tx;        /* Tank X and Y Co-ordinates */
  BYTE ty;
  BYTE closeBase; /* The closest base to our current position */


  if (MY_TANK(csPtr) == NULL) {
    return;
  }
  
  tx = tankGetMX(&MY_TANK(csPtr));
  ty = tankGetMY(&MY_TANK(csPtr));

  /* Max's */
  value->max_players = MAX_TANKS;//-1; /* FIXME: Huh? */
  value->max_refbases = basesGetNumBases(&csPtr->sim.bs);//-1;
  value->max_pillboxes = pillsGetNumPills(&csPtr->sim.pb);//-1;
  value->player_number = csPtr->myPlayerNum;
  value->num_players = playersGetNumPlayers(&csPtr->sim.plyrs);
  value->playernames = playersGetBrainsNamesArray(&csPtr->sim.plyrs);
  value->allies = malloc(sizeof(PlayerBitMap));
  *(value->allies) = playersGetAlliesBitMap(&csPtr->sim.plyrs, csPtr->myPlayerNum);

  /* Tank */
  tankGetWorld(&MY_TANK(csPtr), &(value->tankx), &(value->tanky));
  value->direction = tankGet256Dir(&MY_TANK(csPtr));
  /* Float tank angle for sub-brad-precision brains. tank->angle is
   * the float the engine fires shells at; `direction` above floors
   * it for legacy BYTE consumers. */
  value->tank_angle = (MY_TANK(csPtr) != NULL) ? (float)MY_TANK(csPtr)->angle : 0.0f;
  value->speed = (BYTE) (tankGetSpeed(&MY_TANK(csPtr)) * 4);
  value->inboat = tankIsOnBoat(&MY_TANK(csPtr));
  value->hidden = utilIsTankInTrees(&csPtr->sim.mp, &csPtr->sim.pb, &csPtr->sim.bs, value->tankx, value->tanky);

  tankGetStats(&MY_TANK(csPtr), &(value->shells), &(value->mines), &(value->armour), &(value->trees));


  /* Count carried pills from pillbox state (server syncs inTank via snapshots/events) */
  {
    BYTE selfPlayer = csPtr->myPlayerNum;
    BYTE numPb = pillsGetNumPills(&csPtr->sim.pb);
    BYTE carried = 0;
    for (BYTE pi = 0; pi < numPb; pi++) {
      if ((*csPtr->sim.pb).item[pi].inTank && (*csPtr->sim.pb).item[pi].owner == selfPlayer) {
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
  value->server_tick = csPtr->lastServerTick;
  value->assistant_msg = csPtr->brainLastAssistMsg;
  csPtr->brainLastAssistMsg = 0;  /* consume once */

  /* Filter brain events based on aiMode */
  {
    int ei;
    int filtered = 0;
    int evCount = csPtr->brainEventCount;
    GameEvent *buf = evCount > 0 ? malloc(sizeof(GameEvent) * evCount) : NULL;
    for (ei = 0; ei < evCount; ei++) {
      GameEvent *e = &csPtr->brainEvents[ei];
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
        buf[filtered++] = *e;
        break;
      case EVENT_PILL_UPDATE:
        if (aiMode == aiYesAdvantage || aiMode == aiFull) {
          buf[filtered++] = *e;
        } else {
          /* Only include if pill is within view rect */
          BYTE px = e->data[1], py = e->data[2];
          if (px >= value->view_left && px <= value->view_left + value->view_width &&
              py >= value->view_top && py <= value->view_top + value->view_height) {
            buf[filtered++] = *e;
          }
        }
        break;
      case EVENT_BASE_UPDATE:
        if (aiMode == aiYesAdvantage || aiMode == aiFull) {
          buf[filtered++] = *e;
        } else {
          /* Look up base position and check view rect */
          BYTE idx = e->data[0];
          if (idx < MAX_BASES && csPtr->sim.bs != NULL) {
            BYTE bx = (*csPtr->sim.bs).item[idx].x;
            BYTE by = (*csPtr->sim.bs).item[idx].y;
            if (bx >= value->view_left && bx <= value->view_left + value->view_width &&
                by >= value->view_top && by <= value->view_top + value->view_height) {
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
    csPtr->brainEventCount = 0;
  }

  /* Base nearby */
  closeBase = basesGetClosest(&csPtr->sim, value->tankx, value->tanky);
  if (closeBase == BASE_NOT_FOUND) {
    value->base = NULL;
  } else {
    value->base = (ObjectInfo*) malloc(sizeof(ObjectInfo));
    value->base->object = OBJECT_REFBASE;
    value->base->idnum = closeBase;
    basesGetBrainBaseItem(&csPtr->sim, closeBase, &(value->base->x), &(value->base->y), &(value->base->info), &(value->base_shells), &(value->base_mines), &(value->base_armour));
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
  if (csPtr->inPillView == TRUE) {
    *(value->pillview) = pillsGetPillNum(&csPtr->sim.pb, csPtr->pillViewX, csPtr->pillViewY, FALSE, FALSE) -1;
    value->view_left = csPtr->pillViewX-7;
    value->view_width = 15;
    value->view_top = csPtr->pillViewY-7;
    value->view_height = 15;
  } else {
    *(value->pillview) = 0x8000;
    value->view_left = tx-14;
    value->view_width = 29;
    value->view_top = ty-14;
    value->view_height = 29;
  }
  //value->viewdata = malloc((value->view_width+1) * (value->view_height+1));
  value->viewdata = malloc(30 * 30);
  screenMakeBrainViewDataCS(csPtr, value->viewdata, value->view_left, (BYTE) (value->view_left+value->view_width), value->view_top, (BYTE) (value->view_top+value->view_height));

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
      screenBrainMapFillFromMap(csPtr, &csPtr->sim.mp, &csPtr->sim.mns);
    }
    basesGetBrainBaseInRect(csPtr, &csPtr->sim, 0, 255, 0, 255);
    pillsGetBrainPillsInRect(csPtr, &csPtr->sim, &csPtr->sim.pb, 0, 255, 0, 255);
    shellsGetBrainShellsInRect(csPtr, &csPtr->sim, &csPtr->sim.shs, value->view_left, (BYTE) (value->view_left+value->view_width), value->view_top, (BYTE) (value->view_top+value->view_height));
    playersGetBrainTanksInRect(csPtr, &csPtr->sim.plyrs, value->view_left, (BYTE) (value->view_left+value->view_width), value->view_top, (BYTE) (value->view_top+value->view_height), value->tankx, value->tanky);
    playersGetBrainLgmsInRect(csPtr, &csPtr->sim.plyrs, value->view_left, (BYTE) (value->view_left+value->view_width), value->view_top, (BYTE) (value->view_top+value->view_height));
  } else {
    /* Must be aiYes else we wouldn't be called would we? */
    basesGetBrainBaseInRect(csPtr, &csPtr->sim, value->view_left, (BYTE) (value->view_left+value->view_width), value->view_top, (BYTE) (value->view_top+value->view_height));
    pillsGetBrainPillsInRect(csPtr, &csPtr->sim, &csPtr->sim.pb, value->view_left, (BYTE) (value->view_left+value->view_width), value->view_top, (BYTE) (value->view_top+value->view_height));
    shellsGetBrainShellsInRect(csPtr, &csPtr->sim, &csPtr->sim.shs, value->view_left, (BYTE) (value->view_left+value->view_width), value->view_top, (BYTE) (value->view_top+value->view_height));
    playersGetBrainTanksInRect(csPtr, &csPtr->sim.plyrs, value->view_left, (BYTE) (value->view_left+value->view_width), value->view_top, (BYTE) (value->view_top+value->view_height), value->tankx, value->tanky);
    playersGetBrainLgmsInRect(csPtr, &csPtr->sim.plyrs, value->view_left, (BYTE) (value->view_left+value->view_width), value->view_top, (BYTE) (value->view_top+value->view_height));
  }
  /* Bots have no client-side prediction layer that fills sim.shs, so
   * shellsGetBrainShellsInRect above adds nothing for bot players.
   * Mirror the snapshot shells (now retained for bots — see
   * screenSyncFromSnapshotCS) into the brain object array so
   * info.objects actually contains type=OBJECT_SHOT entries the
   * brain (and BrainTest's shell-hitbox overlay) can render. */
  if (csPtr->isBot) {
    BYTE leftPos   = value->view_left;
    BYTE rightPos  = (BYTE)(value->view_left  + value->view_width);
    BYTE topPos    = value->view_top;
    BYTE bottomPos = (BYTE)(value->view_top   + value->view_height);
    BYTE myPN      = csPtr->myPlayerNum;
    for (int i = 0; i < csPtr->serverShellCount; i++) {
      const ShellSnapshot *s = &csPtr->serverShellSnaps[i];
      BYTE smx = (BYTE)(s->worldX >> TANK_SHIFT_MAPSIZE);
      BYTE smy = (BYTE)(s->worldY >> TANK_SHIFT_MAPSIZE);
      if (smx < leftPos || smx > rightPos || smy < topPos || smy > bottomPos)
        continue;
      BYTE owner;
      if (s->owner == NEUTRAL) owner = SHELLS_BRAIN_NEUTRAL;
      else if (playersIsAllie(&csPtr->sim.plyrs, myPN, s->owner) == TRUE)
        owner = SHELLS_BRAIN_FRIENDLY;
      else
        owner = SHELLS_BRAIN_HOSTILE;
      screenAddBrainObject(csPtr, SHELLS_BRAIN_OBJECT_TYPE,
                           s->worldX, s->worldY, 0,
                           utilGet16Dir((TURNTYPE)s->angle),
                           owner, 0);
    }
  }

  value->num_objects = *clientSimGetBrainsNumObjects(csPtr);
  *clientSimGetBrainsNumObjects(csPtr) = 0;

  /* Message */
  if (messageIsNewMessage(&csPtr->messages) == TRUE) {
    value->message = (MessageInfo*) malloc(sizeof(MessageInfo));
    value->message->receivers = malloc(sizeof(value->message->receivers));
    value->message->message = malloc(512);
    value->message->sender = messageGetNewMessage(&csPtr->messages, (char *) value->message->message, &(value->message->receivers)); /* FIXME: Second parameter? */
  } else {
    value->message = NULL;
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

  /* Message Sending */
  value->messagedest = clientSimGetBrainsMessageDest(csPtr);
  clientSimGetBrainsMessage(csPtr)[0] = '\0';
  value->sendmessage = (u_char *) clientSimGetBrainsMessage(csPtr);

  /* Game World */
  value->theWorld = screenBrainMapGetPointer(csPtr);

  /* Game Info */
  strcpy(((char *) &(value->gameinfo.mapname)), csPtr->mapName);
  value->gameinfo.gametype = gameTypeGet(&csPtr->sim.game);
  value->gameinfo.start_delay = csPtr->gmeStartDelay;
  value->gameinfo.time_limit = csPtr->gmeLength;

  if (minesGetAllowHiddenMines(&csPtr->sim.mns) == TRUE) {
    value->gameinfo.hidden_mines = GAMEINFO_HIDDENMINES;
  } else {
    value->gameinfo.hidden_mines = GAMEINFO_ALLMINES_VISIBLE;
  }
  value->gameinfo.gameid.start_time = (unsigned long) csPtr->timeStart;
  value->gameinfo.gameid.serveraddress = csPtr->serverAddress;
  value->gameinfo.gameid.serverport = csPtr->serverPort;
}
/*********************************************************
*NAME:          screenExtractBrainInfo
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
void screenExtractBrainInfoCS(ClientSim *csPtr, BrainInfo *value) {
  BYTE pillNum;

  free(value->allies);
  if (value->base != NULL) {
    free(value->base);
  }

  /* Pill view manipulation */
  if (*(value->pillview) != 0x8000) {
    pillNum = (BYTE) (*(value->pillview));
    if (pillNum != (pillsGetPillNum(&csPtr->sim.pb, csPtr->pillViewX, csPtr->pillViewY, FALSE, FALSE)-1)) {
      if (pillsSetView(&csPtr->sim, &csPtr->sim.pb, pillNum, csPtr->myPlayerNum) == TRUE) {
        /* We can set the new view */
        pillbox p;
        pillsGetPill(&csPtr->sim.pb, &p, (BYTE) (pillNum+ 1));
        csPtr->inPillView = TRUE;
        csPtr->pillViewX = p.x;
        csPtr->pillViewY = p.y;
        scrollCenterObject(&csPtr->scroll, &csPtr->xOffset, &csPtr->yOffset, csPtr->pillViewX, csPtr->pillViewY);
        screenReCalcCS(csPtr);
      }
    }
  }

  free(value->pillview);
  free(value->viewdata);
  if (value->events != NULL) {
    free(value->events);
    value->events = NULL;
  }
  if (value->message != NULL) {
    free(value->message->receivers);
    free(value->message->message);
    free(value->message);
  }

  /* Controling the tank */
  *clientSimGetBrainHoldKeys(csPtr) = *(value->holdkeys);
  *clientSimGetBrainTapKeys(csPtr) = *(value->tapkeys);

  /* Build requests are routed through InputPacket so the server sim
   * processes them authoritatively.  brainBuildInfo->action is 1-based
   * (0=none, 1=BsTrees, 2=BsRoad, ...) and screenBuildInputPacket
   * will pick it up and put it in the InputPacket as-is.  The server
   * sim decrements to 0-based before passing to lgmAddRequest. */
  if (value->build->action != 0) {
    if (tankGetArmour(&MY_TANK(csPtr)) > TANK_FULL_ARMOUR) {
      /* Tank is dead, cancel build */
      value->build->action = 0;
    }
    /* Otherwise leave action set for screenBuildInputPacket to read */
  }

  /* Allies — emit request packets for every slot the brain wants allied
   * that we aren't allied with yet. Throttled per-target so a brain that
   * sets the bit every tick (e.g. NewAutopilot's auto_ally_all) doesn't
   * flood the wire while waiting for the other side to accept.
   *
   * NOTE: brainsWantAllies was seeded to current allies in
   * screenMakeBrainInfoCS, then overwritten by the brain's wantallies
   * field. The diff against the current allies bitmap is what we act on. */
  {
    PlayerBitMap want    = csPtr->brainsWantAllies;
    PlayerBitMap current = playersGetAlliesBitMap(&csPtr->sim.plyrs, csPtr->myPlayerNum);
    /* Only consider bits that are wanted but not yet allied. The "current"
     * bitmap from playersGetAlliesBitMap includes our own slot, so masking
     * with ~current also drops self. */
    PlayerBitMap todo = want & ~current;
    /* 5 seconds at 30 ticks/sec — same cadence as the human UI button. */
    const uint32_t COOLDOWN_TICKS = 150;
    BYTE i;
    for (i = 0; i < MAX_TANKS; i++) {
      if (i == csPtr->myPlayerNum) continue;
      if (!(todo & (1u << i))) continue;
      if (!playersIsInUse(&csPtr->sim.plyrs, i)) continue;
      uint32_t now = csPtr->lastServerTick;
      uint32_t last = csPtr->brainsAllianceReqLastTick[i];
      if (last != 0 && (uint32_t)(now - last) < COOLDOWN_TICKS) continue;
      clientSimRequestAlliance(csPtr, csPtr->myPlayerNum, i);
      csPtr->brainsAllianceReqLastTick[i] = (now == 0) ? 1 : now;
    }
  }

  /* Message Sending */
  if (value->sendmessage[0] != 0) {
    char msg[255];
    utilPtoCString((char *) value->sendmessage, msg);
    if (*(value->messagedest) == 0) {
      /* Its a debug message */
      clientMessageAdd(&csPtr->messages, AIMessage, langGetText(MESSAGE_AI), msg);
    } else {
      /* Send this message to the appropriate players */
      playersSendAiMessage(csPtr, &csPtr->sim, &csPtr->sim.plyrs, *(value->messagedest), msg);
    }
    clientSimGetBrainsMessage(csPtr)[0] = '\0';
  }
}
/*********************************************************
*NAME:          screenAddBrainObject
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
void screenAddBrainObject(ClientSim *cs, unsigned short object, WORLD wx, WORLD wy, unsigned short idNum, BYTE dir, BYTE info, BYTE speed) {
  unsigned short *numObjects;
  ObjectInfo *objects;

  numObjects = clientSimGetBrainsNumObjects(cs);
  objects = clientSimGetBrainObjects(cs);
  objects[*numObjects].object = object;
  objects[*numObjects].x = wx;
  objects[*numObjects].y = wy;
  objects[*numObjects].idnum = idNum;
  objects[*numObjects].direction = dir;
  objects[*numObjects].info = info;
  objects[*numObjects].speed = speed;
  (*numObjects)++;
}
