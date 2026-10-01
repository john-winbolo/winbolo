/*
 * $Id$
 *
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
*Name:          Screen
*Filename:      screen.c
*Author:        John Morrison
*Creation Date: 28/10/98
*Last Modified:  4/11/98
*Purpose:
*  Provides Interfaces with the front end
*********************************************************/

/* Includes */
#include <math.h>     /* isfinite — a log_RuleSet value */
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#  include <winsock2.h>
#else
#  include <arpa/inet.h>
#endif
#include "lv_global.h"
#include "backend.h"
#include "lv_bolo_map.h"
#include "tiles.h"
#include "lv_pillbox.h"
#include "lv_bases.h"
#include "lv_screencalc.h"
#include "lv_screentank.h"
#include "lv_screenlgm.h"
#include "lv_screenbullet.h"
#include "lv_starts.h"
#include "lv_util.h"
#include "lv_shells.h"
#include "lv_sounddist.h"
#include "lv_players.h"
#include "snapshot.h"
#include "blocks.h"
#include "dns.h"
#include "logviewer.h"
#include "lv_messages.h"
#include "../gui/lang.h"
#include "../gui/ping_kinds.h"   /* PING_DISPLAY_MS, pingKindMessageId */
#include "cJSON.h"               /* scripts.json, read into g_lv->scripts */
#include "../common/wb_log.h"      /* the warnings a full index or list gives */
#include "sim_rules_names.h"     /* simRulesRuleIndex — a rule's name to its index */

/* File-scope pointer to the central LogViewerState */
static LogViewerState *g_lv = NULL;

void lv_screenSetState(LogViewerState *lv) { g_lv = lv; }
LogViewerState *lv_screenGetState(void) { return g_lv; }

/* The lobby settings the recording carries, as the raw log_GameSettings
 * payload (layout in docs/replay-format.md). Length 0 means no settings,
 * which is every log written before the event existed. Held as raw bytes
 * because the payload is append-only: the reader that draws it takes the
 * fields it knows and ignores anything past them. */
static BYTE s_gameSettings[LV_GAME_SETTINGS_MAX];
static int  s_gameSettingsLen = 0;
/* Set once the load-time walk has read the whole file. What it leaves in the
 * store is the last settings event the file holds, which is the settings the
 * round was played under — no such event is written after the round starts.
 * A live feed gets no walk, so this stays FALSE there. */
static bool s_gameSettingsWalked = FALSE;

/* Keep payload as the current settings. A zero length clears the store. */
static void lv_screenStoreGameSettings(const BYTE *payload, int len) {
  if (payload == NULL || len <= 0) {
    s_gameSettingsLen = 0;
    return;
  }
  if (len > LV_GAME_SETTINGS_MAX) {
    len = LV_GAME_SETTINGS_MAX;
  }
  memcpy(s_gameSettings, payload, (size_t)len);
  s_gameSettingsLen = len;
}

/* A settings event the decoder passed. On a live feed this is the only way
 * the settings arrive. On a loaded file the walk has already read the whole
 * stream, so an event the playhead crosses is an older one it has seen and
 * discarded — replaying it would put the lobby's opening settings back in
 * place of the round's. */
static void lv_screenPlaybackGameSettings(const BYTE *payload, int len) {
  if (s_gameSettingsWalked) {
    return;
  }
  lv_screenStoreGameSettings(payload, len);
}

int lv_screenGetGameSettings(BYTE *out, int maxLen) {
  int len = s_gameSettingsLen;

  if (out == NULL || len <= 0) {
    return 0;
  }
  if (len > maxLen) {
    len = maxLen;
  }
  memcpy(out, s_gameSettings, (size_t)len);
  return len;
}

/* Accessor functions for sounddist.c (replaces extern globals) */
BYTE lv_screenGetXOffset(void) { return g_lv->xOffset; }
BYTE lv_screenGetYOffset(void) { return g_lv->yOffset; }
bool lv_screenGetFastForwarding(void) { return g_lv->fastForwarding; }
uint32_t lv_screenGetTimeRunning(void) { return g_lv->timeRunning; }

/* Decode a log_RuleSet payload from its two index bytes, its blob's length
 * byte and the blob. TRUE, with *index and *value set, only for a length of
 * eight, an index this build has a rule for and a finite value: the record
 * comes from a file anyone could have written, and nothing else in one is a
 * rule change. The value is an IEEE-754 double, most significant byte first. */
static bool lv_ruleSetDecode(BYTE idxHi, BYTE idxLo, BYTE len, const BYTE *blob,
                             int *index, double *value) {
  int      rule = ((int)idxHi << 8) | (int)idxLo;
  uint64_t bits = 0;
  double   v;
  int      i;

  if (len != 8 || rule >= SIM_RULE_COUNT) {
    return FALSE;
  }
  for (i = 0; i < 8; i++) {
    bits = (bits << 8) | (uint64_t)blob[i];
  }
  memcpy(&v, &bits, sizeof(v));
  if (!isfinite(v)) {
    return FALSE;
  }
  *index = rule;
  *value = v;
  return TRUE;
}

/* Add a change to the recording's list. unlessPresent is what playback
 * passes: when the list already holds a change to the same rule at the same
 * time, no second entry is added. On a loaded file the load walk collected the
 * change first, and on a live feed a seek back replays changes already added.
 *
 * One entry at that time takes this value. That is how a live feed, which
 * collects as playback reads, ends a tick that changed a rule twice on the
 * value the tick ended with, which is the answer lv_screenRuleValueAt gives a
 * loaded file. More than one entry at that time can only be the walk's, which
 * already holds each of the tick's values in file order, so it is left as it
 * is.
 *
 * A full list keeps what it has and sets the truncated flag, with one
 * warning. */
static void lv_ruleChangeAppend(uint32_t ms, int index, double value,
                                bool unlessPresent) {
  int i;

  if (unlessPresent) {
    int found   = -1;
    int matches = 0;
    for (i = 0; i < g_lv->ruleChangeCount; i++) {
      if (g_lv->ruleChanges[i].ms == ms && g_lv->ruleChanges[i].index == index) {
        found = i;
        matches++;
      }
    }
    if (matches == 1) {
      g_lv->ruleChanges[found].value = value;
    }
    if (matches > 0) {
      return;
    }
  }
  if (g_lv->ruleChangeCount >= LV_RULE_CHANGES_MAX) {
    if (!g_lv->ruleChangesTruncated) {
      WB_LOG_WARN(WB_LOG_CAT_LOGVIEWER,
                  "recording has more than %d rule changes; the rest are not "
                  "shown", LV_RULE_CHANGES_MAX);
    }
    g_lv->ruleChangesTruncated = TRUE;
    return;
  }
  g_lv->ruleChanges[g_lv->ruleChangeCount].ms    = ms;
  g_lv->ruleChanges[g_lv->ruleChangeCount].index = index;
  g_lv->ruleChanges[g_lv->ruleChangeCount].value = value;
  g_lv->ruleChangeCount++;
}

static void lv_ruleChangesClear(void) {
  g_lv->ruleChangeCount      = 0;
  g_lv->ruleChangesTruncated = FALSE;
}

double lv_screenRuleValueAt(int index, uint32_t ms) {
  const LvRuleChange *c;
  bool                found  = FALSE;
  uint32_t            bestMs = 0;
  double              best   = 0.0;
  int                 i;

  if (index < 0 || index >= SIM_RULE_COUNT || g_lv == NULL) {
    return simRulesClassicValue(index);
  }
  /* The last change at or before ms. Two changes to one rule at the same
     time are one tick's, and the later in the file is the value the tick
     ended on. */
  for (i = 0; i < g_lv->ruleChangeCount; i++) {
    c = &g_lv->ruleChanges[i];
    if (c->index == index && c->ms <= ms && (found == FALSE || c->ms >= bestMs)) {
      found  = TRUE;
      bestMs = c->ms;
      best   = c->value;
    }
  }
  if (found == TRUE) {
    return best;
  }
  for (i = 0; i < g_lv->scripts.ruleCount && i < SIM_RULE_COUNT; i++) {
    if (g_lv->scripts.rules[i].index == index) {
      return g_lv->scripts.rules[i].value;
    }
  }
  return simRulesClassicValue(index);
}

/* A rule's value as a whole number in [lo, hi]. The comparison is written so
 * a NaN lands on lo. */
static int lv_rulesClampInt(double v, int lo, int hi) {
  if (!(v >= (double)lo)) {
    return lo;
  }
  if (v > (double)hi) {
    return hi;
  }
  return (int)v;
}

/* Fill g_lv->rules with every rule's value at the playhead. Run wherever the
 * playhead moves without passing each record in between: at the end of a
 * load, after a snapshot restore and at the end of a seek. Playback passing
 * a log_RuleSet runs it from that record's case. */
static void lv_rulesRefresh(void) {
  uint32_t t = g_lv->timeRunning;

  g_lv->rules.tankFullShells = (BYTE)lv_rulesClampInt(
      lv_screenRuleValueAt(SIM_RULE_tank_full_shells, t), 0, 255);
  g_lv->rules.tankFullMines  = (BYTE)lv_rulesClampInt(
      lv_screenRuleValueAt(SIM_RULE_tank_full_mines, t), 0, 255);
  g_lv->rules.tankFullArmour = (BYTE)lv_rulesClampInt(
      lv_screenRuleValueAt(SIM_RULE_tank_full_armour, t), 0, 255);
  g_lv->rules.tankFullTrees  = (BYTE)lv_rulesClampInt(
      lv_screenRuleValueAt(SIM_RULE_tank_full_trees, t), 0, 255);
  g_lv->rules.baseFullShells = (BYTE)lv_rulesClampInt(
      lv_screenRuleValueAt(SIM_RULE_base_full_shells, t), 0, 255);
  g_lv->rules.baseFullMines  = (BYTE)lv_rulesClampInt(
      lv_screenRuleValueAt(SIM_RULE_base_full_mines, t), 0, 255);
  g_lv->rules.baseFullArmour = (BYTE)lv_rulesClampInt(
      lv_screenRuleValueAt(SIM_RULE_base_full_armour, t), 0, 255);
  g_lv->rules.pillMaxArmour  = lv_rulesClampInt(
      lv_screenRuleValueAt(SIM_RULE_pill_max_armour, t), 1, 255);
}

int lv_screenGetRuleChanges(const LvRuleChange **out) {
  if (out != NULL) {
    *out = (g_lv != NULL) ? g_lv->ruleChanges : NULL;
  }
  return (g_lv != NULL) ? g_lv->ruleChangeCount : 0;
}

const LvScripts *lv_screenGetScripts(void) {
  if (g_lv == NULL || g_lv->logLoaded == FALSE) {
    return NULL;
  }
  return &g_lv->scripts;
}

/* --- Smart pings ----------------------------------------------------
 * Every log_Ping the playback has walked past that is still inside its
 * PING_DISPLAY_MS. Aged on g_lv->timeRunning, the playback clock, so a ping
 * lasts the same five seconds of game time however fast the round is being
 * played back — and a rewind, which winds that clock backwards, leaves every
 * stored ping in the future and so drops them all with no explicit reset.
 *
 * The viewer shows every team's pings: a replay is watched from outside, so
 * there is no team to filter to. */
#define LV_MAX_PINGS 16

typedef struct {
  BYTE     sender;
  BYTE     kind;
  uint16_t worldX;
  uint16_t worldY;
  uint32_t timeMs;   /* playback clock at the ping, +1 so 0 means "empty" */
} lvPing;

static lvPing s_pings[LV_MAX_PINGS];
static int    s_pingWrite = 0;

/* Drop every stored ping. A new log restarts the playback clock at zero, so
   the previous log's pings all read as "in the future" and are hidden — until
   playback runs past the time they were stored at, when they would come back
   over a replay they were never part of. Called from lv_screenSetup, which
   every load path runs through. */
static void lv_pingReset(void) {
  memset(s_pings, 0, sizeof(s_pings));
  s_pingWrite = 0;
}

static void lv_pingAdd(BYTE sender, BYTE kind, uint16_t wx, uint16_t wy) {
  s_pings[s_pingWrite].sender = sender;
  s_pings[s_pingWrite].kind   = kind;
  s_pings[s_pingWrite].worldX = wx;
  s_pings[s_pingWrite].worldY = wy;
  s_pings[s_pingWrite].timeMs = g_lv->timeRunning + 1;
  s_pingWrite = (s_pingWrite + 1) % LV_MAX_PINGS;
}

/* Read out one live ping. `index` walks 0..LV_MAX_PINGS-1 from the oldest
 * slot, so drawing in order puts the newest on top; a slot that is empty,
 * expired or (after a rewind) still in the future returns false and the
 * caller moves on. ageMs is measured on the playback clock. `sender` is the
 * slot that sent it, which is how the drawer names the marker. */
bool lv_screenGetPing(int index, BYTE *sender, BYTE *kind, uint16_t *worldX,
                      uint16_t *worldY, uint32_t *ageMs) {
  const lvPing *p;
  uint32_t at;
  if (index < 0 || index >= LV_MAX_PINGS) return FALSE;
  p = &s_pings[(s_pingWrite + index) % LV_MAX_PINGS];
  if (p->timeMs == 0) return FALSE;
  at = p->timeMs - 1;
  if (g_lv->timeRunning < at) return FALSE;
  if (g_lv->timeRunning - at >= (uint32_t)PING_DISPLAY_MS) return FALSE;
  if (sender) *sender = p->sender;
  if (kind)   *kind   = p->kind;
  if (worldX) *worldX = p->worldX;
  if (worldY) *worldY = p->worldY;
  if (ageMs)  *ageMs  = g_lv->timeRunning - at;
  return TRUE;
}

int lv_screenGetPingCapacity(void) { return LV_MAX_PINGS; }

/* Store a slot's tank stocks. The recording is the only source: the four
 * values arrive either on the end of a snapshot's player block or in a
 * log_TankSetStock record. All zeros is what a slot gets when the recording
 * carries neither — a file written before the stocks were recorded, or a slot
 * not in use — and the status panel then draws empty bars rather than a
 * guessed number. */
static void lv_screenSetTankStock(BYTE slot, BYTE shells, BYTE mines, BYTE armour, BYTE trees) {
  if (slot >= MAX_TANKS) return;
  g_lv->tankInv[slot].shells = shells;
  g_lv->tankInv[slot].mines  = mines;
  g_lv->tankInv[slot].armour = armour;
  g_lv->tankInv[slot].trees  = trees;
}

/* Store a slot's modifier set. A log_TankSetModifiers record replaces the whole
 * set, as the op that wrote it did. Nothing draws these yet. */
static void lv_screenSetTankModifiers(BYTE slot, const BYTE *mods) {
  if (slot >= MAX_TANKS) return;
  g_lv->tankMods[slot].speed  = mods[0];
  g_lv->tankMods[slot].accel  = mods[1];
  g_lv->tankMods[slot].turn   = mods[2];
  g_lv->tankMods[slot].reload = mods[3];
  g_lv->tankMods[slot].dealt  = mods[4];
  g_lv->tankMods[slot].taken  = mods[5];
}

// Some prototypes to cleanup and document

bool logIsEOF();

int logReadBytes(BYTE *buff, int len);

void lv_updateItem(BYTE itemType, BYTE itemNumber, BYTE owner, BYTE x, BYTE y, BYTE armour, BYTE shells, BYTE mines, bool inTank);

bool lv_processSnapshot();
bool lv_logLoad(char *fileName, int memoryBufferSize);
void lv_frontEndSetGameInformation(bool clear, BYTE versionMajor, BYTE versionMinor, BYTE versionRevision, char *mapName, BYTE gameType, bool hiddenMines, BYTE aiType, int32_t startDelay, int32_t timeLimit, BYTE *wbnKey, int32_t startTime);
void lv_startOfLog();
void lv_windowRemoveEvents();
void lv_windowRemoveEventsAfter(uint32_t timeMs);

