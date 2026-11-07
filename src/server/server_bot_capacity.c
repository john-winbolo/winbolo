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

/* The dedicated server's host bot capacity - see server_bot_capacity.h.
 *
 * A small state machine, stepped by serverBotCapacityPoll on the tick
 * thread:
 *   IDLE       - nothing known yet. Reads the stored result once. With none
 *                stored, starts the test the first tick the server is in its
 *                lobby.
 *   TESTING    - the CPU speed test runs on its own threads. The game
 *                starting stops it (CANCELLING); a finished test is read
 *                back from the store (QUERY).
 *   CANCELLING - waits for the stopped test's threads, then back to IDLE,
 *                so the next lobby tries again.
 *   QUERY      - a worker thread loads the bot brain to ask Brain.capacity
 *                (a brain load is too slow for the tick thread).
 *   DONE       - the sim has its number.
 * The sim reads MEASURING through TESTING and QUERY, so a lobby Add Bot
 * waits for the number; NONE (no cap) when there is no result. */

#include "server_bot_capacity.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "brain_capacity.h"
#include "cpu_speed_capacity.h"
#include "cpu_speed_store.h"
#include "cpu_speed_test.h"
#include "threads.h"
#include "../gui/sdl3/luabrainshandler.h" /* luaBrainQueryCapacity */

#ifndef WINBOLO_VERSION
#define WINBOLO_VERSION "dev"
#endif

typedef enum {
  SBC_IDLE = 0,
  SBC_TESTING,
  SBC_CANCELLING,
  SBC_QUERY,
  SBC_DONE
} SbcState;

#define SBC_PATH_LEN 1024

static SbcState s_state = SBC_IDLE;
static bool s_storeChecked = false;
static bool s_fromStore = false;
static char s_brainPath[SBC_PATH_LEN];
static char s_brainName[128];
static SDL_Thread *s_queryThread = NULL;
static SDL_AtomicInt s_queryDone;
static int s_queryResult = BRAIN_CAPACITY_UNKNOWN;

static void sbcSetSim(ServerSim *sim, int capacity) {
  threadsWaitForMutex();
  serverSimSetHostBotCapacity(sim, capacity);
  threadsReleaseMutex();
}

/* The CPU speed test's own log lines, from its thread. */
static void sbcTestLog(const char *line) {
  fprintf(stderr, "%s\n", line);
}

static int SDLCALL sbcQueryMain(void *data) {
  (void)data;
  (void)SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
  s_queryResult = luaBrainQueryCapacity(s_brainPath, s_brainName);
  SDL_SetAtomicInt(&s_queryDone, 1);
  return 0;
}

/* Gives the stored result to the brain capacity code and starts the brain
   query. False when the server has no bot brain to ask about. */
static bool sbcStartQuery(ServerSim *sim) {
  threadsWaitForMutex();
  SDL_strlcpy(s_brainPath, serverSimGetBotBrainPath(sim), sizeof(s_brainPath));
  threadsReleaseMutex();
  if (s_brainPath[0] == '\0') {
    return false;
  }
  brainCapacityNameFromPath(s_brainPath, s_brainName, sizeof(s_brainName));
  SDL_SetAtomicInt(&s_queryDone, 0);
  s_queryResult = BRAIN_CAPACITY_UNKNOWN;
  s_queryThread = SDL_CreateThread(sbcQueryMain, "wb-bot-capacity", NULL);
  if (s_queryThread == NULL) {
    /* No thread: ask here. It is once, and only at a lobby start. */
    sbcQueryMain(NULL);
  }
  return true;
}

/* A result is in the store (just made, or from an earlier start): hand it
   to the brain capacity code and ask the bot brain. */
static void sbcUseStored(ServerSim *sim, bool fromStore) {
  (void)cpuSpeedApplyStoredToBrains(WINBOLO_VERSION);
  s_fromStore = fromStore;
  if (sbcStartQuery(sim)) {
    sbcSetSim(sim, SERVER_HOST_BOT_CAPACITY_MEASURING);
    s_state = SBC_QUERY;
  } else {
    fprintf(stderr, "Bot capacity: no bot brain, so no cap\n");
    sbcSetSim(sim, SERVER_HOST_BOT_CAPACITY_NONE);
    s_state = SBC_DONE;
  }
}

