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
*Name:          Headless Client
*Filename:      headless_main.c
*Purpose:
*  Headless WinBolo client for automated testing.
*
*  Network mode:
*    WinBoloHeadless --server HOST --port PORT [options]
*
*  Fast local mode (runs as fast as CPU allows):
*    WinBoloHeadless --fast --map FILE [options]
*
*  Common options:
*    --name NAME       Player name (default: "HeadlessBot")
*    --brain PATH      Path to Lua brain script
*    --ticks N         Run for N game ticks then exit (0 = unlimited)
*    --ai TYPE         AI type: yes (default), full, advantage, no
*    --log-state FILE  Log verbose JSON state each tick (- for stdout)
*    --log-state binary  Output fixed-size binary observation frames to stdout
*    --quiet           Suppress non-error output
*
*  Network-only options:
*    --server HOST     Server address
*    --port PORT       Server port
*    --password PASS   Server password
*
*  Fast-only options:
*    --fast            Enable fast local mode (no wall-clock gating)
*    --map FILE        Path to .map file (required with --fast)
*    --stdin           Read input from stdin (one JSON line per game tick)
*********************************************************/

#ifdef _MSC_VER
#include <crtdbg.h>
#endif
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include "cJSON.h"

#include "bolo_rand.h"
#include "brain_data.h"
#include "client_sim.h"
#include "control_event.h"
#include "discovery.h"
#include "frontend.h"
#include "../gui/lang.h"
#include "brain.h"
#include "client_net.h"
#include "gui_message.h"
#include "server_sim.h"
#include "../bolo/internal/server_sim_lifecycle.h"
#include "../server/server_lifecycle.h"
#include "../gui/brainsHandler.h"
#include "../gui/clientmutex.h"
#include "../gui/gamefront.h"
#include "../common/sentry_integration.h"
#include "../common/wb_log.h"
#include "../common/prefs.h"
#include "../winbolonet/winbolonet_core.h"
#include "cmd_stdin.h"

/* ------------------------------------------------------------------ */
/* Globals needed by the game engine                                   */
/* ------------------------------------------------------------------ */

/* These are referenced by various parts of the engine */
bool isInMenu = FALSE;

/* State logging */
static FILE *logFile = NULL;
static bool logToStdout = FALSE;

/* Control-event logging (parallel to --log-state, distinct stream).
 * The events log records the ControlEvent stream arriving at the
 * local ClientSim via clientSimApplyControl, observed through
 * clientSimSetControlObserver. Used to diff event delivery between
 * --fast (server-bus → in-process subscriber → ClientSim) and
 * --server (UDP decoder → ClientSim) modes. */
static FILE *logEventsFile = NULL;
static bool  logEventsToStdout = FALSE;

/* Command-line options */
static char optServer[256] = "";
static unsigned short optPort = 27500;
static char optTrackerAddr[256] = "";
static unsigned short optTrackerPort = 0;
static char optName[64] = "HeadlessBot";
static char optBrain[512] = "";
static int optTicks = 0; /* 0 = unlimited */
static char optPassword[256] = "";
static char optLogState[512] = "";
static char optLogEvents[512] = "";
static char optCmdStdin[512] = "";
static bool optQuiet = FALSE;
static bool optFast = FALSE;
static char optMap[512] = "";
static bool optStdin = FALSE;
static bool optLogBinary = FALSE;
static uint64_t optSeed = 0;
static bool optSeedSet = FALSE;
static aiType optAi = aiYes;

/* Binary observation format constants */
#define BINARY_SPATIAL_SIZE 29
#define BINARY_NUM_CHANNELS 10
#define BINARY_NUM_SCALARS  17
#define BINARY_MAX_EVENTS   16
static gameType optGameType = gameStrictTournament;

/* Quit flag for signal handler */
static volatile bool headlessQuit = FALSE;

/* Transport state — the Transport handle itself now lives inside
 * humanSim; this flag only tracks high-level lifecycle gating. */
static bool transportActive = FALSE;
static SubscriberHandle headlessControlSub = SUBSCRIBER_HANDLE_INVALID;

static BYTE playerNum = 0;
static ClientSim *humanSim = NULL;

/* Fast mode: local server sim */
static ServerSim *fastServerSim = NULL;

/* Scripted command stream (NULL unless --cmd-stdin was supplied). */
static CmdStdin *cmdStream = NULL;

/* ------------------------------------------------------------------ */
/* Signal handler for clean shutdown                                   */
/* ------------------------------------------------------------------ */

static void signalHandler(int sig) {
  (void)sig;
  headlessQuit = TRUE;
}

/* ------------------------------------------------------------------ */
/* Message handler (replaces SDL message box)                          */
/* ------------------------------------------------------------------ */

static void headlessMessageHandler(const char *message, const char *title) {
  if (!optQuiet) {
    fprintf(stderr, "[%s] %s\n", title ? title : "WinBolo", message ? message : "");
  }
}

/* ------------------------------------------------------------------ */
/* State logging                                                       */
/* ------------------------------------------------------------------ */

static void logStateOpen(const char *path) {
  if (path[0] == '\0') {
    return;
  }
  if (strcmp(path, "binary") == 0) {
    optLogBinary = TRUE;
    logToStdout = TRUE;
    logFile = stdout;
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    return;
  }
  if (strcmp(path, "-") == 0) {
    logToStdout = TRUE;
    logFile = stdout;
  } else {
    logFile = fopen(path, "w");
    if (logFile == NULL) {
      fprintf(stderr, "Error: cannot open log file '%s'\n", path);
    }
  }
}

static void logStateClose(void) {
  if (logFile != NULL && !logToStdout) {
    fclose(logFile);
  }
  logFile = NULL;
}

/* ------------------------------------------------------------------ */
/* Control-event logging                                               */
/* ------------------------------------------------------------------ */
/*
 * Each line is one JSON object per ControlEvent the local ClientSim
 * receives. Field order is fixed (declaration-order from
 * control_event.h's union, with tick + type first). Strings are
 * JSON-escaped; bools are true/false; named enums render as their
 * symbolic name. This serializer is intentionally local to the test
 * harness: when the wire codec lands, sharing code would defeat the
 * purpose of using the goldens to diff in-process vs UDP delivery.
 */

static void logEventsOpen(const char *path) {
  if (path[0] == '\0') {
    return;
  }
  if (strcmp(path, "-") == 0) {
    logEventsToStdout = TRUE;
    logEventsFile = stdout;
  } else {
    logEventsFile = fopen(path, "w");
    if (logEventsFile == NULL) {
      fprintf(stderr, "Error: cannot open events log file '%s'\n", path);
    }
  }
}

static void logEventsClose(void) {
  if (logEventsFile != NULL && !logEventsToStdout) {
    fclose(logEventsFile);
  }
  logEventsFile = NULL;
}

/* Write a JSON-quoted string. Escapes ", \, and control chars 0x00-0x1F.
 * maxLen caps the read in case the source field is a fixed-size buffer
 * that may not be NUL-terminated. */
static void logEventsJsonStr(FILE *f, const char *s, size_t maxLen) {
  size_t i;
  fputc('"', f);
  for (i = 0; i < maxLen && s[i] != '\0'; i++) {
    unsigned char c = (unsigned char)s[i];
    switch (c) {
      case '"':  fputs("\\\"", f); break;
      case '\\': fputs("\\\\", f); break;
      case '\b': fputs("\\b", f);  break;
      case '\f': fputs("\\f", f);  break;
      case '\n': fputs("\\n", f);  break;
      case '\r': fputs("\\r", f);  break;
      case '\t': fputs("\\t", f);  break;
      default:
        if (c < 0x20) {
          fprintf(f, "\\u%04x", (unsigned)c);
        } else {
          fputc((int)c, f);
        }
    }
  }
  fputc('"', f);
}

static const char *logEventsTypeName(int type) {
  switch (type) {
    case CTRL_ALLIANCE_REQUEST:      return "CTRL_ALLIANCE_REQUEST";
    case CTRL_ALLIANCE_ACCEPT:       return "CTRL_ALLIANCE_ACCEPT";
    case CTRL_ALLIANCE_LEAVE:        return "CTRL_ALLIANCE_LEAVE";
    case CTRL_ALLIANCE_RESET:        return "CTRL_ALLIANCE_RESET";
    case CTRL_PLAYER_JOIN:           return "CTRL_PLAYER_JOIN";
    case CTRL_PLAYER_LEAVE:          return "CTRL_PLAYER_LEAVE";
    case CTRL_PLAYER_NAME:           return "CTRL_PLAYER_NAME";
    case CTRL_LOBBY_SLOT:            return "CTRL_LOBBY_SLOT";
    case CTRL_LOBBY_SETTINGS:        return "CTRL_LOBBY_SETTINGS";
    case CTRL_LOBBY_MAP_CHANGE:      return "CTRL_LOBBY_MAP_CHANGE";
    case CTRL_MAP_DOWNLOAD_COMPLETE: return "CTRL_MAP_DOWNLOAD_COMPLETE";
    case CTRL_BALANCE_PROPOSAL:      return "CTRL_BALANCE_PROPOSAL";
    case CTRL_MAP_SKIP_STATE:        return "CTRL_MAP_SKIP_STATE";
    case CTRL_GAME_PHASE_LOBBY:
    case CTRL_GAME_PHASE_COUNTDOWN:
    case CTRL_GAME_PHASE_RUNNING:
    case CTRL_GAME_PHASE_GAME_OVER:  return "CTRL_GAME_PHASE";
    case CTRL_GAME_OVER:             return "CTRL_GAME_OVER";
    case CTRL_SERVER_SHUTDOWN:       return "CTRL_SERVER_SHUTDOWN";
    case CTRL_LOBBY_SYNC_COMPLETE:   return "CTRL_LOBBY_SYNC_COMPLETE";
    case CTRL_CHAT:                  return "CTRL_CHAT";
    case CTRL_LOBBY_TEAM_META:       return "CTRL_LOBBY_TEAM_META";
    case CTRL_LOBBY_BOT_CONFIG:      return "CTRL_LOBBY_BOT_CONFIG";
    case CTRL_LOBBY_BOT_BRAIN:       return "CTRL_LOBBY_BOT_BRAIN";
    case CTRL_LOBBY_BRAIN_LIST:      return "CTRL_LOBBY_BRAIN_LIST";
    case CTRL_GAME_VOTE_STATE:       return "CTRL_GAME_VOTE_STATE";
    case CTRL_SERVER_TEXT:           return "CTRL_SERVER_TEXT";
    case CTRL_COMMAND_REJECTED:      return "CTRL_COMMAND_REJECTED";
    case CTRL_BALANCE_FAILED:        return "CTRL_BALANCE_FAILED";
    case CTRL_ROUND_STATS:           return "CTRL_ROUND_STATS";
    case CTRL_ROUND_RATING_POSTED:   return "CTRL_ROUND_RATING_POSTED";
    default:                         return NULL;
  }
}

