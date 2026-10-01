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

/*********************************************************
 *Name:          Client Frontend Single Player
 *Filename:      client_frontend_sp.c
 *Author:        John Morrison
 *Purpose:
 *  The single-player lobby seeding every frontend that
 *  hosts a local game calls: one bot into one slot, the
 *  same steps in the same order on desktop and web. See
 *  client_frontend_sp.h.
 *********************************************************/

#include <string.h>
#include <SDL3/SDL.h>

#include "client_frontend_sp.h"
#include "client_net.h"   /* clientSimNetSendTeamSet */

void clientFrontSeedBot(ServerSim *srv, ClientSim *cs, BYTE slot,
                        const char *brainPath, const char *botName,
                        aiType aiPolicy, gameType game, bool hiddenMines,
                        BYTE team, uint8_t spMode, uint8_t spLevel) {
  const BrainList *bl;

  /* Resolved through the one rule a new bot follows, so the single-player
   * path and the lobby cannot disagree. This bot is appearing for the first
   * time, so the player's remembered manual pick is NOT applied: that pick
   * is for the Add Bot button afterwards. The difficulty goes into the
   * slot's lobby config before the bot is created, so the brain is handed
   * the difficulty= token on its very first load. */
  serverSimResolveNewBotConfig(srv, (int)team, brainPath, false,
                               &spMode, &spLevel);
  serverSimSetBotConfig(srv, slot, spMode, spLevel,
                        0 /* personality: normal */, NULL);
  /* No team and no init table here: single-player bots are placed by the
   * caller's alliance pass, and their config comes from the slot config set
   * just above. */
  serverSimCreateBot(srv, slot, brainPath, botName, aiPolicy, game,
                     hiddenMines, 0, NULL);
  /* serverSimCreateBot loads the brain from the path but leaves the lobby
   * brain-INDEX at the 0xFF "default" sentinel, so the lobby Bot Code
   * dropdown renders "(none)". Resolve the index from the path
   * (case-insensitive exact match, else the version-suffixed dir name as a
   * substring) so the dropdown shows the actual brain. */
  bl = serverSimGetBrainList(srv);
  if (bl != NULL) {
    int k;
    for (k = 0; k < bl->count; k++) {
      const char *kp = serverSimGetBrainPathForIdx(srv, (uint8_t)k);
      if ((kp && SDL_strcasecmp(kp, brainPath) == 0) ||
          strstr(brainPath, bl->entries[k].name) != NULL) {
        serverSimSetBotBrainIdxFor(srv, slot, (uint8_t)k);
        break;
      }
    }
  }
  if (team > 0) {
    clientSimNetSendTeamSet(cs, slot, team);
  }
}
