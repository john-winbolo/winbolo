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
 *Name:          Server Simulation Lobby Settings
 *Filename:      server_sim_lobby.c
 *Author:        John Morrison
 *Purpose:
 *  The lobby settings band — the operator's startup
 *  config, team metadata, per-slot bot brain and config,
 *  the shared LST_* apply path, and the publish helpers
 *  that broadcast each of them.
 *********************************************************/

#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "server_sim_internal.h"
#include "server_sim_shared.h"      /* serverSimAnnounce — the rename publish below asks the policy */
#include "server_sim_lifecycle.h"   /* ServerInstanceConfig, the settings mutators, serverSimGetTeamMetaMut / serverSimGetBotConfigMut */
#include "server_sim_join.h"        /* serverSimRepickAllLobbyStarts — the start re-pick when a team's side changes */
#include "netpacks.h"               /* lobbyTimeMinutesIsValid — the LST_TIME_MINUTES range check */
#include "wire_limits.h"            /* the LST_* selectors carried in PACKET_LOBBY_SET_SETTING */
#include "lobby_bot_pools.h"        /* lobbyBotPoolCount — the per-team naming-pool uniqueness pass */
#include "brain_list.h"             /* BrainModes — the remembered mode/level keys */
#include "start_sides.h"            /* START_SIDE_ANY / START_SIDE_COUNT — the team start-side range */

void serverSimApplyInstanceConfig(ServerSim *sim, const ServerInstanceConfig *cfg) {
  sim->sim.viewPlayer = cfg->viewPlayer;
  sim->maxBots        = cfg->maxBots;
  sim->maxSpectators  = cfg->maxSpectators;
  sim->specDelayTicks = (uint32_t)cfg->specDelaySeconds * 50u;   /* 50 ticks/s */

  serverSimSetEmptyResetEnabled(sim, cfg->emptyResetEnabled);
  serverSimSetHasPassword(sim, cfg->hasPassword);
  if (cfg->botBrainPath != NULL) {
    serverSimSetBotBrainPath(sim, cfg->botBrainPath);
  }
  if ((aiType)cfg->botAiType != aiNone) {
    serverSimSetBotAiType(sim, (aiType)cfg->botAiType);
  }
  /* ranked forces autolock-on-game-start (matches the server-side
   * LST_RANKED handler at PACKET_LOBBY_SET_SETTING and the existing
   * servermain.c -ranked CLI behaviour). */
  serverSimSetAutoLockOnGameStart(sim,
      cfg->autoLockOnGameStart || cfg->ranked);
  serverSimSetRanked(sim, cfg->ranked);
  serverSimSetOpenHost(sim, cfg->openHost);
  serverSimSetServerLocks(sim, cfg->serverLocks);
  serverSimSetVoiceMode(sim, cfg->voiceMode);

  /* lobbyEnabled and skipLobby drive state transitions. If neither is
   * set, the sim stays in whatever state serverSimCreate* left it
   * (today's dedicated-server-with-no-cfg-fields behaviour). */
  if (cfg->skipLobby) {
    serverSimSetLobbyEnabled(sim, false);
    serverSimStartGame(sim);
    /* serverSimStartGame latches hadPlayersEver = TRUE, but a map-rotation
     * server's first round boots up empty and waits for joiners. Left set, the
     * lifecycle's empty-server check would fire on the very next tick and
     * rotate before anyone joins. Re-arm it so the empty rotation only fires
     * once a player has joined and then left — serverSimMapRotateRound does the
     * same for every later round. */
    if (sim->mapRotateEnabled) {
      sim->hadPlayersEver = FALSE;
    }
  } else if (cfg->lobbyEnabled) {
    serverSimSetLobbyEnabled(sim, true);
    serverSimEnterLobby(sim);
  }

  /* Snapshot the configured lobby settings now that every startup field
   * is in place — serverSimResetLobbyToDefaults restores from this when
   * the last human leaves the lobby. */
  sim->originalLobbySettings.valid               = true;
  sim->originalLobbySettings.gameType            = gameTypeGet(&sim->sim.game);
  sim->originalLobbySettings.hiddenMines         = sim->sim.hiddenMines ? true : false;
  sim->originalLobbySettings.botAiType           = sim->botAiType;
  sim->originalLobbySettings.aiPolicy            = sim->aiPolicy;
  sim->originalLobbySettings.timeLimit           = sim->timeLimit;
  sim->originalLobbySettings.timeMinutes         = sim->timeMinutes;
  sim->originalLobbySettings.gameLength          = sim->gameLength;
  sim->originalLobbySettings.openHost            = sim->openHost;
  sim->originalLobbySettings.autoLockOnGameStart = sim->autoLockOnGameStart;
  sim->originalLobbySettings.ranked              = sim->ranked;
  sim->originalLobbySettings.serverLocks         = sim->serverLocks;
  for (int vc = 0; vc < VIEW_CATEGORY_COUNT; vc++) {
    sim->originalLobbySettings.viewPolicy[vc]    = sim->viewPolicy[vc];
    sim->originalLobbySettings.viewDecaySecs[vc] = sim->viewDecaySecs[vc];
  }
  sim->originalLobbySettings.classicMode         = sim->classicMode;
  sim->originalLobbySettings.alliesInTrees       = sim->alliesInTrees;
  sim->originalLobbySettings.overviewWindow      = sim->overviewWindow;
  sim->originalLobbySettings.lineOfSight         = sim->lineOfSight;
}