/*********************************************************
*NAME:          lv_screenCalcSquare
*AUTHOR:        John Morrison
*CREATION DATE: 29/10/98
*LAST MODIFIED:  1/11/99
*PURPOSE:
*  Calculates the terrain type for a given location
*
*ARGUMENTS:
*  xValue - The x co-ordinate
*  yValue - The y co-ordinate
*********************************************************/
BYTE lv_screenCalcSquare(BYTE xValue, BYTE yValue, BYTE scrX, BYTE scrY) {
  baseAlliance ba;  /* The allience of a base */
  BYTE returnValue; /* Value to return */
  BYTE currentPos;
  BYTE aboveLeft;
  BYTE above;
  BYTE aboveRight;
  BYTE leftPos;
  BYTE rightPos;
  BYTE belowLeft;
  BYTE below;
  BYTE belowRight;

/* Stride is sizeX+1 (not sizeX) because the screen buffer carries a
 * one-tile margin column/row beyond the visible viewport, used by the
 * sub-tile-scrolling blit to avoid the trailing-edge bleed. */
int a = (scrY*(lv_screenGetSizeX()+1))+scrX ;

  if (a > 1989) {
    above = 1;
  }
  *((*g_lv->mineView).mineItem+a) = FALSE;
  /* Set up Items */
  if ((lv_pillsExistPos(&g_lv->pb,xValue,yValue)) == TRUE) {
    returnValue = lv_pillsGetScreenHealth(&g_lv->pb, xValue, yValue,
                                          g_lv->rules.pillMaxArmour);
  } else if ((lv_basesExistPos(&g_lv->bs,xValue,yValue)) == TRUE) {
     ba = lv_basesGetAlliancePos(&g_lv->bs, xValue, yValue);
    switch (ba) {
    case baseOwnGood:
      returnValue = BASE_GOOD;
      break;
    case baseAllieGood:
      returnValue = BASE_GOOD;
      break;
    case baseNeutral:
      returnValue = BASE_NEUTRAL;
      break;
    case baseDead:
      if (lv_basesAmOwner(&g_lv->bs, lv_playersGetSelf(), xValue, yValue) == TRUE) {
        returnValue = BASE_GOOD;
      } else {
        returnValue = BASE_EVIL;
      }
      break;
    case baseEvil:
    default:
      /* Base Evil */
      returnValue = BASE_EVIL;
    }
  }  else {
    currentPos = lv_mapGetPos(&g_lv->mp,xValue,yValue);
    if (lv_mapIsMine(&g_lv->mp, xValue, yValue) == TRUE) {
      *((*g_lv->mineView).mineItem+a) = TRUE;
      if (currentPos != DEEP_SEA) {
        currentPos = currentPos - MINE_SUBTRACT;
      }
    } else {
      *((*g_lv->mineView).mineItem+a) = FALSE;
    }

    if (lv_basesExistPos(&g_lv->bs, (BYTE) (xValue-1), (BYTE) (yValue-1)) == TRUE) {
      aboveLeft = ROAD;
    } else {
      aboveLeft = lv_mapGetPos(&g_lv->mp,(BYTE) (xValue-1),(BYTE) (yValue-1));
      if (aboveLeft >= MINE_START && aboveLeft <= MINE_END) {
        aboveLeft = aboveLeft - MINE_SUBTRACT;
      }
    }

    if (lv_basesExistPos(&g_lv->bs, xValue, (BYTE) (yValue-1)) == TRUE) {
      above = ROAD;
    } else {
      above = lv_mapGetPos(&g_lv->mp,xValue,(BYTE) (yValue-1));
      if (above >= MINE_START && above <= MINE_END) {
        above = above - MINE_SUBTRACT;
      }
    }

    if (lv_basesExistPos(&g_lv->bs, (BYTE) (xValue+1), (BYTE) (yValue-1)) == TRUE) {
      aboveRight = ROAD;
    } else {
      aboveRight = lv_mapGetPos(&g_lv->mp,(BYTE) (xValue+1),(BYTE) (yValue-1));
      if (aboveRight >= MINE_START && aboveRight <= MINE_END) {
        aboveRight = aboveRight - MINE_SUBTRACT;
      }
    }

    if (lv_basesExistPos(&g_lv->bs, (BYTE) (xValue-1), yValue) == TRUE) {
      leftPos = ROAD;
    } else {
      leftPos = lv_mapGetPos(&g_lv->mp,(BYTE) (xValue-1),yValue);
      if (leftPos >= MINE_START && leftPos <= MINE_END) {
        leftPos = leftPos - MINE_SUBTRACT;
      }
    }

    if (lv_basesExistPos(&g_lv->bs, (BYTE) (xValue+1), yValue) == TRUE) {
      rightPos = ROAD;
    } else {
      rightPos = lv_mapGetPos(&g_lv->mp,(BYTE) (xValue+1),yValue);
      if (rightPos >= MINE_START && rightPos <= MINE_END) {
        rightPos = rightPos - MINE_SUBTRACT;
      }
    }

    if (lv_basesExistPos(&g_lv->bs, (BYTE) (xValue-1), (BYTE) (yValue+1)) == TRUE) {
      belowLeft = ROAD;
    } else {
      belowLeft = lv_mapGetPos(&g_lv->mp,(BYTE) (xValue-1),(BYTE) (yValue+1));
      if (belowLeft >= MINE_START && belowLeft <= MINE_END) {
        belowLeft = belowLeft - MINE_SUBTRACT;
      }
    }


    if (lv_basesExistPos(&g_lv->bs, xValue, (BYTE) (yValue+1)) == TRUE) {
      below = ROAD;
    } else {
      below = lv_mapGetPos(&g_lv->mp,xValue,(BYTE) (yValue+1));
      if (below >= MINE_START && below <= MINE_END) {
        below = below - MINE_SUBTRACT;
      }
    }

    if (lv_basesExistPos(&g_lv->bs, (BYTE) (xValue+1), (BYTE) (yValue+1)) == TRUE) {
      belowRight = ROAD;
    } else {
      belowRight = lv_mapGetPos(&g_lv->mp,(BYTE) (xValue+1),(BYTE) (yValue+1));
      if (belowRight >= MINE_START && belowRight <= MINE_END) {
        belowRight = belowRight - MINE_SUBTRACT;
      }
    }

    switch (currentPos) {
    case ROAD:
      returnValue = lv_screenCalcRoad(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
      break;
    case BUILDING:
      returnValue = lv_screenCalcBuilding(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
      break;
    case FOREST:
      returnValue = lv_screenCalcForest(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
      break;
    case RIVER:
      returnValue = lv_screenCalcRiver(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
      break;
    case DEEP_SEA:
      returnValue = lv_screenCalcDeepSea(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
      break;
    case BOAT:
      returnValue = lv_screenCalcBoat(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
      break;
    default:
      returnValue = currentPos;
      break;
    }
  }
  return returnValue;
}

/*********************************************************
*NAME:          lv_screenUpdateView
*AUTHOR:        John Morrison
*CREATION DATE: 29/10/98
*LAST MODIFIED: 29/10/98
*PURPOSE:
*  Updates the values in the view area
*
*ARGUMENTS:
* value - The update type (Helps in optimisations)
*********************************************************/
void lv_screenUpdateView(updateType value) {
  /* int (not BYTE): the loops run to ssx/ssy inclusive, which clamp to 255 at
   * a fullscreen, zoomed-out viewport. A BYTE counter would wrap 255->0 at
   * count++ and keep satisfying count <= 255, looping forever (hard lockup). */
  int count;    /* Looping Variables */
  int count2;
  int ssx = lv_screenGetSizeX();
  int ssy = lv_screenGetSizeY();

  if (g_lv->logLoaded == FALSE) {
    return;
  }

  if (g_lv->centredTank == FALSE || value == redraw) {
    if (value == left) {
      g_lv->xOffset--;
    } else if (value == right) {
      g_lv->xOffset++;
    } else if (value == up) {
      g_lv->yOffset--;
    } else if (value == down) {
      g_lv->yOffset++;
    }
  }

  /* Iterate sizeX+1 by sizeY+1 to populate one extra column and row
   * beyond the visible viewport. The margin tile is shown when sub-tile
   * scrolling shifts the final blit, eliminating the trailing-edge
   * bleed. The margin coords reach 255 at the map boundary, which is
   * still in-range for lv_mapGetPos; adjacency reads inside
   * lv_screenCalcSquare wrap (BYTE +1 of 255 -> 0) but only when
   * subPx == 0, in which case the blit's srcRect clips the margin
   * tile from view, so the wrong adjacency is never user-visible. */
  for (count=0;count <= ssx; count++) {
    for (count2=0;count2 <= ssy; count2++) {
      *((*g_lv->view).screenItem+((ssx+1)*count2)+count) = lv_screenCalcSquare((BYTE) (count+g_lv->xOffset),(BYTE) (count2+g_lv->yOffset), count, count2);
    }
  }
}



/*********************************************************
*NAME:          lv_screenSetup
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Sets up all the variables - Should be run when the
*  program starts.
*
*ARGUMENTS:
*
*********************************************************/
void lv_screenSetup() {
  int a = 0;
  /* Every load path runs through here, so drop the previous log's recorded
     settings before the new one's walk can collect its own. */
  lv_screenStoreGameSettings(NULL, 0);
  s_gameSettingsWalked = FALSE;
  lv_pingReset();
  g_lv->gmeStartDelay = 0;
  g_lv->gmeLength = UNLIMITED_GAME_TIME;
  g_lv->isPlaying = FALSE;
  g_lv->logLoaded = FALSE;
  g_lv->xOffset = 127;
  g_lv->yOffset = 127;
  g_lv->subPxX  = 0;
  g_lv->subPxY  = 0;
  lv_mapCreate(&g_lv->mp);
  lv_pillsCreate(&g_lv->pb);
  lv_startsCreate(&g_lv->ss);
  lv_basesCreate(&g_lv->bs);
  lv_playersCreate();
  lv_playersSetSelf(0);
  g_lv->shs = lv_shellsCreate();
  if (g_lv->view != NULL) {
    free((*g_lv->view).screenItem);
    Dispose(g_lv->view);
  }
  New(g_lv->view);
  if (g_lv->view != NULL) {
    (*g_lv->view).screenItem = malloc((lv_screenGetSizeX()+2) * (lv_screenGetSizeY()+2));
  }
  if (g_lv->mineView != NULL) {
    free((*g_lv->mineView).mineItem);
    free(g_lv->mineView);
  }
  a = (lv_screenGetSizeX()+1) * (lv_screenGetSizeY()+1);
  New(g_lv->mineView);
  if (g_lv->mineView != NULL) {
    (*g_lv->mineView).mineItem = malloc(a * sizeof(bool));
  }

  lv_screenUpdateView(redraw);
}

/*********************************************************
*NAME:          lv_screenDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Destroys the structures Should be called on
*  program exit
*
*ARGUMENTS:
*
*********************************************************/
void lv_screenDestroy() {
  if (g_lv->mp != NULL) {
    lv_mapDestroy(&g_lv->mp);
    g_lv->mp = NULL;
  }
  if (g_lv->pb != NULL) {
    lv_pillsDestroy(&g_lv->pb);
    g_lv->pb = NULL;
  }
  if (g_lv->ss != NULL) {
    lv_startsDestroy(&g_lv->ss);
    g_lv->ss = NULL;
  }
  if (g_lv->bs != NULL) {
    lv_basesDestroy(&g_lv->bs);
    g_lv->bs = NULL;
  }
  lv_playersDestroy();
  if (g_lv->shs != NULL) {
    lv_shellsDestroy(&g_lv->shs);
    g_lv->shs = NULL;
  }
  if (g_lv->view != NULL) {
    free((*g_lv->view).screenItem);
    Dispose(g_lv->view);
    g_lv->view = NULL;
  }
  if (g_lv->mineView != NULL) {
    free((*g_lv->mineView).mineItem);
    Dispose(g_lv->mineView);
    g_lv->mineView = NULL;
  }
  g_lv->isPlaying = FALSE;
  g_lv->logLoaded = FALSE;
}

void lv_frontEndDrawMainScreen(screen *value, screenMines *mineView, screenTanks *tks, screenGunsight *gs, screenBullets *sBullet, screenLgm *lgms, int32_t srtDelay, bool isPillView, int edgeX, int edgeY);


/*********************************************************
*NAME:          lv_screenUpdate
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Updates the screen. Takes numerous directions
*
*ARGUMENTS:
*  value - Pointer to the starts structure
*********************************************************/
void lv_screenUpdate(updateType value) {
  screenTanks st;
  screenBullets sb;
  screenLgm sl;


  lv_screenLgmCreate(&sl);
  sb = lv_screenBulletsCreate();
  lv_screenTanksCreate(&st);

  if (g_lv->logLoaded == FALSE) {
    return;
  }

  lv_screenUpdateView(value);
  {
    int rightEdge = (int)g_lv->xOffset + lv_screenGetSizeX();
    int bottomEdge = (int)g_lv->yOffset + lv_screenGetSizeY();
    if (rightEdge > 255) rightEdge = 255;
    if (bottomEdge > 255) bottomEdge = 255;
    lv_playersMakeScreenLgm(&sl, g_lv->xOffset, (BYTE) rightEdge, g_lv->yOffset, (BYTE) bottomEdge);
    lv_shellsCalcScreenBullets(&g_lv->shs, &sb, g_lv->xOffset, (BYTE) rightEdge, g_lv->yOffset, (BYTE) bottomEdge);
    lv_playersMakeScreenTanks(&st, g_lv->xOffset, (BYTE) rightEdge, g_lv->yOffset, (BYTE) bottomEdge);
  }
  lv_frontEndDrawMainScreen(&g_lv->view, &g_lv->mineView, &st, NULL, &sb, &sl, 0, FALSE, 0, 0);
  lv_screenTanksDestroy(&st);
  lv_screenBulletsDestroy(&sb);
  lv_screenLgmDestroy(&sl);
}

/*********************************************************
*NAME:          lv_screenSetPos
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Sets the value of a square in the structure
*
*ARGUMENTS:
*  xValue - The X co-ordinate
*  yValue - The Y co-ordinate
*  terrain - Terraint to set to
*********************************************************/
void lv_screenSetPos(BYTE xValue, BYTE yValue, BYTE terrain) {
  lv_mapSetPos(&g_lv->mp, xValue, yValue, terrain);
  lv_basesDeleteBase(&g_lv->bs, xValue, yValue);
  lv_startsDeleteStart(&g_lv->ss, xValue, yValue);
  lv_pillsDeletePill(&g_lv->pb, xValue, yValue);
}

/*********************************************************
*NAME:          lv_screenGetPos
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Gets the value of a square in the structure
*  Return DEEP_SEA if out of range
*
*ARGUMENTS:
*  value  - Pointer to the screen structure
*  xValue - The X co-ordinate
*  yValue - The Y co-ordinate
*********************************************************/
BYTE lv_screenGetPos(screen *value,BYTE xValue, BYTE yValue) {
  BYTE returnValue = DEEP_SEA; /* Value to return */

  /* Buffer holds sizeX+1 by sizeY+1 tiles (visible + 1-tile margin for
   * sub-tile scrolling). Stride is sizeX+1. */
  if (xValue <= lv_screenGetSizeX() && yValue <= lv_screenGetSizeY()) {
      returnValue = *((*g_lv->view).screenItem+(yValue*(lv_screenGetSizeX()+1)+xValue));
  }
  return returnValue;
}

#include "lv_log.h"
void lv_windowAddEvent(int eventType, char *msg);

/* --- Scenario presentation --------------------------------------------
 * The panels, scores, announcement, markers and status lines a scenario put
 * up, as the recording's log_ScnPanel, log_ScnScore, log_ScnAnnounce,
 * log_ScnMarker and log_ScnStatus records state them, kept in g_lv->pres.
 * Each record replaces the state for its own key — one panel row, one score,
 * the announcement, one marker or one status row — so what a store holds at
 * a time is what the last record for its key up to that time left there.
 *
 * Each slot's lobby team rides the same index, from log_TeamSet, because a
 * team panel is drawn for the players on that team and the snapshots do not
 * carry the team either. Only the store is shared: the newswire line a
 * log_TeamSet posts stays in playback's own case, so a rebuild posts none.
 * The walk indexes framed files only, so a v1 log's log_TeamSet records are
 * never indexed and its teams are lost at the first seek; a v1 log carries no
 * panels for a team to pick between. A team given before the recording
 * starts (a log with no lobby, or a spectator's seed) is not known until the
 * next log_TeamSet for that slot.
 *
 * Playback applies each record as it passes it. The snapshots carry none of
 * this, so a jump of the playhead cannot be followed by playback alone. The
 * load walk checks every one of these records once and indexes the ones that
 * pass, with the key each fills; lv_presRebuild then clears the stores and,
 * for each key, applies the last indexed record at or before the new time. A
 * live feed has no walk and no index, and keeps what playback applied.
 *
 * Every field comes from a file anyone could have written. A record that
 * fails a check is consumed and changes nothing. */

/* The keys, one per store a record can fill: the panel rows, the slot
   scores, the team scores for teams 1 to 15, the announcement, the markers
   and each slot's team. */
#define LV_PRES_KEY_PANEL         0
#define LV_PRES_KEY_PLAYER_SCORE  (LV_PRES_KEY_PANEL + LV_PRES_PANEL_ROWS)
#define LV_PRES_KEY_TEAM_SCORE    (LV_PRES_KEY_PLAYER_SCORE + MAX_TANKS)
#define LV_PRES_KEY_ANNOUNCE      (LV_PRES_KEY_TEAM_SCORE + LV_PRES_TEAMS - 1)
#define LV_PRES_KEY_MARKER        (LV_PRES_KEY_ANNOUNCE + 1)
#define LV_PRES_KEY_SLOT_TEAM     (LV_PRES_KEY_MARKER + SCN_MARKERS_MAX)
#define LV_PRES_KEY_STATUS        (LV_PRES_KEY_SLOT_TEAM + MAX_TANKS)
#define LV_PRES_KEYS              (LV_PRES_KEY_STATUS + LV_PRES_PANEL_ROWS)

/* A key rides in a byte. */
BOLO_STATIC_ASSERT(LV_PRES_KEYS <= 256, lv_pres_key_fits_a_byte);

/* One record that passed its checks, as the load walk found it: the time
   playback reaches it, its type code, the key it fills, where its payload
   starts and its framed length, which is what says whether an announcement
   carries its position bytes. */
typedef struct {
  uint32_t       ms;
  BYTE           code;
  BYTE           key;
  unsigned short frameLen;
  size_t         payloadPos;
} LvPresRecord;

/* Most records the index holds for one recording. It grows as the walk
   finds records, and one past this many sets s_presIndexTruncated. */
#define LV_PRES_INDEX_MAX 65536

/* Sorted by key at the end of the walk, each key's records in file order:
   s_presKeyStart[k] is where key k's run begins, and s_presKeyStart[k + 1]
   where it ends. */
static LvPresRecord *s_presIndex          = NULL;
static int           s_presIndexCount     = 0;
static int           s_presIndexCap       = 0;
static int           s_presKeyStart[LV_PRES_KEYS + 1];
static bool          s_presIndexTruncated = FALSE;
/* Set once the load walk has read the whole file. */
static bool          s_presWalked         = FALSE;
/* A feed has no load walk, so playback indexes each record the first time it
   reads one. s_presLive is set once it has indexed any, s_presLiveLastPos is
   the payload position of the last record it looked at, so a record replayed
   after a seek back is not indexed twice, and s_presIndexUnsorted says records
   were added since the index was last sorted by key. With neither a walk nor
   a record indexed there is nothing to rebuild from, and lv_presRebuild
   leaves the stores alone. */
static bool          s_presLive           = FALSE;
static bool          s_presLiveAny        = FALSE;
static size_t        s_presLiveLastPos    = 0;
static bool          s_presIndexUnsorted  = FALSE;
/* Where a list is parsed to be checked. Static because a parsed list is
   several kilobytes. */
static ScnPanelList  s_presPanelScratch;

/* One record's payload as it came off the stream. */
typedef struct {
  BYTE     code;
  bool     whole;                 /* every byte the lengths named was there */
  BYTE     hdr[6];                /* the fixed bytes ahead of the length */
  unsigned len;                   /* the list's, label's, line's or blob's */
  BYTE     data[SCN_PANEL_MAX];   /* its bytes, when len fits */
  bool     hasPos;                /* an announcement's two position bytes */
  BYTE     pos[2];                /* across, then down */
} LvPresPayload;

/* The payload being read. Static for the same reason as the scratch list,
   and one is enough: nothing reads two at once. */
static LvPresPayload s_presPayload;

static void lv_presClearStores(void) {
  memset(&g_lv->pres, 0, sizeof(g_lv->pres));
}

/* Drop the stores and the index, as every load and close does. */
static void lv_presReset(void) {
  if (g_lv != NULL) {
    lv_presClearStores();
  }
  free(s_presIndex);
  s_presIndex          = NULL;
  s_presIndexCount     = 0;
  s_presIndexCap       = 0;
  s_presIndexTruncated = FALSE;
  s_presWalked         = FALSE;
  s_presLive           = FALSE;
  s_presLiveAny        = FALSE;
  s_presLiveLastPos    = 0;
  s_presIndexUnsorted  = FALSE;
  memset(s_presKeyStart, 0, sizeof(s_presKeyStart));
}

static void lv_presIndexAdd(uint32_t ms, BYTE code, int key,
                            size_t payloadPos, unsigned short frameLen);

/* On a feed, index a record playback has just stored under key, the way the
 * load walk would have indexed it, so a seek back can rebuild the stores from
 * the index. Only a record past every one looked at so far is added: a seek
 * back replays records the feed has already indexed. Payload positions only
 * grow through a feed, and each key's records must stay in that order for the
 * rebuild's search, so the index is marked for a sort rather than kept sorted
 * here. Does nothing on a loaded file, whose walk indexed every record. */
static void lv_presLiveIndex(uint32_t ms, BYTE code, int key,
                             size_t payloadPos, unsigned short frameLen) {
  if (s_presWalked || key < 0) {
    return;
  }
  if (s_presLiveAny && payloadPos <= s_presLiveLastPos) {
    return;
  }
  s_presLiveAny     = TRUE;
  s_presLiveLastPos = payloadPos;
  lv_presIndexAdd(ms, code, key, payloadPos, frameLen);
  s_presLive          = TRUE;
  s_presIndexUnsorted = TRUE;
}

/* A destination pair as the server writes one: team 0 to 15, and 0xFF or a
   player slot. */
static bool lv_presDestValid(BYTE destTeam, BYTE destPlayer) {
  return destTeam < LV_PRES_TEAMS &&
         (destPlayer == 0xFF || destPlayer < MAX_TANKS);
}

/* The panel row a destination pair names, the way the server picks one: a
   player slot wins, else the team, with team 0 the everyone row. -1 for a
   pair out of range. */
static int lv_presPanelRow(BYTE destTeam, BYTE destPlayer) {
  if (!lv_presDestValid(destTeam, destPlayer)) {
    return -1;
  }
  if (destPlayer != 0xFF) {
    return LV_PRES_TEAMS + destPlayer;
  }
  return destTeam;
}

/* Read one of the four records' payloads, or a log_TeamSet's, from the
 * reader's position into *p.
 * Entered just past the event's code byte and its framed length; consumes
 * what the record's own lengths say, whatever they say, so the stream stays
 * aligned whatever the record held. The layouts are
 * docs/replay-format.md's. frameLen is the record's framed length, 0 for an
 * unframed one; only an announcement reads it, for its position bytes. */
static void lv_presReadPayload(BYTE code, unsigned frameLen,
                               LvPresPayload *p) {
  BYTE len = 0;

  memset(p->hdr, 0, sizeof(p->hdr));
  p->code   = code;
  p->len    = 0;
  p->whole  = FALSE;
  p->hasPos = FALSE;
  switch (code) {
  case log_ScnPanel: {
    /* panel id, destTeam, destPlayer, then the list's length as a
       big-endian u16 and that many bytes. */
    unsigned left;

    p->whole = logReadBytes(p->hdr, 5) == 5;
    p->len   = ((unsigned)p->hdr[3] << 8) | (unsigned)p->hdr[4];
    /* A bufferful at a time: what the length says is what has to come off
       the stream, whatever it says. Past SCN_PANEL_MAX the buffer holds only
       the last piece, and the check turns the length down. */
    left = p->len;
    while (left > 0) {
      unsigned want = (left > sizeof(p->data)) ? (unsigned)sizeof(p->data)
                                               : left;
      if (logReadBytes(p->data, (int)want) != (int)want) {
        p->whole = FALSE;
        break;
      }
      left -= want;
    }
    return;
  }
  case log_ScnScore:
    /* kind, target, the score as a big-endian int32, then the label as a
       pascal string. */
  case log_ScnStatus:
    /* destTeam, destPlayer, the countdown's end tick as a big-endian u32,
       then the line as a pascal string. */
    p->whole = logReadBytes(p->hdr, 6) == 6;
    break;
  case log_ScnAnnounce:
    /* destTeam, destPlayer, the ticks as a big-endian u16, then the line as
       a pascal string, then for a line with a position its two bytes,
       across and down. Only the framed length says whether they are there;
       they are read after the line, below. */
  case log_ScnMarker:
    /* id, kind, destTeam, destPlayer, then the placement as a pascal blob of
       x, y, slot and colour. */
    p->whole = logReadBytes(p->hdr, 4) == 4;
    break;
  case log_TeamSet:
    /* slot, then the team: two bytes and nothing after them. */
    p->whole = logReadBytes(p->hdr, 2) == 2;
    return;
  default:
    return;
  }
  p->whole = logReadBytes(&len, 1) == 1 && p->whole;
  if (len > 0) {
    p->whole = logReadBytes(p->data, len) == (int)len && p->whole;
  }
  p->len = len;
  /* The position bytes ride after the line when the frame holds two more
     bytes than the header, the length byte and the line. Anything past
     them is a later build's and is left for the framed length to skip. */
  if (code == log_ScnAnnounce && p->whole && len > 0 &&
      frameLen >= 4u + 1u + (unsigned)len + 2u) {
    p->hasPos = logReadBytes(p->pos, 2) == 2;
  }
}

/* The key a payload fills, or -1 when it fails a check. Playback and the
 * load walk both ask this, so a record the walk indexes is one playback
 * would have applied. A clear — an empty list, an empty line, the clear
 * kind of marker — is a record for its key like any other. */
static int lv_presCheck(const LvPresPayload *p) {
  const BYTE *h = p->hdr;
  int         row;

  if (!p->whole) {
    return -1;
  }
  switch (p->code) {
  case log_ScnPanel:
    /* Panel 0, the in-game square, is the only panel there is. The high
       four bits say which script of the round's list drew it
       (SCN_PANEL_WIRE_OWNER); this viewer keeps one list per audience and
       shows whichever script wrote to it last, as the recording played. */
    row = lv_presPanelRow(h[1], h[2]);
    if (SCN_PANEL_WIRE_ID(h[0]) != 0 || row < 0 || p->len > SCN_PANEL_MAX) {
      return -1;
    }
    if (scnPanelParse(p->data, (uint16_t)p->len, &s_presPanelScratch) !=
        SCN_PANEL_OK) {
      return -1;
    }
    return LV_PRES_KEY_PANEL + row;
  case log_ScnScore:
    if (p->len > LV_PRES_LABEL_LEN - 1) {
      return -1;
    }
    if (h[0] == SCN_SCORE_KIND_PLAYER && h[1] < MAX_TANKS) {
      return LV_PRES_KEY_PLAYER_SCORE + h[1];
    }
    if (h[0] == SCN_SCORE_KIND_TEAM && h[1] >= 1 && h[1] < LV_PRES_TEAMS) {
      return LV_PRES_KEY_TEAM_SCORE + h[1] - 1;
    }
    return -1;
  case log_ScnAnnounce:
    if (!lv_presDestValid(h[0], h[1]) || p->len > LV_PRES_ANNOUNCE_MAX) {
      return -1;
    }
    return LV_PRES_KEY_ANNOUNCE;
  case log_ScnMarker:
    /* The clear kind reads none of the placement but is held to the same
       checks, since the server writes all four bytes whatever the kind. */
    if (h[0] >= SCN_MARKERS_MAX || h[1] > SCN_MARKER_KIND_CLEAR ||
        p->len != 4 || !lv_presDestValid(h[2], h[3]) ||
        p->data[3] >= SCN_PANEL_COLOURS) {
      return -1;
    }
    if (h[1] == SCN_MARKER_KIND_FOLLOW && p->data[2] >= MAX_TANKS) {
      return -1;
    }
    return LV_PRES_KEY_MARKER + h[0];
  case log_TeamSet:
    /* A slot, and a team from 0 (none) to 15. */
    if (h[0] >= MAX_TANKS || h[1] >= LV_PRES_TEAMS) {
      return -1;
    }
    return LV_PRES_KEY_SLOT_TEAM + h[0];
  case log_ScnStatus:
    row = lv_presPanelRow(h[0], h[1]);
    if (row < 0 || p->len > LV_PRES_ANNOUNCE_MAX) {
      return -1;
    }
    return LV_PRES_KEY_STATUS + row;
  default:
    return -1;
  }
}

/* Store a payload lv_presCheck gave key for, as at time ms. Makes no checks
 * of its own: every payload that reaches it has passed them, either just now
 * or when the load walk indexed it. */
static void lv_presApply(const LvPresPayload *p, int key, uint32_t ms) {
  const BYTE *h = p->hdr;

  switch (p->code) {
  case log_ScnPanel: {
    LvPresPanelRow *row = &g_lv->pres.panels[key - LV_PRES_KEY_PANEL];
    /* A clear is a write too: it replaces whatever an older row showed. */
    row->written = TRUE;
    row->ms      = ms;
    if (p->len == 0) {
      row->set = FALSE;
      row->len = 0;
      return;
    }
    row->set = TRUE;
    row->len = (uint16_t)p->len;
    memcpy(row->bytes, p->data, p->len);
    return;
  }
  case log_ScnScore: {
    LvPresScore *row = (key < LV_PRES_KEY_TEAM_SCORE)
        ? &g_lv->pres.playerScores[key - LV_PRES_KEY_PLAYER_SCORE]
        : &g_lv->pres.teamScores[key - LV_PRES_KEY_TEAM_SCORE + 1];
    memset(row, 0, sizeof(*row));
    row->valid = TRUE;
    row->score = (int32_t)(((uint32_t)h[2] << 24) | ((uint32_t)h[3] << 16) |
                           ((uint32_t)h[4] << 8)  | (uint32_t)h[5]);
    memcpy(row->label, p->data, p->len);
    return;
  }
  case log_ScnAnnounce: {
    /* A text length of 0 is the clear, and its ticks are not read. */
    LvPresAnnounce *a = &g_lv->pres.announce;
    memset(a, 0, sizeof(*a));
    if (p->len == 0) {
      return;
    }
    a->set        = TRUE;
    a->ms         = ms;
    a->ticks      = (uint16_t)(((unsigned)h[2] << 8) | h[3]);
    a->destTeam   = h[0];
    a->destPlayer = h[1];
    if (p->hasPos) {
      /* A byte past the max reads as the max, as the game reads one. */
      a->hasPos = TRUE;
      a->posX   = (p->pos[0] > SCN_ANNOUNCE_POS_MAX)
                      ? (BYTE)SCN_ANNOUNCE_POS_MAX : p->pos[0];
      a->posY   = (p->pos[1] > SCN_ANNOUNCE_POS_MAX)
                      ? (BYTE)SCN_ANNOUNCE_POS_MAX : p->pos[1];
    }
    memcpy(a->text, p->data, p->len);
    return;
  }
  case log_ScnMarker: {
    LvPresMarker *m = &g_lv->pres.markers[key - LV_PRES_KEY_MARKER];
    memset(m, 0, sizeof(*m));
    if (h[1] == SCN_MARKER_KIND_CLEAR) {
      return;
    }
    m->set        = TRUE;
    m->kind       = h[1];
    m->destTeam   = h[2];
    m->destPlayer = h[3];
    m->x          = p->data[0];
    m->y          = p->data[1];
    m->slot       = p->data[2];
    m->colour     = p->data[3];
    return;
  }
  case log_TeamSet: {
    BYTE slot = (BYTE)(key - LV_PRES_KEY_SLOT_TEAM);
    g_lv->pres.team[slot]      = h[1];
    g_lv->pres.teamKnown[slot] = TRUE;
    return;
  }
  case log_ScnStatus: {
    /* A clear is a write too, as a panel row's is: it replaces whatever an
       older row showed. */
    LvPresStatus *s = &g_lv->pres.status[key - LV_PRES_KEY_STATUS];
    memset(s, 0, sizeof(*s));
    s->written = TRUE;
    s->ms      = ms;
    s->endsAt  = SCN_STATUS_NO_COUNTDOWN;
    if (p->len == 0) {
      return;
    }
    s->set    = TRUE;
    s->endsAt = ((uint32_t)h[2] << 24) | ((uint32_t)h[3] << 16) |
                ((uint32_t)h[4] << 8)  | (uint32_t)h[5];
    memcpy(s->text, p->data, p->len);
    return;
  }
  default:
    return;
  }
}

/* Read one of the four records from the reader's position and store what it
 * says, as at time ms, if it passes its checks. What playback runs. Answers
 * the key it stored under, or -1 for a record that failed; s_presPayload
 * still holds what was read. */
static int lv_presReadRecord(BYTE code, unsigned frameLen, uint32_t ms) {
  int key;

  lv_presReadPayload(code, frameLen, &s_presPayload);
  key = lv_presCheck(&s_presPayload);
  if (key >= 0) {
    lv_presApply(&s_presPayload, key, ms);
  }
  return key;
}

/* Post an announcement playback has just stored to the newswire, labelled
 * with who it went to when that was not everyone. The viewer watches every
 * player at once, so every announcement is posted whatever its destination.
 * The message step only: a rebuild stores announcements without it, so a
 * seek never posts one twice. A clear posts nothing.
 *
 * The line is the script's own bytes, up to LV_PRES_ANNOUNCE_MAX of them,
 * split across the three string arguments since each holds 63. */
static void lv_presPostAnnounce(const LvPresPayload *p) {
  const BYTE *h = p->hdr;
  char        text[LV_PRES_ANNOUNCE_MAX + 1];
  size_t      len;
  size_t      part = LANG_MSGARG_STRING_LEN - 1;
  MessageArgs args = {0};
  langid      body = STR_LV_SCN_ANNOUNCE;

  if (p->code != log_ScnAnnounce || p->len == 0) {
    return;
  }
  len = (p->len > LV_PRES_ANNOUNCE_MAX) ? LV_PRES_ANNOUNCE_MAX : p->len;
  memcpy(text, p->data, len);
  text[len] = '\0';
  snprintf(args.string1, sizeof(args.string1), "%.*s",
           (int)(len < part ? len : part), text);
  if (len > part) {
    size_t rest = len - part;
    snprintf(args.string2, sizeof(args.string2), "%.*s",
             (int)(rest < part ? rest : part), text + part);
  }
  if (len > 2 * part) {
    snprintf(args.string3, sizeof(args.string3), "%s", text + 2 * part);
  }

  /* A slot wins over a team, the way the panel rows are picked. */
  if (h[1] != 0xFF) {
    char name[PLAYER_NAME_LEN];
    name[0] = '\0';
    if (lv_playersIsInUse(h[1])) {
      lv_playersGetPlayerName(h[1], name, sizeof(name));
    } else if (!lv_screenGetLoggedPlayerName(h[1], name, sizeof(name))) {
      MessageArgs slotArgs = {0};
      slotArgs.number = h[1];
      snprintf(name, sizeof(name), "%s",
               langGetTextFmt(STR_LV_INFO_SCORE_SLOT, &slotArgs));
    }
    snprintf(args.playerName, sizeof(args.playerName), "%s", name);
    body = STR_LV_SCN_ANNOUNCE_TO;
  } else if (h[0] != 0) {
    MessageArgs teamArgs = {0};
    teamArgs.number = h[0];
    snprintf(args.playerName, sizeof(args.playerName), "%s",
             langGetTextFmt(STR_DLGLOBBY_TEAM_HEADER, &teamArgs));
    body = STR_LV_SCN_ANNOUNCE_TO;
  }
  lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, body, &args);
}

/* Store the team a log_TeamSet playback has already read, as at time ms, if
 * it passes the checks the walk makes, and answer the key it stored under or
 * -1. The record's bytes are read and its newswire line posted by playback's
 * own case; this is the store step only, the same one a rebuild takes. */
static int lv_presStoreTeam(BYTE slot, BYTE team, uint32_t ms) {
  int key;

  memset(s_presPayload.hdr, 0, sizeof(s_presPayload.hdr));
  s_presPayload.code   = log_TeamSet;
  s_presPayload.whole  = TRUE;
  s_presPayload.len    = 0;
  s_presPayload.hdr[0] = slot;
  s_presPayload.hdr[1] = team;
  key = lv_presCheck(&s_presPayload);
  if (key >= 0) {
    lv_presApply(&s_presPayload, key, ms);
  }
  return key;
}

bool lv_screenGetSlotTeam(BYTE slot, BYTE *team) {
  if (g_lv == NULL || slot >= MAX_TANKS || !g_lv->pres.teamKnown[slot]) {
    return FALSE;
  }
  if (team != NULL) {
    *team = g_lv->pres.team[slot];
  }
  return TRUE;
}

BYTE lv_screenFollowedSlot(void) {
  if (g_lv == NULL) {
    return NEUTRAL;
  }
  return g_lv->gameView ? g_lv->cameraSlot : lv_playersGetSelf();
}

const LvPresPanelRow *lv_screenChoosePanelRow(const LvPresPanelRow *everyone,
                                              const LvPresPanelRow *team,
                                              const LvPresPanelRow *slot) {
  /* In the order a tie goes: a later candidate wins only on a greater ms. */
  const LvPresPanelRow *order[3];
  const LvPresPanelRow *best = NULL;
  int                   i;

  order[0] = slot;
  order[1] = team;
  order[2] = everyone;
  for (i = 0; i < 3; i++) {
    const LvPresPanelRow *row = order[i];
    if (row == NULL || !row->written) {
      continue;
    }
    if (best == NULL || row->ms > best->ms) {
      best = row;
    }
  }
  return (best != NULL && best->set) ? best : NULL;
}

const LvPresPanelRow *lv_screenFollowedPanelRow(void) {
  const LvPresPanelRow *teamRow = NULL;
  const LvPresPanelRow *slotRow = NULL;
  BYTE                  slot;
  BYTE                  team;

  if (g_lv == NULL) {
    return NULL;
  }
  slot = lv_screenFollowedSlot();
  if (slot < MAX_TANKS) {
    slotRow = lv_screenGetPanelRow(0, slot);
    /* Team 0 is no team, whose row is the everyone row. */
    if (lv_screenGetSlotTeam(slot, &team) && team != 0) {
      teamRow = lv_screenGetPanelRow(team, 0xFF);
    }
  }
  return lv_screenChoosePanelRow(lv_screenGetPanelRow(0, 0xFF), teamRow,
                                 slotRow);
}

bool lv_screenMarkerVisible(const LvPresMarker *m, BYTE followedSlot) {
  BYTE team;

  if (m == NULL || !m->set) {
    return FALSE;
  }
  /* A slot wins over a team, the way the panel rows are picked. */
  if (m->destPlayer != 0xFF) {
    return followedSlot < MAX_TANKS && m->destPlayer == followedSlot;
  }
  if (m->destTeam != 0) {
    return followedSlot < MAX_TANKS &&
           lv_screenGetSlotTeam(followedSlot, &team) && team == m->destTeam;
  }
  return TRUE;
}

bool lv_screenMarkerPlace(BYTE id, BYTE *mx, BYTE *my, BYTE *colour) {
  const LvPresMarker *m = lv_screenGetMarker(id);
  BYTE x, y;

  if (m == NULL || !lv_screenMarkerVisible(m, lv_screenFollowedSlot())) {
    return FALSE;
  }
  if (m->kind == SCN_MARKER_KIND_FOLLOW) {
    /* The square of the tank the viewer draws for that slot: the same test
       lv_playersMakeScreenTanks makes, so a marker rides a tank that is on
       the map and nothing else. */
    BYTE px, py, frame;
    bool onBoat;

    if (m->slot >= MAX_TANKS || !lv_playersIsInUse(m->slot)) {
      return FALSE;
    }
    lv_playersGetTankDetails(m->slot, &x, &y, &px, &py, &frame, &onBoat);
    if (x == 0 && y == 0 && px == 0 && py == 0) {
      return FALSE;
    }
  } else {
    x = m->x;
    y = m->y;
  }
  if (mx != NULL) *mx = x;
  if (my != NULL) *my = y;
  if (colour != NULL) *colour = m->colour;
  return TRUE;
}

const LvPresPanelRow *lv_screenGetPanelRow(BYTE destTeam, BYTE destPlayer) {
  int index;

  if (g_lv == NULL) {
    return NULL;
  }
  index = lv_presPanelRow(destTeam, destPlayer);
  return (index < 0) ? NULL : &g_lv->pres.panels[index];
}

const LvPresScore *lv_screenGetScore(BYTE kind, BYTE target) {
  if (g_lv == NULL) {
    return NULL;
  }
  if (kind == SCN_SCORE_KIND_PLAYER && target < MAX_TANKS) {
    return &g_lv->pres.playerScores[target];
  }
  if (kind == SCN_SCORE_KIND_TEAM && target >= 1 && target < LV_PRES_TEAMS) {
    return &g_lv->pres.teamScores[target];
  }
  return NULL;
}

const LvPresStatus *lv_screenGetStatusRow(BYTE destTeam, BYTE destPlayer) {
  int index;

  if (g_lv == NULL) {
    return NULL;
  }
  index = lv_presPanelRow(destTeam, destPlayer);
  return (index < 0) ? NULL : &g_lv->pres.status[index];
}

const LvPresStatus *lv_screenFollowedStatus(void) {
  const LvPresStatus *order[3];
  const LvPresStatus *best = NULL;
  BYTE                slot;
  BYTE                team;
  int                 i;

  if (g_lv == NULL) {
    return NULL;
  }
  /* The slot's row, the team's, then everyone's: the order a tie goes, so a
     later candidate wins only on a greater ms, as lv_screenChoosePanelRow
     picks a panel row. */
  order[0] = NULL;
  order[1] = NULL;
  order[2] = lv_screenGetStatusRow(0, 0xFF);
  slot = lv_screenFollowedSlot();
  if (slot < MAX_TANKS) {
    order[0] = lv_screenGetStatusRow(0, slot);
    /* Team 0 is no team, whose row is the everyone row. */
    if (lv_screenGetSlotTeam(slot, &team) && team != 0) {
      order[1] = lv_screenGetStatusRow(team, 0xFF);
    }
  }
  for (i = 0; i < 3; i++) {
    const LvPresStatus *row = order[i];
    if (row == NULL || !row->written) {
      continue;
    }
    if (best == NULL || row->ms > best->ms) {
      best = row;
    }
  }
  return (best != NULL && best->set) ? best : NULL;
}

const LvPresAnnounce *lv_screenGetAnnounce(void) {
  return (g_lv == NULL) ? NULL : &g_lv->pres.announce;
}

const LvPresMarker *lv_screenGetMarker(BYTE id) {
  if (g_lv == NULL || id >= SCN_MARKERS_MAX) {
    return NULL;
  }
  return &g_lv->pres.markers[id];
}

/* --- Server tick ------------------------------------------------------
 * A scenario counts time in the server's game tick, a hundred a second and
 * reset each round. The recording states that tick in log_ServerTick records,
 * one at the round's first entry and one at every entry that carries a
 * snapshot. Each is kept as an anchor: the playback time it landed at, the
 * writer ticks counted up to it and the server tick it carries.
 * lv_screenServerTickAt answers any other time from the last anchor at or
 * before it.
 *
 * Playback time is not a count of the server's log entries, so the answer
 * cannot come from the ms alone. The server writes one entry every other game
 * tick — one writer tick — and the decoder spends 20 ms on every record it
 * reads, so a LOG_NOEVENTS run, a snapshot and the entity-mask block each
 * cost playback 20 ms more than the writer ticks behind them. What is counted
 * instead is writer ticks, kept as spans: from span.ms the count is span.wt,
 * rising by one every 20 ms for span.run steps. The server tick at ms is the
 * anchor's tick + 2 × (writer ticks at ms − writer ticks at the anchor).
 *
 * How a loaded file's records count, by what log.c writes:
 *   LOG_NOEVENTS n / _LONG   n, one on each of the n waiting steps
 *   LOG_EVENT / _LONG        1, the block logWriteTick wrote for its tick
 *   LOG_SNAPSHOT             0
 *   the entity-mask block    0: an event block holding log_EntityMasks,
 *                            which only logWriteEntityMasks writes, straight
 *                            after a snapshot
 *   the snapshot's flush     0: logWriteSnapshot writes the events its tick
 *                            has queued so far as a block of their own ahead
 *                            of the snapshot, and the tick itself is counted
 *                            later, by logWriteTick
 *
 * The flush is the one block the bytes cannot name. It is the block straight
 * before a snapshot, but so is the previous tick's own block when the
 * snapshot's tick had queued nothing, and the two are written alike. It is
 * counted as a flush, the usual case in a round where anything moves, and the
 * anchor after the snapshot settles it: two anchors say how many writer ticks
 * passed between them, and a count one short means the block was the previous
 * tick's own, so the tick is put back from that block on. A block holding
 * log_ServerTick is never a flush, since the record is queued after its
 * tick's snapshot.
 *
 * A live feed has no walk. Its records come from the spectator ring, one per
 * writer tick — an event block, a LOG_NOEVENTS 1 or a keyframe snapshot — so
 * each counts one, and playback adds them the first time it reads them. A
 * seek back replays records the spans already cover and adds nothing, and
 * every snapshot a seek can restore from was read on the way in, so the spans
 * cover everything behind the playhead and stay exact.
 *
 * Every field comes from a file anyone could have written: a record of the
 * wrong length is consumed and ignored. */

/* Playback time per record the decoder reads. */
#define LV_WT_STEP_MS               20u

/* Most anchors kept. One per written snapshot, which is every 250 game ticks
   or 2.5 s, so this is over five and a half hours of round. */
#define LV_SERVER_TICK_ANCHORS_MAX  8192

/* Most spans kept. A round where something moves every tick needs about
   three per snapshot; the most a file can ask for is one per LOG_NOEVENTS,
   which with events and empty ticks alternating is one per two writer ticks,
   so this is nearly three hours of round at worst. Twelve bytes each. */
#define LV_WRITER_TICK_SPANS_MAX    262144

typedef struct {
  uint32_t ms;    /* playback time the span starts at */
  uint32_t wt;    /* writer ticks counted at ms */
  uint32_t run;   /* steps of one per 20 ms after ms */
} LvWtSpan;

typedef struct {
  uint32_t ms;    /* playback time the record landed at */
  uint32_t wt;    /* writer ticks counted at ms */
  uint32_t tick;  /* the server tick it carries */
} LvTickAnchor;

static LvWtSpan     *s_wtSpans            = NULL;
static int           s_wtSpanCount        = 0;
static int           s_wtSpanCap          = 0;
static bool          s_wtTruncated        = FALSE;
/* Whether any record has been counted, and the time of the last one. Playback
   on a live feed counts only past it. */
static bool          s_wtAny              = FALSE;
static uint32_t      s_wtLastMs           = 0;
/* Set once the load walk has filled the spans and anchors, so playback adds
   nothing to either. A live feed has no walk. */
static bool          s_wtWalked           = FALSE;
/* The last record was an event block that a snapshot straight after it would
   make a flush: the spans as they stood before it, to put back. */
static bool          s_wtUndoValid        = FALSE;
static int           s_wtUndoCount        = 0;
static LvWtSpan      s_wtUndoLast;
static uint32_t      s_wtUndoMs           = 0;
/* The flushes counted since the last anchor, and the span of the first. */
static int           s_wtFlushCount       = 0;
static int           s_wtFlushSpan        = -1;

static LvTickAnchor *s_tickAnchors        = NULL;
static int           s_tickAnchorCount    = 0;
static int           s_tickAnchorCap      = 0;
static bool          s_tickAnchorsTruncated = FALSE;

/* The anchor list could not take an anchor. Says so once per recording:
 * past the last anchor it holds, the tick is counted on from that one. */
static void lv_tickAnchorsTruncate(void) {
  if (!s_tickAnchorsTruncated) {
    WB_LOG_WARN(WB_LOG_CAT_LOGVIEWER,
                "server tick anchors are full at %d; later ticks are counted "
                "from the last of them", s_tickAnchorCount);
  }
  s_tickAnchorsTruncated = TRUE;
}

/* Drop the spans and the anchors, as every load and close does. */
static void lv_serverTickReset(void) {
  free(s_wtSpans);
  s_wtSpans              = NULL;
  s_wtSpanCount          = 0;
  s_wtSpanCap            = 0;
  s_wtTruncated          = FALSE;
  s_wtAny                = FALSE;
  s_wtLastMs             = 0;
  s_wtWalked             = FALSE;
  s_wtUndoValid          = FALSE;
  s_wtUndoCount          = 0;
  s_wtUndoMs             = 0;
  s_wtFlushCount         = 0;
  s_wtFlushSpan          = -1;
  free(s_tickAnchors);
  s_tickAnchors          = NULL;
  s_tickAnchorCount      = 0;
  s_tickAnchorCap        = 0;
  s_tickAnchorsTruncated = FALSE;
}

/* Writer ticks at ms by one span. */
static uint32_t lv_wtSpanAt(const LvWtSpan *s, uint32_t ms) {
  uint32_t steps;

  if (ms <= s->ms) {
    return s->wt;
  }
  steps = (ms - s->ms) / LV_WT_STEP_MS;
  if (steps > s->run) {
    steps = s->run;
  }
  return s->wt + steps;
}

/* Writer ticks counted by playback time ms: 0 before the first span. Past the
   end of a list that ran out of room, the count goes on at one per 20 ms, the
   rate playback time alone would give. */
static uint32_t lv_wtAt(uint32_t ms) {
  int             lo    = 0;
  int             hi    = s_wtSpanCount - 1;
  int             found = -1;
  const LvWtSpan *s;

  while (lo <= hi) {
    int mid = lo + (hi - lo) / 2;
    if (s_wtSpans[mid].ms <= ms) {
      found = mid;
      lo    = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  if (found < 0) {
    return 0;
  }
  s = &s_wtSpans[found];
  if (s_wtTruncated && found == s_wtSpanCount - 1) {
    uint32_t end = s->ms + s->run * LV_WT_STEP_MS;
    if (ms > end) {
      return s->wt + s->run + (ms - end) / LV_WT_STEP_MS;
    }
  }
  return lv_wtSpanAt(s, ms);
}

/* Add a span, growing the list as needed. A full list, or one that cannot
   grow, keeps what it has and sets the truncated flag. */
static bool lv_wtPush(uint32_t ms, uint32_t wt, uint32_t run) {
  if (s_wtTruncated) {
    return FALSE;
  }
  if (s_wtSpanCount >= s_wtSpanCap) {
    int       cap = (s_wtSpanCap == 0) ? 256 : s_wtSpanCap * 2;
    LvWtSpan *grown;

    if (cap > LV_WRITER_TICK_SPANS_MAX) {
      cap = LV_WRITER_TICK_SPANS_MAX;
    }
    if (cap <= s_wtSpanCount) {
      s_wtTruncated = TRUE;
      return FALSE;
    }
    grown = (LvWtSpan *)realloc(s_wtSpans, (size_t)cap * sizeof(*grown));
    if (grown == NULL) {
      s_wtTruncated = TRUE;
      return FALSE;
    }
    s_wtSpans   = grown;
    s_wtSpanCap = cap;
  }
  s_wtSpans[s_wtSpanCount].ms  = ms;
  s_wtSpans[s_wtSpanCount].wt  = wt;
  s_wtSpans[s_wtSpanCount].run = run;
  s_wtSpanCount++;
  return TRUE;
}

/* A LOG_NOEVENTS run of n read at ms: no tick on the reading step, then one
   on each of the n waiting steps. */
static void lv_wtCountWait(uint32_t ms, uint32_t n) {
  s_wtUndoValid = FALSE;
  lv_wtPush(ms, lv_wtAt(ms), n);
  s_wtAny    = TRUE;
  s_wtLastMs = ms;
}

/* One writer tick at ms. mayBeFlush keeps what is needed to take it back if
   a snapshot is the next record. A tick 20 ms after the end of the last span
   carries that span on. */
static void lv_wtCountTick(uint32_t ms, bool mayBeFlush) {
  LvWtSpan *last = (s_wtSpanCount > 0) ? &s_wtSpans[s_wtSpanCount - 1] : NULL;
  uint32_t  before = lv_wtAt(ms);

  s_wtUndoValid = mayBeFlush;
  if (mayBeFlush) {
    s_wtUndoCount = s_wtSpanCount;
    if (last != NULL) {
      s_wtUndoLast = *last;
    }
    s_wtUndoMs = ms;
  }
  if (last != NULL && ms == last->ms + (last->run + 1u) * LV_WT_STEP_MS) {
    last->run++;
  } else {
    lv_wtPush(ms, before + 1u, 0);
  }
  s_wtAny    = TRUE;
  s_wtLastMs = ms;
}

/* A snapshot in a loaded file. It counts nothing, and the event block
   straight before it, if there was one that could be, becomes the
   snapshot's flush: its tick is taken back and a flat span marks where it
   stood, for the next anchor to put the tick back from if it was not. */
static void lv_wtCountFileSnapshot(void) {
  if (s_wtUndoValid) {
    s_wtSpanCount = s_wtUndoCount;
    if (s_wtSpanCount > 0) {
      s_wtSpans[s_wtSpanCount - 1] = s_wtUndoLast;
    }
    if (lv_wtPush(s_wtUndoMs, lv_wtAt(s_wtUndoMs), 0)) {
      if (s_wtFlushCount == 0) {
        s_wtFlushSpan = s_wtSpanCount - 1;
      }
      s_wtFlushCount++;
    }
  }
  s_wtUndoValid = FALSE;
}

/* Add an anchor at ms. One per time: a second record at a time already held,
   or one at or before the last anchor, which is what a seek back on a live
   feed replays, adds nothing. settleFlush lets the loaded file's walk put a
   flush's tick back, as the section comment above describes. A full list
   keeps what it has and sets the truncated flag. */
static void lv_tickAnchorAdd(uint32_t ms, uint32_t tick, bool settleFlush) {
  LvTickAnchor *prev = (s_tickAnchorCount > 0)
                           ? &s_tickAnchors[s_tickAnchorCount - 1] : NULL;
  uint32_t      wt;

  if (prev != NULL && prev->ms >= ms) {
    return;
  }
  wt = lv_wtAt(ms);
  /* Only within one round's run of ticks, and only when there is exactly one
     flush to blame: the writer puts an anchor after every snapshot, so a
     well-formed file never has two between anchors. */
  if (settleFlush && prev != NULL && tick >= prev->tick && wt >= prev->wt &&
      s_wtFlushCount == 1 && s_wtFlushSpan >= 0 &&
      s_wtFlushSpan < s_wtSpanCount &&
      (wt - prev->wt) + 1u == (tick - prev->tick) / 2u) {
    int i;
    for (i = s_wtFlushSpan; i < s_wtSpanCount; i++) {
      s_wtSpans[i].wt++;
    }
    wt++;
  }
  s_wtFlushCount = 0;
  s_wtFlushSpan  = -1;

  if (s_tickAnchorCount >= s_tickAnchorCap) {
    int           cap = (s_tickAnchorCap == 0) ? 64 : s_tickAnchorCap * 2;
    LvTickAnchor *grown;

    if (cap > LV_SERVER_TICK_ANCHORS_MAX) {
      cap = LV_SERVER_TICK_ANCHORS_MAX;
    }
    if (cap <= s_tickAnchorCount) {
      lv_tickAnchorsTruncate();
      return;
    }
    grown = (LvTickAnchor *)realloc(s_tickAnchors,
                                    (size_t)cap * sizeof(*grown));
    if (grown == NULL) {
      lv_tickAnchorsTruncate();
      return;
    }
    s_tickAnchors   = grown;
    s_tickAnchorCap = cap;
  }
  s_tickAnchors[s_tickAnchorCount].ms   = ms;
  s_tickAnchors[s_tickAnchorCount].wt   = wt;
  s_tickAnchors[s_tickAnchorCount].tick = tick;
  s_tickAnchorCount++;
}

/* Whether playback on a live feed should count the record it has just read:
   no walk ran, and the record lies past everything counted so far. */
static bool lv_wtLiveCounts(void) {
  return !s_wtWalked && (!s_wtAny || g_lv->timeRunning > s_wtLastMs);
}

uint32_t lv_screenServerTickAt(uint32_t ms) {
  int                 lo    = 0;
  int                 hi    = s_tickAnchorCount - 1;
  int                 found = -1;
  const LvTickAnchor *a;
  uint32_t            wt;
  uint32_t            back;

  if (s_tickAnchorCount == 0) {
    /* No anchors: a recording made before log_ServerTick existed, or a live
       feed that has not reached its first one. Two ticks per 20 ms of
       playback, which is the server's clock only as far as playback time
       and the log's entries agree. */
    return ms / 10u;
  }
  while (lo <= hi) {
    int mid = lo + (hi - lo) / 2;
    if (s_tickAnchors[mid].ms <= ms) {
      found = mid;
      lo    = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  wt = lv_wtAt(ms);
  if (found >= 0) {
    a = &s_tickAnchors[found];
    return (wt >= a->wt) ? a->tick + 2u * (wt - a->wt) : a->tick;
  }
  /* Before the first anchor: count back from it, and not below 0. */
  a    = &s_tickAnchors[0];
  back = (a->wt > wt) ? 2u * (a->wt - wt) : 0u;
  return (a->tick > back) ? a->tick - back : 0u;
}

bool lv_screenHasServerTick(void) {
  return s_tickAnchorCount > 0;
}


void lv_screenProcessLog(unsigned short numEvents) {
  unsigned short count = 0;
  BYTE code;
  BYTE opt1, opt2, opt3, opt4, opt5, px, py, frame, onBoat;
  char mem[4096 + 1024]; /* Extra space for snprintf format overhead with names */
  char str[4096];
  char name[256];
  char name2[256];

  while (count < numEvents) {
    /* Framing, not a single version: v2 brought it in and every version
       since keeps it. */
    bool isV2 = (g_lv->loadedLogVersion >= LOG_VERSION_V2);
    unsigned short evLen = 0; /* framed payload length after code */
    size_t payloadStart = 0;  /* where the framed payload begins */

    /* The count is off the recording and can name more events than the log
       holds. Once a read comes back short there are none left, so stop
       rather than count down the rest with nothing to read. */
    if (logReadBytes(&code, 1) != 1) {
      break;
    }

    if (isV2) {
      /* v2 frames every event as [type][u16 BE payload-length][payload].
         The cases below read their own fields; the cursor is then put at
         the end of the frame, however many bytes a case took. */
      BYTE lenBytes[2];
      if (logReadBytes(lenBytes, 2) != 2) {
        break;
      }
      evLen = (unsigned short)((lenBytes[0] << 8) | lenBytes[1]);
      payloadStart = lv_logGetCurrentPosition();
    }

    switch (code) {
    case log_PlayerJoined:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      logReadBytes(&opt4, 1);
      logReadBytes(&opt5, 1);
      logReadBytes((BYTE *)mem, 1);
      logReadBytes((BYTE *)mem+1, (unsigned char)mem[0]);
      lv_utilPtoCString(mem, name);
      {
        BYTE accountFlags = 0;
        if (g_lv->loadedLogVersion == LOG_VERSION_V0) {
          /* Version 0: opt2-opt5 are IP address octets */
          snprintf(mem, sizeof(mem), "%d.%d.%d.%d", opt2, opt3, opt4, opt5);
          lv_dnsLookup(mem, str, sizeof(str));
          snprintf(mem, sizeof(mem), "%s", str);
        } else if (g_lv->loadedLogVersion >= LOG_VERSION_V1) {
          /* Version 1 and later: opt2-opt3 are 2-char country code,
           * opt4 is accountFlags (bit 0=WBN, bit 1=Steam, bit 5=bot),
           * opt5 reserved (zero in current writers). */
          snprintf(mem, sizeof(mem), "[%c%c]", opt2, opt3);
          accountFlags = opt4;
        }
        lv_playersSetPlayer(opt1, name, mem, 0, 0, 0, 0, 0, FALSE, 0, NULL, TRUE, FALSE, accountFlags);
      }
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, name);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, STR_LV_PLAYER_JOINED, &args);
      }
      if (opt1 < MAX_TANKS) {
        g_lv->gameViewHud[opt1].alive = true;
        g_lv->gameViewHud[opt1].respawnTimeMs = g_lv->timeRunning;
      }
      break;
    case log_PlayerQuit:
      logReadBytes(&opt1, 1);
      lv_playersGetPlayerName(opt1, str, sizeof(str));
      lv_playersLeaveGame(opt1, TRUE);
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, str);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, MESSAGE_QUIT_GAME, &args);
      }
      if (opt1 < MAX_TANKS) {
        g_lv->gameViewHud[opt1].alive = false;
        if (g_lv->gameView && opt1 == g_lv->cameraSlot) {
          /* Advance camera to next in-use slot. Mirrors Tab cycle in
           * logviewer.c:651-660. The `next != opt1` guard skips the
           * leaving player explicitly: lv_playersIsInUse(opt1) may still
           * return TRUE here, and we don't want the camera to bounce
           * back. */
          BYTE start = g_lv->cameraSlot;
          BYTE found = start;
          BYTE i;
          for (i = 1; i <= MAX_TANKS; i++) {
            BYTE next = (BYTE)((start + i) % MAX_TANKS);
            if (next != opt1 && lv_playersIsInUse(next)) {
              found = next;
              break;
            }
          }
          g_lv->cameraSlot = found;
          g_lv->wantScreenUpdate = TRUE;
        }
      }
      break;
    case log_LostMan:
      logReadBytes(&opt1, 1);
      lv_playersSetLgmDead(opt1);
      lv_playersGetPlayerName(opt1, str, sizeof(str));
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, str);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, MESSAGE_LGM_DEAD, &args);
      }
      break;
    case log_MapChange:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      lv_mapSetPos(&g_lv->mp, opt1, opt2, opt3);
      break;
    case log_ChangeName:
      logReadBytes(&opt1, 1);
      logReadBytes((BYTE *)mem, 1);
      logReadBytes((BYTE *)mem+1, (unsigned char)mem[0]);
      lv_utilPtoCString(mem, str);
      lv_playersSetPlayerName(opt1, str);
      break;
    case log_AllyRequest:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      lv_playersGetPlayerName(opt1, str, sizeof(str));
      lv_playersGetPlayerName(opt2, mem, sizeof(mem));
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, str);
        snprintf(args.otherName, sizeof(args.otherName), "%.*s", (int)sizeof(args.otherName) - 1, mem);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, STR_LV_ALLY_REQUEST, &args);
      }
      break;
    case log_AllyAccept:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      lv_playersAcceptAlliance(opt1, opt2);
      lv_playersGetPlayerName(opt1, str, sizeof(str));
      lv_playersGetPlayerName(opt2, mem, sizeof(mem));
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, str);
        snprintf(args.otherName, sizeof(args.otherName), "%.*s", (int)sizeof(args.otherName) - 1, mem);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, STR_LV_ALLY_ACCEPT, &args);
      }
      break;
    case log_AllyLeave:
      logReadBytes(&opt1, 1);
      lv_playersLeaveAlliance(opt1);
      lv_playersGetPlayerName(opt1, str, sizeof(str));
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, str);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, STR_LV_ALLY_LEAVE, &args);
      }
      break;
    case log_SoundBuild:
    case log_SoundFarm:
    case log_SoundShoot:
    case log_SoundHitTank:
    case log_SoundHitTree:
    case log_SoundHitWall:
    case log_SoundMineLay:
    case log_SoundMineExplode:
    case log_SoundExplosion:
    case log_SoundBigExplosion:
    case log_SoundManDie:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);

      if (code == log_SoundBuild) {
        opt3 = manBuildingNear;
      } else if (code == log_SoundFarm) {
        opt3 = farmingTreeNear;
      } else if (code == log_SoundShoot) {
        opt3 = shootSelf;
      } else if (code == log_SoundHitTank) {
        opt3 = hitTankNear;
      } else if (code == log_SoundHitWall) {
        opt3 = shotBuildingNear;
      } else if (code == log_SoundMineLay) {
        opt3 = manLayingMineNear;
      } else if (code ==log_SoundMineExplode) {
        opt3 = mineExplosionNear;
      } else if (code == log_SoundExplosion) {
        opt3 = mineExplosionNear;
      } else if (code == log_SoundBigExplosion) {
        opt3 = bigExplosionNear;
      } else if (code == log_SoundHitTree) {
        opt3 = shotTreeNear;
      } else {
         opt3 = manDyingNear;
      }
      lv_soundDist(opt3, opt1, opt2);
      break;
    case log_PlayerLocation:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      logReadBytes(&opt4, 1);
      logReadBytes(&opt5, 1);
      lv_utilGetNibbles(opt4, &px, &py);
      lv_utilGetNibbles(opt5, &frame, &onBoat);
      if (opt2 != 0) {
        lv_playersUpdateTank(opt1, opt2, opt3, px, py, frame, onBoat);
        if (opt1 < MAX_TANKS && !g_lv->gameViewHud[opt1].alive) {
          g_lv->gameViewHud[opt1].alive = true;
          g_lv->gameViewHud[opt1].respawnTimeMs = g_lv->timeRunning;
        }
      }
      break;
    case log_TankSetStock:
      logReadBytes(&opt1, 1);  /* player */
      logReadBytes(&opt2, 1);  /* shells */
      logReadBytes(&opt3, 1);  /* mines */
      logReadBytes(&opt4, 1);  /* armour */
      logReadBytes(&opt5, 1);  /* trees */
      lv_screenSetTankStock(opt1, opt2, opt3, opt4, opt5);
      break;
    case log_TankSetModifiers: {
      /* player, then a length-prefixed blob of the six modifier bytes. */
      BYTE modLen;
      BYTE mods[6];
      logReadBytes(&opt1, 1);
      logReadBytes(&modLen, 1);
      /* Consume the blob whatever its length byte says, so a record with the
         wrong length costs this one value and not the reader's alignment for
         the rest of the file. Only a six-byte blob is a modifier set. */
      if (modLen > 0) {
        logReadBytes((BYTE *)mem, modLen);
      }
      if (modLen == sizeof(mods)) {
        memcpy(mods, mem, sizeof(mods));
        lv_screenSetTankModifiers(opt1, mods);
      }
      break;
    }
    case log_Shell:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      logReadBytes(&opt4, 1);
      lv_utilGetNibbles(opt3, &px, &py);
      lv_shellsAddItem(&g_lv->shs, opt1, opt2, px, py, opt4);
      break;
    case log_LgmLocation:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      logReadBytes(&opt4, 1);
      lv_utilGetNibbles(opt1, &onBoat, &frame);
      lv_utilGetNibbles(opt4, &px, &py);
      lv_playersUpdateLgm(onBoat, opt2, opt3, px, py, frame);
      break;
    case log_MessageAll:
      logReadBytes(&opt1, 1);
      logReadBytes((BYTE *)mem, 1);
      logReadBytes((BYTE *)(mem+1), (unsigned char)mem[0]);
      lv_utilPtoCString(mem, str);
      lv_playersGetPlayerName(opt1, name, sizeof(name));
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, name);
        snprintf(args.string1, sizeof(args.string1), "%.*s", (int)sizeof(args.string1) - 1, str);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, STR_LV_MSG_ALL, &args);
      }
      break;
    case log_MessagePlayers:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes((BYTE *)mem, 1);
      logReadBytes((BYTE *)(mem+1), (unsigned char)mem[0]);
      lv_utilPtoCString(mem, str);
      lv_playersGetPlayerName(opt1, name, sizeof(name));
      lv_playersGetPlayerName(opt2, name2, sizeof(name2));
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, name);
        snprintf(args.otherName, sizeof(args.otherName), "%.*s", (int)sizeof(args.otherName) - 1, name2);
        snprintf(args.string1, sizeof(args.string1), "%.*s", (int)sizeof(args.string1) - 1, str);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, STR_LV_MSG_PLAYERS, &args);
      }
      break;
    case log_MessageServer:
      logReadBytes((BYTE *)mem, 1);
      logReadBytes((BYTE *)(mem+1), (unsigned char)mem[0]);
      lv_utilPtoCString(mem, str);
      {
        MessageArgs args = {0};
        snprintf(args.string1, sizeof(args.string1), "%.*s", (int)sizeof(args.string1) - 1, str);
        lv_messageAdd(networkMessage, MESSAGE_NETSERVER, STR_LV_MSG_SERVER, &args);
      }
      break;
    case log_ServerText:
      /* A server line with the destination it was published to: opt1 the team
         it was held to, opt2 the slot. A line the whole game saw carries 0 and
         0xFF and reads like any other server line; one that reached a single
         team or a single player says so, because the recording is the only
         place that difference is visible. */
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes((BYTE *)mem, 1);
      logReadBytes((BYTE *)(mem+1), (unsigned char)mem[0]);
      lv_utilPtoCString(mem, str);
      {
        MessageArgs args = {0};
        /* The destination and the line share one 64-byte argument, so each
           part carries its own precision — the same defence the message cases
           above use against a name or a line longer than the field. */
        if (opt2 != 0xFF) {
          lv_playersGetPlayerName(opt2, name, sizeof(name));
          snprintf(args.string1, sizeof(args.string1), "[to %.*s] %.*s",
                   16, name, 36, str);
        } else if (opt1 != 0) {
          snprintf(args.string1, sizeof(args.string1), "[to team %u] %.*s",
                   (unsigned)opt1, 36, str);
        } else {
          snprintf(args.string1, sizeof(args.string1), "%.*s",
                   (int)sizeof(args.string1) - 1, str);
        }
        lv_messageAdd(networkMessage, MESSAGE_NETSERVER, STR_LV_MSG_SERVER, &args);
      }
      break;
    case log_GameTimeSet:
      /* The round's game time, as a big-endian int32 of ticks. The viewer
         counts gmeLength down a tick at a time the way the server does, so
         adopting the recorded value keeps a replay's clock on the round's own
         remaining time instead of the length the round opened with. */
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      logReadBytes(&opt4, 1);
      g_lv->gmeLength = (int32_t)(((uint32_t)opt1 << 24) |
                                  ((uint32_t)opt2 << 16) |
                                  ((uint32_t)opt3 << 8)  |
                                  (uint32_t)opt4);
      break;
    case log_RuleSet:
      /* One simulation rule a scenario changed: the rule's index as a
         big-endian u16, then the value the field ended up holding as a
         pascal blob of eight bytes. A loaded file's changes were all
         collected by the load walk, so the append finds this one there and
         adds nothing; a live feed has no walk, and this is how its changes
         arrive. Either way the rules are then read again at the playhead.
         A record that does not decode, or runs out of bytes before its
         blob does, is consumed and changes nothing. */
      {
        int    ruleIndex;
        double ruleValue;
        bool   whole;
        whole = logReadBytes(&opt1, 1) == 1 && logReadBytes(&opt2, 1) == 1 &&
                logReadBytes((BYTE *)mem, 1) == 1;
        whole = whole &&
                logReadBytes((BYTE *)(mem+1), (unsigned char)mem[0]) ==
                    (int)(unsigned char)mem[0];
        if (whole &&
            lv_ruleSetDecode(opt1, opt2, (BYTE)mem[0], (const BYTE *)(mem+1),
                             &ruleIndex, &ruleValue)) {
          lv_ruleChangeAppend(g_lv->timeRunning, ruleIndex, ruleValue, TRUE);
          lv_rulesRefresh();
        }
      }
      break;
    case log_ScnPanel:
    case log_ScnScore:
    case log_ScnAnnounce:
    case log_ScnMarker:
    case log_ScnStatus:
      /* A scenario panel's display list, a score row, the announcement
         line, a map marker or a status line, stored for the playhead. On a
         loaded file this leaves the stores as a rebuild at this time would;
         on a live feed,
         which has no walk, it is the only way they fill, and the record is
         indexed here so a seek back can rebuild from it. The reader
         consumes the record's own lengths, so everything after it is still
         read from the right byte. An announcement that was stored is also
         posted to the newswire, here and not in the store step, so a rebuild
         never posts it again. */
      {
        int presKey = lv_presReadRecord(code, isV2 ? evLen : 0u,
                                        g_lv->timeRunning);
        if (presKey >= 0 && isV2) {
          lv_presLiveIndex(g_lv->timeRunning, code, presKey, payloadStart,
                           evLen);
        }
        if (presKey >= 0 && code == log_ScnAnnounce) {
          lv_presPostAnnounce(&s_presPayload);
        }
      }
      break;
    case log_ScnHint:
      /* An order a scenario gave one bot: the bot's slot, then the verb the
         order led with as a pascal string. Read and dropped — the viewer
         shows nothing for it — and what this case has to do is consume the
         record's bytes so everything after it is still read from the right
         byte. */
      logReadBytes(&opt1, 1);
      logReadBytes((BYTE *)mem, 1);
      logReadBytes((BYTE *)(mem+1), (unsigned char)mem[0]);
      break;
    case log_ServerTick:
      /* The server's game tick at this entry, as a big-endian u32. A loaded
         file's anchors were all collected by the load walk; a live feed has
         no walk, and this is how its anchors arrive. The record is consumed
         by its framed length whatever that says, and read as a tick only
         when the length is four. */
      if (isV2) {
        size_t payloadPos = lv_logGetCurrentPosition();
        BYTE   t[4];
        if (evLen == 4 && logReadBytes(t, 4) == 4 && !s_wtWalked) {
          lv_tickAnchorAdd(g_lv->timeRunning,
                           ((uint32_t)t[0] << 24) | ((uint32_t)t[1] << 16) |
                           ((uint32_t)t[2] << 8)  | (uint32_t)t[3],
                           FALSE);
        }
        lv_logSetPosition(payloadPos + evLen);
      } else {
        /* Never written to an unframed file. Four bytes, as the byte walker
           sizes it. */
        BYTE t[4];
        logReadBytes(t, 4);
      }
      break;
    case log_BaseSetOwner:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      lv_basesSetOwner(&g_lv->bs, opt1, opt2, opt3);
      break;
    case log_BaseSetStock:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      logReadBytes(&opt4, 1);
      lv_basesSetStock(&g_lv->bs, opt1, opt2, opt3, opt4);
      /* opt1 is the 0-based base index in the log (the server emits it
       * post-decrement — see basesSetBase / basesUpdateStock in
       * src/bolo/bases.c). */
      break;
    case log_PillSetOwner:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      lv_pillsSetPillOwner(&g_lv->pb, opt1, opt2, opt3);
      break;
    case log_PillSetPlace:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      lv_pillsSetPos(&g_lv->pb, opt1, opt2, opt3);
      break;
    case log_PillSetHealth:
      /* v3 names the pillbox and its armour in a byte each. Up to v2 the
         pair shared one byte — index in the high nibble, armour in the low
         — which is all the armour a pillbox could hold back then. Reading
         two bytes from an older file would take the next record's type
         code as the armour and lose the stream from there, so the length
         follows the version the file states. */
      logReadBytes(&opt1, 1);
      if (g_lv->loadedLogVersion >= LOG_VERSION_V3) {
        logReadBytes(&opt2, 1);
        lv_pillsSetHealth(&g_lv->pb, opt1, opt2);
      } else {
        lv_utilGetNibbles(opt1, &opt2, &opt3);
        lv_pillsSetHealth(&g_lv->pb, opt2, opt3);
      }
      break;
    case log_PillSetInTank:
      logReadBytes(&opt1, 1);
      lv_utilGetNibbles(opt1, &opt2, &opt3);
      lv_pillsSetInTank(&g_lv->pb, opt2, opt3);
      break;
    case log_EntityChange:
      /* One pillbox, base or start has joined the map or left it. opt1 is
         which list, opt2 the item's number counting from zero — the three
         modules count from one — and opt3 whether it is now on the map. The
         pascal blob after them is the item's map record, six bytes for a
         pillbox or a base and three for a start, the same bytes a live client
         gets on the wire. A removal keeps the slot and the count, so every
         number above it goes on meaning the same item. */
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      logReadBytes((BYTE *)mem, 1);
      logReadBytes((BYTE *)(mem+1), (unsigned char)mem[0]);
      {
        BYTE num = (BYTE)(opt2 + 1);
        const BYTE *rec = (const BYTE *)(mem + 1);
        BYTE recLen = (BYTE)mem[0];
        switch (opt1) {
        case LV_ENTITY_KIND_PILL:
          if (opt3 != 0) {
            if (recLen >= 6) {
              pillbox item;
              memset(&item, 0, sizeof(item));
              item.x = rec[0];
              item.y = rec[1];
              item.owner = rec[2];
              item.armour = rec[3];
              item.speed = rec[4];
              item.inTank = rec[5] ? TRUE : FALSE;
              lv_pillsInstallItem(&g_lv->pb, &item, num);
            }
          } else {
            lv_pillsRemoveItem(&g_lv->pb, num);
          }
          break;
        case LV_ENTITY_KIND_BASE:
          if (opt3 != 0) {
            if (recLen >= 6) {
              base item;
              memset(&item, 0, sizeof(item));
              item.x = rec[0];
              item.y = rec[1];
              item.owner = rec[2];
              item.armour = rec[3];
              item.shells = rec[4];
              item.mines = rec[5];
              lv_basesInstallItem(&g_lv->bs, &item, num);
            }
          } else {
            lv_basesRemoveItem(&g_lv->bs, num);
          }
          break;
        case LV_ENTITY_KIND_START:
          if (opt3 != 0) {
            if (recLen >= 3) {
              start item;
              memset(&item, 0, sizeof(item));
              item.x = rec[0];
              item.y = rec[1];
              item.dir = rec[2];
              lv_startsInstallItem(&g_lv->ss, &item, num);
            }
          } else {
            lv_startsRemoveItem(&g_lv->ss, num);
          }
          break;
        default:
          /* A kind with no list behind it. The framed length has already
             been consumed, so there is nothing to resynchronise. */
          break;
        }
      }
      g_lv->wantScreenUpdate = TRUE;
      break;
    case log_EntityMasks:
      /* Which indices are on the map, as three big-endian 16-bit masks — the
         part the snapshot before this one had nowhere to put. Bit i stands
         for index i counting from zero; a bit at or above a list's own count
         names no item and is skipped. Only the flags move: the counts and the
         records are the ones the snapshot installed, and taking an item off
         the map keeps its record, so an index put back holds the item it
         always held. That makes a snapshot and this record together enough to
         state the world, which is what a seek lands on. */
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      logReadBytes(&opt4, 1);
      logReadBytes(&opt5, 1);
      logReadBytes(&px, 1);
      {
        unsigned short pillMask  = (unsigned short)((opt1 << 8) | opt2);
        unsigned short baseMask  = (unsigned short)((opt3 << 8) | opt4);
        unsigned short startMask = (unsigned short)((opt5 << 8) | px);
        BYTE num;
        BYTE total;

        /* Each count is clamped to its list's size before it is walked: a
           16-bit mask cannot name anything past index 15 anyway, and a count
           a malformed blob left above the array is not a number this loop
           should reach for. */
        total = lv_pillsGetNumPills(&g_lv->pb);
        if (total > MAX_PILLS) total = MAX_PILLS;
        for (num = 1; num <= total; num++) {
          lv_pillsSetActive(&g_lv->pb, num,
                            (pillMask & (1u << (num - 1))) != 0);
        }
        total = lv_basesGetNumBases(&g_lv->bs);
        if (total > MAX_BASES) total = MAX_BASES;
        for (num = 1; num <= total; num++) {
          lv_basesSetActive(&g_lv->bs, num,
                            (baseMask & (1u << (num - 1))) != 0);
        }
        total = lv_startsGetNumStarts(&g_lv->ss);
        if (total > MAX_STARTS) total = MAX_STARTS;
        for (num = 1; num <= total; num++) {
          lv_startsSetActive(&g_lv->ss, num,
                             (startMask & (1u << (num - 1))) != 0);
        }
      }
      g_lv->wantScreenUpdate = TRUE;
      break;
    case log_KillPlayer:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      lv_playersGetPlayerName(opt1, mem, sizeof(mem));
      if (opt1 == opt2 || opt2 == NEUTRAL) {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, mem);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, STR_LV_PLAYER_DIED, &args);
      } else {
        MessageArgs args = {0};
        lv_playersGetPlayerName(opt2, str, sizeof(str));
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, str);
        snprintf(args.otherName, sizeof(args.otherName), "%.*s", (int)sizeof(args.otherName) - 1, mem);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, STR_LV_PLAYER_KILLED, &args);
      }
      if (opt1 < MAX_TANKS) g_lv->deaths[opt1]++;
      if (opt2 != opt1 && opt2 != NEUTRAL && opt2 < MAX_TANKS) {
        g_lv->kills[opt2]++;
      }
      lv_playersUpdateTank(opt1, 0, 0, 0, 0, 0, TRUE);
      if (opt1 < MAX_TANKS) {
        g_lv->gameViewHud[opt1].alive = false;
        g_lv->gameViewHud[opt1].deathTimeMs = g_lv->timeRunning;
      }
      break;
    case log_PlayerRejoin:
      logReadBytes(&opt1, 1);
      lv_playersGetPlayerName(opt1, str, sizeof(str));
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, str);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, STR_LV_PLAYER_REJOINED, &args);
      }
      if (opt1 < MAX_TANKS) {
        g_lv->gameViewHud[opt1].alive = true;
        g_lv->gameViewHud[opt1].respawnTimeMs = g_lv->timeRunning;
      }
      break;
    case log_PlayerLeaving:
      logReadBytes(&opt1, 1);
      lv_playersGetPlayerName(opt1, str, sizeof(str));
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, str);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, STR_LV_PLAYER_LEAVING, &args);
      }
      if (opt1 < MAX_TANKS) {
        g_lv->gameViewHud[opt1].alive = false;
        if (g_lv->gameView && opt1 == g_lv->cameraSlot) {
          /* Advance camera to next in-use slot. Mirrors Tab cycle in
           * logviewer.c:651-660. The `next != opt1` guard skips the
           * leaving player explicitly: lv_playersIsInUse(opt1) may still
           * return TRUE here, and we don't want the camera to bounce
           * back. */
          BYTE start = g_lv->cameraSlot;
          BYTE found = start;
          BYTE i;
          for (i = 1; i <= MAX_TANKS; i++) {
            BYTE next = (BYTE)((start + i) % MAX_TANKS);
            if (next != opt1 && lv_playersIsInUse(next)) {
              found = next;
              break;
            }
          }
          g_lv->cameraSlot = found;
          g_lv->wantScreenUpdate = TRUE;
        }
      }
      break;
    case log_PlayerDied:
      logReadBytes(&opt1, 1);
      lv_playersUpdateTank(opt1, 0, 0, 0, 0, 0, TRUE);
      if (opt1 < MAX_TANKS) {
        g_lv->gameViewHud[opt1].alive = false;
        g_lv->gameViewHud[opt1].deathTimeMs = g_lv->timeRunning;
      }
      break;
    case log_SaveMap:
      /* No-op — marker event with no visual effect on replay */
      break;
    case log_LobbyEnter:
      lv_messageAdd(networkStatus, MESSAGE_NETSERVER, STR_LV_LOBBY_OPENED, NULL);
      break;
    case log_LobbyExit:
      lv_messageAdd(networkStatus, MESSAGE_NETSERVER, STR_LV_GAME_STARTED, NULL);
      break;
    case log_PlayerReady:
      logReadBytes(&opt1, 1);
      lv_playersGetPlayerName(opt1, str, sizeof(str));
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, str);
        lv_messageAdd(networkStatus, MESSAGE_NETSERVER, STR_LV_PLAYER_READY, &args);
      }
      break;
    case log_PlayerUnready:
      logReadBytes(&opt1, 1);
      lv_playersGetPlayerName(opt1, str, sizeof(str));
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, str);
        lv_messageAdd(networkStatus, MESSAGE_NETSERVER, STR_LV_PLAYER_UNREADY, &args);
      }
      break;
    case log_TeamSet:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      lv_playersGetPlayerName(opt1, str, sizeof(str));
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, str);
        if (opt2 == 0) {
          lv_messageAdd(networkStatus, MESSAGE_NETSERVER, STR_LV_PLAYER_LEFT_TEAM, &args);
        } else {
          args.number = opt2;
          lv_messageAdd(networkStatus, MESSAGE_NETSERVER, STR_LV_PLAYER_JOINED_TEAM, &args);
        }
      }
      /* The slot's team, for the team panels. The line above is playback's
         alone; a rebuild sets the team without it. A feed indexes it here,
         since it has no walk to. */
      {
        int teamKey = lv_presStoreTeam(opt1, opt2, g_lv->timeRunning);
        if (isV2) {
          lv_presLiveIndex(g_lv->timeRunning, code, teamKey, payloadStart,
                           evLen);
        }
      }
      break;
    case log_CountdownStart:
      lv_messageAdd(networkStatus, MESSAGE_NETSERVER, STR_LV_COUNTDOWN_START, NULL);
      break;
    case log_CountdownCancel:
      lv_messageAdd(networkStatus, MESSAGE_NETSERVER, STR_LV_COUNTDOWN_CANCEL, NULL);
      break;
    case log_MapSkipVote:
      logReadBytes(&opt1, 1);
      lv_playersGetPlayerName(opt1, str, sizeof(str));
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, str);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, STR_LV_MAP_SKIP_VOTE, &args);
      }
      break;
    case log_MapSkipApplied:
      logReadBytes((BYTE *)mem, 1);
      logReadBytes((BYTE *)(mem+1), (unsigned char)mem[0]);
      lv_utilPtoCString(mem, str);
      {
        MessageArgs args = {0};
        snprintf(args.string1, sizeof(args.string1), "%.*s", (int)sizeof(args.string1) - 1, str);
        lv_messageAdd(networkStatus, MESSAGE_NETSERVER, STR_LV_MAP_SKIPPED, &args);
      }
      break;
    case log_GameSettings:
      /* Panel data, not a chat line: the Game Information window reads the
         settings back out of the store, so nothing goes to the newswire. */
      logReadBytes((BYTE *)mem, 1);
      logReadBytes((BYTE *)(mem+1), (unsigned char)mem[0]);
      lv_screenPlaybackGameSettings((BYTE *)(mem+1), (unsigned char)mem[0]);
      break;
    case log_BalanceApplied:
      lv_messageAdd(networkStatus, MESSAGE_NETSERVER, STR_LV_TEAM_BALANCE, NULL);
      break;
    case log_GameVoteStart:
      logReadBytes(&opt1, 1);  /* kind */
      logReadBytes(&opt2, 1);  /* initiator */
      logReadBytes(&opt3, 1);  /* team (0 = global) */
      lv_playersGetPlayerName(opt2, str, sizeof(str));
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, str);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE,
                      opt1 == GAME_VOTE_KIND_SURRENDER
                          ? STR_LV_VOTE_START_SURRENDER
                          : STR_LV_VOTE_START_LOBBY,
                      &args);
      }
      break;
    case log_GameVoteCast:
      logReadBytes(&opt1, 1);  /* kind */
      logReadBytes(&opt2, 1);  /* player */
      logReadBytes(&opt3, 1);  /* voteYes (0/1) */
      lv_playersGetPlayerName(opt2, str, sizeof(str));
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, str);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE,
                      opt3 ? STR_LV_VOTE_CAST_YES : STR_LV_VOTE_CAST_NO,
                      &args);
      }
      break;
    case log_GameVoteEnd:
      logReadBytes(&opt1, 1);  /* kind */
      logReadBytes(&opt2, 1);  /* result */
      lv_messageAdd(networkStatus, MESSAGE_NETSERVER,
                    opt2 ? STR_LV_VOTE_PASSED : STR_LV_VOTE_FAILED, NULL);
      break;
    case log_Ping:
      /* sender + kind + worldX (BE u16) + worldY (BE u16). */
      logReadBytes(&opt1, 1);  /* sender */
      logReadBytes(&opt2, 1);  /* kind */
      logReadBytes(&opt3, 1);  /* worldX high */
      logReadBytes(&opt4, 1);  /* worldX low */
      logReadBytes(&opt5, 1);  /* worldY high */
      {
        BYTE yLo = 0;
        uint16_t wx, wy;
        logReadBytes(&yLo, 1);
        wx = (uint16_t)((opt3 << 8) | opt4);
        wy = (uint16_t)((opt5 << 8) | yLo);
        lv_pingAdd(opt1, opt2, wx, wy);
        lv_playersGetPlayerName(opt1, str, sizeof(str));
        {
          MessageArgs args = {0};
          snprintf(args.playerName, sizeof(args.playerName), "%.*s",
                   (int)sizeof(args.playerName) - 1, str);
          lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE,
                        pingKindMessageId(opt2), &args);
        }
      }
      break;
    case log_SpectatorJoined:
      logReadBytes(&opt1, 1);  /* spectator slot */
      logReadBytes(&opt2, 1);  /* country[0] */
      logReadBytes(&opt3, 1);  /* country[1] */
      logReadBytes(&opt4, 1);  /* wbnFlags (not displayed) */
      logReadBytes(&opt5, 1);  /* reserved */
      logReadBytes((BYTE *)mem, 1);
      logReadBytes((BYTE *)mem+1, (unsigned char)mem[0]);
      lv_utilPtoCString(mem, name);
      {
        /* Prefix the 2-char country into {player} only when present and not
           the "XX" unknown sentinel; the [%c%c] tag is a raw country code and
           is not localized. */
        MessageArgs args = {0};
        if (opt2 != 0 && opt3 != 0 && !(opt2 == 'X' && opt3 == 'X')) {
          snprintf(args.playerName, sizeof(args.playerName), "[%c%c] %s", opt2, opt3, name);
        } else {
          snprintf(args.playerName, sizeof(args.playerName), "%s", name);
        }
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, STR_LV_SPEC_JOINED, &args);
      }
      break;
    case log_SpectatorLeft:
      logReadBytes(&opt1, 1);  /* spectator slot */
      logReadBytes((BYTE *)mem, 1);
      logReadBytes((BYTE *)mem+1, (unsigned char)mem[0]);
      lv_utilPtoCString(mem, name);
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%s", name);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, STR_LV_SPEC_LEFT, &args);
      }
      break;
    case log_SpectatorChat:
      /* No emitter yet (format-reserved for Phase 6); decode so the cursor
         stays aligned and the line is ready when chat ships. */
      logReadBytes(&opt1, 1);  /* sender spectator slot */
      logReadBytes((BYTE *)mem, 1);
      logReadBytes((BYTE *)(mem+1), (unsigned char)mem[0]);
      lv_utilPtoCString(mem, str);  /* copies the message out of mem */
      {
        MessageArgs args = {0};
        args.number = opt1;
        snprintf(args.string1, sizeof(args.string1), "%s", str);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, STR_LV_SPEC_CHAT, &args);
      }
      break;
    default:
      if (isV2) {
        /* Unknown future event type: skip its framed payload and keep
           going. evLen is a full u16 so it can exceed the v1 per-type
           maximum; advance the read cursor directly. */
        lv_logSetPosition(lv_logGetCurrentPosition() + evLen);
      } else {
        lv_windowStop(TRUE);
        count = numEvents;
      }
      break;

    }
    /* The frame's length decides where the next event starts, not the
       fields a case read. The writer frames each event by the bytes it
       wrote, so the two agree in a file it produced; where they do not, a
       case that read short or long would otherwise leave every event after
       it decoding from the wrong byte. */
    if (isV2) {
      lv_logSetPosition(payloadStart + evLen);
    }
    /* v2 is plaintext: blockKey stays 0 for the whole stream, so the
       per-event key roll is suppressed. v1 rolls the key to the event
       code (matches the writer's logKey rotation). */
    if (!isV2) {
      lv_blocksSetKey(code);
    }
    count++;
  }
}

