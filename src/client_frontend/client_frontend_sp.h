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
 * client_frontend_sp.h - Single-player lobby seeding shared by every
 * frontend that hosts a local game.
 *
 * The desktop's single-player start and the web client's practice start
 * both put bots into the lobby before the player presses Start. The steps
 * one bot takes, and their order, are the contract: the mode and
 * difficulty into the slot before the bot is made, so its brain loads with
 * them; the bot made; the lobby's Bot Code dropdown pointed at the brain it
 * runs; its team set through the player's ClientSim. One body here, called
 * by each platform, so no copy can drop a step.
 */

#ifndef CLIENT_FRONTEND_SP_H
#define CLIENT_FRONTEND_SP_H

#include "global.h"
#include "client_sim.h"
#include "server_sim.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Put one bot into slot of a local game's lobby. brainPath is the brain it
 * runs and botName its player name. spMode and spLevel are the
 * single-player skill guess for that brain; they are resolved through the
 * one rule a new bot follows (serverSimResolveNewBotConfig, without the
 * player's remembered manual pick, which is for the Add Bot button) and
 * written into the slot before the bot is created. team is sent through cs
 * when it is not zero. The caller runs serverSimReapplyTeamAlliances once
 * after its last bot. */
void clientFrontSeedBot(ServerSim *srv, ClientSim *cs, BYTE slot,
                        const char *brainPath, const char *botName,
                        aiType aiPolicy, gameType game, bool hiddenMines,
                        BYTE team, uint8_t spMode, uint8_t spLevel);

#ifdef __cplusplus
}
#endif

#endif /* CLIENT_FRONTEND_SP_H */
