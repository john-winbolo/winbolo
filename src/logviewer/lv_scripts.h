/*
 * lv_scripts.h - the scripts and rule changes a recording carries
 *
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The recording's scripts.json holder, its log_RuleSet list, and the
 * accessors that read them at the playhead.
 *
 * Deliberately plain C with nothing but sim_rules_names.h for its rule
 * count: this file exists so the Game Information panel can read these
 * without the bolo headers or backend.h, as game_settings_blob.h does for
 * the settings blob. logviewer.h includes it for everything else.
 */

#ifndef LV_SCRIPTS_H
#define LV_SCRIPTS_H

#include <stdbool.h>
#include <stdint.h>
#include "sim_rules_names.h"   /* SIM_RULE_COUNT — LvScripts' rules */

#ifdef __cplusplus
extern "C" {
#endif

/* One log_RuleSet the recording holds: the absolute log time it lands at,
   the rule's index in sim_rules_names.h and the value it set. */
typedef struct LvRuleChange {
  uint32_t ms;
  int      index;
  double   value;
} LvRuleChange;

/* Most rule changes the viewer keeps for one recording. The load walk stops
   collecting at this many and says so in ruleChangesTruncated. */
#define LV_RULE_CHANGES_MAX 256

/* What a recording's scripts.json says the round ran (docs/replay-format.md,
   "scripts.json"), read once when the recording is opened. Every count is
   clamped to its array and every string is cut to its buffer and terminated,
   because the member is a file anyone could have written. All zero, with
   present false, for a recording without the member, one over the cap, one
   that is not the JSON the writer produces and one of another version. */
#define LV_SCRIPTS_MAX              10
#define LV_SCRIPTS_REGIONS_MAX      64
#define LV_SCRIPTS_FILE_LEN         128
#define LV_SCRIPTS_WORD_LEN         16   /* source and kind */
#define LV_SCRIPTS_NAME_LEN         64
#define LV_SCRIPTS_DESC_LEN         256
#define LV_SCRIPTS_REGION_NAME_LEN  32
#define LV_SCRIPTS_MAP_LEN          128

/* One script the round ran, in load order. name and description are its
   manifest's. */
typedef struct {
  char file[LV_SCRIPTS_FILE_LEN];
  char source[LV_SCRIPTS_WORD_LEN];
  char kind[LV_SCRIPTS_WORD_LEN];
  char name[LV_SCRIPTS_NAME_LEN];
  char description[LV_SCRIPTS_DESC_LEN];
} LvScriptRow;

/* One rule of the composed table: its index in sim_rules_names.h and the
   value the round opened on. */
typedef struct {
  int    index;
  double value;
} LvScriptRule;

/* One composed region, with the file of the script that declared it. */
typedef struct {
  char    name[LV_SCRIPTS_REGION_NAME_LEN];
  uint8_t x, y;
  uint8_t w, h;
  char    file[LV_SCRIPTS_FILE_LEN];
} LvScriptRegion;

typedef struct LvScripts {
  bool           present;      /* the member was there and parsed */
  char           map[LV_SCRIPTS_MAP_LEN];
  bool           modsEnabled;
  int            count;
  LvScriptRow    scripts[LV_SCRIPTS_MAX];
  int            ruleCount;
  LvScriptRule   rules[SIM_RULE_COUNT];
  int            regionCount;
  LvScriptRegion regions[LV_SCRIPTS_REGIONS_MAX];
} LvScripts;

/* Rules at the playhead, for the game info panel.
 *
 * lv_screenRuleValueAt answers rule index's value at absolute log time ms:
 * the last change at or before ms, else the value the round opened on in
 * scripts.json, else the classic value. lv_screenGetRuleChanges points *out
 * at the recording's changes and returns how many there are.
 * lv_screenGetScripts returns the recording's scripts.json holder, or NULL
 * while no log is loaded. */
double lv_screenRuleValueAt(int index, uint32_t ms);
int lv_screenGetRuleChanges(const LvRuleChange **out);
const LvScripts *lv_screenGetScripts(void);

/* The playhead and the game start, both in absolute log ms. The same
 * declarations as backend.h's, repeated so a reader of this header needs
 * nothing else to place a change against the playhead. */
uint32_t lv_screenGetTimeRunning(void);
uint32_t lv_screenGameStartMs(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* LV_SCRIPTS_H */
