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
#include <string.h>

#include "../winbolonet/http.h"

#ifdef _WIN32
  #include <WinSock2.h>
#else
  #include <sys/time.h>
  #include <sys/types.h>
  #include <unistd.h>
  #include <signal.h>
  #include <errno.h>
  #include <SDL3/SDL.h>
  #define USING_SDL
#endif

#include "../bolo/everard_map.h"

#include "../bolo/debug_file_output.h"
#include "geolookup.h"
#include "../bolo/global.h"
#include "../bolo/gametype.h"
#include "threads.h"
#include "../winbolonet/winbolonet.h"
#include "server_sim.h"
#include "../bolo/transport_udp.h"
#include "../bolo/bot_manager.h"

/* Constants previously from backend.h */
#define GAME_TICK_LENGTH 10
#ifndef GAME_NUMGAMETICKS_SEC
#define GAME_NUMGAMETICKS_SEC (1000 / 20)
#endif

/* From backend.c / timer code */
void initWinboloTimer(void);
DWORD winboloTimer(void);
void endWinboloTimer(void);

/* From servermessages.c — called directly now instead of through servercore wrappers */
void serverMessageSetQuietMode(bool modeOn);
void serverMessagesSetLogFile(char *logFile);
void serverMessageConsoleMessage(char *msg);

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

static ServerSim serverSim;

/* Tracker settings (set from command-line args, read by timer) */
static char  sTrackerAddr[FILENAME_MAX] = "";
static unsigned short sTrackerPort = 0;
static bool  sTrackerUse = FALSE;

/* Game Tick */
#define SERVER_TICK_LENGTH (GAME_TICK_LENGTH*2)

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

#ifndef _WIN32
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
    if (serverSimSaveMap(&serverSim, ptr) == FALSE) {
      fprintf(stderr, "Sorry, an error occured saving the map. Is the path correct?\n");
    }
  }
}

void printHelp() {
  fprintf(stderr, "Help:\n Lock - Locks the server and stops new players from joining.\n Unlock - Unlocks the server and allows new players to join.\n savemap <map file> - Save the map file to path and file <map file>\n Say <text> - Sends this message to all players in the game unless they have turned off server messages.\n Quit - Exits the server.\n Info - Provide information about the current game\n Kick - Kicks a player. Case insensitive, prefix a * for WBN players.\n Status - Returns list of players who aren't locked.\n");
}