void lv_screenRequestUpdate() {
  /* Finally lets update our item in the frontend */
  if (g_lv->isPlaying == TRUE && g_lv->fastForwarding == FALSE) {
    if (g_lv->selectedItemType == 0) {
      lv_updateItem(0, 0, 0, 0, 0, 0, 0, 0, 0);
    } else if (g_lv->selectedItemType == 1) {
      lv_updateItem(1, g_lv->selectedItem, g_lv->bs->item[g_lv->selectedItem].owner, g_lv->bs->item[g_lv->selectedItem].x, g_lv->bs->item[g_lv->selectedItem].y, g_lv->bs->item[g_lv->selectedItem].armour, g_lv->bs->item[g_lv->selectedItem].shells, g_lv->bs->item[g_lv->selectedItem].mines, FALSE);
    } else {
      lv_updateItem(2, g_lv->selectedItem, g_lv->pb->item[g_lv->selectedItem].owner, g_lv->pb->item[g_lv->selectedItem].x, g_lv->pb->item[g_lv->selectedItem].y, g_lv->pb->item[g_lv->selectedItem].armour, 0, 0, g_lv->pb->item[g_lv->selectedItem].inTank);
    }
  }
}

/* Set by lv_screenLogTick when a live feed had no record to read: the tick
   is done and playback is waiting for the feed to grow. Cleared at the top of
   every tick. */
