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
 *Name:          Server Simulation Pause
 *Filename:      server_sim_pause.c
 *Author:        John Morrison
 *Purpose:
 *  The bot pause and the game pause (server_sim.h): the
 *  manual pause of each target, the idle pause that starts
 *  when the server has no human, the seconds each pause has
 *  been on, the status line for the console and the notice
 *  clients are sent. The bot manager reads botMgr.paused;
 *  serverSimLocalTick reads serverSimIsGamePaused and runs
 *  serverSimPausedGameFrame in place of the sim tick.
 *********************************************************/

#include <stdio.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"

/* Frames (20 ms) a second. serverSimPauseFrame runs once a frame. */
#define PAUSE_FRAMES_PER_SECOND 50u

static bool validTarget(SimPauseTarget target) {
    return target == SIM_PAUSE_BOTS || target == SIM_PAUSE_GAME;
}

static bool idleOn(const ServerSim *sim, SimPauseTarget target) {
    return sim->pause.idle && sim->pause.idleTarget == (uint8_t)target;
}

static uint8_t computeFlags(const ServerSim *sim) {
    uint8_t flags = 0;
    if (sim->pause.manual[SIM_PAUSE_BOTS] || idleOn(sim, SIM_PAUSE_BOTS)) {
        flags |= SIM_PAUSE_FLAG_BOTS;
    }
    if (sim->pause.manual[SIM_PAUSE_GAME] || idleOn(sim, SIM_PAUSE_GAME)) {
        flags |= SIM_PAUSE_FLAG_GAME;
    }
    return flags;
}

/* Bring the derived state up to date after any change: the flags, the
 * start frame of each pause that has just come on, the bot manager's flag
 * and a console line for each edge. */
static void refreshPause(ServerSim *sim) {
    uint8_t before = sim->pause.flags;
    uint8_t after  = computeFlags(sim);
    sim->pause.flags = after;
    /* A game pause stops the bot tick as well, so the bots only need their
     * own flag for a bot pause; but both are set so a game pause that is
     * lifted while a bot pause stays leaves the bots stopped. */
    sim->botMgr.paused = (after & SIM_PAUSE_FLAG_BOTS) != 0;
    if (before == after) {
        return;
    }
    sim->pause.notifyOwed = true;
    static const struct {
        uint8_t        flag;
        SimPauseTarget target;
        const char    *on;
        const char    *off;
    } kEdges[] = {
        { SIM_PAUSE_FLAG_BOTS, SIM_PAUSE_BOTS, "AI players paused", "AI players resumed" },
        { SIM_PAUSE_FLAG_GAME, SIM_PAUSE_GAME, "Game paused",       "Game resumed" },
    };
    for (size_t i = 0; i < sizeof(kEdges) / sizeof(kEdges[0]); i++) {
        bool was = (before & kEdges[i].flag) != 0;
        bool is  = (after & kEdges[i].flag) != 0;
        if (was == is) {
            continue;
        }
        char msg[96];
        if (is) {
            sim->pause.sinceFrame[kEdges[i].target] = sim->pause.frames;
            bool manual = sim->pause.manual[kEdges[i].target];
            snprintf(msg, sizeof(msg), "%s (%s).", kEdges[i].on,
                     manual ? "manual" : "no human in the server");
        } else {
            snprintf(msg, sizeof(msg), "%s.", kEdges[i].off);
        }
        serverSimConsoleMessage(msg);
    }
}

bool serverSimSetManualPause(ServerSim *sim, SimPauseTarget target, bool on) {
    if (sim == NULL || !validTarget(target)) {
        return false;
    }
    if (sim->pause.manual[target] == on) {
        return false;
    }
    sim->pause.manual[target] = on;
    refreshPause(sim);
    return true;
}

bool serverSimIsManualPause(const ServerSim *sim, SimPauseTarget target) {
    return sim != NULL && validTarget(target) && sim->pause.manual[target];
}

bool serverSimAreBotsPaused(const ServerSim *sim) {
    return sim != NULL && (sim->pause.flags & SIM_PAUSE_FLAG_BOTS) != 0;
}

bool serverSimIsGamePaused(const ServerSim *sim) {
    return sim != NULL && (sim->pause.flags & SIM_PAUSE_FLAG_GAME) != 0;
}

uint8_t serverSimGetPauseFlags(const ServerSim *sim) {
    return sim != NULL ? sim->pause.flags : 0;
}

void serverSimSetIdlePause(ServerSim *sim, int seconds, SimPauseTarget target) {
    if (sim == NULL || !validTarget(target)) {
        return;
    }
    if (seconds < -1) {
        seconds = -1;
    }
    sim->pause.idleSeconds = seconds;
    if (sim->pause.idle && sim->pause.idleTarget != (uint8_t)target) {
        /* The idle pause moves to the new target on the next frame. */
        sim->pause.idle = false;
    }
    sim->pause.idleTarget = (uint8_t)target;
    if (seconds < 0) {
        sim->pause.idle = false;
        sim->pause.noHumanFrames = 0;
    }
    refreshPause(sim);
}