/* ────────────────────────────────────────────────────────────────
 * Lobby Layout A accessors / mutators / publish helpers
 * ──────────────────────────────────────────────────────────────── */

void serverSimSetBotBrainIdxFor(ServerSim *sim, BYTE slot, uint8_t brainIdx) {
    if (!sim || slot >= MAX_TANKS) return;
    /* 0xFF is the "use server default" sentinel; any other in-range
     * value indexes into the catalogue. Out-of-range is a no-op,
     * matching the existing "ignore malformed input" pattern. */
    if (brainIdx != 0xFF && brainIdx >= sim->brainList.count) return;
    sim->botBrainIdx[slot] = brainIdx;
    serverSimPublishLobbyBotBrain(sim, slot);
    lobbyAutoUnreadyOnChange(sim);
}

const char *serverSimGetBrainPathForIdx(const ServerSim *sim, uint8_t brainIdx) {
    if (!sim) return NULL;
    if (brainIdx == 0xFF) return sim->botBrainPath;
    if (brainIdx >= sim->brainList.count) return NULL;
    return sim->brainPaths[brainIdx];
}

/* The brain a lobby slot is running, or NULL when it has none (a human, or
 * a bot wired up without one). Same path botManagerOnGameStart reloads from,
 * so a manifest read through it describes the brain that will actually run. */
static const char *slotBrainPath(const ServerSim *sim, BYTE slot) {
    const char *path;
    if (sim == NULL || slot >= MAX_TANKS) return NULL;
    path = sim->botMgr.bots[slot].brainPath;
    return (path[0] != '\0') ? path : NULL;
}

/* ── A newly added bot's brain mode and difficulty ─────────────────────
 *
 * Andrew's rule, for Survival: "Hard mode always when the bots first appear
 * on survival, then if they're removed and one added with Medium or Easy
 * THEN a Add Bot defaults to what the user has manually added."
 *
 * Generalised, every bot a lobby gains resolves its mode and difficulty
 * (serverSimResolveNewBotConfig) in this order:
 *
 *   1. the caller's base — the lobby default, or single player's own
 *      chosen level;
 *   2. what the map requires for the bot's side — the scenario's
 *      bot_mode(game, team) hook; on Survival the horde is survival mode at
 *      Hard;
 *   3. what the host last picked BY HAND, when the caller honours it — only
 *      the difficulty when step 2 fixed the mode (a horde bot stays in
 *      survival mode, at the Medium the host chose), mode and difficulty
 *      both otherwise.
 *
 * Bots that FIRST APPEAR — the scenario seed, single player's setup bots —
 * do not honour step 3, so they always come up at the map's default. The
 * Add Bot button does honour it.
 *
 * The manual pick is recorded only where a person made one
 * (serverSimRememberManualBotPick). It used to be recorded on EVERY config
 * write, so an automatic one — single player's add path writing its skill
 * guess — was remembered as though the host had chosen it. */

void serverSimRememberManualBotPick(ServerSim *sim, BYTE slot) {
    BrainModes modes;
    const BrainMode *m;
    const LobbyBotConfig *bc;
    const char *path = slotBrainPath(sim, slot);

    if (path == NULL) return;
    if (!brainListLoadModesForPath(path, &modes)) return;
    bc = &sim->botConfigs[slot];
    if (bc->mode >= (uint8_t)modes.modeCount) return;
    m = &modes.modes[bc->mode];
    if (bc->difficulty >= (uint8_t)m->levelCount) return;

    SDL_strlcpy(sim->lastBotModeKey, m->key, sizeof(sim->lastBotModeKey));
    SDL_strlcpy(sim->lastBotLevelKey, m->levels[bc->difficulty].key,
                sizeof(sim->lastBotLevelKey));
}

