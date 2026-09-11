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
 *    on_start(game)          -- once per round, on its first running tick
 *    on_tick(game, tick)     -- EVERY game tick while the round runs
 *    on_choose_start(game, p) -- every tank placement (spawns and
 *                                respawns): return 1..num_starts to
 *                                force that start, nil for engine pick
 *
 *  The `game` API table exposes reads (tiles, pills, bases, tanks and
 *  their stats), writes (pill/base allegiance and base stock), bot
 *  spawning at valid start positions, team assignment, broadcast
 *  messages, and a programmatic round-end (win condition). The full
 *  per-call reference lives at the top of scenario.c. The script may
 *  also declare
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

/* Run the script's on_setup(game) hook: a SILENT pre-snapshot tick at
 * round start, with the tanks already placed, for arranging owners /
 * teams / pills before any client sees the world. serverSimStartGame*
 * call this just before entering the running state and drop the
 * world-delta events it queued (the arranged world rides the baseline
 * snapshot — no newswire spam). Returns TRUE when a scenario is active
 * and this round's setup ran just now (the caller flushes events);
 * FALSE for plain maps or when setup already fired. Lobby-less sims
 * get it lazily from scenarioTick right before on_start. */
bool scenarioSetup(struct ServerSim *sim);

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

/* Query the script's `enemy_bots(game)` hook: how many enemy bots the
 * scenario wants seeded onto lobby Team 2 when the map is picked.
 * 0 when the hook is missing / errors / returns 0 — no standing enemy
 * team. (The scenario's lobby-behavior surface is METHODS the engine
 * calls, not metadata constants.) */
int scenarioGetEnemyBots(const struct ServerSim *sim);

/* The sidecar's scenario.default_brain path ("" when none) — the bot
 * AI the map wants when the server has none configured. */
const char *scenarioGetDefaultBrain(const struct ServerSim *sim);

/* Script-declared blurb (scenario.description) — shown under the map
 * info in the lobby. Empty string when the script has none. */
const char *scenarioGetDescription(const struct ServerSim *sim);

/* Query the script's `show_add_team_button(game)` hook. Scenarios
 * default to FALSE (two sides: defenders vs the script); TRUE when no
 * scenario is loaded so plain-map lobbies keep their Add Team button.
 * Re-queried (via the lobby-settings republish) every time the roster
 * changes, so scripts can decide dynamically from game.lobby_slot(p). */
bool scenarioGetAllowExtraTeams(const struct ServerSim *sim);

/* Query the script's `allow_base_win(game)` hook: whether the engine's
 * all-bases sweep may end the round. Defaults to TRUE (plain maps and
 * scenarios that don't define it keep the classic rule); a survival
 * map whose win is "outlast the waves" returns false so capturing the
 * whole ring mid-wave doesn't cut the game short. */
bool scenarioGetAllowBaseWin(const struct ServerSim *sim);

/* Query the script's `bot_mode(game, team)` hook: which brain MODE — and
 * optionally which difficulty LEVEL inside it — a bot joining `team`
 * should start in, as the KEY strings that team's brain declares in its
 * own modes.txt. Lets a scenario say "every bot on the horde team runs in
 * survival mode" without the host setting it by hand.
 *
 * Returns false and leaves both buffers alone when there is no scenario,
 * no hook, the hook errors, or it names no mode; `lvlKey` comes back empty
 * when the script gave a mode but no level. Callers resolve the keys
 * themselves and ignore an answer their brain has no mode for.
 *
 * Call it BEFORE creating the bot's brain and write the result into the
 * slot's lobby config: the pair then rides the init arg the brain create
 * already stages, which costs no control event. See the comment on the
 * implementation for why that matters (a publish per bot once overflowed
 * the control channel and dropped the host). */
bool scenarioGetBotModeForTeam(const struct ServerSim *sim, int team,
                               char *modeKey, size_t modeKeySz,
                               char *lvlKey, size_t lvlKeySz);

/* The message passed to game.end_round(), for the returning lobby's
 * win line. Empty string when none was set. */
const char *scenarioGetWinMessage(const struct ServerSim *sim);

/* Notify the script that a lobby roster slot changed: calls the
 * optional on_lobby(game) hook (re-entrancy-guarded — the hook may
 * edit the roster itself via game.lobby_add_bot / lobby_remove_bot /
 * lobby_set_team, whose publishes land back here). Lobby state only. */
void scenarioLobbyChanged(struct ServerSim *sim);

/* Start-placement override: calls the script's on_choose_start(game, p)
 * hook. The engine consults this (via the GameSim chooseStart callback)
 * for EVERY tank placement — initial spawns and respawns, humans and
 * bots. A number 1..num_starts from the script returns TRUE with
 * *startIdx set 0-based; nil / no hook / error / recursion returns
 * FALSE (engine picks as usual). NOTE: at round start this fires
 * BEFORE on_start — only the chunk's top-level state exists yet. */
bool scenarioChooseStart(struct ServerSim *sim, BYTE playerNum,
                         BYTE *startIdx);

/* Implemented in server_sim.c (they need file-static internals):
 * broadcast a server message to every player, and end the round the
 * same way the all-bases win does (lobby return / quit-on-win aware),
 * tagged RETURN_REASON_SCENARIO so no WBN win crediting happens. */
void serverSimScenarioPublish(struct ServerSim *sim, const char *message);
void serverSimScenarioEndRound(struct ServerSim *sim);

#endif /* SCENARIO_H */
