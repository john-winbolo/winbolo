/*
 * $Id$
 *
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


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../winbolonet/http.h"

#ifdef _WIN32
  #include <WinSock2.h>
  #include <process.h>
  #define getpid _getpid
#else
  #include <sys/time.h>
  #include <sys/types.h>
  #include <unistd.h>
  #include <signal.h>
  #include <errno.h>
  #include <SDL3/SDL.h>
  #define USING_SDL
#endif

#include "everard_map.h"

#include "bolo_rand.h"
#include "debug_file_output.h"
#include "geolookup.h"
#include "global.h"
#include "gametype.h"
#include "threads.h"
#include "../winbolonet/winbolonet_core.h"
#include "../winbolonet/winbolonet_server.h"
#include "server_sim.h"
#include "server_sim_lifecycle.h"
#include "server_sim_join.h"      /* serverSimRepickAllLobbyStarts — after -allybots / -teams move the bots */
#include "mapgen.h"
#include "log.h"
#include "transport_udp.h"
#include "net_impair.h"   /* WB_ENABLE_NETIMPAIR master switch */
#include "bot_manager.h"
#include "brain_list.h"  /* BrainModes, brainListLoadModesForPath — -mode */
#include "bot_worker_pool.h"
#include "lobby_bot_pools.h"
#include "brain_record.h"
#include "../gui/sdl3/luabrainshandler.h"  /* luaBrainsSetProfile — shared with BrainTest */
#include "server_dedicated_log.h"
#include "server_lifecycle.h"
#include "../common/sentry_integration.h"
#include "../common/wb_log.h"
#include "../common/prefs.h"
#include "../headless/cmd_stdin.h"
#include "server_console.h"
#include "wire_limits.h"
#include "../scenario/scenario_host.h"
#include "../scenario/scenario_pack.h"
#include "../scenario_io/scenario_package.h"
#include "../scenario/scenario_validate.h"
#include "scenario_settings.h"
#include "cJSON.h"

/* Constants previously from backend.h */
#define GAME_TICK_LENGTH 10
#ifndef GAME_NUMGAMETICKS_SEC
#define GAME_NUMGAMETICKS_SEC (1000 / 20)
#endif

/* From servermessages.c — called directly now instead of through servercore wrappers */
void serverMessageSetQuietMode(ServerSim *sim, bool modeOn);
void serverMessagesSetLogFile(ServerSim *sim, char *logFile);
void serverMessageConsoleMessage(ServerSim *sim, char *msg);

#ifndef _AITYPE_ENUM
#define _AITYPE_ENUM
typedef enum {
  aiNone,
  aiYes,
  aiYesAdvantage,
  aiFull
} aiType;
#endif

#ifndef MAX_PATH
	#define MAX_PATH 512
#endif


#include <ctype.h>
#include <time.h>

DWORD oldTick;     /* Number of ticks passed */
bool isQuiet = FALSE;
bool isNoInput = FALSE;
/* -asap: run game ticks back-to-back instead of one per SERVER_TICK_LENGTH of
 * wall clock. Nothing in a tick needs real time to pass between ticks --
 * botManagerTick joins the worker pool inside the tick -- so the only thing
 * the 20 ms timer buys a headless measurement run is wall-clock waiting.
 * Read from the tick loop and from the command loops (which shorten their
 * sleeps so they don't pace the game); set once, before any thread starts. */
bool isAsap = FALSE;
unsigned int serverTimerGameID = 1;
/* The dedicated-server replay-log state (former fileName/isLogging/
 * dontSendLog globals) is now private to server_dedicated_log.c. This TU
 * drives it via serverDedicatedLogInstall() and reads it back for the
 * teardown upload via serverDedicatedLogIsActive()/CurrentFile(). */

bool statusFile = FALSE;

time_t ticks = 0;

static ServerSim *serverSim = NULL;
static ScenarioHost *scenarioHost = NULL;

/* -finaljson destination: "" = disabled, "-" = stdout, else a file path.
 * When set, a single global game-state snapshot is written once the game
 * reaches a terminal game-over (as -ticks produces). See serverEmitFinalJson. */
static char optFinalJson[512] = "";

/* -snapjson / -snapinterval: periodic JSONL time series of the same global
 * snapshot -finaljson writes once. optSnapJson is the destination ("" =
 * disabled, "-" = stdout, else a file path opened in append mode so each
 * snapshot is one line); optSnapInterval is the period in running ticks
 * (0 = disabled). Both must be set for anything to be emitted. */
static char optSnapJson[512] = "";
static int32_t optSnapInterval = 0;

/* Shutdown handshake for the game-tick timer.
 *
 * serverGameTimer runs on a separate thread (Win32 multimedia timer or the
 * SDL timer thread). Neither timeKillEvent nor SDL_RemoveTimer joins an
 * in-flight callback, so without this a tick can still be running
 * serverInstanceTick -> serverSimBotTick -> brain think on bot lua_States
 * while the main thread frees them in serverInstanceShutdown — the
 * production shutdown use-after-free (SIGSEGV in luaH_getint).
 *
 * g_serverShuttingDown is raised first so the callback bails (and its
 * catch-up loop stops re-entering ticks); g_serverTickLock is held for the
 * whole callback body, so the shutdown path can acquire it to block until
 * any in-flight tick has fully drained. See serverQuiesceGameTimer. */
static SDL_AtomicInt g_serverShuttingDown;
static SDL_Mutex    *g_serverTickLock = NULL;

/* Tracker settings (set from command-line args, read by timer) */
static char  sTrackerAddr[FILENAME_MAX] = "";
static unsigned short sTrackerPort = 0;
static bool  sTrackerUse = FALSE;

#define WIND_CLASSNAME "WinBoloServ"
#define WIND_TITLE "WinBoloServ"

/* There are 60 seconds in a minute */
#define NUM_SECONDS 60 


typedef enum {
  alarmNone,
  alarmInterrupt,
  alarmLock,
  alarmUnlock
} alarmType;


alarmType alarmRaised;

#ifdef _WIN32
/* Signal handler for Windows console */
BOOL WINAPI consoleCtrlHandler(DWORD ctrlType) {
  switch (ctrlType) {
  case CTRL_C_EVENT:
  case CTRL_BREAK_EVENT:
  case CTRL_CLOSE_EVENT:
    alarmRaised = alarmInterrupt;
    return TRUE;
  default:
    return FALSE;
  }
}
#else
/* Signal handler for linux */
void
catch_alarm (int sig)
{
  switch (sig) {
  case SIGINT:
    alarmRaised = alarmInterrupt;
    break;
  case SIGUSR1:
    alarmRaised = alarmLock;
    break;
  case SIGUSR2:
    alarmRaised = alarmUnlock;
    break;
  }
}
#endif


/* The console command set itself lives in server_console.c, which parses a
 * line and calls back through ServerConsoleOps for anything that touches
 * the server. These are those callbacks: each takes the sim mutex for the
 * whole of its work, exactly as the command bodies did when they were
 * inline here. */

static void consoleOpSetLock(bool locked) {
  threadsWaitForMutex();
  transportUdpServerSetLock(serverSim, locked);
  threadsReleaseMutex();
}

static void consoleOpInfo(void) {
  threadsWaitForMutex();
  serverSimInformation(serverSim, transportUdpServerGetLock());
  threadsReleaseMutex();
}

static bool consoleOpSaveMap(const char *path) {
  bool saved;

  threadsWaitForMutex();
  transportUdpServerSendServerMessage("Server Admin saved map file.");
  saved = serverSimSaveMap(serverSim, (char *) path);
  if (saved) {
    logAddEvent(log_SaveMap, 0, 0, 0, 0, 0, NULL);
  }
  threadsReleaseMutex();
  return saved;
}

static void consoleOpSay(const char *text) {
  transportUdpServerSendServerMessage(text);
}

static void consoleOpLogSay(const char *pstr) {
  logAddEvent(log_MessageServer, 0, 0, 0, 0, 0, (char *) pstr);
}

static void consoleOpStatus(void) {
  transportUdpServerPrintStatus(statusFile);
}

static void consoleOpKick(const char *name) {
  threadsWaitForMutex();
  transportUdpServerKickPlayer(serverSim, name);
  threadsReleaseMutex();
}

static bool consoleOpSetHost(const char *name) {
  bool hostSet;

  threadsWaitForMutex();
  hostSet = transportUdpServerSetHostByName(serverSim, name);
  threadsReleaseMutex();
  return hostSet;
}

static bool consoleOpReloadScenario(char *msg, size_t msgLen) {
  char err[512];
  bool ok;

  if (scenarioHost == NULL) {
    snprintf(msg, msgLen, "No scenario is attached to this map");
    return false;
  }

  /* Under the mutex like every other console command: the bytes this
     replaces are what the round-start callback reads. */
  threadsWaitForMutex();
  ok = scenarioHostReload(scenarioHost, err, sizeof(err));
  threadsReleaseMutex();

  if (!ok) {
    snprintf(msg, msgLen, "%s", err);
    return false;
  }
  snprintf(msg, msgLen,
           "Scenario '%s': %s re-read. The new settings take effect at the "
           "next round; the round in progress keeps the ones it started with.",
           scenarioHostName(scenarioHost),
           scenarioHostScriptPath(scenarioHost));
  return true;
}

/* Positional, and the struct's own comment says why the newest entry goes
   last rather than beside a relative. */
static const ServerConsoleOps serverConsoleOps = {
  consoleOpSetLock,
  consoleOpInfo,
  consoleOpSaveMap,
  consoleOpSay,
  consoleOpLogSay,
  consoleOpStatus,
  consoleOpKick,
  consoleOpSetHost,
  consoleOpReloadScenario
};


#ifdef _WIN32

/* Background thread that reads lines from stdin so the main loop never blocks */
static volatile int stdinLineReady = 0;
static char stdinLine[256];

static DWORD WINAPI stdinReaderThread(LPVOID param) {
	(void)param;
	while (1) {
		if (fgets(stdinLine, sizeof(stdinLine), stdin) != NULL) {
			stdinLineReady = 1;
			/* Wait for main thread to consume the line */
			while (stdinLineReady) {
				Sleep(50);
			}
		} else {
			break;
		}
	}
	return 0;
}

void processKeys(bool isQuiet) {
	char keyBuff[256] = "\0";
	char saveBuff[256] = "\0";

	if (isQuiet == TRUE || isNoInput == TRUE) {
		while (!serverSimIsTerminalGameOver(serverSim)) {
			if (alarmRaised == alarmInterrupt) {
				break;
			}
			/* Under -asap the whole game can finish inside one of these
			 * sleeps, so poll fast enough not to add a second to the run. */
			Sleep(isAsap ? 1 : 1000);
		}
	} else {
		/* Start background thread to read stdin */
		HANDLE hThread = CreateThread(NULL, 0, stdinReaderThread, NULL, 0, NULL);
		if (hThread) {
			CloseHandle(hThread);
		}

		while (strncmp(keyBuff, "quit", 4) != 0 && !serverSimIsTerminalGameOver(serverSim)) {
			if (alarmRaised == alarmInterrupt) {
				strcpy(keyBuff, "quit");
				continue;
			}
			/* Check if the reader thread has a line ready */
			if (stdinLineReady) {
				strcpy(keyBuff, stdinLine);
				strcpy(saveBuff, stdinLine);
				strlower(keyBuff);
				stdinLineReady = 0;

				serverConsoleDispatch(&serverConsoleOps, keyBuff, saveBuff);
			} else {
				Sleep(isAsap ? 1 : 100);
			}
		}

		closeDebugFile();
	}
}

#else
/* Linux */
void processKeys(bool isQuiet) {
  char keyBuff[SERVER_CONSOLE_LINE] = "\0";
  char saveBuff[SERVER_CONSOLE_LINE] = "\0";
  bool consoleOpen = TRUE;

  if (isQuiet == TRUE || isNoInput == TRUE) {
    while (!serverSimIsTerminalGameOver(serverSim)) {
      if (alarmRaised == alarmInterrupt) {
        break;
      } else if (alarmRaised == alarmLock) {
        threadsWaitForMutex();
        transportUdpServerSetLock(serverSim, TRUE);
        threadsReleaseMutex();
        alarmRaised = alarmNone;
      } else if (alarmRaised == alarmUnlock) {
        threadsWaitForMutex();
        transportUdpServerSetLock(serverSim, FALSE);
        threadsReleaseMutex();
        alarmRaised = alarmNone;
      }
      sleep(1);
    }
  } else {
    while (strncmp(keyBuff, "quit", 4) != 0 && !serverSimIsTerminalGameOver(serverSim)) {
      serverConsoleDispatch(&serverConsoleOps, keyBuff, saveBuff);

      if (consoleOpen == TRUE) {
        switch (serverConsoleReadLine(stdin, keyBuff, sizeof(keyBuff), 1)) {
        case SERVER_CONSOLE_READ_LINE:
          strcpy(saveBuff, keyBuff);
          strlower(keyBuff);
          break;
        case SERVER_CONSOLE_READ_EOF:
          /* stdin has ended — a server put in the background without
           * -noinput, or a closed pipe. select() then reports the
           * descriptor readable for ever and the read fails without
           * touching the buffer, so the loop used to re-run whatever
           * command it read last, flat out and with no delay. Stop
           * reading and idle the way the -noinput path does. */
          fprintf(stderr, "Console input has closed - no further commands "
                          "will be read. Use quit or Ctrl-C to stop the "
                          "server.\n");
          consoleOpen = FALSE;
          saveBuff[0] = '\0';
          break;
        case SERVER_CONSOLE_READ_TIMEOUT:
          saveBuff[0] = '\0';
          break;
        }
      } else {
        /* Console closed: idle a second at a time, the way the read's
         * timeout does, and clear the line so a command left behind by a
         * signal below runs once and not once a second. */
        sleep(1);
        keyBuff[0] = '\0';
        saveBuff[0] = '\0';
      }

      if (alarmRaised == alarmInterrupt) {
        strcpy(keyBuff, "quit");
      } else if (alarmRaised == alarmLock) {
        strcpy(keyBuff, "lock");
        alarmRaised = alarmNone;
      } else if (alarmRaised == alarmUnlock) {
        strcpy(keyBuff, "unlock");
        alarmRaised = alarmNone;
      }
    }
  }
}

#endif

/*********************************************************
 * Scripted command dispatch (-cmd-stdin).
 *
 * Reads JSON-per-line commands from FILE and dispatches at
 * each command's tick (server-side serverSimGetTick). Used by
 * the centralize test harness to drive server-originated
 * events deterministically: start_game, reapply_alliances,
 * shutdown, exit. Client-originated ops (add_bot, set_team,
 * etc.) error out — those belong on WinBoloHeadless --cmd-stdin.
 *
 * Replaces processKeys when -cmd-stdin is supplied; the two
 * are mutually exclusive (so the stdin-reader thread on
 * Windows and the select() on Linux are not contested).
 *********************************************************/
static void processCmdStdin(CmdStdin *cs) {
    while (1) {
        if (alarmRaised == alarmInterrupt) break;
        if (serverSimIsTerminalGameOver(serverSim)) {
            break;
        }

        CmdLine cmd;
        if (!cmdStdinPeek(cs, &cmd)) {
            /* EOF — keep the server running until SIGINT or game-over
             * matches the processKeys quiet path. The scenario fixture
             * is expected to supply an explicit exit/shutdown op once
             * its goldens have been written. */
#ifdef _WIN32
            Sleep(isAsap ? 1 : 50);
#else
            SDL_Delay(isAsap ? 1 : 50);
#endif
            continue;
        }

        uint32_t serverTick = serverSimGetTick(serverSim);
        if (cmd.tick > serverTick) {
#ifdef _WIN32
            Sleep(isAsap ? 0 : 10);
#else
            SDL_Delay(isAsap ? 0 : 10);
#endif
            continue;
        }

        cmdStdinConsume(cs);
        bool keepGoing = true;
        threadsWaitForMutex();
        switch (cmd.op) {
            case CMD_OP_START_GAME:
                serverSimStartGame(serverSim);
                break;
            case CMD_OP_REAPPLY_ALLIANCES:
                serverSimReapplyTeamAlliances(serverSim);
                break;
            case CMD_OP_SHUTDOWN:
            case CMD_OP_EXIT:
                /* Both paths break the main loop. The cleanup code
                 * in main() runs serverInstanceShutdown which, via
                 * transportUdpServerStop, publishes
                 * CTRL_SERVER_SHUTDOWN to connected clients. */
                keepGoing = false;
                break;
            default:
                fprintf(stderr,
                        "cmd-stdin: line %d: op '%s' not valid in WinBoloDS mode\n",
                        cmd.lineNumber, cmdOpName(cmd.op));
                threadsReleaseMutex();
                exit(2);
        }
        threadsReleaseMutex();
        if (!keepGoing) break;
    }
}

/* One owed tick for serverTickCatchUp. Answering the shutdown flag here is
 * what used to be the `if (...) break;` at the top of the catch-up loop, so
 * g_serverShuttingDown stays private to this file. */
static bool serverTickStep(void *ctx) {
  (void)ctx;
  if (SDL_GetAtomicInt(&g_serverShuttingDown)) {
    return false;
  }
  serverInstanceTick(serverSim);
  return true;
}