static bool s_logTickAtLiveHead = FALSE;

/* Returns TRUE on log end or snapshot */
bool lv_screenLogTick() {
  bool returnValue = FALSE;
  BYTE code;
  BYTE top = 0;    /* stay 0 if the log runs out partway through a header */
  BYTE bottom = 0;
  unsigned short us;
  unsigned short len = 0;

  bool process = FALSE;
  s_logTickAtLiveHead = FALSE;
  g_lv->timeRunning += 20; /* Add 20 ms */
  if (g_lv->gmeStartDelay > 0) {
    g_lv->gmeStartDelay--;
  }
  if (g_lv->gmeLength > 0 && g_lv->gmeStartDelay == 0) {
    g_lv->gmeLength--;
  }
  lv_shellsDestroy(&g_lv->shs);
  g_lv->shs = lv_shellsCreate();
  if (g_lv->isPlaying == TRUE) {
    switch (g_lv->state) {
    case lv_lr_start:
      /* Read bytes */
      if (logReadBytes(&code, 1) != 1) {
        /* Out of bytes with no LOG_QUIT read. A live feed has caught up with
           its head and waits for the next record. A file was cut short, so
           it ends here as LOG_QUIT would end it, rather than going on to
           switch on a byte it never read on every tick from now on. */
        if (lv_screenSpecIsLiveMode() == FALSE) {
          g_lv->isPlaying = FALSE;
          lv_messageAdd(networkStatus, MESSAGE_NETSERVER, STR_LV_END_OF_LOG, NULL);
          lv_finished();
          returnValue = TRUE;
        } else {
          s_logTickAtLiveHead = TRUE;
        }
        break;
      }
      switch (code) {
      case LOG_QUIT:
        g_lv->isPlaying = FALSE;
        lv_messageAdd(networkStatus, MESSAGE_NETSERVER, STR_LV_END_OF_LOG, NULL);
        lv_finished();
        returnValue = TRUE;
        break;
      case LOG_SNAPSHOT:
        /* On a live feed a snapshot is a ring keyframe, which stands in for
           its writer tick. */
        if (lv_wtLiveCounts()) {
          lv_wtCountTick(g_lv->timeRunning, FALSE);
        }
        lv_processSnapshot();
        returnValue = TRUE;
        break;
      case LOG_NOEVENTS:
        logReadBytes((BYTE *) &g_lv->waitLen, 1);
        g_lv->state = lv_lr_shortwait;
        if (g_lv->waitLen == 0) {
          g_lv->waitLen = 1;
        }
        if (lv_wtLiveCounts()) {
          lv_wtCountWait(g_lv->timeRunning, (uint32_t)g_lv->waitLen);
        }
        break;
      case LOG_NOEVENTS_LONG:
        logReadBytes(&top, 1);
        logReadBytes(&bottom, 1);
        us = top << 8;
        us += bottom;
        g_lv->waitLen = ntohs(us);

        g_lv->state = lv_lr_longwait;
        if (g_lv->waitLen == 0) {
          g_lv->waitLen = 1;
        }
        if (lv_wtLiveCounts()) {
          lv_wtCountWait(g_lv->timeRunning, (uint32_t)g_lv->waitLen);
        }
        break;
      case LOG_EVENT:
        logReadBytes(&top, 1);
        len = top;
        process = TRUE;
        break;
      case LOG_EVENT_LONG:
        logReadBytes(&top, 1);
        logReadBytes(&bottom, 1);
        us = top << 8;
        us += bottom;
        len = ntohs(us);
        process = TRUE;
        break;
      }
      /* Counted before the block is read, so a log_ServerTick in it anchors
         on a count that includes its own tick. */
      if (process == TRUE && lv_wtLiveCounts()) {
        lv_wtCountTick(g_lv->timeRunning, FALSE);
      }
      break;
    case lv_lr_longwait:
    case lv_lr_shortwait:
      g_lv->waitLen--;
      if (g_lv->waitLen == 0) {
        g_lv->state = lv_lr_start;
      }
      break;
    }
    if (process == TRUE) {
      lv_playersLgmZero();
      lv_screenProcessLog(len);
      if (g_lv->centredTank == TRUE) {
        lv_screenFollowCentredTank();
      }
    }
  }
  return returnValue;
}

/* Put the camera on the followed tank, in native pixels rather than whole
 * map squares.
 *
 * This used to assign xOffset/yOffset straight from lv_playersGetCentredX/Y —
 * which are BYTE map squares — and never touch subPxX/subPxY, so the view
 * could only move in 16-pixel steps: the tank drifted a whole tile off centre
 * and the whole world snapped back the instant it crossed a boundary. One
 * jump per tile of travel, forever, which is exactly the cyclic one-tile jump
 * the reel showed while a round played.
 *
 * Everything needed for the smooth version was already here: the render target
 * carries one spare tile (draw.c sizes it (screenSizeX + 1) * TILE_SIZE_X) and
 * the host already uses subPxX/subPxY as the blit's source origin
 * (lv_screenGetSubOffset -> lvEmbedFrameTexture). Only the sub-tile part was
 * missing. Same defect and same fix as viewCamPixelsF in the map preview
 * widget: derive every value from one un-truncated position.
 *
 * Clamps mirror lv_screenPanToTotalPixels — the whole-tile range is
 * [0, 255 - screenSize] and sub-pixel is forced to zero at the far edge so the
 * trailing edge has no bleed past the rendered tiles.
 *
 * A sub-tile-only move needs no tile re-render: the host re-reads the sub
 * offset every frame and shifts the source rect. wantScreenUpdate is still set
 * because the tank sprites inside the target moved. */
void lv_screenFollowCentredTank(void) {
  int px, py;
  int sizeX, sizeY, maxOffX, maxOffY, maxPxX, maxPxY;
  int camPxX, camPxY, newOffX, newOffY, newSubX, newSubY;

  if (g_lv == NULL || g_lv->logLoaded == FALSE) {
    return;
  }
  px = lv_playersGetCentredPixelX();
  py = lv_playersGetCentredPixelY();
  if (px < 0 || py < 0) {
    return;
  }

  sizeX = lv_screenGetSizeX();
  sizeY = lv_screenGetSizeY();
  /* Centre the viewport on the tank, in pixels. */
  camPxX = px - (sizeX * TILE_SIZE_X) / 2;
  camPxY = py - (sizeY * TILE_SIZE_Y) / 2;

  maxOffX = 255 - sizeX; if (maxOffX < 0) maxOffX = 0;
  maxOffY = 255 - sizeY; if (maxOffY < 0) maxOffY = 0;
  maxPxX = maxOffX * TILE_SIZE_X;
  maxPxY = maxOffY * TILE_SIZE_Y;
  if (camPxX < 0) camPxX = 0;
  if (camPxY < 0) camPxY = 0;
  if (camPxX > maxPxX) camPxX = maxPxX;
  if (camPxY > maxPxY) camPxY = maxPxY;

  newOffX = camPxX / TILE_SIZE_X;
  newOffY = camPxY / TILE_SIZE_Y;
  newSubX = camPxX - newOffX * TILE_SIZE_X;
  newSubY = camPxY - newOffY * TILE_SIZE_Y;

  /* Compare BEFORE storing, then flag on ANY move, sub-tile included: the
   * host's blit origin comes from a snapshot taken at render time, so a camera
   * move that triggers no re-render would never reach the screen. Matters most
   * for the paused reel, where a focus jump is the only thing moving; during
   * playback the sprites dirty the screen every tick anyway, so this costs
   * nothing extra. Skipping the flag when nothing moved keeps a paused,
   * unfocused reel fully idle. */
  if ((BYTE)newOffX != g_lv->xOffset || (BYTE)newOffY != g_lv->yOffset ||
      newSubX != g_lv->subPxX || newSubY != g_lv->subPxY) {
    g_lv->xOffset = (BYTE)newOffX;
    g_lv->yOffset = (BYTE)newOffY;
    g_lv->subPxX  = newSubX;
    g_lv->subPxY  = newSubY;
    g_lv->wantScreenUpdate = TRUE;
  }
}

void lv_screenCentreOnSelectedItem() {
  BYTE x;
  BYTE y;
  base b;
  pillbox p;
  if (g_lv->isPlaying == TRUE && g_lv->selectedItemType != 0) {
    if (g_lv->selectedItemType == 2) {
      lv_pillsGetPill(&g_lv->pb, &p, (BYTE) (g_lv->selectedItem+1));
      x = p.x;
      y = p.y;
    } else {
      lv_basesGetBase(&g_lv->bs, &b, (BYTE) (g_lv->selectedItem+1));
      x = b.x;
      y = b.y;
    }
    int cx = (int)x - (int)(g_lv->screenSizeX / 2);
    int cy = (int)y - (int)(g_lv->screenSizeY / 2);
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx + g_lv->screenSizeX > 255) cx = 255 - g_lv->screenSizeX;
    if (cy + g_lv->screenSizeY > 255) cy = 255 - g_lv->screenSizeY;
    BYTE newXOffset = (BYTE)cx;
    BYTE newYOffset = (BYTE)cy;
    if (newXOffset != g_lv->xOffset || g_lv->yOffset != newYOffset) {
      g_lv->xOffset = newXOffset;
      g_lv->yOffset = newYOffset;
      /* Defer to the flag — see lv_screenPanToTotalPixels: an inline render
       * during input pairs new content with the frame's already-recorded
       * blit offsets (one displaced frame). */
      g_lv->wantScreenUpdate = TRUE;
    }
  }
}

/* Whether the snapshot lv_processSnapshot last decoded carried a world. The
 * recorder's lobby-mode body writes no map runs at all (logSerializeSnapshotBody
 * in log.c), a running-round body always writes at least one, so the run count
 * is what separates the two. */
static bool s_lastSnapshotHadWorld = FALSE;

/* File position of the round's first world snapshot: the kickoff rewrite on a
 * lobby-started log, the opening snapshot otherwise. Palette colours are dealt
 * afresh there and kept everywhere else (lv_playersRebuildTeams), and keying
 * that on the position rather than on the previous decode means a seek back
 * to it deals the same colours the first pass did, while a seek from the
 * lobby straight into mid-game keeps the stored ones. Forgotten with the
 * snapshot store, since the position belongs to one file. */
static bool   s_firstWorldSnapKnown = FALSE;
static size_t s_firstWorldSnapPos = 0;

/* The same answer for the opening snapshot, which the loader consumes before
 * the event stream starts. Set, it means the log opened on a running round and
 * has no lobby in front of it. Both loaders write it as they decode that
 * snapshot, so it needs no separate reset. */
static bool s_openingSnapshotHadWorld = FALSE;

