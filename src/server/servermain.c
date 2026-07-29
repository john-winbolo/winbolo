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
#include "mapgen.h"
#include "log.h"
#include "transport_udp.h"
#include "net_impair.h"   /* WB_ENABLE_NETIMPAIR master switch */
#include "bot_manager.h"
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
#include "wire_limits.h"
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
unsigned int serverTimerGameID = 1;
char fileName[MAX_PATH]; /* Log file Name */
bool isLogging = FALSE;
bool dontSendLog = FALSE;

bool statusFile = FALSE;

time_t ticks = 0;

static ServerSim *serverSim = NULL;

/* -finaljson destination: "" = disabled, "-" = stdout, else a file path.
 * When set, a single global game-state snapshot is written once the game
 * reaches a terminal game-over (as -ticks produces). See serverEmitFinalJson. */
static char optFinalJson[512] = "";

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


void strlower(char *s) {
  while(*s) {
    *s = tolower(*s);
    s++;
  }
}

void saveMap(char *line) {
  char *ptr;
  int len;
  ptr = line;
  ptr += 7;

  /* Strip newline */
  len = (int) strlen(line);
  if (line[len-1] == '\n') {
    line[len-1] = '\0';
  }

  while (*ptr != EMPTY_CHAR  && (*ptr == '\t' || *ptr == ' ')) {
    ptr++;
  }
  if (*ptr == EMPTY_CHAR) {
    fprintf(stderr, "Sorry, you must enter a filename for this command\n");
  } else {
    len = (int) strlen(ptr);
    {
      size_t remaining = 256 - (size_t)(ptr - line) - (size_t)len - 1;
      if (len < 4) {
        strncat(ptr, ".map", remaining);
      } else if (strcmp(ptr+len-4, ".map") != 0) {
        strncat(ptr, ".map", remaining);
      }
    }
    transportUdpServerSendServerMessage("Server Admin saved map file.");
    if (serverSimSaveMap(serverSim, ptr) == FALSE) {
      fprintf(stderr, "Sorry, an error occured saving the map. Is the path correct?\n");
    } else {
      logAddEvent(log_SaveMap, 0, 0, 0, 0, 0, NULL);
    }
  }
}

void printHelp() {
  fprintf(stderr, "Help:\n Lock - Locks the server and stops new players from joining.\n Unlock - Unlocks the server and allows new players to join.\n savemap <map file> - Save the map file to path and file <map file>\n Say <text> - Sends this message to all players in the game unless they have turned off server messages.\n Quit - Exits the server.\n Info - Provide information about the current game\n Kick - Kicks a player. Case insensitive, prefix a * for WBN players.\n Host - Transfers the host role to a player. Case insensitive.\n Status - Returns list of players who aren't locked.\n");
}


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
	char playerKick[33] = "\0";
	char playerHost[33] = "\0";
	size_t newbuflen;

	if (isQuiet == TRUE || isNoInput == TRUE) {
		while (!serverSimIsTerminalGameOver(serverSim)) {
			if (alarmRaised == alarmInterrupt) {
				break;
			}
			Sleep(1000);
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

				if (strncmp(keyBuff, "help", 4) == 0) {
					printHelp();
				} else if (strncmp(keyBuff, "unlock", 6) == 0) {
					threadsWaitForMutex();
					transportUdpServerSetLock(serverSim, FALSE);
					threadsReleaseMutex();
				} else if (strncmp(keyBuff, "lock", 4) == 0) {
					threadsWaitForMutex();
					transportUdpServerSetLock(serverSim, TRUE);
					threadsReleaseMutex();
				} else if (strncmp(keyBuff, "info", 4) == 0) {
					threadsWaitForMutex();
					serverSimInformation(serverSim, transportUdpServerGetLock());
					threadsReleaseMutex();
				} else if (strncmp(keyBuff, "savemap", 7) == 0) {
					threadsWaitForMutex();
					saveMap(saveBuff);
					threadsReleaseMutex();
				} else if (strncmp(keyBuff, "say ", 4) == 0) {
					transportUdpServerSendServerMessage((char *) keyBuff+4);
					{
						char pstr[256];
						int len = (int)strlen(keyBuff + 4);
						if (len > 0 && keyBuff[4 + len - 1] == '\n') len--;
						if (len > 255) len = 255;
						pstr[0] = (char)len;
						memcpy(pstr + 1, keyBuff + 4, len);
						logAddEvent(log_MessageServer, 0, 0, 0, 0, 0, pstr);
					}
				} else if(strncmp(keyBuff, "status", 6) == 0){
					transportUdpServerPrintStatus(statusFile);
				} else if (strncmp(keyBuff, "kick ", 5) == 0) {
					sprintf(playerKick, "%.*s", 32, keyBuff+5);
					newbuflen = strlen(playerKick);
					playerKick[newbuflen - 1] = '\0';
					threadsWaitForMutex();
					transportUdpServerKickPlayer(serverSim, playerKick);
					threadsReleaseMutex();
				} else if (strncmp(keyBuff, "host ", 5) == 0) {
					bool hostSet;
					sprintf(playerHost, "%.*s", 32, keyBuff+5);
					newbuflen = strlen(playerHost);
					playerHost[newbuflen - 1] = '\0';
					threadsWaitForMutex();
					hostSet = transportUdpServerSetHostByName(serverSim, playerHost);
					threadsReleaseMutex();
					if (hostSet) {
						printf("Host set to %s\n", playerHost);
					} else {
						printf("No such player\n");
					}
				} else if (strncmp(keyBuff, "quit", 4) == 0) {
					/* Loop's while-condition will exit on next check */
				} else if (strncmp(keyBuff, "\n", 1) != 0 && strncmp(keyBuff, "\0", 1) != 0) {
					fprintf(stderr, "Unknown command - Type \"help\" for help\n");
				}
			} else {
				Sleep(100);
			}
		}

		closeDebugFile();
	}
}