/*********************************************************
*NAME:          serverGameTimer
*AUTHOR:        John Morrison
*CREATION DATE: 24/11/98
*LAST MODIFIED: 20/3/99
*PURPOSE:
* The Game Timer. If there are no events to prcess this
* routine is called. If the elapsed
*
*ARGUMENTS:
*
*********************************************************/
#ifdef _WIN32
void CALLBACK serverGameTimer(UINT uID, UINT uMsg, DWORD_PTR dwUser, DWORD_PTR dw1, DWORD_PTR dw2) {
  DWORD tick;
  tick = SDL_GetTicks();
#else
  Uint32 SDLCALL serverGameTimer(void *userdata, SDL_TimerID timerID, Uint32 interval) {
  (void)userdata; (void)timerID;
  DWORD tick;
  tick = SDL_GetTicks();
#endif

  /* Shutdown handshake: bail before touching the sim once teardown has
   * begun, and hold g_serverTickLock for the whole tick body so the
   * shutdown path can block on it until an in-flight tick fully drains.
   * The flag is re-checked after acquiring the lock (and inside the
   * catch-up loop) to cover the case where shutdown raced in between. */
  if (!SDL_GetAtomicInt(&g_serverShuttingDown) && g_serverTickLock != NULL) {
    SDL_LockMutex(g_serverTickLock);
    if (!SDL_GetAtomicInt(&g_serverShuttingDown) &&
        (tick - oldTick) > SERVER_TICK_LENGTH) {
      /* `ticks` is a time_t, so the burst counts into a uint32_t of its own
       * and is added on afterwards. Nothing reads the global mid-burst. */
      uint32_t ran = 0;
      (void)serverTickCatchUp(tick, &oldTick, &ran, serverTickStep, NULL);
      ticks += (time_t)ran;
    }
    SDL_UnlockMutex(g_serverTickLock);
  }
#ifdef USING_SDL
  return interval;
#endif
}

/* -asap tick driver: the serverGameTimer body with the elapsed-time gate
 * removed, on a thread of its own instead of the timer's. Same lock, same
 * shutdown handshake, same `ticks` counter -- so -ticks, -snapinterval and
 * -finaljson count exactly what they counted under the timer. */
static int SDLCALL serverAsapLoop(void *unused) {
  (void)unused;
  while (!SDL_GetAtomicInt(&g_serverShuttingDown) && g_serverTickLock != NULL) {
    SDL_LockMutex(g_serverTickLock);
    if (SDL_GetAtomicInt(&g_serverShuttingDown)) {
      SDL_UnlockMutex(g_serverTickLock);
      break;
    }
    serverInstanceTick(serverSim);
    ticks++;
    SDL_UnlockMutex(g_serverTickLock);
    /* The command loops (processKeys / processCmdStdin) and the shutdown
     * path both want g_serverTickLock or the global mutex; yield between
     * ticks so a "quit" is not starved on a single-core box. */
    SDL_Delay(0);
  }
  return 0;
}
static SDL_Thread *serverAsapThread = NULL;

/* Stop the game-tick timer and guarantee no tick callback is — or will be —
 * executing before the caller frees the sim / bot lua_States. timeKillEvent
 * and SDL_RemoveTimer only unschedule future callbacks; they do not join an
 * invocation already running on the timer thread. So we raise the shutdown
 * flag (serverGameTimer then bails and its catch-up loop stops re-entering
 * ticks), unschedule the timer, then take and release g_serverTickLock —
 * which the callback holds across its whole body — to block until any
 * in-flight tick has drained. Idempotent and safe to call once per exit
 * path. */
static void serverQuiesceGameTimer(void) {
  SDL_SetAtomicInt(&g_serverShuttingDown, 1);
  if (isAsap) {
    /* No timer was armed; join the tick thread instead. SDL_WaitThread is a
     * no-op on NULL, and the flag above makes the loop exit at its next
     * iteration boundary. */
    SDL_WaitThread(serverAsapThread, NULL);
    serverAsapThread = NULL;
  } else {
#ifdef _WIN32
    timeKillEvent(serverTimerGameID);
#else
    SDL_RemoveTimer(serverTimerGameID);
#endif
  }
  if (g_serverTickLock != NULL) {
    SDL_LockMutex(g_serverTickLock);
    SDL_UnlockMutex(g_serverTickLock);
  }
}

#define DEFAULT_TRACKER_ADDR "tracker.winbolo.com"
#define DEFAULT_TRACKER_PORT 50000

void printArgs() {
#ifdef _WIN32
  fprintf(stderr, "Usage:\nWinBoloDS -map <Filename> -port <Port> -gametype <GameType> [options]\n\n");
#else
  fprintf(stderr, "Usage:\nLinBoloDS -map <Filename> -port <Port> -gametype <GameType> [options]\n\n");
#endif
  fprintf(stderr, "Map selection:\n");
  fprintf(stderr, "-map <File>   - Path and file name of the map file to open (-inbuilt can be\n");
  fprintf(stderr, "                used instead of -map to enable inbuilt map Everard Island)\n");
  fprintf(stderr, "-mapdir <Dir> - Directory of .map files for random rotation between rounds.\n");
  fprintf(stderr, "                Can be used with -map (initial map) or alone (random first map).\n");
  fprintf(stderr, "                Requires lobby mode. Invalid maps are skipped at startup.\n");
  fprintf(stderr, "-randommap    - Generate a random procedural map instead of loading a file.\n");
  fprintf(stderr, "                -randommap alone generates a fully random map each round.\n");
  fprintf(stderr, "                -randommap tournament|natural|maze|fractal — specific generator type.\n");
  fprintf(stderr, "                -randommap <seed> — reproduce a specific map from its seed.\n");
  fprintf(stderr, "                -randommap tournament <seed> — type with specific seed.\n");
  fprintf(stderr, "                Map name shown as 'rand_<seed>' in server info.\n");
  fprintf(stderr, "-moddir <Dir> - Directory of mods and scenarios this server offers on their\n");
  fprintf(stderr, "                own, independently of any map: .scenario packages and loose\n");
  fprintf(stderr, "                .lua scripts. Read on top of the mods that ship with the\n");
  fprintf(stderr, "                build, which are always offered. A directory that is not\n");
  fprintf(stderr, "                there means the server offers none of its own, which is not\n");
  fprintf(stderr, "                an error. -scenariodir is the old name for this argument.\n");
  fprintf(stderr, "-noscenarios  - Do not load the scenario script beside a map. Every map,\n");
  fprintf(stderr, "                including one committed later, plays plainly. A map that\n");
  fprintf(stderr, "                has a script says which one was not loaded.\n");
  fprintf(stderr, "-setting [<File>:]<id>=<value> - Choose a value for one of a scenario\n");
  fprintf(stderr, "                script's own settings, as the lobby host would. File is the\n");
  fprintf(stderr, "                script's file name and defaults to the map's own script.\n");
  fprintf(stderr, "                Value is a number, true or false for a bool setting, or one\n");
  fprintf(stderr, "                of the words of a choice setting (quote words with spaces).\n");
  fprintf(stderr, "                May be given more than once.\n");
  fprintf(stderr, "-nouploadscripts - The old spelling of -scriptuploads off (see Script\n");
  fprintf(stderr, "                uploads below). -scriptuploads wins when both are given.\n");
  fprintf(stderr, "-allow-unsafe-scripts - Run scenario scripts with the full Lua standard\n");
  fprintf(stderr, "                library, no memory cap, no time limits and precompiled chunks\n");
  fprintf(stderr, "                accepted. Reaches uploaded maps' scripts and -validate too;\n");
  fprintf(stderr, "                -scriptuploads off still refuses uploads. A script a player\n");
  fprintf(stderr, "                sends runs its top level the moment it lands, before any host\n");
  fprintf(stderr, "                picks it. Only for trusted content.\n");
  fprintf(stderr, "-validate <File> - Check the scenario script beside a map and exit without\n");
  fprintf(stderr, "                starting a server. Each problem is printed as\n");
  fprintf(stderr, "                file:line: key: message. Exits 0 when the map is\n");
  fprintf(stderr, "                playable, 1 when it is not.\n");
  fprintf(stderr, "-pack <File>  - Write the scenario script beside a map into the map file\n");
  fprintf(stderr, "                itself and exit without starting a server. The manifest\n");
  fprintf(stderr, "                comes from the script's own scenario table, and a\n");
  fprintf(stderr, "                container already on the map is replaced. A script with\n");
  fprintf(stderr, "                problems against it is not packed. Exits 0 when the map\n");
  fprintf(stderr, "                was packed, 1 when it was not.\n");

  fprintf(stderr, "\nGame rules:\n");
  fprintf(stderr, "-gametype <T> - Specifies the game type: \"Open\" or \"Tournament\" or \"Strict\"\n");
  fprintf(stderr, "-mines <M>    - Specifies allowing hidden mines: \"yes\" for allow,\n");
  fprintf(stderr, "                \"no\" for disallow (on if not specified)\n" );
  fprintf(stderr, "-ai <AiType>  - Specifies allowing brains. Valid values are \"no\" for\n");
  fprintf(stderr, "                disallowing, \"yes\" for allowing, \"yesAdv\" for giving\n");
  fprintf(stderr, "                them an advantage, or \"yesFull\" for full map advantage.\n");
  fprintf(stderr, "                (disallowed if not specified)\n");
  fprintf(stderr, "-delay <D>    - Specifies the start delay (in seconds) (none if not specified)\n");
  fprintf(stderr, "-limit <L>    - Specifies the game time limit (in minutes)\n");
  fprintf(stderr, "                \"-1\" for no time limit (none if not specified)\n");
  fprintf(stderr, "-password <P> - Game Password (none if not specified)\n");

  fprintf(stderr, "\nLobby & host:\n");
  fprintf(stderr, "-nolobby      - Skip lobby, start game immediately (backward-compatible mode)\n");
  fprintf(stderr, "-autolock     - Start with auto-lock-on-game-start enabled\n");
  fprintf(stderr, "-ranked       - Start with the lobby flagged Ranked (also forces auto-lock-on-game-start)\n");
  fprintf(stderr, "-openhost     - Start with Open Host on so any connected player can edit lobby settings\n");
  fprintf(stderr, "-firstjoinhost- On an empty dedicated server, promote the next joiner to host;\n");
  fprintf(stderr, "                slot re-opens when the host leaves\n");
  fprintf(stderr, "-lock <list>  - Comma-separated list of lobby settings to lock as read-only.\n");
  fprintf(stderr, "                Valid: gametype, ai, mines, timelimit (alias: limit),\n");
  fprintf(stderr, "                autolock, password, ranked, openhost, map, pillview,\n");
  fprintf(stderr, "                baseview, allyview, classicmode, alliesintrees,\n");
  fprintf(stderr, "                overviewwindow, lineofsight, smartpings,\n");
  fprintf(stderr, "                positionalsound.\n");
  fprintf(stderr, "                Locking pillview, baseview, allyview, alliesintrees,\n");
  fprintf(stderr, "                overviewwindow, lineofsight or positionalsound also\n");
  fprintf(stderr, "                locks classicmode, which writes those values.\n");
  fprintf(stderr, "                e.g. -lock gametype,ranked,map\n");
  fprintf(stderr, "-maxplayers <N> - Specifies the maximum number of players that can be on this\n");
  fprintf(stderr, "                server.\n");
  fprintf(stderr, "-maxspectators <N> - Maximum number of spectator connections (default 16,\n");
  fprintf(stderr, "                0 disables spectating).\n");
  fprintf(stderr, "-specdelay <S> - Spectator view delay in seconds (default 90, 0 = live).\n");

  fprintf(stderr, "\nVisibility (what players see of pills, bases and allied tanks):\n");
  fprintf(stderr, "-pillview <M> - Pillbox visibility: always, key (default), decay, off\n");
  fprintf(stderr, "-baseview <M> - Base visibility: always, key, decay, off (default off)\n");
  fprintf(stderr, "-allyview <M> - Allied tank visibility: always, key, decay, off (default off)\n");
  fprintf(stderr, "-pillviewdecay <S> - Seconds a pill stays visible under \"decay\"\n");
  fprintf(stderr, "                (5-600, default 30)\n");
  fprintf(stderr, "-baseviewdecay <S> - Same for bases (5-600, default 30)\n");
  fprintf(stderr, "-allyviewdecay <S> - Same for allied tanks (5-600, default 30)\n");
  fprintf(stderr, "                An unrecognised mode warns and falls back to that\n");
  fprintf(stderr, "                switch's default; a decay outside the range is\n");
  fprintf(stderr, "                clamped into it.\n");
  fprintf(stderr, "-alliesintrees- Allied tanks standing in trees are sent to their allies\n");
  fprintf(stderr, "                instead of being withheld (fog of war still applies).\n");
  fprintf(stderr, "                Off by default, and off under -classicmode.\n");
  fprintf(stderr, "-overviewwindow <M> - Map overview live block: expanded, classic (default), none\n");
  fprintf(stderr, "-lineofsight  - Buildings and stands of trees block sight inside the live\n");
  fprintf(stderr, "                block. Off by default, and off under -classicmode.\n");
  fprintf(stderr, "-positionalsound- Sounds tell each player which side they are on and\n");
  fprintf(stderr, "                roughly how far. Off by default (every sound centred),\n");
  fprintf(stderr, "                and off under -classicmode.\n");
  fprintf(stderr, "-smartpingsoff- Refuse smart pings for the whole server. Off by default,\n");
  fprintf(stderr, "                i.e. pings are allowed. Not part of -classicmode.\n");
  fprintf(stderr, "-classicmode  - Classic Bolo view: sets pillview key, baseview off and\n");
  fprintf(stderr, "                allyview off, overriding those three switches, turns\n");
  fprintf(stderr, "                allies in trees off, sets the overview window to classic\n");
  fprintf(stderr, "                with line of sight off, turns positional sound off,\n");
  fprintf(stderr, "                and stops the lobby changing them.\n");

  fprintf(stderr, "\nMap uploads (client-pushed maps in the lobby):\n");
  fprintf(stderr, "-uploadpolicy <P> - Client map-upload handling: \"off\" refuses uploads,\n");
  fprintf(stderr, "                \"allow\" plays the upload in memory and drops it on the next\n");
  fprintf(stderr, "                map change (default), \"persist\" also saves it to\n");
  fprintf(stderr, "                <map root>/Uploads/, or the -uploaddir directory.\n");
  fprintf(stderr, "-uploaddir <Dir> - Directory persisted maps are written to, used with\n");
  fprintf(stderr, "                -uploadpolicy persist (default <map root>/Uploads).\n");
  fprintf(stderr, "-uploadmaxfiles <N> - Max stored upload files in persist mode (1-255,\n");
  fprintf(stderr, "                default 64).\n");
  fprintf(stderr, "-uploadmaxstorage <MB> - Max upload storage in persist mode (1-4095 MB,\n");
  fprintf(stderr, "                default 8).\n");

  fprintf(stderr, "\nScript uploads (scripts players send in the lobby):\n");
  fprintf(stderr, "-scriptuploads <P> - Player script handling: \"off\" refuses script uploads\n");
  fprintf(stderr, "                and does not run a script carried by a map a client\n");
  fprintf(stderr, "                uploaded (those maps play plainly, and each script turned\n");
  fprintf(stderr, "                down is named), \"allow\" keeps them for the session\n");
  fprintf(stderr, "                (default), \"persist\" keeps them for good.\n");
  fprintf(stderr, "-scriptuploaddir <Dir> - Directory persisted scripts are written to.\n");
  fprintf(stderr, "-scriptuploadmaxfiles <N> - Max stored script files in persist mode\n");
  fprintf(stderr, "                (1-255, default 32).\n");
  fprintf(stderr, "-scriptuploadmaxstorage <MB> - Max script storage in persist mode\n");
  fprintf(stderr, "                (1-4095 MB, default 64).\n");
  fprintf(stderr, "-noscriptsharing - Refuse players' requests for a copy of this server's\n");
  fprintf(stderr, "                mods and scenarios. Sharing is on by default.\n");

  fprintf(stderr, "\nBots & AI:\n");
  fprintf(stderr, "-bots <N>     - Number of AI bot players to add (default: 0)\n");
  fprintf(stderr, "-maxbots <N>  - Maximum number of AI bots that can be in the lobby\n");
  fprintf(stderr, "                (default: 0 = no limit). Caps lobby \"Add Bot\" requests\n");
  fprintf(stderr, "                and clamps -bots.\n");
  fprintf(stderr, "-brain <path> - Path to the Lua brain script for bots (default: the\n");
  fprintf(stderr, "                first of Brains/GoalHunter_1.7/init.lua,\n");
  fprintf(stderr, "                brains/GoalHunter_1.7/init.lua,\n");
  fprintf(stderr, "                data/Brains/GoalHunter_1.7/init.lua that exists)\n");
  fprintf(stderr, "-mode <key> - Mode for the -bots bots, one of the mode keys in the\n");
  fprintf(stderr, "                brain's modes.txt (default 'default'). Handed to the\n");
  fprintf(stderr, "                brain as a 'mode=<key>' BRAIN_INIT_ARG token.\n");
  fprintf(stderr, "-difficulty <key> - Difficulty for the -bots bots: a level key from the\n");
  fprintf(stderr, "                selected mode, or easy|medium|hard (default: the mode's\n");
  fprintf(stderr, "                own default, which is hard). Handed to the brain as a\n");
  fprintf(stderr, "                'difficulty=<key>' BRAIN_INIT_ARG token; every setting\n");
  fprintf(stderr, "                plays the same way for now.\n");
  fprintf(stderr, "-bot-init <spec> - Per-bot brain paths by player id: 'range=path[arg],...'\n");
  fprintf(stderr, "                where range is 'a-b' or 'n' and the optional [arg] becomes\n");
  fprintf(stderr, "                that bot's BRAIN_INIT Lua table: ';'-separated key=value\n");
  fprintf(stderr, "                pairs, a bare word being the value '1'. Ids not listed use\n");
  fprintf(stderr, "                -brain. E.g. -bot-init 0-3=brains/A/init.lua,4=brains/B/init.lua[llm]\n");
  fprintf(stderr, "-botnames <path> - JSON file of bot name pools (themed name lists) for\n");
  fprintf(stderr, "                naming auto-added bots. Defaults to data/bot_names.json.\n");
  fprintf(stderr, "-allybots [N] - Place all -bots on the same team (1-16, default 1) so\n");
  fprintf(stderr, "                they start allied. Pick the same team in the lobby to join\n");
  fprintf(stderr, "                them, or a different one to fight against them.\n");
  fprintf(stderr, "-teams <spec> - Split -bots into teams. 'N' = round-robin into N teams;\n");
  fprintf(stderr, "                'a,b,c' = contiguous blocks of those sizes (first a bots ->\n");
  fprintf(stderr, "                team 1, next b -> team 2, ...). Overrides -allybots.\n");
  fprintf(stderr, "-threads <N>  - Total concurrent bot-think runners including the main\n");
  fprintf(stderr, "                thread. 1 disables the worker pool. Default: logical cores.\n");
  fprintf(stderr, "-brain-debug  - Enable BRAIN_DEBUG_MODE for bots: per-bot print2_bot<N>.log\n");
  fprintf(stderr, "                (grep MSG_TX / SYNC_P6 to audit bot comms). Use a base -brain\n");
  fprintf(stderr, "                path (not opt/) so print2 calls aren't stripped. Implies\n");
  fprintf(stderr, "                -allow-unsafe-brains (needs file writes into debug_sessions/).\n");
  fprintf(stderr, "                Implies ALL debug streams; turn individual ones off with:\n");
  fprintf(stderr, "  -bd-noviz    - don't emit/record visualizer overlays (brainrec.btr stays\n");
  fprintf(stderr, "                 loadable, just without viz frames)\n");
  fprintf(stderr, "  -bd-nopool   - skip the pool-breakdown JSON capture entirely\n");
  fprintf(stderr, "  -bd-pool-rr  - round-robin pool capture (ONE bot per tick) instead of the\n");
  fprintf(stderr, "                 default every-bot-every-tick. Same 50 Hz either way (the JSON\n");
  fprintf(stderr, "                 builds run on the bot workers); rr records ~1/botCount the\n");
  fprintf(stderr, "                 bytes (~40 KB vs ~0.5 MB per tick at 12 bots) when session\n");
  fprintf(stderr, "                 size matters more than exact per-tick panel history.\n");
  fprintf(stderr, "  -bd-noprint2 - no print2_bot<N>.log lines\n");
  fprintf(stderr, "  -bd-nojsonl  - no brain_p<N>.jsonl / player<N>.jsonl behavior traces\n");
  fprintf(stderr, "                A braindbg_perf.log (per-250-tick phase costs + achieved Hz)\n");
  fprintf(stderr, "                is always written to the session dir for diagnosis.\n");
  fprintf(stderr, "-brain-profile-log - Profile the PRODUCTION (opt/) brain: BRAIN_PROFILE on, writes\n");
  fprintf(stderr, "                optimize.log + performance.ticks.log into debug_sessions/<TS>_<N>/\n");
  fprintf(stderr, "                alongside brainrec.btr (loadable in BrainTest). Implies recording\n");
  fprintf(stderr, "                and -allow-unsafe-brains (needs file writes into debug_sessions/);\n");
  fprintf(stderr, "                forces the opt/ brain with debug OFF for representative timings.\n");
  fprintf(stderr, "                Profile data ONLY: no print2 debug logs, no pool-viz capture\n");
  fprintf(stderr, "                (independent of -brain-debug).\n");
  fprintf(stderr, "-allow-unsafe-brains - Open the full Lua standard library for bot brains.\n");
  fprintf(stderr, "                Default OFF: brains are sandboxed (no shell/process/native code,\n");
  fprintf(stderr, "                file access confined to the brain directory). Only for trusted brains.\n");

  fprintf(stderr, "\nNetworking:\n");
  fprintf(stderr, "-port <Port>  - Port to run the server on\n");
  fprintf(stderr, "-addr         - Specify a different address to use if avaliable\n");
  fprintf(stderr, "-tracker      - Internet tracker to notify. On by default (%s:%d).\n", DEFAULT_TRACKER_ADDR, DEFAULT_TRACKER_PORT);
  fprintf(stderr, "                -tracker <host> uses <host> with default port %d\n", DEFAULT_TRACKER_PORT);
  fprintf(stderr, "                -tracker <host:port> uses the specified host and port\n");
  fprintf(stderr, "-notracker    - do not notify any Internet tracker\n");
  fprintf(stderr, "-upnp         - request automatic UPnP/NAT-PMP port mapping\n");
  fprintf(stderr, "-no-natpunch  - disable hole-punch keepalive (on by default with tracker)\n");
  fprintf(stderr, "-wbnhost      - WinBolo.net host to connect to (overrides preferences file).\n");
  fprintf(stderr, "                Bare hostname uses https (e.g. -wbnhost wbn.winbolo.net),\n");
  fprintf(stderr, "                or specify scheme (e.g. -wbnhost http://wbn.winbolo.net)\n");
  fprintf(stderr, "-nowinbolonet - Do not participate in winbolo.net game tracking\n");
  fprintf(stderr, "-mdns         - Advertise the game on the local network via mDNS\n");
  fprintf(stderr, "                (_winbolo._udp.local); off by default for dedicated servers\n");
  fprintf(stderr, "-voice <M>    - Voice chat forwarding: on (default), off, proximity.\n");
  fprintf(stderr, "                \"off\" drops the segments a client sends anyway rather\n");
  fprintf(stderr, "                than carrying them to anyone. \"proximity\" is not\n");
  fprintf(stderr, "                implemented and forwards the same as \"on\".\n");
  fprintf(stderr, "                An unrecognised mode warns and falls back to on.\n");
  fprintf(stderr, "-no-voice     - Alias for -voice off. Wins when both are given.\n");

  fprintf(stderr, "\nLifecycle & shutdown:\n");
  fprintf(stderr, "-autoclose    - Automatically quit the server when all players have left\n");
  fprintf(stderr, "                the game\n");
  fprintf(stderr, "-quitonwin    - Quit server when a player/alliance wins\n");
  fprintf(stderr, "-maprotate    - No-lobby map rotation: on a win or when the server empties,\n");
  fprintf(stderr, "                boot all players, pick the next -mapdir map and restart a\n");
  fprintf(stderr, "                fresh round. Never auto-quits (Ctrl-C / quit only). Implies\n");
  fprintf(stderr, "                -nolobby and requires -mapdir.\n");
  fprintf(stderr, "-noemptyreset - Disable automatic lobby reset when server is empty\n");
  fprintf(stderr, "                (enabled by default, resets after 5 minutes)\n");
  fprintf(stderr, "-emptyresetmins <N> - Minutes before empty server resets to lobby (default: 5)\n");
  fprintf(stderr, "-ticks <N>    - Exit cleanly after N game-ticks of running play.\n");
  fprintf(stderr, "                \"0\" or omitted means unlimited (default).\n");
  fprintf(stderr, "-ticklimit <N> - End the current game (transition to GAME_OVER) after N\n");
  fprintf(stderr, "                game-ticks of running play. Unlike -ticks, the server is\n");
  fprintf(stderr, "                not asked to exit; in lobby mode the round returns to lobby.\n");
  fprintf(stderr, "-finaljson <F> - On terminal game-over (as -ticks produces), write a single\n");
  fprintf(stderr, "                JSON snapshot of the final global game state (all tanks,\n");
  fprintf(stderr, "                pillboxes, bases, winner). \"-\" writes to stdout, else a file.\n");
  fprintf(stderr, "-snapjson <F> - Append the same global snapshot periodically, one JSON object\n");
  fprintf(stderr, "                per line (JSONL), with \"reason\":\"snapshot\". Requires\n");
  fprintf(stderr, "                -snapinterval. The file is truncated at startup, and a final\n");
  fprintf(stderr, "                \"reason\":\"final\" line is appended on terminal game-over.\n");
  fprintf(stderr, "-snapinterval <N> - Emit a -snapjson snapshot every N game-ticks of running\n");
  fprintf(stderr, "                play (same tick units as -ticks). 0/omitted disables.\n");

  fprintf(stderr, "\nLogging & diagnostics:\n");
  fprintf(stderr, "-log [name]   - Create game log file. Optional [name] is a filename, or a\n");
  fprintf(stderr, "                directory (e.g. -log /tmp) to auto-name the log inside it.\n");
  fprintf(stderr, "-logfile      - Write all output to file instead of console.\n");
  fprintf(stderr, "-dontsendlog  - Don't upload game log to winbolo.net\n");
  fprintf(stderr, "-servelog <on|off> - Hand the last finished round's log to players who ask\n");
  fprintf(stderr, "                for it, so their post-game recap plays. Omitted, the server\n");
  fprintf(stderr, "                serves unless it is registered with winbolo.net. Needs -log.\n");
  fprintf(stderr, "-statusFile   - Save list of unlocked players to a file.\n");
  fprintf(stderr, "-seed <N>     - Seed the RNG with N (64-bit unsigned) for reproducible runs.\n");
  fprintf(stderr, "                Seeds the C sim stream only; see -brain-lua-seed for Lua.\n");
  fprintf(stderr, "-brain-tier <1..10> - Pin every brain's capacity tier instead of deriving it\n");
  fprintf(stderr, "                from think times. The tier normally comes from wall-clock\n");
  fprintf(stderr, "                measurements, so the same seed yields different tiers -- and\n");
  fprintf(stderr, "                a different tier is a different brain. Timing telemetry still\n");
  fprintf(stderr, "                reports real values.\n");
  fprintf(stderr, "-brain-lua-seed <N> - Seed each brain's Lua math.random with N + player\n");
  fprintf(stderr, "                number. Without it PUC-Lua auto-seeds per process and runs\n");
  fprintf(stderr, "                diverge from the first tick.\n");
  fprintf(stderr, "-brain-no-budget-kill - Give each brain a 1000 ms budget so the watchdog\n");
  fprintf(stderr, "                never truncates a think. Still finite, so a hung brain is\n");
  fprintf(stderr, "                still aborted. For measurement runs; pair with -threads 1.\n");
  fprintf(stderr, "                ALSO suppresses the consecutive-crash kick, so a brain that\n");
  fprintf(stderr, "                crashes every tick stays in the game instead of being\n");
  fprintf(stderr, "                removed. Wanted for measurement (a kick is a huge fork),\n");
  fprintf(stderr, "                surprising on a live server.\n");
  fprintf(stderr, "-asap         - Run game ticks back-to-back instead of one per 20 ms of\n");
  fprintf(stderr, "                wall clock. The simulation is unchanged (same ticks, same\n");
  fprintf(stderr, "                order, same -ticks/-snapinterval counting); it just stops\n");
  fprintf(stderr, "                waiting for real time between them, so a headless\n");
  fprintf(stderr, "                measurement or golden run finishes as fast as the CPU\n");
  fprintf(stderr, "                allows. Pointless with real clients connected.\n");
#if WB_ENABLE_NETIMPAIR
  fprintf(stderr, "-netimpair <spec> - Apply network impairment to both directions for testing.\n");
  fprintf(stderr, "                spec is comma-separated keys, e.g.\n");
  fprintf(stderr, "                -netimpair delay=75,jitter=30,loss=2,burst=2\n");
  fprintf(stderr, "                (delay/jitter in ms, loss in %%, burst = drops per loss event).\n");
  fprintf(stderr, "                Combine with -seed for a reproducible impaired run.\n");
#endif
  fprintf(stderr, "-quiet        - No screen input or output (silent mode)\n");
  fprintf(stderr, "-noinput      - No keyboard input\n");
}


void processTrackerArg(char *argItem, char *trackerAddr, unsigned short *trackerPort) {
  char *tmp;
  tmp = strtok(argItem, ":");
  if (tmp == NULL) {
    strncpy(trackerAddr, DEFAULT_TRACKER_ADDR, FILENAME_MAX - 1);
    trackerAddr[FILENAME_MAX - 1] = '\0';
    *trackerPort = DEFAULT_TRACKER_PORT;
    return;
  }
  strncpy(trackerAddr, tmp, FILENAME_MAX - 1);
  trackerAddr[FILENAME_MAX - 1] = '\0';
  tmp = strtok(NULL, ":");
  if (tmp == NULL) { *trackerPort = DEFAULT_TRACKER_PORT; return; }
  *trackerPort = (unsigned short)atoi(tmp);
}

#define ARG_NOT_FOUND -1

/* One -setting argument, "[file:]id=value", chosen on sim as the lobby
 * host's CMD_SET_SCRIPT_SETTING would choose it. defaultFile is the map's own
 * script, used when the argument names no file. The value is read against
 * the setting's declaration: a number for any type, true or false (or on or
 * off) for a bool, one of the words for a choice. Says on the console what
 * it did, or why it did nothing. */
static void serverApplySettingArg(ServerSim *sim, const char *arg,
                                  const char *defaultFile) {
  char              file[LOBBY_SCENARIO_FILE_LEN];
  char              id[SCN_SETTING_ID_LEN];
  char              line[512];
  const char       *eq;
  const char       *colon;
  const char       *word;
  uint8_t           blob[SCN_SETTINGS_BLOB_MAX];
  ScnSetting        rows[SCN_SETTINGS_MAX];
  const ScnSetting *decl = NULL;
  int               len;
  int               n = 0;
  int32_t           value = 0;
  int32_t           got = 0;
  char             *end = NULL;
  long              num;
  size_t            idLen;

  eq = strchr(arg, '=');
  if (eq == NULL) {
    snprintf(line, sizeof(line), "-setting %s: wanted [file:]id=value", arg);
    serverMessageConsoleMessage(sim, line);
    return;
  }
  colon = memchr(arg, ':', (size_t)(eq - arg));
  if (colon != NULL) {
    size_t fl = (size_t)(colon - arg);
    if (fl == 0 || fl >= sizeof(file)) {
      snprintf(line, sizeof(line), "-setting %s: bad file name", arg);
      serverMessageConsoleMessage(sim, line);
      return;
    }
    memcpy(file, arg, fl);
    file[fl] = '\0';
    arg = colon + 1;
  } else {
    if (defaultFile == NULL || defaultFile[0] == '\0') {
      snprintf(line, sizeof(line),
               "-setting %s: the map has no script, so name the file", arg);
      serverMessageConsoleMessage(sim, line);
      return;
    }
    snprintf(file, sizeof(file), "%s", defaultFile);
  }
  idLen = (size_t)(eq - arg);
  if (idLen == 0 || idLen >= sizeof(id)) {
    snprintf(line, sizeof(line), "-setting %s: bad setting id", arg);
    serverMessageConsoleMessage(sim, line);
    return;
  }
  memcpy(id, arg, idLen);
  id[idLen] = '\0';
  word = eq + 1;

  len = serverSimScenarioSettingsDecl(sim, file, blob, sizeof(blob));
  if (len > 0) {
    n = scnSettingsBlobRead(blob, (size_t)len, rows, SCN_SETTINGS_MAX);
  }
  if (n > 0) {
    decl = scnSettingFind(rows, n, id);
  }
  if (decl == NULL) {
    snprintf(line, sizeof(line), "-setting: %s declares no setting '%s'",
             file, id);
    serverMessageConsoleMessage(sim, line);
    return;
  }
  num = strtol(word, &end, 10);
  if (word[0] != '\0' && end != NULL && *end == '\0') {
    value = (int32_t)num;
  } else if (decl->type == SCN_SETTING_TYPE_CHOICE &&
             scnSettingChoiceIndex(decl, word) >= 0) {
    value = scnSettingChoiceIndex(decl, word);
  } else if (decl->type == SCN_SETTING_TYPE_BOOL &&
             (strcmp(word, "true") == 0 || strcmp(word, "on") == 0)) {
    value = 1;
  } else if (decl->type == SCN_SETTING_TYPE_BOOL &&
             (strcmp(word, "false") == 0 || strcmp(word, "off") == 0)) {
    value = 0;
  } else {
    snprintf(line, sizeof(line), "-setting: '%s' is not a value of %s:%s",
             word, file, id);
    serverMessageConsoleMessage(sim, line);
    return;
  }
  if (!serverSimSetScriptSetting(sim, file, id, value, &got)) {
    snprintf(line, sizeof(line), "-setting: %s:%s refused %d", file, id,
             (int)value);
    serverMessageConsoleMessage(sim, line);
    return;
  }
  if (decl->type == SCN_SETTING_TYPE_CHOICE) {
    snprintf(line, sizeof(line), "Setting %s:%s = %s", file, id,
             scnSettingChoiceText(decl, got));
  } else {
    snprintf(line, sizeof(line), "Setting %s:%s = %d", file, id, (int)got);
  }
  serverMessageConsoleMessage(sim, line);
}

int findArg(int numArgs, char **argv, const char *argname) {
  int returnValue; /* Value to return */
  char temp[255];
  int count;

  snprintf(temp, sizeof(temp), "-%s", argname);
  strlower(temp);
  returnValue = ARG_NOT_FOUND;
  count = 0;
  while (returnValue == -1 && count < numArgs) {
    if (strcmp((char *) argv[count], temp) == 0) {
      returnValue = count+1;
    }
    count++;
  }

  /* Make sure we don't fall off the end of the arguments */
  if (returnValue == numArgs) {
    returnValue = ARG_NOT_FOUND;
  }

  return returnValue;
}


bool argExist(int numArgs, char **argv, char *argname) {
  bool returnValue; /* Value to return */
  char temp[255];
  char argLower[255];
  int count;

  snprintf(temp, sizeof(temp), "-%s", argname);
  strlower(temp);
  returnValue = FALSE;
  count = 0;
  while (returnValue == FALSE && count < numArgs) {
    strncpy(argLower, (char *) argv[count], sizeof(argLower) - 1);
    argLower[sizeof(argLower) - 1] = '\0';
    strlower(argLower);
    if (strcmp(argLower, temp) == 0) {
      returnValue = TRUE;
    }
    count++;
  }


  return returnValue;
}

bool processArgs(int numArgs, char **argv, char *mapName, unsigned short *port, gameType *game, bool *hiddenMines, aiType *ai, int *srtDelay, int32_t *gmeLen, char *trackerAddr, unsigned short *trackerPort, bool *trackerUse, char *password) {
  bool returnValue; /* Value to return */
  int argNum;
  char temp[255];

  returnValue = TRUE;
  
  /* Map */
  argNum = findArg(numArgs, argv, "map");
  if (argNum != ARG_NOT_FOUND) {
    strncpy(mapName, (char *) argv[argNum], 2047);
    mapName[2047] = '\0';
  } else if (argExist(numArgs, argv, "inbuilt") == TRUE) {
    strcpy(mapName, "-inbuilt");
  } else if (argExist(numArgs, argv, "randommap") == TRUE) {
    /* Build the randommap marker string.
     * Format: "-randommap" or "-randommap <type>" or "-randommap <type> <seed>"
     * or "-randommap <seed>". Store the full info for main() to parse. */
    argNum = findArg(numArgs, argv, "randommap");
    if (argNum != ARG_NOT_FOUND && argv[argNum][0] != '-') {
      /* There's a next arg that isn't another flag */
      char *arg1 = (char *)argv[argNum];
      /* Check if it's a type name or a seed */
      char lower[64];
      strncpy(lower, arg1, sizeof(lower) - 1);
      lower[sizeof(lower) - 1] = '\0';
      strlower(lower);
      if (strcmp(lower, "tournament") == 0 || strcmp(lower, "natural") == 0 || strcmp(lower, "maze") == 0) {
        /* Type specified — check for seed after it */
        snprintf(mapName, 2048, "-randommap %s", lower);
        if (argNum + 1 < numArgs && argv[argNum + 1][0] != '-') {
          snprintf(mapName, 2048, "-randommap %s %s", lower, (char *)argv[argNum + 1]);
        }
      } else {
        /* Assume it's a seed string */
        snprintf(mapName, 2048, "-randommap seed %s", arg1);
      }
    } else {
      strcpy(mapName, "-randommap");
    }
  } else if (argExist(numArgs, argv, "mapdir") == TRUE) {
    /* -mapdir without -map: will pick random map after directory scan */
    strcpy(mapName, "-mapdir");
  } else {
    fprintf(stderr, "Missing map file\n");
    returnValue = FALSE;
  }
  
  /* Port */
  argNum = findArg(numArgs, argv, "port");
  if (argNum != ARG_NOT_FOUND) {
    *port = atoi((char *) argv[argNum]);
  } else {
    fprintf(stderr, "Missing port Number\n");
    returnValue = FALSE;
  }


  /* Game type */
  argNum = findArg(numArgs, argv, "gametype");
  if (argNum != ARG_NOT_FOUND) {
    strlower((char *) argv[argNum]);
    strncpy(temp, (char *) argv[argNum], sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';
    if (strcmp((char *) argv[argNum], "open") == 0) {
      *game = gameOpen;
    } else if (strcmp((char *) argv[argNum], "tournament") == 0) {
      *game = gameTournament;
    } else if (strcmp((char *) argv[argNum], "strict") == 0) {
      *game = gameStrictTournament;
    } else {
      returnValue = FALSE;
      fprintf(stderr, "Error in game type parameter\n");
    }
  } else {
    fprintf(stderr, "Missing game type parameter\n");
    returnValue = FALSE;
  }

  /* Option Arguments */

  /* Tracker — enabled by default using the public tracker.  Pass
     -notracker to opt out, or -tracker [host[:port]] to point at a
     different tracker. */
  if (argExist(numArgs, argv, "notracker") == TRUE) {
    *trackerUse = FALSE;
  } else {
    *trackerUse = TRUE;
    /* Default tracker unless -tracker supplies an explicit host. */
    strncpy(trackerAddr, DEFAULT_TRACKER_ADDR, FILENAME_MAX - 1);
    trackerAddr[FILENAME_MAX - 1] = '\0';
    *trackerPort = DEFAULT_TRACKER_PORT;
    if (argExist(numArgs, argv, "tracker") == TRUE) {
      argNum = findArg(numArgs, argv, "tracker");
      if (argNum != ARG_NOT_FOUND && argv[argNum][0] != '-') {
        processTrackerArg((char *) argv[argNum], trackerAddr, trackerPort);
      }
    }
  }

  /* Mines */
  argNum = findArg(numArgs, argv, "mines");
  if (argNum != ARG_NOT_FOUND) {
    strlower((char *) argv[argNum]);
    if (strcmp((char *) argv[argNum], "yes") == 0) {
      *hiddenMines = TRUE;
    } else if (strcmp((char *) argv[argNum], "no") == 0) {
      *hiddenMines = FALSE;
    } else {
      returnValue = FALSE;
      fprintf(stderr, "Error in hidden mines parameter\n");
    }
  } else {
    *hiddenMines = TRUE;
  }

  /* Allow AI */
  argNum = findArg(numArgs, argv, "ai");
  if (argNum != ARG_NOT_FOUND) {
    strlower((char *) argv[argNum]);
    if (strcmp((char *) argv[argNum], "no") == 0) {
      *ai = aiNone;
    } else if (strcmp((char *) argv[argNum], "yes") == 0) {
      *ai = aiYes;
    } else if (strcmp((char *) argv[argNum], "yesadv") == 0) {
      *ai = aiYesAdvantage;
    } else if (strcmp((char *) argv[argNum], "yesfull") == 0) {
      *ai = aiFull;
    } else {
      returnValue = FALSE;
      fprintf(stderr, "Error in ai parameter\n");
    }
  } else {
    *ai = aiNone;
  }

  /* Start delay */
  argNum = findArg(numArgs, argv, "delay");
  if (argNum != ARG_NOT_FOUND) {
    *srtDelay = atoi((char *) argv[argNum]);
    if (*srtDelay < 0) {
      *srtDelay = 0;
    } else if (*srtDelay > 0) {
      (*srtDelay) *= GAME_NUMGAMETICKS_SEC;
    }
  } else {
    *srtDelay = 0;
  }

  /* Time limit */
  argNum = findArg(numArgs, argv, "limit");
  if (argNum != ARG_NOT_FOUND) {
    *gmeLen = atoi((char *) argv[argNum]);
    if (*gmeLen <= 0) {
      *gmeLen = -1;
    } else if (*gmeLen > 0) {
      (*gmeLen) *= GAME_NUMGAMETICKS_SEC;
      (*gmeLen) *= NUM_SECONDS;
    }
  } else {
    *gmeLen = -1;
  }


  /* Password */
  argNum = findArg(numArgs, argv, "password");
  if (argNum != ARG_NOT_FOUND) {
    strncpy(password, (char *) argv[argNum], FILENAME_MAX - 1);
    password[FILENAME_MAX - 1] = '\0';
  } else {
    password[0] = '\0';
  }


  return returnValue;
}

#include <time.h>

/*********************************************************
*NAME:          serverEmitFinalJson
*PURPOSE:
*  Write a single JSON snapshot of the authoritative global
*  game state to `dest` ("-" = stdout, else a file path).
*
*  Unlike the headless client's per-tick --log-state (which is
*  player-centric: fog-of-war viewport around "self"), the
*  dedicated server has no ClientSim/brain view, so this is a
*  global snapshot: every connected tank, every pillbox, every
*  base, plus the winner (if any). Emitted once at end-of-game
*  for -finaljson, and repeatedly for -snapjson (append mode,
*  one object per line = JSONL).
*
*ARGUMENTS:
*  sim    - The server sim (must still hold final state).
*  dest   - "-" for stdout, otherwise a file path.
*  reason - Short machine tag for why the game ended.
*  append - FALSE truncates the file (the -finaljson contract,
*           unchanged); TRUE appends one line (-snapjson).
*********************************************************/
static void serverEmitFinalJson(ServerSim *sim, const char *dest,
                                const char *reason, bool append) {
  cJSON *root;
  cJSON *tanks;
  cJSON *pills;
  cJSON *bases;
  char winMsg[512];
  BYTE i;
  BYTE count;
  char *out;
  FILE *f;

  if (sim == NULL || dest == NULL || dest[0] == '\0') {
    return;
  }

  root = cJSON_CreateObject();
  if (root == NULL) {
    return;
  }

  cJSON_AddNumberToObject(root, "tick", (double)serverSimGetTick(sim));
  cJSON_AddStringToObject(root, "reason", reason);

  /* Winner: serverSimBuildWinMessage populates winMsg and returns TRUE only
   * when a single alliance won. A tick/time-limit end has no winner. */
  if (serverSimBuildWinMessage(sim, winMsg, sizeof(winMsg))) {
    cJSON_AddStringToObject(root, "winner", winMsg);
  } else {
    cJSON_AddNullToObject(root, "winner");
  }

  /* Tanks — one entry per connected player slot. owner/alliance is not
   * meaningful without a "self", so we report raw player index + identity,
   * score and (when a live tank exists) position. */
  tanks = cJSON_AddArrayToObject(root, "tanks");
  for (i = 0; i < MAX_TANKS; i++) {
    TankInfo ti;
    cJSON *t;
    if (!serverSimGetTankInfo(sim, i, &ti)) {
      continue;
    }
    /* Kills come from serverSimGetPlayerKills, NOT ti.kills: TankInfo.kills
     * reads tank->numKills, which only tankAddKill writes, and tankAddKill
     * is called solely from the client snapshot path (client_snapshot.c,
     * EVENT_TANK_KILLED for the local player). A dedicated server has no
     * client, so ti.kills is permanently 0 here. Deaths are credited
     * server-side, so ti.deaths is left alone. */
    t = cJSON_CreateObject();
    cJSON_AddNumberToObject(t, "player", (double)i);
    cJSON_AddStringToObject(t, "name", ti.name);
    cJSON_AddBoolToObject(t, "alive", ti.alive);
    cJSON_AddNumberToObject(t, "kills",
                            (double)serverSimGetPlayerKills(sim, i));
    cJSON_AddNumberToObject(t, "deaths", (double)ti.deaths);
    /* Per-cause split of the same death count, so a bench can tell a
     * drowning from a shell without replaying the game. The five entries
     * sum to "deaths". */
    {
      uint32_t causes[DEATH_CAUSE_NUM];
      cJSON *dc = cJSON_AddObjectToObject(t, "deaths_by");
      serverSimGetDeathCauses(sim, i, causes);
      if (dc != NULL) {
        /* "drowned" stays the TOTAL number of drownings so readers written
         * before the split keep working; "drowned_unforced" is the subset
         * of those in which no shell came near the tank in the last second
         * (the bot drove itself in).  Summing every key would therefore
         * double-count -- the independent causes are drowned + shell_tank +
         * shell_pill + mine + other, and those sum to "deaths". */
        cJSON_AddNumberToObject(dc, "drowned",
                                (double)(causes[DEATH_CAUSE_DROWNED] +
                                         causes[DEATH_CAUSE_DROWNED_UNFORCED]));
        cJSON_AddNumberToObject(dc, "drowned_unforced",
                                (double)causes[DEATH_CAUSE_DROWNED_UNFORCED]);
        cJSON_AddNumberToObject(dc, "shell_tank",
                                (double)causes[DEATH_CAUSE_SHELL_TANK]);
        cJSON_AddNumberToObject(dc, "shell_pill",
                                (double)causes[DEATH_CAUSE_SHELL_PILL]);
        cJSON_AddNumberToObject(dc, "mine",
                                (double)causes[DEATH_CAUSE_MINE]);
        cJSON_AddNumberToObject(dc, "other",
                                (double)causes[DEATH_CAUSE_OTHER]);
      }
    }
    if (ti.has_tank) {
      cJSON_AddNumberToObject(t, "x", (double)ti.world_x / 256.0);
      cJSON_AddNumberToObject(t, "y", (double)ti.world_y / 256.0);
      cJSON_AddNumberToObject(t, "tx", (double)(ti.world_x >> 8));
      cJSON_AddNumberToObject(t, "ty", (double)(ti.world_y >> 8));
      cJSON_AddNumberToObject(t, "dir", (double)ti.dir);
      cJSON_AddBoolToObject(t, "on_boat", ti.on_boat);
    }
    cJSON_AddItemToArray(tanks, t);
  }

  /* Pillboxes — 255 owner means neutral. */
  pills = cJSON_AddArrayToObject(root, "pillboxes");
  count = serverSimGetPillCount(sim);
  for (i = 1; i <= count; i++) {
    BYTE px, py, powner, parmour;
    bool pinTank;
    cJSON *p;
    if (!serverSimGetPill(sim, i, &px, &py, &powner, &parmour, &pinTank)) {
      continue;
    }
    p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "tx", (double)px);
    cJSON_AddNumberToObject(p, "ty", (double)py);
    cJSON_AddNumberToObject(p, "owner", (double)powner);
    cJSON_AddNumberToObject(p, "armor", (double)parmour);
    cJSON_AddBoolToObject(p, "in_tank", pinTank);
    cJSON_AddItemToArray(pills, p);
  }

  /* Bases — 255 owner means neutral. */
  bases = cJSON_AddArrayToObject(root, "bases");
  count = serverSimGetBaseCount(sim);
  for (i = 1; i <= count; i++) {
    BYTE bx, by, bowner;
    BYTE bshells, bmines, barmour;
    cJSON *b;
    if (!serverSimGetBase(sim, i, &bx, &by, &bowner)) {
      continue;
    }
    serverSimGetBaseStats(sim, i, &bshells, &bmines, &barmour);
    b = cJSON_CreateObject();
    cJSON_AddNumberToObject(b, "tx", (double)bx);
    cJSON_AddNumberToObject(b, "ty", (double)by);
    cJSON_AddNumberToObject(b, "owner", (double)bowner);
    cJSON_AddNumberToObject(b, "armor", (double)barmour);
    cJSON_AddNumberToObject(b, "shells", (double)bshells);
    cJSON_AddNumberToObject(b, "mines", (double)bmines);
    cJSON_AddItemToArray(bases, b);
  }

  out = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (out == NULL) {
    return;
  }

  if (strcmp(dest, "-") == 0) {
    f = stdout;
  } else {
    f = fopen(dest, append ? "a" : "w");
    if (f == NULL) {
      fprintf(stderr, "Error: cannot open %s file '%s'\n",
              append ? "-snapjson" : "-finaljson", dest);
      cJSON_free(out);
      return;
    }
  }

  fprintf(f, "%s\n", out);
  fflush(f);
  if (f != stdout) {
    fclose(f);
  }
  cJSON_free(out);
}

/*********************************************************
*NAME:          serverSnapshotTick
*PURPOSE:
*  serverSimSetSnapshotHook callback: appends one -snapjson
*  line every -snapinterval running ticks. Called from inside
*  the sim step (game-timer thread) before that step does any
*  work, so the state written is settled, not half-applied.
*  Read-only with respect to the sim.
*********************************************************/
static void serverSnapshotTick(ServerSim *sim) {
  serverEmitFinalJson(sim, optSnapJson, "snapshot", TRUE);
}

/* The lower-case word -pillview / -baseview / -allyview accept for a
 * policy. Only used to tell the operator what an unrecognised word fell
 * back to, so the message names the same value the parse below does
 * rather than a second copy of it. */
static const char *viewPolicyArgWord(ViewPolicy policy) {
  return (policy == viewPolicyAlways) ? "always"
       : (policy == viewPolicyKey)    ? "key"
       : (policy == viewPolicyDecay)  ? "decay"
                                      : "off";
}

/* Same for -overviewwindow. */
static const char *overviewWindowArgWord(OverviewWindow window) {
  return (window == overviewWindowNone)    ? "none"
       : (window == overviewWindowClassic) ? "classic"
                                           : "expanded";
}

/* One map's scenario script, checked and reported, for -validate. Returns
   what the process exits with: 0 for a map that is playable, 1 for one that is
   not. Nothing else in the server is running by the time this is called, and
   nothing it does starts anything. */
static int validateMapAndReport(char *mapPath) {
  ServerSim *sim;
  ScnValidateResult *result;
  char script[SCN_SCRIPT_PATH_MAX];
  bool ok;
  uint16_t i;

#ifdef USING_SDL
  /* The sim builds its locks through SDL. Nothing here needs a subsystem. */
  if (!SDL_Init(0)) {
    fprintf(stderr, "Error starting SDL - %s\n", SDL_GetError());
    return 1;
  }
#endif
  /* The debug file the server opens is a server's; a check writes nothing to
     it. */
  setWriteToDebugFileStream(-1);

  /* What the issues are printed against. A path with no room for a script
     name is reported against the map's own name. */
  if (scnScriptPath(mapPath, script, sizeof(script)) == FALSE) {
    snprintf(script, sizeof(script), "%s", mapPath);
  }

  sim = serverSimCreate(mapPath, gameOpen, FALSE, 0, -1);
  if (sim == NULL) {
    fprintf(stderr, "%s: the map could not be loaded\n", mapPath);
    return 1;
  }

  /* On the heap rather than the stack: a result carries the whole manifest and
     the issue list with it, which is more than this frame should hold. */
  result = (ScnValidateResult *)malloc(sizeof(*result));
  if (result == NULL) {
    fprintf(stderr, "%s: out of memory reading the script\n", mapPath);
    serverSimDestroy(sim);
    return 1;
  }

  ok = scenarioValidateMap(sim, mapPath, result);

  for (i = 0; i < result->count; i++) {
    const ScnValidateIssue *issue = &result->issues[i];
    if (issue->line > 0) {
      fprintf(stderr, "%s:%d: %s: %s\n", script, issue->line, issue->key,
              issue->message);
    } else {
      fprintf(stderr, "%s: %s: %s\n", script, issue->key, issue->message);
    }
  }

  if (result->haveManifest == FALSE && result->count == 0) {
    fprintf(stderr, "%s: no scenario script beside it\n", mapPath);
  } else if (ok == TRUE) {
    fprintf(stderr, "%s: no problems\n", script);
  } else if (result->dropped > 0) {
    fprintf(stderr, "%s: %u problems, and %u more than the list holds\n",
            script, (unsigned)result->count, (unsigned)result->dropped);
  } else {
    fprintf(stderr, "%s: %u problem%s\n", script, (unsigned)result->count,
            (result->count == 1) ? "" : "s");
  }

  free(result);
  serverSimDestroy(sim);
  return (ok == TRUE) ? 0 : 1;
}

/* How many bytes of the container -pack just wrote, read back off the file so
   the line below says what landed rather than what was meant to. 0 when the
   file cannot be read again, which is not a reason to call a pack that
   succeeded a failure. */
static size_t packedContainerLen(char *mapPath) {
  FILE *fp;
  long size;
  uint8_t *buf;
  size_t got;
  const uint8_t *chunk = NULL;
  size_t chunkLen = 0;

  fp = fopen(mapPath, "rb");
  if (fp == NULL) {
    return 0;
  }
  if (fseek(fp, 0, SEEK_END) != 0 || (size = ftell(fp)) <= 0 ||
      fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    return 0;
  }
  buf = (uint8_t *)malloc((size_t)size);
  if (buf == NULL) {
    fclose(fp);
    return 0;
  }
  got = fread(buf, 1, (size_t)size, fp);
  fclose(fp);
  if (got != (size_t)size ||
      scnPackageFindInMap(buf, got, &chunk, &chunkLen) == FALSE) {
    chunkLen = 0;
  }
  free(buf);
  return chunkLen;
}

/* One map's scenario written into the map, for -pack. Returns what the process
   exits with: 0 for a map that was packed, 1 for one that was not. Nothing
   else in the server is running by the time this is called, and nothing it
   does starts anything. */
static int packMapAndReport(char *mapPath) {
  char err[512];
  size_t containerLen;

#ifdef USING_SDL
  /* The sim the pack reads the map through builds its locks through SDL.
     Nothing here needs a subsystem. */
  if (!SDL_Init(0)) {
    fprintf(stderr, "Error starting SDL - %s\n", SDL_GetError());
    return 1;
  }
#endif
  /* The debug file the server opens is a server's; a pack writes nothing to
     it. */
  setWriteToDebugFileStream(-1);

  err[0] = '\0';
  if (scnPackMap(mapPath, err, sizeof(err)) == FALSE) {
    fprintf(stderr, "%s\n",
            (err[0] != '\0') ? err : "the map could not be packed");
    return 1;
  }

  containerLen = packedContainerLen(mapPath);
  if (containerLen > 0) {
    fprintf(stderr, "%s: packed, %lu bytes of scenario on the end of it\n",
            mapPath, (unsigned long)containerLen);
  } else {
    fprintf(stderr, "%s: packed\n", mapPath);
  }
  return 0;
}

int main(int argc, char **argv) {
  bolo_srand((uint64_t)time(NULL) ^ (uint64_t)getpid());
  {
    int seedArg = findArg(argc, argv, "seed");
    if (seedArg != ARG_NOT_FOUND) {
      bolo_srand(strtoull((char *)argv[seedArg], NULL, 0));
    }
  }
  sentryInit("WinBoloDS", argc, argv);
  atexit(sentryClose);
  wb_log_init("WinBolo", "WinBoloDS", "winbolods.log");
  atexit(wb_log_shutdown);

  /* -allow-unsafe-scripts: scenario scripts run with the full Lua library and
     no memory or time limits, uploaded maps' scripts included. Set ahead of
     -validate and -pack below, because the check they make boots a state as
     well and reads the same switch. Said loudly, as -allow-unsafe-brains is. */
  if ((argExist(argc, argv, "allow-unsafe-scripts") == TRUE) ||
      (argExist(argc, argv, "-allow-unsafe-scripts") == TRUE)) {
    scenarioHostSetUnsafeScripts(true);
    fprintf(stderr,
            "Note: -allow-unsafe-scripts — scenario scripts, including those "
            "in uploaded maps, now run with the full Lua library and no memory "
            "or time limits. Only run a server this way with content you "
            "trust.\n");
  }

  /* -validate <map> checks a map's scenario script and exits, and -pack <map>
     writes that script into the map file and exits. Both are answered here,
     ahead of the argument checks a server start needs, so a map can be checked
     or packed without a port and a game type to go with it — and before any of
     the network, the tracker, mDNS or a window is brought up. */
  {
    int validateArg = findArg(argc, argv, "validate");
    int packArg = findArg(argc, argv, "pack");
    if (validateArg != ARG_NOT_FOUND) {
      return validateMapAndReport((char *) argv[validateArg]);
    }
    if (packArg != ARG_NOT_FOUND) {
      return packMapAndReport((char *) argv[packArg]);
    }
  }

#ifdef _WIN32
  // Show the console w/o activation if we were started hidden by WinBolo.exe
  HWND hConsoleWnd = GetConsoleWindow();
  if (hConsoleWnd != NULL) {
    ShowWindow(hConsoleWnd, SW_SHOWNOACTIVATE);
  }
#endif

  gameType game;
  unsigned short port;
  bool hiddenMines;
  int srtDelay;
  int32_t gmeLen;
  char pass[FILENAME_MAX]; /* Password */
  char mapName[2048];
  /* The map file a scenario script would sit beside, for the two start
     paths that have one. -inbuilt and -randommap have no file on disk, so
     they carry no scenario. */
  char scenarioMapPath[2048] = "";
  aiType ai; /* Should we allow ai */
  /* Tracker stuff */
  char trackerAddr[FILENAME_MAX];
  unsigned short trackerPort;
  bool trackerUse;
  char *useAddr;
  char debugFileName[2048];
  int maxPlayers;
  int maxBots;
  int maxSpectators = 16;
  int specDelay = 90;
  char key[WINBOLONET_KEY_LEN]; /* WBN Key */

  strcpy(debugFileName,"server_test.txt");

  /* Debugging file stuff */
  setWriteToDebugFileStream(-1);
  setFileName(debugFileName);
  if (openDebugFile() == -1) {
    setWriteToDebugFileStream(-1);
  }

  isQuiet = FALSE;
  isNoInput = FALSE;
  maxPlayers = 0;
  maxBots = 0;

  alarmRaised = alarmNone;
#ifdef _WIN32
  /* Set up console ctrl handler */
  SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
#else
  /* Set up signal handler */
  signal(SIGINT, catch_alarm);
  signal(SIGUSR1, catch_alarm);
  signal(SIGUSR2, catch_alarm);
#endif

  if (processArgs(argc, argv, mapName, &port, &game, &hiddenMines, &ai, &srtDelay, &gmeLen, trackerAddr, &trackerPort, &trackerUse, pass) == FALSE) {
    fprintf(stderr, "Error in command line parameters\n");
    printArgs();
    exit(0);
  }

  /* Copy tracker settings to file-scope globals for the timer */
  sTrackerUse = trackerUse;
  if (trackerUse) {
    strncpy(sTrackerAddr, trackerAddr, FILENAME_MAX - 1);
    sTrackerAddr[FILENAME_MAX - 1] = '\0';
    sTrackerPort = trackerPort;
  }

  if (argExist(argc, argv, "quiet") == TRUE) {
    isQuiet = TRUE;
  }
  isNoInput = argExist(argc, argv, "noinput");
  isAsap = (argExist(argc, argv, "asap") == TRUE);

  if (argExist(argc, argv, "maxplayers") == TRUE) {
    maxPlayers = atoi((char *) argv[findArg(argc, argv, "maxplayers")]);
    if (maxPlayers < 0 || maxPlayers > MAX_TANKS) {
      maxPlayers = 0;
    }
  }

  if (argExist(argc, argv, "maxbots") == TRUE) {
    maxBots = atoi((char *) argv[findArg(argc, argv, "maxbots")]);
    if (maxBots < 0 || maxBots > MAX_TANKS) {
      maxBots = 0;
    }
  }

  if (argExist(argc, argv, "maxspectators") == TRUE) {
    maxSpectators = atoi((char *) argv[findArg(argc, argv, "maxspectators")]);
    /* 0 is valid (= spectating disabled). Negative resets to the default.
     * Spectators aren't tank-bounded, so there is no MAX_TANKS ceiling;
     * clamp at 255 since the field is a BYTE. */
    if (maxSpectators < 0) {
      maxSpectators = 16;
    } else if (maxSpectators > 255) {
      maxSpectators = 255;
    }
  }

  if (argExist(argc, argv, "specdelay") == TRUE) {
    specDelay = atoi((char *) argv[findArg(argc, argv, "specdelay")]);
    /* 0 is valid (= live, no floor). Negative resets to the default.
     * Clamp at 65535 since specDelaySeconds is a uint16_t. */
    if (specDelay < 0) {
      specDelay = 90;
    } else if (specDelay > 65535) {
      specDelay = 65535;
    }
  }

#ifdef USING_SDL
  /* SDL_Init logs an INFO version/app banner on the SYSTEM category. Treat it
     as debug detail: suppress it unless SYSTEM logging is at debug/trace. */
  SDL_LogPriority sysPrio = SDL_GetLogPriority(SDL_LOG_CATEGORY_SYSTEM);
  bool sysQuieted = (sysPrio > SDL_LOG_PRIORITY_DEBUG);
  if (sysQuieted) {
    SDL_SetLogPriority(SDL_LOG_CATEGORY_SYSTEM, SDL_LOG_PRIORITY_WARN);
  }
  if (!SDL_Init(0)) {
    fprintf(stderr, "Error starting SDL - %s\n", SDL_GetError());
    exit(0);
  }
  if (sysQuieted) {
    SDL_SetLogPriority(SDL_LOG_CATEGORY_SYSTEM, sysPrio);
  }
#endif
  /* IP-to-country geolocation (DB-IP Lite). Resolve the database relative to
     the executable directory so the server works regardless of the CWD it was
     launched from; fall back to the CWD-relative path if that fails. */
  bool geoLookupOk = FALSE;
#ifdef USING_SDL
  {
    const char *basePath = SDL_GetBasePath();
    if (basePath != NULL) {
      char mmdbPath[FILENAME_MAX];
      snprintf(mmdbPath, sizeof(mmdbPath), "%sdata/dbip-country-lite.mmdb",
               basePath);
      geoLookupOk = geoLookupCreate(mmdbPath);
    }
  }
#endif
  if (geoLookupOk == FALSE) {
    geoLookupOk = geoLookupCreate("data/dbip-country-lite.mmdb");
  }

  /* Create server simulation */
  if (strncmp(mapName, "-randommap", 10) == 0) {
    MapGenConfig cfg;
    bool hasFixedSeed = false;

    /* Parse the marker string */
    char typeStr[32] = "";
    char seedStr[64] = "";
    /* mapName is "-randommap" or "-randommap <type>" or "-randommap <type> <seed>"
       or "-randommap seed <seed>" */
    if (strlen(mapName) > 11) {
      sscanf(mapName + 11, "%31s %63s", typeStr, seedStr);
    }

    if (strcmp(typeStr, "seed") == 0) {
      /* "-randommap seed <seed>" — seed is in seedStr */
      cfg = mapGenDefaultConfig(MAPGEN_TOURNAMENT);
      if (mapGenSeedToConfig(seedStr, &cfg)) {
        hasFixedSeed = true;
      }
    } else if (typeStr[0] != '\0') {
      /* Type specified */
      int genType = MAPGEN_TOURNAMENT;
      if (strcmp(typeStr, "natural") == 0) genType = MAPGEN_NATURAL;
      else if (strcmp(typeStr, "maze") == 0) genType = MAPGEN_MAZE;
      else if (strcmp(typeStr, "fractal") == 0) genType = MAPGEN_FRACTAL;
      cfg = mapGenDefaultConfig(genType);

      if (seedStr[0] != '\0') {
        /* Type + seed */
        if (mapGenSeedToConfig(seedStr, &cfg)) {
          hasFixedSeed = true;
        }
      }
    } else {
      /* Plain "-randommap" — fully random */
      int types[] = { MAPGEN_TOURNAMENT, MAPGEN_NATURAL, MAPGEN_MAZE, MAPGEN_FRACTAL };
      cfg = mapGenDefaultConfig(types[bolo_rand_below(4)]);
    }

    /* Force tournament maps to max objects */
    if (cfg.genType == MAPGEN_TOURNAMENT) {
      cfg.bases = 16;
      cfg.pills = 16;
      cfg.starts = 16;
    }

    /* Generate a random seed if none was provided */
    if (!hasFixedSeed) {
      cfg.seed = (uint32_t)time(NULL) ^ ((uint32_t)clock() << 16);
      if (cfg.seed == 0) cfg.seed = 1;
    }

    /* Set region to full playable area */
    cfg.x1 = 21; cfg.y1 = 21;
    cfg.x2 = 235; cfg.y2 = 235;

    serverSim = serverSimCreateRandomMap(&cfg, game, hiddenMines, srtDelay, gmeLen);
    if (serverSim == NULL) {
      fprintf(stderr, "Error generating random map\n");
#ifdef USING_SDL
      SDL_Quit();
#endif
      return 0;
    }

    /* Log the seed so admins can reproduce this map */
    {
      char seedBuf[64];
      char msg[128];
      mapGenConfigToSeed(&cfg, seedBuf, sizeof(seedBuf));
      snprintf(msg, sizeof(msg), "Generated random map with seed: %s", seedBuf);
      serverMessageConsoleMessage(serverSim, msg);
    }

    /* Store config for between-round regeneration */
    serverSimEnableRandomMap(serverSim, &cfg, hasFixedSeed);

  } else if (strcmp(mapName, "-inbuilt") == 0) {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4305)
#endif
    BYTE emap[6000] = E_MAP;
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    serverSim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island", game, hiddenMines, srtDelay, gmeLen);
    if (serverSim == NULL) {
      fprintf(stderr, "Error starting server simulation (inbuilt map)\n");
#ifdef USING_SDL
      SDL_Quit();
#endif
      return 0;
    }
    serverSimSetMapName(serverSim, "Everard Island");
  } else if (strcmp(mapName, "-mapdir") == 0) {
    /* -mapdir without -map: scan the directory for valid maps, pick one at
     * random for the initial Create, then transfer the list onto the
     * created sim. */
    char **scannedFiles = NULL;
    int scannedCount = 0;
    int mdArg = findArg(argc, argv, "mapdir");
    if (serverSimScanMapDir((const char *)argv[mdArg], &scannedFiles, &scannedCount) == FALSE) {
#ifdef USING_SDL
      SDL_Quit();
#endif
      return 0;
    }
    {
      const char *chosen = scannedFiles[bolo_rand_below((uint32_t)scannedCount)];
      snprintf(scenarioMapPath, sizeof(scenarioMapPath), "%s", chosen);
      serverSim = serverSimCreate((char *)chosen, game, hiddenMines, srtDelay, gmeLen);
    }
    if (serverSim == NULL) {
      int i;
      for (i = 0; i < scannedCount; i++) SDL_free(scannedFiles[i]);
      free(scannedFiles);
      fprintf(stderr, "Error starting server simulation\n");
#ifdef USING_SDL
      SDL_Quit();
#endif
      return 0;
    }
    serverSimInstallMapDirList(serverSim, scannedFiles, scannedCount,
                               (const char *)argv[mdArg]);
  } else {
    snprintf(scenarioMapPath, sizeof(scenarioMapPath), "%s", mapName);
    serverSim = serverSimCreate(mapName, game, hiddenMines, srtDelay, gmeLen);
    if (serverSim == NULL) {
      fprintf(stderr, "Error starting server simulation\n");
#ifdef USING_SDL
      SDL_Quit();
#endif
      return 0;
    }
  }

  /* sim now exists — perform the init that used to happen before
   * Create (when sim was an embedded zero-struct that Create then
   * clobbered). */
  serverMessageSetQuietMode(serverSim, isQuiet ? TRUE : FALSE);
  {
    char banner[256];
    snprintf(banner, sizeof banner,
             "WinBolo Server - v%s\n"
             "Copyright 1998-2026 John Morrison\n"
             "Bolo Copyright 1987-1995 Stuart Cheshire",
             WINBOLO_DISPLAY_VERSION);
    serverMessageConsoleMessage(serverSim, banner);
  }
  if (findArg(argc, argv, "logfile") != ARG_NOT_FOUND) {
    serverMessagesSetLogFile(serverSim, (char *) argv[findArg(argc, argv, "logfile")]);
  }
  if (geoLookupOk) {
    serverMessageConsoleMessage(serverSim, "Geo lookup database loaded.\n");
  } else {
    serverMessageConsoleMessage(serverSim, "Geo lookup database not found — country codes will be XX.\n");
  }

  /* -noscenarios: run every map plainly, whatever sits beside it. Set on the
     library before the first attach, so the map commits that follow answer to
     it as well. */
  if (argExist(argc, argv, "noscenarios") == TRUE) {
    scenarioHostSetEnabled(false);
  }
  /* -scriptuploads: the narrower one. Under off a map a client sent plays
     plainly whatever it carries, and the operator's own maps are untouched.
     -nouploadscripts is the old spelling of off, used when no word is given.
     Resolved here rather than beside the map-upload flags below because the
     host switch has to be set before the first attach, for the same reason
     as -noscenarios; the same value goes into the instance config below. */
  const int scriptPolicyArg = findArg(argc, argv, "scriptuploads");
  const ScriptUploadPolicy scriptUploadPolicy = scriptUploadPolicyResolve(
      scriptPolicyArg != ARG_NOT_FOUND ? (char *)argv[scriptPolicyArg] : NULL,
      argExist(argc, argv, "nouploadscripts") == TRUE);
  scenarioHostSetUploadScriptsEnabled(scriptUploadPolicy != SCRIPT_UPLOAD_OFF);
  /* And the question the map lister asks of every map it finds, so the list
     a player picks from says which maps are scripted. Registered here rather
     than at the attach below: an attach answers NULL for a map with no
     script, so a server whose own map is plain would report every scripted
     map in its directory as plain. */
  scenarioHostRegisterMapScripted(serverSim);
  /* And the read of the scenarios directory, so a client asking what this
     server offers on its own is answered. Registered in the same place and
     for the same reason: what the list holds has nothing to do with whichever
     map is loaded.

     -moddir names that directory; without it the sim's own default,
     data/scenarios, stands. Either way the mods that ship with the build are
     read behind it, so a server that names nothing still offers those. A
     directory that is not there is not an error — it means this server
     offers none of its own, which is the ordinary case.

     -scenariodir is what this argument was called before mods and scenarios
     were one directory. Still taken, so a startup script written against it
     keeps working; -moddir wins when both are given. */
  {
    int argNum = findArg(argc, argv, "moddir");
    if (argNum == ARG_NOT_FOUND) {
      argNum = findArg(argc, argv, "scenariodir");
    }
    if (argNum != ARG_NOT_FOUND) {
      serverSimSetScenarioDir(serverSim, argv[argNum]);
    }
  }
  scenarioHostRegisterScenarioLister(serverSim);

  /* A scenario script beside the map, when the map came from a file and one
     is there. No script is the ordinary case and says nothing; a script
     that cannot be used says why, as does one -noscenarios turned down, and
     the server runs the map plainly. */
  if (scenarioMapPath[0] != '\0') {
    char scenarioErr[512];
    scenarioHost = scenarioHostAttach(serverSim, scenarioMapPath,
                                      scenarioErr, sizeof(scenarioErr));
    if (scenarioHost != NULL) {
      char line[SCN_SCRIPT_PATH_MAX + 128];
      snprintf(line, sizeof(line), "Scenario loaded: %s (from %s)",
               scenarioHostName(scenarioHost),
               scenarioHostScriptPath(scenarioHost));
      serverMessageConsoleMessage(serverSim, line);
    } else if (scenarioErr[0] != '\0') {
      serverMessageConsoleMessage(serverSim, scenarioErr);
    }
  }
  /* -setting: the values a lobby host would choose for scripts' own
     settings, for a server with no lobby to choose them in. Applied before
     any round starts, so on_setup already reads them. */
  {
    const char *mapScript = "";
    int         i;

    if (scenarioHost != NULL && argExist(argc, argv, "setting") == TRUE) {
      /* The map's own script declares its settings on a row the sim is
         handed at a map commit, and none has happened yet. */
      scenarioHostPublishMapScript(serverSim, scenarioHost);
    }
    if (scenarioHost != NULL) {
      const char *path = scenarioHostScriptPath(scenarioHost);
      const char *p;
      mapScript = path;
      for (p = path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') mapScript = p + 1;
      }
    }
    for (i = 1; i + 1 < argc; i++) {
      if (strcmp(argv[i], "-setting") == 0) {
        serverApplySettingArg(serverSim, argv[i + 1], mapScript);
      }
    }
  }
  /* And from here on the sim says when a map is committed, so the scenario
     follows the map without this file knowing how. */
  scenarioHostFollowMap(serverSim, &scenarioHost);

  useAddr = NULL;
  httpSetAltIpAddress("");
  if (argExist(argc, argv, "addr") == TRUE) {
    useAddr = (char *) argv[findArg(argc, argv, "addr")];
    httpSetAltIpAddress(useAddr);
  }

  statusFile = argExist(argc, argv, "statusFile");
  /* -maprotate: no-lobby server that rotates maps forever. A win or an empty
   * server boots everyone, picks the next -mapdir map and restarts a fresh
   * round; it never auto-quits. Implies no-lobby (set below) and forces
   * quit-on-win so a win drives the round to game-over, where the lifecycle
   * intercepts it and rotates instead of shutting the process down. */
  bool mapRotate = (argExist(argc, argv, "maprotate") == TRUE);
  serverSimSetQuitOnWin(serverSim,
                        (argExist(argc, argv, "quitonwin") == TRUE) || mapRotate);
  serverSimSetAutoCloseOnEmpty(serverSim, argExist(argc, argv, "autoclose") == TRUE);
  serverSimSetMapRotate(serverSim, mapRotate);

  {
    int argNum = findArg(argc, argv, "ticks");
    if (argNum != ARG_NOT_FOUND) {
      serverSimSetTickLimit(serverSim, (int32_t)strtoul((char *)argv[argNum], NULL, 0));
    }
  }
  {
    int argNum = findArg(argc, argv, "ticklimit");
    if (argNum != ARG_NOT_FOUND) {
      serverSimSetGameTickLimit(serverSim, (int32_t)strtoul((char *)argv[argNum], NULL, 0));
    }
  }
  {
    int argNum = findArg(argc, argv, "finaljson");
    if (argNum != ARG_NOT_FOUND) {
      strncpy(optFinalJson, (char *)argv[argNum], sizeof(optFinalJson) - 1);
      optFinalJson[sizeof(optFinalJson) - 1] = '\0';
    }
  }
  {
    int argNum = findArg(argc, argv, "snapjson");
    if (argNum != ARG_NOT_FOUND) {
      strncpy(optSnapJson, (char *)argv[argNum], sizeof(optSnapJson) - 1);
      optSnapJson[sizeof(optSnapJson) - 1] = '\0';
    }
    argNum = findArg(argc, argv, "snapinterval");
    if (argNum != ARG_NOT_FOUND) {
      optSnapInterval = (int32_t)strtol((char *)argv[argNum], NULL, 0);
      if (optSnapInterval < 0) {
        optSnapInterval = 0;
      }
    }
    if (optSnapJson[0] != '\0' && optSnapInterval > 0) {
      /* Start a fresh series: the emit path appends, so an existing file
       * from a previous run would otherwise be extended. "-" is stdout. */
      if (strcmp(optSnapJson, "-") != 0) {
        FILE *snapTrunc = fopen(optSnapJson, "w");
        if (snapTrunc == NULL) {
          fprintf(stderr, "Error: cannot open -snapjson file '%s'\n",
                  optSnapJson);
          optSnapJson[0] = '\0';
        } else {
          fclose(snapTrunc);
        }
      }
      if (optSnapJson[0] != '\0') {
        serverSimSetSnapshotHook(serverSim, serverSnapshotTick,
                                 optSnapInterval);
      }
    } else if (optSnapJson[0] != '\0') {
      fprintf(stderr,
              "Warning: -snapjson given without a positive -snapinterval; "
              "no snapshots will be written\n");
      optSnapJson[0] = '\0';
    }
  }

  /* Empty reset configuration — on by default */
  bool emptyResetEnabled = (argExist(argc, argv, "noemptyreset") == FALSE);
  {
    int argNum = findArg(argc, argv, "emptyresetmins");
    if (argNum != ARG_NOT_FOUND) {
      int mins = atoi((char *)argv[argNum]);
      if (mins > 0) {
        serverSimSetEmptyResetMinutes(serverSim, mins);
      }
    }
  }

  /* Lobby presets — set initial values for lobby toggles the host can
   * normally flip in the UI. Only meaningful in lobby mode; if
   * -nolobby is also set, the lobby state machine is skipped and the
   * initial values just bake into the running game's settings. ranked
   * forces autolock-on-game-start inside serverInstanceStartup. */
  bool autoLockOnGameStart = (argExist(argc, argv, "autolock") == TRUE);
  bool ranked              = (argExist(argc, argv, "ranked") == TRUE);
  bool openHost            = (argExist(argc, argv, "openhost") == TRUE);
  if (argExist(argc, argv, "firstjoinhost") == TRUE) {
    serverSimSetFirstJoinerBecomesHost(serverSim, true);
  }

  /* -lock <comma-list> — bit-OR a set of LOBBY_LOCK_* flags into the
   * server's lock mask. Locked settings are read-only from any client
   * (including host / admin / openHost); the lobby UI renders them
   * disabled with a lock badge. Plumbed end-to-end before this flag
   * existed but inert because nothing set the mask; this fixes that.
   * Valid names: gametype, ai, mines, timelimit, autolock, password,
   * ranked, openhost, map. Unknown names emit a warning and are
   * skipped (forward-compat for future locks). */
  uint32_t serverLocks = 0;
  {
    int argNum = findArg(argc, argv, "lock");
    if (argNum != ARG_NOT_FOUND) {
      const char *list = (const char *)argv[argNum];
      char tmp[256];
      strncpy(tmp, list, sizeof(tmp) - 1);
      tmp[sizeof(tmp) - 1] = '\0';
      char *saveptr = NULL;
      (void)saveptr;
      for (char *tok = strtok(tmp, ","); tok != NULL; tok = strtok(NULL, ",")) {
        /* trim + lowercase */
        while (*tok == ' ') tok++;
        char lo[64];
        int li = 0;
        for (int i = 0; tok[i] && li < 63; i++) {
          char c = tok[i];
          if (c == ' ') continue;
          if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
          lo[li++] = c;
        }
        lo[li] = '\0';
        if      (strcmp(lo, "gametype") == 0)  serverLocks |= LOBBY_LOCK_GAME_TYPE;
        else if (strcmp(lo, "ai") == 0)        serverLocks |= LOBBY_LOCK_AI_POLICY;
        else if (strcmp(lo, "mines") == 0)     serverLocks |= LOBBY_LOCK_MINES;
        else if (strcmp(lo, "timelimit") == 0 ||
                 strcmp(lo, "limit") == 0)     serverLocks |= LOBBY_LOCK_TIME_LIMIT;
        else if (strcmp(lo, "autolock") == 0)  serverLocks |= LOBBY_LOCK_AUTO_LOCK_ON_GAME;
        else if (strcmp(lo, "password") == 0)  serverLocks |= LOBBY_LOCK_PASSWORD;
        else if (strcmp(lo, "ranked") == 0)    serverLocks |= LOBBY_LOCK_RANKED;
        else if (strcmp(lo, "openhost") == 0)  serverLocks |= LOBBY_LOCK_OPEN_HOST;
        else if (strcmp(lo, "map") == 0)       serverLocks |= LOBBY_LOCK_MAP;
        else if (strcmp(lo, "pillview") == 0)  serverLocks |= LOBBY_LOCK_PILL_VIEW;
        else if (strcmp(lo, "baseview") == 0)  serverLocks |= LOBBY_LOCK_BASE_VIEW;
        else if (strcmp(lo, "allyview") == 0)  serverLocks |= LOBBY_LOCK_ALLY_VIEW;
        else if (strcmp(lo, "classicmode") == 0) serverLocks |= LOBBY_LOCK_CLASSIC_MODE;
        else if (strcmp(lo, "alliesintrees") == 0) serverLocks |= LOBBY_LOCK_ALLIES_IN_TREES;
        else if (strcmp(lo, "overviewwindow") == 0) serverLocks |= LOBBY_LOCK_OVERVIEW_WINDOW;
        else if (strcmp(lo, "lineofsight") == 0) serverLocks |= LOBBY_LOCK_LINE_OF_SIGHT;
        else if (strcmp(lo, "smartpings") == 0) serverLocks |= LOBBY_LOCK_SMART_PINGS;
        else if (strcmp(lo, "mods") == 0)      serverLocks |= LOBBY_LOCK_MODS;
        else if (strcmp(lo, "positionalsound") == 0) serverLocks |= LOBBY_LOCK_POSITIONAL_SOUND;
        else {
          fprintf(stderr,
                  "Warning: unknown -lock name '%s' (valid: gametype, "
                  "ai, mines, timelimit, autolock, password, ranked, "
                  "openhost, map, pillview, baseview, allyview, "
                  "classicmode, alliesintrees, overviewwindow, "
                  "lineofsight, smartpings, mods, positionalsound)\n", lo);
        }
      }
      /* Locking any visibility setting locks classicmode too, because
       * turning classic mode on writes those same values. The sim does
       * this for us; say so here so the operator isn't surprised by a
       * locked checkbox they never named. */
      uint32_t implied = serverSimAddImpliedLocks(serverLocks);
      if (implied != serverLocks) {
        fprintf(stderr,
                "Note: -lock of pillview / baseview / allyview / "
                "alliesintrees / overviewwindow / lineofsight / "
                "positionalsound also "
                "locks classicmode, which writes those values.\n");
        serverLocks = implied;
      }
    }
  }

  /* -pillview / -baseview / -allyview and their decay values. Applied
   * straight onto the created sim rather than through
   * ServerInstanceConfig, so serverSimCreate* keeps its signature. Every
   * category is set on every run — with no switches given that writes
   * back the same defaults serverSimInit already put there. An unknown
   * mode word or an out-of-range decay warns and falls back, matching
   * -uploadpolicy / -uploadmaxfiles. */
  {
    static const struct {
      const char  *modeArg;
      const char  *decayArg;
      ViewCategory cat;
      ViewPolicy   stock;     /* meaning A in view_policy.h */
    } viewArgs[] = {
      { "pillview", "pillviewdecay", viewCategoryPill, VIEW_POLICY_STOCK_PILL },
      { "baseview", "baseviewdecay", viewCategoryBase, VIEW_POLICY_STOCK_BASE },
      { "allyview", "allyviewdecay", viewCategoryAlly, VIEW_POLICY_STOCK_ALLY },
    };
    for (int vi = 0; vi < (int)(sizeof(viewArgs) / sizeof(viewArgs[0])); vi++) {
      ViewPolicy policy = viewArgs[vi].stock;
      int secs = VIEW_DECAY_DEFAULT_SECS;
      int modeNum = findArg(argc, argv, viewArgs[vi].modeArg);
      if (modeNum != ARG_NOT_FOUND) {
        char modeStr[32];
        strncpy(modeStr, (char *)argv[modeNum], sizeof(modeStr) - 1);
        modeStr[sizeof(modeStr) - 1] = '\0';
        strlower(modeStr);
        if (strcmp(modeStr, "always") == 0) {
          policy = viewPolicyAlways;
        } else if (strcmp(modeStr, "key") == 0) {
          policy = viewPolicyKey;
        } else if (strcmp(modeStr, "decay") == 0) {
          policy = viewPolicyDecay;
        } else if (strcmp(modeStr, "off") == 0) {
          policy = viewPolicyOff;
        } else {
          fprintf(stderr, "Unknown -%s '%s'; using %s\n",
                  viewArgs[vi].modeArg, modeStr,
                  viewPolicyArgWord(viewArgs[vi].stock));
          policy = viewArgs[vi].stock;
        }
      }
      int decayNum = findArg(argc, argv, viewArgs[vi].decayArg);
      if (decayNum != ARG_NOT_FOUND) {
        secs = atoi((char *)argv[decayNum]);
        if (secs < VIEW_DECAY_MIN_SECS) {
          fprintf(stderr, "-%s %d out of range; clamping to %d\n",
                  viewArgs[vi].decayArg, secs, VIEW_DECAY_MIN_SECS);
          secs = VIEW_DECAY_MIN_SECS;
        } else if (secs > VIEW_DECAY_MAX_SECS) {
          fprintf(stderr, "-%s %d out of range; clamping to %d\n",
                  viewArgs[vi].decayArg, secs, VIEW_DECAY_MAX_SECS);
          secs = VIEW_DECAY_MAX_SECS;
        }
      }
      serverSimSetViewPolicy(serverSim, viewArgs[vi].cat, policy,
                             (uint16_t)secs);
    }
  }

  /* -alliesintrees: send allied tanks standing in trees to their allies.
   * Applied before -classicmode so classic mode wins when both are on the
   * same command line. Only set when the flag is present — the sim
   * default is off. */
  if (argExist(argc, argv, "alliesintrees") == TRUE) {
    serverSimSetAlliesInTrees(serverSim, true);
  }

  /* -overviewwindow <M>: which block of squares the map overview keeps
   * live. An unrecognised word warns and falls back to the stock
   * window, the same as the three view switches. */
  {
    OverviewWindow window = OVERVIEW_WINDOW_STOCK;
    int windowNum = findArg(argc, argv, "overviewwindow");
    if (windowNum != ARG_NOT_FOUND) {
      char modeStr[32];
      strncpy(modeStr, (char *)argv[windowNum], sizeof(modeStr) - 1);
      modeStr[sizeof(modeStr) - 1] = '\0';
      strlower(modeStr);
      if (strcmp(modeStr, "expanded") == 0) {
        window = overviewWindowExpanded;
      } else if (strcmp(modeStr, "classic") == 0) {
        window = overviewWindowClassic;
      } else if (strcmp(modeStr, "none") == 0) {
        window = overviewWindowNone;
      } else {
        fprintf(stderr, "Unknown -overviewwindow '%s'; using %s\n", modeStr,
                overviewWindowArgWord(OVERVIEW_WINDOW_STOCK));
        window = OVERVIEW_WINDOW_STOCK;
      }
    }
    serverSimSetOverviewWindow(serverSim, (uint8_t)window);
  }

  /* -lineofsight: buildings and stands of trees block sight inside the
   * live block. Applied before -classicmode so classic mode wins when
   * both are on the same command line. Only set when the flag is
   * present — the sim default is off. */
  if (argExist(argc, argv, "lineofsight") == TRUE) {
    serverSimSetLineOfSight(serverSim, (uint8_t)lineOfSightBuildingsAndTrees);
  }

  /* -positionalsound: sounds carry which side they are on and a banded
   * distance. Applied before -classicmode so classic mode wins when both
   * are on the same command line. Only set when the flag is present — the
   * sim default is off. */
  if (argExist(argc, argv, "positionalsound") == TRUE) {
    serverSimSetPositionalSound(serverSim, true);
  }

  /* -smartpingsoff: the whole server refuses CMD_PING. Set here, before
   * serverInstanceStartup, so the lobby snapshot captures it — a value set
   * after that is not in originalLobbySettings and the next reset to
   * defaults would turn pings back on. Only set when the flag is present;
   * the sim default is allowed. Classic mode does not touch this: smart
   * pings are not one of the seven settings it writes. */
  if (argExist(argc, argv, "smartpingsoff") == TRUE) {
    serverSimSetSmartPingsOff(serverSim, true);
  }

  /* -classicmode: the classic Bolo view. Applied after the three view
   * switches so it wins when both are on the same command line, and
   * before serverInstanceStartup so the lobby snapshot captures it.
   * Only set when the flag is present — the sim default is off. */
  if (argExist(argc, argv, "classicmode") == TRUE) {
    serverSimSetClassicMode(serverSim, true);
  }

  /* -nolobby: skip lobby, start running immediately (backward-compatible
   * mode). serverInstanceStartup runs SetLobbyEnabled(false) + StartGame
   * from cfg.skipLobby; emptyReset is force-disabled in this mode. */
  bool skipLobby = (argExist(argc, argv, "nolobby") == TRUE) || mapRotate;
  if (skipLobby) {
    emptyResetEnabled = false;
  }

  /* -maprotate needs a -mapdir to rotate through. */
  if (mapRotate && findArg(argc, argv, "mapdir") == ARG_NOT_FOUND) {
    fprintf(stderr, "Error: -maprotate requires -mapdir\n");
#ifdef USING_SDL
    SDL_Quit();
#endif
    return 0;
  }

  /* -mapdir: build validated map list for rotation between rounds.
   * Skip if already built (the -mapdir without -map case builds it earlier). */
  {
    int argNum = findArg(argc, argv, "mapdir");
    if (argNum != ARG_NOT_FOUND && serverSimGetMapDirFiles(serverSim) == NULL) {
      /* -mapdir normally requires lobby mode (no-lobby has no round boundary
       * to rotate at). -maprotate is the exception: it is a no-lobby mode
       * built around rotating at each round end. */
      if (skipLobby && !mapRotate) {
        fprintf(stderr, "Error: -mapdir requires lobby mode (incompatible with -nolobby)\n");
#ifdef USING_SDL
        SDL_Quit();
#endif
        return 0;
      }
      if (serverSimMapDirBuild(serverSim, (char *)argv[argNum]) == FALSE) {
#ifdef USING_SDL
        SDL_Quit();
#endif
        return 0;
      }
    }
  }

  /* Load the process-global preferences document the shared winbolonet code
   * reads through (e.g. httpCreate's [WINBOLO.NET] Host). */
  prefsInit("WinBolo.json");
  {
    int argNum = findArg(argc, argv, "wbnhost");
    if (argNum != ARG_NOT_FOUND) {
      httpSetHostOverride((char *)argv[argNum]);
    }
  }

  /* Bot count + brain path resolution. Hoisted above instCfg so cfg
   * can carry them into serverInstanceStartup; the actual bot creation
   * (botManagerAddBot loop) still runs after startup. */
  int  numBots = 0;
  char brainPath[MAX_PATH];
  brainPath[0] = '\0';
  {
    int argNum = findArg(argc, argv, "bots");
    if (argNum != ARG_NOT_FOUND) {
      numBots = atoi((char *)argv[argNum]);
      if (numBots < 0) numBots = 0;
      if (numBots > MAX_TANKS) numBots = MAX_TANKS;
      if (maxBots > 0 && numBots > maxBots) {
        fprintf(stderr,
                "Warning: -bots %d exceeds -maxbots %d, capping at %d\n",
                numBots, maxBots, maxBots);
        numBots = maxBots;
      }
    }
    argNum = findArg(argc, argv, "brain");
    if (argNum != ARG_NOT_FOUND) {
      strncpy(brainPath, (char *)argv[argNum], MAX_PATH - 1);
      brainPath[MAX_PATH - 1] = '\0';
    }

    /* Bot naming pools: a custom file via -botnames, otherwise the
     * shipped data/bot_names.json. These supply the themed names for
     * the auto-add loop below. Bot names travel on the wire as
     * strings, so a server's choice of pool is always rendered
     * correctly on every client regardless of which file the client
     * has. Silent fallback to the built-in names on any failure. */
    {
      int bnArg = findArg(argc, argv, "botnames");
      LobbyBotPoolLoadStats poolStats;
      bool poolsLoaded = false;
      if (bnArg != ARG_NOT_FOUND) {
        poolsLoaded =
            lobbyBotPoolsLoadFromFile((char *)argv[bnArg], &poolStats);
        if (!poolsLoaded) {
          fprintf(stderr,
                  "Warning: -botnames '%s' could not be loaded; "
                  "using built-in bot names\n",
                  (char *)argv[bnArg]);
        }
      } else {
        poolsLoaded = lobbyBotPoolsLoadDefault(&poolStats);
      }
      if (poolsLoaded) {
        fprintf(stderr, "Loaded %d bot name pool(s)\n", poolStats.poolsKept);
      }
      /* The sim was made further up, before these pools were loaded, so
       * the catalogue copy it took then holds the built-in pools. Take it
       * again so joiners are offered the pools loaded here. */
      serverSimRefreshBotPools(serverSim);
    }
    /* If no -brain specified but AI is enabled, auto-discover a brain path
     * so that lobby "Add Bot" requests have a brain to use. */
    if (brainPath[0] == '\0' && ai != aiNone) {
      static const char *candidates[] = {
        "Brains/GoalHunter_1.7/init.lua",
        "brains/GoalHunter_1.7/init.lua",
        "data/Brains/GoalHunter_1.7/init.lua",
      };
      int c;
      for (c = 0; c < 3; c++) {
        FILE *f = fopen(candidates[c], "r");
        if (f) {
          fclose(f);
          strncpy(brainPath, candidates[c], MAX_PATH - 1);
          brainPath[MAX_PATH - 1] = '\0';
          fprintf(stderr, "Auto-discovered brain: %s\n", brainPath);
          break;
        }
      }
    }
  }

  /* The bot worker pool and the per-brain overrides, before the startup
     below. A startup that skips the lobby builds the seats a scenario's
     template asks for and starts the round inside itself, so the pool has
     to be up and the overrides in place before the first brain is made.
     None of this reads the sim — the pool init takes a thread count and
     the overrides are process-wide — so early costs nothing. The -bots
     loop that adds the operator's own bots still runs after the startup. */
  {
    int threadsArg = 0;
    int argNum = findArg(argc, argv, "threads");
    if (argNum != ARG_NOT_FOUND) {
      threadsArg = atoi((char *)argv[argNum]);
    }
    /* Determinism aids for A/B measurement runs. Neither has any effect
     * unless asked for, and both are honest about what they are: the tier pin
     * changes what the brain DOES, the Lua seed changes what it draws. Timing
     * telemetry keeps reporting real measured values either way. */
    {
      int argNum = findArg(argc, argv, "brain-tier");
      if (argNum != ARG_NOT_FOUND) {
        int tier = atoi((char *)argv[argNum]);
        if (tier < 1 || tier > 10) {
          fprintf(stderr, "-brain-tier must be 1..10 (got %d)\n", tier);
          return 0;
        }
        botManagerSetBrainTierOverride(tier);
        fprintf(stderr, "Brain capacity tier pinned to %d "
                        "(dynamic controller disabled)\n", tier);
      }
      argNum = findArg(argc, argv, "brain-lua-seed");
      if (argNum != ARG_NOT_FOUND) {
        long ls = strtol((char *)argv[argNum], NULL, 0);
        botManagerSetBrainLuaSeed(ls);
        fprintf(stderr, "Brain math.random seeded from %ld (+ player number)\n", ls);
      }
      /* NOTE: argExist prepends the "-" itself — passing the name with a
       * leading dash made it look for "--brain-no-budget-kill" and the flag
       * was silently dead (found 20260831: killbot.log full of 3ms kills in
       * runs that passed it). */
      if (argExist(argc, argv, "brain-no-budget-kill") == TRUE) {
        /* Reuses the slow-mo path: a 1000 ms budget, which no real tick
         * approaches, so the watchdog never truncates a think mid-computation
         * -- including the abort-flag polls inside the C pathfinder and
         * worldsim, which otherwise cut their results at a wall-clock-
         * dependent instruction. Still finite, so a genuinely hung brain is
         * aborted rather than hanging the server. */
        botManagerSetSlowMoDebug(1);
        fprintf(stderr, "Brain budget kill disabled (1000 ms per-bot budget); "
                        "consecutive-crash kick also suppressed\n");
      }
    }
    if (!botManagerInit(threadsArg)) {
      fprintf(stderr, "Error initializing bot manager\n");
#ifdef USING_SDL
      SDL_Quit();
#endif
      return 0;
    }
  }

  {
    ServerInstanceConfig instCfg;
    UploadPolicy uploadPolicy = UPLOAD_POLICY_ALLOW;
    uint8_t      uploadMaxFiles = 0;        /* 0 = leave transport default */
    uint32_t     uploadMaxStorageBytes = 0; /* 0 = leave transport default */
    const char  *uploadDir = NULL;          /* NULL = <map root>/Uploads */
    uint8_t     scriptUploadMaxFiles = 0;        /* 0 = leave transport default */
    uint32_t     scriptUploadMaxStorageBytes = 0; /* 0 = leave transport default */
    const char  *scriptUploadDir = NULL;

    {
      int policyArg = findArg(argc, argv, "uploadpolicy");
      if (policyArg != ARG_NOT_FOUND) {
        char policyStr[32];
        strncpy(policyStr, (char *)argv[policyArg], sizeof(policyStr) - 1);
        policyStr[sizeof(policyStr) - 1] = '\0';
        strlower(policyStr);
        if (strcmp(policyStr, "off") == 0) {
          uploadPolicy = UPLOAD_POLICY_OFF;
        } else if (strcmp(policyStr, "allow") == 0) {
          uploadPolicy = UPLOAD_POLICY_ALLOW;
        } else if (strcmp(policyStr, "persist") == 0) {
          uploadPolicy = UPLOAD_POLICY_PERSIST;
        } else {
          fprintf(stderr, "Unknown -uploadpolicy '%s'; using allow\n", policyStr);
          uploadPolicy = UPLOAD_POLICY_ALLOW;
        }
      }
    }
    {
      int filesArg = findArg(argc, argv, "uploadmaxfiles");
      if (filesArg != ARG_NOT_FOUND) {
        int v = atoi((char *)argv[filesArg]);
        if (v < 1) {
          fprintf(stderr, "-uploadmaxfiles %d out of range; clamping to 1\n", v);
          v = 1;
        } else if (v > 255) {
          fprintf(stderr, "-uploadmaxfiles %d out of range; clamping to 255\n", v);
          v = 255;
        }
        uploadMaxFiles = (uint8_t)v;
      }
    }
    {
      /* 4095, not 4096: the cap is held as bytes in a uint32_t, and 4096 MB
         is 2^32, which wraps to 0 and would read as "keep the default". */
      int storageArg = findArg(argc, argv, "uploadmaxstorage");
      if (storageArg != ARG_NOT_FOUND) {
        int v = atoi((char *)argv[storageArg]);
        if (v < 1) {
          fprintf(stderr, "-uploadmaxstorage %d out of range; clamping to 1\n", v);
          v = 1;
        } else if (v > 4095) {
          fprintf(stderr, "-uploadmaxstorage %d out of range; clamping to 4095\n", v);
          v = 4095;
        }
        uploadMaxStorageBytes = (uint32_t)v * 1024u * 1024u;
      }
    }
    {
      int dirArg = findArg(argc, argv, "uploaddir");
      if (dirArg != ARG_NOT_FOUND) {
        uploadDir = (const char *)argv[dirArg];
      }
    }
    {
      int dirArg = findArg(argc, argv, "scriptuploaddir");
      if (dirArg != ARG_NOT_FOUND) {
        scriptUploadDir = (const char *)argv[dirArg];
      }
    }
    {
      int filesArg = findArg(argc, argv, "scriptuploadmaxfiles");
      if (filesArg != ARG_NOT_FOUND) {
        int v = atoi((char *)argv[filesArg]);
        if (v < 1) {
          fprintf(stderr, "-scriptuploadmaxfiles %d out of range; clamping to 1\n", v);
          v = 1;
        } else if (v > 255) {
          fprintf(stderr, "-scriptuploadmaxfiles %d out of range; clamping to 255\n", v);
          v = 255;
        }
        scriptUploadMaxFiles = (uint8_t)v;
      }
    }
    {
      /* 4095, not 4096: the cap is held as bytes in a uint32_t, and 4096 MB
         is 2^32, which wraps to 0 and would read as "keep the default". */
      int storageArg = findArg(argc, argv, "scriptuploadmaxstorage");
      if (storageArg != ARG_NOT_FOUND) {
        int v = atoi((char *)argv[storageArg]);
        if (v < 1) {
          fprintf(stderr, "-scriptuploadmaxstorage %d out of range; clamping to 1\n", v);
          v = 1;
        } else if (v > 4095) {
          fprintf(stderr, "-scriptuploadmaxstorage %d out of range; clamping to 4095\n", v);
          v = 4095;
        }
        scriptUploadMaxStorageBytes = (uint32_t)v * 1024u * 1024u;
      }
    }

    memset(&instCfg, 0, sizeof(instCfg));
    instCfg.udpPort             = port;
    instCfg.bindAddr            = useAddr;
    instCfg.password            = pass;
    instCfg.maxPlayers          = (BYTE)maxPlayers;
    instCfg.maxBots             = (BYTE)maxBots;
    instCfg.maxSpectators       = (BYTE)maxSpectators;
    instCfg.specDelaySeconds    = (uint16_t)specDelay;
    instCfg.acceptRemoteClients = TRUE;
    instCfg.useWbn              = (argExist(argc, argv, "nowinbolonet") == FALSE);
    instCfg.compTanks           = (BYTE)ai;
    instCfg.useTracker          = sTrackerUse;
    instCfg.trackerAddr         = sTrackerAddr;
    instCfg.trackerPort         = sTrackerPort;
    instCfg.uploadPolicy          = uploadPolicy;
    instCfg.uploadMaxFiles        = uploadMaxFiles;
    instCfg.uploadMaxStorageBytes = uploadMaxStorageBytes;
    instCfg.uploadPersistDir      = uploadDir;
    instCfg.scriptUploadPolicy          = scriptUploadPolicy;
    instCfg.scriptUploadMaxFiles        = scriptUploadMaxFiles;
    instCfg.scriptUploadMaxStorageBytes = scriptUploadMaxStorageBytes;
    instCfg.scriptUploadDir             = scriptUploadDir;
    instCfg.noScriptSharing             =
        (argExist(argc, argv, "noscriptsharing") == TRUE);
    instCfg.skipLobby           = skipLobby;
    instCfg.emptyResetEnabled   = emptyResetEnabled;
    instCfg.hasPassword         = (pass[0] != '\0');
    instCfg.botBrainPath        = (brainPath[0] != '\0') ? brainPath : NULL;
    instCfg.botAiType           = (BYTE)ai;
    instCfg.autoLockOnGameStart = autoLockOnGameStart;
    instCfg.ranked              = ranked;
    instCfg.openHost            = openHost;
    instCfg.serverLocks         = serverLocks;
    {
      bool natPunchOptOut = (argExist(argc, argv, "no-natpunch") == TRUE);
      instCfg.useNatPortmap   = (argExist(argc, argv, "upnp") == TRUE);
      instCfg.useNatKeepalive = sTrackerUse && !natPunchOptOut;
    }
    /* LAN mDNS advertising is opt-in for dedicated servers (the memset
     * above leaves it false by default); -mdns turns it on. */
    instCfg.mdnsAdvertise = (argExist(argc, argv, "mdns") == TRUE);
    /* Voice is forwarded by default (the memset above leaves the mode
     * serverVoiceOn, so no switch at all needs no code here). -voice <mode>
     * names one of the three modes; an unknown word warns and leaves it on,
     * the same shape -pillview uses. serverVoiceProximity is not
     * implemented: a server set to it forwards voice exactly as "on" does. */
    {
      int voiceNum = findArg(argc, argv, "voice");
      if (voiceNum != ARG_NOT_FOUND) {
        char voiceStr[32];
        strncpy(voiceStr, (char *)argv[voiceNum], sizeof(voiceStr) - 1);
        voiceStr[sizeof(voiceStr) - 1] = '\0';
        strlower(voiceStr);
        if (strcmp(voiceStr, "off") == 0) {
          instCfg.voiceMode = serverVoiceOff;
        } else if (strcmp(voiceStr, "on") == 0) {
          instCfg.voiceMode = serverVoiceOn;
        } else if (strcmp(voiceStr, "proximity") == 0) {
          instCfg.voiceMode = serverVoiceProximity;
        } else {
          fprintf(stderr, "Unknown -voice '%s'; using on\n", voiceStr);
          instCfg.voiceMode = serverVoiceOn;
        }
      }
      /* -no-voice (either dash form) is the older spelling of -voice off.
       * Read after -voice so it wins when both are on the command line: it
       * is the narrower statement, and the one an existing script is most
       * likely to be carrying. */
      if (argExist(argc, argv, "no-voice") == TRUE ||
          argExist(argc, argv, "-no-voice") == TRUE) {
        instCfg.voiceMode = serverVoiceOff;
      }
    }
    /* A server that skips the lobby starts its round inside the startup
       below, so the scenario's own settings have to be in force before it:
       the round is built and the first tanks placed in there, and a game
       type set afterwards would never be asked for. A lobby server takes
       them further down instead, with the seating, after the startup has
       snapshotted the operator's own settings for the empty-lobby reset. */
    if (scenarioHost != NULL && skipLobby) {
      serverSimScenarioApplyLobbyRules(serverSim);
    }
    if (serverInstanceStartup(serverSim, &instCfg) == FALSE) {
      fprintf(stderr, "Error creating network transport\n");
      scenarioHostDetach(scenarioHost);
      serverSimDestroy(serverSim);
#ifdef USING_SDL
      SDL_Quit();
#endif
      return 0;
    }

#if WB_ENABLE_NETIMPAIR
    {
      int impairArg = findArg(argc, argv, "netimpair");
      if (impairArg != ARG_NOT_FOUND && argv[impairArg][0] != '-') {
        transportUdpServerSetNetImpair((char *)argv[impairArg]);
      }
    }
#endif
  }
  bool dontSendLog = argExist(argc, argv, "dontsendlog");

  /* Log file recording — configure the sim's wantLogging /
   * userLogFileName state, then register the dedicated-server log
   * subscriber. The subscriber's sync-replay opens the log on the
   * current phase (LOBBY for lobby mode, RUNNING for no-lobby). */
  if (argExist(argc, argv, "log") == TRUE) {
    int logArg = findArg(argc, argv, "log");
    char userLogFile[MAX_PATH] = {0};
    if (logArg != ARG_NOT_FOUND && argv[logArg][0] != '-') {
      strncpy(userLogFile, (char *)argv[logArg], MAX_PATH - 1);
    }
    serverSimSetWantLogging(serverSim, true);
    if (userLogFile[0] != '\0') {
      serverSimSetUserLogFileName(serverSim, userLogFile);
    }
    serverDedicatedLogInstall(serverSim, dontSendLog);
    /* -servelog on|off overrides the round-log serve policy. Left out, the
     * mode the install just reset to decides: serve unless winbolo.net is
     * running. Set after the install, which clears it. */
    {
      int serveArg = findArg(argc, argv, "servelog");
      if (serveArg != ARG_NOT_FOUND && argv[serveArg][0] != '-') {
        char serveVal[16] = {0};
        strncpy(serveVal, (char *)argv[serveArg], sizeof(serveVal) - 1);
        strlower(serveVal);
        if (strcmp(serveVal, "on") == 0) {
          serverDedicatedLogSetServeMode(ROUND_LOG_SERVE_ON);
        } else if (strcmp(serveVal, "off") == 0) {
          serverDedicatedLogSetServeMode(ROUND_LOG_SERVE_OFF);
        } else {
          fprintf(stderr,
                  "-servelog: expected \"on\" or \"off\", got \"%s\" - using the default\n",
                  serveVal);
        }
      }
    }
  }


  /* The lobby the scenario asks for. Its template reached the sim at the
     attach far above; seating it is the separate step made wherever a lobby
     is built, and a server booting on this map is one of those points — the
     two callers inside the sim are a map being committed and a lobby
     resetting once the last player leaves, and a fresh boot is neither.

     Here rather than beside the attach because a team the template fields
     with no brain of its own falls back to the server's, and that path and
     the bot AI level are written into the sim by the startup above.

     Before the operator's -bots, which take the seats above these.

     The headless makes the same call after its own player has joined: that
     binary plays as well as hosts and its player has to hold slot 0, which
     bots seated first would take. Nobody plays from here — every
     participant joins over the wire — so there is no slot to keep back.

     Not on a server that skipped the lobby: its round started inside the
     startup above, and the startup seated the template itself on the way in
     so the round could build a tank for every fielded seat. Seating again
     now would empty those seats and rebuild them inside a round already
     running, leaving the scenario's own bots with no tanks.

     A map with no scenario has no template and this seats nothing. */
  /* And the lobby's own settings, in the order a map commit does the two:
     the map change seats the template and the rules follow it. Without this
     a server booted onto a scripted map stays on the operator's game type,
     so gameTypeResolve is never asked and the game the scenario declares is
     ignored for the whole run. A server that skipped the lobby has already
     had them applied, above the startup where its round begins; the call
     here then finds the lobby on the scripted type already and changes
     nothing. */
  if (scenarioHost != NULL) {
    if (!skipLobby) {
      serverSimScenarioSeatLobby(serverSim);
    }
    serverSimScenarioApplyLobbyRules(serverSim);
  }

  /* botBrainPath + botAiType were already pushed into the sim via the
   * cfg block above; the loop below only needs to spawn the configured
   * bot count (numBots / brainPath resolved earlier). */
  {
    /* Runs whenever a brain is available — NOT gated on -bots N. The debug/
     * profile flag handling inside must also cover servers whose bots come
     * from elsewhere (hosts adding lobby bots): -brain-debug used to be
     * silently ignored without -bots. With numBots == 0 the spawn/team
     * loops below are natural no-ops. */
    if (brainPath[0] != '\0') {
      int i;
      /* Brain debug / profiling flags. Canonical names are -brain-debug and
       * -brain-profile-log; -braindebug and -profile-log are kept as legacy
       * aliases. Computed up front because both imply the unsafe sandbox
       * opt-out below.
       *
       * Headless server: only -brain-profile-log (file output) is meaningful.
       * There's no -profile flag here — the in-memory Y-panel it drives is a
       * BrainTest windowed feature, not something a dedicated server uses. */
      bool profileLog = (argExist(argc, argv, "brain-profile-log") == TRUE)
                     || (argExist(argc, argv, "-brain-profile-log") == TRUE)
                     || (argExist(argc, argv, "profile-log") == TRUE)
                     || (argExist(argc, argv, "-profile-log") == TRUE);
      bool brainDebug = (argExist(argc, argv, "brain-debug") == TRUE)
                     || (argExist(argc, argv, "-brain-debug") == TRUE)
                     || (argExist(argc, argv, "braindebug") == TRUE)
                     || (argExist(argc, argv, "-braindebug") == TRUE);
      /* Brains run in the restricted Lua sandbox by default. -allow-unsafe-brains
       * opens the full standard library for trusted brain authors. -brain-debug
       * and -brain-profile-log also need it: they direct the brain to write its
       * print2_bot<N>.log / optimize.log / performance.ticks.log into
       * debug_sessions/, which lives outside the brain directory the sandbox
       * jails io.open to — without the opt-out those writes are rejected and the
       * logs never appear. Set BEFORE the bots are created so each VM constructs
       * with the chosen policy. */
      bool allowUnsafeExplicit = (argExist(argc, argv, "allow-unsafe-brains") == TRUE)
                              || (argExist(argc, argv, "-allow-unsafe-brains") == TRUE);
      luaBrainsSetAllowUnsafe(allowUnsafeExplicit || profileLog || brainDebug);
      /* Make the implied sandbox opt-out loud: an operator who passed only a
       * debug/profile flag (not -allow-unsafe-brains itself) has just had the
       * brain sandbox turned OFF as a side effect, so say so explicitly. */
      if (!allowUnsafeExplicit && (profileLog || brainDebug)) {
        fprintf(stderr,
                "Note: %s implies -allow-unsafe-brains — the Lua brain sandbox "
                "is now OFF (brains get the full standard library, needed to "
                "write logs into debug_sessions/). Only run trusted brains.\n",
                profileLog ? "-brain-profile-log" : "-brain-debug");
      }
      /* -brain-debug: turn BRAIN_DEBUG_MODE on for every bot (set BEFORE they're
       * created so each brain constructs with debug on → un-stripped brain +
       * print2 logging). Lets you audit bot comms on a dedicated server: each
       * bot writes print2_bot<N>.log (grep MSG_TX for outbound /info traffic,
       * SYNC_P6 / process_message for what it received).
       *
       * Recording is ARMED here, but the debug_sessions/<TS>/ dir, file open,
       * and per-bot DEBUG_SESSION_DIR publish are deferred to each game's first
       * running tick (server_lifecycle.c) — so lobby time never enters the
       * timeline and every game gets its own fresh, tick-0-anchored dir.
       * BrainTest detects a loadable winbolods session by brainrec.btr. */
      if (profileLog) {
        /* Profile the PRODUCTION brain. luaBrainsSetProfile is the SAME shared
         * setter BrainTest uses (luabrainshandler.c, reached via bot_manager),
         * so every brain we create sets BRAIN_PROFILE / BRAIN_PROFILE_LOG and
         * writes optimize.log + performance.ticks.log into DEBUG_SESSION_DIR.
         * Arm recording so server_lifecycle creates that session dir + a
         * brainrec.btr, giving BrainTest a loadable session with the profile
         * files beside it. Debug stays OFF and we force the opt/ brain so the
         * timings reflect what actually ships (the base brain carries print2/
         * viz overhead). */
        /* pool_viz=0: a dedicated-server profiling run wants profile data
         * ONLY — no print2 (debug stays off) and no pool-breakdown capture.
         * Leaving pool-viz off keeps the pool-string GC cost out of the
         * timings and the pool bytes out of the .btr. */
        luaBrainsSetProfile(1, 1, 0);
        brainRecordSetEnabled(true);
        {
          char optPath[MAX_PATH];
          strncpy(optPath, brainPath, MAX_PATH - 1);
          optPath[MAX_PATH - 1] = '\0';
          if (!strstr(optPath, "opt/") && !strstr(optPath, "opt\\")) {
            char *sep = strrchr(optPath, '/');
            char *bs  = strrchr(optPath, '\\');
            if (bs && (!sep || bs > sep)) sep = bs;
            if (sep && strlen(optPath) + 5 <= (size_t)MAX_PATH) {
              size_t taillen = strlen(sep + 1);
              memmove(sep + 5, sep + 1, taillen + 1);  /* +1 keeps the NUL */
              memcpy(sep + 1, "opt/", 4);
            }
          }
          FILE *tf = fopen(optPath, "r");
          if (tf) {
            fclose(tf);
            strncpy(brainPath, optPath, MAX_PATH - 1);
            brainPath[MAX_PATH - 1] = '\0';
            fprintf(stderr, "-brain-profile-log: profiling opt/ brain '%s'\n", brainPath);
          } else {
            fprintf(stderr, "-brain-profile-log: opt brain '%s' not found — profiling "
                            "base brain '%s' (timings include debug/print2 "
                            "overhead)\n", optPath, brainPath);
          }
        }
        fprintf(stderr, "Profiling ON: BRAIN_PROFILE + optimize.log/"
                        "performance.ticks.log in debug_sessions/<TS>_<N>/; "
                        "brainrec.btr recorded for BrainTest.\n");
      } else if (brainDebug) {
        serverSimSetBotDefaultDebugMode(serverSim, true);
        /* Selective debug streams: -brain-debug implies ALL of viz recording,
         * pool-breakdown capture, print2 logs and jsonl traces. Each can be
         * turned off individually to keep a many-bot debug game playable —
         * the streams cost real tick time (pool JSON is a per-bot per-tick
         * Lua serialization; viz is thousands of overlay calls; print2 is
         * debug.getinfo + string building per line). */
        {
          bool bdNoViz    = (argExist(argc, argv, "bd-noviz") == TRUE)
                         || (argExist(argc, argv, "-bd-noviz") == TRUE);
          bool bdNoPool   = (argExist(argc, argv, "bd-nopool") == TRUE)
                         || (argExist(argc, argv, "-bd-nopool") == TRUE);
          bool bdPoolRR   = (argExist(argc, argv, "bd-pool-rr") == TRUE)
                         || (argExist(argc, argv, "-bd-pool-rr") == TRUE);
          bool bdNoPrint2 = (argExist(argc, argv, "bd-noprint2") == TRUE)
                         || (argExist(argc, argv, "-bd-noprint2") == TRUE);
          bool bdNoJsonl  = (argExist(argc, argv, "bd-nojsonl") == TRUE)
                         || (argExist(argc, argv, "-bd-nojsonl") == TRUE);
          /* Pool default is FULL (every bot every tick — exact panel
           * history). The JSON builds run in parallel on each bot's own
           * worker right after its think (see brainRecordPoolCaptureWanted),
           * so full capture holds 50 Hz even at 12 bots; the serial recorder
           * section only writes the prefetched bytes. -bd-pool-rr opts into
           * round-robin (one bot per tick) when session SIZE matters —
           * ~1/botCount the bytes (~0.5 MB/tick vs ~40 KB/tick at 12 bots),
           * each bot's panel data at most botCount frames stale. */
          int poolMode = bdNoPool ? 0 : (bdPoolRR ? 1 : 2);
          luaBrainsSetDebugParts(!bdNoPrint2, !bdNoPool, !bdNoViz, !bdNoJsonl);
          brainRecordSetParts(!bdNoViz, poolMode);
          fprintf(stderr, "-brain-debug streams: viz=%s pool=%s print2=%s jsonl=%s\n",
                  bdNoViz ? "OFF" : "on",
                  bdNoPool ? "OFF" : (bdPoolRR ? "round-robin (1 bot/tick)"
                                               : "full (every bot every tick)"),
                  bdNoPrint2 ? "OFF" : "on", bdNoJsonl ? "OFF" : "on");
        }
        /* print2 is stripped from the opt/ brain SOURCE, so running an opt/
         * -brain path under -braindebug yields brainrec.btr but zero
         * print2_botN.log — the exact issue the usage text warns about.
         * Auto-redirect an "opt/" (or "opt\") path segment to the base path
         * so the per-bot debug logs always appear in -braindebug. */
        {
          char *optSeg = strstr(brainPath, "opt/");
          if (!optSeg) optSeg = strstr(brainPath, "opt\\");
          if (optSeg && (optSeg == brainPath ||
                         optSeg[-1] == '/' || optSeg[-1] == '\\')) {
            /* Splice out the 4-char "opt/" (or "opt\") segment in place. */
            memmove(optSeg, optSeg + 4, strlen(optSeg + 4) + 1);
            fprintf(stderr, "-brain-debug: redirected opt/ brain to base path "
                            "'%s' (print2 is stripped from opt/)\n", brainPath);
          }
        }
        brainRecordSetEnabled(true);   /* arm; dir + file open at game start */
        fprintf(stderr, "Bot brain debug mode ON (print2 + brainrec.btr; a fresh "
                        "debug_sessions/<TS>_<N>/ per game, rolled into 15-min "
                        "blocks (_1,_2,...). Use a base -brain path, not opt/, so "
                        "print2 isn't stripped)\n");
      }
      int allyTeam = 0;  /* 0 = no allying; 1-16 = team to place bots on */
      if (argExist(argc, argv, "allybots") == TRUE) {
        int aArg = findArg(argc, argv, "allybots");
        allyTeam = 1;
        if (aArg != ARG_NOT_FOUND && argv[aArg][0] != '-') {
          int t = atoi((char *)argv[aArg]);
          if (t >= 1 && t <= 16) {
            allyTeam = t;
          } else {
            fprintf(stderr, "Warning: -allybots team must be 1-16, defaulting to 1\n");
          }
        }
      }
      /* -teams (copied from BrainTest): "-teams 4,5,6" assigns bots to teams of
       * those sizes in contiguous blocks (first 4 bots -> team 1, next 5 -> team
       * 2, next 6 -> team 3); "-teams N" (no comma) splits bots round-robin into
       * N teams. Bots beyond the listed sizes stay FFA. Takes precedence over
       * -allybots when both are given. Assignment + alliance reapply happen after
       * the bot-add loop below. */
      int teamSizes[MAX_TANKS] = { 0 };
      int numTeamSizes = 0;
      int numTeams = 0;
      if (argExist(argc, argv, "teams") == TRUE) {
        int tArg = findArg(argc, argv, "teams");
        if (tArg != ARG_NOT_FOUND && argv[tArg][0] != '-') {
          const char *tv = (const char *)argv[tArg];
          if (strchr(tv, ',') != NULL) {
            const char *p = tv;
            while (*p && numTeamSizes < MAX_TANKS) {
              int sz = atoi(p);
              if (sz < 1) {
                fprintf(stderr, "Warning: -teams: each team size must be >= 1 (got '%s'); ignoring -teams\n", tv);
                numTeamSizes = 0;
                break;
              }
              teamSizes[numTeamSizes++] = sz;
              const char *comma = strchr(p, ',');
              if (!comma) break;
              p = comma + 1;
            }
            numTeams = numTeamSizes;
          } else {
            numTeams = atoi(tv);
            if (numTeams < 0) numTeams = 0;
            if (numTeams > MAX_TANKS) numTeams = MAX_TANKS;
          }
        } else {
          fprintf(stderr, "Warning: -teams given with no value; ignoring\n");
        }
      }
      if (numTeams > 0 && allyTeam > 0) {
        fprintf(stderr, "Warning: -teams overrides -allybots\n");
        allyTeam = 0;
      }
      /* -mode / -difficulty: the mode and difficulty every -bots bot is
       * created with, as INDICES into the brain's own modes.txt manifest.
       * Both land in the slot's LobbyBotConfig before the brain is created,
       * so the brain sees them as "mode=<key>;difficulty=<key>"
       * BRAIN_INIT_ARG tokens. A brain with no manifest gets the
       * synthesized default mode with easy/medium/hard, so leaving both
       * flags off is the pre-manifest "hard" exactly as before. */
      BrainModes botModes;
      brainListLoadModesForPath(brainPath, &botModes);
      uint8_t botMode = 0;
      if (argExist(argc, argv, "mode") == TRUE) {
        int mArg = findArg(argc, argv, "mode");
        if (mArg != ARG_NOT_FOUND && argv[mArg][0] != '-') {
          int found = brainModesFindMode(&botModes, (const char *)argv[mArg]);
          if (found < 0) {
            fprintf(stderr, "Warning: -mode '%s' is not a mode this brain declares; using '%s'\n",
                    (const char *)argv[mArg], botModes.modes[0].key);
          } else {
            botMode = (uint8_t)found;
          }
        } else {
          fprintf(stderr, "Warning: -mode given with no value; using '%s'\n",
                  botModes.modes[0].key);
        }
      }
      /* The mode's own default level — "hard" for the default mode, which
       * is what a lobby bot left alone gets. */
      uint8_t botDifficulty = (uint8_t)botModes.modes[botMode].defaultLevel;
      if (argExist(argc, argv, "difficulty") == TRUE) {
        int dArg = findArg(argc, argv, "difficulty");
        if (dArg != ARG_NOT_FOUND && argv[dArg][0] != '-') {
          /* A level key of the selected mode first; then the frozen
           * easy/medium/hard words, so an old command line keeps working
           * against a mode that happens to name its levels differently. */
          int lvl = brainModeFindLevel(&botModes.modes[botMode],
                                       (const char *)argv[dArg]);
          uint8_t legacy = 0;
          if (lvl >= 0) {
            botDifficulty = (uint8_t)lvl;
          } else if (botDifficultyFromName((const char *)argv[dArg], &legacy) &&
                     legacy < (uint8_t)botModes.modes[botMode].levelCount) {
            botDifficulty = legacy;
          } else {
            fprintf(stderr, "Warning: -difficulty '%s' is not a level of mode '%s'; using '%s'\n",
                    (const char *)argv[dArg], botModes.modes[botMode].key,
                    botModes.modes[botMode].levels[botDifficulty].key);
          }
        } else {
          fprintf(stderr, "Warning: -difficulty given with no value; using '%s'\n",
                  botModes.modes[botMode].levels[botDifficulty].key);
        }
      }
      /* -bot-init: per-player-id brain/init.lua paths (+ optional [arg]). Every
       * id defaults to the shared brainPath with an empty init table; the spec
       * overrides the ids it names. Shared parser/semantics with BrainTest. */
      BotInitSlot botInit[MAX_TANKS];
      for (i = 0; i < MAX_TANKS; i++) {
        snprintf(botInit[i].path, sizeof(botInit[i].path), "%s", brainPath);
        scnTableClear(&botInit[i].init);
        botInit[i].covered = 0;
      }
      if (argExist(argc, argv, "bot-init") == TRUE) {
        int biArg = findArg(argc, argv, "bot-init");
        if (biArg != ARG_NOT_FOUND && argv[biArg][0] != '-') {
          if (!luaBrainsParseBotInitSpec((char *)argv[biArg], botInit, MAX_TANKS)) {
            fprintf(stderr, "Warning: -bot-init spec rejected; using -brain '%s' for all bots\n",
                    brainPath);
          }
        } else {
          fprintf(stderr, "Warning: -bot-init given with no value; ignoring\n");
        }
      }
      /* Draw themed names from one randomly-chosen pool so a -bots
       * server gets varied names instead of "Bot 1..N". The name draws are
       * wrapped in a bolo_rand save/restore so this cosmetic randomness leaves
       * the deterministic game stream (tank placement, etc.) untouched for a
       * given -seed — only the game sim should advance the shared PRNG. Names
       * are picked up front, then the bots are added with them. usedStore
       * backs the uniqueness list handed to lobbyBotPoolPick. (numBots is
       * clamped to MAX_TANKS above, so botNames is always in bounds.) */
      char botNames[MAX_TANKS][64];
      {
        static char usedStore[MAX_TANKS][64];
        const char *usedNames[MAX_TANKS];
        int usedCount = 0;
        BoloRandState rngBeforeNaming;
        int botPool;
        bolo_rand_save(&rngBeforeNaming);
        botPool = (int)bolo_rand_below((uint32_t)lobbyBotPoolCount());
        for (i = 0; i < numBots; i++) {
          char picked[64];
          lobbyBotPoolPick(botPool, usedNames, usedCount, picked, sizeof(picked));
          if (picked[0] != '\0') {
            snprintf(botNames[i], sizeof(botNames[i]), "%s", picked);
          } else {
            snprintf(botNames[i], sizeof(botNames[i]), "Bot %d", i + 1);
          }
          if (usedCount < MAX_TANKS) {
            snprintf(usedStore[usedCount], sizeof(usedStore[usedCount]),
                     "%s", botNames[i]);
            usedNames[usedCount] = usedStore[usedCount];
            usedCount++;
          }
        }
        bolo_rand_restore(&rngBeforeNaming);
      }
      for (i = 0; i < numBots; i++) {
        /* This bot's init table and resolved brain path go down the create
         * call, so each bot gets its own. The name was pre-picked into
         * botNames[i] above (under a bolo_rand save/restore so it stays off
         * the sim PRNG). */
        if (botInit[i].covered) {
          char initText[256];
          scnTableFormat(&botInit[i].init, initText, sizeof(initText));
          fprintf(stderr, "Bot %d: -bot-init brain '%s'%s%s\n", i, botInit[i].path,
                  initText[0] ? " init=" : "", initText);
        }
        /* Mode and difficulty have to be in the slot's config BEFORE the
         * brain is created: botManagerAddBot reads them from there to build
         * the mode= / difficulty= tokens it appends to the staged arg. */
        serverSimSetBotConfig(serverSim, (BYTE)i, botMode, botDifficulty,
                              0 /* personality: normal */, NULL);
        /* No team in the add: -allybots and -teams place these bots through
         * serverSimSetTeamBatch below, once the whole set is in. */
        if (!botManagerAddBot(serverSim, (BYTE)i, botInit[i].path, botNames[i], ai, game,
                              hiddenMines, 0, &botInit[i].init)) {
          fprintf(stderr, "Warning: failed to add bot %d\n", i);
        } else if (allyTeam > 0) {
          /* Shared non-zero team for every bot — server_sim's start-of-round
           * pass converts matching teamNumber into alliances, and the lobby
           * protocol already broadcasts teamNumber to clients so the lobby
           * UI shows the bots on this team. */
          serverSimSetTeamBatch(serverSim, (BYTE)i, (uint8_t)allyTeam);
        }
      }
      if (numBots <= 0) {
        /* No -bots requested — this pass only processed the debug/profile
         * flags above; nothing was spawned, so skip the summary prints. */
      } else if (numTeamSizes > 0) {
        /* Explicit per-team sizes: contiguous blocks. First teamSizes[0] bots
         * -> team 1, next teamSizes[1] -> team 2, etc. Bots past the listed
         * total stay on team 0 (FFA). */
        int bot = 0;
        for (int t = 0; t < numTeamSizes; t++) {
          for (int k = 0; k < teamSizes[t] && bot < numBots; k++) {
            serverSimSetTeamBatch(serverSim, (BYTE)bot, (BYTE)(t + 1));
            bot++;
          }
        }
        serverSimReapplyTeamAlliances(serverSim);
        fprintf(stderr, "Added %d bot(s) with brain '%s' (teams, sizes",
                numBots, brainPath);
        for (int t = 0; t < numTeamSizes; t++)
          fprintf(stderr, "%s%d", t ? "," : " ", teamSizes[t]);
        fprintf(stderr, ")\n");
      } else if (numTeams >= 2) {
        for (i = 0; i < numBots; i++) {
          serverSimSetTeamBatch(serverSim, (BYTE)i, (BYTE)((i % numTeams) + 1));
        }
        serverSimReapplyTeamAlliances(serverSim);
        fprintf(stderr, "Added %d bot(s) with brain '%s' (%d teams, round-robin)\n",
                numBots, brainPath, numTeams);
      } else if (allyTeam > 0) {
        serverSimReapplyTeamAlliances(serverSim);
        fprintf(stderr, "Added %d bot(s) with brain '%s' (allied on team %d)\n",
                numBots, brainPath, allyTeam);
      } else {
        fprintf(stderr, "Added %d bot(s) with brain '%s'\n", numBots, brainPath);
      }
      /* Each bot reserved a lobby start as it was added, on team 0 and so
       * with no side; -allybots and -teams then moved it with the batch
       * setter, which leaves that reservation where it was. Pick again for
       * the teams the bots are on now, or a team with a side starts a bot
       * on the other team's. Only a lobby server has reservations: a
       * -nolobby round is already running here and this does nothing. */
      serverSimRepickAllLobbyStarts(serverSim);
    } else if (numBots > 0) {
      fprintf(stderr, "Warning: -bots specified but no -brain path given\n");
    }
  }

  if (threadsCreate(TRUE) == FALSE) {
    fprintf(stderr, "Error starting Thread Manager\n");
    threadsDestroy();
    serverInstanceShutdown(serverSim);
    scenarioHostDetach(scenarioHost);
    serverSimDestroy(serverSim);
#ifdef USING_SDL
    SDL_Quit();
#endif
    return 0;
  }
  serverMessageConsoleMessage(serverSim,"Type \"help\" for help, \"quit\" to exit.");
  /* Created before the timer starts so serverGameTimer always sees a valid
   * lock; serverQuiesceGameTimer drains the timer through it on shutdown. */
  g_serverTickLock = SDL_CreateMutex();
  oldTick = SDL_GetTicks();
  if (isAsap) {
    fprintf(stderr, "ASAP mode: ticks run back-to-back\n");
    serverAsapThread = SDL_CreateThread(serverAsapLoop, "wb-asap-tick", NULL);
    if (serverAsapThread == NULL) {
      fprintf(stderr, "Error: -asap could not start the tick thread\n");
      return 0;
    }
  } else {
#ifdef _WIN32
    serverTimerGameID = timeSetEvent(SERVER_TICK_LENGTH, 10, serverGameTimer, 0, TIME_PERIODIC);
#else
    serverTimerGameID = SDL_AddTimer(SERVER_TICK_LENGTH, serverGameTimer, NULL);
#endif
  }

  {
    CmdStdin *cmdStream = NULL;
    int cmdArg = findArg(argc, argv, "cmd-stdin");
    if (cmdArg != ARG_NOT_FOUND) {
      cmdStream = cmdStdinOpen((char *)argv[cmdArg]);
      if (cmdStream == NULL) {
        fprintf(stderr, "Error: failed to open -cmd-stdin file '%s'\n",
                (char *)argv[cmdArg]);
        serverQuiesceGameTimer();
        botWorkerPoolDestroy();
        threadsDestroy();
        serverInstanceShutdown(serverSim);
        scenarioHostDetach(scenarioHost);
        serverSimDestroy(serverSim);
#ifdef USING_SDL
        SDL_Quit();
#endif
        return 0;
      }
    }
    if (cmdStream != NULL) {
      processCmdStdin(cmdStream);
      cmdStdinClose(cmdStream);
    } else {
      processKeys(isQuiet);
    }
  }

  /* Drain the game-tick timer first: blocks until no serverInstanceTick is
   * (or can be) running, so the brain-think workers are provably idle before
   * anything they touch is freed. Then tear the worker pool down before the
   * lua_States it dispatches into are closed (defence in depth). */
  serverQuiesceGameTimer();

  /* -finaljson: dump the final global game state once the game ended by
   * reaching a terminal game-over (what -ticks produces). Gated on the
   * terminal state so a plain SIGINT / operator quit stays silent. The
   * timer is drained above, so the sim is quiescent and still fully
   * populated here (destroy happens further down). */
  if (optFinalJson[0] != '\0' && serverSimIsTerminalGameOver(serverSim)) {
    serverEmitFinalJson(serverSim, optFinalJson, "tick_limit", FALSE);
  }
  /* -snapjson: close the series with the true final state, so the last row
   * is the terminal one rather than the last interval boundary. Same
   * terminal-only gate as -finaljson. */
  if (optSnapJson[0] != '\0' && serverSimIsTerminalGameOver(serverSim)) {
    serverEmitFinalJson(serverSim, optSnapJson, "final", TRUE);
  }
  botWorkerPoolDestroy();
  brainRecordShutdown();   /* flush + close brainrec.btr (no-op if not recording) */
  threadsDestroy();

  if (serverDedicatedLogIsActive() == TRUE && winbolonetIsRunning() == TRUE && argExist(argc, argv, "dontsendlog") == FALSE) {
    winboloNetGetServerKey(key);
  } else {
    key[0] = EMPTY_CHAR;
  }

  serverInstanceShutdown(serverSim);

  if (serverDedicatedLogIsActive() == TRUE) {
    logStop(); /* Finalize zip — must run regardless of WBN upload */
  }
  if (serverDedicatedLogIsActive() == TRUE && key[0] != EMPTY_CHAR && argExist(argc, argv, "dontsendlog") == FALSE) {
    serverMessageConsoleMessage(serverSim,(char *)"Uploading log file to winbolo.net");
    httpCreate();
    httpSendLogFile(serverDedicatedLogCurrentFile(), key, FALSE);
    httpDestroy();
  }
  geoLookupDestroy();
  serverSimMapDirDestroy(serverSim);
  scenarioHostDetach(scenarioHost);
  serverSimDestroy(serverSim);
  if (g_serverTickLock != NULL) {
    SDL_DestroyMutex(g_serverTickLock);
    g_serverTickLock = NULL;
  }
#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}

time_t serverMainGetTicks() {
  return ticks;
}
