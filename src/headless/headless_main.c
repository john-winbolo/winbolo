/*
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

#include "../bolo/screen.h"
#include "../bolo/client_sim.h"
#include "../bolo/frontend.h"
#include "../bolo/players.h"
#include "../bolo/brain.h"
#include "../bolo/pillbox.h"
#include "../bolo/bases.h"
#include "../bolo/transport.h"
#include "../bolo/transport_udp.h"
#include "../bolo/gui_message.h"
#include "../server/server_sim.h"
#include "../gui/brainsHandler.h"
#include "../gui/clientmutex.h"
#include "../gui/gamefront.h"
#include "../common/sentry_integration.h"
#include "../common/wb_log.h"

/* ------------------------------------------------------------------ */
/* Globals needed by the game engine                                   */
/* ------------------------------------------------------------------ */

/* These are referenced by various parts of the engine */
bool isInMenu = FALSE;

/* State logging */
static FILE *logFile = NULL;
static bool logToStdout = FALSE;

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
static bool optQuiet = FALSE;
static bool optFast = FALSE;
static char optMap[512] = "";
static bool optStdin = FALSE;
static bool optLogBinary = FALSE;
static aiType optAi = aiYes;

/* Binary observation format constants */
#define BINARY_SPATIAL_SIZE 29
#define BINARY_NUM_CHANNELS 10
#define BINARY_NUM_SCALARS  17
#define BINARY_MAX_EVENTS   16
static gameType optGameType = gameStrictTournament;

/* Quit flag for signal handler */
static volatile bool headlessQuit = FALSE;

/* Transport state */
static Transport headlessTransport;
static bool transportActive = FALSE;
static BYTE playerNum = 0;
static ClientSim humanSimStorage;
static ClientSim *humanSim = NULL;