bool serverSimResolveNewBotConfig(const ServerSim *sim, int team,
                                  const char *brainPath,
                                  bool honourManualPick,
                                  uint8_t *ioMode, uint8_t *ioLevel) {
    BrainModes modes;
    int mode;
    int level;
    bool mapFixedMode = false;

    if (sim == NULL || ioMode == NULL || ioLevel == NULL) return false;
    if (brainPath == NULL || brainPath[0] == '\0') return false;
    /* A brain with no manifest reads no mode=/difficulty= token at all
     * (botManagerStageInitArg), so there is nothing to resolve. */
    if (!brainListLoadModesForPath(brainPath, &modes)) return false;

    /* 1. The base, clamped into this brain's own lists the same way
     *    botInitArgAppendModeTokens clamps it. */
    mode = (int)*ioMode;
    if (mode >= modes.modeCount) mode = 0;
    level = (int)*ioLevel;
    if (level >= modes.modes[mode].levelCount) {
        level = modes.modes[mode].defaultLevel;
    }

    /* Step 2 of this rule, "what the map requires for this side", has no
     * meaning here: that step asks a map script which mode a team runs in, and
     * this build has no script layer. mapFixedMode therefore stays false, and
     * the host's pick below is free to set the mode as well as the level. */

    /* 3. What the host last picked by hand. */
    if (honourManualPick && sim->lastBotLevelKey[0] != '\0') {
        if (mapFixedMode) {
            int li = brainModeFindLevel(&modes.modes[mode],
                                        sim->lastBotLevelKey);
            if (li >= 0) level = li;
        } else if (sim->lastBotModeKey[0] != '\0') {
            int mi = brainModesFindMode(&modes, sim->lastBotModeKey);
            if (mi >= 0) {
                int li = brainModeFindLevel(&modes.modes[mi],
                                            sim->lastBotLevelKey);
                mode  = mi;
                level = (li >= 0) ? li : modes.modes[mi].defaultLevel;
            }
        }
    }

    *ioMode  = (uint8_t)mode;
    *ioLevel = (uint8_t)level;
    return true;
}

void serverSimApplyNewBotDefaults(ServerSim *sim, BYTE slot, int team,
                                  const char *brainPath,
                                  bool honourManualPick) {
    uint8_t mode  = 0;
    uint8_t level = BOT_DIFFICULTY_HARD;

    if (sim == NULL || slot >= MAX_TANKS) return;
    /* The lobby default is the base, and it is written even when the brain
     * ships no manifest: botConfigs[slot] still holds whatever the slot's
     * PREVIOUS occupant had, and a new defender must not inherit a removed
     * horde bot's survival mode. */
    serverSimResolveNewBotConfig(sim, team, brainPath, honourManualPick,
                                 &mode, &level);
    sim->botConfigs[slot].mode       = mode;
    sim->botConfigs[slot].difficulty = level;
    serverSimQueueBotConfigPublish(sim, slot);
}

/* How many queued bot-config events one lobby tick sends. Ten seeded bots
 * show their mode within five ticks, a tenth of a second. */
#define BOT_CONFIG_PUBLISHES_PER_TICK 2

void serverSimQueueBotConfigPublish(ServerSim *sim, BYTE slot) {
    if (sim == NULL || slot >= MAX_TANKS) return;
    sim->botConfigPublishPending |= (uint16_t)(1u << slot);
}

void serverSimFlushBotConfigPublishes(ServerSim *sim) {
    int sent = 0;
    BYTE s;
    if (sim == NULL || sim->botConfigPublishPending == 0) return;
    for (s = 0; s < MAX_TANKS && sent < BOT_CONFIG_PUBLISHES_PER_TICK; s++) {
        uint16_t bit = (uint16_t)(1u << s);
        if ((sim->botConfigPublishPending & bit) == 0) continue;
        sim->botConfigPublishPending &= (uint16_t)~bit;
        if (!sim->playerConnected[s] || !serverSimIsBot(sim, s)) continue;
        serverSimPublishLobbyBotConfig(sim, s);
        sent++;
    }
}

