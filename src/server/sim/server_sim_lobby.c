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
    /* The lobby the attached scenario asks for, seated here and not by the
       caller, because this is the only point that is after both of the things
       it needs and before the thing that needs it. A team the template fields
       with no brain of its own falls back to the server's, and that path and
       the bot AI level were written a few lines above; the start below builds
       a tank for every fielded seat, and only for the seats that already
       exist when it runs. With no scenario attached there is no template and
       this seats nothing, so a plain server is unchanged. */
    serverSimScenarioSeatLobby(sim);
    serverSimStartGame(sim);
    /* serverSimStartGame latches hadPlayersEver = TRUE, but a no-lobby
     * server's first round boots up empty and waits for joiners. Left set,
     * the lifecycle's empty-server check fires on the very next tick:
     * -maprotate rotates before anyone joins, and -autoclose shuts the
     * server down before anyone can connect. Re-arm it so both only fire
     * once a player has joined and then left — serverSimMapRotateRound does
     * the same for every later round. */
    sim->hadPlayersEver = FALSE;
  } else if (cfg->lobbyEnabled) {
    serverSimSetLobbyEnabled(sim, true);
    serverSimEnterLobby(sim);
  }

  /* A server that opens a lobby opens it with teams 1 and 2 on the default
   * pair for its map's shape (north/south, or east/west for a wide map).
   * The dedicated server sets neither flag above and is left in the
   * lobby serverSimCreate* made, so the test is the sim's own flag, not
   * cfg->lobbyEnabled. A skipLobby start (-nolobby, -maprotate, and the
   * local sims of bg_game, braintest and the gym) has no lobby and keeps
   * placing teams with no side, as it always has. */
  if (!cfg->skipLobby && sim->lobbyEnabled) {
    serverSimApplyDefaultTeamSides(sim);
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
  sim->originalLobbySettings.positionalSound     = sim->positionalSound;
  sim->originalLobbySettings.overviewWindow      = sim->overviewWindow;
  sim->originalLobbySettings.lineOfSight         = sim->lineOfSight;
  sim->originalLobbySettings.smartPingsOff       = sim->smartPingsOff;
  sim->originalLobbySettings.modsOff             = sim->modsOff;
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
 *      chosen level. Only its level is kept: the mode is the game type's
 *      starting mode (open_default on Open, else mode 0);
 *   2. what the map requires for the bot's side — the attached scenario's
 *      lobby template, read for that team; on Survival the horde's seats
 *      are survival mode at Hard;
 *   3. what a person last picked BY HAND, when the caller honours it — on a
 *      team step 2 configured, the level last chosen on a seat of THAT team
 *      and never the mode (a bot on the horde stays in survival mode, at the
 *      Medium somebody chose for the horde); on every other team, the one
 *      mode-and-level pair the lobby remembers.
 *
 * Bots that FIRST APPEAR — the scenario seed, single player's setup bots —
 * do not honour step 3, so they always come up at the map's default. The
 * Add Bot button does honour it.
 *
 * The manual pick is recorded only where a person made one
 * (serverSimRememberManualBotPick). It used to be recorded on EVERY config
 * write, so an automatic one — single player's add path writing its skill
 * guess — was remembered as though the host had chosen it. */

/* The index in mode `to` of the level at index `level` in mode `from`,
 * matched by key; `to`'s own default level when it lists no such key or the
 * index is out of range. */
static int lobbyLevelAcrossModes(const BrainModes *modes, int from, int level,
                                 int to) {
    const BrainMode *fm = &modes->modes[from];
    const BrainMode *tm = &modes->modes[to];
    if (level >= 0 && level < fm->levelCount) {
        int li = brainModeFindLevel(tm, fm->levels[level].key);
        if (li >= 0) return li;
    }
    return tm->defaultLevel;
}

/* The attached scenario's template entry for `team`, when that team has one
 * AND it names a mode or a difficulty — the two fields that make a team's
 * bots the MAP's to configure rather than the lobby's.
 *
 * NULL for every other team, which is every team of an ordinary lobby, a
 * team the template does not list, and a template team that names neither
 * key. The two callers below both ask this one question, so a team is
 * "templated" in exactly one place. */
static const ScnLobbyTeam *lobbyTemplateTeam(const ServerSim *sim, int team) {
    int t;

    if (sim == NULL || !sim->scenarioLobbyValid) return NULL;
    if (team <= 0 || team >= MAX_TANKS) return NULL;
    for (t = 0; t < (int)sim->scenarioLobby.numTeams && t < MAX_TANKS; t++) {
        const ScnLobbyTeam *lt = &sim->scenarioLobby.teams[t];
        if ((int)lt->id != team) continue;
        if (lt->mode[0] == '\0' && lt->difficulty[0] == '\0') return NULL;
        return lt;
    }
    return NULL;
}

void serverSimMarkBotModeSetByHand(ServerSim *sim, BYTE slot) {
    if (sim == NULL || slot >= MAX_TANKS) return;
    sim->botModeSetByHand |= (uint16_t)(1u << slot);
}

void serverSimFollowGameTypeBotModes(ServerSim *sim, gameType oldType) {
    gameType newType;
    BYTE     slot;

    if (sim == NULL) return;
    newType = serverSimGetGameType(sim);
    if ((oldType == gameOpen) == (newType == gameOpen)) return;
    for (slot = 0; slot < MAX_TANKS; slot++) {
        BrainModes      modes;
        LobbyBotConfig *bc;
        const char     *path;
        int             oldStart;
        int             newStart;

        if (!serverSimIsBot(sim, slot)) continue;
        if ((sim->botModeSetByHand & (1u << slot)) != 0) continue;
        /* A team the map configures owns its bots' mode. */
        if (lobbyTemplateTeam(sim, (int)sim->lobbyPlayers[slot].teamNumber)
                != NULL) continue;
        path = slotBrainPath(sim, slot);
        if (path == NULL) continue;
        if (!brainListLoadModesForPath(path, &modes)) continue;
        oldStart = brainModesStartMode(&modes, oldType == gameOpen);
        newStart = brainModesStartMode(&modes, newType == gameOpen);
        if (oldStart == newStart) continue;
        bc = &sim->botConfigs[slot];
        /* Only a bot still on the old type's starting mode follows. One that
         * came in on a remembered pick of some other mode keeps it. */
        if ((int)bc->mode != oldStart) continue;
        serverSimSetBotConfigQuiet(
            sim, slot, (uint8_t)newStart,
            (uint8_t)lobbyLevelAcrossModes(&modes, oldStart,
                                           (int)bc->difficulty, newStart));
    }
}

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

    /* And, on a team the map configures, the level alone against that team.
     * The mode is deliberately not kept: the template owns it, and the next
     * Add Bot on that team is put back into it however far the host moved
     * this one seat. */
    {
        int team = (int)sim->lobbyPlayers[slot].teamNumber;
        if (lobbyTemplateTeam(sim, team) != NULL) {
            SDL_strlcpy(sim->lastTeamBotLevelKey[team],
                        m->levels[bc->difficulty].key,
                        sizeof(sim->lastTeamBotLevelKey[team]));
        }
    }
}