bool lv_processSnapshot() {
  bool returnValue = TRUE;
  BYTE data[512];
  BYTE dataLenRaw;
  unsigned int dataLen;
  int len;
  BYTE count = 0;
  BYTE mx, my, px, py, lgmmx, lgmmy, lgmpx, lgmpy, lgmframe, frame;
  bool onBoat;
  char name[64];
  char location[64];
  BYTE numAllies;
  BYTE *allies;
  BYTE pos;
  /* Where this snapshot starts, for the seek store. The teams that go with
     it are stored at the end, once they have been rebuilt, so a seek back
     here restores the colours this snapshot settled on rather than the ones
     that were current before it was decoded. */
  size_t snapPos = lv_logGetCurrentPosition();
  uint32_t snapTime = g_lv->timeRunning;
  BYTE snapKey = lv_blocksGetKey();
  bool worldRead = FALSE;


  /* Read in start delay and time limit */
  logReadBytes((BYTE *) &g_lv->gmeStartDelay, sizeof(int32_t));
  g_lv->gmeStartDelay = ntohl(g_lv->gmeStartDelay);
  logReadBytes((BYTE *) &g_lv->gmeLength , sizeof(int32_t));
  g_lv->gmeLength = ntohl(g_lv->gmeLength);

  /* Read pillboxes, bases and starts */
  if (returnValue == TRUE) {
    len = logReadBytes(&dataLenRaw, 1);
    dataLen = dataLenRaw;
    if (dataLen > sizeof(data)) {
      returnValue = FALSE;
    } else {
      logReadBytes(data, dataLen);
      lv_pillsSetPillNetData(&g_lv->pb, data, dataLen);
    }
  }

  if (returnValue == TRUE) {
    len = logReadBytes(&dataLenRaw, 1);
    dataLen = dataLenRaw;
    if (dataLen > sizeof(data)) {
      returnValue = FALSE;
    } else {
      logReadBytes(data, dataLen);
      lv_basesSetBaseNetData(&g_lv->bs, data, dataLen);
    }
  }
  if (returnValue == TRUE) {
    len = logReadBytes(&dataLenRaw, 1);
    dataLen = dataLenRaw;
    if (dataLen > sizeof(data)) {
      returnValue = FALSE;
    } else {
      logReadBytes(data, dataLen);
      lv_startsSetStartNetData(&g_lv->ss, data, dataLen);
    }
  }
  if (returnValue == TRUE) {
    int numRuns = 0;
    returnValue = lv_mapReadRuns(&g_lv->mp, &numRuns);
    s_lastSnapshotHadWorld = (numRuns > 0);
    worldRead = s_lastSnapshotHadWorld;
  }


  /* Process each player */
  while (count < MAX_TANKS && returnValue == TRUE) {
    logReadBytes(&dataLenRaw, 1);
    dataLen = dataLenRaw;
    if (dataLen == 2) {
      /* Player is not in use */
      lv_playersLeaveGame(count, FALSE);
      // Process the dummy data
      len = logReadBytes(data, 2);
      lv_screenSetTankStock(count, 0, 0, 0, 0);

    } else if (dataLen > sizeof(data)) {
      returnValue = FALSE;
    } else {
      len = logReadBytes(data, dataLen);
      if ((unsigned int)len != dataLen) {
        returnValue = FALSE;
      } else if (dataLen < 12) {
        /* Minimum player record: 2 header + 9 fixed fields + 1 name len byte */
        returnValue = FALSE;
      } else {
        pos = 2;
        mx = data[pos++];
        my = data[pos++];
        lv_utilGetNibbles(data[pos++], &px, &py);
        frame = data[pos++];
        onBoat = data[pos++];
        lgmmx = data[pos++];
        lgmmy = data[pos++];
        lv_utilGetNibbles(data[pos++], &lgmpx, &lgmpy);
        lgmframe = data[pos++];
        /* pos is now 11 — skip the redundant dataLen byte */
        pos++;
        /* Parse player name (pascal string: length byte + chars) */
        if (pos > dataLen || *(data+pos-1) >= sizeof(name)) {
          returnValue = FALSE;
        } else {
          lv_utilPtoCString((char *)(data+pos-1), name);
          pos += *(data+pos-1);
          pos++;
        }
        /* Parse location (pascal string) */
        if (returnValue == TRUE) {
          if (pos > dataLen || *(data+pos-1) >= sizeof(location)) {
            returnValue = FALSE;
          } else {
            lv_utilPtoCString((char *)(data+pos-1), location);
            pos += *(data+pos-1);
          }
        }
        /* Parse allies */
        if (returnValue == TRUE) {
          if (pos >= dataLen) {
            returnValue = FALSE;
          } else {
            numAllies = data[pos];
            pos++;
            if ((unsigned int)(pos + numAllies) > dataLen) {
              returnValue = FALSE;
            } else {
              allies = data+pos;
              /* Snapshot replay does not extract the clientFlags byte in
               * TankSnapshot; default the v1-log accountFlags storage to 0.
               * The flags will be re-set by any subsequent log_PlayerJoined
               * event for this slot. */
              lv_playersSetPlayer(count, name, location, mx ,my, px, py, frame, onBoat, numAllies, allies, FALSE, TRUE, 0);
              /* Tank stocks: shells, mines, armour, trees, on the end of the
                 block after the alliance list. A block that ends with the
                 alliances was written before the recorder carried them, so the
                 slot reads as no stocks rather than a guessed value. */
              pos = (BYTE)(pos + numAllies);
              if ((unsigned int)(pos + 4) <= dataLen) {
                lv_screenSetTankStock(count, data[pos], data[pos+1],
                                      data[pos+2], data[pos+3]);
              } else {
                lv_screenSetTankStock(count, 0, 0, 0, 0);
              }
              /* mx != 0 means the tank is on the map (alive) — the same sentinel
                 the forward log_PlayerLocation path uses. Mark the slot alive so
                 a mid-game seed clears the death-static overlay for living tanks;
                 a dead/off-map tank (mx == 0) stays not-alive. */
              if (mx != 0) {
                g_lv->gameViewHud[count].alive = true;
                g_lv->gameViewHud[count].respawnTimeMs = g_lv->timeRunning;
              }
              /* A snapshot carries (0,0) lgm coords for a man who is aboard/idle
                 (the server's idle sentinel). Only mark him out for a real
                 out-of-tank position; lv_playersSetPlayer above already left
                 lgmIsOut FALSE for the aboard case, matching the forward
                 stream where a boarded man emits no log_LgmLocation. */
              if (lgmmx != 0 || lgmmy != 0) {
                lv_playersUpdateLgm(count, lgmmx, lgmmy, lgmpx, lgmpy,lgmframe);
              }
            }
          }
        }
      }
    }
    count++;
  }

  if (worldRead && s_firstWorldSnapKnown == FALSE) {
    s_firstWorldSnapKnown = TRUE;
    s_firstWorldSnapPos = snapPos;
  }
  /* Unconditional: a snapshot that failed part-way can still have left a
     slot on NO_TEAM_SET (a seek restores that for players the stored teams
     predate), and the rebuild only reads inUse, allies and team, so it is
     safe to run on whatever did load. Callers draw regardless of the return. */
  lv_playersRebuildTeams(!(s_firstWorldSnapKnown && snapPos == s_firstWorldSnapPos));
  lv_playersCopyPTeams(data);
  lv_snapshotAdd(&g_lv->snap, snapPos, snapTime, snapKey, data);
  return returnValue;
}

/*********************************************************
* Walker for total-time computation
*
* Walks the decompressed log byte stream from the current
* position to LOG_QUIT/EOF, counting ticks (20ms each) so
* that the seek bar can show real time/remaining instead of
* a byte-ratio estimate. Saves and restores logPosition and
* the XOR key so it leaves no observable side effects.
*
* Compression makes byte position non-linear in time, and
* event density varies wildly between phases (lobby vs.
* play), so a one-shot scan is the only way to get a stable
* total. The buffer is fully decompressed before this runs,
* so it's just a byte walk.
*********************************************************/

/* Returns bytes-after-code consumed by event 'code' (excluding code byte
 * itself). For variable-length events with a pascal-string payload this
 * peeks the length byte by reading and decrypting it via lv_blocksReadBytes.
 * Returns -1 on read error / unknown event. */
static int walkSkipEventBody(BYTE code) {
  BYTE lenByte;
  int rc;
  switch (code) {
    /* Fixed-size payloads */
    case log_PlayerQuit:
    case log_LostMan:
    case log_AllyLeave:
    case log_PillSetInTank:
    case log_PlayerRejoin:
    case log_PlayerLeaving:
    case log_PlayerDied:
    case log_PlayerReady:
    case log_PlayerUnready:
    case log_MapSkipVote:
      /* 1 byte */
      { BYTE b; if (logReadBytes(&b, 1) != 1) return -1; }
      return 1;
    case log_PillSetHealth:
      /* Two bytes from v3 on — index and armour — and one before it, where
         the pair shared a byte's nibbles. This walker only ever runs on a
         v0/v1 file (v2 and later are skipped by their framed length), but
         it sizes by the version so it stays right whichever file reaches
         it. */
      if (g_lv->loadedLogVersion >= LOG_VERSION_V3) {
        BYTE b[2]; if (logReadBytes(b, 2) != 2) return -1;
        return 2;
      }
      { BYTE b; if (logReadBytes(&b, 1) != 1) return -1; }
      return 1;
    case log_AllyRequest:
    case log_AllyAccept:
    case log_KillPlayer:
    case log_TeamSet:
    case log_SoundBuild:
    case log_SoundFarm:
    case log_SoundShoot:
    case log_SoundHitTank:
    case log_SoundHitTree:
    case log_SoundHitWall:
    case log_SoundMineLay:
    case log_SoundMineExplode:
    case log_SoundExplosion:
    case log_SoundBigExplosion:
    case log_SoundManDie:
    case log_GameVoteEnd:
      { BYTE b[2]; if (logReadBytes(b, 2) != 2) return -1; }
      return 2;
    case log_MapChange:
    case log_BaseSetOwner:
    case log_PillSetOwner:
    case log_PillSetPlace:
    case log_GameVoteStart:
    case log_GameVoteCast:
      { BYTE b[3]; if (logReadBytes(b, 3) != 3) return -1; }
      return 3;
    case log_BaseSetStock:
    case log_LgmLocation:
    case log_Shell:
    case log_GameTimeSet:
    case log_ServerTick:
      /* log_ServerTick's four bytes are the server tick as a big-endian u32.
         Only a v2 log can carry one; the v1 walker is given the case anyway,
         for the reason it is given one for log_Ping. */
      { BYTE b[4]; if (logReadBytes(b, 4) != 4) return -1; }
      return 4;
    case log_PlayerLocation:
    case log_TankSetStock:
      { BYTE b[5]; if (logReadBytes(b, 5) != 5) return -1; }
      return 5;
    case log_Ping:
    case log_EntityMasks:
      /* A ping is sender + kind + two big-endian u16 coordinates; the masks
         record is three big-endian u16. Six bytes either way. Only v2 logs
         can carry either, but the v1 walker keeps a full table so a future
         re-encoder cannot silently desynchronise the cursor. */
      { BYTE b[6]; if (logReadBytes(b, 6) != 6) return -1; }
      return 6;
    case log_SaveMap:
    case log_LobbyEnter:
    case log_LobbyExit:
    case log_CountdownStart:
    case log_CountdownCancel:
    case log_BalanceApplied:
      return 0;
    /* Variable-length: pascal string trailing the fixed prefix */
    case log_PlayerJoined:
      /* 5 opt bytes + pascal string */
      { BYTE b[5]; if (logReadBytes(b, 5) != 5) return -1; }
      if (logReadBytes(&lenByte, 1) != 1) return -1;
      { BYTE buf[256]; rc = lenByte ? logReadBytes(buf, lenByte) : 0;
        if (rc != lenByte) return -1; }
      return 6 + lenByte;
    case log_ChangeName:
    case log_TankSetModifiers:
      /* 1 opt byte + pascal string (the modifier record's blob is always six
         bytes, but it is walked as a pascal string like any other) */
      { BYTE b; if (logReadBytes(&b, 1) != 1) return -1; }
      if (logReadBytes(&lenByte, 1) != 1) return -1;
      { BYTE buf[256]; rc = lenByte ? logReadBytes(buf, lenByte) : 0;
        if (rc != lenByte) return -1; }
      return 2 + lenByte;
    case log_MessageAll:
      /* 1 opt byte + pascal string */
      { BYTE b; if (logReadBytes(&b, 1) != 1) return -1; }
      if (logReadBytes(&lenByte, 1) != 1) return -1;
      { BYTE buf[256]; rc = lenByte ? logReadBytes(buf, lenByte) : 0;
        if (rc != lenByte) return -1; }
      return 2 + lenByte;
    case log_MessagePlayers:
    case log_ServerText:
    case log_RuleSet:
      /* 2 opt bytes + pascal string. The rule record's two bytes are its rule
         index and its blob is always the eight bytes of one value, but both
         are walked the way a text record's are. Only a v2 log can carry a
         rule record; the v1 walker is given the case anyway, for the reason
         it is given one for log_Ping. */
      { BYTE b[2]; if (logReadBytes(b, 2) != 2) return -1; }
      if (logReadBytes(&lenByte, 1) != 1) return -1;
      { BYTE buf[256]; rc = lenByte ? logReadBytes(buf, lenByte) : 0;
        if (rc != lenByte) return -1; }
      return 3 + lenByte;
    case log_EntityChange:
      /* kind + index + on-the-map flag + the item's record as a pascal blob.
         Only v2 logs can carry one, but the v1 walker keeps a full table for
         the reason it keeps one for log_Ping. */
      { BYTE b[3]; if (logReadBytes(b, 3) != 3) return -1; }
      if (logReadBytes(&lenByte, 1) != 1) return -1;
      { BYTE buf[256]; rc = lenByte ? logReadBytes(buf, lenByte) : 0;
        if (rc != lenByte) return -1; }
      return 4 + lenByte;
    case log_ScnAnnounce:
    case log_ScnMarker:
      /* 4 opt bytes + pascal string. The announcement's pair of them is its
         destination and the two bytes of its tick count, the marker's is its
         id, its kind and its own destination, and the marker's blob is always
         the four bytes of a placement; both are walked the way a text
         record's are. Only a v2 log can carry either; the v1 walker is given
         the cases anyway, for the reason it is given one for log_Ping.
         Note: an announcement with a position has two more bytes, across
         and down, after the string (log.c writes them only then). Nothing in
         the unframed bytes says whether they are there, so only the framed
         length can size a positioned line. This case walks the form with no
         position, which is the only form an unframed stream could hold. */
      { BYTE b[4]; if (logReadBytes(b, 4) != 4) return -1; }
      if (logReadBytes(&lenByte, 1) != 1) return -1;
      { BYTE buf[256]; rc = lenByte ? logReadBytes(buf, lenByte) : 0;
        if (rc != lenByte) return -1; }
      return 5 + lenByte;
    case log_ScnStatus:
      /* The destination pair + the countdown's end tick as four big-endian
         bytes, then the line as a pascal string. Only a v2 log can carry
         one; the v1 walker is given the case anyway, for the reason it is
         given one for log_Ping. */
      { BYTE b[6]; if (logReadBytes(b, 6) != 6) return -1; }
      if (logReadBytes(&lenByte, 1) != 1) return -1;
      { BYTE buf[256]; rc = lenByte ? logReadBytes(buf, lenByte) : 0;
        if (rc != lenByte) return -1; }
      return 7 + lenByte;
    case log_ScnScore:
      /* kind + target + the score as four big-endian bytes, then the label as
         a pascal string. */
      { BYTE b[6]; if (logReadBytes(b, 6) != 6) return -1; }
      if (logReadBytes(&lenByte, 1) != 1) return -1;
      { BYTE buf[256]; rc = lenByte ? logReadBytes(buf, lenByte) : 0;
        if (rc != lenByte) return -1; }
      return 7 + lenByte;
    case log_ScnHint:
      /* The ordered bot's slot, then the order's verb as a pascal string.
         Only a v2 log can carry one; the v1 walker is given the case anyway,
         for the reason it is given one for log_Ping. */
      { BYTE b; if (logReadBytes(&b, 1) != 1) return -1; }
      if (logReadBytes(&lenByte, 1) != 1) return -1;
      { BYTE buf[256]; rc = lenByte ? logReadBytes(buf, lenByte) : 0;
        if (rc != lenByte) return -1; }
      return 2 + lenByte;
    case log_ScnPanel: {
      /* panel id + the two destination bytes + the list's length as a
         big-endian u16, then the list. The length is two bytes rather than a
         pascal string's one because a display list runs past what one byte
         counts, so this is the one record the walker cannot size with
         lenByte. Skipped a bufferful at a time for the same reason. */
      BYTE     hdr[5];
      unsigned listLen;
      unsigned left;
      if (logReadBytes(hdr, 5) != 5) return -1;
      listLen = ((unsigned)hdr[3] << 8) | (unsigned)hdr[4];
      left = listLen;
      while (left > 0) {
        BYTE buf[256];
        int  want = (left > sizeof(buf)) ? (int)sizeof(buf) : (int)left;
        if (logReadBytes(buf, want) != want) return -1;
        left -= (unsigned)want;
      }
      return 5 + (int)listLen;
    }
    case log_MessageServer:
    case log_MapSkipApplied:
      /* pascal string only */
      if (logReadBytes(&lenByte, 1) != 1) return -1;
      { BYTE buf[256]; rc = lenByte ? logReadBytes(buf, lenByte) : 0;
        if (rc != lenByte) return -1; }
      return 1 + lenByte;
    default:
      return -1;
  }
}

/* Skip an event packet of numEvents events. Reader updates the XOR key
 * to the event's code byte after each event (matches the writer's
 * logKey rotation). Returns FALSE on read error / unknown event. */
static bool walkSkipEvents(unsigned short numEvents) {
  unsigned short i;
  BYTE code;
  /* Framing, not a single version: every version from v2 on frames its
     events, so every one of them is skipped by the framed length. */
  bool isV2 = (g_lv->loadedLogVersion >= LOG_VERSION_V2);
  for (i = 0; i < numEvents; i++) {
    if (logReadBytes(&code, 1) != 1) return FALSE;
    if (isV2) {
      /* v2: [type][u16 BE payload-length][payload]. Skip via the framed
         length; blockKey stays 0 so no key roll. */
      BYTE lenBytes[2];
      unsigned short evLen;
      if (logReadBytes(lenBytes, 2) != 2) return FALSE;
      evLen = (unsigned short)((lenBytes[0] << 8) | lenBytes[1]);
      lv_logSetPosition(lv_logGetCurrentPosition() + evLen);
    } else {
      if (walkSkipEventBody(code) < 0) return FALSE;
      lv_blocksSetKey(code);
    }
  }
  return TRUE;
}

/* Skip a snapshot block. Mirrors lv_processSnapshot's read order without
 * applying any state. When baseX/baseY/numBases are non-NULL and the snapshot
 * carries a base table, the bases' map cells are copied out in blob order —
 * the same 0-based index a log_BaseSetOwner event carries. A snapshot with no
 * bases (any lobby-phase snapshot: the game world doesn't exist yet) leaves
 * the outputs untouched, so a previously extracted table survives. When
 * numRuns is non-NULL it reports how many non-terminator map runs the body
 * carried — none for a lobby snapshot, at least one for a running round. */
static bool walkSkipSnapshotBases(uint8_t *baseX, uint8_t *baseY,
                                  int *numBases, int *numRuns) {
  BYTE buf[512];
  BYTE dlen;
  int32_t hdr;
  BYTE runHead[SIZEOFBMAP_RUN_HEADER];
  int i;
  int runs = 0;

  /* gmeStartDelay + gmeLength */
  if (logReadBytes((BYTE *)&hdr, (int)sizeof(int32_t)) != (int)sizeof(int32_t)) return FALSE;
  if (logReadBytes((BYTE *)&hdr, (int)sizeof(int32_t)) != (int)sizeof(int32_t)) return FALSE;

  /* pills, bases, starts: 1-byte len + len bytes (BYTE max 255 fits in buf) */
  for (i = 0; i < 3; i++) {
    if (logReadBytes(&dlen, 1) != 1) return FALSE;
    if (dlen > 0 && logReadBytes(buf, dlen) != dlen) return FALSE;
    if (i == 1 && numBases != NULL && dlen > 0 && buf[0] > 0) {
      /* Bases blob: [numBases] then 10 bytes per base — x, y, owner, armour,
       * shells, mines, refuelTime, baseTime(2), justStopped (the layout
       * lv_basesSetBaseNetData consumes). Only the cells are kept. */
      int n = buf[0] > MAX_BASES ? MAX_BASES : buf[0];
      int k;
      for (k = 0; k < n; k++) {
        int off = 1 + k * 10;
        if (off + 1 >= (int)dlen) { n = k; break; }
        baseX[k] = buf[off];
        baseY[k] = buf[off + 1];
      }
      *numBases = n;
    }
  }

  /* Map runs: 4-byte header repeating until terminator
   * (datalen==4, y==255, startx==255, endx==255). Non-terminator runs
   * are followed by (datalen - 4) data bytes. */
  for (;;) {
    if (logReadBytes(runHead, SIZEOFBMAP_RUN_HEADER) != SIZEOFBMAP_RUN_HEADER) return FALSE;
    /* Layout: datalen, y, startx, endx (all BYTE per bmapRunHeader) */
    if (runHead[0] == SIZEOFBMAP_RUN_HEADER && runHead[1] == MAP_ARRAY_LAST
        && runHead[2] == MAP_ARRAY_LAST && runHead[3] == MAP_ARRAY_LAST) {
      break;
    }
    {
      int dataBytes = (int)runHead[0] - SIZEOFBMAP_RUN_HEADER;
      if (dataBytes < 0) return FALSE;
      while (dataBytes > 0) {
        int chunk = dataBytes > (int)sizeof(buf) ? (int)sizeof(buf) : dataBytes;
        if (logReadBytes(buf, chunk) != chunk) return FALSE;
        dataBytes -= chunk;
      }
    }
    runs++;
  }
  if (numRuns != NULL) *numRuns = runs;

  /* MAX_TANKS player records: 1-byte len + len bytes (BYTE max 255 fits in buf) */
  for (i = 0; i < MAX_TANKS; i++) {
    if (logReadBytes(&dlen, 1) != 1) return FALSE;
    if (dlen > 0 && logReadBytes(buf, dlen) != dlen) return FALSE;
  }
  return TRUE;
}

static bool walkSkipSnapshot(void) {
  return walkSkipSnapshotBases(NULL, NULL, NULL, NULL);
}

/* Walk the log buffer from the current position to LOG_QUIT/EOF, counting
 * 20ms ticks. Saves and restores logPosition + XOR key. */
static uint32_t lv_walkComputeTotalTimeMs(void) {
  size_t   savedPos = lv_logGetCurrentPosition();
  BYTE     savedKey = lv_blocksGetKey();
  uint64_t ticks    = 0;
  bool     done     = FALSE;
  BYTE     code;
  BYTE     b1, b2;
  unsigned short waitLen, numEvents;
  uint16_t us;

  while (!done && !lv_blocksIsEOF()) {
    if (logReadBytes(&code, 1) != 1) break;
    switch (code) {
      case LOG_QUIT:
        ticks++;
        done = TRUE;
        break;
      case LOG_SNAPSHOT:
        if (!walkSkipSnapshot()) { done = TRUE; break; }
        ticks++;
        break;
      case LOG_NOEVENTS:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        waitLen = b1 == 0 ? 1 : b1;
        ticks += 1 + (uint64_t)waitLen;
        break;
      case LOG_NOEVENTS_LONG:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        if (logReadBytes(&b2, 1) != 1) { done = TRUE; break; }
        us = (uint16_t)((b1 << 8) | b2);
        waitLen = ntohs(us);
        if (waitLen == 0) waitLen = 1;
        ticks += 1 + (uint64_t)waitLen;
        break;
      case LOG_EVENT:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        numEvents = b1;
        if (!walkSkipEvents(numEvents)) { done = TRUE; break; }
        ticks++;
        break;
      case LOG_EVENT_LONG:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        if (logReadBytes(&b2, 1) != 1) { done = TRUE; break; }
        us = (uint16_t)((b1 << 8) | b2);
        numEvents = ntohs(us);
        if (!walkSkipEvents(numEvents)) { done = TRUE; break; }
        ticks++;
        break;
      default:
        done = TRUE;
        break;
    }
  }

  lv_logSetPosition(savedPos);
  lv_blocksSetKey(savedKey);
  return (uint32_t)(ticks * 20);
}

/* Playback time (ms) of the world rewrite that ends the lobby — the round's
 * first real frame — computed once at load. 0 when the log has no lobby, so
 * the round runs from tick 0. */
static uint32_t s_gameStartMs = 0;

/* Every name each slot carried this round, collected once at load from the
 * log's own join events. Unlike the live roster this never forgets a player
 * who joined mid-round or quit before the end. */
static char s_slotNames[MAX_TANKS][PLAYER_NAME_LEN];

/* Presentation window: while set, the viewer reports times and seeks against
 * [gameStart, totalTime) instead of the whole file, so a log that sat hours in
 * the lobby reads on the round's own clock. Absolute ms is still what the
 * decoder, the snapshot index and the highlight clips use. */
static bool s_hideLobby = TRUE;

/* Event-stream start position (just past the opening snapshot), recorded at
 * load so later walks (the calibration anchor scan) can re-enter the stream
 * from a known-good position instead of trusting the caller's current one. */
static size_t s_walkStartPos = 0;

/* Walk the log from the current position to LOG_QUIT/EOF counting 20ms ticks,
 * and return the playback time in ms of the first snapshot whose body carries
 * a world — the rewrite the recorder emits when the lobby ends, which is the
 * round's first real frame. Returns 0 when the opening snapshot the loader
 * consumed already carried a world (no lobby ran, so the round starts at tick
 * 0 and every snapshot left in the stream is a periodic in-game resync), and
 * 0 if no such snapshot is reached. The tick accounting mirrors
 * lv_walkComputeTotalTimeMs, so the result is comparable to the decoder's
 * timeRunning; must be entered at the event-stream start (as at load). Saves
 * and restores logPosition + XOR key. */
static uint32_t lv_walkComputeGameStartMs(void) {
  size_t   savedPos;
  BYTE     savedKey;
  uint64_t ticks    = 0;
  bool     done     = FALSE;
  BYTE     code, b1, b2;
  unsigned short waitLen, numEvents;
  uint16_t us;
  uint32_t result = 0;
  int      numRuns;

  if (s_openingSnapshotHadWorld == TRUE) return 0;

  savedPos = lv_logGetCurrentPosition();
  savedKey = lv_blocksGetKey();

  while (!done && !lv_blocksIsEOF()) {
    if (logReadBytes(&code, 1) != 1) break;
    switch (code) {
      case LOG_QUIT:
        done = TRUE;
        break;
      case LOG_SNAPSHOT:
        numRuns = 0;
        if (!walkSkipSnapshotBases(NULL, NULL, NULL, &numRuns)) {
          done = TRUE;
          break;
        }
        ticks++;
        if (numRuns > 0) {
          result = (uint32_t)(ticks * 20);
          done = TRUE;
        }
        break;
      case LOG_NOEVENTS:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        waitLen = b1 == 0 ? 1 : b1;
        ticks += 1 + (uint64_t)waitLen;
        break;
      case LOG_NOEVENTS_LONG:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        if (logReadBytes(&b2, 1) != 1) { done = TRUE; break; }
        us = (uint16_t)((b1 << 8) | b2);
        waitLen = ntohs(us);
        if (waitLen == 0) waitLen = 1;
        ticks += 1 + (uint64_t)waitLen;
        break;
      case LOG_EVENT:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        numEvents = b1;
        if (!walkSkipEvents(numEvents)) { done = TRUE; break; }
        ticks++;
        break;
      case LOG_EVENT_LONG:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        if (logReadBytes(&b2, 1) != 1) { done = TRUE; break; }
        us = (uint16_t)((b1 << 8) | b2);
        numEvents = ntohs(us);
        if (!walkSkipEvents(numEvents)) { done = TRUE; break; }
        ticks++;
        break;
      default:
        done = TRUE;
        break;
    }
  }

  lv_logSetPosition(savedPos);
  lv_blocksSetKey(savedKey);
  return result;
}

/* Read the slot and name out of a log_PlayerJoined (5 opt bytes) or
 * log_ChangeName (1 opt byte) payload and record it in s_slotNames. Entered
 * with the reader just past the event's code byte — and, on v2, past the
 * framed length — and consumes exactly the bytes the matching
 * walkSkipEventBody case would. Last name wins, so a rename replaces the
 * earlier one and a reused slot ends up holding its most recent occupant.
 * Returns FALSE on a short read. */
static bool walkReadSlotName(BYTE code) {
  BYTE opt[5];
  int  optCount = (code == log_PlayerJoined) ? 5 : 1;
  char pstr[256];   /* [len][chars]; `pascal` is an MSVC keyword */
  char name[256];
  int  len;

  if (logReadBytes(opt, optCount) != optCount) return FALSE;
  if (logReadBytes((BYTE *)pstr, 1) != 1) return FALSE;
  len = (unsigned char)pstr[0];
  if (len > 0 && logReadBytes((BYTE *)pstr + 1, len) != len) return FALSE;
  lv_utilPtoCString(pstr, name);
  if (opt[0] < MAX_TANKS && name[0] != '\0') {
    snprintf(s_slotNames[opt[0]], sizeof(s_slotNames[opt[0]]), "%s", name);
  }
  return TRUE;
}

/* Read a log_GameSettings payload — a length byte then that many bytes — into
 * the settings store. Entered with the reader just past the framed length.
 * The last one in the file wins, and the walk keeps going, because a lobby
 * edit writes another event and no settings event follows the round start.
 * Returns FALSE on a short read. */
static bool walkReadGameSettings(void) {
  BYTE blob[LV_GAME_SETTINGS_MAX];
  int  len;

  if (logReadBytes(blob, 1) != 1) return FALSE;
  len = blob[0];
  if (len > 0 && logReadBytes(blob, len) != len) return FALSE;
  lv_screenStoreGameSettings(blob, len);
  return TRUE;
}

/* Like walkSkipEvents, but decodes the two events that carry a slot name and
 * the lobby settings; every other event is skipped by the shared helper. */