void serverSimSetBotConfig(ServerSim *sim, BYTE slot,
                            uint8_t mode, uint8_t difficulty,
                            uint8_t personality,
                            const char *validatedName) {
    if (!sim || slot >= MAX_TANKS) return;
    {
        LobbyBotConfig *bc = serverSimGetBotConfigMut(sim, slot);
        if (bc) {
            bc->mode        = mode;
            bc->difficulty  = difficulty;
            bc->personality = personality;
        }
    }
    /* The publish just below carries these values, so any queued one for
     * this slot is now redundant. Deliberately NOT a manual pick: this is
     * called by automatic paths too — see serverSimRememberManualBotPick. */
    sim->botConfigPublishPending &= (uint16_t)~(1u << slot);
    if (validatedName != NULL && validatedName[0] != '\0') {
        serverSimRenameBotSlot(sim, slot, validatedName);
    }
    serverSimPublishLobbyBotConfig(sim, slot);
    serverSimPublishLobbySlot(sim, slot);
    lobbyAutoUnreadyOnChange(sim);
}

void serverSimSwitchBotBrain(ServerSim *sim, BYTE slot, uint8_t brainIdx) {
    if (!sim || slot >= MAX_TANKS) return;
    serverSimSetBotBrainIdxFor(sim, slot, brainIdx);
    botManagerSetBrainIdx(sim, slot, sim->botBrainIdx[slot]);
}

void serverSimRenameBotSlot(ServerSim *sim, BYTE slot, const char *name) {
    if (!sim || slot >= MAX_TANKS || !name) return;
    {
        char nameBuf[32];
        char loc[3] = "??";
        SDL_strlcpy(nameBuf, name, sizeof(nameBuf));
        playersSetPlayer(NULL, &sim->sim.plyrs, NEUTRAL, slot,
                         nameBuf, loc,
                         0, 0, 0, 0, 0, FALSE, 0, NULL, TRUE);
    }
    /* Subscriber ClientSims track names in sim.plyrs (what the in-game
     * players panel reads), not in lobbySlots. Publish so the rename
     * propagates past the lobby UI into the game view. */
    {
        ControlEvent evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_PLAYER_NAME;
        evt.u.playerName.playerNum = slot;
        snprintf(evt.u.playerName.name, PACKET_MAX_PLAYER_NAME, "%s", name);
        evt.u.playerName.quiet =
            serverSimAnnounce(sim, ANNOUNCE_KIND_NAME_CHANGED, slot, slot)
                ? 0 : 1;
        serverSimPublishControl(sim, &evt);
    }
}

void serverSimPublishLobbySlot(ServerSim *sim, BYTE slot) {
    ControlEvent evt;
    if (!sim) return;
    memset(&evt, 0, sizeof(evt));
    serverSimFillLobbySlotEvent(sim, slot, &evt);
    serverSimPublishControl(sim, &evt);
}

void serverSimPublishLobbyBotBrain(ServerSim *sim, BYTE slot) {
    ControlEvent evt;
    if (!sim) return;
    memset(&evt, 0, sizeof(evt));
    serverSimFillLobbyBotBrainEvent(sim, slot, &evt);
    serverSimPublishControl(sim, &evt);
}

void serverSimPublishLobbyBotConfig(ServerSim *sim, BYTE slot) {
    ControlEvent evt;
    if (!sim) return;
    memset(&evt, 0, sizeof(evt));
    serverSimFillLobbyBotConfigEvent(sim, slot, &evt);
    serverSimPublishControl(sim, &evt);
}

void serverSimPublishLobbyTeamMeta(ServerSim *sim, BYTE teamId) {
    ControlEvent evt;
    if (!sim) return;
    memset(&evt, 0, sizeof(evt));
    serverSimFillLobbyTeamMetaEvent(sim, teamId, &evt);
    serverSimPublishControl(sim, &evt);
}