bool serverSimResolveNewBotConfig(const ServerSim *sim, int team,
                                  const char *brainPath,
                                  bool honourManualPick,
                                  uint8_t *ioMode, uint8_t *ioLevel) {
    BrainModes modes;

    if (sim == NULL || ioMode == NULL || ioLevel == NULL) return false;
    if (brainPath == NULL || brainPath[0] == '\0') return false;
    /* A brain with no manifest reads no mode=/difficulty= token at all
     * (botManagerStageInitArg), so there is nothing to resolve. */
    if (!brainListLoadModesForPath(brainPath, &modes)) return false;
    return serverSimResolveNewBotConfigFromModes(sim, team, &modes,
                                                 honourManualPick,
                                                 ioMode, ioLevel);
}

bool serverSimResolveNewBotConfigFromModes(const ServerSim *sim, int team,
                                           const BrainModes *modes,
                                           bool honourManualPick,
                                           uint8_t *ioMode,
                                           uint8_t *ioLevel) {
    int mode;
    int level;
    const ScnLobbyTeam *lt;

    if (sim == NULL || ioMode == NULL || ioLevel == NULL) return false;
    if (modes == NULL) return false;

    /* 1. The base, clamped into this brain's own lists the same way
     *    botInitArgAppendModeTokens clamps it. */
    mode = (int)*ioMode;
    if (mode >= modes->modeCount) mode = 0;
    level = (int)*ioLevel;
    if (level >= modes->modes[mode].levelCount) {
        level = modes->modes[mode].defaultLevel;
    }

    /* 1b. The base gives the level only, never the mode. The mode is the
     *     game type's starting mode: an Open game starts in the brain's
     *     open_default mode (brainModesStartMode), every other type in mode
     *     0. The level moves across by KEY, so Medium in the base's mode is
     *     Medium in the new mode when that mode lists one.
     *     Single player's base is the player's saved "Chosen Mode" pref
     *     (gameFrontSpBotMode in gamefront.c), which outlives the lobby it
     *     was picked in. Turtle bots start only on Open, or when the last
     *     bot the host added in this lobby was a Turtle bot, so a Turtle
     *     pick saved in an Open game must not start the next Tournament's
     *     bots in Turtle. A mode picked in THIS lobby comes
     *     back at step 3, and a scenario's at step 2. */
    {
        int start = brainModesStartMode(
            modes, serverSimGetGameType(sim) == gameOpen);
        if (start != mode) {
            level = lobbyLevelAcrossModes(modes, mode, level, start);
            mode  = start;
        }
    }

    /* 2. What the map requires for this side: the attached scenario's lobby
     *    template, read for the team this bot is joining. The same two keys
     *    the seating applies, resolved by the same call, so a seat the
     *    template made and a seat Add Bot makes on that team land on the
     *    same pair. A key this brain does not list is refused and costs the
     *    seat nothing — as it does at the seating.
     *
     *    Naming a mode FIXES it: the host's pick below may then move the
     *    level inside it and never the mode itself. That is what makes an
     *    Add Bot on Survival's horde a survival bot whatever mode the host
     *    left some other seat in. */
    lt = lobbyTemplateTeam(sim, team);
    if (lt != NULL) {
        uint8_t m = (uint8_t)mode;
        uint8_t l = (uint8_t)level;
        /* This brain's modes are already in hand from the caller, so the
         * template's pair is resolved against them rather than reading
         * modes.txt a second time for the same brain. */
        if (serverSimResolveBotConfigKeysFromModes(modes, lt->mode,
                                                   lt->difficulty, &m, &l)
                == BOT_CFG_KEYS_OK) {
            mode  = (int)m;
            level = (int)l;
        } else {
            /* The template named a key this brain has never heard of, so
             * this is not a team the map can configure after all and the
             * ordinary rule is the right one. */
            lt = NULL;
        }
    }

    /* 3. What a person last picked by hand.
     *
     *    On a templated team that is the level last set on a seat of THAT
     *    team, and the mode is left alone. On every other team it is the
     *    one pair the lobby remembers, which may set both. */
    if (honourManualPick && lt != NULL) {
        const char *key = sim->lastTeamBotLevelKey[team];
        if (key[0] != '\0') {
            int li = brainModeFindLevel(&modes->modes[mode], key);
            if (li >= 0) level = li;
        }
    } else if (honourManualPick && sim->lastBotLevelKey[0] != '\0' &&
               sim->lastBotModeKey[0] != '\0') {
        int mi = brainModesFindMode(modes, sim->lastBotModeKey);
        if (mi >= 0) {
            int li = brainModeFindLevel(&modes->modes[mi],
                                        sim->lastBotLevelKey);
            mode  = mi;
            level = (li >= 0) ? li : modes->modes[mi].defaultLevel;
        }
    }

    *ioMode  = (uint8_t)mode;
    *ioLevel = (uint8_t)level;
    return true;
}