static void logEventsDeliverCb(void *ctx, const ControlEvent *evt) {
  FILE *f = (FILE *)ctx;
  uint32_t tick;
  const char *typeName;

  if (f == NULL || evt == NULL) return;

  /* The server streams its bot-name pool catalog to every joiner as
   * CTRL_LOBBY_BOT_POOL_CHUNK fragments during lobby sync. That is cosmetic
   * lobby data, not a game/control event these baselines assert, and its
   * fragment count tracks data/bot_names.json — so drop it from the captured
   * stream to keep the baselines stable and content-independent. */
  if (evt->type == CTRL_LOBBY_BOT_POOL_CHUNK) return;

  /* Tick numbers come from the ClientSim's last-server-tick counter,
   * which both modes agree on (set by snapshot ingestion in --fast
   * and --server alike). Pre-snapshot events log tick 0. */
  tick = humanSim != NULL ? clientSimGetLastServerTick(humanSim) : 0;
  typeName = logEventsTypeName((int)evt->type);

  fputc('{', f);
  if (typeName != NULL) {
    fprintf(f, "\"tick\":%u,\"type\":\"%s\"", (unsigned)tick, typeName);
  } else {
    fprintf(f, "\"tick\":%u,\"type\":\"UNKNOWN\",\"raw\":%d",
            (unsigned)tick, (int)evt->type);
  }

  switch (evt->type) {
    case CTRL_ALLIANCE_REQUEST:
      fprintf(f, ",\"fromPlayer\":%u,\"toPlayer\":%u",
              (unsigned)evt->u.allianceRequest.fromPlayer,
              (unsigned)evt->u.allianceRequest.toPlayer);
      break;

    case CTRL_ALLIANCE_ACCEPT:
      fprintf(f, ",\"acceptedBy\":%u,\"newMember\":%u",
              (unsigned)evt->u.allianceAccept.acceptedBy,
              (unsigned)evt->u.allianceAccept.newMember);
      break;

    case CTRL_ALLIANCE_LEAVE:
      fprintf(f, ",\"playerNum\":%u",
              (unsigned)evt->u.allianceLeave.playerNum);
      break;

    case CTRL_ALLIANCE_RESET: {
      BYTE k;
      fprintf(f, ",\"allies\":[");
      for (k = 0; k < MAX_TANKS; k++) {
        fprintf(f, "%s%u", (k == 0) ? "" : ",",
                (unsigned)evt->u.allianceReset.allies[k]);
      }
      fprintf(f, "]");
      break;
    }

    case CTRL_PLAYER_JOIN: {
      BYTE k;
      fprintf(f, ",\"playerNum\":%u,\"name\":",
              (unsigned)evt->u.playerJoin.playerNum);
      logEventsJsonStr(f, evt->u.playerJoin.name, PACKET_MAX_PLAYER_NAME);
      fprintf(f, ",\"country\":");
      logEventsJsonStr(f, evt->u.playerJoin.country,
                       sizeof(evt->u.playerJoin.country));
      fprintf(f, ",\"clientType\":%u,\"clientFlags\":%u,\"allies\":[",
              (unsigned)evt->u.playerJoin.clientType,
              (unsigned)evt->u.playerJoin.clientFlags);
      for (k = 0; k < evt->u.playerJoin.numAllies; k++) {
        fprintf(f, "%s%u", k == 0 ? "" : ",",
                (unsigned)evt->u.playerJoin.allies[k]);
      }
      fputc(']', f);
      break;
    }

    case CTRL_PLAYER_LEAVE:
      fprintf(f, ",\"playerNum\":%u,\"name\":",
              (unsigned)evt->u.playerLeave.playerNum);
      logEventsJsonStr(f, evt->u.playerLeave.name, PACKET_MAX_PLAYER_NAME);
      fprintf(f, ",\"country\":");
      logEventsJsonStr(f, evt->u.playerLeave.country,
                       sizeof(evt->u.playerLeave.country));
      break;

    case CTRL_PLAYER_NAME:
      fprintf(f, ",\"playerNum\":%u,\"name\":",
              (unsigned)evt->u.playerName.playerNum);
      logEventsJsonStr(f, evt->u.playerName.name, PACKET_MAX_PLAYER_NAME);
      break;

    case CTRL_LOBBY_SLOT: {
      const ClientLobbySlot *s = &evt->u.lobbySlot.slot;
      fprintf(f, ",\"playerNum\":%u,\"slot\":{\"connected\":%s,\"playerName\":",
              (unsigned)evt->u.lobbySlot.playerNum,
              s->connected ? "true" : "false");
      logEventsJsonStr(f, s->playerName, PACKET_MAX_PLAYER_NAME);
      fprintf(f, ",\"teamNumber\":%u,\"ready\":%s,\"isBot\":%s,\"pingMs\":%u",
              (unsigned)s->teamNumber,
              s->ready ? "true" : "false",
              s->isBot ? "true" : "false",
              (unsigned)s->pingMs);
      fprintf(f, ",\"countryCode\":");
      logEventsJsonStr(f, s->countryCode, sizeof(s->countryCode));
      fprintf(f, ",\"clientFlags\":%u,\"clientType\":%u}",
              (unsigned)s->clientFlags, (unsigned)s->clientType);
      break;
    }

    case CTRL_LOBBY_SETTINGS:
      fprintf(f, ",\"mapName\":");
      logEventsJsonStr(f, evt->u.lobbySettings.mapName, MAP_STR_SIZE);
      fprintf(f, ",\"lobbyGameType\":%d,\"lobbyHiddenMines\":%s"
                 ",\"lobbyAiType\":%u,\"lobbyTimeLimit\":%d"
                 ",\"lobbyPillCount\":%u,\"lobbyBaseCount\":%u"
                 ",\"lobbyStartCount\":%u,\"mapSkipAvailable\":%s"
                 ",\"netStat\":%d,\"hasLobby\":%s",
              (int)evt->u.lobbySettings.lobbyGameType,
              evt->u.lobbySettings.lobbyHiddenMines ? "true" : "false",
              (unsigned)evt->u.lobbySettings.lobbyAiType,
              (int)evt->u.lobbySettings.lobbyTimeLimit,
              (unsigned)evt->u.lobbySettings.lobbyPillCount,
              (unsigned)evt->u.lobbySettings.lobbyBaseCount,
              (unsigned)evt->u.lobbySettings.lobbyStartCount,
              evt->u.lobbySettings.mapSkipAvailable ? "true" : "false",
              (int)evt->u.lobbySettings.netStat,
              evt->u.lobbySettings.hasLobby ? "true" : "false");
      break;

    case CTRL_LOBBY_MAP_CHANGE:
    case CTRL_MAP_DOWNLOAD_COMPLETE:
    case CTRL_GAME_OVER:
    case CTRL_SERVER_SHUTDOWN:
    case CTRL_LOBBY_SYNC_COMPLETE:
      /* No payload fields. */
      break;

    case CTRL_BALANCE_PROPOSAL: {
      int k;
      fprintf(f, ",\"teamForSlot\":[");
      for (k = 0; k < MAX_TANKS; k++) {
        fprintf(f, "%s%u", k == 0 ? "" : ",",
                (unsigned)evt->u.balanceProposal.teamForSlot[k]);
      }
      fputc(']', f);
      break;
    }

    case CTRL_MAP_SKIP_STATE: {
      int k;
      fprintf(f, ",\"votes\":[");
      for (k = 0; k < MAX_TANKS; k++) {
        fprintf(f, "%s%u", k == 0 ? "" : ",",
                (unsigned)evt->u.mapSkipState.votes[k]);
      }
      fputc(']', f);
      break;
    }

    case CTRL_GAME_PHASE_LOBBY:
      fprintf(f, ",\"phase\":\"LOBBY\",\"countdownSeconds\":0");
      break;
    case CTRL_GAME_PHASE_COUNTDOWN:
      fprintf(f, ",\"phase\":\"COUNTDOWN\",\"countdownSeconds\":%d",
              evt->u.gamePhase.countdownSeconds);
      break;
    case CTRL_GAME_PHASE_RUNNING:
      fprintf(f, ",\"phase\":\"RUNNING\",\"countdownSeconds\":0");
      break;
    case CTRL_GAME_PHASE_GAME_OVER:
      fprintf(f, ",\"phase\":\"GAME_OVER\",\"countdownSeconds\":0");
      break;

    case CTRL_CHAT:
      fprintf(f, ",\"fromPlayer\":%u,\"destPlayer\":%u,\"bodyLen\":%u",
              (unsigned)evt->u.chat.fromPlayer,
              (unsigned)evt->u.chat.destPlayer,
              (unsigned)evt->u.chat.bodyLen);
      break;

    case CTRL_LOBBY_TEAM_META:
      fprintf(f, ",\"teamId\":%u,\"in_use\":%u,\"color\":%u,\"namingPool\":%u"
                 ",\"name\":",
              (unsigned)evt->u.lobbyTeamMeta.teamId,
              (unsigned)evt->u.lobbyTeamMeta.in_use,
              (unsigned)evt->u.lobbyTeamMeta.color,
              (unsigned)evt->u.lobbyTeamMeta.namingPool);
      logEventsJsonStr(f, evt->u.lobbyTeamMeta.name,
                       sizeof(evt->u.lobbyTeamMeta.name));
      break;

    case CTRL_LOBBY_BOT_CONFIG:
      fprintf(f, ",\"slot\":%u,\"difficulty\":%u,\"personality\":%u,\"name\":",
              (unsigned)evt->u.lobbyBotConfig.slot,
              (unsigned)evt->u.lobbyBotConfig.difficulty,
              (unsigned)evt->u.lobbyBotConfig.personality);
      logEventsJsonStr(f, evt->u.lobbyBotConfig.name, PACKET_MAX_PLAYER_NAME);
      break;

    case CTRL_LOBBY_BOT_BRAIN:
      fprintf(f, ",\"slot\":%u,\"brainIdx\":%u",
              (unsigned)evt->u.lobbyBotBrain.slot,
              (unsigned)evt->u.lobbyBotBrain.brainIdx);
      break;

    case CTRL_LOBBY_BRAIN_LIST:
      /* Type-only: both the count and the per-entry payload (path +
       * mtime-derived version) depend on which brain dirs exist where
       * the binary runs from, so they drift across machines and CWDs
       * (project-root vs ~/build-dir). The regression target is that
       * the event fires, not what's in it. */
      break;

    case CTRL_GAME_VOTE_STATE:
      fprintf(f, ",\"kind\":%u,\"active\":%u,\"triggerSrc\":%u,\"teamId\":%u"
                 ",\"threshold\":%u,\"yesCount\":%u,\"noCount\":%u"
                 ",\"eligibleCount\":%u,\"secondsRemaining\":%u,\"votes\":%u",
              (unsigned)evt->u.gameVoteState.kind,
              (unsigned)evt->u.gameVoteState.active,
              (unsigned)evt->u.gameVoteState.triggerSrc,
              (unsigned)evt->u.gameVoteState.teamId,
              (unsigned)evt->u.gameVoteState.threshold,
              (unsigned)evt->u.gameVoteState.yesCount,
              (unsigned)evt->u.gameVoteState.noCount,
              (unsigned)evt->u.gameVoteState.eligibleCount,
              (unsigned)evt->u.gameVoteState.secondsRemaining,
              (unsigned)evt->u.gameVoteState.votes);
      break;

    case CTRL_SERVER_TEXT:
      fputs(",\"text\":", f);
      logEventsJsonStr(f, evt->u.serverText.text,
                       sizeof(evt->u.serverText.text));
      break;

    case CTRL_COMMAND_REJECTED:
      fprintf(f, ",\"origCmdSeq\":%u,\"origCmdType\":%u,"
                 "\"reasonCode\":%u,\"origSlot\":%u",
              (unsigned)evt->u.commandRejected.origCmdSeq,
              (unsigned)evt->u.commandRejected.origCmdType,
              (unsigned)evt->u.commandRejected.reasonCode,
              (unsigned)evt->u.commandRejected.origSlot);
      break;

    case CTRL_BALANCE_FAILED:
      fprintf(f, ",\"reasonCode\":%u",
              (unsigned)evt->u.balanceFailed.reasonCode);
      break;

    case CTRL_SHELL_DEATH:
      fprintf(f, ",\"fireTick\":%u,\"impactWX\":%u,\"impactWY\":%u,"
                 "\"owner\":%u,\"outcome\":%u",
              (unsigned)evt->u.shellDeath.fireTick,
              (unsigned)evt->u.shellDeath.impactWX,
              (unsigned)evt->u.shellDeath.impactWY,
              (unsigned)evt->u.shellDeath.owner,
              (unsigned)evt->u.shellDeath.outcome);
      break;

    case CTRL_ROUND_STATS:
      fprintf(f, ",\"playerCount\":%u,\"awardCount\":%u,\"highlightCount\":%u",
              (unsigned)evt->u.roundStats.playerCount,
              (unsigned)evt->u.roundStats.awardCount,
              (unsigned)evt->u.roundStats.highlightCount);
      break;

    case CTRL_ROUND_RATING_POSTED:
      /* Key prefix only: enough to tell two rounds apart in a trace without
       * putting the whole WinBolo.net key in the log. */
      fprintf(f, ",\"fromPlayer\":%u,\"keyPrefix\":",
              (unsigned)evt->u.ratingPosted.fromPlayer);
      logEventsJsonStr(f, evt->u.ratingPosted.key, 6);
      break;

    case CTRL_EVENT_TYPE_COUNT:
      /* Sentinel — never actually delivered. */
      break;
  }

  fputs("}\n", f);
  fflush(f);
}