#ifdef _WIN32
void processKeys(bool isQuiet) {
	char keyBuff[256] = "\0";
	char saveBuff[256] = "\0";
	char playerKick[33] = "\0";
	size_t newbuflen;

	if (isQuiet == TRUE || isNoInput == TRUE) {
		while (!(serverSim.state == serverStateGameOver && !serverSim.lobbyEnabled)) {
			Sleep(1000);
		}
	} else {
		while (strncmp(keyBuff, "quit", 4) != 0 && !(serverSim.state == serverStateGameOver && !serverSim.lobbyEnabled)) {
			if (strncmp(keyBuff, "help", 4) == 0) {
				printHelp();
			} else if (strncmp(keyBuff, "unlock", 6) == 0) {
				threadsWaitForMutex();
				transportUdpServerSetLock(&serverSim, FALSE);
				threadsReleaseMutex();
			} else if (strncmp(keyBuff, "lock", 4) == 0) {
				threadsWaitForMutex();
				transportUdpServerSetLock(&serverSim, TRUE);
				threadsReleaseMutex();
			} else if (strncmp(keyBuff, "info", 4) == 0) {
				threadsWaitForMutex();
				serverSimInformation(&serverSim, transportUdpServerGetLock());
				threadsReleaseMutex();
			} else if (strncmp(keyBuff, "savemap", 7) == 0) {
				threadsWaitForMutex();
				saveMap(saveBuff);
				threadsReleaseMutex();
			} else if (strncmp(keyBuff, "say ", 4) == 0) {
				transportUdpServerSendServerMessage((char *) keyBuff+4);
			} else if(strncmp(keyBuff, "status", 6) == 0){
				transportUdpServerPrintStatus(statusFile);
			} else if (strncmp(keyBuff, "kick ", 5) == 0) {
				sprintf(playerKick, "%.*s", 32, keyBuff+5);
				newbuflen = strlen(playerKick);
				playerKick[newbuflen - 1] = '\0';
				threadsWaitForMutex();
				transportUdpServerKickPlayer(&serverSim, playerKick);
				threadsReleaseMutex();
			} else if (strncmp(keyBuff, "\n", 1) != 0 && strncmp(keyBuff, "\0", 1) != 0) {
				fprintf(stderr, "Unknown command - Type \"help\" for help\n");
			}
			fgets(keyBuff, 256, stdin);
			strcpy(saveBuff, keyBuff);
			strlower(keyBuff);
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
    while (!(serverSim.state == serverStateGameOver && !serverSim.lobbyEnabled)) {
      if (alarmRaised == alarmInterrupt) {
        break;
      } else if (alarmRaised == alarmLock) {
        threadsWaitForMutex();
        transportUdpServerSetLock(&serverSim, TRUE);
        threadsReleaseMutex();
        alarmRaised = alarmNone;
      } else if (alarmRaised == alarmUnlock) {
        threadsWaitForMutex();
        transportUdpServerSetLock(&serverSim, FALSE);
        threadsReleaseMutex();
        alarmRaised = alarmNone;
      }
      sleep(1);
    }
  } else {
    while (strncmp(keyBuff, "quit", 4) != 0 && !(serverSim.state == serverStateGameOver && !serverSim.lobbyEnabled)) {
      if (strncmp(keyBuff, "help", 4) == 0) {
        printHelp();
      } else if (strncmp(keyBuff, "unlock", 6) == 0) {
        threadsWaitForMutex();
        transportUdpServerSetLock(&serverSim, FALSE);
        threadsReleaseMutex();
      } else if (strncmp(keyBuff, "lock", 4) == 0) {
        threadsWaitForMutex();
        transportUdpServerSetLock(&serverSim, TRUE);
        threadsReleaseMutex();
      } else if (strncmp(keyBuff, "info", 4) == 0) {
        threadsWaitForMutex();
        serverSimInformation(&serverSim, transportUdpServerGetLock());
        threadsReleaseMutex();
      } else if (strncmp(keyBuff, "savemap", 7) == 0) {
        threadsWaitForMutex();
        saveMap(saveBuff);
        threadsReleaseMutex();
      } else if (strncmp(keyBuff, "say ", 4) == 0) {
        transportUdpServerSendServerMessage((char *) keyBuff+4);
      } else if(strncmp(keyBuff, "status", 6) == 0){
        transportUdpServerPrintStatus(statusFile);
      } else if (strncmp(keyBuff, "kick ", 5) == 0) {
        sprintf(playerKick, "%.*s", 32, keyBuff+5);
        newbuflen = strlen(playerKick);
        playerKick[newbuflen - 1] = '\0';
        threadsWaitForMutex();
        transportUdpServerKickPlayer(&serverSim, playerKick);
        threadsReleaseMutex();
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
  DWORD tick;     /* Number of ticks passed */
  static int trackerTime = 5500;   /* When we should update the tracker */
  static int wbnTime = 0;

  tick = winboloTimer();
#else
  Uint32 SDLCALL serverGameTimer(void *userdata, SDL_TimerID timerID, Uint32 interval) {
  (void)userdata; (void)timerID;
  DWORD tick;     /* Number of ticks passed */
  static int wbnTime = 0;
  static int trackerTime = 5500;   /* When we should update the tracker */
  tick = winboloTimer();
#endif

  if ((tick - oldTick) > SERVER_TICK_LENGTH) {
    /* Get the keyboard state */
    while ((tick - oldTick) > SERVER_TICK_LENGTH) {
      trackerTime++;
      wbnTime++;

      threadsWaitForMutex();
      /* Receive packets — queues inputs for both ticks */
      transportUdpServerRecv(&serverSim);

      if (serverSim.state == serverStateRunning) {
        /* Run brain AI bots — queues two InputPackets per bot (keys + game) */
        if (botManagerGetNumBots() > 0) {
          botManagerTick(&serverSim, serverSim.botAiType);
        }
        /* Run two sim ticks per 20ms callback to match the client's
         * 100Hz rate (alternating keys tick + game tick).
         * Drain events after each tick so they're captured before
         * the next tick clears the event buffer. */
        {
          ServerState preTickState = serverSim.state;
          serverSimTick(&serverSim);
          /* If game ended during this tick, broadcast game-over */
          if (preTickState == serverStateRunning && serverSim.state == serverStateGameOver) {
            if (serverSim.lobbyEnabled) {
              /* Capture win message now while game state is intact;
               * it will be sent after players return to the lobby. */
              serverSimBuildWinMessage(&serverSim,
                                       serverSim.pendingWinMessage,
                                       sizeof(serverSim.pendingWinMessage));
              serverSimSendWbnWinEvents(&serverSim);
              transportUdpServerBroadcastGameOver(&serverSim);
            }
          }
          if (serverSim.state == serverStateRunning) {
            transportUdpServerDrainEvents(&serverSim);
          }
        }
        if (serverSim.state == serverStateRunning) {
          /* Save tick 1's events so bots can see them next frame.
           * transportUdpServerDrainEvents already captured them for
           * UDP clients, but bots read directly from the event buffer
           * via serverSimBuildSnapshot — tick 2 would clear these. */
          GameEvent savedEvents[MAX_SNAPSHOT_EVENTS];
          uint8_t savedCount = serverSim.eventCount;
          ServerState preTickState;
          if (savedCount > 0) {
            memcpy(savedEvents, serverSim.events,
                   savedCount * sizeof(GameEvent));
          }
          preTickState = serverSim.state;
          serverSimTick(&serverSim);
          /* If game ended during this tick, broadcast game-over */
          if (preTickState == serverStateRunning && serverSim.state == serverStateGameOver) {
            if (serverSim.lobbyEnabled) {
              serverSimBuildWinMessage(&serverSim,
                                       serverSim.pendingWinMessage,
                                       sizeof(serverSim.pendingWinMessage));
              serverSimSendWbnWinEvents(&serverSim);
              transportUdpServerBroadcastGameOver(&serverSim);
            }
          }
          if (serverSim.state == serverStateRunning) {
            transportUdpServerDrainEvents(&serverSim);
            /* Prepend tick 1's events before tick 2's events */
            if (savedCount > 0 && savedCount + serverSim.eventCount <= MAX_SNAPSHOT_EVENTS) {
              memmove(serverSim.events + savedCount, serverSim.events,
                      serverSim.eventCount * sizeof(GameEvent));
              memcpy(serverSim.events, savedEvents,
                     savedCount * sizeof(GameEvent));
              serverSim.eventCount += savedCount;
            }
          }
        }
        /* Send snapshots only if still running */
        if (serverSim.state == serverStateRunning) {
          transportUdpServerSend(&serverSim);
        }
      } else {
        /* Lobby/countdown/gameover: single tick for state machine processing */
        ServerState preTickState = serverSim.state;
        serverSimTick(&serverSim);

        /* Check if a balance proposal just completed */
        if (serverSim.balanceProposal.broadcastNeeded) {
          transportUdpServerBroadcastBalanceProposal(&serverSim, serverSim.balanceProposal.teamForSlot);
          serverSim.balanceProposal.broadcastNeeded = false;
        }

        /* Handle state transitions */
        if (preTickState == serverStateCountdown) {
          if (serverSim.state == serverStateRunning) {
            /* Countdown finished — game started */
            transportUdpServerBroadcastGameStart(&serverSim);
            if (botManagerGetNumBots() > 0) {
              botManagerOnGameStart(&serverSim);
            }
            /* Notify WBN that we are now in-game */
            winbolonetSendLobbyStatus(FALSE);
            /* Send EVENT_PLAYER_JOIN for each connected WBN player */
            {
              BYTE pi;
              for (pi = 0; pi < MAX_TANKS; pi++) {
                if (serverSim.playerConnected[pi] &&
                    winboloNetIsPlayerParticipant(pi)) {
                  winbolonetAddEvent(WINBOLO_NET_EVENT_PLAYER_JOIN, TRUE,
                                     pi, WINBOLO_NET_NO_PLAYER);
                }
              }
            }
          } else if (serverSim.state == serverStateCountdown &&
                     serverSim.countdownTicks > 0 &&
                     serverSim.countdownTicks % 50 == 0) {
            /* Broadcast countdown tick (once per second) */
            uint8_t secs = (uint8_t)(serverSim.countdownTicks / 50);
            transportUdpServerBroadcastCountdown(&serverSim, secs);
          }
        }
        if (preTickState == serverStateGameOver &&
            serverSim.state == serverStateLobby) {
          /* Flush remaining WBN events (win, final kills, etc.) */
          winbolonetServerUpdate(serverSimGetNumPlayers(&serverSim),
                                 serverSimGetNumNeutralBases(&serverSim),
                                 serverSimGetNumNeutralPills(&serverSim), TRUE);
          /* Pick next map from rotation if mapdir is configured */
          if (serverSim.mapDirFiles != NULL) {
            serverSimMapDirPickRandom(&serverSim);
            transportUdpServerNotifyMapChange(&serverSim);
          }
          /* Re-register with WBN for the new round */
          if (winbolonetIsRunning()) {
            winbolonetReturnToLobby(
              serverSim.mapName, serverSim.serverPort,
              (BYTE)gameTypeGet(&serverSim.sim.game),
              (BYTE)serverSim.botAiType,
              (BYTE)serverSim.sim.hiddenMines,
              serverSim.hasPassword,
              basesGetNumBases(&serverSim.sim.bs),
              pillsGetNumPills(&serverSim.sim.pb),
              serverSimGetNumNeutralBases(&serverSim),
              serverSimGetNumNeutralPills(&serverSim),
              serverSimGetNumPlayers(&serverSim));
          }
          /* Returned to lobby — broadcast full lobby state */
          transportUdpServerBroadcastLobbyState(&serverSim);
          /* Send the win message now that players are back in the lobby */
          if (serverSim.pendingWinMessage[0] != '\0') {
            transportUdpServerSendServerMessage(serverSim.pendingWinMessage);
            serverSim.pendingWinMessage[0] = '\0';
          }
        }

        /* Periodic lobby snapshot — twice per second (every 25 ticks)
         * for ping/country updates and state consistency */
        if ((serverSim.state == serverStateLobby || serverSim.state == serverStateCountdown) &&
            serverSim.tick % 25 == 0) {
          transportUdpServerBroadcastLobbyState(&serverSim);
        }

        /* Timeout check — not called via transportUdpServerSend() during lobby */
        transportUdpServerCheckTimeouts(&serverSim);
      }

      /* Auto-close check — works in any state.
       * When auto-close triggers, force a no-lobby shutdown regardless
       * of lobby mode, since there are no players to return to lobby for. */
      if (serverSim.autoCloseOnEmpty && serverSimCheckAutoClose(&serverSim)) {
        serverSim.lobbyEnabled = FALSE;
        serverSimEnterGameOver(&serverSim);
      }

      /* Empty reset check — when enabled and no players are connected,
       * count down and reset to lobby with map reload after the timeout.
       * Skipped if autoCloseOnEmpty is active (it takes priority). */
      if (serverSim.emptyResetEnabled && !serverSim.autoCloseOnEmpty &&
          serverSim.lobbyEnabled &&
          serverSim.state != serverStateGameOver &&
          serverSimCheckEmptyReset(&serverSim)) {
        serverSimConsoleMessage("Empty reset timer expired. Resetting to lobby...");
        serverSimResetGameWorld(&serverSim);
        serverSim.state = serverStateLobby;
        serverSim.gameLength = serverSim.originalGameLength;
        serverSim.hadPlayersEver = FALSE;
        serverSim.emptyResetTicks = -1;
        /* Pick next map from rotation if mapdir is configured */
        if (serverSim.mapDirFiles != NULL) {
          serverSimMapDirPickRandom(&serverSim);
          transportUdpServerNotifyMapChange(&serverSim);
        }
        /* Re-register with WBN for the new round */
        if (winbolonetIsRunning()) {
          winbolonetReturnToLobby(
            serverSim.mapName, serverSim.serverPort,
            (BYTE)gameTypeGet(&serverSim.sim.game),
            (BYTE)serverSim.botAiType,
            (BYTE)serverSim.sim.hiddenMines,
            serverSim.hasPassword,
            basesGetNumBases(&serverSim.sim.bs),
            pillsGetNumPills(&serverSim.sim.pb),
            serverSimGetNumNeutralBases(&serverSim),
            serverSimGetNumNeutralPills(&serverSim),
            serverSimGetNumPlayers(&serverSim));
        }
        transportUdpServerBroadcastLobbyState(&serverSim);
      }

      threadsReleaseMutex();
      ticks++;
      oldTick += SERVER_TICK_LENGTH;
    }
  }

  if (wbnTime > 100) {
    threadsWaitForMutex();
    winbolonetServerUpdate(serverSimGetNumPlayers(&serverSim), serverSimGetNumNeutralBases(&serverSim), serverSimGetNumNeutralPills(&serverSim), FALSE);
    threadsReleaseMutex();
    wbnTime = 0;
  }

  if (trackerTime >= 6000 && sTrackerUse) {
    transportUdpServerSendTrackerUpdate(&serverSim, sTrackerAddr, sTrackerPort);
    trackerTime = 0;
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
  fprintf(stderr, "-log          - Create game log file (filename optional)\n");
  fprintf(stderr, "-dontsendlog  - Don't upload game log to winbolo.net\n");
  fprintf(stderr, "-statusFile	 - Save list of unlocked players to a file.\n");
  fprintf(stderr, "-bots <N>     - Number of AI bot players to add (default: 0)\n");
  fprintf(stderr, "-brain <path> - Path to the Lua brain script for bots\n");
  fprintf(stderr, "-nolobby      - Skip lobby, start game immediately (backward-compatible mode)\n");
  fprintf(stderr, "-quitonwin    - Quit server when a player/alliance wins\n");
  fprintf(stderr, "-noemptyreset - Disable automatic lobby reset when server is empty\n");
  fprintf(stderr, "                (enabled by default, resets after 5 minutes)\n");
  fprintf(stderr, "-emptyresetmins <N> - Minutes before empty server resets to lobby (default: 5)\n");
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

  serverMessageSetQuietMode(FALSE);
  isQuiet = FALSE;
  isNoInput = FALSE;
  maxPlayers = 0;

  alarmRaised = alarmNone;
  initWinboloTimer();
#ifndef _WIN32
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
  serverSim.botAiType = ai;

  /* Copy tracker settings to file-scope globals for the timer */
  sTrackerUse = trackerUse;
  if (trackerUse) {
    strncpy(sTrackerAddr, trackerAddr, FILENAME_MAX - 1);
    sTrackerAddr[FILENAME_MAX - 1] = '\0';
    sTrackerPort = trackerPort;
  }

  if (argExist(argc, argv, "quiet") == TRUE) {
    serverMessageSetQuietMode(TRUE);
    isQuiet = TRUE;
  }
  isNoInput = argExist(argc, argv, "noinput");
  if (findArg(argc, argv, "logfile") != ARG_NOT_FOUND) {
    serverMessagesSetLogFile((char *) argv[findArg(argc, argv, "logfile")]);
  }

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
  if (geoLookupCreate("data/dbip-country-lite.mmdb")) {
    serverMessageConsoleMessage("Geo lookup database loaded.\n");
  } else {
    serverMessageConsoleMessage("Geo lookup database not found — country codes will be XX.\n");
  }

  /* Create server simulation */
  if (strcmp(mapName, "-inbuilt") == 0) {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4305)
#endif
    BYTE emap[6000] = E_MAP;
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    if (serverSimCreateCompressed(&serverSim, emap, 5097, game, hiddenMines, srtDelay, gmeLen) == FALSE) {
      fprintf(stderr, "Error starting server simulation (inbuilt map)\n");
#ifdef USING_SDL
      SDL_Quit();
#endif
      return 0;
    }
    strncpy(serverSim.mapName, "Everard Island", MAP_STR_SIZE - 1);
    serverSim.mapName[MAP_STR_SIZE - 1] = '\0';
  } else if (strcmp(mapName, "-mapdir") == 0) {
    /* -mapdir without -map: build the map list into a temporary, then pick
     * a random initial map. serverSimCreate zeroes the struct, so we restore
     * the list after creation. */
    char **savedFiles;
    int savedCount;
    int mdArg = findArg(argc, argv, "mapdir");
    srand((unsigned int)time(NULL));
    if (serverSimMapDirBuild(&serverSim, (char *)argv[mdArg]) == FALSE) {
#ifdef USING_SDL
      SDL_Quit();
#endif
      return 0;
    }
    savedFiles = serverSim.mapDirFiles;
    savedCount = serverSim.mapDirCount;
    if (serverSimCreate(&serverSim, savedFiles[rand() % savedCount], game, hiddenMines, srtDelay, gmeLen) == FALSE) {
      fprintf(stderr, "Error starting server simulation\n");
#ifdef USING_SDL
      SDL_Quit();
#endif
      return 0;
    }
    serverSim.mapDirFiles = savedFiles;
    serverSim.mapDirCount = savedCount;
  } else {
    if (serverSimCreate(&serverSim, mapName, game, hiddenMines, srtDelay, gmeLen) == FALSE) {
      fprintf(stderr, "Error starting server simulation\n");
#ifdef USING_SDL
      SDL_Quit();
#endif
      return 0;
    }
  }

  useAddr = NULL;
  httpSetAltIpAddress("");
  if (argExist(argc, argv, "addr") == TRUE) {
    useAddr = (char *) argv[findArg(argc, argv, "addr")];
    httpSetAltIpAddress(useAddr);
  }

  statusFile = argExist(argc, argv, "statusFile");
  serverSim.quitOnWin = argExist(argc, argv, "quitonwin");
  serverSim.autoCloseOnEmpty = argExist(argc, argv, "autoclose");

  /* Empty reset configuration — on by default */
  if (argExist(argc, argv, "noemptyreset") == TRUE) {
    serverSim.emptyResetEnabled = FALSE;
  }
  {
    int argNum = findArg(argc, argv, "emptyresetmins");
    if (argNum != ARG_NOT_FOUND) {
      int mins = atoi((char *)argv[argNum]);
      if (mins > 0) {
        serverSim.emptyResetMinutes = mins;
      }
    }
  }

  /* -nolobby: skip lobby, start running immediately (backward-compatible mode) */
  if (argExist(argc, argv, "nolobby") == TRUE) {
    serverSim.lobbyEnabled = FALSE;
    serverSim.emptyResetEnabled = FALSE;
    serverSim.state = serverStateRunning;
  }

  /* -mapdir: build validated map list for rotation between rounds.
   * Skip if already built (the -mapdir without -map case builds it earlier). */
  {
    int argNum = findArg(argc, argv, "mapdir");
    if (argNum != ARG_NOT_FOUND && serverSim.mapDirFiles == NULL) {
      if (!serverSim.lobbyEnabled) {
        fprintf(stderr, "Error: -mapdir requires lobby mode (incompatible with -nolobby)\n");
#ifdef USING_SDL
        SDL_Quit();
#endif
        return 0;
      }
      if (serverSimMapDirBuild(&serverSim, (char *)argv[argNum]) == FALSE) {
#ifdef USING_SDL
        SDL_Quit();
#endif
        return 0;
      }
    }
  }

  /* Create UDP transport */
  if (transportUdpServerCreate(port, useAddr, &serverSim, pass, (BYTE)maxPlayers) == FALSE) {
    fprintf(stderr, "Error creating network transport\n");
    serverSimDestroy(&serverSim);
#ifdef USING_SDL
    SDL_Quit();
#endif
    return 0;
  }

  /* WinBolo.net host override */
  {
    int argNum = findArg(argc, argv, "wbnhost");
    if (argNum != ARG_NOT_FOUND) {
      httpSetHostOverride((char *)argv[argNum]);
    }
  }

  serverSim.hasPassword = (pass[0] != '\0');
  if (argExist(argc, argv, "nowinbolonet") == FALSE) {
    winbolonetCreateServer(serverSim.mapName, port, (BYTE) game, (BYTE) ai, (BYTE) hiddenMines, (BYTE) serverSim.hasPassword, basesGetNumBases(&serverSim.sim.bs), pillsGetNumPills(&serverSim.sim.pb), serverSimGetNumNeutralBases(&serverSim), serverSimGetNumNeutralPills(&serverSim), serverSimGetNumPlayers(&serverSim));
    /* If no lobby, immediately mark as in-game on WBN */
    if (!serverSim.lobbyEnabled) {
      winbolonetSendLobbyStatus(FALSE);
    }
  }
  dontSendLog = argExist(argc, argv, "dontsendlog");

  /* Log file generation — not yet supported, will be re-implemented against ServerSim */
  if (argExist(argc, argv, "log") == TRUE) {
    fprintf(stderr, "Warning: -log not yet supported, logging disabled\n");
  }

  /* Initialize and add bot players */
  botManagerInit();
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
    strncpy(serverSim.botBrainPath, brainPath, sizeof(serverSim.botBrainPath) - 1);
    serverSim.botBrainPath[sizeof(serverSim.botBrainPath) - 1] = '\0';
    if (numBots > 0 && brainPath[0] != '\0') {
      int i;
      char botName[64];
      for (i = 0; i < numBots; i++) {
        snprintf(botName, sizeof(botName), "Bot %d", i + 1);
        if (!botManagerAddBot(&serverSim, (BYTE)i, brainPath, botName, ai, game, hiddenMines)) {
          fprintf(stderr, "Warning: failed to add bot %d\n", i);
        }
      }
      fprintf(stderr, "Added %d bot(s) with brain '%s'\n", numBots, brainPath);
    } else if (numBots > 0) {
      fprintf(stderr, "Warning: -bots specified but no -brain path given\n");
    }
  }

  if (threadsCreate(TRUE) == FALSE) {
    fprintf(stderr, "Error starting Thread Manager\n");
    threadsDestroy();
    transportUdpServerDestroy();
    botManagerDestroy(&serverSim);
    serverSimDestroy(&serverSim);
#ifdef USING_SDL
    SDL_Quit();
#endif
    return 0;
  }
  serverMessageConsoleMessage("Type \"help\" for help, \"quit\" to exit.");
#ifdef _WIN32
  oldTick = winboloTimer();
  serverTimerGameID = timeSetEvent(SERVER_TICK_LENGTH, 10, serverGameTimer, 0, TIME_PERIODIC);
#else
  oldTick = winboloTimer();
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

  winbolonetDestroy(TRUE);

  if (isLogging == TRUE && key[0] != EMPTY_CHAR && argExist(argc, argv, "dontsendlog") == FALSE) {
    serverMessageConsoleMessage((char *)"Uploading log file to winbolo.net");
    httpCreate();
    httpSendLogFile(fileName, key, FALSE);
    httpDestroy();
  }
  endWinboloTimer();
  geoLookupDestroy();
  transportUdpServerDestroy();
  botManagerDestroy(&serverSim);
  serverSimMapDirDestroy(&serverSim);
  serverSimDestroy(&serverSim);
#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}

time_t serverMainGetTicks() {
  return ticks;
}