/* ── A scenario's own mode and difficulty ──────────────────────────────
 *
 * The keys a script writes, turned into the two indices the lobby carries.
 * Everything this knows about a brain comes out of that brain's modes.txt,
 * so a key is right or wrong by the same list the host's dropdown is filled
 * from and the validator asks the same question the seating does.
 *
 * The answer is written here, against modes already in hand, and the path
 * form below is the wrapper that reads them off disk first. A caller with
 * several seats on one brain loads once and calls this for each of them. */
BotConfigKeyResult serverSimResolveBotConfigKeysFromModes(
        const BrainModes *modes,
        const char *modeKey,
        const char *levelKey,
        uint8_t *ioMode,
        uint8_t *ioLevel) {
    int        mode;
    int        level;
    bool       modeAsked  = (modeKey  != NULL && modeKey[0]  != '\0');
    bool       levelAsked = (levelKey != NULL && levelKey[0] != '\0');

    if (ioMode == NULL || ioLevel == NULL) return BOT_CFG_KEYS_OK;
    if (!modeAsked && !levelAsked) return BOT_CFG_KEYS_OK;
    /* No modes in hand is a brain with no manifest: it lists no mode to name
       and no level either. */
    if (modes == NULL) return BOT_CFG_KEYS_NO_MANIFEST;

    /* The pair as it stands, clamped into this brain's lists the way
       serverSimResolveNewBotConfig clamps it. */
    mode = (int)*ioMode;
    if (mode >= modes->modeCount) mode = 0;
    level = (int)*ioLevel;

    if (modeAsked) {
        int mi = brainModesFindMode(modes, modeKey);
        if (mi < 0) return BOT_CFG_KEYS_NO_MODE;
        /* A level index means something inside ONE mode's list, so a move to
           another mode drops the old index rather than carrying it across:
           the new mode's own default is where a seat lands unless the script
           names a level as well. */
        if (mi != mode) level = modes->modes[mi].defaultLevel;
        mode = mi;
    }
    if (level >= modes->modes[mode].levelCount) {
        level = modes->modes[mode].defaultLevel;
    }
    if (levelAsked) {
        int li = brainModeFindLevel(&modes->modes[mode], levelKey);
        if (li < 0) return BOT_CFG_KEYS_NO_LEVEL;
        level = li;
    }

    *ioMode  = (uint8_t)mode;
    *ioLevel = (uint8_t)level;
    return BOT_CFG_KEYS_OK;
}