#else
/* Linux */
void processKeys(bool isQuiet) {
  char keyBuff[256] = "\0";
  char saveBuff[256] = "\0";
  fd_set fdmask;
  struct timeval timer;
  int ret;
  char playerKick[33] = "\0";
  char playerHost[33] = "\0";
  size_t newbuflen;

  timer.tv_sec = 1;
  timer.tv_usec = 0;
  FD_ZERO(&fdmask);
  FD_SET(STDIN_FILENO, &fdmask);

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
      if (strncmp(keyBuff, "help", 4) == 0) {
        printHelp();
      } else if (strncmp(keyBuff, "unlock", 6) == 0) {
        threadsWaitForMutex();
        transportUdpServerSetLock(serverSim, FALSE);
        threadsReleaseMutex();
      } else if (strncmp(keyBuff, "lock", 4) == 0) {
        threadsWaitForMutex();
        transportUdpServerSetLock(serverSim, TRUE);
        threadsReleaseMutex();
      } else if (strncmp(keyBuff, "info", 4) == 0) {
        threadsWaitForMutex();
        serverSimInformation(serverSim, transportUdpServerGetLock());
        threadsReleaseMutex();
      } else if (strncmp(keyBuff, "savemap", 7) == 0) {
        threadsWaitForMutex();
        saveMap(saveBuff);
        threadsReleaseMutex();
      } else if (strncmp(keyBuff, "say ", 4) == 0) {
        transportUdpServerSendServerMessage((char *) keyBuff+4);
        {
            char pstr[256];
            int len = (int)strlen(keyBuff + 4);
            if (len > 0 && keyBuff[4 + len - 1] == '\n') len--;
            if (len > 255) len = 255;
            pstr[0] = (char)len;
            memcpy(pstr + 1, keyBuff + 4, len);
            logAddEvent(log_MessageServer, 0, 0, 0, 0, 0, pstr);
        }
      } else if(strncmp(keyBuff, "status", 6) == 0){
        transportUdpServerPrintStatus(statusFile);
      } else if (strncmp(keyBuff, "kick ", 5) == 0) {
        sprintf(playerKick, "%.*s", 32, keyBuff+5);
        newbuflen = strlen(playerKick);
        playerKick[newbuflen - 1] = '\0';
        threadsWaitForMutex();
        transportUdpServerKickPlayer(serverSim, playerKick);
        threadsReleaseMutex();
      } else if (strncmp(keyBuff, "host ", 5) == 0) {
        bool hostSet;
        sprintf(playerHost, "%.*s", 32, keyBuff+5);
        newbuflen = strlen(playerHost);
        playerHost[newbuflen - 1] = '\0';
        threadsWaitForMutex();
        hostSet = transportUdpServerSetHostByName(serverSim, playerHost);
        threadsReleaseMutex();
        if (hostSet) {
          printf("Host set to %s\n", playerHost);
        } else {
          printf("No such player\n");
        }
      } else if (strncmp(keyBuff, "quit", 4) == 0) {
        /* Loop's while-condition will exit on next check */
      } else if (strncmp(keyBuff, "\n", 1) != 0 && strncmp(keyBuff, "\0", 1) != 0) {
        fprintf(stderr, "Unknown command - Type \"help\" for help\n");
      }

      timer.tv_sec = 1;
      timer.tv_usec = 0;
      FD_ZERO(&fdmask);
      FD_SET(STDIN_FILENO, &fdmask);

      ret = select(STDIN_FILENO + 1, &fdmask, NULL, NULL, &timer);
      if (ret > 0) {
        fgets(keyBuff, 256, stdin);
        strcpy(saveBuff, keyBuff);
        strlower(keyBuff);
      } else if (ret != -1) {
        keyBuff[0] = '\0';
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
            Sleep(50);
#else
            SDL_Delay(50);
#endif
            continue;
        }

        uint32_t serverTick = serverSimGetTick(serverSim);
        if (cmd.tick > serverTick) {
#ifdef _WIN32
            Sleep(10);
#else
            SDL_Delay(10);
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
      while ((tick - oldTick) > SERVER_TICK_LENGTH) {
        if (SDL_GetAtomicInt(&g_serverShuttingDown)) break;
        serverInstanceTick(serverSim);
        ticks++;
        oldTick += SERVER_TICK_LENGTH;
      }
    }
    SDL_UnlockMutex(g_serverTickLock);
  }
