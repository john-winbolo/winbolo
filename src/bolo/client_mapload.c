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
 *Name:          Client Map Load
 *Filename:      client_mapload.c
 *Purpose:
 *  Map-load entry points that create the ClientSim,
 *  populate it from a map file (or compressed buffer), wire
 *  up the local tank, and prime the viewport. Also save and
 *  preview helpers.
 *********************************************************/

#include <string.h>
#include "client_mapload.h"
#include "client_sim_internal.h"
#include "global.h"
#include "bolo_map.h"
#include "bases.h"
#include "pillbox.h"
#include "starts.h"
#include "tank.h"
#include "viewport.h"
#include "util.h"
#include "frontend.h"
#include "players.h"
#include "messages.h"
#include "player_flags.h"
#include "screen.h"

/*********************************************************
 *NAME:          setupClientSim
 *PURPOSE:
 *  Initializes simulation state, the viewport, and the
 *  display fields on a freshly allocated ClientSim. Used by
 *  the two map-load entry points below.
 *********************************************************/
static void setupClientSim(ClientSim *csPtr, gameType game, bool hiddenMines, int srtDelay, int32_t gmeLen) {
  /* Initialize simulation state in ClientSim */
  clientSimCreate(csPtr, game, hiddenMines, srtDelay, gmeLen);

  /* Initialize rendering state */
  viewportInit(clientSimViewportMut(csPtr));

  /* Initialize display variables */
  clientSimGetMapNameMutable(csPtr)[0] = '\0';
  clientSimSetGmeStartDelay(csPtr, srtDelay);
  clientSimSetGmeLength(csPtr, gmeLen);
  clientSimSetTimeStart(csPtr, 0);
}

/*********************************************************
 *NAME:          clientLoadMap
 *PURPOSE:
 *  Loads a map. Returns if it was sucessful reading the
 *  map or not.
 *
 *ARGUMENTS:
 * fileName - File name and path to map to open
 *  game - The game type-Open/tournament/strict tournament
 *  hiddenMines - Are hidden mines allowed
 *  srtDelay    - Game start delay (50th second increments)
 *  gmeLen      - Length of the game (in 50ths)
 *                (-1 =unlimited)
 *  playerName  - Name of the player
 *  wantFree    - Should we free the backend after loading
 *                Usually TRUE if you only want to check
 *                if a map is valid
 *********************************************************/
bool clientLoadMap(ClientSim *csPtr, char *fileName, gameType game, bool hiddenMines, int32_t srtDelay, int32_t gmeLen, char *playerName, bool wantFree) {
  bool returnValue; /* Value to return */
  bool doneFree;    /* If we have done the free */

  returnValue = FALSE;
  doneFree = FALSE;
  setupClientSim(csPtr, game, hiddenMines, srtDelay, gmeLen);
  returnValue = mapRead(fileName, &clientSimGetGameSim(csPtr)->mp, &clientSimGetGameSim(csPtr)->pb, &clientSimGetGameSim(csPtr)->bs, &clientSimGetGameSim(csPtr)->ss);

  if (returnValue == TRUE) {
    utilExtractMapName(fileName, clientSimGetMapNameMutable(csPtr));
    utilStripNameReplace(playerName);
    {
      /* Self is the local source of truth for client identity — PLAYER_JOINED
       * and PLAYER_LIST receive paths skip self, so without this the local
       * row would stay CLIENT_TYPE_UNKNOWN. */
      uint8_t selfType  = bolo_detect_client_type();
      uint8_t selfFlags = 0;
#ifdef HAVE_STEAM
      selfFlags |= PLAYER_FLAG_STEAM_BUILD;
#endif
      if (bolo_steam_has_supporter_dlc()) selfFlags |= PLAYER_FLAG_SUPPORTER;

      clientSimSetupSelf(csPtr, 0, playerName, selfType, selfFlags);

      { BYTE sh, mi, ar, tr;
        tankGetStats(&MY_TANK(csPtr), &sh, &mi, &ar, &tr);
        frontEndUpdateTankStatusBars(csPtr, sh, mi, ar, tr);
      }
      frontEndSetPlayer(csPtr, (playerNumbers) clientSimGetMyPlayerNum(csPtr), playerName, "", 0, selfType, selfFlags);
    }
    screenUpdateViewCS(csPtr, redraw);
    basesClearMines(clientSimGetGameSim(csPtr));

  } else {
    clientSimDestroy(csPtr);
    doneFree = TRUE;
  }
  if (doneFree == FALSE && wantFree == TRUE) {
    clientSimDestroy(csPtr);
  }

  return returnValue;
}