/* ------------------------------------------------------------------ */
/* Scripted command dispatch (--cmd-stdin)                             */
/* ------------------------------------------------------------------ */
/*
 * Tick-driven dispatch. Both --fast and --server share the same
 * command vocabulary (parsed in cmd_stdin.c) but route ops
 * differently: --fast hits the in-process ServerSim directly,
 * while --server fans them out through clientSimNetSend* wrappers.
 *
 * Ops invalid for the current mode print to stderr and abort the
 * binary with exit code 2. Silent skip would mask scenario bugs.
 */

static void cmdAbortBadMode(const CmdLine *cmd, const char *mode) {
  fprintf(stderr,
          "cmd-stdin: line %d: op '%s' not valid in %s mode\n",
          cmd->lineNumber, cmdOpName(cmd->op), mode);
  exit(2);
}

/* Scripted command dispatch in --fast mode. The alliance/name
 * helpers go through serverSimAcceptAlliance / serverSimLeaveAlliance /
 * serverSimSetPlayerName, which do the same mutate-then-publish pair
 * that transport_udp_server.c's PACKET_ALLIANCE_* / PACKET_NAME_CHANGE
 * handlers do inline. Alliance-request and map-skip-state have no
 * server-side mutation, so they just publish. */
static void cmdFastPublishAllianceRequest(BYTE fromPlayer, BYTE toPlayer) {
  ControlEvent evt;
  memset(&evt, 0, sizeof(evt));
  evt.type = CTRL_ALLIANCE_REQUEST;
  evt.u.allianceRequest.fromPlayer = fromPlayer;
  evt.u.allianceRequest.toPlayer   = toPlayer;
  serverSimPublishControl(fastServerSim, &evt);
}

static void cmdFastPublishAllianceAccept(BYTE acceptedBy, BYTE newMember) {
  serverSimAcceptAlliance(fastServerSim, acceptedBy, newMember);
}

static void cmdFastPublishAllianceLeave(BYTE playerNum) {
  serverSimLeaveAlliance(fastServerSim, playerNum);
}

static void cmdFastPublishPlayerName(BYTE playerNum, const char *name) {
  serverSimSetPlayerName(fastServerSim, playerNum, name);
}

static void cmdFastPublishMapSkipState(void) {
  ControlEvent evt;
  int i;
  memset(&evt, 0, sizeof(evt));
  evt.type = CTRL_MAP_SKIP_STATE;
  for (i = 0; i < MAX_TANKS; i++) {
    evt.u.mapSkipState.votes[i] =
        serverSimIsMapSkipVote(fastServerSim, (BYTE)i) ? 1 : 0;
  }
  serverSimPublishControl(fastServerSim, &evt);
}

/* Dispatch a single cmd in --fast mode. Returns true if the loop
 * should keep running, false on "exit". */
static bool cmdDispatchFast(const CmdLine *cmd) {
  switch (cmd->op) {
    case CMD_OP_ADD_BOT: {
      ServerSimBotConfig cfg;
      char nameBuf[PACKET_MAX_PLAYER_NAME];
      BYTE slot;
      memset(&cfg, 0, sizeof(cfg));
      /* Find the first unconnected slot (1..MAX_TANKS-1 — slot 0
       * is the local "player"). The scenarios only need a slot to
       * exist; the bot's brain never runs because we route ops
       * through serverSimAddBot rather than botManagerAddBot. */
      for (slot = 1; slot < MAX_TANKS; slot++) {
        if (!serverSimIsPlayerConnected(fastServerSim, slot)) break;
      }
      if (slot >= MAX_TANKS) {
        fprintf(stderr, "cmd-stdin: line %d: add_bot: no free slot\n",
                cmd->lineNumber);
        exit(2);
      }
      snprintf(nameBuf, sizeof(nameBuf), "Bot %u", (unsigned)slot);
      cfg.brainPath  = "(cmd-stdin)";
      cfg.brainName  = nameBuf;
      cfg.ai         = optAi;
      cfg.gameType   = optGameType;
      cfg.hiddenMines= false;
      cfg.teamNumber = 0;
      serverSimAddBot(fastServerSim, slot, &cfg);
      return true;
    }
    case CMD_OP_SET_TEAM:
      serverSimSetTeam(fastServerSim, cmd->slot, cmd->team);
      return true;
    case CMD_OP_SET_READY:
      serverSimSetReady(fastServerSim, cmd->slot, cmd->ready);
      return true;
    case CMD_OP_NAME_CHANGE:
      cmdFastPublishPlayerName(cmd->slot, cmd->name);
      return true;
    case CMD_OP_ALLIANCE_REQUEST:
      cmdFastPublishAllianceRequest(cmd->from, cmd->to);
      return true;
    case CMD_OP_ALLIANCE_ACCEPT:
      cmdFastPublishAllianceAccept(cmd->from, cmd->to);
      return true;
    case CMD_OP_ALLIANCE_LEAVE:
      cmdFastPublishAllianceLeave(cmd->slot);
      return true;
    case CMD_OP_MAP_SKIP_VOTE:
      serverSimMapSkipVoteToggle(fastServerSim, cmd->slot);
      cmdFastPublishMapSkipState();
      return true;
    case CMD_OP_START_GAME:
      serverSimStartGame(fastServerSim);
      return true;
    case CMD_OP_REAPPLY_ALLIANCES:
      serverSimReapplyTeamAlliances(fastServerSim);
      return true;
    case CMD_OP_SHUTDOWN:
      /* No symmetric "stop now" path on the in-process sim — the
       * server is destroyed at process exit. Treat as a no-op. */
      return true;
    case CMD_OP_EXIT:
      return false;
    default:
      cmdAbortBadMode(cmd, "--fast");
      return false;  /* unreachable */
  }
}

/* Dispatch a single cmd in --server (network) mode. */
static bool cmdDispatchServer(const CmdLine *cmd) {
  BYTE selfSlot = clientSimGetServerPlayerNum(humanSim);
  switch (cmd->op) {
    case CMD_OP_ADD_BOT:
      clientSimNetSendAddBot(humanSim);
      return true;
    case CMD_OP_SET_TEAM:
      if (cmd->slot != selfSlot) {
        fprintf(stderr,
                "cmd-stdin: line %d: set_team in --server mode requires slot=%u (own slot)\n",
                cmd->lineNumber, (unsigned)selfSlot);
        exit(2);
      }
      clientSimNetSendTeamSet(humanSim, cmd->slot, cmd->team);
      return true;
    case CMD_OP_SET_READY:
      if (cmd->slot != selfSlot) {
        fprintf(stderr,
                "cmd-stdin: line %d: set_ready in --server mode requires slot=%u (own slot)\n",
                cmd->lineNumber, (unsigned)selfSlot);
        exit(2);
      }
      clientSimNetSendReady(humanSim, cmd->ready);
      return true;
    case CMD_OP_NAME_CHANGE:
      if (cmd->slot != selfSlot) {
        fprintf(stderr,
                "cmd-stdin: line %d: name_change in --server mode requires slot=%u (own slot)\n",
                cmd->lineNumber, (unsigned)selfSlot);
        exit(2);
      }
      clientSimNetSendNameChange(humanSim, cmd->name);
      return true;
    case CMD_OP_ALLIANCE_REQUEST:
      clientSimNetSendAllianceRequest(humanSim, cmd->to);
      return true;
    case CMD_OP_ALLIANCE_ACCEPT:
      clientSimNetSendAllianceAccept(humanSim, cmd->to);
      return true;
    case CMD_OP_ALLIANCE_LEAVE:
      clientSimNetSendAllianceLeave(humanSim);
      return true;
    case CMD_OP_MAP_SKIP_VOTE:
      clientSimNetSendMapSkipVote(humanSim);
      return true;
    case CMD_OP_CHAT:
      clientSimNetSendChat(humanSim, cmd->dest, cmd->body);
      return true;
    case CMD_OP_START_GAME:
    case CMD_OP_REAPPLY_ALLIANCES:
    case CMD_OP_SHUTDOWN:
      cmdAbortBadMode(cmd, "--server");
      return false;  /* unreachable */
    case CMD_OP_EXIT:
      return false;
    default:
      cmdAbortBadMode(cmd, "--server");
      return false;  /* unreachable */
  }
}

/* Drain commands whose tick has arrived. Returns true if the
 * loop should keep running, false on "exit". `fastMode` selects
 * the dispatch table. */
static bool cmdStreamPump(uint32_t currentTick, bool fastMode) {
  if (cmdStream == NULL) return true;
  for (;;) {
    CmdLine cmd;
    if (!cmdStdinPeek(cmdStream, &cmd)) return true;     /* EOF */
    if (cmd.tick > currentTick) return true;             /* not yet due */
    cmdStdinConsume(cmdStream);
    bool keepGoing = fastMode ? cmdDispatchFast(&cmd)
                              : cmdDispatchServer(&cmd);
    if (!keepGoing) return false;
  }
}

/* ------------------------------------------------------------------ */
/* Verbose state logging                                               */
/* ------------------------------------------------------------------ */

/* Helper: ownership string from player number */
static const char *verboseOwnerStr(BYTE owner, BYTE self, PlayerBitMap alliesBits) {
  if (owner == 0xFF) return "neutral";
  if (owner == self) return "self";
  if (alliesBits & (1u << owner)) return "ally";
  return "enemy";
}