#ifdef USING_SDL
  return interval;
#endif
}

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
#ifdef _WIN32
  timeKillEvent(serverTimerGameID);
#else
  SDL_RemoveTimer(serverTimerGameID);
#endif
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
  fprintf(stderr, "                autolock, password, ranked, openhost, map.\n");
  fprintf(stderr, "                e.g. -lock gametype,ranked,map\n");
  fprintf(stderr, "-maxplayers <N> - Specifies the maximum number of players that can be on this\n");
  fprintf(stderr, "                server.\n");
  fprintf(stderr, "-maxspectators <N> - Maximum number of spectator connections (default 16,\n");
  fprintf(stderr, "                0 disables spectating).\n");
  fprintf(stderr, "-specdelay <S> - Spectator view delay in seconds (default 90, 0 = live).\n");

  fprintf(stderr, "\nMap uploads (client-pushed maps in the lobby):\n");
  fprintf(stderr, "-uploadpolicy <P> - Client map-upload handling: \"off\" refuses uploads,\n");
  fprintf(stderr, "                \"allow\" plays the upload in memory and drops it on the next\n");
  fprintf(stderr, "                map change (default), \"persist\" also saves it to\n");
  fprintf(stderr, "                data/maps/Uploads/.\n");
  fprintf(stderr, "-uploadmaxfiles <N> - Max stored upload files in persist mode (1-255,\n");
  fprintf(stderr, "                default 64).\n");
  fprintf(stderr, "-uploadmaxstorage <MB> - Max upload storage in persist mode (1-4096 MB,\n");
  fprintf(stderr, "                default 8).\n");

  fprintf(stderr, "\nBots & AI:\n");
  fprintf(stderr, "-bots <N>     - Number of AI bot players to add (default: 0)\n");
  fprintf(stderr, "-maxbots <N>  - Maximum number of AI bots that can be in the lobby\n");
  fprintf(stderr, "                (default: 0 = no limit). Caps lobby \"Add Bot\" requests\n");
  fprintf(stderr, "                and clamps -bots.\n");
  fprintf(stderr, "-brain <path> - Path to the Lua brain script for bots\n");
  fprintf(stderr, "-bot-init <spec> - Per-bot brain paths by player id: 'range=path[arg],...'\n");
  fprintf(stderr, "                where range is 'a-b' or 'n' and the optional [arg] becomes\n");
  fprintf(stderr, "                that bot's BRAIN_INIT_ARG Lua global. Ids not listed use\n");
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

  fprintf(stderr, "\nLogging & diagnostics:\n");
  fprintf(stderr, "-log [name]   - Create game log file. Optional [name] is a filename, or a\n");
  fprintf(stderr, "                directory (e.g. -log /tmp) to auto-name the log inside it.\n");
  fprintf(stderr, "-logfile      - Write all output to file instead of console.\n");
  fprintf(stderr, "-dontsendlog  - Don't upload game log to winbolo.net\n");
  fprintf(stderr, "-statusFile   - Save list of unlocked players to a file.\n");
  fprintf(stderr, "-seed <N>     - Seed the RNG with N (64-bit unsigned) for reproducible runs.\n");
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

