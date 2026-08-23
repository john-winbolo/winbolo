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
 *Name:          Scenario
 *Filename:      scenario.h
 *Purpose:
 *  Server-side scripted map scenarios. A map "X.map" may ship a
 *  companion "X.scenario.lua"; when present the server hosts a Lua VM
 *  for the round and calls the script's hooks:
 *
 *    on_start(game)        -- once per round, on its first running tick
 *    on_tick(game, tick)   -- EVERY game tick while the round runs
 *
 *  The `game` API table exposes reads (tiles, pills, bases, tanks and
 *  their stats), writes (pill/base allegiance), bot spawning at valid
 *  start positions, team assignment, broadcast messages, and a
 *  programmatic round-end (win condition). The script may also declare
 *
 *    scenario = { max_players = 6 }
 *
 *  which the lobby/join path enforces as a hard player cap.
 *
 *  Everything executes on the SERVER; clients need no changes to play
 *  a scripted map beyond the ordinary wire protocol.
 *********************************************************/

#ifndef SCENARIO_H
#define SCENARIO_H

#include "global.h"
#include "gametype.h"

struct ServerSim;

/* Look for <map minus .map>.scenario.lua beside the map file; when found,
 * boot the VM, run the chunk, and capture metadata (on_start fires later,
 * from the first scenarioTick of the round). Returns TRUE when a scenario
 * is active afterwards. Absence of a sidecar is not an error (returns
 * FALSE, sim->scenario stays NULL). */
bool scenarioLoad(struct ServerSim *sim, const char *mapFileName,
                  gameType game, bool hiddenMines);

/* Call the script's on_tick. Invoked once per game tick while the round
 * is running. Errors are logged and the scenario disables itself after
 * repeated failures rather than wedging the sim. */
void scenarioTick(struct ServerSim *sim);

/* Re-boot the VM with fresh script state — called at each authoritative
 * round start (serverSimStartGame*) so a lobby server's second round
 * doesn't inherit the first round's script variables; the fresh VM's
 * on_start then fires on the round's first tick. Safe no-op when no
 * scenario is loaded. */
void scenarioReset(struct ServerSim *sim);

/* Tear down the VM (safe when no scenario is loaded). */
void scenarioShutdown(struct ServerSim *sim);

/* TRUE when a scenario VM is live for this sim. */
bool scenarioIsActive(const struct ServerSim *sim);

/* Script-declared hard player cap (scenario.max_players); 0 = none. */
int scenarioGetMaxPlayers(const struct ServerSim *sim);

/* The message passed to game.end_round(), for the returning lobby's
 * win line. Empty string when none was set. */
const char *scenarioGetWinMessage(const struct ServerSim *sim);

/* Implemented in server_sim.c (they need file-static internals):
 * broadcast a server message to every player, and end the round the
 * same way the all-bases win does (lobby return / quit-on-win aware),
 * tagged RETURN_REASON_SCENARIO so no WBN win crediting happens. */
void serverSimScenarioPublish(struct ServerSim *sim, const char *message);
void serverSimScenarioEndRound(struct ServerSim *sim);

#endif /* SCENARIO_H */