static void logStateVerbose(int tickNum) {
  static bool needMapInit = TRUE;
  FILE *f;
  BrainInfo bi;
  BYTE selfPlayer;
  PlayerBitMap alliesBits;
  int i;

  if (logFile == NULL) {
    return;
  }
  f = logFile;

  /* Build brain info (same data Lua brains see).
   * first=TRUE on initial call fills the entire 256x256 brain map from the real map.
   * Subsequent calls use first=FALSE (incremental viewport updates suffice). */
  brainDataMakeInfo(humanSim, &bi, needMapInit, optAi);
  needMapInit = FALSE;
  selfPlayer = (BYTE)bi.player_number;
  alliesBits = bi.allies ? *(bi.allies) : 0;

  /* --- Begin JSON --- */
  fprintf(f, "{\"tick\":%d,\"game_over\":false,\"winner\":null", tickNum);

  /* Tank (self) — coordinates as sub-tile floats, direction 0-255, speed x4 */
  {
    double sx = (double)bi.tankx / 256.0;
    double sy = (double)bi.tanky / 256.0;
    int tx = bi.tankx >> 8;
    int ty = bi.tanky >> 8;
    bool dead = bi.armour > TANK_FULL_ARMOUR;
    unsigned armor = dead ? 0 : (unsigned)bi.armour;
    fprintf(f, ",\"tank\":{\"x\":%.2f,\"y\":%.2f,\"tx\":%d,\"ty\":%d"
               ",\"dir\":%u,\"speed\":%u"
               ",\"dead\":%s,\"armor\":%u,\"shells\":%u,\"mines\":%u,\"trees\":%u"
               ",\"on_boat\":%s,\"reload_ticks\":%u"
               ",\"has_pill\":%s,\"pill_count\":%u}",
      sx, sy, tx, ty,
      (unsigned)bi.direction, (unsigned)bi.speed,
      dead ? "true" : "false", armor,
      (unsigned)bi.shells, (unsigned)bi.mines, (unsigned)bi.trees,
      bi.inboat ? "true" : "false",
      (unsigned)bi.reload,
      bi.carriedpills > 0 ? "true" : "false", (unsigned)bi.carriedpills);
  }

  /* Enemies (visible hostile tanks) */
  fprintf(f, ",\"enemies\":[");
  {
    int first = 1;
    for (i = 0; i < bi.num_objects; i++) {
      if (bi.objects[i].object == OBJECT_TANK && (bi.objects[i].info & OBJECT_HOSTILE)) {
        if (!first) fprintf(f, ",");
        fprintf(f, "{\"x\":%.2f,\"y\":%.2f,\"tx\":%u,\"ty\":%u,\"dir\":%u,\"speed\":%u}",
          (double)bi.objects[i].x / 256.0, (double)bi.objects[i].y / 256.0,
          (unsigned)(bi.objects[i].x >> 8), (unsigned)(bi.objects[i].y >> 8),
          (unsigned)bi.objects[i].direction, (unsigned)bi.objects[i].speed);
        first = 0;
      }
    }
  }
  fprintf(f, "]");

  /* Allies (visible allied tanks) */
  fprintf(f, ",\"allies\":[");
  {
    int first = 1;
    for (i = 0; i < bi.num_objects; i++) {
      if (bi.objects[i].object == OBJECT_TANK && !(bi.objects[i].info & OBJECT_HOSTILE)) {
        if (!first) fprintf(f, ",");
        fprintf(f, "{\"x\":%.2f,\"y\":%.2f,\"tx\":%u,\"ty\":%u,\"dir\":%u,\"speed\":%u}",
          (double)bi.objects[i].x / 256.0, (double)bi.objects[i].y / 256.0,
          (unsigned)(bi.objects[i].x >> 8), (unsigned)(bi.objects[i].y >> 8),
          (unsigned)bi.objects[i].direction, (unsigned)bi.objects[i].speed);
        first = 0;
      }
    }
  }
  fprintf(f, "]");

  /* Shells in flight */
  fprintf(f, ",\"shells\":[");
  {
    int first = 1;
    for (i = 0; i < bi.num_objects; i++) {
      if (bi.objects[i].object == OBJECT_SHOT) {
        if (!first) fprintf(f, ",");
        const char *rel = (bi.objects[i].info & OBJECT_HOSTILE) ? "enemy" : "self";
        fprintf(f, "{\"x\":%.2f,\"y\":%.2f,\"dir\":%u,\"owner\":\"%s\"}",
          (double)bi.objects[i].x / 256.0, (double)bi.objects[i].y / 256.0,
          (unsigned)bi.objects[i].direction, rel);
        first = 0;
      }
    }
  }
  fprintf(f, "]");

  /* Terrain: 29x29 grid centered on tank (brain terrain IDs, matches player view) */
  {
    int tx = bi.tankx >> 8;
    int ty = bi.tanky >> 8;
    int ox = tx - 14;
    int oy = ty - 14;
    const TERRAIN *world = bi.theWorld;

    fprintf(f, ",\"terrain\":[");
    for (int row = 0; row < 29; row++) {
      if (row > 0) fprintf(f, ",");
      fprintf(f, "[");
      for (int col = 0; col < 29; col++) {
        int mx = ox + col;
        int my = oy + row;
        BYTE t = BDEEPSEA;
        if (mx >= 0 && mx < 256 && my >= 0 && my < 256 && world != NULL) {
          t = world[my * 256 + mx] & TERRAIN_MASK;
        }
        if (col > 0) fprintf(f, ",");
        fprintf(f, "%u", (unsigned)t);
      }
      fprintf(f, "]");
    }
    fprintf(f, "]");
  }

  /* Pill views: 15x15 terrain grid centered on each owned pillbox */
  fprintf(f, ",\"pill_views\":[");
  if (fastServerSim != NULL) {
    const TERRAIN *world = bi.theWorld;
    BYTE np = serverSimGetPillCount(fastServerSim);
    int first = 1;
    for (BYTE pi = 1; pi <= np; pi++) {
      BYTE px, py, powner;
      bool pinTank;
      if (!serverSimGetPill(fastServerSim, pi, &px, &py, &powner, NULL, &pinTank)) continue;
      if (powner != selfPlayer || pinTank) continue;
      if (!first) fprintf(f, ",");
      fprintf(f, "{\"id\":%u,\"tx\":%u,\"ty\":%u,\"terrain\":[", (unsigned)pi, (unsigned)px, (unsigned)py);
      int pox = (int)px - 7;
      int poy = (int)py - 7;
      for (int row = 0; row < 15; row++) {
        if (row > 0) fprintf(f, ",");
        fprintf(f, "[");
        for (int col = 0; col < 15; col++) {
          int mx = pox + col;
          int my = poy + row;
          BYTE t = BDEEPSEA;
          if (mx >= 0 && mx < 256 && my >= 0 && my < 256 && world != NULL) {
            t = world[my * 256 + mx] & TERRAIN_MASK;
          }
          if (col > 0) fprintf(f, ",");
          fprintf(f, "%u", (unsigned)t);
        }
        fprintf(f, "]");
      }
      fprintf(f, "]}");
      first = 0;
    }
  }
  fprintf(f, "]");

  /* Pillboxes — full data from server sim */
  fprintf(f, ",\"pillboxes\":[");
  if (fastServerSim != NULL) {
    BYTE np = serverSimGetPillCount(fastServerSim);
    for (BYTE pi = 1; pi <= np; pi++) {
      BYTE px, py, powner, parmour;
      bool pinTank;
      if (!serverSimGetPill(fastServerSim, pi, &px, &py, &powner, &parmour, &pinTank)) continue;
      if (pi > 1) fprintf(f, ",");
      fprintf(f, "{\"tx\":%u,\"ty\":%u,\"owner\":\"%s\",\"armor\":%u,\"in_tank\":%s}",
        (unsigned)px, (unsigned)py,
        verboseOwnerStr(powner, selfPlayer, alliesBits),
        (unsigned)parmour,
        pinTank ? "true" : "false");
    }
  }
  fprintf(f, "]");

  /* Bases — full data from server sim */
  fprintf(f, ",\"bases\":[");
  if (fastServerSim != NULL) {
    BYTE nb = serverSimGetBaseCount(fastServerSim);
    for (BYTE bsi = 1; bsi <= nb; bsi++) {
      BYTE bx, by, bowner;
      BYTE bshells, bmines, barmour;
      if (!serverSimGetBase(fastServerSim, bsi, &bx, &by, &bowner)) continue;
      serverSimGetBaseStats(fastServerSim, bsi, &bshells, &bmines, &barmour);
      if (bsi > 1) fprintf(f, ",");
      fprintf(f, "{\"tx\":%u,\"ty\":%u,\"owner\":\"%s\",\"armor\":%u,\"shells\":%u,\"mines\":%u}",
        (unsigned)bx, (unsigned)by,
        verboseOwnerStr(bowner, selfPlayer, alliesBits),
        (unsigned)barmour, (unsigned)bshells, (unsigned)bmines);
    }
  }
  fprintf(f, "]");

  /* Team summary */
  {
    int self_pills = 0, ally_pills = 0, enemy_pills = 0;
    int self_bases = 0, ally_bases = 0, enemy_bases = 0;
    if (fastServerSim != NULL) {
      BYTE np = serverSimGetPillCount(fastServerSim);
      for (BYTE pi = 1; pi <= np; pi++) {
        BYTE powner;
        if (!serverSimGetPill(fastServerSim, pi, NULL, NULL, &powner, NULL, NULL)) continue;
        if (powner == 0xFF) { /* neutral — skip */ }
        else if (powner == selfPlayer) self_pills++;
        else if (alliesBits & (1u << powner)) ally_pills++;
        else enemy_pills++;
      }
      BYTE nb = serverSimGetBaseCount(fastServerSim);
      for (BYTE bsi = 1; bsi <= nb; bsi++) {
        BYTE bowner;
        if (!serverSimGetBase(fastServerSim, bsi, NULL, NULL, &bowner)) continue;
        if (bowner == 0xFF) { /* neutral — skip */ }
        else if (bowner == selfPlayer) self_bases++;
        else if (alliesBits & (1u << bowner)) ally_bases++;
        else enemy_bases++;
      }
    }
    fprintf(f, ",\"team\":{\"self_pillboxes\":%d,\"self_bases\":%d"
               ",\"enemy_pillboxes\":%d,\"enemy_bases\":%d"
               ",\"ally_pillboxes\":%d,\"ally_bases\":%d}",
      self_pills, self_bases, enemy_pills, enemy_bases, ally_pills, ally_bases);
  }

  /* Events — array of typed objects */
  fprintf(f, ",\"events\":[");
  {
    int first = 1;
    for (i = 0; i < bi.num_events; i++) {
      GameEvent *e = &bi.events[i];
      switch (e->type) {
      case EVENT_TANK_KILLED:
        if (e->data[0] == selfPlayer) {
          if (!first) fprintf(f, ",");
          fprintf(f, "{\"type\":\"kill\",\"target\":\"enemy\"}");
          first = 0;
        }
        if (e->data[1] == selfPlayer) {
          if (!first) fprintf(f, ",");
          fprintf(f, "{\"type\":\"death\"}");
          first = 0;
        }
        break;
      case EVENT_SOUND_TANK_HIT:
        if (e->data[3] != selfPlayer) {
          if (!first) fprintf(f, ",");
          fprintf(f, "{\"type\":\"hit_dealt\"}");
          first = 0;
        }
        if (e->data[3] == selfPlayer) {
          if (!first) fprintf(f, ",");
          fprintf(f, "{\"type\":\"hit_received\"}");
          first = 0;
        }
        break;
      case EVENT_PILL_CAPTURED:
        if (e->data[0] == selfPlayer) {
          if (!first) fprintf(f, ",");
          fprintf(f, "{\"type\":\"pill_captured\"}");
          first = 0;
        }
        if (e->data[1] == selfPlayer) {
          if (!first) fprintf(f, ",");
          fprintf(f, "{\"type\":\"pill_lost\"}");
          first = 0;
        }
        break;
      case EVENT_BASE_CAPTURED:
        if (e->data[0] == selfPlayer) {
          if (!first) fprintf(f, ",");
          fprintf(f, "{\"type\":\"base_captured\"}");
          first = 0;
        }
        if (e->data[1] == selfPlayer) {
          if (!first) fprintf(f, ",");
          fprintf(f, "{\"type\":\"base_lost\"}");
          first = 0;
        }
        break;
      default:
        break;
      }
    }
  }
  fprintf(f, "]");

  fprintf(f, "}\n");
  fflush(f);

  /* Cleanup BrainInfo allocations (subset of brainDataExtractInfo —
   * we only need to free, not apply outputs) */
  free(bi.allies);
  if (bi.base != NULL) free(bi.base);
  free(bi.pillview);
  free(bi.viewdata);
  if (bi.events != NULL) free(bi.events);
  if (bi.message != NULL) {
    free(bi.message->receivers);
    free(bi.message->message);
    free(bi.message);
  }
}