static bool walkScanNames(unsigned short numEvents) {
  unsigned short i;
  BYTE code;
  /* Framing again, so v2 and every version after it take this arm. */
  bool isV2 = (g_lv->loadedLogVersion >= LOG_VERSION_V2);
  for (i = 0; i < numEvents; i++) {
    bool named;
    if (logReadBytes(&code, 1) != 1) return FALSE;
    named = (code == log_PlayerJoined || code == log_ChangeName);
    if (isV2) {
      /* v2: [type][u16 BE payload-length][payload]. Read what we need, then
         land on the framed end regardless; blockKey stays 0 so no key roll. */
      BYTE lenBytes[2];
      unsigned short evLen;
      size_t payloadPos;
      if (logReadBytes(lenBytes, 2) != 2) return FALSE;
      evLen = (unsigned short)((lenBytes[0] << 8) | lenBytes[1]);
      payloadPos = lv_logGetCurrentPosition();
      if (named && !walkReadSlotName(code)) return FALSE;
      /* Only the v2 arm looks for settings: the event postdates v0 and v1,
         so no file the arm below reads can contain one. */
      if (code == log_GameSettings && !walkReadGameSettings()) return FALSE;
      lv_logSetPosition(payloadPos + evLen);
    } else {
      if (named) {
        if (!walkReadSlotName(code)) return FALSE;
      } else if (walkSkipEventBody(code) < 0) {
        return FALSE;
      }
      lv_blocksSetKey(code);
    }
  }
  return TRUE;
}

/* Walk the log from the event-stream start to LOG_QUIT/EOF, recording the name
 * each join or rename event gives a slot. Mirrors lv_walkComputeGameStartMs;
 * must be entered at load, while the reader's XOR key still matches the stream
 * start. Saves and restores logPosition + XOR key. */
static void lv_walkCollectSlotNames(void) {
  size_t savedPos = lv_logGetCurrentPosition();
  BYTE   savedKey = lv_blocksGetKey();
  bool   done     = FALSE;
  BYTE   code, b1, b2;
  unsigned short numEvents;
  uint16_t us;

  lv_logSetPosition(s_walkStartPos);

  while (!done && !lv_blocksIsEOF()) {
    if (logReadBytes(&code, 1) != 1) break;
    switch (code) {
      case LOG_QUIT:
        done = TRUE;
        break;
      case LOG_SNAPSHOT:
        if (!walkSkipSnapshot()) done = TRUE;
        break;
      case LOG_NOEVENTS:
        if (logReadBytes(&b1, 1) != 1) done = TRUE;
        break;
      case LOG_NOEVENTS_LONG:
        if (logReadBytes(&b1, 1) != 1 || logReadBytes(&b2, 1) != 1) done = TRUE;
        break;
      case LOG_EVENT:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        numEvents = b1;
        if (!walkScanNames(numEvents)) done = TRUE;
        break;
      case LOG_EVENT_LONG:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        if (logReadBytes(&b2, 1) != 1) { done = TRUE; break; }
        us = (uint16_t)((b1 << 8) | b2);
        numEvents = ntohs(us);
        if (!walkScanNames(numEvents)) done = TRUE;
        break;
      default:
        done = TRUE;
        break;
    }
  }

  lv_logSetPosition(savedPos);
  lv_blocksSetKey(savedKey);
  /* The whole stream has been read: whatever settings the store holds now are
     the last the file carries, and playback must not put an earlier set back
     in their place. */
  s_gameSettingsWalked = TRUE;
}

/* Read a log_RuleSet payload and add it to the change list at ms when it
 * decodes. Entered with the reader just past the event's code byte — and, on
 * v2, past the framed length — and consumes exactly the bytes the matching
 * walkSkipEventBody case would, whether or not the record decodes. Returns
 * FALSE on a short read. */
static bool walkReadRuleSet(uint32_t ms) {
  BYTE   idx[2];
  BYTE   len;
  BYTE   blob[256];
  int    index;
  double value;

  if (logReadBytes(idx, 2) != 2) return FALSE;
  if (logReadBytes(&len, 1) != 1) return FALSE;
  if (len > 0 && logReadBytes(blob, len) != len) return FALSE;
  if (lv_ruleSetDecode(idx[0], idx[1], len, blob, &index, &value)) {
    lv_ruleChangeAppend(ms, index, value, FALSE);
  }
  return TRUE;
}

/* Like walkScanNames, but reads the rule changes in one event packet, all of
 * which land at ms; every other event is skipped by the shared helper. */
static bool walkScanRuleSets(unsigned short numEvents, uint32_t ms) {
  unsigned short i;
  BYTE code;
  bool isV2 = (g_lv->loadedLogVersion >= LOG_VERSION_V2);
  for (i = 0; i < numEvents; i++) {
    if (logReadBytes(&code, 1) != 1) return FALSE;
    if (isV2) {
      BYTE lenBytes[2];
      unsigned short evLen;
      size_t payloadPos;
      if (logReadBytes(lenBytes, 2) != 2) return FALSE;
      evLen = (unsigned short)((lenBytes[0] << 8) | lenBytes[1]);
      payloadPos = lv_logGetCurrentPosition();
      if (code == log_RuleSet && !walkReadRuleSet(ms)) return FALSE;
      lv_logSetPosition(payloadPos + evLen);
    } else {
      if (code == log_RuleSet) {
        if (!walkReadRuleSet(ms)) return FALSE;
      } else if (walkSkipEventBody(code) < 0) {
        return FALSE;
      }
      lv_blocksSetKey(code);
    }
  }
  return TRUE;
}

/* Walk the log from the event-stream start to LOG_QUIT/EOF, collecting every
 * log_RuleSet into g_lv->ruleChanges at the playback time the decoder will
 * reach it. The viewer's snapshots carry no rules, so a seek back could not
 * otherwise put an earlier value back. The tick accounting mirrors
 * lv_walkComputeTotalTimeMs: an event packet read on the tick that takes
 * playback to (ticks + 1) * 20 ms is applied at that time. Must be entered at
 * load, while the reader's XOR key still matches the stream start. Saves and
 * restores logPosition + XOR key. */
static void lv_walkCollectRuleChanges(void) {
  size_t   savedPos = lv_logGetCurrentPosition();
  BYTE     savedKey = lv_blocksGetKey();
  uint64_t ticks    = 0;
  bool     done     = FALSE;
  BYTE     code, b1, b2;
  unsigned short waitLen, numEvents;
  uint16_t us;

  lv_ruleChangesClear();
  lv_logSetPosition(s_walkStartPos);

  while (!done && !lv_blocksIsEOF()) {
    if (logReadBytes(&code, 1) != 1) break;
    switch (code) {
      case LOG_QUIT:
        done = TRUE;
        break;
      case LOG_SNAPSHOT:
        if (!walkSkipSnapshot()) { done = TRUE; break; }
        ticks++;
        break;
      case LOG_NOEVENTS:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        waitLen = b1 == 0 ? 1 : b1;
        ticks += 1 + (uint64_t)waitLen;
        break;
      case LOG_NOEVENTS_LONG:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        if (logReadBytes(&b2, 1) != 1) { done = TRUE; break; }
        us = (uint16_t)((b1 << 8) | b2);
        waitLen = ntohs(us);
        if (waitLen == 0) waitLen = 1;
        ticks += 1 + (uint64_t)waitLen;
        break;
      case LOG_EVENT:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        numEvents = b1;
        if (!walkScanRuleSets(numEvents, (uint32_t)((ticks + 1) * 20))) {
          done = TRUE;
          break;
        }
        ticks++;
        break;
      case LOG_EVENT_LONG:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        if (logReadBytes(&b2, 1) != 1) { done = TRUE; break; }
        us = (uint16_t)((b1 << 8) | b2);
        numEvents = ntohs(us);
        if (!walkScanRuleSets(numEvents, (uint32_t)((ticks + 1) * 20))) {
          done = TRUE;
          break;
        }
        ticks++;
        break;
      default:
        done = TRUE;
        break;
    }
  }

  lv_logSetPosition(savedPos);
  lv_blocksSetKey(savedKey);
}

/* The index could not take a record. Says so once per recording: a seek
 * past the last record it holds shows the stores as that record left them. */
static void lv_presIndexTruncate(void) {
  if (!s_presIndexTruncated) {
    WB_LOG_WARN(WB_LOG_CAT_LOGVIEWER,
                "scenario display index is full at %d records; a seek past "
                "the last of them shows older panels, scores and markers",
                s_presIndexCount);
  }
  s_presIndexTruncated = TRUE;
}

/* Add a record to the presentation index, growing it as needed. A full
 * index, or one that cannot grow, keeps what it has and sets the truncated
 * flag. */
static void lv_presIndexAdd(uint32_t ms, BYTE code, int key,
                            size_t payloadPos, unsigned short frameLen) {
  if (s_presIndexCount >= s_presIndexCap) {
    int           cap = (s_presIndexCap == 0) ? 256 : s_presIndexCap * 2;
    LvPresRecord *grown;

    if (cap > LV_PRES_INDEX_MAX) {
      cap = LV_PRES_INDEX_MAX;
    }
    if (cap <= s_presIndexCount) {
      lv_presIndexTruncate();
      return;
    }
    grown = (LvPresRecord *)realloc(s_presIndex, (size_t)cap * sizeof(*grown));
    if (grown == NULL) {
      lv_presIndexTruncate();
      return;
    }
    s_presIndex    = grown;
    s_presIndexCap = cap;
  }
  s_presIndex[s_presIndexCount].ms         = ms;
  s_presIndex[s_presIndexCount].code       = code;
  s_presIndex[s_presIndexCount].frameLen   = frameLen;
  s_presIndex[s_presIndexCount].key        = (BYTE)key;
  s_presIndex[s_presIndexCount].payloadPos = payloadPos;
  s_presIndexCount++;
}

/* Key first, then file order: a payload's position only ever grows through
   the file, so it keeps each key's records in the order playback meets
   them. */
static int lv_presRecordCompare(const void *a, const void *b) {
  const LvPresRecord *ra = (const LvPresRecord *)a;
  const LvPresRecord *rb = (const LvPresRecord *)b;

  if (ra->key != rb->key) {
    return (ra->key < rb->key) ? -1 : 1;
  }
  if (ra->payloadPos != rb->payloadPos) {
    return (ra->payloadPos < rb->payloadPos) ? -1 : 1;
  }
  return 0;
}

/* Sort the finished index by key and note where each key's run starts. */
static void lv_presIndexByKey(void) {
  int i;
  int k = 0;

  if (s_presIndexCount > 1) {
    qsort(s_presIndex, (size_t)s_presIndexCount, sizeof(*s_presIndex),
          lv_presRecordCompare);
  }
  for (i = 0; i < s_presIndexCount; i++) {
    while (k <= (int)s_presIndex[i].key) {
      s_presKeyStart[k++] = i;
    }
  }
  while (k <= LV_PRES_KEYS) {
    s_presKeyStart[k++] = s_presIndexCount;
  }
}

/* Like walkScanRuleSets, but reads each panel, score, announcement, marker
 * and team record, puts it through the checks playback makes and indexes the
 * ones that pass, all at ms. Framed files only. */
static bool walkScanPresentation(unsigned short numEvents, uint32_t ms) {
  unsigned short i;
  BYTE code;
  for (i = 0; i < numEvents; i++) {
    BYTE           lenBytes[2];
    unsigned short evLen;
    size_t         payloadPos;
    if (logReadBytes(&code, 1) != 1) return FALSE;
    if (logReadBytes(lenBytes, 2) != 2) return FALSE;
    evLen = (unsigned short)((lenBytes[0] << 8) | lenBytes[1]);
    payloadPos = lv_logGetCurrentPosition();
    if (code == log_ScnPanel || code == log_ScnScore ||
        code == log_ScnAnnounce || code == log_ScnMarker ||
        code == log_ScnStatus || code == log_TeamSet) {
      int key;
      lv_presReadPayload(code, evLen, &s_presPayload);
      key = lv_presCheck(&s_presPayload);
      if (key >= 0) {
        lv_presIndexAdd(ms, code, key, payloadPos, evLen);
      }
    }
    lv_logSetPosition(payloadPos + evLen);
  }
  return TRUE;
}

/* Walk the log from the event-stream start to LOG_QUIT/EOF, indexing every
 * log_ScnPanel, log_ScnScore, log_ScnAnnounce, log_ScnMarker and log_TeamSet
 * that passes its checks, at the playback time the decoder will reach it,
 * stamped the way lv_walkCollectRuleChanges stamps a rule change. Only a
 * framed file can carry the scenario records, so an older file is left with
 * an empty index; a v1 log's log_TeamSet records go unindexed with them,
 * which costs nothing because such a log has no panels. Must be entered at
 * load. Saves and restores logPosition + XOR key. */
static void lv_walkCollectPresentation(void) {
  size_t   savedPos = lv_logGetCurrentPosition();
  BYTE     savedKey = lv_blocksGetKey();
  uint64_t ticks    = 0;
  bool     done     = FALSE;
  BYTE     code, b1, b2;
  unsigned short waitLen, numEvents;
  uint16_t us;

  lv_presReset();
  s_presWalked = TRUE;
  if (g_lv->loadedLogVersion < LOG_VERSION_V2) {
    return;
  }
  lv_logSetPosition(s_walkStartPos);
  lv_blocksSetKey(0);   /* framed files are plaintext */

  while (!done && !lv_blocksIsEOF()) {
    if (logReadBytes(&code, 1) != 1) break;
    switch (code) {
      case LOG_QUIT:
        done = TRUE;
        break;
      case LOG_SNAPSHOT:
        if (!walkSkipSnapshot()) { done = TRUE; break; }
        ticks++;
        break;
      case LOG_NOEVENTS:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        waitLen = b1 == 0 ? 1 : b1;
        ticks += 1 + (uint64_t)waitLen;
        break;
      case LOG_NOEVENTS_LONG:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        if (logReadBytes(&b2, 1) != 1) { done = TRUE; break; }
        us = (uint16_t)((b1 << 8) | b2);
        waitLen = ntohs(us);
        if (waitLen == 0) waitLen = 1;
        ticks += 1 + (uint64_t)waitLen;
        break;
      case LOG_EVENT:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        numEvents = b1;
        if (!walkScanPresentation(numEvents, (uint32_t)((ticks + 1) * 20))) {
          done = TRUE;
          break;
        }
        ticks++;
        break;
      case LOG_EVENT_LONG:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        if (logReadBytes(&b2, 1) != 1) { done = TRUE; break; }
        us = (uint16_t)((b1 << 8) | b2);
        numEvents = ntohs(us);
        if (!walkScanPresentation(numEvents, (uint32_t)((ticks + 1) * 20))) {
          done = TRUE;
          break;
        }
        ticks++;
        break;
      default:
        done = TRUE;
        break;
    }
  }
  lv_presIndexByKey();

  lv_logSetPosition(savedPos);
  lv_blocksSetKey(savedKey);
}

/* Read one event block for what the server-tick count needs: whether it holds
 * log_EntityMasks, and the first log_ServerTick in it whose length is four.
 * Every event is skipped by its framed length. Framed files only. */
static bool walkScanServerTicks(unsigned short numEvents, bool *masks,
                                bool *haveTick, uint32_t *tick) {
  unsigned short i;
  BYTE code;
  for (i = 0; i < numEvents; i++) {
    BYTE           lenBytes[2];
    unsigned short evLen;
    size_t         payloadPos;
    if (logReadBytes(&code, 1) != 1) return FALSE;
    if (logReadBytes(lenBytes, 2) != 2) return FALSE;
    evLen = (unsigned short)((lenBytes[0] << 8) | lenBytes[1]);
    payloadPos = lv_logGetCurrentPosition();
    if (code == log_EntityMasks) {
      *masks = TRUE;
    } else if (code == log_ServerTick && evLen == 4 && !*haveTick) {
      BYTE t[4];
      if (logReadBytes(t, 4) != 4) return FALSE;
      *tick = ((uint32_t)t[0] << 24) | ((uint32_t)t[1] << 16) |
              ((uint32_t)t[2] << 8)  | (uint32_t)t[3];
      *haveTick = TRUE;
    }
    lv_logSetPosition(payloadPos + evLen);
  }
  return TRUE;
}

/* Walk the log from the event-stream start to LOG_QUIT/EOF, counting writer
 * ticks and collecting every log_ServerTick as an anchor, at the playback
 * time the decoder reaches each record, stamped the way
 * lv_walkCollectRuleChanges stamps a rule change. The counting rules are the
 * server-tick section's, above lv_screenProcessLog. Only a framed file can
 * carry the record, so an older file is left with no anchors. Must be entered
 * at load. Saves and restores logPosition + XOR key. */
static void lv_walkCollectServerTicks(void) {
  size_t   savedPos = lv_logGetCurrentPosition();
  BYTE     savedKey = lv_blocksGetKey();
  uint64_t ticks    = 0;
  bool     done     = FALSE;
  BYTE     code, b1, b2;
  unsigned short waitLen, numEvents;
  uint16_t us;

  lv_serverTickReset();
  s_wtWalked = TRUE;
  if (g_lv->loadedLogVersion < LOG_VERSION_V2) {
    return;
  }
  lv_logSetPosition(s_walkStartPos);
  lv_blocksSetKey(0);   /* framed files are plaintext */

  while (!done && !lv_blocksIsEOF()) {
    uint32_t ms = (uint32_t)((ticks + 1) * LV_WT_STEP_MS);
    if (logReadBytes(&code, 1) != 1) break;
    switch (code) {
      case LOG_QUIT:
        done = TRUE;
        break;
      case LOG_SNAPSHOT:
        if (!walkSkipSnapshot()) { done = TRUE; break; }
        lv_wtCountFileSnapshot();
        ticks++;
        break;
      case LOG_NOEVENTS:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        waitLen = b1 == 0 ? 1 : b1;
        lv_wtCountWait(ms, waitLen);
        ticks += 1 + (uint64_t)waitLen;
        break;
      case LOG_NOEVENTS_LONG:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        if (logReadBytes(&b2, 1) != 1) { done = TRUE; break; }
        us = (uint16_t)((b1 << 8) | b2);
        waitLen = ntohs(us);
        if (waitLen == 0) waitLen = 1;
        lv_wtCountWait(ms, waitLen);
        ticks += 1 + (uint64_t)waitLen;
        break;
      case LOG_EVENT:
      case LOG_EVENT_LONG: {
        bool     masks    = FALSE;
        bool     haveTick = FALSE;
        uint32_t tick     = 0;
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        if (code == LOG_EVENT_LONG) {
          if (logReadBytes(&b2, 1) != 1) { done = TRUE; break; }
          us = (uint16_t)((b1 << 8) | b2);
          numEvents = ntohs(us);
        } else {
          numEvents = b1;
        }
        if (!walkScanServerTicks(numEvents, &masks, &haveTick, &tick)) {
          done = TRUE;
          break;
        }
        if (masks && !haveTick) {
          /* The entity-mask block: no tick of its own. */
          s_wtUndoValid = FALSE;
        } else {
          lv_wtCountTick(ms, !haveTick);
        }
        if (haveTick) {
          lv_tickAnchorAdd(ms, tick, TRUE);
        }
        ticks++;
        break;
      }
      default:
        done = TRUE;
        break;
    }
  }

  lv_logSetPosition(savedPos);
  lv_blocksSetKey(savedKey);
}

/* Put the stores back to what the records up to t left there: clear them,
 * then for each key apply the last of its indexed records at or before t,
 * read from its own payload and stored with the time the index gives it,
 * which is the time playback would have stored. A binary search over each
 * key's run finds it, so the cost follows the number of keys and not the
 * number of records. The index holds only records that passed their checks,
 * so none is checked again. On a feed the index is what playback has read so
 * far, sorted by key here when records were added since the last sort; a
 * feed that has indexed nothing is left alone. Saves and restores
 * logPosition + XOR key. */
static void lv_presRebuild(uint32_t t) {
  size_t savedPos;
  BYTE   savedKey;
  int    k;

  if (g_lv == NULL || (s_presWalked == FALSE && s_presLive == FALSE)) {
    return;
  }
  if (s_presIndexUnsorted) {
    lv_presIndexByKey();
    s_presIndexUnsorted = FALSE;
  }
  lv_presClearStores();
  if (s_presIndexCount == 0) {
    return;
  }
  savedPos = lv_logGetCurrentPosition();
  savedKey = lv_blocksGetKey();
  lv_blocksSetKey(0);   /* the index only holds framed, plaintext files */
  for (k = 0; k < LV_PRES_KEYS; k++) {
    const LvPresRecord *e;
    int                 lo = s_presKeyStart[k];
    int                 hi = s_presKeyStart[k + 1];

    /* The first of this key's records past t. Its run is in file order, so
       its times never go down. */
    while (lo < hi) {
      int mid = lo + (hi - lo) / 2;
      if (s_presIndex[mid].ms <= t) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    if (lo == s_presKeyStart[k]) {
      continue;   /* none at or before t */
    }
    e = &s_presIndex[lo - 1];
    lv_logSetPosition(e->payloadPos);
    lv_presReadPayload(e->code, e->frameLen, &s_presPayload);
    lv_presApply(&s_presPayload, e->key, e->ms);
  }
  lv_logSetPosition(savedPos);
  lv_blocksSetKey(savedKey);
}

/* Log time (ms) at which the round started (the lobby's world rewrite); 0 if
 * no lobby. */
uint32_t lv_screenGameStartMs(void) { return s_gameStartMs; }

static void lv_screenSeekToAbsoluteMs(uint32_t targetTime);

/* Start of the presented window in absolute log ms; 0 when the window is the
 * whole file (feature off, no lobby marker, degenerate log, or a live feed). */
static uint32_t lv_windowStartMs(void) {
  if (s_hideLobby == FALSE) return 0;
  if (lv_screenSpecIsLiveMode()) return 0;
  if (g_lv == NULL) return 0;
  if (s_gameStartMs == 0 || s_gameStartMs >= g_lv->totalTimeMs) return 0;
  return s_gameStartMs;
}

/* Length of the presented window in ms; 0 when nothing is loaded. */
static uint32_t lv_windowLenMs(void) {
  uint32_t start = lv_windowStartMs();
  if (g_lv == NULL || g_lv->totalTimeMs <= start) return 0;
  return g_lv->totalTimeMs - start;
}

/* With the lobby hidden, a freshly loaded log opens at game start so 00:00, the
 * first rendered frame and Play all agree. No-op when the window is the whole
 * file. */
static void lv_screenParkAtWindowStart(void) {
  uint32_t start = lv_windowStartMs();
  if (start > 0) {
    lv_screenSeekToAbsoluteMs(start);
  }
}

/* Scan one framed LOG_EVENT frame for base-ownership gains, counting matches of
 * (cell, owner) for the two anchors and latching each anchor's time when its
 * ordinal is reached. Base positions are static per round, so the cell is
 * resolved from the base index via the caller-maintained table (parsed from
 * the log's own snapshots — the loaded lobby snapshot has no bases, so
 * g_lv->bs cannot be used here). */
static bool walkScanBaseOwners(unsigned short numEvents, uint32_t frameMs,
                               const uint8_t *baseX, const uint8_t *baseY,
                               int numBases,
                               uint8_t xE, uint8_t yE, uint8_t ownerE, int ordE,
                               uint8_t xL, uint8_t yL, uint8_t ownerL, int ordL,
                               int *cntE, uint32_t *msE, bool *haveE,
                               int *cntL, uint32_t *msL, bool *haveL) {
  unsigned short i;
  for (i = 0; i < numEvents; i++) {
    BYTE code, lenBytes[2];
    unsigned short evLen;
    size_t payloadStart;
    if (logReadBytes(&code, 1) != 1) return FALSE;
    if (logReadBytes(lenBytes, 2) != 2) return FALSE;
    evLen = (unsigned short)((lenBytes[0] << 8) | lenBytes[1]);
    payloadStart = lv_logGetCurrentPosition();
    if (code == log_BaseSetOwner && evLen >= 2) {
      BYTE idx, owner;
      if (logReadBytes(&idx, 1) == 1 && logReadBytes(&owner, 1) == 1 &&
          owner < MAX_TANKS && idx < numBases) {
        uint8_t bx = baseX[idx], by = baseY[idx];
        if (!*haveE && bx == xE && by == yE && owner == ownerE &&
            ++(*cntE) == ordE) { *msE = frameMs; *haveE = true; }
        if (!*haveL && bx == xL && by == yL && owner == ownerL &&
            ++(*cntL) == ordL) { *msL = frameMs; *haveL = true; }
      }
    }
    lv_logSetPosition(payloadStart + evLen);
  }
  return TRUE;
}

/* Calibration anchors: return the playback ms of two base-ownership gains,
 * each identified as the ordinal-th gain at cell (x,y) by `owner`. Pairs the
 * attribution track's first/last base captures to their real scrubber times so
 * the tick->ms line can be fitted. Ordinal + owner matching pins the exact
 * event: allied captures are recorded in the track too (ATTR_CAP_ALLY), so
 * before game over every owner<MAX_TANKS gain has a matching capture record —
 * but the game-over handover re-assigns every base to the winner with no
 * record, so "last gain at this cell" can be a later event than the track's
 * last capture (observed inflating the fitted slope ~10%). v2 and later only
 * (framed events); false otherwise or if either anchor is missing. Walks from
 * the recorded event-stream start; saves and restores position + key.
 *
 * Base index -> cell resolution comes from the log's own snapshots: the walk
 * is seeded from g_lv->bs (covers a no-lobby log whose only base table is the
 * opening snapshot, consumed before the stream start) and updated from every
 * snapshot it passes. A lobby-started log's opening snapshot has NO bases —
 * the game world doesn't exist yet — so the table only appears in the first
 * in-game snapshot, which the walk reaches before any capture event can. */
bool lv_walkFindBaseOwnerTimes(uint8_t xE, uint8_t yE, uint8_t ownerE, int ordE,
                               uint8_t xL, uint8_t yL, uint8_t ownerL, int ordL,
                               uint32_t *outMsE, uint32_t *outMsL) {
  size_t   savedPos;
  BYTE     savedKey;
  uint64_t ticks = 0;
  bool     done = FALSE, haveE = FALSE, haveL = FALSE;
  uint32_t msE = 0, msL = 0;
  int      cntE = 0, cntL = 0;
  BYTE     code, b1, b2;
  unsigned short waitLen, numEvents;
  uint16_t us;
  uint8_t  baseX[MAX_BASES], baseY[MAX_BASES];
  int      numBases = 0;
  int      i;

  if (ordE <= 0 || ordL <= 0) return FALSE;

  /* Framed events only: walkScanBaseOwners steps by the framed length, so
     v2 and every version after it, and nothing older. */
  if (g_lv == NULL || g_lv->loadedLogVersion < LOG_VERSION_V2) return FALSE;
  savedPos = lv_logGetCurrentPosition();
  savedKey = lv_blocksGetKey();
  lv_logSetPosition(s_walkStartPos);
  lv_blocksSetKey(0);   /* v2 and later are plaintext (identity de-XOR) */

  /* Seed from the loaded base table (empty on a lobby-started log). */
  for (i = 0; i < (int)lv_basesGetNumBases(&g_lv->bs) && i < MAX_BASES; i++) {
    base bi;
    lv_basesGetBase(&g_lv->bs, &bi, (BYTE)(i + 1));
    baseX[i] = bi.x;
    baseY[i] = bi.y;
    numBases = i + 1;
  }

  while (!done && !lv_blocksIsEOF()) {
    if (logReadBytes(&code, 1) != 1) break;
    switch (code) {
      case LOG_QUIT:
        done = TRUE;
        break;
      case LOG_SNAPSHOT:
        if (!walkSkipSnapshotBases(baseX, baseY, &numBases, NULL)) { done = TRUE; break; }
        ticks++;
        break;
      case LOG_NOEVENTS:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        waitLen = b1 == 0 ? 1 : b1;
        ticks += 1 + (uint64_t)waitLen;
        break;
      case LOG_NOEVENTS_LONG:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        if (logReadBytes(&b2, 1) != 1) { done = TRUE; break; }
        us = (uint16_t)((b1 << 8) | b2);
        waitLen = ntohs(us);
        if (waitLen == 0) waitLen = 1;
        ticks += 1 + (uint64_t)waitLen;
        break;
      case LOG_EVENT:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        numEvents = b1;
        ticks++;
        if (!walkScanBaseOwners(numEvents, (uint32_t)(ticks * 20),
                                baseX, baseY, numBases,
                                xE, yE, ownerE, ordE, xL, yL, ownerL, ordL,
                                &cntE, &msE, &haveE,
                                &cntL, &msL, &haveL)) { done = TRUE; break; }
        break;
      case LOG_EVENT_LONG:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        if (logReadBytes(&b2, 1) != 1) { done = TRUE; break; }
        us = (uint16_t)((b1 << 8) | b2);
        numEvents = ntohs(us);
        ticks++;
        if (!walkScanBaseOwners(numEvents, (uint32_t)(ticks * 20),
                                baseX, baseY, numBases,
                                xE, yE, ownerE, ordE, xL, yL, ownerL, ordL,
                                &cntE, &msE, &haveE,
                                &cntL, &msL, &haveL)) { done = TRUE; break; }
        break;
      default:
        done = TRUE;
        break;
    }
  }

  lv_logSetPosition(savedPos);
  lv_blocksSetKey(savedKey);
  if (haveE && haveL) { *outMsE = msE; *outMsL = msL; return TRUE; }
  return FALSE;
}

/* A string field copied into dst, cut to cap and terminated. Missing, or not
 * a string, reads as "". */
static void lv_scriptsCopyString(char *dst, size_t cap, const cJSON *obj,
                                 const char *key) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);

  dst[0] = '\0';
  if (cJSON_IsString(v) && v->valuestring != NULL) {
    snprintf(dst, cap, "%s", v->valuestring);
  }
}

/* A map square coordinate or size: a whole number from 0 to 255, and nothing
 * else. FALSE for a missing key, another type, a fraction or a value out of
 * range. */
static bool lv_scriptsSquare(const cJSON *obj, const char *key, uint8_t *out) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
  double       d;

  if (!cJSON_IsNumber(v)) return FALSE;
  d = v->valuedouble;
  if (!(d >= 0.0 && d <= 255.0) || d != (double)(int)d) return FALSE;
  *out = (uint8_t)d;
  return TRUE;
}