void serverSimSetTeamMeta(ServerSim *sim, BYTE teamId,
                           uint8_t color, uint8_t namingPool,
                           uint8_t startSide,
                           const uint8_t *name, uint8_t nameLen) {
    if (!sim || teamId == 0 || teamId >= MAX_TANKS) return;
    TeamMetadata *t = serverSimGetTeamMetaMut(sim, teamId);
    if (t == NULL) return;
    uint8_t prevSide = t->startSide;
    t->in_use = 1;
    t->color = color;
    /* A side outside the START_SIDE_* range means no side. */
    if (startSide >= START_SIDE_COUNT) startSide = START_SIDE_ANY;
    t->startSide = startSide;
    /* Per-team uniqueness on namingPool: if another in_use team
     * already owns this pool, pick the lowest pool index not
     * used by any other team. Falls back to the requested value
     * if every pool is taken. */
    {
        int poolCount = lobbyBotPoolCount();
        bool poolTaken = false;
        for (BYTE other = 1; other < MAX_TANKS; other++) {
            if (other == teamId) continue;
            const TeamMetadata *ot = serverSimGetTeamMetaMut(sim, other);
            if (ot && ot->in_use && ot->namingPool == namingPool) {
                poolTaken = true;
                break;
            }
        }
        if (poolTaken && poolCount > 0) {
            for (int p = 0; p < poolCount; p++) {
                bool used = false;
                for (BYTE other = 1; other < MAX_TANKS; other++) {
                    if (other == teamId) continue;
                    const TeamMetadata *ot = serverSimGetTeamMetaMut(sim, other);
                    if (ot && ot->in_use && ot->namingPool == p) {
                        used = true;
                        break;
                    }
                }
                if (!used) { namingPool = (uint8_t)p; break; }
            }
        }
    }
    t->namingPool = namingPool;
    memset(t->name, 0, LOBBY_TEAM_NAME_LEN);
    if (nameLen > 0 && name != NULL) {
        memcpy(t->name, name, nameLen);
    }
    serverSimPublishLobbyTeamMeta(sim, teamId);
    /* A side change moves every reservation, picks included, so the lobby
     * ends up as if everyone had joined after the sides were set. */
    if (prevSide != startSide) {
        serverSimRepickAllLobbyStarts(sim);
    }
    lobbyAutoUnreadyOnChange(sim);
}

void serverSimClearTeamMeta(ServerSim *sim, BYTE teamId) {
    if (!sim || teamId == 0 || teamId >= MAX_TANKS) return;
    TeamMetadata *t = serverSimGetTeamMetaMut(sim, teamId);
    bool hadSide = false;
    if (t != NULL) {
        hadSide = (t->startSide != START_SIDE_ANY);
        memset(t, 0, sizeof(TeamMetadata));
    }
    serverSimPublishLobbyTeamMeta(sim, teamId);
    /* Zeroing the struct drops the side too; that is a side change. */
    if (hadSide) {
        serverSimRepickAllLobbyStarts(sim);
    }
    lobbyAutoUnreadyOnChange(sim);
}

void serverSimPublishLobbySettings(ServerSim *sim) {
    ControlEvent evt;
    if (!sim) return;
    memset(&evt, 0, sizeof(evt));
    serverSimFillLobbySettingsEvent(sim, &evt);
    serverSimPublishControl(sim, &evt);
}

/* Shared apply path for the LST_* setting cluster carried in
 * PACKET_LOBBY_SET_SETTING and its SP-host local-transport
 * equivalent. The caller is responsible for upstream lock-bit /
 * authority gates; on success this helper publishes
 * CTRL_LOBBY_SETTINGS and clears humans' ready state before
 * returning true.
 *
 * Returns true if the setting was applied, false if the payload
 * was malformed, out of range, or rejected by a cross-setting
 * invariant (e.g. ranked forbids gameOpen / non-aiNone / autoLock
 * off). */