void serverBotCapacityInit(ServerSim *sim, int override) {
  s_state = SBC_IDLE;
  s_storeChecked = false;
  s_queryThread = NULL;
  SDL_SetAtomicInt(&s_queryDone, 0);
  if (override >= 0) {
    if (override > BRAIN_CAPACITY_MAX) {
      override = BRAIN_CAPACITY_MAX;
    }
    fprintf(stderr, "Bot capacity: override %d (-botcapacity)\n", override);
    sbcSetSim(sim, override);
    s_state = SBC_DONE;
  }
}

void serverBotCapacityPoll(ServerSim *sim) {
  bool inLobby;

  if (sim == NULL || s_state == SBC_DONE) {
    return;
  }
  threadsWaitForMutex();
  inLobby = serverSimIsLobbyEnabled(sim) &&
            serverSimGetState(sim) == serverStateLobby;
  threadsReleaseMutex();

  switch (s_state) {
  case SBC_IDLE:
    if (!s_storeChecked) {
      CpuSpeedStored stored;
      s_storeChecked = true;
      if (cpuSpeedStoreLoad(WINBOLO_VERSION, &stored)) {
        sbcUseStored(sim, true);
        return;
      }
    }
    if (!inLobby) {
      return;
    }
    if (cpuSpeedBackgroundStart(WINBOLO_VERSION, CPU_SPEED_CONTEXT_SERVER,
                                sbcTestLog)) {
      sbcSetSim(sim, SERVER_HOST_BOT_CAPACITY_MEASURING);
      s_state = SBC_TESTING;
    } else if (!cpuSpeedBackgroundRunning()) {
      /* Nothing started and nothing runs: a result was stored since the
         check above, or the test could not start. */
      CpuSpeedStored stored;
      if (cpuSpeedStoreLoad(WINBOLO_VERSION, &stored)) {
        sbcUseStored(sim, true);
      } else {
        fprintf(stderr,
                "Bot capacity: the CPU speed test could not start, so no "
                "cap\n");
        sbcSetSim(sim, SERVER_HOST_BOT_CAPACITY_NONE);
        s_state = SBC_DONE;
      }
    }
    return;

  case SBC_TESTING:
    if (!inLobby) {
      cpuSpeedBackgroundCancel();
      sbcSetSim(sim, SERVER_HOST_BOT_CAPACITY_NONE);
      s_state = SBC_CANCELLING;
      return;
    }
    if (!cpuSpeedBackgroundRunning()) {
      CpuSpeedStored stored;
      if (cpuSpeedStoreLoad(WINBOLO_VERSION, &stored)) {
        sbcUseStored(sim, false);
      } else {
        fprintf(stderr,
                "Bot capacity: the CPU speed test gave no result, so no "
                "cap\n");
        sbcSetSim(sim, SERVER_HOST_BOT_CAPACITY_NONE);
        s_state = SBC_DONE;
      }
    }
    return;

  case SBC_CANCELLING:
    if (!cpuSpeedBackgroundRunning()) {
      fprintf(stderr,
              "Bot capacity: the game started before the CPU speed test "
              "finished; nothing stored, the next lobby tries again\n");
      s_state = SBC_IDLE;
    }
    return;

  case SBC_QUERY:
    if (SDL_GetAtomicInt(&s_queryDone)) {
      int capacity = s_queryResult;
      bool loaded = capacity != BRAIN_CAPACITY_UNKNOWN;
      SDL_WaitThread(s_queryThread, NULL);
      s_queryThread = NULL;
      if (!loaded) {
        /* The brain did not load here: the default answer for this
           machine, the one a brain with no capacity function gets. */
        BrainMachineInfo machine;
        brainCapacityGetMachine(&machine);
        capacity = brainCapacityDefault(&machine);
      }
      fprintf(stderr, "Bot capacity: %d bots of %s (CPU speed test, %s%s)\n",
              capacity, s_brainName,
              s_fromStore ? "stored result" : "measured now",
              loaded ? "" : "; the brain did not load, default answer");
      sbcSetSim(sim, capacity);
      s_state = SBC_DONE;
    }
    return;

  case SBC_DONE:
  default:
    return;
  }
}

void serverBotCapacityShutdown(void) {
  cpuSpeedBackgroundCancel();
  (void)cpuSpeedBackgroundWait(2000);
  if (s_queryThread != NULL) {
    SDL_WaitThread(s_queryThread, NULL);
    s_queryThread = NULL;
  }
}