/* ------------------------------------------------------------------ */
/* Binary state logging                                                */
/* ------------------------------------------------------------------ */

/* Binary observation frame (little-endian x86):
 *   HEADER:  8 bytes  (tick u32, dead u8, game_over u8, winner u8, num_events u8)
 *   SPATIAL: 33640 bytes  (float32[29][29][10], row-major channels-last)
 *   SCALAR:  68 bytes (float32[17])
 *   EVENTS:  num_events bytes (uint8 per event, max 16)
 *
 * Event types: 0=hit_dealt 1=kill 2=death 3=hit_received
 *              4=pill_captured 5=pill_lost 6=base_captured 7=base_lost
 */

static void logStateBinary(int tickNum) {
  static bool needMapInit = TRUE;
  BrainInfo bi;
  BYTE selfPlayer;
  PlayerBitMap alliesBits;
  int i;

  if (logFile == NULL) {
    return;
  }

  brainDataMakeInfo(humanSim, &bi, needMapInit, optAi);
  needMapInit = FALSE;
  selfPlayer = (BYTE)bi.player_number;
  alliesBits = bi.allies ? *(bi.allies) : 0;

  bool dead = bi.armour > TANK_FULL_ARMOUR;
  int tank_tx = bi.tankx >> 8;
  int tank_ty = bi.tanky >> 8;

  /* --- Collect events --- */
  uint8_t eventBuf[BINARY_MAX_EVENTS];
  uint8_t numEvents = 0;
  for (i = 0; i < bi.num_events && numEvents < BINARY_MAX_EVENTS; i++) {
    GameEvent *e = &bi.events[i];
    switch (e->type) {
    case EVENT_SOUND_TANK_HIT:
      if (e->data[3] != selfPlayer && numEvents < BINARY_MAX_EVENTS)
        eventBuf[numEvents++] = 0; /* hit_dealt */
      if (e->data[3] == selfPlayer && numEvents < BINARY_MAX_EVENTS)
        eventBuf[numEvents++] = 3; /* hit_received */
      break;
    case EVENT_TANK_KILLED:
      if (e->data[0] == selfPlayer && numEvents < BINARY_MAX_EVENTS)
        eventBuf[numEvents++] = 1; /* kill */
      if (e->data[1] == selfPlayer && numEvents < BINARY_MAX_EVENTS)
        eventBuf[numEvents++] = 2; /* death */
      break;
    case EVENT_PILL_CAPTURED:
      if (e->data[0] == selfPlayer && numEvents < BINARY_MAX_EVENTS)
        eventBuf[numEvents++] = 4; /* pill_captured */
      if (e->data[1] == selfPlayer && numEvents < BINARY_MAX_EVENTS)
        eventBuf[numEvents++] = 5; /* pill_lost */
      break;
    case EVENT_BASE_CAPTURED:
      if (e->data[0] == selfPlayer && numEvents < BINARY_MAX_EVENTS)
        eventBuf[numEvents++] = 6; /* base_captured */
      if (e->data[1] == selfPlayer && numEvents < BINARY_MAX_EVENTS)
        eventBuf[numEvents++] = 7; /* base_lost */
      break;
    default:
      break;
    }
  }

  /* --- HEADER (8 bytes) --- */
  {
    uint32_t tick = (uint32_t)tickNum;
    uint8_t hdr_dead = dead ? 1 : 0;
    uint8_t hdr_game_over = 0;
    uint8_t hdr_winner = 0;
    fwrite(&tick, 4, 1, logFile);
    fwrite(&hdr_dead, 1, 1, logFile);
    fwrite(&hdr_game_over, 1, 1, logFile);
    fwrite(&hdr_winner, 1, 1, logFile);
    fwrite(&numEvents, 1, 1, logFile);
  }

  /* --- SPATIAL OBSERVATION (33640 bytes) --- */
  {
    float spatial[BINARY_SPATIAL_SIZE][BINARY_SPATIAL_SIZE][BINARY_NUM_CHANNELS];
    memset(spatial, 0, sizeof(spatial));
    const TERRAIN *world = bi.theWorld;

    /* Channel 0: terrain normalized, Channel 8: known mines */
    for (int row = 0; row < 29; row++) {
      for (int col = 0; col < 29; col++) {
        int mx = tank_tx - 14 + col;
        int my = tank_ty - 14 + row;
        if (mx >= 0 && mx < 256 && my >= 0 && my < 256 && world != NULL) {
          BYTE raw = world[my * 256 + mx];
          spatial[row][col][0] = (float)(raw & TERRAIN_MASK) / 15.0f;
          if (raw & TERRAIN_MINE) {
            spatial[row][col][8] = 1.0f;
          }
        }
      }
    }

    /* Channels 1-2: enemy tanks, 3-4: shells (from visible objects) */
    for (i = 0; i < bi.num_objects; i++) {
      ObjectInfo *o = &bi.objects[i];
      int gx = (int)(o->x >> 8) - tank_tx + 14;
      int gy = (int)(o->y >> 8) - tank_ty + 14;
      if (gx < 0 || gx >= 29 || gy < 0 || gy >= 29) continue;

      if (o->object == OBJECT_TANK && (o->info & OBJECT_HOSTILE)) {
        spatial[gy][gx][1] = 1.0f;
        spatial[gy][gx][2] = (float)o->direction / 256.0f;
      } else if (o->object == OBJECT_SHOT) {
        if (o->info & OBJECT_HOSTILE) {
          spatial[gy][gx][4] = 1.0f; /* enemy/other shell */
        } else {
          spatial[gy][gx][3] = 1.0f; /* self shell */
        }
      }
    }

    /* Channels 5-7: pillboxes, Channel 9: bases (from server sim) */
    if (fastServerSim != NULL) {
      BYTE np = serverSimGetPillCount(fastServerSim);
      for (BYTE pi = 1; pi <= np; pi++) {
        BYTE px, py, powner, parmour;
        bool pinTank;
        if (!serverSimGetPill(fastServerSim, pi, &px, &py, &powner, &parmour, &pinTank)) continue;
        if (pinTank) continue;
        int gx = (int)px - tank_tx + 14;
        int gy = (int)py - tank_ty + 14;
        if (gx < 0 || gx >= 29 || gy < 0 || gy >= 29) continue;
        float intensity = (float)parmour / 15.0f;
        if (powner == 0xFF) {
          spatial[gy][gx][5] = intensity; /* neutral */
        } else if (powner == selfPlayer) {
          spatial[gy][gx][6] = intensity; /* self */
        } else {
          spatial[gy][gx][7] = intensity; /* enemy */
        }
      }

      BYTE nb = serverSimGetBaseCount(fastServerSim);
      for (BYTE bsi = 1; bsi <= nb; bsi++) {
        BYTE bx, by;
        if (!serverSimGetBase(fastServerSim, bsi, &bx, &by, NULL)) continue;
        int gx = (int)bx - tank_tx + 14;
        int gy = (int)by - tank_ty + 14;
        if (gx < 0 || gx >= 29 || gy < 0 || gy >= 29) continue;
        spatial[gy][gx][9] = 1.0f;
      }
    }

    fwrite(spatial, sizeof(spatial), 1, logFile);
  }

  /* --- SCALAR OBSERVATION (68 bytes) --- */
  {
    float scalars[BINARY_NUM_SCALARS];
    unsigned armor = dead ? 0 : (unsigned)bi.armour;
    float dir_rad = (float)bi.direction * (2.0f * 3.14159265f / 256.0f);

    scalars[0]  = (float)armor / 40.0f;
    scalars[1]  = (float)bi.shells / 40.0f;
    scalars[2]  = (float)bi.mines / 40.0f;
    scalars[3]  = (float)bi.trees / 40.0f;
    scalars[4]  = (float)bi.speed / 128.0f;
    scalars[5]  = sinf(dir_rad);
    scalars[6]  = cosf(dir_rad);
    scalars[7]  = (float)bi.reload / 15.0f;
    scalars[8]  = bi.inboat ? 1.0f : 0.0f;
    scalars[9]  = bi.carriedpills > 0 ? 1.0f : 0.0f;
    scalars[10] = (float)bi.carriedpills / 16.0f;
    scalars[11] = dead ? 1.0f : 0.0f;

    /* Team pill/base ratios */
    int self_pills = 0, enemy_pills = 0, ally_pills = 0, total_pills = 0;
    int self_bases = 0, ally_bases = 0, total_bases = 0;
    if (fastServerSim != NULL) {
      BYTE np = serverSimGetPillCount(fastServerSim);
      total_pills = np;
      for (BYTE pi = 1; pi <= np; pi++) {
        BYTE powner;
        if (!serverSimGetPill(fastServerSim, pi, NULL, NULL, &powner, NULL, NULL)) continue;
        if (powner == 0xFF) continue;
        if (powner == selfPlayer) self_pills++;
        else if (alliesBits & (1u << powner)) ally_pills++;
        else enemy_pills++;
      }
      BYTE nb = serverSimGetBaseCount(fastServerSim);
      total_bases = nb;
      for (BYTE bsi = 1; bsi <= nb; bsi++) {
        BYTE bowner;
        if (!serverSimGetBase(fastServerSim, bsi, NULL, NULL, &bowner)) continue;
        if (bowner == 0xFF) continue;
        if (bowner == selfPlayer) self_bases++;
        else if (alliesBits & (1u << bowner)) ally_bases++;
      }
    }
    float tp = total_pills > 0 ? (float)total_pills : 1.0f;
    float tb = total_bases > 0 ? (float)total_bases : 1.0f;
    scalars[12] = (float)self_pills / tp;
    scalars[13] = (float)enemy_pills / tp;
    scalars[14] = (float)ally_pills / tp;
    scalars[15] = (float)self_bases / tb;
    scalars[16] = (float)ally_bases / tb;

    fwrite(scalars, sizeof(scalars), 1, logFile);
  }

  /* --- EVENTS (num_events bytes) --- */
  if (numEvents > 0) {
    fwrite(eventBuf, numEvents, 1, logFile);
  }

  fflush(logFile);

  /* Cleanup BrainInfo allocations */
  free(bi.allies);
  if (bi.base != NULL) free(bi.base);
  free(bi.pillview);
  free(bi.viewdata);
  if (bi.events != NULL) free(bi.events);
  if (bi.message != NULL) {
    free(bi.message->receivers);
    free(bi.message->message);
    free(bi.message);
  }
}