static bool serverSimApplyLobbySettingInner(ServerSim *sim,
                                            uint8_t lst,
                                            const uint8_t *value, size_t len) {
    if (sim == NULL || value == NULL) return false;
    switch (lst) {
        case LST_GAME_TYPE:
            if (len != 1 || value[0] < 1 || value[0] > 3) return false;
            if (serverSimGetRanked(sim) &&
                (gameType)value[0] == gameOpen) return false;
            /* A scripted round plays the game its scenario declared, and the
               type that says so is the map commit's to set. Every value this
               case admits would take the lobby off gameScripted, so while a
               scenario is attached none of them is the host's to send. */
            if (sim->scenarioIdentity.source != lobbyScenarioNone) return false;
            serverSimSetGameType(sim, (gameType)value[0]);
            return true;
        case LST_HIDDEN_MINES:
            if (len != 1) return false;
            serverSimSetHiddenMines(sim, value[0] != 0);
            return true;
        case LST_AI_POLICY:
            if (len != 1 || value[0] > 3) return false;
            if (serverSimGetRanked(sim) &&
                (aiType)value[0] != aiNone) return false;
            /* A scenario fields its own bots, and this is the setting that
               takes every bot off the roster, so a scripted lobby cannot be
               put into it. */
            if (sim->scenarioIdentity.source != lobbyScenarioNone &&
                (aiType)value[0] == aiNone) return false;
            serverSimSetAiPolicy(sim, value[0]);
            serverSimSetBotAiType(sim, (aiType)value[0]);
            if ((aiType)value[0] == aiNone) {
                for (BYTE bi = 0; bi < MAX_TANKS; bi++) {
                    if (serverSimIsBot(sim, bi)) {
                        serverSimRemoveBot(sim, bi);
                    }
                }
            }
            return true;
        case LST_TIME_LIMIT: {
            if (len != 1) return false;
            bool tl = value[0] != 0;
            serverSimSetTimeLimit(sim, tl);
            if (tl) {
                uint16_t mins = serverSimGetTimeMinutes(sim) > 0
                    ? serverSimGetTimeMinutes(sim) : 30;
                serverSimSetGameLength(sim,
                    (int32_t)mins * 60 * GAME_NUMGAMETICKS_SEC);
            } else {
                serverSimSetGameLength(sim, UNLIMITED_GAME_TIME);
            }
            return true;
        }
        case LST_TIME_MINUTES: {
            if (len != 2) return false;
            uint16_t mins = (uint16_t)((value[0] << 8) | value[1]);
            if (!lobbyTimeMinutesIsValid(mins)) return false;
            serverSimSetTimeMinutes(sim, mins);
            if (serverSimGetTimeLimit(sim)) {
                serverSimSetGameLength(sim,
                    (int32_t)mins * 60 * GAME_NUMGAMETICKS_SEC);
            }
            return true;
        }
        case LST_AUTO_LOCK_ON_GAME: {
            if (len != 1) return false;
            bool v = value[0] != 0;
            if (serverSimGetRanked(sim) && !v) return false;
            serverSimSetAutoLockOnGameStart(sim, v);
            return true;
        }
        case LST_RANKED: {
            if (len != 1) return false;
            bool r = value[0] != 0;
            /* A scripted round is not a measured one, so ranked is refused
               while a scenario is attached. The other direction — ranked
               already on when a scripted map is committed — is answered at
               the commit, which clears it. */
            if (r && sim->scenarioIdentity.source != lobbyScenarioNone) {
                return false;
            }
            serverSimSetRanked(sim, r);
            if (r) {
                serverSimSetAiPolicy(sim, (uint8_t)aiNone);
                serverSimSetBotAiType(sim, aiNone);
                for (BYTE bi = 0; bi < MAX_TANKS; bi++) {
                    if (serverSimIsBot(sim, bi)) {
                        serverSimRemoveBot(sim, bi);
                    }
                }
                if (serverSimGetGameType(sim) == gameOpen) {
                    serverSimSetGameType(sim, gameTournament);
                }
                if (!serverSimGetAutoLockOnGameStart(sim)) {
                    serverSimSetAutoLockOnGameStart(sim, true);
                }
            }
            return true;
        }
        case LST_CLASSIC_MODE: {
            if (len != 1) return false;
            serverSimSetClassicMode(sim, value[0] != 0);
            return true;
        }
        case LST_ALLIES_IN_TREES: {
            if (len != 1) return false;
            /* Classic mode hides allies in trees and owns this value while it
             * is set, the same way it owns the three view policies. */
            if (sim->classicMode) return false;
            serverSimSetAlliesInTrees(sim, value[0] != 0);
            return true;
        }
        case LST_OVERVIEW_WINDOW: {
            if (len != 1) return false;
            /* Classic mode owns the overview while it is set, the same
             * way it owns the three view policies. */
            if (sim->classicMode) return false;
            /* A byte outside the enum is refused rather than left to the
             * setter to ignore: the lobby offers a fixed set of choices,
             * so anything else is a malformed command — the same reading
             * LST_PILL_VIEW gives an unknown policy. LST_ALLIES_IN_TREES
             * takes any non-zero byte because it is a bool; these two are
             * selectors. */
            if (value[0] >= (uint8_t)OVERVIEW_WINDOW_COUNT) return false;
            serverSimSetOverviewWindow(sim, value[0]);
            return true;
        }
        case LST_LINE_OF_SIGHT: {
            if (len != 1) return false;
            if (sim->classicMode) return false;
            if (value[0] >= (uint8_t)LINE_OF_SIGHT_COUNT) return false;
            serverSimSetLineOfSight(sim, value[0]);
            return true;
        }
        case LST_PILL_VIEW:
        case LST_BASE_VIEW:
        case LST_ALLY_VIEW: {
            /* Classic mode owns these three values while it is set, so an
             * edit that would contradict it is refused — the same way
             * LST_AUTO_LOCK_ON_GAME refuses an edit that contradicts
             * ranked. */
            if (sim->classicMode) return false;
            /* [policy 1][decaySecs 2 BE]. Reject an unknown policy the
             * same way LST_AI_POLICY rejects an out-of-range aiType;
             * the decay seconds are clamped by the setter. */
            if (len != 3 || value[0] > (uint8_t)viewPolicyOff) return false;
            ViewCategory cat = (lst == LST_PILL_VIEW) ? viewCategoryPill
                             : (lst == LST_BASE_VIEW) ? viewCategoryBase
                                                      : viewCategoryAlly;
            uint16_t secs = (uint16_t)((value[1] << 8) | value[2]);
            serverSimSetViewPolicy(sim, cat, (ViewPolicy)value[0], secs);
            return true;
        }
        default:
            return false;
    }
}

