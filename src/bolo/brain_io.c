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
 *Name:          Brain I/O
 *Filename:      brain_io.c
 *Purpose:
 *  Brain-side bridge between the ClientSim, Lua brains, and
 *  the network layer.  Extracted from screen.c so that the
 *  dedicated server (which links no rendering code) can drive
 *  bots through the same code path the GUI client uses for
 *  brains.
 *
 *  Contents:
 *    screenMakeBrainViewDataCS  - terrain rect for the brain's view
 *    screenMakeBrainInfoCS      - populate BrainInfo before think()
 *    screenExtractBrainInfoCS   - read brain decisions back, free buffers
 *    screenAddBrainObject       - append an object to the brain object list
 *    screenBuildInputPacketCS   - pack brain keys/build into an InputPacket
 *    screenSyncFromSnapshotCS   - apply a server snapshot to the ClientSim
 *
 *  The functions retain their original screen* names so existing
 *  callers (and screen.h declarations) continue to resolve here.
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

  /* Allies */
  /* FIXME!!!! Get want allies stuff */
/*  brainsWantAllies = *(value->allies);
    value->wantallies = &brainsWantAllies; */

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
/*********************************************************
*NAME:          screenBuildInputPacket
*PURPOSE:
*  Builds an InputPacket from the current input state.
*  Handles both keyboard input and brain input.
*********************************************************/
void screenBuildInputPacketCS(ClientSim *csPtr, InputPacket *pkt, tankButton tb, bool isShoot, bool isMine, bool isBrain, bool isGameTick, BYTE playerNum, uint32_t tick) {
  memset(pkt, 0, sizeof(InputPacket));
  pkt->tick = tick;
  pkt->playerNum = playerNum;

  /* Pack autoslowdown state into flags (sent every packet so server stays in sync) */
  if (tankGetAutoSlowdown(&MY_TANK(csPtr))) {
    pkt->flags |= INPUT_FLAG_AUTOSLOW;
  }

  if (isBrain) {
    /* Translate brain keys to tankButton + shoot, same as screenTranslateBrainButtons
     * but without the side effects (gunsight, mine laying, etc.) that the brain
     * handles through the InputPacket instead */
    uint32_t *holdKeys = clientSimGetBrainHoldKeys(csPtr);
    uint32_t *tapKeys = clientSimGetBrainTapKeys(csPtr);
    unsigned long temp = *holdKeys;

    /* Handle tap keys for movement */
    if (testkey(*tapKeys, KEY_faster))    { setkey(temp, KEY_faster); }
    if (testkey(*tapKeys, KEY_slower))    { setkey(temp, KEY_slower); }
    if (testkey(*tapKeys, KEY_turnleft))  { setkey(temp, KEY_turnleft); }
    if (testkey(*tapKeys, KEY_turnright)) { setkey(temp, KEY_turnright); }

    /* Build button bitmask from brain keys */
    if (testkey(temp, KEY_faster))    { pkt->buttons |= INPUT_BTN_ACCEL; }
    if (testkey(temp, KEY_slower))    { pkt->buttons |= INPUT_BTN_DECEL; }
    if (testkey(temp, KEY_turnleft))  { pkt->buttons |= INPUT_BTN_LEFT; }
    if (testkey(temp, KEY_turnright)) { pkt->buttons |= INPUT_BTN_RIGHT; }

    /* Fire action — only on game ticks */
    if (isGameTick) {
      if (testkey(*tapKeys, KEY_shoot) || testkey(*holdKeys, KEY_shoot)) {
        pkt->actions |= INPUT_ACTION_FIRE;
      }
    }

    /* Mine laying */
    if (testkey(*tapKeys, KEY_dropmine) || testkey(*holdKeys, KEY_dropmine)) {
      pkt->actions |= INPUT_ACTION_LAY_MINE;
    }

    /* Gunsight adjustment (bits 2-3 of flags) */
    if (testkey(*tapKeys, KEY_morerange) || testkey(*holdKeys, KEY_morerange)) {
      pkt->flags |= (1 << INPUT_FLAG_GUNSIGHT_SHIFT);  /* increase */
    } else if (testkey(*tapKeys, KEY_lessrange) || testkey(*holdKeys, KEY_lessrange)) {
      pkt->flags |= (2 << INPUT_FLAG_GUNSIGHT_SHIFT);  /* decrease */
    }

    /* Build action from brain (1-based: 0=none, 1=BsTrees, ...).
     * Only consume on game ticks — the server ignores buildAction on keys ticks. */
    BuildInfo *buildInfo = *clientSimGetBrainBuildInfo(csPtr);
    if (isGameTick && buildInfo != NULL && buildInfo->action != 0) {
      pkt->buildAction = buildInfo->action;
      pkt->buildX = buildInfo->x;
      pkt->buildY = buildInfo->y;
      buildInfo->action = 0;  /* consume the request */
    }

    /* Client-side display actions (pill/tank view toggle) */
    if (testkey(*tapKeys, KEY_TankView) || testkey(*holdKeys, KEY_TankView)) {
      csPtr->inPillView = FALSE;
      clientCenterTankCS(csPtr);
    }
    if (testkey(*tapKeys, KEY_PillView) || testkey(*holdKeys, KEY_PillView)) {
      screenPillViewCS(csPtr, 0, 0);
    }

    /* Clear tap keys after reading — preserve shoot tap on non-game ticks
     * so it fires on the next game tick (matches screenTranslateBrainButtons) */
    {
      uint32_t preserved = 0;
      if (!isGameTick && testkey(*tapKeys, KEY_shoot)) {
        setkey(preserved, KEY_shoot);
      }
      *tapKeys = preserved;
    }
  } else {
    /* Keyboard input: convert tankButton enum to bitmask */
    switch (tb) {
      case TLEFTACCEL:  pkt->buttons = INPUT_BTN_LEFT | INPUT_BTN_ACCEL; break;
      case TRIGHTACCEL: pkt->buttons = INPUT_BTN_RIGHT | INPUT_BTN_ACCEL; break;
      case TLEFTDECEL:  pkt->buttons = INPUT_BTN_LEFT | INPUT_BTN_DECEL; break;
      case TRIGHTDECEL: pkt->buttons = INPUT_BTN_RIGHT | INPUT_BTN_DECEL; break;
      case TLEFT:       pkt->buttons = INPUT_BTN_LEFT; break;
      case TRIGHT:      pkt->buttons = INPUT_BTN_RIGHT; break;
      case TACCEL:      pkt->buttons = INPUT_BTN_ACCEL; break;
      case TDECEL:      pkt->buttons = INPUT_BTN_DECEL; break;
      default:          pkt->buttons = 0; break;
    }

    /* Fire action — only on game ticks */
    if (isGameTick && isShoot) {
      pkt->actions |= INPUT_ACTION_FIRE;
    }

    /* Mine laying */
    if (isMine) {
      pkt->actions |= INPUT_ACTION_LAY_MINE;
    }
  }

  /* Pending human-player build request (from screenManMove).
   * Only consume on game ticks — the server ignores buildAction on keys ticks. */
  if (!isBrain && isGameTick && csPtr->pendingBuildAction != 0) {
    pkt->buildAction = csPtr->pendingBuildAction;
    pkt->buildX = csPtr->pendingBuildX;
    pkt->buildY = csPtr->pendingBuildY;
    csPtr->pendingBuildAction = 0;
    csPtr->pendingBuildX = 0;
    csPtr->pendingBuildY = 0;
  }
}
/*********************************************************
*NAME:          screenSyncFromSnapshot
*PURPOSE:
*  Syncs client state from a network snapshot received over
*  UDP transport. Updates tank positions (own tank via
*  reconciliation, others via interpolation), rebuilds
*  shells and explosions from snapshot data, and processes
*  game events (map changes, etc.).
*********************************************************/
void screenSyncFromSnapshotCS(ClientSim *csPtr,
                              const SnapshotHeader *hdr,
                              const TankSnapshot *tanks, int tankCount,
                              const ShellSnapshot *shellSnaps, int shellCount,
                              const TkExplosionSnapshot *tkExplSnaps, int tkExplosionCount,
                              const BaseSnapshot *baseSnaps, int baseCount,
                              const PillSnapshot *pillSnaps, int pillCount,
                              const GameEvent *events, int eventCount,
                              BYTE playerNum) {
  int i;
  bool isHuman = !csPtr->isBot;

  /* Update other players via interpolation */
  csPtr->interpCtx.localPlayer = playerNum;
  for (i = 0; i < tankCount; i++) {
    BYTE pn = (BYTE)(tanks[i].playerNum & TANK_SNAPSHOT_PLAYER_MASK);

    /* Hidden stub: this slot is connected but its tank/LGM are outside our
     * viewport.  Mark the slot missing for interp and clear the players
     * struct entry so the renderer doesn't keep drawing the last in-view
     * position as a ghost. */
    if (tanks[i].playerNum & TANK_SNAPSHOT_HIDDEN_FLAG) {
      if (pn != playerNum && pn < MAX_TANKS) {
        interpMarkMissing(&csPtr->interpCtx, pn);
        if (csPtr->sim.plyrs != NULL && playersIsInUse(&csPtr->sim.plyrs, pn)) {
          playersUpdate(&csPtr->sim.plyrs, pn, 0, 0, 0, 0, 0, FALSE,
                        0, 0, 0, 0, 0);
        }
      }
      continue;
    }

    /* Update ping and client flags for all players from snapshot */
    playersSetPing(&csPtr->sim.plyrs, pn, tanks[i].pingMs);
    {
      /* Snapshot is authoritative only for these bits — preserve any others
       * (e.g. STEAM_BUILD set once from JOIN_REQUEST) across snapshot ticks. */
      const uint8_t snapshotMask = PLAYER_FLAG_WBN_VERIFIED
                                 | PLAYER_FLAG_WBN_STEAM_LINKED
                                 | PLAYER_FLAG_SUPPORTER;
      uint8_t cur = playersGetClientFlags(&csPtr->sim.plyrs, pn);
      uint8_t next = (uint8_t)((cur & ~snapshotMask) | (tanks[i].clientFlags & snapshotMask));
      playersSetClientFlags(&csPtr->sim.plyrs, pn, next);
    }
    if (tanks[i].clientFlags != 0) {
      WB_LOG_TRACE(WB_LOG_CAT_CLIENT, "[WBN] player %d clientFlags=0x%02x", pn, tanks[i].clientFlags);
    }

    if (pn == playerNum) {
      /* Own tank: first sync or reconcile */
      if (csPtr->clientState.initialized && !csPtr->clientState.hasPredictedTank && MY_TANK(csPtr) != NULL) {
        /* First snapshot — initialize predicted tank from server state */
        TURNTYPE decodedAngle = (TURNTYPE)tanks[i].angle / 256.0f;
        SPEEDTYPE decodedSpeed = (SPEEDTYPE)tanks[i].speed / 256.0f;
        tankSetWorld(&csPtr->sim, &MY_TANK(csPtr), tanks[i].worldX, tanks[i].worldY,
                     decodedAngle, FALSE);
        {
          BYTE isDead, onBoat;
          utilGetNibbles(tanks[i].tankStatus, &isDead, &onBoat);
          tankSetOnBoat(&MY_TANK(csPtr), onBoat);
        }
        tankSetSpeed(&MY_TANK(csPtr), decodedSpeed);
        tankSetFirstLeft(&MY_TANK(csPtr), tanks[i].firstLeft);
        tankSetFirstRight(&MY_TANK(csPtr), tanks[i].firstRight);
        tankSetArmour(&MY_TANK(csPtr), tanks[i].armour);
        csPtr->lastServerArmour = tanks[i].armour;
        tankSetShells(&MY_TANK(csPtr), tanks[i].shells);
        tankSetMines(&MY_TANK(csPtr), tanks[i].mines);
        tankSetTrees(&MY_TANK(csPtr), tanks[i].trees);
        tankSetGunsightLength(&MY_TANK(csPtr), tanks[i].gunsightLen);
        tankSetReload(&MY_TANK(csPtr), tanks[i].reload);
        csPtr->clientState.hasPredictedTank = TRUE;
        if (isHuman) {
          clientCenterTankCS(csPtr);
        }
      } else if (csPtr->clientState.initialized && csPtr->clientState.hasPredictedTank && MY_TANK(csPtr) != NULL) {
        /* Build a temporary tank-like state for reconciliation.
         * We use the wire snapshot data to check/correct our prediction. */
        WORLD predX, predY, servX, servY;
        TURNTYPE predAngle, servAngle;
        SPEEDTYPE decodedSpeed;
        int32_t dx, dy;

        tankGetWorld(&MY_TANK(csPtr), &predX, &predY);
        predAngle = tankGetAngle(&MY_TANK(csPtr));
        servX = tanks[i].worldX;
        servY = tanks[i].worldY;
        servAngle = (TURNTYPE)tanks[i].angle / 256.0f;
        decodedSpeed = (SPEEDTYPE)tanks[i].speed / 256.0f;

        /* Update oldest unacked based on server acknowledgment */
        csPtr->clientState.oldestUnacked = hdr->lastProcessedInput + 1;

        dx = (int32_t)predX - (int32_t)servX;
        dy = (int32_t)predY - (int32_t)servY;

        /* Compare angles in the quantized uint16 domain the wire uses
         * (angle × 256) to avoid false mismatches from float round-trip. */
        {
          bool angleMismatch = ((uint16_t)(predAngle * 256.0f) != tanks[i].angle);

          if (dx != 0 || dy != 0 || angleMismatch) {
            /* Prediction diverged — snap to server state and replay.
             * Save the predicted angle: if the angle was actually correct
             * (quantized values match) we restore it after replay to avoid
             * gunsight jitter caused by speed-quantization-induced position
             * drift triggering unnecessary angle changes during replay. */
            TURNTYPE savedAngle = predAngle;
            BYTE savedFirstLeft = tankGetFirstLeft(&MY_TANK(csPtr));
            BYTE savedFirstRight = tankGetFirstRight(&MY_TANK(csPtr));

            tankSetWorld(&csPtr->sim, &MY_TANK(csPtr), servX, servY, servAngle, FALSE);
            {
              BYTE isDead, onBoat;
              utilGetNibbles(tanks[i].tankStatus, &isDead, &onBoat);
              tankSetOnBoat(&MY_TANK(csPtr), onBoat);
            }
            tankSetSpeed(&MY_TANK(csPtr), decodedSpeed);
            tankSetFirstLeft(&MY_TANK(csPtr), tanks[i].firstLeft);
            tankSetFirstRight(&MY_TANK(csPtr), tanks[i].firstRight);
            tankSetReload(&MY_TANK(csPtr), tanks[i].reload);

            /* Replay unacknowledged inputs (suppress sounds/side effects) */
            csPtr->sim.isPredicting = TRUE;
            {
              uint32_t tick;
              for (tick = csPtr->clientState.oldestUnacked; tick <= csPtr->clientState.newestInput; tick++) {
                uint8_t idx = tick & (CLIENT_INPUT_HISTORY_SIZE - 1);
                InputPacket *histPkt = &csPtr->clientState.history[idx];
                bool isKeysTick;
                tankButton tb = TNONE;
                bool accel, decel, left, right;

                if (histPkt->tick != tick) continue;

                isKeysTick = (tick % 2) == 1;
                accel = (histPkt->buttons & INPUT_BTN_ACCEL) != 0;
                decel = (histPkt->buttons & INPUT_BTN_DECEL) != 0;
                left  = (histPkt->buttons & INPUT_BTN_LEFT)  != 0;
                right = (histPkt->buttons & INPUT_BTN_RIGHT) != 0;
                if (accel && decel) { accel = FALSE; decel = FALSE; }
                if (left && right)  { left = FALSE;  right = FALSE; }
                if (left && accel)  tb = TLEFTACCEL;
                else if (right && accel) tb = TRIGHTACCEL;
                else if (left && decel)  tb = TLEFTDECEL;
                else if (right && decel) tb = TRIGHTDECEL;
                else if (left)  tb = TLEFT;
                else if (right) tb = TRIGHT;
                else if (accel) tb = TACCEL;
                else if (decel) tb = TDECEL;

                if (isKeysTick) {
                  BYTE bmx = tankGetMX(&MY_TANK(csPtr));
                  BYTE bmy = tankGetMY(&MY_TANK(csPtr));
                  tankTurn(&csPtr->sim, &MY_TANK(csPtr), bmx, bmy, tb);
                } else {
                  tankUpdate(&csPtr->sim, &MY_TANK(csPtr), tb, FALSE, FALSE);
                  /* Simulate fire's effect on reload during replay.
                   * tankUpdate was called with shoot=FALSE to avoid creating
                   * shells, but we must still apply the reload reset so the
                   * client doesn't think it can fire again. */
                  if ((histPkt->actions & INPUT_ACTION_FIRE) &&
                      tankGetReloadTime(&MY_TANK(csPtr)) == 0 &&
                      tankGetShells(&MY_TANK(csPtr)) > 0 &&
                      tankGetArmour(&MY_TANK(csPtr)) <= TANK_FULL_ARMOUR) {
                    tankSetReload(&MY_TANK(csPtr), TANK_RELOAD_TIME);
                    tankSetShells(&MY_TANK(csPtr), tankGetShells(&MY_TANK(csPtr)) - 1);
                  }
                }
              }
            }
            csPtr->sim.isPredicting = FALSE;

            /* If the server's angle matched our prediction (quantized), the
             * reconciliation was triggered only by position drift (e.g. from
             * speed quantization).  Restore the
             * predicted angle + turn ramp-up so the gunsight doesn't flicker
             * from tiny position-induced turn-rate differences during replay. */
            if (!angleMismatch) {
              WORLD finalX, finalY;
              tankGetWorld(&MY_TANK(csPtr), &finalX, &finalY);
              tankSetWorld(&csPtr->sim, &MY_TANK(csPtr), finalX, finalY, savedAngle, FALSE);
              tankSetFirstLeft(&MY_TANK(csPtr), savedFirstLeft);
              tankSetFirstRight(&MY_TANK(csPtr), savedFirstRight);
            }
          }
        }

        /* Detect death/respawn transitions using server armour values
         * (not predicted state, which may already reflect the death) */
        {
          tankSetArmour(&MY_TANK(csPtr), tanks[i].armour);
          if (csPtr->lastServerArmour <= TANK_FULL_ARMOUR && tanks[i].armour > TANK_FULL_ARMOUR) {
            /* alive→dead: set death type for static screen rendering */
            tankSetLastTankDeath(&MY_TANK(csPtr), LAST_DEATH_BY_SHELL);
            tankAddDeath(&csPtr->sim, &MY_TANK(csPtr));
          }
          if (csPtr->lastServerArmour > TANK_FULL_ARMOUR && tanks[i].armour <= TANK_FULL_ARMOUR) {
            /* dead→alive: recenter view on respawn */
            csPtr->sim.inStartFind = FALSE;
            if (isHuman) {
              csPtr->inPillView = FALSE;
              clientCenterTankCS(csPtr);
            }
          }
          csPtr->lastServerArmour = tanks[i].armour;
        }

        /* Sync resources from server — but not reload/shells, which are
         * already set correctly by the reconciliation replay (it accounts
         * for unprocessed fire inputs that the server hasn't seen yet). */
        tankSetShells(&MY_TANK(csPtr), tanks[i].shells);
        tankSetMines(&MY_TANK(csPtr), tanks[i].mines);
        tankSetTrees(&MY_TANK(csPtr), tanks[i].trees);
        tankSetGunsightLength(&MY_TANK(csPtr), tanks[i].gunsightLen);
        tankSetDeathWait(&MY_TANK(csPtr), tanks[i].deathWait);
        tankSetReload(&MY_TANK(csPtr), tanks[i].reload);
        /* Sync boat state from server — prediction skips the boat state
         * machine (isPredicting guard in tankUpdate), so the client's
         * onBoat flag can go stale if no position mismatch triggers
         * reconciliation. Always apply the server's value. */
        {
          BYTE isDead, onBoat;
          utilGetNibbles(tanks[i].tankStatus, &isDead, &onBoat);
          tankSetOnBoat(&MY_TANK(csPtr), onBoat);
        }

        /* Correct shells/reload for any unprocessed fire inputs.
         * The server snapshot reflects state before our fire was processed,
         * so we must re-apply the fire effect to prevent double-firing. */
        {
          uint32_t tick;
          for (tick = csPtr->clientState.oldestUnacked; tick <= csPtr->clientState.newestInput; tick++) {
            uint8_t idx = tick & (CLIENT_INPUT_HISTORY_SIZE - 1);
            InputPacket *histPkt = &csPtr->clientState.history[idx];
            if (histPkt->tick != tick) continue;
            if ((tick % 2) == 1) continue; /* keys tick — no fire */
            /* Decrement reload like tankUpdate would */
            if (tankGetReloadTime(&MY_TANK(csPtr)) > 0) {
              tankSetReload(&MY_TANK(csPtr), tankGetReloadTime(&MY_TANK(csPtr)) - 1);
            }
            if ((histPkt->actions & INPUT_ACTION_FIRE) &&
                tankGetReloadTime(&MY_TANK(csPtr)) == 0 &&
                tankGetShells(&MY_TANK(csPtr)) > 0 &&
                tankGetArmour(&MY_TANK(csPtr)) <= TANK_FULL_ARMOUR) {
              tankSetReload(&MY_TANK(csPtr), TANK_RELOAD_TIME);
              tankSetShells(&MY_TANK(csPtr), tankGetShells(&MY_TANK(csPtr)) - 1);
            }
          }
        }
      }
      continue;
    }

    /* Other player: feed into interpolation system */
    {
      InterpSnapshot snap;
      snap.worldX = tanks[i].worldX;
      snap.worldY = tanks[i].worldY;
      snap.angle = (TURNTYPE)tanks[i].angle / 256.0f;
      snap.speed = (SPEEDTYPE)tanks[i].speed / 256.0f;
      {
        BYTE isDead, onBoat;
        utilGetNibbles(tanks[i].tankStatus, &isDead, &onBoat);
        snap.onBoat = onBoat;
        snap.alive = (isDead == 0);
      }
      snap.lgmMX = tanks[i].lgmMX;
      snap.lgmMY = tanks[i].lgmMY;
      snap.lgmPX = tanks[i].lgmPX;
      snap.lgmPY = tanks[i].lgmPY;
      snap.lgmFrame = tanks[i].lgmFrame > 0 ? tanks[i].lgmFrame - 1 : 0;
      interpUpdate(&csPtr->interpCtx, pn, &snap, hdr->serverTick);

      /* Auto-register player if not yet known */
      if (csPtr->sim.plyrs != NULL && playersIsInUse(&csPtr->sim.plyrs, pn) == FALSE) {
        char name[FILENAME_MAX];
        sprintf(name, "Player %d", pn);
        fprintf(stderr, "[SCREEN] Auto-registering player %d from snapshot (pos=%u,%u)\n",
                pn, tanks[i].worldX, tanks[i].worldY);
        playersSetPlayer(csPtr, &csPtr->sim.plyrs, csPtr->myPlayerNum, pn, name, "??",
                         0, 0, 0, 0, 0, FALSE, 0, NULL, csPtr->isBot);
      }

      /* Store speed on player struct for brain access (brain API uses * 4 scale) */
      if (csPtr->sim.plyrs != NULL) {
        (*csPtr->sim.plyrs).item[pn].speed = (uint8_t)(tanks[i].speed >> 6);
      }

      /* Update players struct for rendering */
      if (csPtr->sim.plyrs != NULL) {
        WORLD interpX, interpY;
        TURNTYPE interpAngle;
        bool interpOnBoat;
        if (interpGetPosition(&csPtr->interpCtx, pn, 1.0f,
                              &interpX, &interpY,
                              &interpAngle, &interpOnBoat)) {
          BYTE mx = (BYTE)(interpX >> TANK_SHIFT_MAPSIZE);
          BYTE px = (BYTE)((interpX & 0xFF) >> TANK_SHIFT_RIGHT2);
          BYTE my = (BYTE)(interpY >> TANK_SHIFT_MAPSIZE);
          BYTE py = (BYTE)((interpY & 0xFF) >> TANK_SHIFT_RIGHT2);
          BYTE frame = utilGetDir(interpAngle);
          playersUpdate(&csPtr->sim.plyrs, pn, mx, my, px, py, frame,
                        interpOnBoat,
                        snap.lgmMX, snap.lgmMY, snap.lgmPX, snap.lgmPY,
                        snap.lgmFrame);
        } else if (!snap.alive) {
          /* Dead player: move tank off-screen but keep LGM visible —
           * the LGM outlives its owner tank and the server still sends
           * its position in every snapshot. */
          playersUpdate(&csPtr->sim.plyrs, pn, 0, 0, 0, 0, 0, FALSE,
                        snap.lgmMX, snap.lgmMY, snap.lgmPX, snap.lgmPY,
                        snap.lgmFrame);
        }
      }
    }
  }

  /* Mark players NOT in the snapshot as missing.  With per-tick stubs the
   * server emits one entry per connected slot, so reaching here means the
   * slot is truly absent (disconnect, never-yet-seen, packet drop on first
   * snapshot).  Mask the stub flag when comparing slot indices. */
  {
    BYTE p;
    for (p = 0; p < MAX_TANKS; p++) {
      bool found = FALSE;
      if (p == playerNum) continue;
      for (i = 0; i < tankCount; i++) {
        BYTE entryPn = (BYTE)(tanks[i].playerNum & TANK_SNAPSHOT_PLAYER_MASK);
        if (entryPn == p) { found = TRUE; break; }
      }
      if (!found) {
        interpMarkMissing(&csPtr->interpCtx, p);
      }
    }
  }

  /* Store server shell snapshot data for rendering.
   * These are rendered directly to screen bullets in screenUpdate(),
   * bypassing the shells linked list (which would apply unwanted offsets).
   *
   * Humans filter out their own shells here because they're rendered via
   * the local prediction layer instead. Bots have no prediction layer
   * and rely on this snapshot for everything they shoot, so KEEP their
   * own shells — without this the brain (info.objects, BrainTest
   * shell-hitbox overlay) never sees shells fired by the bot itself. */
  if (shellSnaps != NULL) {
    int si, di = 0;
    for (si = 0; si < shellCount && di < MAX_SNAPSHOT_SHELLS; si++) {
      if (isHuman && shellSnaps[si].owner == playerNum) continue;
      csPtr->serverShellSnaps[di++] = shellSnaps[si];
    }
    csPtr->serverShellCount = di;
  }

  /* Tank fireballs are spawned via EVENT_TK_EXPLOSION (handled below) and
   * simulated locally by tkExplosionUpdate — no per-tick replication. */
  (void)tkExplSnaps; (void)tkExplosionCount;

  /* Update own LGM from snapshot data */
  for (i = 0; i < tankCount; i++) {
    if (tanks[i].playerNum == playerNum && MY_LGM(csPtr) != NULL) {
      if (tanks[i].lgmFrame > 0) {
        /* LGM is out on server — lgmFrame is encoded as frame+1 (1-3),
         * so 0 means idle and >0 reliably means out */
        BYTE actualFrame = tanks[i].lgmFrame - 1;
        WORLD lgmWX = (WORLD)((tanks[i].lgmMX << 8) + (tanks[i].lgmPX << 4));
        WORLD lgmWY = (WORLD)((tanks[i].lgmMY << 8) + (tanks[i].lgmPY << 4));
        MY_LGM(csPtr)->inTank = FALSE;
        MY_LGM(csPtr)->isDead = (actualFrame == LGM_HELICOPTER_FRAME) ? TRUE : FALSE;
        MY_LGM(csPtr)->frame = actualFrame;
        lgmPutWorld(&MY_LGM(csPtr), lgmWX, lgmWY, actualFrame);
      } else {
        /* LGM is idle/in tank on server — sync client state */
        MY_LGM(csPtr)->inTank = TRUE;
        MY_LGM(csPtr)->state = LGM_STATE_IDLE;
      }
      break;
    }
  }

  /* Apply base snapshots */
  if (baseSnaps != NULL && csPtr->sim.bs != NULL) {
    for (i = 0; i < baseCount && i < MAX_BASES; i++) {
      (*csPtr->sim.bs).item[i].owner = baseSnaps[i].owner;
      (*csPtr->sim.bs).item[i].armour = baseSnaps[i].armour;
      (*csPtr->sim.bs).item[i].shells = baseSnaps[i].shells;
      (*csPtr->sim.bs).item[i].mines = baseSnaps[i].mines;
    }
  }

  /* Apply pill snapshots */
  if (pillSnaps != NULL && csPtr->sim.pb != NULL) {
    for (i = 0; i < pillCount && i < MAX_PILLS; i++) {
      (*csPtr->sim.pb).item[i].x = pillSnaps[i].x;
      (*csPtr->sim.pb).item[i].y = pillSnaps[i].y;
      (*csPtr->sim.pb).item[i].owner = pillSnaps[i].owner;
      (*csPtr->sim.pb).item[i].armour = pillSnaps[i].armour;
      (*csPtr->sim.pb).item[i].speed = pillSnaps[i].speed;
      (*csPtr->sim.pb).item[i].inTank = pillSnaps[i].inTank ? TRUE : FALSE;
    }
  }

  /* Store server tick for brain access */
  csPtr->lastServerTick = hdr->serverTick;

  /* Buffer events for brain consumption (accumulate across syncs;
   * reset happens when the brain consumes them in screenMakeBrainInfoCS) */
  if (events != NULL) {
    for (i = 0; i < eventCount; i++) {
      switch (events[i].type) {
      case EVENT_SOUND:
      case EVENT_SOUND_SHOOT:
      case EVENT_SOUND_TANK_HIT:
      case EVENT_PILL_CAPTURED:
      case EVENT_BASE_CAPTURED:
      case EVENT_TANK_KILLED:
      case EVENT_LGM_LOST:
      case EVENT_PLAYER_LEAVE:
      case EVENT_PILL_UPDATE:
      case EVENT_BASE_UPDATE:
      case EVENT_EXPLOSION:
        if (csPtr->brainEventCount < MAX_BRAIN_EVENTS) {
          csPtr->brainEvents[csPtr->brainEventCount++] = events[i];
        }
        break;
      case EVENT_ASSISTANT_MSG:
        if (events[i].data[0] == playerNum) {
          csPtr->brainLastAssistMsg = events[i].data[1];
        }
        if (csPtr->brainEventCount < MAX_BRAIN_EVENTS) {
          csPtr->brainEvents[csPtr->brainEventCount++] = events[i];
        }
        break;
      default:
        break;
      }
    }
  }

  /* Process game events (MAP_CHANGE, SOUND — all reliable) */
  bool steamStatsUpdated = false;
  if (events != NULL) {
    for (i = 0; i < eventCount; i++) {
      switch (events[i].type) {
      case EVENT_MAP_CHANGE:
        /* data: [mx, my, newTerrain] */
        if (csPtr->sim.mp != NULL) {
          mapSetPos(&csPtr->sim, &csPtr->sim.mp, events[i].data[0], events[i].data[1],
                    events[i].data[2], FALSE, TRUE);
          screenReCalcCS(csPtr);
        }
        break;
      case EVENT_SOUND:
        /* data: [soundId, mx, my, sourcePlayer] — play with distance attenuation.
         * All sounds are now server-authoritative (isPredicting suppresses
         * prediction-side sounds), so no filtering needed. */
        if (isHuman) {
          clientSoundDist(&csPtr->sim, (sndEffects)events[i].data[0], events[i].data[1], events[i].data[2]);
        }
        break;
      case EVENT_SOUND_SHOOT:
        /* data: [soundId, mx, my, firingPlayer] — skip own shots (client plays shootSelf via prediction) */
        if (isHuman && events[i].data[3] != csPtr->myPlayerNum) {
          clientSoundDist(&csPtr->sim, shootNear, events[i].data[1], events[i].data[2]);
        }
        break;
      case EVENT_SOUND_TANK_HIT:
        /* data: [soundId, mx, my, hitPlayer] */
        if (isHuman) {
          if (events[i].data[3] == csPtr->myPlayerNum) {
            frontEndPlaySound(hitTankSelf);
          } else {
            clientSoundDist(&csPtr->sim, hitTankNear, events[i].data[1], events[i].data[2]);
          }
        }
        break;
      case EVENT_EXPLOSION:
        /* data: [mx, my, px, py] — create explosion locally */
        explosionsAddItem(&csPtr->sim.expl,
                           events[i].data[0], events[i].data[1],
                           events[i].data[2], events[i].data[3],
                           EXPLOSION_START);
        break;
      case EVENT_TK_EXPLOSION: {
        /* data: [xHi, xLo, yHi, yLo, angle, length, explodeType, creator]
         * Spawn the fireball locally; tkExplosionUpdate animates it. */
        WORLD tkX = (WORLD)(((uint16_t)events[i].data[0] << 8) | events[i].data[1]);
        WORLD tkY = (WORLD)(((uint16_t)events[i].data[2] << 8) | events[i].data[3]);
        TURNTYPE tkAngle = (TURNTYPE)events[i].data[4];
        BYTE tkLength = events[i].data[5];
        BYTE tkType = events[i].data[6];
        BYTE tkCreator = events[i].data[7];
        tkExplosionAddItemFromSnapshot(&csPtr->sim, tkX, tkY, tkAngle,
                                       tkLength, tkType, tkCreator);
        break;
      }
      case EVENT_BASE_CAPTURED:
        /* data: [newOwner, previousOwner] */
        if (isHuman) {
          MessageArgs args;
          memset(&args, 0, sizeof(args));
          playersMakeMessageName(csPtr, &csPtr->sim.plyrs, csPtr->myPlayerNum, events[i].data[0], args.playerName);
          args.playerFlags = playersGetAccountFlags(&csPtr->sim.plyrs, events[i].data[0]);
          playersGetCountryCode(&csPtr->sim.plyrs, events[i].data[0], args.playerCountry);
          if (events[i].data[1] != NEUTRAL) {
            playersGetPlayerName(&csPtr->sim.plyrs, events[i].data[1], args.otherName, FALSE);
            args.otherFlags = playersGetAccountFlags(&csPtr->sim.plyrs, events[i].data[1]);
            playersGetCountryCode(&csPtr->sim.plyrs, events[i].data[1], args.otherCountry);
            csPtr->sim.callbacks.messageAdd(csPtr->sim.callbacks.ctx, newsWireMessage, MESSAGE_NEWSWIRE, MESSAGE_STOLE_BASE, &args);
          } else {
            csPtr->sim.callbacks.messageAdd(csPtr->sim.callbacks.ctx, newsWireMessage, MESSAGE_NEWSWIRE, MESSAGE_CAPTURE_BASE, &args);
          }
        }
        /* Steam stat: base captures */
        if (events[i].data[0] == playerNum) {
          if (events[i].data[1] == NEUTRAL) {
            steam_increment_stat("STAT_BASES_CAPTURED_NEUTRAL", 1);
            steamStatsUpdated = true;
          } else if (playersIsAllie(&csPtr->sim.plyrs, playerNum, events[i].data[1]) == FALSE) {
            steam_increment_stat("STAT_BASES_CAPTURED_ENEMY", 1);
            steamStatsUpdated = true;
          }
        }
        /* Steam achievement: first base capture (networked, non-allied) */
        if (csPtr->networkGameType != netSingle &&
            csPtr->hasAnyBaseCaptured == false &&
            events[i].data[0] == playerNum &&
            events[i].data[1] != NEUTRAL &&
            playersIsAllie(&csPtr->sim.plyrs, playerNum, events[i].data[1]) == FALSE) {
          steam_set_achievement("ACH_FIRST_BASE");
          steamStatsUpdated = true;
        }
        csPtr->hasAnyBaseCaptured = true;
        break;
      case EVENT_PILL_CAPTURED:
        /* data: [newOwner, previousOwner] */
        if (isHuman) {
          MessageArgs args;
          memset(&args, 0, sizeof(args));
          playersMakeMessageName(csPtr, &csPtr->sim.plyrs, csPtr->myPlayerNum, events[i].data[0], args.playerName);
          args.playerFlags = playersGetAccountFlags(&csPtr->sim.plyrs, events[i].data[0]);
          playersGetCountryCode(&csPtr->sim.plyrs, events[i].data[0], args.playerCountry);
          if (events[i].data[1] != NEUTRAL) {
            playersGetPlayerName(&csPtr->sim.plyrs, events[i].data[1], args.otherName, FALSE);
            args.otherFlags = playersGetAccountFlags(&csPtr->sim.plyrs, events[i].data[1]);
            playersGetCountryCode(&csPtr->sim.plyrs, events[i].data[1], args.otherCountry);
            csPtr->sim.callbacks.messageAdd(csPtr->sim.callbacks.ctx, newsWireMessage, MESSAGE_NEWSWIRE, MESSAGE_STOLE_PILL, &args);
          } else {
            csPtr->sim.callbacks.messageAdd(csPtr->sim.callbacks.ctx, newsWireMessage, MESSAGE_NEWSWIRE, MESSAGE_CAPTURE_PILL, &args);
          }
        }
        /* Steam stat: pill captures */
        if (events[i].data[0] == playerNum) {
          if (events[i].data[1] == NEUTRAL) {
            steam_increment_stat("STAT_PILLS_CAPTURED_NEUTRAL", 1);
            steamStatsUpdated = true;
          } else if (playersIsAllie(&csPtr->sim.plyrs, playerNum, events[i].data[1]) == FALSE) {
            steam_increment_stat("STAT_PILLS_CAPTURED_ENEMY", 1);
            steamStatsUpdated = true;
          }
        }
        /* Steam achievement: first pill capture (networked, non-allied) */
        if (csPtr->networkGameType != netSingle &&
            csPtr->hasAnyPillCaptured == false &&
            events[i].data[0] == playerNum &&
            events[i].data[1] != NEUTRAL &&
            playersIsAllie(&csPtr->sim.plyrs, playerNum, events[i].data[1]) == FALSE) {
          steam_set_achievement("ACH_FIRST_PILL");
          steamStatsUpdated = true;
        }
        csPtr->hasAnyPillCaptured = true;
        break;
      case EVENT_PILL_UPDATE:
        /* data: [pillIndex, x, y, owner, armour, speed, inTank] */
        {
          BYTE idx = events[i].data[0];
          if (idx < MAX_PILLS && csPtr->sim.pb != NULL) {
            (*csPtr->sim.pb).item[idx].x      = events[i].data[1];
            (*csPtr->sim.pb).item[idx].y      = events[i].data[2];
            (*csPtr->sim.pb).item[idx].owner  = events[i].data[3];
            (*csPtr->sim.pb).item[idx].armour = events[i].data[4];
            (*csPtr->sim.pb).item[idx].speed  = events[i].data[5];
            (*csPtr->sim.pb).item[idx].inTank = events[i].data[6] ? TRUE : FALSE;
          }
        }
        break;
      case EVENT_BASE_UPDATE:
        /* data: [baseIndex, owner, armour, shells, mines] */
        {
          BYTE idx = events[i].data[0];
          if (idx < MAX_BASES && csPtr->sim.bs != NULL) {
            (*csPtr->sim.bs).item[idx].owner  = events[i].data[1];
            (*csPtr->sim.bs).item[idx].armour = events[i].data[2];
            (*csPtr->sim.bs).item[idx].shells = events[i].data[3];
            (*csPtr->sim.bs).item[idx].mines  = events[i].data[4];
          }
        }
        break;
      case EVENT_PLAYER_LEAVE:
        /* data: [playerNum] — server says this player disconnected */
        {
          BYTE leavePlayer = events[i].data[0];
          if (leavePlayer < MAX_TANKS && csPtr->sim.plyrs != NULL &&
              playersIsInUse(&csPtr->sim.plyrs, leavePlayer) == TRUE) {
            playersLeaveGame(&csPtr->sim, &csPtr->sim.plyrs, csPtr->myPlayerNum, leavePlayer, FALSE);
          }
        }
        break;
      case EVENT_SERVER_MSG:
        /* data: [msgId] — server status message (human only) */
        if (isHuman) {
          switch (events[i].data[0]) {
          case SERVER_MSG_GAME_LOCKED:
            screenNetStatusMessage(csPtr, "This game is now locked to new players (server lock)");
            break;
          case SERVER_MSG_GAME_UNLOCKED:
            screenNetStatusMessage(csPtr, "This game is now unlocked to new players (server unlock)");
            break;
          }
        }
        break;
      case EVENT_LGM_LOST:
        /* data: [victim, killer] — builder killed, broadcast newswire */
        if (isHuman) {
          MessageArgs args;
          memset(&args, 0, sizeof(args));
          playersGetPlayerName(&csPtr->sim.plyrs, events[i].data[0], args.playerName, FALSE);
          args.playerFlags = playersGetAccountFlags(&csPtr->sim.plyrs, events[i].data[0]);
          playersGetCountryCode(&csPtr->sim.plyrs, events[i].data[0], args.playerCountry);
          csPtr->sim.callbacks.messageAdd(csPtr->sim.callbacks.ctx, newsWireMessage, MESSAGE_NEWSWIRE, MESSAGE_LGM_DEAD, &args);
        }
        /* Steam stats: LGM losses and kills */
        if (events[i].data[0] == playerNum) {
          steam_increment_stat("STAT_LGM_LOSSES", 1);
          steamStatsUpdated = true;
          csPtr->myLgmLossesThisGame++;
        }
        if (events[i].data[1] == playerNum && events[i].data[0] != playerNum) {
          /* No stats for killing your own LGM */
          steam_increment_stat("STAT_LGM_KILLS", 1);
          steamStatsUpdated = true;
        }
        break;
      case EVENT_ASSISTANT_MSG:
        /* data: [targetPlayer, msgId] — player-specific assistant message */
        if (isHuman && events[i].data[0] == playerNum) {
          langid assistLangId = 0;
          switch (events[i].data[1]) {
          case ASSIST_MSG_MAN_DEAD:          assistLangId = LGM_MAN_DEAD; break;
          case ASSIST_MSG_NO_TREE:           assistLangId = LGM_NO_TREE; break;
          case ASSIST_MSG_NO_BUILD:          assistLangId = LGM_NO_BUILD; break;
          case ASSIST_MSG_NO_BUILD_BOAT:     assistLangId = LGM_NO_BUILD_UNDER_BOAT; break;
          case ASSIST_MSG_INSUFFICIENT_TREES: assistLangId = LGM_INSUFFICIENT_TREES; break;
          case ASSIST_MSG_BUILDTANK:         assistLangId = LGM_BUILDTANK; break;
          case ASSIST_MSG_PILL_NO_REPAIR:    assistLangId = LGM_PILL_NO_NEED_REPAIR; break;
          case ASSIST_MSG_NO_PILLS:          assistLangId = LGM_NO_PILLS; break;
          case ASSIST_MSG_INSUFFICIENT_MINES: assistLangId = LGM_INSUFFICIENT_MINES; break;
          case ASSIST_MSG_PILL_ON_MINE:      assistLangId = LGM_PILL_NO_BUILD_ON_MINE; break;
          case ASSIST_MSG_TANK_SUNK:         assistLangId = MESSAGE_TANKSUNK; break;
          }
          if (assistLangId != 0) {
            csPtr->sim.callbacks.messageAdd(csPtr->sim.callbacks.ctx, assistantMessage, MESSAGE_ASSISTANT, assistLangId, NULL);
          }
        }
        break;
      case EVENT_TANK_KILLED:
        /* data: [killer, killed, deathCause, carriedPills] */
        if (events[i].data[0] == playerNum && events[i].data[0] != events[i].data[1]) {
          tankAddKill(&csPtr->sim, &MY_TANK(csPtr));
          steam_increment_stat("STAT_TANK_KILLS", 1);
          steamStatsUpdated = true;
        }
        if (events[i].data[1] == playerNum) {
          csPtr->myDeathsThisGame++;
          /* Steam achievements: drown */
          if (events[i].data[2] == LAST_DEATH_BY_DEEPSEA) {
            steam_set_achievement("ACH_DROWN");
            if (events[i].data[3] > 0) {
              steam_set_achievement("ACH_DROWN_WITH_PILLS");
            }
            steamStatsUpdated = true;
          }
          /* Rapid death tracking (ACH_RAPID_DEATH) */
          {
            uint32_t now = (uint32_t)SDL_GetTicks();
            csPtr->deathTimestamps[csPtr->deathTimestampIdx] = now;
            csPtr->deathTimestampIdx = (csPtr->deathTimestampIdx + 1) % 10;
            /* Check if 10 deaths within 45 seconds */
            uint32_t oldest = csPtr->deathTimestamps[csPtr->deathTimestampIdx];
            if (oldest != 0) {
              uint32_t newest = csPtr->deathTimestamps[(csPtr->deathTimestampIdx + 9) % 10];
              if (newest - oldest < 45000) {
                steam_set_achievement("ACH_RAPID_DEATH");
                steamStatsUpdated = true;
              }
            }
          }
        }
        break;
      case EVENT_MINE_VISIBLE:
        /* data: [mx, my, sourcePlayer] — reveal mine at position */
        minesAddItem(&csPtr->sim.mns, events[i].data[0], events[i].data[1]);
        screenReCalcCS(csPtr);
        break;
      default:
        break;
      }
    }
  }

  /* Player count tracking (ACH_PLAYERS_6/8/16) */
  {
    BYTE numPlayers = playersGetNumPlayers(&csPtr->sim.plyrs);
    if (numPlayers > csPtr->maxPlayersSeenThisGame) {
      csPtr->maxPlayersSeenThisGame = numPlayers;
      if (numPlayers >= 16) {
        steam_set_achievement("ACH_PLAYERS_16");
        steam_set_achievement("ACH_PLAYERS_8");
        steam_set_achievement("ACH_PLAYERS_6");
        steamStatsUpdated = true;
      } else if (numPlayers >= 8) {
        steam_set_achievement("ACH_PLAYERS_8");
        steam_set_achievement("ACH_PLAYERS_6");
        steamStatsUpdated = true;
      } else if (numPlayers >= 6) {
        steam_set_achievement("ACH_PLAYERS_6");
        steamStatsUpdated = true;
      }
    }
  }

  if (steamStatsUpdated) {
    steam_store_stats();
  }

  /* Check map checksum on full sync ticks — must be after EVENT_MAP_CHANGE
   * processing so the client map includes changes from this snapshot */
  if (hdr->mapChecksum != 0) {
    uint16_t clientChecksum = mapCalcChecksum(&csPtr->sim.mp);
    if (clientChecksum != hdr->mapChecksum) {
      fprintf(stderr, "[SCREEN] Map checksum mismatch: server=%04x client=%04x\n",
              hdr->mapChecksum, clientChecksum);
    }
  }

  /* Invalidate tile cache after applying snapshot state (human only) */
  if (isHuman) {
    csPtr->needScreenReCalc = TRUE;
  }
}