/* ------------------------------------------------------------------ */
/* Log state dispatcher                                                */
/* ------------------------------------------------------------------ */

static void logStateTick(int tickNum) {
  if (optLogBinary) {
    logStateBinary(tickNum);
  } else {
    logStateVerbose(tickNum);
  }
}

/* ------------------------------------------------------------------ */
/* Stdin input parsing                                                 */
/* ------------------------------------------------------------------ */

/* Return codes for stdinReadInput */
#define STDIN_OK    0
#define STDIN_EOF   1
#define STDIN_RESET 2

/* Read one JSON line from stdin and fill an InputPacket.
 * Format: {"accel":true,"left":true,"shoot":true,"build":"road","bx":50,"by":60,"gsight":1}
 *         {"reset":true}
 * All fields optional. Empty line or {} = no input.
 * Returns STDIN_OK, STDIN_EOF, or STDIN_RESET. */
static int stdinReadInput(InputPacket *pkt) {
  char line[1024];

  if (fgets(line, sizeof(line), stdin) == NULL) {
    return STDIN_EOF;
  }

  cJSON *root = cJSON_Parse(line);
  if (root == NULL) {
    return STDIN_OK; /* Unparseable — treat as empty input */
  }

  /* Check for reset command */
  if (cJSON_IsTrue(cJSON_GetObjectItem(root, "reset"))) {
    cJSON_Delete(root);
    return STDIN_RESET;
  }

  /* Movement buttons */
  if (cJSON_IsTrue(cJSON_GetObjectItem(root, "accel")))
    pkt->buttons |= INPUT_BTN_ACCEL;
  if (cJSON_IsTrue(cJSON_GetObjectItem(root, "decel")))
    pkt->buttons |= INPUT_BTN_DECEL;
  if (cJSON_IsTrue(cJSON_GetObjectItem(root, "left")))
    pkt->buttons |= INPUT_BTN_LEFT;
  if (cJSON_IsTrue(cJSON_GetObjectItem(root, "right")))
    pkt->buttons |= INPUT_BTN_RIGHT;

  /* Actions */
  if (cJSON_IsTrue(cJSON_GetObjectItem(root, "shoot")))
    pkt->actions |= INPUT_ACTION_FIRE;
  if (cJSON_IsTrue(cJSON_GetObjectItem(root, "mine")))
    pkt->actions |= INPUT_ACTION_LAY_MINE;

  /* Gunsight adjustment */
  cJSON *gsight = cJSON_GetObjectItem(root, "gsight");
  if (cJSON_IsNumber(gsight)) {
    int val = gsight->valueint;
    if (val > 0) pkt->flags |= (1 << INPUT_FLAG_GUNSIGHT_SHIFT);
    else if (val < 0) pkt->flags |= (2 << INPUT_FLAG_GUNSIGHT_SHIFT);
  }

  /* Build order */
  cJSON *build = cJSON_GetObjectItem(root, "build");
  if (cJSON_IsString(build)) {
    const char *b = build->valuestring;
    if (strcmp(b, "tree") == 0) pkt->buildAction = 1;
    else if (strcmp(b, "road") == 0) pkt->buildAction = 2;
    else if (strcmp(b, "wall") == 0) pkt->buildAction = 3;
    else if (strcmp(b, "pill") == 0) pkt->buildAction = 4;
    else if (strcmp(b, "mine") == 0) pkt->buildAction = 5;
  }

  /* Build target coordinates */
  cJSON *bx = cJSON_GetObjectItem(root, "bx");
  if (cJSON_IsNumber(bx)) pkt->buildX = (uint8_t)bx->valueint;
  cJSON *by = cJSON_GetObjectItem(root, "by");
  if (cJSON_IsNumber(by)) pkt->buildY = (uint8_t)by->valueint;

  cJSON_Delete(root);
  return STDIN_OK;
}

/* ------------------------------------------------------------------ */
/* Command-line parsing                                                */
/* ------------------------------------------------------------------ */

static void printUsage(const char *prog) {
  fprintf(stderr,
    "Usage:\n"
    "  Network mode: %s --server HOST --port PORT [options]\n"
    "  Fast mode:    %s --fast --map FILE [options]\n"
    "\n"
    "Common options:\n"
    "  --name NAME       Player name (default: HeadlessBot)\n"
    "  --brain PATH      Path to Lua brain script\n"
    "  --ticks N         Run for N game ticks then exit (0 = unlimited)\n"
    "  --ai TYPE         AI type: yes (default), full, advantage, no\n"
    "  --gametype TYPE   Game type: strict (default), tournament, open\n"
    "  --log-state FILE  Log verbose JSON state each tick (- for stdout)\n"
    "  --log-state binary  Binary observation frames to stdout (little-endian)\n"
    "  --log-events FILE Log one JSON line per ControlEvent (- for stdout)\n"
    "  --cmd-stdin FILE  Read scripted commands (JSON-per-line) and dispatch at\n"
    "                    each command's tick (- for stdin)\n"
    "  --seed N          Seed the RNG with N for reproducible runs\n"
    "  --quiet           Suppress non-error output\n"
    "\n"
    "Network options:\n"
    "  --server HOST     Server address\n"
    "  --port PORT       Server port\n"
    "  --password PASS   Server password\n"
    "  --tracker HOST    Tracker address (enables hole-punch fallback)\n"
    "  --tracker-port PORT Tracker port\n"
    "\n"
    "Fast mode options:\n"
    "  --fast            Run locally as fast as possible (no wall-clock gating)\n"
    "  --map FILE        Path to .map file (required with --fast)\n"
    "  --stdin           Read input from stdin (one JSON line per game tick)\n",
    prog, prog);
}

static bool parseArgs(int argc, char **argv) {
  int i;
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--server") == 0 && i + 1 < argc) {
      strncpy(optServer, argv[++i], sizeof(optServer) - 1);
    } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      optPort = (unsigned short)atoi(argv[++i]);
    } else if (strcmp(argv[i], "--tracker") == 0 && i + 1 < argc) {
      strncpy(optTrackerAddr, argv[++i], sizeof(optTrackerAddr) - 1);
    } else if (strcmp(argv[i], "--tracker-port") == 0 && i + 1 < argc) {
      optTrackerPort = (unsigned short)atoi(argv[++i]);
    } else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
      strncpy(optName, argv[++i], sizeof(optName) - 1);
    } else if (strcmp(argv[i], "--brain") == 0 && i + 1 < argc) {
      strncpy(optBrain, argv[++i], sizeof(optBrain) - 1);
    } else if (strcmp(argv[i], "--ticks") == 0 && i + 1 < argc) {
      optTicks = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--log-state") == 0 && i + 1 < argc) {
      strncpy(optLogState, argv[++i], sizeof(optLogState) - 1);
    } else if (strcmp(argv[i], "--log-events") == 0 && i + 1 < argc) {
      strncpy(optLogEvents, argv[++i], sizeof(optLogEvents) - 1);
    } else if (strcmp(argv[i], "--cmd-stdin") == 0 && i + 1 < argc) {
      strncpy(optCmdStdin, argv[++i], sizeof(optCmdStdin) - 1);
    } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
      optSeed = strtoull(argv[++i], NULL, 0);
      optSeedSet = TRUE;
    } else if (strcmp(argv[i], "--password") == 0 && i + 1 < argc) {
      strncpy(optPassword, argv[++i], sizeof(optPassword) - 1);
    } else if (strcmp(argv[i], "--quiet") == 0) {
      optQuiet = TRUE;
    } else if (strcmp(argv[i], "--fast") == 0) {
      optFast = TRUE;
    } else if (strcmp(argv[i], "--stdin") == 0) {
      optStdin = TRUE;
    } else if (strcmp(argv[i], "--ai") == 0 && i + 1 < argc) {
      i++;
      if (strcmp(argv[i], "yes") == 0) optAi = aiYes;
      else if (strcmp(argv[i], "full") == 0) optAi = aiFull;
      else if (strcmp(argv[i], "advantage") == 0) optAi = aiYesAdvantage;
      else if (strcmp(argv[i], "no") == 0) optAi = aiNone;
      else {
        fprintf(stderr, "Error: unknown AI type '%s' (use: yes, full, advantage, no)\n", argv[i]);
        return FALSE;
      }
    } else if (strcmp(argv[i], "--gametype") == 0 && i + 1 < argc) {
      i++;
      if (strcmp(argv[i], "strict") == 0) optGameType = gameStrictTournament;
      else if (strcmp(argv[i], "tournament") == 0) optGameType = optGameType;
      else if (strcmp(argv[i], "open") == 0) optGameType = gameOpen;
      else {
        fprintf(stderr, "Error: unknown game type '%s' (use: strict, tournament, open)\n", argv[i]);
        return FALSE;
      }
    } else if (strcmp(argv[i], "--map") == 0 && i + 1 < argc) {
      strncpy(optMap, argv[++i], sizeof(optMap) - 1);
    } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      printUsage(argv[0]);
      exit(0);
    } else {
      fprintf(stderr, "Unknown argument: %s\n", argv[i]);
      printUsage(argv[0]);
      return FALSE;
    }
  }

  if (optFast) {
    if (optMap[0] == '\0') {
      fprintf(stderr, "Error: --map is required with --fast\n");
      printUsage(argv[0]);
      return FALSE;
    }
    /* In fast mode with log output, stdin is required for lockstep control */
    if (optLogState[0] != '\0'  && optBrain[0] == '\0') {
      optStdin = TRUE;
    }
  } else {
    if (optServer[0] == '\0') {
      fprintf(stderr, "Error: --server is required (or use --fast --map FILE)\n");
      printUsage(argv[0]);
      return FALSE;
    }
    if (optPort == 0) {
      fprintf(stderr, "Error: --port is required\n");
      printUsage(argv[0]);
      return FALSE;
    }
  }

  return TRUE;
}

/* ------------------------------------------------------------------ */
/* gameFront* functions called by the engine                           */
/* ------------------------------------------------------------------ */

/* These are called by the network module during join — kept as stubs
 * since the new transport doesn't use the old network.c callbacks. */
void gameFrontGetPassword(char *pword) {
  strcpy(pword, optPassword);
}

void gameFrontGetPlayerName(char *pn) {
  strcpy(pn, optName);
}

void gameFrontSetPlayerName(char *pn) {
  strncpy(optName, pn, sizeof(optName) - 1);
}

void gameFrontSetAIType(aiType ait) {
  clientSimSetAiType(humanSim, ait);
}

void gameFrontEnableRejoin(void) {
  /* no-op for headless */
}


/* brainsHandler* functions are provided by luabrainshandler.c */

/* ------------------------------------------------------------------ */
/* Tick counters used by network.c and servernet.c                     */
/* ------------------------------------------------------------------ */

time_t windowsGetTicks(void) {
  return (time_t)SDL_GetTicks();
}

time_t serverMainGetTicks(void) {
  return (time_t)SDL_GetTicks();
}

/* ------------------------------------------------------------------ */
/* Fast local mode: setup and game loop                                */
/* ------------------------------------------------------------------ */

/* Cached compressed map for fast reset (avoids disk I/O) */
static BYTE *cachedCompressedMap = NULL;
static int cachedCompressedMapLen = 0;

/* Flag: first verbose log call after setup needs first=TRUE to fill brain map */
static bool verboseNeedMapInit = TRUE;

/* Set up the server sim, transport, and client sim from cached map.
 * Called at initial startup and on each reset. */
