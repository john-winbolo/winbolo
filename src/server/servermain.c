/*
 * $Id$
 *
 * Copyright (c) 1998-2008 John Morrison.
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

#include "debug_file_output.h"
#include "geolookup.h"
#include "global.h"
#include "gametype.h"
#include "threads.h"
#include "../winbolonet/winbolonet.h"
#include "server_sim.h"
#include "../mapeditor/mapeditor_generate.h"
#include "log.h"
#include "transport_udp.h"
#include "bot_manager.h"
#include "wire_limits.h"  /* LOBBY_LOCK_* bitmask values */
#include "server_lifecycle.h"
#include "../common/sentry_integration.h"
#include "../common/wb_log.h"

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
  fprintf(stderr, "Help:\n Lock - Locks the server and stops new players from joining.\n Unlock - Unlocks the server and allows new players to join.\n savemap <map file> - Save the map file to path and file <map file>\n Say <text> - Sends this message to all players in the game unless they have turned off server messages.\n Quit - Exits the server.\n Info - Provide information about the current game\n Kick - Kicks a player. Case insensitive, prefix a * for WBN players.\n Status - Returns list of players who aren't locked.\n");
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
	size_t newbuflen;

	if (isQuiet == TRUE || isNoInput == TRUE) {
		while (!(serverSimGetState(serverSim) == serverStateGameOver && !serverSimIsLobbyEnabled(serverSim))) {
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

		while (strncmp(keyBuff, "quit", 4) != 0 && !(serverSimGetState(serverSim) == serverStateGameOver && !serverSimIsLobbyEnabled(serverSim))) {
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
  size_t newbuflen;

  timer.tv_sec = 1;
  timer.tv_usec = 0;
  FD_ZERO(&fdmask);
  FD_SET(STDIN_FILENO, &fdmask);

  if (isQuiet == TRUE || isNoInput == TRUE) {
    while (!(serverSimGetState(serverSim) == serverStateGameOver && !serverSimIsLobbyEnabled(serverSim))) {
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
    while (strncmp(keyBuff, "quit", 4) != 0 && !(serverSimGetState(serverSim) == serverStateGameOver && !serverSimIsLobbyEnabled(serverSim))) {
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

  if ((tick - oldTick) > SERVER_TICK_LENGTH) {
    while ((tick - oldTick) > SERVER_TICK_LENGTH) {
      serverInstanceTick(serverSim);
      ticks++;
      oldTick += SERVER_TICK_LENGTH;
    }
  }
#ifdef USING_SDL
  return interval;
#endif
}

#define DEFAULT_TRACKER_ADDR "tracker.winbolo.com"
#define DEFAULT_TRACKER_PORT 50000

void printArgs() {
#ifdef _WIN32
  fprintf(stderr, "Usage:\nWinBoloDS -map <Filename> -port <Port> -gametype <GameType> -mines <Mines> -ai <AiType> -delay <Delay> -limit <Limit> -tracker <Tracker> -wbnhost <Host> -password <Password>\n\n");
#else
  fprintf(stderr, "Usage:\nLinBoloDS -map <Filename> -port <Port> -gametype <GameType> -mines <Mines> -ai <AiType> -delay <Delay> -limit <Limit> -tracker <Tracker> -wbnhost <Host> -password <Password>\n\n");
#endif
  fprintf(stderr, "<Filename>    - Path and file name of the map file to open (-inbuilt can be used\n");
  fprintf(stderr, "                instead of -map to enable inbuilt map Everard Island)\n");
  fprintf(stderr, "-mapdir <Dir> - Directory of .map files for random rotation between rounds.\n");
  fprintf(stderr, "                Can be used with -map (initial map) or alone (random first map).\n");
  fprintf(stderr, "                Requires lobby mode. Invalid maps are skipped at startup.\n");
  fprintf(stderr, "<Port>        - Port to run the server on\n");
  fprintf(stderr, "<GameType>    - Specifies the game type: \"Open\" or \"Tournament\" or \"Strict\"\n");
  fprintf(stderr, "\nOptional\n");
  fprintf(stderr, "<Mines>       - Specifies allowing hidden mines: \"yes\" for allow,\n");
  fprintf(stderr, "                \"no\" for disallow (on if not specified)\n" );
  fprintf(stderr, "<AiType>      - Specifies allowing brains. Valid values are \"no\" for\n");
  fprintf(stderr, "                disallowing, \"yes\" for allowing, \"yesAdv\" for giving\n");
  fprintf(stderr, "                them an advantage, or \"yesFull\" for full map advantage.\n");
  fprintf(stderr, "                (disallowed if not specified)\n");
  fprintf(stderr, "<Delay>       - Specifies the start delay (in seconds) (none if not specified)\n");
  fprintf(stderr, "<Limit>       - Specifies the game time limit (in minutes)\n");
  fprintf(stderr, "                \"-1\" for no time limit (none if not specified)\n");
  fprintf(stderr, "-ticks <N>    - Exit cleanly after N game-ticks of running play.\n");
  fprintf(stderr, "                \"0\" or omitted means unlimited (default).\n");
  fprintf(stderr, "<Password>    - Game Password (none if not specified)\n");
  fprintf(stderr, "<tracker>     - Internet tracker to notify. Options:\n");
  fprintf(stderr, "                -tracker alone uses default (%s:%d)\n", DEFAULT_TRACKER_ADDR, DEFAULT_TRACKER_PORT);
  fprintf(stderr, "                -tracker <host> uses <host> with default port %d\n", DEFAULT_TRACKER_PORT);
  fprintf(stderr, "                -tracker <host:port> uses the specified host and port\n\n");
  fprintf(stderr, "-quiet        - No screen input or output (silent mode)\n");
  fprintf(stderr, "-noinput      - No keyboard input\n");
  fprintf(stderr, "-addr         - Specify a different address to use if avaliable\n");
  fprintf(stderr, "-autoclose    - Automatically quit the server when all players have left\n");
  fprintf(stderr, "                the game\n");
  fprintf(stderr, "-wbnhost      - WinBolo.net host to connect to (overrides preferences file).\n");
  fprintf(stderr, "                Bare hostname uses https (e.g. -wbnhost wbn.winbolo.net),\n");
  fprintf(stderr, "                or specify scheme (e.g. -wbnhost http://wbn.winbolo.net)\n");
  fprintf(stderr, "-nowinbolonet - Do not participate in winbolo.net game tracking\n");
  fprintf(stderr, "-logfile      - Write all output to file instead of console.\n");
  fprintf(stderr, "-maxplayers   - Specifies the maximum number of players that can be on this\n");
  fprintf(stderr, "                server.\n");
  fprintf(stderr, "-seed <N>     - Seed the RNG with N for reproducible runs.\n");
  fprintf(stderr, "-log          - Create game log file (filename optional)\n");
  fprintf(stderr, "-dontsendlog  - Don't upload game log to winbolo.net\n");
  fprintf(stderr, "-statusFile	 - Save list of unlocked players to a file.\n");
  fprintf(stderr, "-threads <N>  - Total concurrent bot-think runners including the main\n");
  fprintf(stderr, "                thread. 1 disables the worker pool. Default: logical cores.\n");
  fprintf(stderr, "-bots <N>     - Number of AI bot players to add (default: 0)\n");
  fprintf(stderr, "-brain <path> - Path to the Lua brain script for bots\n");
  fprintf(stderr, "-allybots [N] - Place all -bots on the same team (1-16, default 1) so\n");
  fprintf(stderr, "                they start allied. Pick the same team in the lobby to join\n");
  fprintf(stderr, "                them, or a different one to fight against them.\n");
  fprintf(stderr, "-nolobby      - Skip lobby, start game immediately (backward-compatible mode)\n");
  fprintf(stderr, "-quitonwin    - Quit server when a player/alliance wins\n");
  fprintf(stderr, "-noemptyreset - Disable automatic lobby reset when server is empty\n");
  fprintf(stderr, "                (enabled by default, resets after 5 minutes)\n");
  fprintf(stderr, "-emptyresetmins <N> - Minutes before empty server resets to lobby (default: 5)\n");
  fprintf(stderr, "-upnp         - request automatic UPnP/NAT-PMP port mapping\n");
  fprintf(stderr, "-no-natpunch  - disable hole-punch keepalive (on by default with tracker)\n");
  fprintf(stderr, "-randommap        - Generate a random procedural map instead of loading a file.\n");
  fprintf(stderr, "                    -randommap alone generates a fully random map each round.\n");
  fprintf(stderr, "                    -randommap tournament|natural|maze|fractal — specific generator type.\n");
  fprintf(stderr, "                    -randommap <seed> — reproduce a specific map from its seed.\n");
  fprintf(stderr, "                    -randommap tournament <seed> — type with specific seed.\n");
  fprintf(stderr, "                    Map name shown as 'rand_<seed>' in server info.\n");
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

  /* Tracker */
  *trackerUse = FALSE;
  if (argExist(numArgs, argv, "tracker") == TRUE) {
    *trackerUse = TRUE;
    argNum = findArg(numArgs, argv, "tracker");
    if (argNum == ARG_NOT_FOUND || argv[argNum][0] == '-') {
      /* -tracker with no argument: use default tracker */
      strncpy(trackerAddr, DEFAULT_TRACKER_ADDR, FILENAME_MAX - 1);
      trackerAddr[FILENAME_MAX - 1] = '\0';
      *trackerPort = DEFAULT_TRACKER_PORT;
    } else {
      processTrackerArg((char *) argv[argNum], trackerAddr, trackerPort);
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

int main(int argc, char **argv) {
  srand((unsigned int)(time(NULL) ^ getpid()));
  {
    int seedArg = findArg(argc, argv, "seed");
    if (seedArg != ARG_NOT_FOUND) {
      srand((unsigned int)strtoul((char *)argv[seedArg], NULL, 0));
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

#ifdef USING_SDL
  if (!SDL_Init(0)) {
    fprintf(stderr, "Error starting SDL - %s\n", SDL_GetError());
    exit(0);
  }
#endif
  /* IP-to-country geolocation (DB-IP Lite) */
  bool geoLookupOk = geoLookupCreate("data/dbip-country-lite.mmdb");

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
      cfg = mapGenDefaultConfig(types[rand() % 4]);
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
    serverSim = serverSimCreateCompressed(emap, 5097, game, hiddenMines, srtDelay, gmeLen);
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
    serverSim = serverSimCreate(scannedFiles[rand() % scannedCount], game, hiddenMines, srtDelay, gmeLen);
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
    serverSimInstallMapDirList(serverSim, scannedFiles, scannedCount);
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
  serverSimSetBotAiType(serverSim, ai);
  if (findArg(argc, argv, "logfile") != ARG_NOT_FOUND) {
    serverMessagesSetLogFile(serverSim, (char *) argv[findArg(argc, argv, "logfile")]);
  }
  if (geoLookupOk) {
    serverMessageConsoleMessage(serverSim, "Geo lookup database loaded.\n");
  } else {
    serverMessageConsoleMessage(serverSim, "Geo lookup database not found — country codes will be XX.\n");
  }

  /* Layout A lobby — admin-only --lock-* CLI flags. Sets the
   * serverLocks bitmask exposed in PACKET_LOBBY_STATE. Hosts can't
   * change locks at runtime; only the admin/operator running bolod
   * sets them. Clients render matching settings disabled with a
   * lock badge. */
  {
    uint16_t locks = 0;
    if (argExist(argc, argv, "lock-game-type"))     locks |= LOBBY_LOCK_GAME_TYPE;
    if (argExist(argc, argv, "lock-ai-policy"))     locks |= LOBBY_LOCK_AI_POLICY;
    if (argExist(argc, argv, "lock-mines"))         locks |= LOBBY_LOCK_MINES;
    if (argExist(argc, argv, "lock-time-limit"))    locks |= LOBBY_LOCK_TIME_LIMIT;
    if (argExist(argc, argv, "lock-auto-lock"))     locks |= LOBBY_LOCK_AUTO_LOCK_ON_GAME;
    serverSimSetServerLocks(serverSim, locks);
    if (locks) {
      fprintf(stderr, "  Server locks (admin):");
      if (locks & LOBBY_LOCK_GAME_TYPE)         fprintf(stderr, " game-type");
      if (locks & LOBBY_LOCK_AI_POLICY)         fprintf(stderr, " ai-policy");
      if (locks & LOBBY_LOCK_MINES)             fprintf(stderr, " mines");
      if (locks & LOBBY_LOCK_TIME_LIMIT)        fprintf(stderr, " time-limit");
      if (locks & LOBBY_LOCK_AUTO_LOCK_ON_GAME) fprintf(stderr, " auto-lock");
      fprintf(stderr, "\n");
    }
  }

  useAddr = NULL;
  httpSetAltIpAddress("");
  if (argExist(argc, argv, "addr") == TRUE) {
    useAddr = (char *) argv[findArg(argc, argv, "addr")];
    httpSetAltIpAddress(useAddr);
  }

  /* -admins <comma-separated-ips> — clients connecting from any of
   * these IPs are tagged with PLAYER_FLAG_ADMIN and get host-level
   * lobby authority (settings, teams, bot control). The host (slot 0)
   * always retains authority regardless. */
  serverSimSetAdminIps(serverSim, "");
  if (argExist(argc, argv, "admins")) {
    int argNum = findArg(argc, argv, "admins");
    if (argNum != ARG_NOT_FOUND) {
      serverSimSetAdminIps(serverSim, (char *)argv[argNum]);
      fprintf(stderr, "  Admin IPs: %s\n", serverSimGetAdminIps(serverSim));
    }
  }

  /* -adminfirst — first player to join while the server has no other
   * connected players gets PLAYER_FLAG_ADMIN. Re-arms whenever the
   * server becomes empty again. Convenient for dynamic-IP hosts who
   * can't pre-whitelist their own address via -admins. */
  serverSimSetAdminFirstJoinAfterEmpty(serverSim,
                                       argExist(argc, argv, "adminfirst") == TRUE);
  if (serverSimGetAdminFirstJoinAfterEmpty(serverSim)) {
    fprintf(stderr, "  Admin grant on first-join after empty: ENABLED\n");
  }

  statusFile = argExist(argc, argv, "statusFile");
  serverSimSetQuitOnWin(serverSim, argExist(argc, argv, "quitonwin") == TRUE);
  serverSimSetAutoCloseOnEmpty(serverSim, argExist(argc, argv, "autoclose") == TRUE);

  {
    int argNum = findArg(argc, argv, "ticks");
    if (argNum != ARG_NOT_FOUND) {
      serverSimSetTickLimit(serverSim, (int32_t)strtoul((char *)argv[argNum], NULL, 0));
    }
  }

  /* Empty reset configuration — on by default */
  if (argExist(argc, argv, "noemptyreset") == TRUE) {
    serverSimSetEmptyResetEnabled(serverSim, false);
  }
  {
    int argNum = findArg(argc, argv, "emptyresetmins");
    if (argNum != ARG_NOT_FOUND) {
      int mins = atoi((char *)argv[argNum]);
      if (mins > 0) {
        serverSimSetEmptyResetMinutes(serverSim, mins);
      }
    }
  }

  /* -nolobby: skip lobby, start running immediately (backward-compatible mode) */
  if (argExist(argc, argv, "nolobby") == TRUE) {
    serverSimSetLobbyEnabled(serverSim, false);
    serverSimSetEmptyResetEnabled(serverSim, false);
    serverSimStartGame(serverSim);
  }

  /* -mapdir: build validated map list for rotation between rounds.
   * Skip if already built (the -mapdir without -map case builds it earlier). */
  {
    int argNum = findArg(argc, argv, "mapdir");
    if (argNum != ARG_NOT_FOUND && serverSimGetMapDirFiles(serverSim) == NULL) {
      if (!serverSimIsLobbyEnabled(serverSim)) {
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

  /* WinBolo.net host override — must run before serverInstanceStartup
   * so winbolonetCreateServer hits the override host. */
  {
    int argNum = findArg(argc, argv, "wbnhost");
    if (argNum != ARG_NOT_FOUND) {
      httpSetHostOverride((char *)argv[argNum]);
    }
  }

  serverSimSetHasPassword(serverSim, pass[0] != '\0');
  {
    ServerInstanceConfig instCfg;
    instCfg.udpPort      = port;
    instCfg.bindAddr     = useAddr;
    instCfg.password     = pass;
    instCfg.maxPlayers   = (BYTE)maxPlayers;
    instCfg.useWbn       = (argExist(argc, argv, "nowinbolonet") == FALSE);
    instCfg.compTanks    = (BYTE)ai;
    instCfg.useTracker   = sTrackerUse;
    instCfg.trackerAddr  = sTrackerAddr;
    instCfg.trackerPort  = sTrackerPort;
    {
      bool natPunchOptOut = (argExist(argc, argv, "no-natpunch") == TRUE);
      instCfg.useNatPortmap   = (argExist(argc, argv, "upnp") == TRUE);
      instCfg.useNatKeepalive = sTrackerUse && !natPunchOptOut;
    }
    if (serverInstanceStartup(serverSim, &instCfg) == FALSE) {
      fprintf(stderr, "Error creating network transport\n");
      serverSimDestroy(serverSim);
#ifdef USING_SDL
      SDL_Quit();
#endif
      return 0;
    }
  }
  dontSendLog = argExist(argc, argv, "dontsendlog");

  /* Log file recording */
  if (argExist(argc, argv, "log") == TRUE) {
    int logArg = findArg(argc, argv, "log");
    char userLogFile[MAX_PATH] = {0};
    if (logArg != ARG_NOT_FOUND && argv[logArg][0] != '-') {
      strncpy(userLogFile, (char *)argv[logArg], MAX_PATH - 1);
    }
    if (!serverSimIsLobbyEnabled(serverSim)) {
      /* No-lobby: game is already running, start logging immediately */
      if (userLogFile[0] != '\0') {
        strncpy(fileName, userLogFile, MAX_PATH - 1);
      } else {
        makeLogFileName(fileName, serverSimGetMapName(serverSim));
      }
      /* Ensure .wbv extension */
      {
        size_t flen = strlen(fileName);
        if (flen <= 4 || strcmp(fileName + flen - 4, ".wbv") != 0) {
          strncat(fileName, ".wbv", sizeof(fileName) - flen - 1);
        }
      }
      isLogging = logStart(fileName, serverSim,
                           (BYTE)ai, (BYTE)maxPlayers,
                           serverSimHasPassword(serverSim));
      if (isLogging) {
        fprintf(stderr, "Logging to %s\n", fileName);
      } else {
        fprintf(stderr, "Warning: failed to start logging\n");
      }
    } else {
      /* Lobby mode: start logging now so lobby joins/chat are captured */
      serverSimSetWantLogging(serverSim, true);
      if (userLogFile[0] != '\0') {
        strncpy(fileName, userLogFile, MAX_PATH - 1);
        serverSimSetUserLogFileName(serverSim, userLogFile);
      } else {
        makeLogFileName(fileName, serverSimGetMapName(serverSim));
      }
      {
        size_t flen = strlen(fileName);
        if (flen <= 4 || strcmp(fileName + flen - 4, ".wbv") != 0) {
          strncat(fileName, ".wbv", sizeof(fileName) - flen - 1);
        }
      }
      isLogging = logStart(fileName, serverSim,
                           (BYTE)ai, (BYTE)maxPlayers,
                           serverSimHasPassword(serverSim));
      if (isLogging) {
        fprintf(stderr, "Logging to %s (lobby)\n", fileName);
        logAddEvent(log_LobbyEnter, 0, 0, 0, 0, 0, NULL);
      } else {
        fprintf(stderr, "Warning: failed to start logging\n");
      }
    }
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
  {
    int numBots = 0;
    char brainPath[MAX_PATH];
    int argNum;

    brainPath[0] = '\0';
    argNum = findArg(argc, argv, "bots");
    if (argNum != ARG_NOT_FOUND) {
      numBots = atoi((char *)argv[argNum]);
      if (numBots < 0) numBots = 0;
      if (numBots > MAX_TANKS) numBots = MAX_TANKS;
    }
    argNum = findArg(argc, argv, "brain");
    if (argNum != ARG_NOT_FOUND) {
      strncpy(brainPath, (char *)argv[argNum], MAX_PATH - 1);
      brainPath[MAX_PATH - 1] = '\0';
    }
    /* If no -brain specified but AI is enabled, auto-discover a brain path
     * so that lobby "Add Bot" requests have a brain to use. */
    if (brainPath[0] == '\0' && ai != aiNone) {
      static const char *candidates[] = {
        "Brains/NewAutopilot/init.lua",
        "brains/NewAutopilot/init.lua",
        "data/Brains/NewAutopilot/init.lua",
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
    serverSimSetBotBrainPath(serverSim, brainPath);
    if (numBots > 0 && brainPath[0] != '\0') {
      int i;
      char botName[64];
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
      for (i = 0; i < numBots; i++) {
        snprintf(botName, sizeof(botName), "Bot %d", i + 1);
        if (!botManagerAddBot(serverSim, (BYTE)i, brainPath, botName, ai, game, hiddenMines)) {
          fprintf(stderr, "Warning: failed to add bot %d\n", i);
        } else if (allyTeam > 0) {
          /* Shared non-zero team for every bot — server_sim's start-of-round
           * pass converts matching teamNumber into alliances, and the lobby
           * protocol already broadcasts teamNumber to clients so the lobby
           * UI shows the bots on this team. */
          serverSimSetTeam(serverSim, (BYTE)i, (uint8_t)allyTeam);
        }
      }
      if (allyTeam > 0) {
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
#ifdef _WIN32
  oldTick = SDL_GetTicks();
  serverTimerGameID = timeSetEvent(SERVER_TICK_LENGTH, 10, serverGameTimer, 0, TIME_PERIODIC);
#else
  oldTick = SDL_GetTicks();
  serverTimerGameID = SDL_AddTimer(SERVER_TICK_LENGTH, serverGameTimer, NULL);
#endif

  processKeys(isQuiet);

#ifdef _WIN32
  timeKillEvent(serverTimerGameID);
#else
  SDL_RemoveTimer(serverTimerGameID);
#endif
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
#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}

time_t serverMainGetTicks() {
  return ticks;
}