BotConfigKeyResult serverSimResolveBotConfigKeys(const char *brainPath,
                                                 const char *modeKey,
                                                 const char *levelKey,
                                                 uint8_t *ioMode,
                                                 uint8_t *ioLevel) {
    BrainModes modes;

    /* The two questions that are answered without looking at a brain at all
       are asked here as well as in the variant, so a caller that names no key
       still costs no disk read. */
    if (ioMode == NULL || ioLevel == NULL) return BOT_CFG_KEYS_OK;
    if ((modeKey  == NULL || modeKey[0]  == '\0') &&
        (levelKey == NULL || levelKey[0] == '\0')) {
        return BOT_CFG_KEYS_OK;
    }
    if (brainPath == NULL || brainPath[0] == '\0' ||
        !brainListLoadModesForPath(brainPath, &modes)) {
        return serverSimResolveBotConfigKeysFromModes(NULL, modeKey, levelKey,
                                                      ioMode, ioLevel);
    }
    return serverSimResolveBotConfigKeysFromModes(&modes, modeKey, levelKey,
                                                  ioMode, ioLevel);
}

void serverSimSetBotConfigQuiet(ServerSim *sim, BYTE slot,
                                uint8_t mode, uint8_t difficulty) {
    if (sim == NULL || slot >= MAX_TANKS) return;
    sim->botConfigs[slot].mode       = mode;
    sim->botConfigs[slot].difficulty = difficulty;
    serverSimQueueBotConfigPublish(sim, slot);
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
     * bot's survival mode. */
    serverSimResolveNewBotConfig(sim, team, brainPath, honourManualPick,
                                 &mode, &level);
    sim->botModeSetByHand &= (uint16_t)~(1u << slot);
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

/* The side facing this one. START_SIDE_ANY, and anything outside the
 * START_SIDE_* range, has no opposite and comes back unchanged. */
static uint8_t lobbyOppositeSide(uint8_t side) {
    switch (side) {
        case START_SIDE_N: return START_SIDE_S;
        case START_SIDE_S: return START_SIDE_N;
        case START_SIDE_E: return START_SIDE_W;
        case START_SIDE_W: return START_SIDE_E;
        default:           return START_SIDE_ANY;
    }
}

/* The one other team holding players, or 0 when there is not exactly one.
 * Counted by connected members rather than the in_use flag, so a team row
 * that exists but nobody is on does not count as present — the same test
 * the start rules make when they decide which sides are closed. */
static BYTE lobbyLoneOtherTeamWithPlayers(const ServerSim *sim, BYTE teamId) {
    BYTE found = 0;
    BYTE k;
    for (k = 0; k < MAX_TANKS; k++) {
        BYTE t;
        if (!sim->playerConnected[k]) continue;
        t = sim->lobbyPlayers[k].teamNumber;
        if (t == 0 || t >= MAX_TANKS || t == teamId) continue;
        if (found != 0 && found != t) return 0;   /* more than one other team */
        found = t;
    }
    return found;
}

/* Keep the one other team's filled-in side in step with teamId's, which has
 * just been written. The other team's row is published here when it changes.
 *
 * Only the two-team case has an answer to give. With three teams two would
 * share a side, or the third would take an east or west that many maps have
 * no starts on, so nothing is written and the placement rules confine the
 * unnamed teams instead.
 *
 * sideAutoFilled says which sides this may write: one filled in here, or
 * none at all. A side a player named is left alone, both when the other team
 * changes its own side and when it drops it. Moving from one real side to
 * another re-mirrors a filled-in side; dropping to START_SIDE_ANY takes a
 * filled-in side back to START_SIDE_ANY, because the only reason it was
 * there has gone.
 *
 * in_use is deliberately not set. The side is stored, published and used
 * whatever in_use says; setting it would tell the client the team has a name
 * and colour of its own, and a team that never had its meta set has neither,
 * so the lobby would show an empty name and colour 0 instead of "Team N" and
 * the team's default colour. */
static void lobbyMirrorSideToLoneOtherTeam(ServerSim *sim, BYTE teamId,
                                           uint8_t startSide) {
    BYTE other = lobbyLoneOtherTeamWithPlayers(sim, teamId);
    TeamMetadata *ot;
    uint8_t want;
    if (other == 0) return;
    ot = serverSimGetTeamMetaMut(sim, other);
    if (ot == NULL) return;
    if (startSide == START_SIDE_ANY) {
        if (!ot->sideAutoFilled) return;
        want = START_SIDE_ANY;
    } else {
        if (ot->startSide != START_SIDE_ANY && !ot->sideAutoFilled) return;
        want = lobbyOppositeSide(startSide);
    }
    if (ot->startSide == want &&
        ot->sideAutoFilled == (uint8_t)(want != START_SIDE_ANY)) {
        return;
    }
    ot->startSide      = want;
    ot->sideAutoFilled = (uint8_t)(want != START_SIDE_ANY ? 1 : 0);
    serverSimPublishLobbyTeamMeta(sim, other);
}

/* start_sides.h repeats the deep sea terrain value; keep the two equal. */
SDL_COMPILE_TIME_ASSERT(start_side_deep_sea,
                        START_SIDE_TERRAIN_DEEP_SEA == DEEP_SEA);

/* The sides a fresh lobby starts on, from the shape of the live map. The
 * squares that are not deep sea are measured (startSideDefaultIsEastWest):
 * when they span more columns than rows the pair is east/west, otherwise
 * (a tie included) north/south. The lower team id takes the first side of
 * the pair and the next one the second, the order the map preview's
 * compass assigns an axis in (lobby_side_axis.h): north then south, east
 * then west. Pills, bases and starts do not count on their own; a start on
 * deep sea is one square and does not change the answer on a real map. A
 * buffer that cannot be had gives north/south, the answer before the map
 * was measured. */
static void lobbyDefaultSidePair(const ServerSim *sim,
                                 uint8_t *outA, uint8_t *outB) {
    BYTE *terrain;
    bool eastWest = false;
    terrain = (BYTE *)SDL_malloc(SERVER_SIM_TERRAIN_BYTES);
    if (terrain != NULL) {
        if (serverSimGetMapTerrainBuffer(sim, terrain, SERVER_SIM_TERRAIN_BYTES)) {
            eastWest = startSideDefaultIsEastWest(terrain, MAP_ARRAY_SIZE,
                                                  MAP_ARRAY_SIZE, MAP_ARRAY_SIZE);
        }
        SDL_free(terrain);
    }
    if (eastWest) {
        *outA = START_SIDE_E;
        *outB = START_SIDE_W;
    } else {
        *outA = START_SIDE_N;
        *outB = START_SIDE_S;
    }
}

/* The sides a fresh lobby starts on: teams 1 and 2 on the pair
 * lobbyDefaultSidePair picks for the map's shape. Every other team is left
 * with no side, which the start rules already keep off the two chosen
 * sides; a third team is not given the other axis, because many maps have
 * no starts there.
 *
 * Both sides are marked filled-in rather than named, so the lobby treats
 * them as the answer to the other team's side: a host who moves team 1 to
 * another side has team 2 follow to the opposite one, and one who takes
 * team 1 back to no side takes team 2 with it. A side the host names for a
 * team clears the mark, and from then on it is never written over.
 *
 * A map with no starts on the chosen sides needs nothing here: the
 * placement already drops a side with no valid start back to no side.
 *
 * Nothing is published. Both callers run with no subscribers (a server
 * starting up, a lobby the last human has just left), and the next joiner
 * reads the sides from the join-time replay of teams 1 and 2. */
void serverSimApplyDefaultTeamSides(ServerSim *sim) {
    TeamMetadata *t1;
    TeamMetadata *t2;
    uint8_t a;
    uint8_t b;
    if (sim == NULL) return;
    t1 = serverSimGetTeamMetaMut(sim, 1);
    t2 = serverSimGetTeamMetaMut(sim, 2);
    if (t1 == NULL || t2 == NULL) return;
    lobbyDefaultSidePair(sim, &a, &b);
    t1->startSide      = a;
    t1->sideAutoFilled = 1;
    t2->startSide      = b;
    t2->sideAutoFilled = 1;
}

/* The map has changed under a lobby. When teams 1 and 2 still hold the
 * default pair untouched — both sides filled-in, north/south or east/west
 * in the default order — the pair is picked again for the new map's shape
 * and both teams are published. Only the default does both marks at once:
 * a side the host names clears its mark, and the two-team mirror marks
 * only the side it fills in, so any side the host chose is left as it is.
 * Run before the reservations are reconciled, so the starts are re-picked
 * against the new sides. Returns true when the sides changed. */
bool serverSimRefreshDefaultTeamSides(ServerSim *sim) {
    TeamMetadata *t1;
    TeamMetadata *t2;
    uint8_t a;
    uint8_t b;
    bool isDefaultPair;
    if (sim == NULL) return false;
    t1 = serverSimGetTeamMetaMut(sim, 1);
    t2 = serverSimGetTeamMetaMut(sim, 2);
    if (t1 == NULL || t2 == NULL) return false;
    if (!t1->sideAutoFilled || !t2->sideAutoFilled) return false;
    isDefaultPair =
        (t1->startSide == START_SIDE_N && t2->startSide == START_SIDE_S) ||
        (t1->startSide == START_SIDE_E && t2->startSide == START_SIDE_W);
    if (!isDefaultPair) return false;
    lobbyDefaultSidePair(sim, &a, &b);
    if (t1->startSide == a && t2->startSide == b) return false;
    t1->startSide = a;
    t2->startSide = b;
    serverSimPublishLobbyTeamMeta(sim, 1);
    serverSimPublishLobbyTeamMeta(sim, 2);
    return true;
}

void serverSimSetTeamMeta(ServerSim *sim, BYTE teamId,
                           uint8_t color, uint8_t namingPool,
                           uint8_t startSide,
                           const uint8_t *name, uint8_t nameLen) {
    if (!sim || teamId == 0 || teamId >= MAX_TANKS) return;
    TeamMetadata *t = serverSimGetTeamMetaMut(sim, teamId);
    if (t == NULL) return;
    uint8_t prevSide = t->startSide;
    bool repeatsAll;
    /* A side outside the START_SIDE_* range means no side. */
    if (startSide >= START_SIDE_COUNT) startSide = START_SIDE_ANY;
    /* Whether this write names every field as the team already has it.
     * The lobby sends one of those only when the host picks the side the
     * team already shows: a pool, colour or name edit changes that field. */
    repeatsAll = t->in_use && prevSide == startSide &&
                 t->color == color && t->namingPool == namingPool &&
                 SDL_strnlen(t->name, LOBBY_TEAM_NAME_LEN) == nameLen &&
                 (nameLen == 0 ||
                  (name != NULL && memcmp(t->name, name, nameLen) == 0));
    t->in_use = 1;
    t->color = color;
    t->startSide = startSide;
    /* A write that changes the side makes it the team's own choice —
     * START_SIDE_ANY included — and the fill-in below never writes over it
     * again. So does a write that repeats every field: that is the host
     * picking the side the team already shows, and on the lobby's default
     * pair it keeps that pair through a map change. A write that repeats
     * the side but changes another field leaves the mark alone: the lobby
     * sends the side it already shows with every rename, colour and pool
     * edit, and renaming a team is not choosing its side. */
    if (prevSide != startSide || repeatsAll) {
        t->sideAutoFilled = 0;
    }
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
    /* Naming a side in a two-team lobby names the other team's too. A
     * player choosing north means south for the only other team, and
     * saying so puts it on that team's row rather than leaving it to be
     * inferred: an unnamed team is kept off the chosen sides either way,
     * but nothing in the lobby says where it ends up. The helper decides
     * what the other team's row should read, including taking a side it
     * filled in earlier back off when this team drops its own. */
    if (prevSide != startSide) {
        lobbyMirrorSideToLoneOtherTeam(sim, teamId, startSide);
    }
    /* A side change moves every reservation, picks included, so the lobby
     * ends up as if everyone had joined after the sides were set. One
     * re-pick covers both teams: the other team's side can only change when
     * this one's did, so the test above is true for either. */
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
    /* Zeroing the struct drops the side too; that is a side change. It also
     * takes away the reason the one other team was given the opposite side,
     * so a side filled in for that team goes back to START_SIDE_ANY — the
     * same answer as setting this team to START_SIDE_ANY. A side that team
     * named itself is left alone. */
    if (hadSide) {
        lobbyMirrorSideToLoneOtherTeam(sim, teamId, START_SIDE_ANY);
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

/* Whether this write is one the attached scenario fixes: the game type it
 * declared, the ranked flag a scripted round cannot be measured under, and
 * the AI policy that would take every bot it fields off the roster. The
 * three arms below refuse through this, and the reject code the dispatcher
 * sends back reads the same answer — so the reason a host is shown cannot
 * drift from the test that produced the refusal.
 *
 * Each arm's own payload check is mirrored here, so a malformed write is
 * still refused as malformed rather than blamed on the scenario. */
static bool lobbySettingScenarioFixes(const ServerSim *sim, uint8_t lst,
                                      const uint8_t *value, size_t len) {
    if (sim == NULL || value == NULL) return false;
    if (sim->scenarioIdentity.source == lobbyScenarioNone) return false;
    if (len != 1) return false;
    switch (lst) {
        /* Every value the handler admits would take the lobby off
           gameScripted, so with a scenario attached none of them is the
           host's to send. A mod is not a scenario: it keeps the round's win
           condition, names no game of its own and never moved the lobby onto
           gameScripted in the first place, so the type stays the host's for
           as long as mods are all that is attached. */
        case LST_GAME_TYPE:
            if (sim->scenarioIdentity.keepsWinCondition) return false;
            return value[0] >= 1 && value[0] <= 3;
        case LST_RANKED:    return value[0] != 0;
        case LST_AI_POLICY: return (aiType)value[0] == aiNone;
        default:            return false;
    }
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
 * off, or the attached scenario fixes the value). */
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
               type that says so is the map commit's to set. */
            if (lobbySettingScenarioFixes(sim, lst, value, len)) return false;
            {
                gameType was = serverSimGetGameType(sim);
                serverSimSetGameType(sim, (gameType)value[0]);
                /* Bots nobody set a mode for follow the new type's
                   starting mode (Open starts in open_default). */
                serverSimFollowGameTypeBotModes(sim, was);
            }
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
            if (lobbySettingScenarioFixes(sim, lst, value, len)) return false;
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
            if (lobbySettingScenarioFixes(sim, lst, value, len)) return false;
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
        case LST_POSITIONAL_SOUND: {
            if (len != 1) return false;
            /* Classic mode plays every sound centred and owns this value
             * while it is set, the same as LST_ALLIES_IN_TREES. A plain
             * bool, so any non-zero byte means on. */
            if (sim->classicMode) return false;
            serverSimSetPositionalSound(sim, value[0] != 0);
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
        case LST_SMART_PINGS_OFF: {
            if (len != 1) return false;
            /* A plain bool like LST_ALLIES_IN_TREES, so any non-zero byte
             * counts. Classic mode does not own it: what a player may
             * point at is not one of the visibility rules. */
            serverSimSetSmartPingsOff(sim, value[0] != 0);
            return true;
        }
        case LST_MODS_OFF: {
            if (len != 1) return false;
            /* A plain bool like LST_SMART_PINGS_OFF above, so any non-zero
             * byte counts. The pick list is left alone: this decides whether
             * the mods on it compose, not whether they are on it.
             *
             * Nothing is recomposed here. The caller in
             * server_command_dispatch.c asks for that through
             * lobbyScenarioReselect, which is the same call every pick path
             * makes and is what sends the script list and the seats out with
             * it. This translation unit cannot reach that static, and a
             * second copy of it here would be a second thing to keep in
             * step. */
            serverSimSetModsOff(sim, value[0] != 0);
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

CmdResult serverSimApplyLobbySettingResult(ServerSim *sim,
                                           uint8_t lst,
                                           const uint8_t *value, size_t len) {
    if (!serverSimApplyLobbySettingInner(sim, lst, value, len)) {
        /* Why, for the line the sender is shown: a setting the map's
           scenario fixes says so, and everything else is the payload being
           one this lobby will not take. */
        return lobbySettingScenarioFixes(sim, lst, value, len)
                   ? CMD_REJECT_SCENARIO : CMD_REJECT_INVALID;
    }
    serverSimPublishLobbySettings(sim);
    lobbyAutoUnreadyOnChange(sim);
    serverSimWbnLobbyUpdate(sim, FALSE);
    return CMD_OK;
}

bool serverSimApplyLobbySetting(ServerSim *sim,
                                uint8_t lst,
                                const uint8_t *value, size_t len) {
    return serverSimApplyLobbySettingResult(sim, lst, value, len) == CMD_OK;
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