bool clientLoadCompressedMap(ClientSim *csPtr, BYTE *buff, int buffLen, char *mapn, gameType game, bool hiddenMines, int32_t srtDelay, int32_t gmeLen, char *playerName, BYTE playerNum, bool wantFree) {
  bool returnValue;
  bool doneFree = FALSE;

  setupClientSim(csPtr, game, hiddenMines, srtDelay, gmeLen);
  /* clientSimCreate (inside setupClientSim) resets myPlayerNum to 0.
   * Restore the correct player number before creating the tank so it
   * ends up in the right sim.tanks[] slot. This also moves the LGM
   * and sets the base refuel timer for the correct index. */
  if (playerNum != 0) {
    clientSimSetPlayerNum(csPtr, playerNum);
  }
  returnValue = mapLoadCompressedMap(&clientSimGetGameSim(csPtr)->mp, &clientSimGetGameSim(csPtr)->pb, &clientSimGetGameSim(csPtr)->bs, &clientSimGetGameSim(csPtr)->ss, buff, buffLen);

  if (returnValue == TRUE) {
    strncpy(clientSimGetMapNameMutable(csPtr), mapn, MAP_STR_SIZE - 1);
    clientSimGetMapNameMutable(csPtr)[MAP_STR_SIZE - 1] = '\0';
    utilStripNameReplace(playerName);
    {
      uint8_t selfType  = bolo_detect_client_type();
      uint8_t selfFlags = 0;
#ifdef HAVE_STEAM
      selfFlags |= PLAYER_FLAG_STEAM_BUILD;
#endif
      if (bolo_steam_has_supporter_dlc()) selfFlags |= PLAYER_FLAG_SUPPORTER;

      clientSimSetupSelf(csPtr, playerNum, playerName, selfType, selfFlags);

      { BYTE sh, mi, ar, tr;
        tankGetStats(&MY_TANK(csPtr), &sh, &mi, &ar, &tr);
        frontEndUpdateTankStatusBars(csPtr, sh, mi, ar, tr);
      }
      frontEndSetPlayer(csPtr, (playerNumbers) clientSimGetMyPlayerNum(csPtr), playerName, "", 0, selfType, selfFlags);
    }
    screenUpdateViewCS(csPtr, redraw);
    basesClearMines(clientSimGetGameSim(csPtr));
  } else {
    clientSimDestroy(csPtr);
    doneFree = TRUE;
  }
  if (doneFree == FALSE && wantFree == TRUE) {
    clientSimDestroy(csPtr);
  }
  return returnValue;
}

/*********************************************************
 *NAME:          clientSaveMap
 *PURPOSE:
 * Saves the map. Returns whether the operation was
 * sucessful or not.
 *
 *ARGUMENTS:
 *  fileName - path and filename to save
 *********************************************************/
bool clientSaveMap(ClientSim *csPtr, char *fileName) {
  bool returnValue;                 /* Value to return */

  returnValue = mapWrite(fileName, &clientSimGetGameSim(csPtr)->mp, &clientSimGetGameSim(csPtr)->pb, &clientSimGetGameSim(csPtr)->bs, &clientSimGetGameSim(csPtr)->ss);
  if (returnValue == TRUE) {
    if (clientSimGetNetType(csPtr) == netSingle) {
      MessageArgs args;
      memset(&args, 0, sizeof(args));
      playersMakeMessageName(csPtr, &clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), clientSimGetMyPlayerNum(csPtr), args.playerName);
      args.playerFlags = playersGetAccountFlags(&clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr));
      playersGetCountryCode(&clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), args.playerCountry);
      clientSimGetGameSim(csPtr)->callbacks.messageAdd(clientSimGetGameSim(csPtr)->callbacks.ctx, newsWireMessage, MESSAGE_NEWSWIRE, MESSAGE_SAVED_MAP, &args);
    }
  }
  return returnValue;
}

/*********************************************************
 *NAME:          clientGenerateMapPreview
 *PURPOSE:
 * Generates a map preview for the front end. A map preview
 * is a 256x256 byte buff with each square equal to a map
 * tile. tile numbers are identical to map square numbers
 * except DEEP_SEA = 16, pills = 17, bases = 18 and starts
 * = 19. Returns success
 *
 *ARGUMENTS:
 *  fileName - Map file name to open
 *  buff     - Buffer to copy into
 *********************************************************/
bool clientGenerateMapPreview(char *fileName, BYTE *buff) {
  bool returnValue; /* Value to return */
  map prevmp;       /* Items in loading */
  bases prevbs;
  pillboxes prevpb;
  starts prevss;
  BYTE xValue;           /* Looping variables */
  BYTE yValue;
  BYTE *ptr;


  /* Create */
  mapCreate(&prevmp);
  startsCreate(&prevss);
  basesCreate(&prevbs);
  pillsCreate(&prevpb);

  /* Load */
  returnValue = mapRead(fileName, &prevmp, &prevpb, &prevbs, &prevss);
  if (returnValue == TRUE) {
    /* Make preview map info */
    xValue = 0;
    yValue = 0;
    ptr = buff;
    /* Set up Items */
    while (yValue < 255) {
      while (xValue < 255) {
        if ((pillsExistPos(&prevpb, xValue, yValue)) == TRUE) {
          *ptr = 17;
        } else if ((basesExistPos(&prevbs, xValue, yValue)) == TRUE) {
          *ptr = 18;
        } else if ((startsExistPos(&prevss, xValue, yValue)) == TRUE) {
          *ptr = 19;
        } else {
          *ptr = mapGetPos(&prevmp, xValue, yValue);
          if (*ptr == DEEP_SEA) {
            *ptr = 16;
          }

        }
        xValue++;
        ptr++;
      }
      xValue = 0;
      yValue++;
    }
  }

  /* Clean up */
  mapDestroy(&prevmp);
  startsDestroy(&prevss);
  basesDestroy(&prevbs);
  pillsDestroy(&prevpb);

  return returnValue;

}
