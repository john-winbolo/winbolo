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
  BYTE count1; /* Looping variable */
  BYTE count2; /* Looping variable */
  BYTE pos;    /* Upto position    */
  GameSim *gs = clientSimGetGameSim(cs);

  pos = 0;
  for (count1=topPos;count1<=bottomPos;count1++) {
    for (count2=leftPos;count2<=rightPos;count2++) {
      if (basesExistPos(&gs->bs, count2, count1) == TRUE) {
        buff[pos] = BREFBASE_T;
      } else if (pillsViewExistPos(&gs->pb, count2, count1) == TRUE) {
        /* The brain's view of the map is the bot's screen, so it shows a pill
         * at the square it was last seen on, exactly as a human's does. The
         * bot's movement still asks pillsExistPos through mapGetSpeed and
         * friends, so it is no more blocked by one than a human is. */
        buff[pos] = BPILLBOX_T;
      } else {
        buff[pos] = mapGetPos(&gs->mp, count2, count1);
        if (buff[pos] == DEEP_SEA) {
          buff[pos] = BDEEPSEA;
        } else if (buff[pos] >= MINE_START && buff[pos] <= MINE_END) {
          buff[pos] = buff[pos] - MINE_SUBTRACT;
        }
        if (minesExistPos(&gs->mns, &gs->mp, count2, count1) == TRUE) {
          buff[pos] |= TERRAIN_MINE;
        }
      }
      pos++;
    }
  }
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


  if (MY_TANK(csPtr) == NULL) {
    return;
  }

  tx = tankGetMX(&MY_TANK(csPtr));
  ty = tankGetMY(&MY_TANK(csPtr));

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


  /* Count carried pills from pillbox state (server syncs inTank via snapshots/events) */
  {
    BYTE selfPlayer = clientSimGetMyPlayerNum(csPtr);
    BYTE numPb = pillsGetNumPills(&gs->pb);
    BYTE carried = 0;
    for (BYTE pi = 0; pi < numPb; pi++) {
      if ((*gs->pb).item[pi].inTank && (*gs->pb).item[pi].owner == selfPlayer) {
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
          /* Only include if pill is within view rect */
          BYTE px = e->data[1], py = e->data[2];
          if (px >= value->view_left && px <= value->view_left + value->view_width &&
              py >= value->view_top && py <= value->view_top + value->view_height) {
            buf[filtered++] = *e;
          }
        }
        break;
      case EVENT_BASE_UPDATE:
      case EVENT_BASE_STOCK:
        if (aiMode == aiYesAdvantage || aiMode == aiFull) {
          buf[filtered++] = *e;
        } else {
          /* Look up base position and check view rect */
          BYTE idx = e->data[0];
          if (idx < MAX_BASES && gs->bs != NULL) {
            BYTE bx = (*gs->bs).item[idx].x;
            BYTE by = (*gs->bs).item[idx].y;
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
    *(value->pillview) = 0x8000;
    value->view_left = tx-14;
    value->view_width = 29;
    value->view_top = ty-14;
    value->view_height = 29;
  }
  //value->viewdata = malloc((value->view_width+1) * (value->view_height+1));
  value->viewdata = malloc(30 * 30);
  brainDataMakeViewData(csPtr, value->viewdata, value->view_left, (BYTE) (value->view_left+value->view_width), value->view_top, (BYTE) (value->view_top+value->view_height));

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
  /* Bots have no client-side prediction layer that fills sim.shs, so
   * shellsGetBrainShellsInRect above adds nothing for bot players.
   * Mirror the snapshot shells (now retained for bots — see
   * clientApplySnapshot) into the brain object array so
   * info.objects actually contains type=OBJECT_SHOT entries the
   * brain (and BrainTest's shell-hitbox overlay) can render. */
  if (clientSimIsBot(csPtr)) {
    BYTE leftPos   = value->view_left;
    BYTE rightPos  = (BYTE)(value->view_left  + value->view_width);
    BYTE topPos    = value->view_top;
    BYTE bottomPos = (BYTE)(value->view_top   + value->view_height);
    BYTE myPN      = clientSimGetMyPlayerNum(csPtr);
    int shellCount = clientSimGetServerShellCount(csPtr);
    const ShellSnapshot *shellSnaps = clientSimGetServerShellSnaps(csPtr);
    for (int i = 0; i < shellCount; i++) {
      const ShellSnapshot *s = &shellSnaps[i];
      BYTE smx = (BYTE)(s->worldX >> TANK_SHIFT_MAPSIZE);
      BYTE smy = (BYTE)(s->worldY >> TANK_SHIFT_MAPSIZE);
      if (smx < leftPos || smx > rightPos || smy < topPos || smy > bottomPos)
        continue;
      BYTE owner;
      if (s->owner == NEUTRAL) owner = SHELLS_BRAIN_NEUTRAL;
      else if (playersIsAllie(&gs->plyrs, myPN, s->owner) == TRUE)
        owner = SHELLS_BRAIN_FRIENDLY;
      else
        owner = SHELLS_BRAIN_HOSTILE;
      brainDataAddObject(csPtr, SHELLS_BRAIN_OBJECT_TYPE,
                         s->worldX, s->worldY, 0,
                         utilGet16Dir((TURNTYPE)s->angle),
                         owner, 0);
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

  /* Controling the tank */
  *clientSimGetBrainHoldKeys(csPtr) = *(value->holdkeys);
  *clientSimGetBrainTapKeys(csPtr) = *(value->tapkeys);

  /* Build requests are routed through InputPacket so the server sim
   * processes them authoritatively.  brainBuildInfo->action is 1-based
   * (0=none, 1=BsTrees, 2=BsRoad, ...) and clientBuildInputPacket
   * will pick it up and put it in the InputPacket as-is.  The server
   * sim decrements to 0-based before passing to lgmAddRequest. */
  if (value->build->action != 0) {
    if (tankGetArmour(&MY_TANK(csPtr)) > TANK_FULL_ARMOUR) {
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
        botManagerDeliverInternalMessage(bound,
                                         clientSimGetMyPlayerNum(csPtr), msg);
      }
    } else {
      /* Send this message to the appropriate players */
      playersSendAiMessage(csPtr, gs, &gs->plyrs, *(value->messagedest), msg);
    }
    clientSimGetBrainsMessage(csPtr)[0] = '\0';
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
  objects[*numObjects].object = object;
  objects[*numObjects].x = wx;
  objects[*numObjects].y = wy;
  objects[*numObjects].idnum = idNum;
  objects[*numObjects].direction = dir;
  objects[*numObjects].info = info;
  objects[*numObjects].speed = speed;
  (*numObjects)++;
}