/*********************************************************
*NAME:          lv_scriptsParse
*PURPOSE:
*  Fills out from a recording's scripts.json text. The text
*  is untrusted: it is parsed with its length, only version
*  1 is read, every count stops at its array, every string
*  is cut to its buffer, a rule this build cannot name or
*  whose value is not a finite number is skipped, and a
*  region whose rectangle is not four whole numbers from 0
*  to 255, is empty or runs off the 256-square map is
*  skipped. Anything that is not a JSON object of version 1
*  leaves out all zero with present FALSE.
*********************************************************/
static void lv_scriptsParse(LvScripts *out, const char *text, size_t len) {
  cJSON       *root;
  const cJSON *v;
  const cJSON *item;

  memset(out, 0, sizeof(*out));
  if (text == NULL || len == 0) return;

  root = cJSON_ParseWithLength(text, len);
  if (!cJSON_IsObject(root)) {
    cJSON_Delete(root);
    return;
  }
  v = cJSON_GetObjectItemCaseSensitive(root, "version");
  if (!cJSON_IsNumber(v) || v->valuedouble != 1.0) {
    cJSON_Delete(root);
    return;
  }

  lv_scriptsCopyString(out->map, sizeof(out->map), root, "map");
  out->modsEnabled =
      cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "mods_enabled"))
          ? TRUE : FALSE;

  v = cJSON_GetObjectItemCaseSensitive(root, "rules");
  if (cJSON_IsObject(v)) {
    cJSON_ArrayForEach(item, v) {
      int index;
      if (out->ruleCount >= SIM_RULE_COUNT) break;
      if (item->string == NULL || !cJSON_IsNumber(item)) continue;
      /* 1e999 parses as a number and is infinite. */
      if (!isfinite(item->valuedouble)) continue;
      index = simRulesRuleIndex(item->string);
      if (index < 0) continue;
      out->rules[out->ruleCount].index = index;
      out->rules[out->ruleCount].value = item->valuedouble;
      out->ruleCount++;
    }
  }

  v = cJSON_GetObjectItemCaseSensitive(root, "regions");
  if (cJSON_IsArray(v)) {
    cJSON_ArrayForEach(item, v) {
      LvScriptRegion *r;
      uint8_t         x, y, w, h;
      if (out->regionCount >= LV_SCRIPTS_REGIONS_MAX) break;
      if (!cJSON_IsObject(item)) continue;
      if (!lv_scriptsSquare(item, "x", &x) || !lv_scriptsSquare(item, "y", &y) ||
          !lv_scriptsSquare(item, "w", &w) || !lv_scriptsSquare(item, "h", &h)) {
        continue;
      }
      /* A rectangle has to cover at least one square and stay on the
         256-square map. */
      if (w == 0 || h == 0 || (int)x + (int)w > 256 || (int)y + (int)h > 256) {
        continue;
      }
      r = &out->regions[out->regionCount];
      lv_scriptsCopyString(r->name, sizeof(r->name), item, "name");
      lv_scriptsCopyString(r->file, sizeof(r->file), item, "file");
      r->x = x;
      r->y = y;
      r->w = w;
      r->h = h;
      out->regionCount++;
    }
  }

  v = cJSON_GetObjectItemCaseSensitive(root, "scripts");
  if (cJSON_IsArray(v)) {
    cJSON_ArrayForEach(item, v) {
      LvScriptRow *s;
      const cJSON *manifest;
      if (out->count >= LV_SCRIPTS_MAX) break;
      if (!cJSON_IsObject(item)) continue;
      s = &out->scripts[out->count];
      lv_scriptsCopyString(s->file, sizeof(s->file), item, "file");
      lv_scriptsCopyString(s->source, sizeof(s->source), item, "source");
      lv_scriptsCopyString(s->kind, sizeof(s->kind), item, "kind");
      manifest = cJSON_GetObjectItemCaseSensitive(item, "manifest");
      /* A manifest that is not an object leaves both "". */
      lv_scriptsCopyString(s->name, sizeof(s->name), manifest, "name");
      lv_scriptsCopyString(s->description, sizeof(s->description), manifest,
                           "description");
      out->count++;
    }
  }

  out->present = TRUE;
  cJSON_Delete(root);
}

/* The open source's scripts.json into g_lv->scripts, which it clears first.
 * Called straight after the blocks source is set up, whether or not that
 * worked, so a load never leaves the last recording's scripts behind. */
static void lv_scriptsLoadFromBlocks(void) {
  size_t      len  = 0;
  const char *text = lv_blocksGetScriptsJson(&len);

  lv_scriptsParse(&g_lv->scripts, text, len);
}