/** Generates a log file name based on current time and map name and copies to fileName */
void makeLogFileName(char *outFileName, const char *mapName) {
  time_t t;
  struct tm *tmt;
  int count = 0;
  int len;

  time(&t);
  tmt = localtime(&t);

  sprintf(outFileName, "%04d%02d%02dt%02d%02d%02d_%s", (1900 + tmt->tm_year), (1 + tmt->tm_mon), tmt->tm_mday, tmt->tm_hour, tmt->tm_min, tmt->tm_sec, mapName);
  len = (int) strlen(outFileName);
  /* Replace spaces with underscores */
  while (count < len){
    if (outFileName[count] == ' ') {
      outFileName[count] = '_';
    }
    count++;
  }
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
*  base, plus the winner (if any). Emitted once at end-of-game.
*
*ARGUMENTS:
*  sim    - The server sim (must still hold final state).
*  dest   - "-" for stdout, otherwise a file path (truncated).
*  reason - Short machine tag for why the game ended.
*********************************************************/
static void serverEmitFinalJson(ServerSim *sim, const char *dest,
                                const char *reason) {
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
    t = cJSON_CreateObject();
    cJSON_AddNumberToObject(t, "player", (double)i);
    cJSON_AddStringToObject(t, "name", ti.name);
    cJSON_AddBoolToObject(t, "alive", ti.alive);
    cJSON_AddNumberToObject(t, "kills", (double)ti.kills);
    cJSON_AddNumberToObject(t, "deaths", (double)ti.deaths);
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
    f = fopen(dest, "w");
    if (f == NULL) {
      fprintf(stderr, "Error: cannot open -finaljson file '%s'\n", dest);
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
    serverSim = serverSimCreateCompressed(emap, 5097, "Everard Island", game, hiddenMines, srtDelay, gmeLen);
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
    serverSim = serverSimCreate(scannedFiles[bolo_rand_below((uint32_t)scannedCount)], game, hiddenMines, srtDelay, gmeLen);
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
  uint16_t serverLocks = 0;
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
        else {
          fprintf(stderr,
                  "Warning: unknown -lock name '%s' (valid: gametype, "
                  "ai, mines, timelimit, autolock, password, ranked, "
                  "openhost, map)\n", lo);
        }
      }
    }
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
    }
    /* If no -brain specified but AI is enabled, auto-discover a brain path
     * so that lobby "Add Bot" requests have a brain to use. */
    if (brainPath[0] == '\0' && ai != aiNone) {
      static const char *candidates[] = {
        "Brains/GoalHunter_1.6/init.lua",
        "brains/GoalHunter_1.6/init.lua",
        "data/Brains/GoalHunter_1.6/init.lua",
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

  {
    ServerInstanceConfig instCfg;
    UploadPolicy uploadPolicy = UPLOAD_POLICY_ALLOW;
    uint8_t      uploadMaxFiles = 0;        /* 0 = leave transport default */
    uint32_t     uploadMaxStorageBytes = 0; /* 0 = leave transport default */

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
      int storageArg = findArg(argc, argv, "uploadmaxstorage");
      if (storageArg != ARG_NOT_FOUND) {
        int v = atoi((char *)argv[storageArg]);
        if (v < 1) {
          fprintf(stderr, "-uploadmaxstorage %d out of range; clamping to 1\n", v);
          v = 1;
        } else if (v > 4096) {
          fprintf(stderr, "-uploadmaxstorage %d out of range; clamping to 4096\n", v);
          v = 4096;
        }
        uploadMaxStorageBytes = (uint32_t)v * 1024u * 1024u;
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
    if (serverInstanceStartup(serverSim, &instCfg) == FALSE) {
      fprintf(stderr, "Error creating network transport\n");
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
  dontSendLog = argExist(argc, argv, "dontsendlog");

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
    serverDedicatedLogInstall(serverSim);
  }

  /* Initialize and add bot players */
  {
    int threadsArg = 0;
    int argNum = findArg(argc, argv, "threads");
    if (argNum != ARG_NOT_FOUND) {
      threadsArg = atoi((char *)argv[argNum]);
    }
    if (!botManagerInit(threadsArg)) {
      fprintf(stderr, "Error initializing bot manager\n");
#ifdef USING_SDL
      SDL_Quit();
#endif
      return 0;
    }
  }
  /* botBrainPath + botAiType were already pushed into the sim via the
   * cfg block above; the loop below only needs to spawn the configured
   * bot count (numBots / brainPath resolved earlier). */
  {
    if (numBots > 0 && brainPath[0] != '\0') {
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
         * print2_botN.log — the exact footgun the usage text warns about.
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
      /* -bot-init: per-player-id brain/init.lua paths (+ optional [arg]). Every
       * id defaults to the shared brainPath with no arg; the spec overrides the
       * ids it names. Shared parser/semantics with BrainTest. */
      BotInitSlot botInit[MAX_TANKS];
      for (i = 0; i < MAX_TANKS; i++) {
        snprintf(botInit[i].path, sizeof(botInit[i].path), "%s", brainPath);
        botInit[i].arg[0] = '\0';
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
        /* Stage this bot's BRAIN_INIT_ARG (consumed by the create below) and
         * use its resolved brain path. The name was pre-picked into botNames[i]
         * above (under a bolo_rand save/restore so it stays off the sim PRNG). */
        luaBrainsSetNextInitArg(botInit[i].arg);
        if (botInit[i].covered) {
          fprintf(stderr, "Bot %d: -bot-init brain '%s'%s%s\n", i, botInit[i].path,
                  botInit[i].arg[0] ? " arg=" : "", botInit[i].arg);
        }
        if (!botManagerAddBot(serverSim, (BYTE)i, botInit[i].path, botNames[i], ai, game, hiddenMines)) {
          fprintf(stderr, "Warning: failed to add bot %d\n", i);
        } else if (allyTeam > 0) {
          /* Shared non-zero team for every bot — server_sim's start-of-round
           * pass converts matching teamNumber into alliances, and the lobby
           * protocol already broadcasts teamNumber to clients so the lobby
           * UI shows the bots on this team. */
          serverSimSetTeamBatch(serverSim, (BYTE)i, (uint8_t)allyTeam);
        }
      }
      if (numTeamSizes > 0) {
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
    } else if (numBots > 0) {
      fprintf(stderr, "Warning: -bots specified but no -brain path given\n");
    }
  }

  if (threadsCreate(TRUE) == FALSE) {
    fprintf(stderr, "Error starting Thread Manager\n");
    threadsDestroy();
    serverInstanceShutdown(serverSim);
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
#ifdef _WIN32
  oldTick = SDL_GetTicks();
  serverTimerGameID = timeSetEvent(SERVER_TICK_LENGTH, 10, serverGameTimer, 0, TIME_PERIODIC);
#else
  oldTick = SDL_GetTicks();
  serverTimerGameID = SDL_AddTimer(SERVER_TICK_LENGTH, serverGameTimer, NULL);
#endif

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
    serverEmitFinalJson(serverSim, optFinalJson, "tick_limit");
  }
  botWorkerPoolDestroy();
  brainRecordShutdown();   /* flush + close brainrec.btr (no-op if not recording) */
  threadsDestroy();

  if (isLogging == TRUE && winbolonetIsRunning() == TRUE && argExist(argc, argv, "dontsendlog") == FALSE) {
    winboloNetGetServerKey(key);
  } else {
    key[0] = EMPTY_CHAR;
  }

  serverInstanceShutdown(serverSim);

  if (isLogging == TRUE) {
    logStop(); /* Finalize zip — must run regardless of WBN upload */
  }
  if (isLogging == TRUE && key[0] != EMPTY_CHAR && argExist(argc, argv, "dontsendlog") == FALSE) {
    serverMessageConsoleMessage(serverSim,(char *)"Uploading log file to winbolo.net");
    httpCreate();
    httpSendLogFile(fileName, key, FALSE);
    httpDestroy();
  }
  geoLookupDestroy();
  serverSimMapDirDestroy(serverSim);
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