bool serverSimApplyLobbySetting(ServerSim *sim,
                                uint8_t lst,
                                const uint8_t *value, size_t len) {
    if (!serverSimApplyLobbySettingInner(sim, lst, value, len)) return false;
    serverSimPublishLobbySettings(sim);
    lobbyAutoUnreadyOnChange(sim);
    serverSimWbnLobbyUpdate(sim, FALSE);
    return true;
}

/* Auto-unready: any meaningful lobby change clears every human's
 * ready flag and aborts an in-flight countdown. The per-slot
 * CTRL_LOBBY_SLOT publishes (plus the CTRL_GAME_PHASE_LOBBY publish
 * if the countdown was aborted) fan out to both in-process subscribers and
 * remote UDP clients via the codec — no wire-only blast needed. Bots
 * stay permanently ready by design (set in botManagerAddBot) so the
 * next all-ready check still triggers a countdown when the human
 * re-confirms. */
void lobbyAutoUnreadyOnChange(ServerSim *sim) {
    BYTE i;
    bool countdownWasRunning = (serverSimGetState(sim) == serverStateCountdown);
    bool toggled[MAX_TANKS];

    for (i = 0; i < MAX_TANKS; i++) {
        const LobbyPlayer *lp = serverSimGetLobbyPlayer(sim, i);
        toggled[i] = false;
        if (lp == NULL) continue;
        if (lp->isBot) continue;
        if (lp->ready) {
            serverSimSetReady(sim, i, false);
            toggled[i] = true;
        }
    }

    if (countdownWasRunning) {
        /* serverSimAbortCountdown publishes the CTRL_GAME_PHASE_LOBBY
         * transition itself; no separate publish needed here. */
        serverSimAbortCountdown(sim);
    }

    for (i = 0; i < MAX_TANKS; i++) {
        if (toggled[i]) {
            serverSimPublishLobbySlot(sim, i);
        }
    }
}

const BrainList *serverSimGetBrainList(const ServerSim *sim) {
    return sim ? &sim->brainList : NULL;
}

bool serverSimRankedShapeReady(const ServerSim *sim) {
    if (sim == NULL) return false;
    int teamSizes[17] = {0};
    int teamsInUse = 0;
    for (int i = 0; i < MAX_TANKS; i++) {
        if (!serverSimIsPlayerConnected(sim, i)) continue;
        const LobbyPlayer *lp = serverSimGetLobbyPlayer(sim, i);
        if (lp == NULL || lp->isBot) continue;
        uint8_t t = lp->teamNumber;
        if (t == 0 || t > 16) continue;
        if (teamSizes[t] == 0) teamsInUse++;
        teamSizes[t]++;
    }
    int firstSize = 0, secondSize = 0;
    for (int t = 1; t <= 16; t++) {
        if (teamSizes[t] == 0) continue;
        if (firstSize == 0) firstSize = teamSizes[t];
        else                secondSize = teamSizes[t];
    }
    return (teamsInUse == 2) &&
           (firstSize == secondSize) &&
           (firstSize >= 1 && firstSize <= 3);
}