// Memory size in MB
bool lv_logLoad(char *fileName, int memoryBufferSize) {
  char id[LENGTH_ID+1]; /* The map ID Should read "BMAPBOLO" */
  BYTE dataLen;
  BYTE logVersion;    /* Version of the map file */
  bool returnValue = TRUE;
  int len;
  BYTE ip[4];

  lv_snapshotDestroy(&g_lv->snap);
  g_lv->snap = lv_snapshotCreate();
  s_firstWorldSnapKnown = FALSE;
  g_lv->timeRunning = 0;
  lv_ruleChangesClear();
  lv_serverTickReset();
  lv_presReset();
  memset(g_lv->kills,        0, sizeof(g_lv->kills));
  memset(g_lv->deaths,       0, sizeof(g_lv->deaths));
  memset(g_lv->gameViewHud,  0, sizeof(g_lv->gameViewHud));
  memset(g_lv->tankInv,      0, sizeof(g_lv->tankInv));

  returnValue = lv_blocksCreate(fileName, memoryBufferSize);
  lv_scriptsLoadFromBlocks();
  if (returnValue == TRUE) {
    len = logReadBytes((BYTE *)id, LENGTH_ID);
    if (len != LENGTH_ID || strncmp(id,"WBOLOMOV", LENGTH_ID) != 0) {
      returnValue = FALSE;
    }
  }
  if (returnValue == TRUE) {
    len = logReadBytes(&logVersion, 1);
    if (len <= 0) {
      returnValue = FALSE;
    } else if (logVersion <= LOG_VERSION_V3) {
      /* Every version up to the newest one loads: v0 and v1 XOR'd, v2 and
         v3 plaintext and framed, and the readers below take the version
         the file states rather than assuming the newest. */
      g_lv->loadedLogVersion = logVersion;
    } else {
      returnValue = FALSE;
    }
  }

  /* Read map name */
  if (returnValue == TRUE) {
    logReadBytes(&dataLen, 1);
    /* An empty map name is valid (display-only field; the map data lives in the
       snapshot body). Only read+check when there are name bytes — a zero-length
       read returns -1, which would otherwise fail the load. */
    if (dataLen > 0) {
      len = logReadBytes((BYTE *)g_lv->mapName, dataLen);
      if (len != dataLen) {
        returnValue = FALSE;
      }
    }
    g_lv->mapName[dataLen] = '\0';
  }

  /* Read game type, mines, ai, password, max players */
  if (returnValue == TRUE) {
    logReadBytes(&g_lv->gt, 1);
    logReadBytes(&g_lv->allowHiddenMines, 1);
    logReadBytes(&g_lv->ai, 1);
    { BYTE tmp; logReadBytes(&tmp, 1); g_lv->usePassword = tmp; }
    logReadBytes(&g_lv->maxPlayers, 1);
    logReadBytes(&g_lv->versionMajor, 1);
    logReadBytes(&g_lv->versionMinor, 1);
    logReadBytes(&g_lv->versionRevision, 1);
    logReadBytes(ip, 4);
    snprintf(g_lv->serverIP, sizeof(g_lv->serverIP), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    logReadBytes((BYTE *) &g_lv->serverPort, sizeof(unsigned short));
    g_lv->serverPort = ntohs(g_lv->serverPort);
    logReadBytes((BYTE *) &g_lv->gmeCreateTime, sizeof(int32_t));
    g_lv->gmeCreateTime = ntohl(g_lv->gmeCreateTime);
    len = logReadBytes((BYTE *) &g_lv->wbnKey, 32);
    if (len != 32) {
      returnValue = FALSE;
    }
  }

  /* v2 and later are plaintext: blockKey stays 0 (identity de-XOR). v0/v1
     seed the rolling key from the low byte of the game create time. */
  lv_blocksSetKey(g_lv->loadedLogVersion >= LOG_VERSION_V2
                      ? 0
                      : (BYTE) (g_lv->gmeCreateTime & 0xFF));
  len = logReadBytes(&dataLen, 1);
  if (len != 1 || dataLen != LOG_SNAPSHOT) {
    returnValue = FALSE;
  } else {
    returnValue = lv_processSnapshot();
    /* Latch whether the opening snapshot carried a world, before anything else
     * can decode another one. That is what tells lv_walkComputeGameStartMs
     * whether a lobby ran in front of the event stream. */
    s_openingSnapshotHadWorld = s_lastSnapshotHadWorld;
  }

  g_lv->logLoaded = returnValue;
  return returnValue;
}

/*********************************************************
*NAME:          lv_screenLoadMap
*AUTHOR:        John Morrison
*CREATION DATE: 29/10/98
*LAST MODIFIED: 11/11/00
*PURPOSE:
*  Loads a map. Returns if it was sucessful reading the
*  map or not.
*
*ARGUMENTS:
*********************************************************/
bool lv_screenLoadMap(char *fileName, int memoryBufferSize) {
  bool returnValue; /* Value to return */

  returnValue = FALSE;
  lv_blocksDestroy();
  lv_screenDestroy();
  lv_screenSetup();
  /* Drop the previous log's names before anything can read them back. */
  memset(s_slotNames, 0, sizeof(s_slotNames));
  returnValue = lv_logLoad(fileName, memoryBufferSize);
  if (returnValue == TRUE) {
    /* Decompress entire log so we know total size for the seek slider */
    lv_logDecompressAll();
    /* Pre-scan to compute total game time. Byte position vs. time is
     * non-linear due to compression and variable event density, so the
     * one-shot walk is the only way to get accurate total/remaining
     * before a player joins. */
    s_walkStartPos = lv_logGetCurrentPosition();
    g_lv->totalTimeMs = lv_walkComputeTotalTimeMs();
    s_gameStartMs = lv_walkComputeGameStartMs();
    lv_walkCollectSlotNames();
    lv_walkCollectRuleChanges();
    lv_walkCollectPresentation();
    lv_walkCollectServerTicks();
    /* Set the game information up */
    lv_frontEndSetGameInformation(FALSE, g_lv->versionMajor, g_lv->versionMinor, g_lv->versionRevision, g_lv->mapName, g_lv->gt, g_lv->allowHiddenMines, g_lv->ai, g_lv->gmeStartDelay, g_lv->gmeLength, g_lv->wbnKey, g_lv->gmeCreateTime);
    g_lv->isPlaying = TRUE;
    lv_screenUpdateView(redraw);
    g_lv->state = lv_lr_start;
    lv_rulesRefresh();
    lv_presRebuild(g_lv->timeRunning);
    lv_screenParkAtWindowStart();
  } else {
    /* The last log's rules must not outlive it. */
    lv_rulesRefresh();
    lv_presRebuild(g_lv->timeRunning);
  }
  return returnValue;
}


/*********************************************************
*NAME:          lv_logLoadCommon
*PURPOSE:
*  Decodes the log header and opening snapshot from the
*  already-set-up blocks source. Resets the per-log g_lv
*  fields, reads the WBOLOMOV header, seeds the block key,
*  processes the opening snapshot, and records the result in
*  g_lv->logLoaded. The caller must have set up the blocks
*  source first (lv_blocksCreateFromMemory for a .wbv zip, or
*  lv_blocksBeginStream + lv_blocksAppendBytes for a stream).
*********************************************************/
static bool lv_logLoadCommon(void) {
  char id[LENGTH_ID+1];
  BYTE dataLen;
  BYTE logVersion;
  bool returnValue = TRUE;
  int len;
  BYTE ip[4];

  lv_snapshotDestroy(&g_lv->snap);
  g_lv->snap = lv_snapshotCreate();
  s_firstWorldSnapKnown = FALSE;
  g_lv->timeRunning = 0;
  lv_ruleChangesClear();
  lv_serverTickReset();
  lv_presReset();
  memset(g_lv->kills,        0, sizeof(g_lv->kills));
  memset(g_lv->deaths,       0, sizeof(g_lv->deaths));
  memset(g_lv->gameViewHud,  0, sizeof(g_lv->gameViewHud));
  memset(g_lv->tankInv,      0, sizeof(g_lv->tankInv));

  if (returnValue == TRUE) {
    len = logReadBytes((BYTE *)id, LENGTH_ID);
    if (len != LENGTH_ID || strncmp(id,"WBOLOMOV", LENGTH_ID) != 0) {
      returnValue = FALSE;
    }
  }
  if (returnValue == TRUE) {
    len = logReadBytes(&logVersion, 1);
    if (len <= 0) {
      returnValue = FALSE;
    } else if (logVersion <= LOG_VERSION_V3) {
      /* Every version up to the newest one loads: v0 and v1 XOR'd, v2 and
         v3 plaintext and framed, and the readers below take the version
         the file states rather than assuming the newest. */
      g_lv->loadedLogVersion = logVersion;
    } else {
      returnValue = FALSE;
    }
  }

  if (returnValue == TRUE) {
    logReadBytes(&dataLen, 1);
    /* An empty map name is valid (display-only field; the map data lives in the
       snapshot body). Only read+check when there are name bytes — a zero-length
       read returns -1, which would otherwise fail the load. */
    if (dataLen > 0) {
      len = logReadBytes((BYTE *)g_lv->mapName, dataLen);
      if (len != dataLen) {
        returnValue = FALSE;
      }
    }
    g_lv->mapName[dataLen] = '\0';
  }

  if (returnValue == TRUE) {
    logReadBytes(&g_lv->gt, 1);
    logReadBytes(&g_lv->allowHiddenMines, 1);
    logReadBytes(&g_lv->ai, 1);
    { BYTE tmp; logReadBytes(&tmp, 1); g_lv->usePassword = tmp; }
    logReadBytes(&g_lv->maxPlayers, 1);
    logReadBytes(&g_lv->versionMajor, 1);
    logReadBytes(&g_lv->versionMinor, 1);
    logReadBytes(&g_lv->versionRevision, 1);
    logReadBytes(ip, 4);
    snprintf(g_lv->serverIP, sizeof(g_lv->serverIP), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    logReadBytes((BYTE *) &g_lv->serverPort, sizeof(unsigned short));
    g_lv->serverPort = ntohs(g_lv->serverPort);
    logReadBytes((BYTE *) &g_lv->gmeCreateTime, sizeof(int32_t));
    g_lv->gmeCreateTime = ntohl(g_lv->gmeCreateTime);
    len = logReadBytes((BYTE *) &g_lv->wbnKey, 32);
    if (len != 32) {
      returnValue = FALSE;
    }
  }

  /* v2 and later are plaintext: blockKey stays 0 (identity de-XOR). v0/v1
     seed the rolling key from the low byte of the game create time. */
  lv_blocksSetKey(g_lv->loadedLogVersion >= LOG_VERSION_V2
                      ? 0
                      : (BYTE) (g_lv->gmeCreateTime & 0xFF));
  len = logReadBytes(&dataLen, 1);
  if (len != 1 || dataLen != LOG_SNAPSHOT) {
    returnValue = FALSE;
  } else {
    returnValue = lv_processSnapshot();
    /* Latch whether the opening snapshot carried a world, before anything else
     * can decode another one. That is what tells lv_walkComputeGameStartMs
     * whether a lobby ran in front of the event stream. */
    s_openingSnapshotHadWorld = s_lastSnapshotHadWorld;
  }

  g_lv->logLoaded = returnValue;
  return returnValue;
}

/*********************************************************
*NAME:          lv_logLoadFromMemory
*PURPOSE:
*  Loads log data from an in-memory zip buffer.
*  Same as lv_logLoad but uses lv_blocksCreateFromMemory.
*  Takes ownership of zipData.
*********************************************************/
static bool lv_logLoadFromMemory(uint8_t *zipData, size_t zipLen) {
  bool created = lv_blocksCreateFromMemory(zipData, zipLen);

  lv_scriptsLoadFromBlocks();
  lv_ruleChangesClear();
  lv_serverTickReset();
  lv_presReset();
  if (created != TRUE) {
    g_lv->logLoaded = FALSE;
    return FALSE;
  }
  return lv_logLoadCommon();
}

bool lv_screenLoadMapFromMemory(uint8_t *zipData, size_t zipLen) {
  bool returnValue;

  returnValue = FALSE;
  lv_blocksDestroy();
  lv_screenDestroy();
  lv_screenSetup();
  /* Drop the previous log's names before anything can read them back. */
  memset(s_slotNames, 0, sizeof(s_slotNames));
  returnValue = lv_logLoadFromMemory(zipData, zipLen);
  if (returnValue == TRUE) {
    lv_logDecompressAll();
    s_walkStartPos = lv_logGetCurrentPosition();
    g_lv->totalTimeMs = lv_walkComputeTotalTimeMs();
    s_gameStartMs = lv_walkComputeGameStartMs();
    lv_walkCollectSlotNames();
    lv_walkCollectRuleChanges();
    lv_walkCollectPresentation();
    lv_walkCollectServerTicks();
    lv_frontEndSetGameInformation(FALSE, g_lv->versionMajor, g_lv->versionMinor, g_lv->versionRevision, g_lv->mapName, g_lv->gt, g_lv->allowHiddenMines, g_lv->ai, g_lv->gmeStartDelay, g_lv->gmeLength, g_lv->wbnKey, g_lv->gmeCreateTime);
    g_lv->isPlaying = TRUE;
    lv_screenUpdateView(redraw);
    g_lv->state = lv_lr_start;
    lv_rulesRefresh();
    lv_presRebuild(g_lv->timeRunning);
    lv_screenParkAtWindowStart();
  } else {
    /* The last log's rules must not outlive it. */
    lv_rulesRefresh();
    lv_presRebuild(g_lv->timeRunning);
  }
  return returnValue;
}

/*********************************************************
*NAME:          lv_screenLoadFromStream
*PURPOSE:
*  Loads the decoder from a plaintext (v2) byte stream. The
*  caller supplies the initial header + opening snapshot bytes
*  here, then appends further records via lv_blocksAppendBytes
*  and steps lv_screenLogTick. Unlike the .wbv zip path there is
*  no fixed total size, so the decompress / total-time /
*  game-info steps are intentionally skipped.
*********************************************************/
bool lv_screenLoadFromStream(const uint8_t *bytes, size_t len) {
  bool ok;

  lv_blocksDestroy();
  lv_screenDestroy();
  lv_screenSetup();
  lv_blocksBeginStream();
  /* A stream has no zip and so no scripts.json: this clears the holder. */
  lv_scriptsLoadFromBlocks();
  lv_ruleChangesClear();
  lv_serverTickReset();
  lv_presReset();
  if (lv_blocksAppendBytes(bytes, len) != TRUE) {
    g_lv->logLoaded = FALSE;
    lv_rulesRefresh();
    lv_presRebuild(g_lv->timeRunning);
    return FALSE;
  }
  ok = lv_logLoadCommon();
  if (ok == TRUE) {
    /* Publish the header's game information (map name / type / settings) to the
     * front end, as the file and in-memory loaders do — lv_logLoadCommon parses
     * it into g_lv but doesn't push it. This is the spectator seed-load path
     * (lv_specSeedLoad); standalone .wbv loads run through lv_screenLoadMap /
     * lv_screenLoadMapFromMemory and are unaffected. */
    /* The spectator seed's synthesized header carries no real version, so
     * lv_logLoadCommon parsed zeros into g_lv->version*. The spectator runs a
     * protocol compatible with the server, so the client's own build version is
     * the right thing to show — overwrite with it before publishing. */
    g_lv->versionMajor    = BOLO_VERSION_MAJOR;
    g_lv->versionMinor    = BOLO_VERSION_MINOR;
    g_lv->versionRevision = BOLO_VERSION_REVISION;
    lv_frontEndSetGameInformation(FALSE, g_lv->versionMajor, g_lv->versionMinor, g_lv->versionRevision, g_lv->mapName, g_lv->gt, g_lv->allowHiddenMines, g_lv->ai, g_lv->gmeStartDelay, g_lv->gmeLength, g_lv->wbnKey, g_lv->gmeCreateTime);
    g_lv->isPlaying = TRUE;
    lv_screenUpdateView(redraw);
    g_lv->state = lv_lr_start;
  }
  /* A feed has no walk: its rules start from scripts.json (none on a stream)
     and the classic values, and its changes arrive as playback meets them. */
  lv_rulesRefresh();
  lv_presRebuild(g_lv->timeRunning);
  return ok;
}

bool lv_screenIsPlaying() {
  return g_lv->isPlaying;
}

/*********************************************************
*NAME:          lv_screenStreamPump
*PURPOSE:
*  Feeds a live, append-only byte stream into the decoder.
*  Appends the caller-supplied newly-arrived bytes, then
*  advances playback over the whole records that are now
*  fully buffered, and parks cleanly when it catches up.
*
*  Appends are record-aligned (the ring and the record
*  translator emit complete records), so logPosition <
*  logSize means at least one complete record is present and
*  one reading tick lands exactly on the next record
*  boundary. When the cursor reaches logSize the decoder is
*  left untouched (state intact, still playing) rather than
*  ticked — a reading tick at the boundary would read a short
*  count and misalign the cursor, and a live stream carries no
*  LOG_QUIT, so "caught up" must never be treated as
*  end-of-log. The isPlaying guard stops the loop if playback
*  ever does finish so a non-advancing tick cannot spin.
*
*  Single-threaded and source-agnostic: the caller supplies
*  the bytes. Returns the decoder's isPlaying state.
*PARAMS:        bytes - newly-arrived stream bytes (may be NULL)
*               len   - number of bytes (may be 0 when caught up)
*RETURNS:       TRUE while playback is live, FALSE once finished.
*********************************************************/
/* --- Spectator live-DVR state (see backend.h / lv_screenSpec* below) --- */
static bool     s_specLiveMode    = false; /* spectatorRun active: append-only pump, host-driven advance */
static bool     s_specFollowLive  = true;  /* TRUE: slam to head; FALSE: parked, paced real-time playback */
static bool     s_specSeekPark    = false; /* a user seek/rewind happened -> park on the next frame update */
static bool     s_specHaveAnchor  = false;
static uint32_t s_specHeadTick    = 0;     /* latest drained forward-record game tick */
static uint32_t s_specAnchorTick  = 0;     /* head tick captured at the last follow-live re-anchor */
static uint32_t s_specAnchorMs    = 0;     /* timeRunning captured at the last follow-live re-anchor */
static uint32_t s_specPaceLastMs  = 0;     /* wall-clock anchor for the parked 20ms pacing accumulator */
static int32_t  s_specPaceAccumMs = 0;

bool lv_screenStreamPump(const uint8_t *bytes, size_t len) {
  if (g_lv == NULL) {
    return FALSE;
  }
  if (bytes != NULL && len > 0) {
    if (lv_blocksAppendBytes(bytes, len) != TRUE) {
      return g_lv->isPlaying;
    }
  }
  /* Live spectator mode appends only — the host advances the decoder itself via
     lv_screenSpecFrameUpdate so a parked view doesn't slam to the head. */
  while (g_lv->isPlaying == TRUE && !s_specLiveMode &&
         lv_logGetCurrentPosition() < lv_logGetTotalSize()) {
    lv_screenLogTick();
  }
  return g_lv->isPlaying;
}

/* Allocate a decoder state, set its field defaults, and register it as the
 * active state. Host-callable: lets a caller drive lv_screenLoadMapFromMemory
 * / lv_screenLogTick / lv_screenCloseLog without the standalone GUI/platform
 * scaffolding. Returns NULL on allocation failure. */
LogViewerState *lv_decoderCreate(bool fromMainMenu) {
  LogViewerState *lv = (LogViewerState *)calloc(1, sizeof(LogViewerState));
  if (lv == NULL) {
    return NULL;
  }
  /* The classic gameplay numbers. calloc gave zeroes, and a zero cap would
     draw every bar empty and divide by zero. */
  lv->rules.tankFullShells = 40;
  lv->rules.tankFullMines  = 40;
  lv->rules.tankFullArmour = 40;
  lv->rules.tankFullTrees  = 40;
  lv->rules.baseFullShells = 90;
  lv->rules.baseFullMines  = 90;
  lv->rules.baseFullArmour = 90;
  lv->rules.pillMaxArmour  = 15;

  lv->fromMainMenu = fromMainMenu;
  lv->screenSizeX = MAIN_SCREEN_SIZE_X + 15; /* default 30 */
  lv->screenSizeY = MAIN_SCREEN_SIZE_Y + 15; /* default 30 */
  lv->isLoaded = FALSE;
  lv->isSoundsPlaying = TRUE;
  lv->soundVolume = 50;

  /* Game-view skin state — calloc above already zeroed these, but be
   * explicit so the defaults are visible alongside the other init. */
  lv->gameView = FALSE;
  lv->cameraSlot = 0;
  lv->savedUseTeamColours = FALSE;
  memset(lv->kills, 0, sizeof(lv->kills));
  memset(lv->deaths, 0, sizeof(lv->deaths));
  memset(lv->gameViewHud, 0, sizeof(lv->gameViewHud));
  memset(lv->tankInv, 0, sizeof(lv->tankInv));

  lv_screenSetState(lv);
  /* The same numbers again, from the rules table this time: with no log
     there are no changes and no scripts.json, so every rule is classic. */
  lv_rulesRefresh();
  lv_presRebuild(g_lv->timeRunning);
  return lv;
}

/* Close any loaded log (frees the zip buffer + screen structures), free the
 * decoder state, and clear the active state. NULL-safe. lv_screenCloseLog is
 * safe on a never-loaded state (lv_blocksDestroy and lv_screenDestroy both
 * no-op on the zeroed pointers), so it is called unconditionally. Closing
 * keeps the snapshot list, which a load replaces, so it is freed here. */
void lv_decoderDestroy(LogViewerState *lv) {
  if (lv == NULL) {
    return;
  }
  lv_screenCloseLog();
  lv_snapshotDestroy(&lv->snap);
  free(lv);
  lv_screenSetState(NULL);
}

bool lv_screenCloseLog() {
  g_lv->isPlaying = FALSE;
  g_lv->logLoaded = FALSE;
  /* lv_screenRuleValueAt reads the scripts whether or not a log is loaded,
     so the closed recording's are dropped here rather than at the next
     load. */
  memset(&g_lv->scripts, 0, sizeof(g_lv->scripts));
  lv_ruleChangesClear();
  lv_serverTickReset();
  lv_presReset();
  lv_screenStoreGameSettings(NULL, 0);
  s_gameSettingsWalked = FALSE;

  lv_blocksDestroy();
  lv_screenDestroy();
  return TRUE;
}

void lv_screenSetOffset(BYTE x, BYTE y) {
  g_lv->xOffset = x;
  g_lv->yOffset = y;
}
BYTE lv_screenGetOffsetX() {
  return g_lv->xOffset;
}

BYTE lv_screenGetOffsetY() {
  return g_lv->yOffset;
}

BYTE lv_screenGetNumPills() {
  return lv_pillsGetNumPills(&g_lv->pb);
}

BYTE lv_screenGetNumBases() {
  return lv_basesGetNumBases(&g_lv->bs);
}

BYTE lv_screenGetNumStarts() {
  return lv_startsGetNumStarts(&g_lv->ss);
}


bool lv_screenSetStart(BYTE x, BYTE y) {
  BYTE num;
  start s;
  num = lv_startsGetNumStarts(&g_lv->ss);
  if (num >= MAX_STARTS) {
    return FALSE;
  }
  lv_basesDeleteBase(&g_lv->bs, x, y);
  lv_startsDeleteStart(&g_lv->ss, x, y);
  lv_pillsDeletePill(&g_lv->pb, x, y);

  lv_mapSetPos(&g_lv->mp, x, y, DEEP_SEA);
  s.x = x;
  s.y = y;
  s.dir = 0;
  num = lv_startsGetNumStarts(&g_lv->ss);
  lv_startsSetNumStarts(&g_lv->ss, (BYTE) (num+1));
  lv_startsSetStart(&g_lv->ss, &s, (BYTE) (num+1));
  return TRUE;
}

bool lv_screenSetPill(BYTE x, BYTE y) {
  BYTE num;
  pillbox s;
  num = lv_pillsGetNumPills(&g_lv->pb);
  if (num >= MAX_PILLS) {
    return FALSE;
  }
  lv_basesDeleteBase(&g_lv->bs, x, y);
  lv_startsDeleteStart(&g_lv->ss, x, y);
  lv_pillsDeletePill(&g_lv->pb, x, y);

  lv_mapSetPos(&g_lv->mp, x, y, ROAD);
  s.x = x;
  s.y = y;
  s.owner = 0xFF;
  s.armour = 15;
  s.speed = 0;
  s.inTank = FALSE;
  num = lv_pillsGetNumPills(&g_lv->pb);
  lv_pillsSetNumPills(&g_lv->pb, (BYTE) (num+1));
  lv_pillsSetPill(&g_lv->pb, &s, (BYTE) (num+1));
  return TRUE;

}

bool lv_screenSetBase(BYTE x, BYTE y) {
  BYTE num;
  base s;

  num = lv_basesGetNumBases(&g_lv->bs);
  if (num >= MAX_BASES) {
    return FALSE;
  }
  lv_basesDeleteBase(&g_lv->bs, x, y);
  lv_startsDeleteStart(&g_lv->ss, x, y);
  lv_pillsDeletePill(&g_lv->pb, x, y);

  lv_mapSetPos(&g_lv->mp, x, y, ROAD);
  s.x = x;
  s.y = y;
  s.owner = 0xFF;
  s.armour = 90;
  s.mines = 90;
  s.shells = 90;


  num = lv_basesGetNumBases(&g_lv->bs);
  lv_basesSetNumBases(&g_lv->bs, (BYTE) (num+1));
  lv_basesSetBase(&g_lv->bs, &s, (BYTE) (num+1));
  return TRUE;
}

/*********************************************************
*NAME:          lv_screenIsMine
*AUTHOR:        John Morrison
*CREATION DATE: 6/11/98
*LAST MODIFIED: 6/11/98
*PURPOSE:
*  Returns if a square on the screen should have a mine
*  drawn on it.
*  If value is out of range returns FALSE
*
*ARGUMENTS:
*  value  - Pointer to the screenMines structure
*  xValue - The X co-ordinate
*  yValue - The Y co-ordinate
*********************************************************/
bool lv_screenIsMine(screenMines *value,BYTE xValue, BYTE yValue) {
  bool returnValue = FALSE; /* Value to return */

  /* Same +1 margin geometry as the screen tile buffer; see lv_screenGetPos. */
  if (xValue <= lv_screenGetSizeX() && yValue <= lv_screenGetSizeY()) {
    returnValue = *((*value)->mineItem+(yValue*(lv_screenGetSizeX()+1)+xValue));
  }
  return returnValue;
}

/*********************************************************
*NAME:          lv_screenNumPills
*AUTHOR:        John Morrison
*CREATION DATE: 21/12/98
*LAST MODIFIED: 21/12/98
*PURPOSE:
*  Returns the number of pillboxes
*
*ARGUMENTS:
*
*********************************************************/
BYTE lv_screenNumPills(void) {
  return lv_pillsGetNumPills(&g_lv->pb);
}

/*********************************************************
*NAME:          lv_screenNumBases
*AUTHOR:        John Morrison
*CREATION DATE: 21/12/98
*LAST MODIFIED: 21/12/98
*PURPOSE:
*  Returns the number of bases
*
*ARGUMENTS:
*
*********************************************************/
BYTE lv_screenNumBases(void) {
  return lv_basesGetNumBases(&g_lv->bs);
}


BYTE lv_screenGetSizeX() {
  return g_lv->screenSizeX;
}

BYTE lv_screenGetSizeY() {
  return g_lv->screenSizeY;
}

/* Forward declaration for draw.c function */
extern void lv_drawResizeRenderTarget(void);

void lv_screenSetSizeX(BYTE x) {
  g_lv->screenSizeX = x;
  if (g_lv->view != NULL) {
    BYTE *newItems = malloc((lv_screenGetSizeX()+2) * (lv_screenGetSizeY()+2));
    if (newItems != NULL) {
      free((*g_lv->view).screenItem);
      (*g_lv->view).screenItem = newItems;
    }
  }
  if (g_lv->mineView != NULL) {
    bool *newItems = malloc((lv_screenGetSizeX()+1) * (lv_screenGetSizeY()+1) * sizeof(bool));
    if (newItems != NULL) {
      free((*g_lv->mineView).mineItem);
      (*g_lv->mineView).mineItem = newItems;
    }
  }
  /* Resize the render target to match the new screen size.
   * This is critical for correct mouse coordinate mapping. */
  lv_drawResizeRenderTarget();
}

void lv_screenSetSizeY(BYTE y) {
  g_lv->screenSizeY = y;
  if (g_lv->view != NULL) {
    BYTE *newItems = malloc((lv_screenGetSizeX()+2) * (lv_screenGetSizeY()+2));
    if (newItems != NULL) {
      free((*g_lv->view).screenItem);
      (*g_lv->view).screenItem = newItems;
    }
  }
  if (g_lv->mineView != NULL) {
    bool *newItems = malloc((lv_screenGetSizeX()+1) * (lv_screenGetSizeY()+1) * sizeof(bool));
    if (newItems != NULL) {
      free((*g_lv->mineView).mineItem);
      (*g_lv->mineView).mineItem = newItems;
    }
  }
  /* Resize the render target to match the new screen size.
   * This is critical for correct mouse coordinate mapping. */
  lv_drawResizeRenderTarget();
}

void lv_screenGetOffsets(BYTE *x, BYTE *y) {
  if (x != NULL) {
    *x = g_lv->xOffset;
  }
  if (y != NULL) {
    *y = g_lv->yOffset;
  }
}

void lv_screenGetSubOffset(int *x, int *y) {
  if (x != NULL) *x = g_lv->subPxX;
  if (y != NULL) *y = g_lv->subPxY;
}

void lv_screenSetSubOffset(int x, int y) {
  g_lv->subPxX = x;
  g_lv->subPxY = y;
}

/* Decompose a total pan position (in zoom-1 native pixels) into
 * (xOffset,yOffset) tile + (subPxX,subPxY) pixel components, clamping
 * to the map. The whole-tile range matches the existing resize-time
 * clamp ([0, 255 - screenSize]); when the viewport reaches that edge
 * we force sub-pixel to zero so the trailing edge has no bleed past
 * the rendered tiles. */
void lv_screenPanToTotalPixels(int totalPxX, int totalPxY) {
  if (g_lv->logLoaded == FALSE) {
    return;
  }

  int sizeX = lv_screenGetSizeX();
  int sizeY = lv_screenGetSizeY();
  int maxOffX = 255 - sizeX; if (maxOffX < 0) maxOffX = 0;
  int maxOffY = 255 - sizeY; if (maxOffY < 0) maxOffY = 0;
  int maxPxX = maxOffX * TILE_SIZE_X;
  int maxPxY = maxOffY * TILE_SIZE_Y;

  if (totalPxX < 0) totalPxX = 0;
  if (totalPxY < 0) totalPxY = 0;
  if (totalPxX > maxPxX) totalPxX = maxPxX;
  if (totalPxY > maxPxY) totalPxY = maxPxY;

  int newOffX = totalPxX / TILE_SIZE_X;
  int newOffY = totalPxY / TILE_SIZE_Y;
  int newSubX = totalPxX - newOffX * TILE_SIZE_X;
  int newSubY = totalPxY - newOffY * TILE_SIZE_Y;

  bool wholeChanged = ((BYTE)newOffX != g_lv->xOffset) ||
                      ((BYTE)newOffY != g_lv->yOffset);
  bool subChanged   = (newSubX != g_lv->subPxX) || (newSubY != g_lv->subPxY);

  g_lv->subPxX = newSubX;
  g_lv->subPxY = newSubY;

  if (wholeChanged) {
    g_lv->xOffset = (BYTE)newOffX;
    g_lv->yOffset = (BYTE)newOffY;
  }

  /* Mutate the camera ONLY — never render here. Both hosts consume
   * wantScreenUpdate at the top of their next frame (lvEmbedFrameTexture for
   * the embedded reel, the logviewer.c main loop for the standalone app), and
   * that is the only place a render pairs coherently with the blit-offset
   * snapshot. Pan input runs AFTER the frame's image was already recorded, so
   * an inline lv_screenUpdate(redraw) on a whole-tile crossing repainted the
   * texture for the NEW camera while the frame presented it with the OLD
   * offsets — one displaced frame at every tile boundary of a drag, the
   * reel's "flickers every ~16 pixels of pan". Deferring to the flag renders
   * once, next frame, with matched offsets. Cheap: the redraw is
   * differential, so an unchanged grid blits nothing. */
  if (wholeChanged || subChanged) {
    g_lv->wantScreenUpdate = TRUE;
  }
}

void lv_messageAdd(messageType msgType, langid topId, langid bodyId,
                   const MessageArgs *args) {
  /* The events panel renders a single line per message — the channel
   * header (topId) is not displayed. Render the body via the lang
   * runtime so each viewer sees its own language; switching languages
   * mid-replay does not retranslate prior entries (acceptable per the
   * Phase 1 caveat — the events panel stores rendered strings). */
  char body[FILENAME_MAX];
  const char *rendered = langGetTextFmt(bodyId, args);
  body[0] = '\0';
  if (rendered) {
    snprintf(body, sizeof(body), "%s", rendered);
  }
  lv_windowAddEvent(0, body);

  /* Feed the scrolling-marquee queue used by the trailer game view.
   * Mirror the live game's clientMessageAdd: the channel header
   * (topId — e.g. "Newswire:") goes on the top line on the first
   * message of a run from a given source, and is blanked on
   * consecutive messages of the same source so successive entries
   * read as one continuing transcript. The queue is dormant when
   * the game view isn't running (no consumer); the cost is one
   * list-append per event. */
  {
    static messageType s_lastMessage = (messageType)-1;
    const char *topRendered =
        (s_lastMessage != msgType) ? langGetText(topId) : MESSAGE_EMPTY;
    s_lastMessage = msgType;
    char top[FILENAME_MAX];
    top[0] = '\0';
    if (topRendered) {
      snprintf(top, sizeof(top), "%s", topRendered);
    }
    lv_messageAddItem(top, body);
  }
}

/* Format an absolute log time as the displayed (window-relative) mm:ss. */
void lv_screenFormatTime(uint32_t absMs, char *dest, size_t destSize) {
  uint32_t start = lv_windowStartMs();
  uint32_t secs  = ((absMs > start) ? (absMs - start) : 0) / 1000u;
  snprintf(dest, destSize, "%02u:%02u", secs / 60u, secs % 60u);
}

void lv_screenGetTime(char *dest) {
  lv_screenFormatTime(g_lv->timeRunning, dest, 6);
}


void lv_screenMouseCentreClick(int xPos, int yPos) {
  div_t dt;        /* Used for integer division */
  int xClick;
  int yClick;

  if (g_lv->logLoaded == FALSE) {
    return;
  }
  dt = div(xPos, (16)); //screenSizeX
  xClick = (int) (dt.quot);
  dt = div(yPos, (16)); //screenSizeY
  yClick = (int) (dt.quot);
  g_lv->xOffset = (g_lv->xOffset + xClick) - (g_lv->screenSizeX / 2);
  g_lv->yOffset = (g_lv->yOffset + yClick) - (g_lv->screenSizeY / 2);
  /* Defer to the flag — see lv_screenPanToTotalPixels. */
  g_lv->wantScreenUpdate = TRUE;
}

/* Centre the game view on a map cell, clamped so the offset stays in range
 * (xOffset/yOffset are unsigned tile indices). */
void lv_screenCentreOnCell(int mapX, int mapY) {
  int cx, cy;
  if (g_lv->logLoaded == FALSE) {
    return;
  }
  cx = mapX - (g_lv->screenSizeX / 2);
  cy = mapY - (g_lv->screenSizeY / 2);
  if (cx < 0) cx = 0;
  if (cy < 0) cy = 0;
  if (cx > 255) cx = 255;
  if (cy > 255) cy = 255;
  g_lv->xOffset = (BYTE)cx;
  g_lv->yOffset = (BYTE)cy;
  /* Defer to the flag — see lv_screenPanToTotalPixels. */
  g_lv->wantScreenUpdate = TRUE;
}

/* Takes an absolute log time (highlight clip times are absolute) and clamps it
 * into the presented window. */
void lv_screenSeekToTimeMs(uint32_t ms) {
  uint32_t start = lv_windowStartMs();
  uint32_t len   = lv_windowLenMs();
  if (len == 0) return;
  if (ms < start) ms = start;
  if (ms > start + len) ms = start + len;
  lv_screenSeekToAbsoluteMs(ms);
}

void lv_screenMouseInformationClick(int xPos, int yPos) {
  div_t dt;        /* Used for integer division */
  int xClick;
  int yClick;
  BYTE mapX, mapY;  /* Map coordinates */

  if (g_lv->logLoaded == FALSE) {
    return;
  }

  dt = div(xPos, (16)); //screenSizeX
  xClick = (int) (dt.quot);
  dt = div(yPos, (16)); //screenSizeY
  yClick = (int) (dt.quot);

  /* Calculate map coordinates */
  mapX = (BYTE) (g_lv->xOffset + xClick);
  mapY = (BYTE) (g_lv->yOffset + yClick);

  // Item info
  if (lv_pillsExistPos(&g_lv->pb, mapX, mapY) == TRUE) {
    g_lv->selectedItem = lv_pillsItemNumAt(&g_lv->pb, mapX, mapY);
    g_lv->selectedItemType = 2;
  } else if (lv_basesExistPos(&g_lv->bs, mapX, mapY) == TRUE) {
    g_lv->selectedItem = lv_basesItemNumAt(&g_lv->bs, mapX, mapY);
    g_lv->selectedItemType = 1;
  }


  if (g_lv->fastForwarding == FALSE) {
    if (g_lv->selectedItemType == 0) {
      lv_updateItem(0, 0, 0, 0, 0, 0, 0, 0, 0);
    } else if (g_lv->selectedItemType == 1) {
      lv_updateItem(1, g_lv->selectedItem, g_lv->bs->item[g_lv->selectedItem].owner, g_lv->bs->item[g_lv->selectedItem].x, g_lv->bs->item[g_lv->selectedItem].y, g_lv->bs->item[g_lv->selectedItem].armour, g_lv->bs->item[g_lv->selectedItem].shells, g_lv->bs->item[g_lv->selectedItem].mines, FALSE);
    } else {
      lv_updateItem(2, g_lv->selectedItem, g_lv->pb->item[g_lv->selectedItem].owner, g_lv->pb->item[g_lv->selectedItem].x, g_lv->pb->item[g_lv->selectedItem].y, g_lv->pb->item[g_lv->selectedItem].armour, 0, 0, g_lv->pb->item[g_lv->selectedItem].inTank);
    }
  }

}

void lv_screenMouseClick(int xPos, int yPos) {
  div_t dt;        /* Used for integer division */
  int xClick;
  int yClick;

  if (g_lv->logLoaded == FALSE) {
    return;
  }
  dt = div(xPos, (16)); //screenSizeX
  xClick = (int) (dt.quot);
  dt = div(yPos, (16)); //screenSizeY
  yClick = (int) (dt.quot);

  // Who am I viewing?
  if (lv_playersChooseView(g_lv->xOffset + xClick, g_lv->yOffset + yClick) == TRUE) {
  } else if (lv_pillsChooseView(&g_lv->pb, g_lv->xOffset + xClick, g_lv->yOffset + yClick) == TRUE) {
  } else if (lv_basesChooseView(&g_lv->bs, g_lv->xOffset + xClick, g_lv->yOffset + yClick) == TRUE) {
  } else {
  }
}

/* Play on to the next snapshot or the end of the log. A live feed that
   reaches its head first stops there: the head stays where it is until the
   host appends more, and it cannot while this holds the client mutex. */
void lv_screenFastForward() {
  bool available;

  if (g_lv->isPlaying == TRUE && g_lv->fastForwarding == FALSE) {
    g_lv->fastForwarding = TRUE;
    available = lv_screenLogTick();
    while (available == FALSE && g_lv->isPlaying == TRUE &&
           s_logTickAtLiveHead == FALSE) {
      available = lv_screenLogTick();
    }
    g_lv->fastForwarding = FALSE;
  }
}

void lv_screenTankCentred(int enabled) {
  g_lv->centredTank = enabled;
}

void lv_screenSetHideLobby(int enabled) {
  bool want = enabled ? TRUE : FALSE;
  uint32_t start;
  if (want == s_hideLobby) return;
  s_hideLobby = want;
  /* Ticking it while the playhead sits in the lobby jumps to game start;
   * anywhere else, and on un-tick, nothing moves — only the scale relabels. */
  if (want == FALSE) return;
  if (g_lv == NULL || g_lv->logLoaded == FALSE) return;
  start = lv_windowStartMs();
  if (start > 0 && g_lv->timeRunning < start) {
    lv_screenSeekToAbsoluteMs(start);
  }
}

int lv_screenGetHideLobby(void) { return s_hideLobby ? 1 : 0; }

uint32_t lv_screenWindowStartMs(void) { return lv_windowStartMs(); }

void lv_screenRewind() {
  uint32_t currentTime = g_lv->timeRunning;
  size_t wantedPos;
  uint32_t firstTime;
  bool available;
  BYTE key;
  BYTE *pTeams = NULL;

  available = lv_snapshotBackwards(&g_lv->snap, &wantedPos, &currentTime, &key, &pTeams);
  if (available == TRUE) {
    if (currentTime > 1000 && (g_lv->timeRunning - currentTime) < 1000) {
      firstTime = currentTime;
      currentTime -= 1000;
      if (lv_snapshotBackwards(&g_lv->snap, &wantedPos, &currentTime, &key, &pTeams) == FALSE) {
        currentTime = firstTime;
      }
    }
    /* A rewind never steps back into the hidden lobby; it parks at game start. */
    if (currentTime < lv_windowStartMs()) {
      lv_screenSeekToAbsoluteMs(lv_windowStartMs());
      return;
    }
    g_lv->timeRunning = currentTime;
    lv_logSetPosition(wantedPos);
    lv_blocksSetKey(key);
    lv_playersSetTeams(pTeams);
    lv_processSnapshot();
    /* The snapshot carries no rules: read them again at its time. */
    lv_rulesRefresh();
    lv_presRebuild(g_lv->timeRunning);
    lv_windowRemoveEventsAfter(g_lv->timeRunning);
    g_lv->isPlaying = TRUE;
    g_lv->state = lv_lr_start;
    /* Rewinding parks the live-DVR view in the past (consumed next frame). */
    s_specSeekPark = true;
    if (wantedPos == 0) {
      lv_startOfLog();
    }
  }
}

void lv_screenGetLogProgress(size_t *currentPos, size_t *totalSize, uint32_t *currentTime, uint32_t *totalTime) {
  uint32_t start = lv_windowStartMs();
  uint32_t len   = lv_windowLenMs();
  uint32_t cur   = g_lv->timeRunning;
  *currentPos = lv_logGetCurrentPosition();
  *totalSize  = lv_logGetTotalSize();
  cur = (cur > start) ? (cur - start) : 0;
  if (cur > len) cur = len;
  *currentTime = cur;
  *totalTime   = len;
}

void lv_screenSeekToPosition(float ratio) {
  uint32_t start = lv_windowStartMs();
  uint32_t len   = lv_windowLenMs();
  if (len == 0) return;
  if (ratio < 0.0f) ratio = 0.0f;
  if (ratio > 1.0f) ratio = 1.0f;
  lv_screenSeekToAbsoluteMs(start + (uint32_t)(ratio * (float)len));
}

/* Seek playback to an absolute log time: restore the newest snapshot at or
 * before it, then fast-forward the decoder to the target.
 *
 * A forward seek skips the restore entirely. The decoder is a sequential state
 * machine and its current state is already the replay of everything up to
 * timeRunning, so ticking on to a later target lands in exactly the state a
 * restore-and-replay would produce, for only the ticks in between. That
 * matters because a round carries essentially one snapshot, at its start: the
 * restore path re-decodes the whole round every time, at 20 ms of log per
 * tick, which is why scrubbing used to be affordable only once on release.
 * With this, dragging the recap's seek slider forward costs just the ticks the
 * handle crossed since the last frame. Backward seeks still rewind through the
 * snapshot — there is no way to un-tick — so callers throttle those. */
static void lv_screenSeekToAbsoluteMs(uint32_t targetTime) {
  size_t snapPos;
  uint32_t snapTime;
  BYTE key;
  BYTE *pTeams = NULL;

  if (targetTime >= g_lv->timeRunning && g_lv->logLoaded == TRUE) {
    /* Already there: nothing to decode, and no state to disturb. */
    if (targetTime == g_lv->timeRunning) {
      return;
    }
    g_lv->isPlaying = TRUE;
    /* A user scrub parks the live-DVR view in the past (consumed next frame),
       exactly as the restore path below does. */
    s_specSeekPark = true;
    g_lv->fastForwarding = TRUE;
    while (g_lv->timeRunning < targetTime && g_lv->isPlaying == TRUE) {
      lv_screenLogTick();
    }
    g_lv->fastForwarding = FALSE;
    /* Same reason as the restore path: drain what the fast-forward queued so
       the newswire is not still scrolling out pre-seek text afterwards. */
    lv_messageDrainQueue();
    lv_rulesRefresh();
    lv_presRebuild(g_lv->timeRunning);
    return;
  }

  if (lv_snapshotFindByTime(&g_lv->snap, targetTime, &snapPos, &snapTime, &key, &pTeams)) {
    g_lv->timeRunning = snapTime;
    lv_logSetPosition(snapPos);
    lv_blocksSetKey(key);
    /* Restore team colours before processing snapshot so that new players
       appearing in the snapshot get a valid team assigned instead of being
       overwritten with NO_TEAM_SET from the pre-snapshot pTeams. */
    lv_playersSetTeams(pTeams);
    lv_processSnapshot();
    /* The snapshot carries no rules: read them again at its time, so the
       fast-forward below starts from the values in force there. */
    lv_rulesRefresh();
    lv_presRebuild(g_lv->timeRunning);
    lv_windowRemoveEventsAfter(snapTime);
    g_lv->isPlaying = TRUE;
    g_lv->state = lv_lr_start;

    /* Reset the scrolling-newswire marquee. Both the pending queue and
     * the visible cells carry forward across a seek; without this the
     * marquee keeps scrolling out characters from messages emitted
     * before the seek long after we've jumped past their time. The
     * fast-forward below re-queues every event between the snapshot
     * and the target. */
    lv_messageDestroy();
    lv_messageCreate();

    /* A user scrub parks the live-DVR view in the past (consumed next frame). */
    s_specSeekPark = true;

    /* Fast-forward from snapshot to target time */
    g_lv->fastForwarding = TRUE;
    while (g_lv->timeRunning < targetTime && g_lv->isPlaying == TRUE) {
      lv_screenLogTick();
    }
    g_lv->fastForwarding = FALSE;

    /* Drain whatever the fast-forward queued straight into the visible
     * cells. Without this, the user would have to wait for tens of
     * seconds of accumulated text to scroll past at wall-clock pace
     * before fresh events show up. After the drain the visible row
     * holds the tail of the [snapshot..target] message stream — i.e.
     * the most recent message(s) at the seek point — and the queue is
     * empty so the next live message starts scrolling in normally. */
    lv_messageDrainQueue();
    lv_rulesRefresh();
    lv_presRebuild(g_lv->timeRunning);
  }
}

/* --- Spectator live-DVR (declared in backend.h) ----------------------------
 * Head-time tracking (decision B, incremental, O(1) per record): following live
 * re-anchors the tracked head time to the decoder's true timeRunning after the
 * slam-to-head; while parked it extrapolates from the latest drained record's
 * game tick (20ms/tick) off that anchor. Any parked-time drift is wiped on the
 * next re-anchor (jump-to-live or reaching the head), so it only needs to be
 * monotonic and close. totalTimeMs is repointed at it so the existing
 * scrubber/seek math tracks the growing live head with no other change. */

static void lv_specHeadTimeFromDelta(void) {
  if (s_specHaveAnchor) {
    uint32_t d = (s_specHeadTick >= s_specAnchorTick)
                     ? (s_specHeadTick - s_specAnchorTick) : 0;
    g_lv->totalTimeMs = s_specAnchorMs + d * 20;
  }
}

static void lv_specReanchorAtHead(void) {
  s_specAnchorTick  = s_specHeadTick;
  s_specAnchorMs    = g_lv->timeRunning;
  s_specHaveAnchor  = true;
  g_lv->totalTimeMs = g_lv->timeRunning;
}

static void lv_specAdvanceToHead(void) {
  while (g_lv->isPlaying == TRUE &&
         lv_logGetCurrentPosition() < lv_logGetTotalSize()) {
    lv_screenLogTick();
  }
}

void lv_screenSpecSetLiveMode(bool on) {
  s_specLiveMode    = on;
  s_specFollowLive  = true;
  s_specSeekPark    = false;
  s_specHeadTick    = 0;
  s_specAnchorTick  = 0;
  s_specAnchorMs    = 0;
  s_specHaveAnchor  = false;
  s_specPaceAccumMs = 0;
  s_specPaceLastMs  = 0;
  /* A stream is not a round with a lobby in front of it; drop any game-start
     offset left behind by a file loaded earlier in this session, and the
     slot names that came with it — a feed has no file to scan for its own. */
  s_gameStartMs     = 0;
  memset(s_slotNames, 0, sizeof(s_slotNames));
  if (on) {
    /* The seed left the decoder at the head; play by default. The first
       follow-live frame re-anchors head time once a record tick is known. */
    g_lv->playIsPlaying = TRUE;
  }
}

bool lv_screenSpecIsLiveMode(void) {
  return s_specLiveMode;
}

void lv_screenSpecNoteHeadTick(uint32_t gameTick) {
  /* Monotonic within a segment. A world reset (new lobby/map) regresses the tick
     below the head; the host detects that and calls lv_screenSpecResetSegment,
     which zeroes the head so the new segment's first tick is captured here. */
  if (gameTick >= s_specHeadTick) {
    s_specHeadTick = gameTick;
  }
}

uint32_t lv_screenSpecHeadTick(void) {
  return s_specHeadTick;
}

/* Reset the DVR at a segment boundary (a world reset: new lobby/map). The live
   buffer and decoder are rebuilt by the re-seed the host runs straight after
   this; here we drop the previous segment's seek index so scroll-back cannot
   cross into the old map, and restart the head/anchor/follow state so head-time
   tracking resumes from the new segment's first tick and the view follows the
   new head. Live-mode only — standalone .wbv playback never calls this. */
void lv_screenSpecResetSegment(void) {
  if (!s_specLiveMode) {
    return;
  }
  lv_snapshotDestroy(&g_lv->snap);
  g_lv->snap        = lv_snapshotCreate();
  s_firstWorldSnapKnown = FALSE;
  s_specFollowLive  = true;
  s_specSeekPark    = false;
  s_specHeadTick    = 0;
  s_specAnchorTick  = 0;
  s_specAnchorMs    = 0;
  s_specHaveAnchor  = false;
  s_specPaceAccumMs = 0;
  s_specPaceLastMs  = 0;
}

void lv_screenSpecJumpToLive(void) {
  if (!s_specLiveMode) {
    return;
  }
  lv_specAdvanceToHead();
  lv_specReanchorAtHead();
  s_specFollowLive = true;
  s_specSeekPark   = false;
}

void lv_screenSpecFrameUpdate(uint32_t nowMs) {
  bool playing;
  if (!s_specLiveMode) {
    return;
  }

  /* A scrubber drag or rewind parks the view in the past. */
  if (s_specSeekPark) {
    s_specSeekPark    = false;
    s_specFollowLive  = false;
    s_specPaceLastMs  = nowMs;
    s_specPaceAccumMs = 0;
  }

  playing = (g_lv->playIsPlaying == TRUE);

  if (s_specFollowLive) {
    if (playing) {
      lv_specAdvanceToHead();   /* slam to the live head */
      lv_specReanchorAtHead();  /* head time == true decoder time here */
    } else {
      lv_specHeadTimeFromDelta(); /* frozen; the head keeps growing */
    }
    s_specPaceLastMs = nowMs;
  } else {
    /* Parked: keep the slider's max tracking the growing head either way. */
    lv_specHeadTimeFromDelta();
    if (playing) {
      if (nowMs > s_specPaceLastMs) {
        s_specPaceAccumMs += (int32_t)(nowMs - s_specPaceLastMs);
      }
      s_specPaceLastMs = nowMs;
      if (s_specPaceAccumMs > 200) {
        s_specPaceAccumMs = 200; /* cap catch-up after a stall/hitch */
      }
      while (s_specPaceAccumMs >= 20 && g_lv->isPlaying == TRUE &&
             lv_logGetCurrentPosition() < lv_logGetTotalSize()) {
        lv_screenLogTick();
        s_specPaceAccumMs -= 20;
      }
      /* Paced playback reached the head -> resume following it. */
      if (lv_logGetCurrentPosition() >= lv_logGetTotalSize()) {
        lv_specReanchorAtHead();
        s_specFollowLive = true;
      }
    } else {
      s_specPaceLastMs = nowMs;
    }
  }
}

int32_t lv_screenGetGameTimeLeft() {
  return g_lv->gmeLength;
}

int32_t lv_screenGetGameStartDelay() {
  return g_lv->gmeStartDelay;
}


BYTE lv_screenGetNumPlayers() {
  return lv_playersGetNumPlayers();
}

void lv_screenGetPlayerName(char *name, BYTE playerNum, size_t destSize) {
  lv_playersGetPlayerName(playerNum, name, destSize);
}

bool lv_screenGetLoggedPlayerName(BYTE slot, char *dest, size_t destSize) {
  if (dest == NULL || destSize == 0) return FALSE;
  dest[0] = '\0';
  if (slot >= MAX_TANKS || s_slotNames[slot][0] == '\0') return FALSE;
  snprintf(dest, destSize, "%s", s_slotNames[slot]);
  return TRUE;
}

void lv_screenGetMapName(char *dest) {
  strncpy(dest, g_lv->mapName, sizeof(g_lv->mapName) - 1);
  dest[sizeof(g_lv->mapName) - 1] = '\0';
}

/*********************************************************
*NAME:          lv_screenSaveMap
*AUTHOR:        John Morrison
*CREATION DATE:  5/2/99
*LAST MODIFIED: 31/10/99
*PURPOSE:
* Saves the map. Returns whether the operation was
* sucessful or not.
*
*ARGUMENTS:
*  fileName - path and filename to save
*  saveOwnerships - Do we save ownerships or not
*********************************************************/
bool lv_screenSaveMap(char *fileName, bool saveOwnerships) {
  return lv_mapWrite(fileName, &g_lv->mp, &g_lv->pb, &g_lv->bs, &g_lv->ss, saveOwnerships);
}


BYTE lv_screenGetPillTeam(BYTE x, BYTE y, BYTE *pillHealth) {
  pillbox p;
  BYTE itemNum = lv_pillsItemNumAt(&g_lv->pb,(BYTE) (x+g_lv->xOffset), (BYTE) (y+g_lv->yOffset));

  p.owner = NEUTRAL;
  p.armour = 15;
  lv_pillsGetPill(&g_lv->pb, &p, (BYTE) (itemNum+1));
 *pillHealth = p.armour;
  if (p.owner == NEUTRAL) {
    return NEUTRAL_TEAM;
  }

  return lv_playersGetTeamForOwner(p.owner);
}

BYTE lv_screenGetBaseTeam(BYTE x, BYTE y) {
  base b;
  BYTE itemNum = lv_basesItemNumAt(&g_lv->bs, (BYTE) (x+g_lv->xOffset), (BYTE) (y+g_lv->yOffset));

  b.owner = NEUTRAL;
  lv_basesGetBase(&g_lv->bs, &b, (BYTE) (itemNum+1));
  if (b.owner == NEUTRAL) {
    return NEUTRAL_TEAM;
  }

  return lv_playersGetTeamForOwner(b.owner);
}