static bool fastModeSetupGame(void) {
  {
    /* Scripted scenarios stay in lobby state until the cmd stream
     * issues start_game (cfg.lobbyEnabled drives SetLobbyEnabled(true)
     * + EnterLobby inside serverInstanceStartup). Non-scripted fast
     * mode goes straight into running state (cfg.skipLobby drives
     * SetLobbyEnabled(false) + StartGame). acceptRemoteClients=false
     * short-circuits the UDP/WBN/tracker/NAT bring-up. */
    ServerInstanceConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.acceptRemoteClients = false;
    if (cmdStream != NULL) {
      cfg.lobbyEnabled = true;
    } else {
      cfg.skipLobby    = true;
    }
    serverInstanceStartup(fastServerSim, &cfg);
  }
  serverSimSetViewPlayer(fastServerSim, 0);

  transportActive = TRUE;
  playerNum = 0;

  /* Build the ClientSim, install the observer, then run the connect
   * body — it joins the sim, installs the map, sets up the local
   * tank, registers the auto-subscriber, and applies the first
   * snapshot in one call. */
  humanSim = clientSimAlloc();
  clientSimCreate(humanSim);
  if (logEventsFile != NULL) {
    clientSimSetControlObserver(humanSim, logEventsDeliverCb, logEventsFile);
  }
  clientSimConnectLocal(humanSim, fastServerSim, optName, "", 0, 0);
  clientSimSetAiType(humanSim, optAi);

  /* Legacy subscriber handle — connect's auto-subscriber registration
   * supersedes the explicit headlessControlSub bookkeeping. Keep the
   * field cleared so the teardown path's unregister is a no-op. */
  headlessControlSub = SUBSCRIBER_HANDLE_INVALID;

  return true;
}

/* Tear down client sim and transport (but not the server sim) */
static void fastModeTeardownGame(void) {
  brainsHandlerShutdown();
  serverSimUnregisterSubscriber(fastServerSim, headlessControlSub);
  headlessControlSub = SUBSCRIBER_HANDLE_INVALID;
  clientSimDestroy(humanSim);  /* also tears down the embedded transport */
  transportActive = FALSE;
}

static int runFastMode(void) {
  int tickCount = 0;
  bool justKeys = FALSE;
  bool brainRunning;
  uint32_t simTickCounter = 0;
  /* Monotonic per-outer-iteration counter. Used as the cmd-stdin
   * pump reference when the server tick is unavailable (lobby
   * phase, before any PACKET_GAME_STATE has been observed). */
  uint32_t pumpTick = 0;

  if (!optQuiet) {
    fprintf(stderr, "WinBolo Headless Client (fast local mode)\n");
    fprintf(stderr, "  Map:    %s\n", optMap);
    fprintf(stderr, "  Name:   %s\n", optName);
    if (optStdin) {
      fprintf(stderr, "  Input:  stdin\n");
    } else if (optBrain[0]) {
      fprintf(stderr, "  Brain:  %s\n", optBrain);
    }
    if (optTicks > 0) {
      fprintf(stderr, "  Ticks:  %d\n", optTicks);
    }
  }

  /* Open scripted command stream first so the later fastModeSetup
   * branches on cmdStream != NULL and stays in lobby state. */
  if (optCmdStdin[0] != '\0') {
    cmdStream = cmdStdinOpen(optCmdStdin);
    if (cmdStream == NULL) return 1;
  }

  /* Create server sim from map file (first time only — hits disk) */
  fastServerSim = serverSimCreate(optMap, optGameType, false, 0, UNLIMITED_GAME_TIME);
  if (fastServerSim == NULL) {
    fprintf(stderr, "Error: failed to load map '%s'\n", optMap);
    return 1;
  }

  /* Cache compressed map for fast resets */
  {
    BYTE tempMap[65536];
    cachedCompressedMapLen = serverSimGetCompressedMap(fastServerSim, tempMap);
    if (cachedCompressedMapLen <= 0) {
      fprintf(stderr, "Error: failed to compress map\n");
      serverSimDestroy(fastServerSim);
      fastServerSim = NULL;
      return 1;
    }
    cachedCompressedMap = (BYTE *)malloc(cachedCompressedMapLen);
    memcpy(cachedCompressedMap, tempMap, cachedCompressedMapLen);
  }

  /* Open events log before the first ClientSim subscriber registration —
   * registration triggers a sync pass that fans out CTRL_PLAYER_JOIN /
   * CTRL_LOBBY_SLOT etc. to the new subscriber, and we want those captured. */
  logEventsOpen(optLogEvents);

  /* Initial game setup. Scripted scenarios (--cmd-stdin) stay in
   * lobby state so add_bot / set_team / start_game ops can drive
   * the lifecycle transitions deterministically. */
  {
    ServerInstanceConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.acceptRemoteClients = false;
    if (cmdStream != NULL) {
      cfg.lobbyEnabled = true;
    } else {
      cfg.skipLobby    = true;
    }
    serverInstanceStartup(fastServerSim, &cfg);
  }
  serverSimSetViewPlayer(fastServerSim, 0);
  transportActive = TRUE;
  playerNum = 0;
  humanSim = clientSimAlloc();
  clientSimCreate(humanSim);
  /* Observer must be set before connect so register-time sync events
   * are observed. Preserved across clientSimCreate's memset. */
  if (logEventsFile != NULL) {
    clientSimSetControlObserver(humanSim, logEventsDeliverCb, logEventsFile);
  }
  clientSimConnectLocal(humanSim, fastServerSim, optName, "", 0, 0);
  clientSimSetAiType(humanSim, optAi);
  headlessControlSub = SUBSCRIBER_HANDLE_INVALID;

  if (!optQuiet) {
    fprintf(stderr, "Game ready. Entering fast loop.\n");
  }

  /* Load and start brain if specified */
  brainsHandlerLoadBrains();
  if (optBrain[0] != '\0') {
    if (!brainsHandlerStart(optBrain, optBrain, humanSim)) {
      fprintf(stderr, "Warning: failed to start brain '%s'\n", optBrain);
    } else if (!optQuiet) {
      fprintf(stderr, "Brain started: %s\n", optBrain);
    }
  }

  /* Open state log */
  logStateOpen(optLogState);

  /* Emit initial state (tick 0) so a controller can read it before sending input */
  if (logFile != NULL) {
    logStateTick(0);
  }

  /* Stdin state: last-read buttons persist across keys tick */
  uint8_t stdinButtons = 0;

  /* Fast game loop — no wall-clock gating, tick as fast as possible */
  while (!headlessQuit) {
    brainRunning = !optStdin && brainHandlerIsBrainRunning();
    pumpTick++;

    if (justKeys) {
      /* Keys tick — replay held buttons from last stdin read */
      InputPacket pkt;
      if (optStdin) {
        memset(&pkt, 0, sizeof(pkt));
        pkt.tick = simTickCounter;
        pkt.playerNum = playerNum;
        pkt.buttons = stdinButtons;
      } else {
        clientBuildInputPacket(humanSim, &pkt, 0, FALSE, FALSE, brainRunning, FALSE, playerNum, simTickCounter);
      }
      clientSimKeysTick(humanSim, &pkt);
      clientSimNetSendInput(humanSim, &pkt);
      clientSimNetTick(humanSim);  /* localTick pulls + applies the snapshot */
      simTickCounter++;
      justKeys = FALSE;
    } else {
      /* Game tick — read fresh input from stdin or brain */
      InputPacket pkt;
      if (optStdin) {
        memset(&pkt, 0, sizeof(pkt));
        pkt.tick = simTickCounter;
        pkt.playerNum = playerNum;
        int rc = stdinReadInput(&pkt);
        if (rc == STDIN_EOF) {
          break;
        }
        if (rc == STDIN_RESET) {
          /* Reset: tear down and recreate game from cached map */
          fastModeTeardownGame();
          fastModeSetupGame();
          tickCount = 0;
          simTickCounter = 0;
          pumpTick = 0;
          justKeys = FALSE;
          stdinButtons = 0;
          verboseNeedMapInit = TRUE;
          /* Re-start brain if needed */
          brainsHandlerLoadBrains();
          if (optBrain[0] != '\0') {
            brainsHandlerStart(optBrain, optBrain, humanSim);
          }
          /* Emit tick 0 state immediately — no input needed */
          if (logFile != NULL) {
            logStateTick(0);
          }
          continue;
        }
        stdinButtons = pkt.buttons;
      } else {
        clientBuildInputPacket(humanSim, &pkt, 0, FALSE, FALSE, brainRunning, TRUE, playerNum, simTickCounter);
      }
      clientSimGameTick(humanSim, &pkt, brainRunning);
      clientSimNetSendInput(humanSim, &pkt);
      clientSimNetTick(humanSim);  /* localTick pulls + applies the snapshot */
      clientSimDisplayTick(humanSim, brainRunning);
      simTickCounter++;
      tickCount++;
      justKeys = TRUE;

      /* Brain processing */
      if (brainRunning) {
        brainHandlerRun();
      }

      /* Log state */
      logStateTick(tickCount);
    }

    /* Scripted command stream — dispatch any pending ops whose tick
     * has arrived. Prefer the server's observed tick when snapshots
     * are flowing; fall back to a local per-iteration counter while
     * the server tick is still 0 (lobby phase, before any
     * PACKET_GAME_STATE has arrived) so lobby-mode ops still fire. */
    if (cmdStream != NULL) {
      uint32_t serverTick = clientSimGetLastServerTick(humanSim);
      uint32_t pumpRef = serverTick > 0 ? serverTick : pumpTick;
      if (!cmdStreamPump(pumpRef, true)) {
        if (!optQuiet) {
          fprintf(stderr, "cmd-stdin: exit op received.\n");
        }
        break;
      }
    }

    /* Check if we've reached the tick limit */
    if (optTicks > 0 && tickCount >= optTicks) {
      if (!optQuiet) {
        fprintf(stderr, "Reached tick limit (%d). Exiting.\n", optTicks);
      }
      break;
    }
  }

  /* Cleanup */
  if (!optQuiet) {
    fprintf(stderr, "Shutting down after %d ticks.\n", tickCount);
  }

  logStateClose();
  brainsHandlerShutdown();
  serverSimUnregisterSubscriber(fastServerSim, headlessControlSub);
  headlessControlSub = SUBSCRIBER_HANDLE_INVALID;
  /* Detach observer before tearing down the ClientSim and closing the
   * file it points at, so no late event can write into a stale FILE. */
  if (humanSim != NULL) {
    clientSimSetControlObserver(humanSim, NULL, NULL);
  }
  clientSimDestroy(humanSim);  /* also tears down the embedded transport */
  transportActive = FALSE;
  serverSimDestroy(fastServerSim);
  fastServerSim = NULL;
  free(cachedCompressedMap);
  cachedCompressedMap = NULL;
  logEventsClose();
  cmdStdinClose(cmdStream);
  cmdStream = NULL;

  return 0;
}

/* ------------------------------------------------------------------ */
/* Network mode: game loop (original behavior)                         */
/* ------------------------------------------------------------------ */