/* Fast mode: local server sim */
static ServerSim *fastServerSim = NULL;

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
  screenMakeBrainInfoCS(humanSim, &bi, needMapInit, optAi);
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
    BYTE np = pillsGetNumPills(&fastServerSim->sim.pb);
    int first = 1;
    for (BYTE pi = 1; pi <= np; pi++) {
      pillbox p;
      pillsGetPill(&fastServerSim->sim.pb, &p, pi);
      if (p.owner != selfPlayer || p.inTank) continue;
      if (!first) fprintf(f, ",");
      fprintf(f, "{\"id\":%u,\"tx\":%u,\"ty\":%u,\"terrain\":[", (unsigned)pi, (unsigned)p.x, (unsigned)p.y);
      int pox = (int)p.x - 7;
      int poy = (int)p.y - 7;
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
    BYTE np = pillsGetNumPills(&fastServerSim->sim.pb);
    for (BYTE pi = 1; pi <= np; pi++) {
      pillbox p;
      pillsGetPill(&fastServerSim->sim.pb, &p, pi);
      if (pi > 1) fprintf(f, ",");
      fprintf(f, "{\"tx\":%u,\"ty\":%u,\"owner\":\"%s\",\"armor\":%u,\"in_tank\":%s}",
        (unsigned)p.x, (unsigned)p.y,
        verboseOwnerStr(p.owner, selfPlayer, alliesBits),
        (unsigned)p.armour,
        p.inTank ? "true" : "false");
    }
  }
  fprintf(f, "]");

  /* Bases — full data from server sim */
  fprintf(f, ",\"bases\":[");
  if (fastServerSim != NULL) {
    BYTE nb = basesGetNumBases(&fastServerSim->sim.bs);
    for (BYTE bsi = 1; bsi <= nb; bsi++) {
      base b;
      basesGetBase(&fastServerSim->sim.bs, &b, bsi);
      if (bsi > 1) fprintf(f, ",");
      fprintf(f, "{\"tx\":%u,\"ty\":%u,\"owner\":\"%s\",\"armor\":%u,\"shells\":%u,\"mines\":%u}",
        (unsigned)b.x, (unsigned)b.y,
        verboseOwnerStr(b.owner, selfPlayer, alliesBits),
        (unsigned)b.armour, (unsigned)b.shells, (unsigned)b.mines);
    }
  }
  fprintf(f, "]");

  /* Team summary */
  {
    int self_pills = 0, ally_pills = 0, enemy_pills = 0;
    int self_bases = 0, ally_bases = 0, enemy_bases = 0;
    if (fastServerSim != NULL) {
      BYTE np = pillsGetNumPills(&fastServerSim->sim.pb);
      for (BYTE pi = 1; pi <= np; pi++) {
        pillbox p;
        pillsGetPill(&fastServerSim->sim.pb, &p, pi);
        if (p.owner == 0xFF) { /* neutral — skip */ }
        else if (p.owner == selfPlayer) self_pills++;
        else if (alliesBits & (1u << p.owner)) ally_pills++;
        else enemy_pills++;
      }
      BYTE nb = basesGetNumBases(&fastServerSim->sim.bs);
      for (BYTE bsi = 1; bsi <= nb; bsi++) {
        base b;
        basesGetBase(&fastServerSim->sim.bs, &b, bsi);
        if (b.owner == 0xFF) { /* neutral — skip */ }
        else if (b.owner == selfPlayer) self_bases++;
        else if (alliesBits & (1u << b.owner)) ally_bases++;
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

  /* Cleanup BrainInfo allocations (subset of screenExtractBrainInfoCS —
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

  screenMakeBrainInfoCS(humanSim, &bi, needMapInit, optAi);
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
      BYTE np = pillsGetNumPills(&fastServerSim->sim.pb);
      for (BYTE pi = 1; pi <= np; pi++) {
        pillbox p;
        pillsGetPill(&fastServerSim->sim.pb, &p, pi);
        if (p.inTank) continue;
        int gx = (int)p.x - tank_tx + 14;
        int gy = (int)p.y - tank_ty + 14;
        if (gx < 0 || gx >= 29 || gy < 0 || gy >= 29) continue;
        float intensity = (float)p.armour / 15.0f;
        if (p.owner == 0xFF) {
          spatial[gy][gx][5] = intensity; /* neutral */
        } else if (p.owner == selfPlayer) {
          spatial[gy][gx][6] = intensity; /* self */
        } else {
          spatial[gy][gx][7] = intensity; /* enemy */
        }
      }

      BYTE nb = basesGetNumBases(&fastServerSim->sim.bs);
      for (BYTE bsi = 1; bsi <= nb; bsi++) {
        base b;
        basesGetBase(&fastServerSim->sim.bs, &b, bsi);
        int gx = (int)b.x - tank_tx + 14;
        int gy = (int)b.y - tank_ty + 14;
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
      BYTE np = pillsGetNumPills(&fastServerSim->sim.pb);
      total_pills = np;
      for (BYTE pi = 1; pi <= np; pi++) {
        pillbox p;
        pillsGetPill(&fastServerSim->sim.pb, &p, pi);
        if (p.owner == 0xFF) continue;
        if (p.owner == selfPlayer) self_pills++;
        else if (alliesBits & (1u << p.owner)) ally_pills++;
        else enemy_pills++;
      }
      BYTE nb = basesGetNumBases(&fastServerSim->sim.bs);
      total_bases = nb;
      for (BYTE bsi = 1; bsi <= nb; bsi++) {
        base b;
        basesGetBase(&fastServerSim->sim.bs, &b, bsi);
        if (b.owner == 0xFF) continue;
        if (b.owner == selfPlayer) self_bases++;
        else if (alliesBits & (1u << b.owner)) ally_bases++;
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
    if (optLogState[0] != '\0') {
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
  screenSetAiTypeCS(humanSim, ait);
}

void gameFrontEnableRejoin(void) {
  /* no-op for headless */
}


/* brainsHandler* functions are provided by luabrainshandler.c */

/* ------------------------------------------------------------------ */
/* Tick counters used by network.c and servernet.c                     */
/* ------------------------------------------------------------------ */

time_t windowsGetTicks(void) {
  return (time_t)winboloTimer();
}

time_t serverMainGetTicks(void) {
  return (time_t)winboloTimer();
}

/* ------------------------------------------------------------------ */
/* Helper: sync snapshot from transport                                */
/* ------------------------------------------------------------------ */
static void headlessSyncSnapshot(void) {
  SnapshotHeader snapHdr;
  TankSnapshot snapTanks[MAX_TANKS];
  ShellSnapshot snapShells[MAX_SNAPSHOT_SHELLS];
  TkExplosionSnapshot snapTkExplosions[MAX_SNAPSHOT_TK_EXPLOSIONS];
  BaseSnapshot snapBases[MAX_SNAPSHOT_BASES];
  PillSnapshot snapPills[MAX_SNAPSHOT_PILLS];
  GameEvent snapEvents[MAX_SNAPSHOT_EVENTS];
  if (headlessTransport.getSnapshot(headlessTransport.ctx, playerNum,
                                     &snapHdr, snapTanks, MAX_TANKS,
                                     snapShells, MAX_SNAPSHOT_SHELLS,
                                     snapTkExplosions, MAX_SNAPSHOT_TK_EXPLOSIONS,
                                     snapBases, MAX_SNAPSHOT_BASES,
                                     snapPills, MAX_SNAPSHOT_PILLS,
                                     snapEvents, MAX_SNAPSHOT_EVENTS)) {
    clientSimSyncFromSnapshot(humanSim, &snapHdr, snapTanks, snapHdr.tankCount,
                            snapShells, snapHdr.shellCount,
                            snapTkExplosions, snapHdr.tkExplosionCount,
                            snapBases, snapHdr.baseCount,
                            snapPills, snapHdr.pillCount,
                            snapEvents, snapHdr.reliableEventCount,
                            playerNum);
  }
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
  /* Reset server world (reloads map/pills/bases from its own cache) */
  serverSimResetGameWorld(fastServerSim);
  fastServerSim->lobbyEnabled = false;
  fastServerSim->state = serverStateRunning;
  serverSimAddPlayer(fastServerSim, 0, optName, false);
  fastServerSim->sim.viewPlayer = 0;

  /* Recreate local transport */
  headlessTransport = transportLocalCreate(fastServerSim, 0);
  transportActive = TRUE;
  playerNum = 0;

  /* Reload client sim from cached compressed map */
  humanSim = &humanSimStorage;
  screenLoadCompressedMapCS(humanSim, cachedCompressedMap, cachedCompressedMapLen,
                            "Fast Local", optGameType, false, 0,
                            UNLIMITED_GAME_TIME, optName, 0, FALSE);
  screenSetAiTypeCS(humanSim, optAi);

  /* Sync initial snapshot and place tank */
  headlessSyncSnapshot();
  screenNetSetupTankGoCS(humanSim);

  return true;
}

/* Tear down client sim and transport (but not the server sim) */
static void fastModeTeardownGame(void) {
  brainsHandlerShutdown();
  clientSimDestroy(humanSim);
  transportLocalDestroy(&headlessTransport);
  transportActive = FALSE;
}

static int runFastMode(void) {
  int tickCount = 0;
  bool justKeys = FALSE;
  bool brainRunning;
  uint32_t simTickCounter = 0;

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

  /* Create server sim from map file (first time only — hits disk) */
  fastServerSim = (ServerSim *)malloc(sizeof(ServerSim));
  if (fastServerSim == NULL) {
    fprintf(stderr, "Error: failed to allocate ServerSim\n");
    return 1;
  }
  if (!serverSimCreate(fastServerSim, optMap, optGameType, false, 0, UNLIMITED_GAME_TIME)) {
    fprintf(stderr, "Error: failed to load map '%s'\n", optMap);
    free(fastServerSim);
    fastServerSim = NULL;
    return 1;
  }

  /* Cache compressed map for fast resets */
  {
    BYTE tempMap[65536];
    cachedCompressedMapLen = serverSimGetCompressedMap(fastServerSim, tempMap);
    if (cachedCompressedMapLen <= 0) {
      fprintf(stderr, "Error: failed to compress map\n");
      serverSimDestroy(fastServerSim);
      free(fastServerSim);
      fastServerSim = NULL;
      return 1;
    }
    cachedCompressedMap = (BYTE *)malloc(cachedCompressedMapLen);
    memcpy(cachedCompressedMap, tempMap, cachedCompressedMapLen);
  }

  /* Initial game setup */
  fastServerSim->lobbyEnabled = false;
  fastServerSim->state = serverStateRunning;
  serverSimAddPlayer(fastServerSim, 0, optName, false);
  fastServerSim->sim.viewPlayer = 0;
  headlessTransport = transportLocalCreate(fastServerSim, 0);
  transportActive = TRUE;
  playerNum = 0;
  humanSim = &humanSimStorage;
  screenLoadCompressedMapCS(humanSim, cachedCompressedMap, cachedCompressedMapLen,
                            "Fast Local", optGameType, false, 0,
                            UNLIMITED_GAME_TIME, optName, 0, FALSE);
  screenSetAiTypeCS(humanSim, optAi);
  headlessSyncSnapshot();
  screenNetSetupTankGoCS(humanSim);

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

    if (justKeys) {
      /* Keys tick — replay held buttons from last stdin read */
      InputPacket pkt;
      if (optStdin) {
        memset(&pkt, 0, sizeof(pkt));
        pkt.tick = simTickCounter;
        pkt.playerNum = playerNum;
        pkt.buttons = stdinButtons;
      } else {
        screenBuildInputPacketCS(humanSim, &pkt, 0, FALSE, FALSE, brainRunning, FALSE, playerNum, simTickCounter);
      }
      clientSimKeysTick(humanSim, &pkt);
      headlessTransport.sendInput(headlessTransport.ctx, &pkt);
      headlessTransport.tick(headlessTransport.ctx);
      headlessSyncSnapshot();
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
        screenBuildInputPacketCS(humanSim, &pkt, 0, FALSE, FALSE, brainRunning, TRUE, playerNum, simTickCounter);
      }
      clientSimGameTick(humanSim, &pkt, brainRunning);
      headlessTransport.sendInput(headlessTransport.ctx, &pkt);
      headlessTransport.tick(headlessTransport.ctx);
      headlessSyncSnapshot();
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
  clientSimDestroy(humanSim);
  transportLocalDestroy(&headlessTransport);
  transportActive = FALSE;
  serverSimDestroy(fastServerSim);
  free(fastServerSim);
  fastServerSim = NULL;
  free(cachedCompressedMap);
  cachedCompressedMap = NULL;

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

  /* Initialize the game engine with dummy params (will be re-created after map load) */
  humanSim = &humanSimStorage;
  clientSimCreate(humanSim, 0, FALSE, 0, UNLIMITED_GAME_TIME);
  playersSetMyLastPlayerName(humanSim, optName);

  /* Connect to the server via new UDP transport */
  if (!optQuiet) {
    fprintf(stderr, "Connecting to %s:%u...\n", optServer, optPort);
  }

  headlessTransport = transportUdpClientCreate(humanSim, optServer, optPort, optName, optPassword, "", false,
                                                optTrackerAddr, optTrackerPort);
  if (transportUdpClientGetJoinState(&headlessTransport) == UDP_CLIENT_ERROR) {
    const char *reason = transportUdpClientGetJoinRejectReason(&headlessTransport);
    fprintf(stderr, "Error: failed to connect: %s\n", reason ? reason : "unknown");
    transportUdpClientDestroy(&headlessTransport);
    clientSimDestroy(humanSim);
    return 1;
  }

  /* Wait for join handshake + map download */
  {
    int joinWaitTicks = 0;
    while ((transportUdpClientGetJoinState(&headlessTransport) == UDP_CLIENT_JOINING ||
            transportUdpClientGetJoinState(&headlessTransport) == UDP_CLIENT_DOWNLOADING_MAP) &&
           joinWaitTicks < 1500) {
      headlessTransport.tick(headlessTransport.ctx);
      SDL_Delay(20);
      joinWaitTicks++;
    }
  }

  if (transportUdpClientGetJoinState(&headlessTransport) != UDP_CLIENT_CONNECTED) {
    const char *reason = transportUdpClientGetJoinRejectReason(&headlessTransport);
    fprintf(stderr, "Error: join failed: %s\n", reason ? reason : "timeout");
    transportUdpClientDestroy(&headlessTransport);
    clientSimDestroy(humanSim);
    return 1;
  }

  playerNum = transportUdpClientGetPlayerNum(&headlessTransport);
  transportActive = TRUE;

  /* Load map from server */
  {
    const BYTE *mapData;
    int mapLen = 0;
    gameType serverGame;
    bool serverHiddenMines;
    int32_t serverStartDelay, serverGameLen;

    mapData = transportUdpClientGetMapData(&headlessTransport, &mapLen);
    transportUdpClientGetGameSettings(&headlessTransport, &serverGame,
                                       &serverHiddenMines,
                                       &serverStartDelay, &serverGameLen);

    if (mapData != NULL && mapLen > 0) {
      char savedMapName[MAP_STR_SIZE];
      strncpy(savedMapName, humanSim->mapName, MAP_STR_SIZE - 1);
      savedMapName[MAP_STR_SIZE - 1] = '\0';
      clientSimDestroy(humanSim);
      if (screenLoadCompressedMapCS(humanSim, (BYTE *)mapData, mapLen, savedMapName,
                                   serverGame, serverHiddenMines,
                                   serverStartDelay, serverGameLen,
                                   optName, playerNum, FALSE) == FALSE) {
        fprintf(stderr, "Error: failed to load map from server\n");
        transportUdpClientDestroy(&headlessTransport);
        return 1;
      }
      screenSetLocalTransportCS(humanSim, false);
      screenSetAiTypeCS(humanSim, optAi);
    } else {
      fprintf(stderr, "Error: no map data from server\n");
      transportUdpClientDestroy(&headlessTransport);
      clientSimDestroy(humanSim);
      return 1;
    }
  }

  /* Set up tank at start position */
  screenNetSetupTankGoCS(humanSim);

  /* Gate lobby vs running: if we received PACKET_LOBBY_STATE during
   * join, stay in lobby state; otherwise proceed to running */
  if (humanSim->inLobby) {
    humanSim->mapDownloadComplete = true;
    humanSim->netStat = netLobby;
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
  oldTick = winboloTimer();

  while (!headlessQuit) {
    brainRunning = brainHandlerIsBrainRunning();
    bool used = FALSE;

    ttick = winboloTimer();

    /* Process game ticks */
    if ((ttick - oldTick) > GAME_TICK_LENGTH) {
      while ((ttick - oldTick) > GAME_TICK_LENGTH) {
        if (humanSim->netStat == netLobby || humanSim->netStat == netLobbyCountdown) {
          /* Lobby/countdown: just tick the transport to receive packets */
          headlessTransport.tick(headlessTransport.ctx);
          justKeys = !justKeys;
        } else if (justKeys) {
          /* Keys tick */
          InputPacket pkt;
          screenBuildInputPacketCS(humanSim, &pkt, 0, FALSE, FALSE, brainRunning, FALSE, playerNum, simTickCounter);
          clientMutexWaitFor();
          clientSimKeysTick(humanSim, &pkt);
          clientMutexRelease();
          headlessTransport.recordInput(headlessTransport.ctx, &pkt);
          headlessTransport.tick(headlessTransport.ctx);
          clientMutexWaitFor();
          headlessSyncSnapshot();
          clientMutexRelease();
          simTickCounter++;
          justKeys = FALSE;
        } else {
          /* Game tick */
          InputPacket pkt;
          screenBuildInputPacketCS(humanSim, &pkt, 0, FALSE, FALSE, brainRunning, TRUE, playerNum, simTickCounter);
          clientMutexWaitFor();
          clientSimGameTick(humanSim, &pkt, brainRunning);
          clientMutexRelease();
          headlessTransport.sendInput(headlessTransport.ctx, &pkt);
          headlessTransport.tick(headlessTransport.ctx);
          clientMutexWaitFor();
          headlessSyncSnapshot();
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
        transportUdpClientGetJoinState(&headlessTransport) != UDP_CLIENT_SERVER_SHUTDOWN) {
      brainHandlerRun();
    }

    /* Log state */
    if (used) {
      clientMutexWaitFor();
      logStateTick(tickCount);
      clientMutexRelease();
    }

    /* Check if we've reached the tick limit */
    if (optTicks > 0 && tickCount >= optTicks) {
      if (!optQuiet) {
        fprintf(stderr, "Reached tick limit (%d). Exiting.\n", optTicks);
      }
      break;
    }

    /* Check for server disconnect */
    if (transportUdpClientGetJoinState(&headlessTransport) == UDP_CLIENT_SERVER_SHUTDOWN) {
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
  clientSimDestroy(humanSim);
  transportUdpClientDestroy(&headlessTransport);
  transportActive = FALSE;
  clientMutexRelease();

  return 0;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
  int result;

  srand((unsigned int)(time(NULL) ^ getpid()));
  sentryInit("WinBoloHeadless", argc, argv);
  atexit(sentryClose);

  if (!parseArgs(argc, argv)) {
    return 1;
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

  initWinboloTimer();

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

  endWinboloTimer();
  clientMutexDestroy();
  langCleanup();
  SDL_Quit();

  return result;
}