int serverSimGetIdlePauseSeconds(const ServerSim *sim) {
    return sim != NULL ? sim->pause.idleSeconds : -1;
}

SimPauseTarget serverSimGetIdlePauseTarget(const ServerSim *sim) {
    return (sim != NULL && sim->pause.idleTarget == SIM_PAUSE_GAME)
               ? SIM_PAUSE_GAME : SIM_PAUSE_BOTS;
}

bool serverSimIsIdlePauseActive(const ServerSim *sim) {
    return sim != NULL && sim->pause.idle;
}

uint32_t serverSimGetPauseSeconds(const ServerSim *sim, SimPauseTarget target) {
    if (sim == NULL || !validTarget(target)) {
        return 0;
    }
    uint8_t flag = (target == SIM_PAUSE_BOTS) ? SIM_PAUSE_FLAG_BOTS
                                              : SIM_PAUSE_FLAG_GAME;
    if ((sim->pause.flags & flag) == 0) {
        return 0;
    }
    return (sim->pause.frames - sim->pause.sinceFrame[target]) /
           PAUSE_FRAMES_PER_SECOND;
}

static void formatTarget(const ServerSim *sim, SimPauseTarget target,
                         char *buf, size_t bufSize) {
    uint8_t flag = (target == SIM_PAUSE_BOTS) ? SIM_PAUSE_FLAG_BOTS
                                              : SIM_PAUSE_FLAG_GAME;
    if ((sim->pause.flags & flag) == 0) {
        snprintf(buf, bufSize, "running");
        return;
    }
    /* A manual pause is named first: it is the one a human join does not
     * lift. */
    const char *reason = sim->pause.manual[target]
                             ? (idleOn(sim, target) ? "manual+idle" : "manual")
                             : "idle";
    snprintf(buf, bufSize, "paused (%s, %u s)", reason,
             (unsigned)serverSimGetPauseSeconds(sim, target));
}

void serverSimFormatPauseStatus(const ServerSim *sim, char *buf, size_t bufSize) {
    if (buf == NULL || bufSize == 0) {
        return;
    }
    if (sim == NULL) {
        buf[0] = '\0';
        return;
    }
    char bots[48];
    char game[48];
    char idle[80];
    formatTarget(sim, SIM_PAUSE_BOTS, bots, sizeof(bots));
    formatTarget(sim, SIM_PAUSE_GAME, game, sizeof(game));
    const char *mode = sim->pause.idleTarget == SIM_PAUSE_GAME ? "game" : "bots";
    if (sim->pause.idleSeconds < 0) {
        snprintf(idle, sizeof(idle), "off");
    } else {
        snprintf(idle, sizeof(idle), "%d s, mode %s, %s",
                 (int)sim->pause.idleSeconds, mode,
                 sim->pause.idle ? "on" : "waiting");
    }
    snprintf(buf, bufSize, "bots: %s; game: %s; idle pause: %s",
             bots, game, idle);
}

void serverSimPauseFrame(ServerSim *sim) {
    if (sim == NULL) {
        return;
    }
    sim->pause.frames++;
    if (sim->pause.idleSeconds < 0) {
        return;
    }
    if (serverSimGetNumHumans(sim) > 0) {
        sim->pause.noHumanFrames = 0;
        if (sim->pause.idle) {
            sim->pause.idle = false;
            refreshPause(sim);
        }
        return;
    }
    if (sim->pause.noHumanFrames < UINT32_MAX) {
        sim->pause.noHumanFrames++;
    }
    if (sim->pause.idle) {
        return;
    }
    /* A lobby round that had a human goes back to the lobby when the last
     * human leaves (serverSimTick). A game pause would stop the tick that
     * makes that check, so it waits for the lobby. A bot pause does not
     * stop the tick and may start at once. */
    if (sim->pause.idleTarget == SIM_PAUSE_GAME &&
        sim->state == serverStateRunning &&
        sim->lobbyEnabled && sim->roundHadHuman) {
        return;
    }
    uint32_t need = (uint32_t)sim->pause.idleSeconds * PAUSE_FRAMES_PER_SECOND;
    if (sim->pause.noHumanFrames >= need) {
        sim->pause.idle = true;
        refreshPause(sim);
    }
}

void serverSimPauseNotify(ServerSim *sim) {
    if (sim == NULL || sim->state != serverStateRunning) {
        return;
    }
    bool repeat = sim->pause.flags != 0 &&
                  (sim->pause.frames - sim->pause.notifyFrame) >= SIM_PAUSE_NOTIFY_FRAMES;
    if (!sim->pause.notifyOwed && !repeat) {
        return;
    }
    GameEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type    = EVENT_SERVER_MSG;
    ev.data[0] = (uint8_t)(SERVER_MSG_PAUSE_STATE_BASE + (sim->pause.flags & 0x03u));
    serverSimAddEvent(sim, &ev);
    sim->pause.notifyOwed  = false;
    sim->pause.notifyFrame = sim->pause.frames;
}