static int runNetworkMode(void) {
  DWORD oldTick, ttick;
  int tickCount = 0;
  bool justKeys = FALSE;
  bool brainRunning;
  uint32_t simTickCounter = 0;
  /* Monotonic per-outer-iteration counter. Used as the cmd-stdin
   * pump reference when the server tick is unavailable (lobby
   * phase, before any PACKET_GAME_STATE has been observed). */
  uint32_t pumpTick = 0;

  if (!optQuiet) {
    fprintf(stderr, "WinBolo Headless Client\n");
    fprintf(stderr, "  Server: %s:%u\n", optServer, (unsigned)optPort);
    fprintf(stderr, "  Name:   %s\n", optName);
    if (optBrain[0]) {
      fprintf(stderr, "  Brain:  %s\n", optBrain);
    }
    if (optTicks > 0) {
      fprintf(stderr, "  Ticks:  %d\n", optTicks);
    }
  }

  /* Open events log before connect so roster / lobby /
   * map-download events that arrive during the join handshake are
   * captured. Observer is preserved across clientSimCreate's memset
   * (see the save/restore block in client_sim.c). */
  logEventsOpen(optLogEvents);

  /* Open scripted command stream (commands queue until each
   * command's tick has been observed on the wire). */
  if (optCmdStdin[0] != '\0') {
    cmdStream = cmdStdinOpen(optCmdStdin);
    if (cmdStream == NULL) return 1;
  }

  /* Initialize the game engine (JOIN_ACCEPT will install the
   * authoritative game settings). */
  humanSim = clientSimAlloc();
  clientSimCreate(humanSim);
  if (logEventsFile != NULL) {
    clientSimSetControlObserver(humanSim, logEventsDeliverCb, logEventsFile);
  }
  clientSimSetMyLastPlayerName(humanSim, optName);

  /* Connect to the server via new UDP transport */
  if (!optQuiet) {
    fprintf(stderr, "Connecting to %s:%u...\n", optServer, optPort);
  }

  /* Pre-flight version negotiation. The legacy info-request is the
   * universal cross-version handshake — any server answers regardless
   * of build, so we can read the server's version triple before
   * committing to a JOIN_REQUEST whose new-protocol length gate would
   * silently drop on mismatch. On mismatch surface a clear stderr
   * message and exit non-zero; on timeout, mirror the SDL3 frontend
   * by aborting with the standard "server unreachable" wording. */
  {
    DiscoveryPingResult dpr;
    if (!discoveryPingServer(optServer, optPort, &dpr)) {
      fprintf(stderr, "Error: failed to connect: server unreachable\n");
      clientSimDestroy(humanSim);
      return 1;
    }
    if (dpr.versionMajor    != BOLO_VERSION_MAJOR ||
        dpr.versionMinor    != BOLO_VERSION_MINOR ||
        dpr.versionRevision != BOLO_VERSION_REVISION) {
      fprintf(stderr,
              "Error: server is version %u.%u.%u, you have %u.%u.%u "
              "— please update.\n",
              (unsigned)dpr.versionMajor,
              (unsigned)dpr.versionMinor,
              (unsigned)dpr.versionRevision,
              (unsigned)BOLO_VERSION_MAJOR,
              (unsigned)BOLO_VERSION_MINOR,
              (unsigned)BOLO_VERSION_REVISION);
      clientSimDestroy(humanSim);
      return 1;
    }
  }

  clientSimConnectUdp(humanSim, optServer, optPort, optName,
                      winbolonetGetCountryCode(),
                      optPassword, "", "",
                      false, optTrackerAddr, optTrackerPort,
                      /*spectator*/ false);
  if (clientSimGetConnectState(humanSim) == CLIENT_CONNECT_ERROR) {
    const char *reason = clientSimGetConnectErrorReason(humanSim);
    fprintf(stderr, "Error: failed to connect: %s\n", reason ? reason : "unknown");
    clientSimDestroy(humanSim);
    return 1;
  }

  /* Wait for join handshake + map download */
  {
    int joinWaitTicks = 0;
    while ((clientSimGetConnectState(humanSim) == CLIENT_CONNECT_JOINING ||
            clientSimGetConnectState(humanSim) == CLIENT_CONNECT_DOWNLOADING_MAP) &&
           joinWaitTicks < 1500) {
      clientSimNetTick(humanSim);
      SDL_Delay(20);
      joinWaitTicks++;
    }
  }

  if (clientSimGetConnectState(humanSim) != CLIENT_CONNECT_CONNECTED) {
    const char *reason = clientSimGetConnectErrorReason(humanSim);
    fprintf(stderr, "Error: join failed: %s\n", reason ? reason : "timeout");
    clientSimDestroy(humanSim);
    return 1;
  }

  playerNum = clientSimGetServerPlayerNum(humanSim);
  transportActive = TRUE;

  /* Map install + snapshot apply happen inside the transport — the
   * MAP_DOWNLOAD completion path drops the buffered bytes onto the
   * ClientSim, and PACKET_STATE_SNAPSHOT applies inline. Headless
   * only sets the bot AI type. */
  clientSimSetAiType(humanSim, optAi);

  /* Gate lobby vs running: if we received CTRL_LOBBY_SETTINGS during
   * join, stay in lobby state; otherwise proceed to running */
  if (clientSimIsInLobby(humanSim)) {
    clientSimSetMapDownloadComplete(humanSim, true);
    clientSimSetNetStatus(humanSim, netLobby);
  }

  if (!optQuiet) {
    fprintf(stderr, "Connected as player %d. Entering game loop.\n", playerNum);
  }

  /* Load and start brain if specified */
  brainsHandlerLoadBrains();
  if (optBrain[0] != '\0') {
    if (!brainsHandlerStart(optBrain, optBrain, humanSim)) {
      fprintf(stderr, "Warning: failed to start brain '%s'\n", optBrain);
    } else if (!optQuiet) {
      fprintf(stderr, "Brain started: %s\n", optBrain);
    }
  }

  /* Open state log */
  logStateOpen(optLogState);

  /* Main game loop */
  oldTick = SDL_GetTicks();

  while (!headlessQuit) {
    brainRunning = brainHandlerIsBrainRunning();
    bool used = FALSE;
    pumpTick++;

    ttick = SDL_GetTicks();

    /* Process game ticks */
    if ((ttick - oldTick) > GAME_TICK_LENGTH) {
      while ((ttick - oldTick) > GAME_TICK_LENGTH) {
        if (clientSimGetNetStatus(humanSim) == netLobby || clientSimGetNetStatus(humanSim) == netLobbyCountdown) {
          /* Lobby/countdown: just tick the transport to receive packets */
          clientSimNetTick(humanSim);
          justKeys = !justKeys;
        } else if (justKeys) {
          /* Keys tick */
          InputPacket pkt;
          clientBuildInputPacket(humanSim, &pkt, 0, FALSE, FALSE, brainRunning, FALSE, playerNum, simTickCounter);
          clientMutexWaitFor();
          clientSimKeysTick(humanSim, &pkt);
          clientMutexRelease();
          clientSimNetRecordInput(humanSim, &pkt);
          clientSimNetTick(humanSim);
          simTickCounter++;
          justKeys = FALSE;
        } else {
          /* Game tick */
          InputPacket pkt;
          clientBuildInputPacket(humanSim, &pkt, 0, FALSE, FALSE, brainRunning, TRUE, playerNum, simTickCounter);
          clientMutexWaitFor();
          clientSimGameTick(humanSim, &pkt, brainRunning);
          clientMutexRelease();
          clientSimNetSendInput(humanSim, &pkt);
          clientSimNetTick(humanSim);
          clientMutexWaitFor();
          clientSimDisplayTick(humanSim, brainRunning);
          clientMutexRelease();
          simTickCounter++;
          tickCount++;
          justKeys = TRUE;
          used = TRUE;
        }
        oldTick += GAME_TICK_LENGTH;
        if (oldTick > ttick) {
          oldTick = ttick;
        }
      }
    }

    /* Brain processing */
    if (used && brainRunning &&
        clientSimGetConnectState(humanSim) != CLIENT_CONNECT_SERVER_SHUTDOWN) {
      brainHandlerRun();
    }

    /* Log state */
    if (used) {
      clientMutexWaitFor();
      logStateTick(tickCount);
      clientMutexRelease();
    }

    /* Scripted command stream — fan out to the wire send-wrappers
     * once the pump reference has reached each command's scheduled
     * tick. Prefer the server's observed tick when snapshots are
     * flowing; fall back to a local per-iteration counter while the
     * server tick is still 0 (lobby phase, before any
     * PACKET_GAME_STATE has arrived) so lobby ops still fire. */
    if (cmdStream != NULL) {
      uint32_t serverTick = clientSimGetLastServerTick(humanSim);
      uint32_t pumpRef = serverTick > 0 ? serverTick : pumpTick;
      if (!cmdStreamPump(pumpRef, false)) {
        if (!optQuiet) {
          fprintf(stderr, "cmd-stdin: exit op received.\n");
        }
        break;
      }
    }

    /* Check if we've reached the tick limit */
    if (optTicks > 0 && tickCount >= optTicks) {
      if (!optQuiet) {
        fprintf(stderr, "Reached tick limit (%d). Exiting.\n", optTicks);
      }
      break;
    }

    /* Check for server disconnect */
    if (clientSimGetConnectState(humanSim) == CLIENT_CONNECT_SERVER_SHUTDOWN) {
      fprintf(stderr, "Server disconnected. Exiting.\n");
      break;
    }

    SDL_Delay(1); /* Don't busy-wait */
  }

  /* Cleanup */
  if (!optQuiet) {
    fprintf(stderr, "Shutting down after %d ticks.\n", tickCount);
  }

  logStateClose();

  clientMutexWaitFor();
  brainsHandlerShutdown();
  /* Detach observer before destroying the ClientSim so a late event
   * cannot write into a closed FILE. */
  if (humanSim != NULL) {
    clientSimSetControlObserver(humanSim, NULL, NULL);
  }
  clientSimDestroy(humanSim);  /* also tears down the embedded transport */
  transportActive = FALSE;
  clientMutexRelease();

  logEventsClose();
  cmdStdinClose(cmdStream);
  cmdStream = NULL;

  return 0;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
  int result;

  bolo_srand((uint64_t)time(NULL) ^ (uint64_t)getpid());
  sentryInit("WinBoloHeadless", argc, argv);
  atexit(sentryClose);

  if (!parseArgs(argc, argv)) {
    return 1;
  }

  if (optSeedSet) {
    bolo_srand((uint64_t)optSeed);
  }

  signal(SIGINT, signalHandler);
  signal(SIGTERM, signalHandler);

  /* Initialize SDL (no video/audio subsystems) */
  if (!SDL_Init(0)) {
    fprintf(stderr, "Error: SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }

  wb_log_init("WinBolo", "WinBoloHeadless", "winbolo-headless.log");
  atexit(wb_log_shutdown);

  if (!serverSimBotPoolInit(0)) {
    fprintf(stderr, "serverSimBotPoolInit failed\n");
    return 1;
  }

  /* Load the process-global preferences document the shared winbolonet code
   * reads through (e.g. httpCreate's [WINBOLO.NET] Host). */
  prefsInit("WinBolo.json");

  if (!clientMutexCreate()) {
    fprintf(stderr, "Error: failed to create client mutex\n");
    return 1;
  }

  /* Set up message handler */
  guiMessageSetHandler(headlessMessageHandler);

  /* Initialize language strings */
  langSetup();

  /* Player name is set on the ClientSim after clientSimCreate (see runNetworkMode/runFastMode) */

  if (optFast) {
    result = runFastMode();
  } else {
    result = runNetworkMode();
  }

  clientMutexDestroy();
  langCleanup();
  serverSimBotPoolDestroy();
  SDL_Quit();

  return result;
}
