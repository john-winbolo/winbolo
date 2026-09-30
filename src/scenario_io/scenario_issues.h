/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Scenario Issues
 *Filename:      scenario_issues.h
 *Author:        John Morrison
 *Purpose:
 *  What is wrong with a scenario, as a list: one problem per
 *  key, line and message, and the manifest the checks ran
 *  against beside them.
 *
 *  The list is here rather than with the validator because
 *  the manifest reader fills one too, and that reader is
 *  read by a map editor and a log viewer that link no Lua.
 *  scenario_validate.h includes this file, so a caller that
 *  runs a validation sees the whole surface as it did.
 *********************************************************/

#ifndef SCENARIO_ISSUES_H
#define SCENARIO_ISSUES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "scenario_manifest.h" /* ScenarioManifest */

#define SCN_VALIDATE_ISSUES_MAX 64
#define SCN_VALIDATE_KEY_LEN    64
#define SCN_VALIDATE_MSG_LEN    512

/* One problem the validator found. key is the dotted path of the thing at
 * fault ("api", "lobby.teams[2].bots", "rules.tank_reload_ticks",
 * "regions.keep"), "" where no key applies. line is 1-based and 0 when the
 * key could not be found in the source. Messages start with the script's
 * full path, so message is sized for a long path plus the reason. */
typedef struct {
    char key[SCN_VALIDATE_KEY_LEN];
    int  line;
    char message[SCN_VALIDATE_MSG_LEN];
} ScnValidateIssue;

typedef struct {
    bool             haveManifest;   /* false when no script, or it failed to run */
    uint16_t         count;
    uint16_t         dropped;        /* problems past SCN_VALIDATE_ISSUES_MAX */
    ScnValidateIssue issues[SCN_VALIDATE_ISSUES_MAX];
    ScenarioManifest manifest;       /* what the table parsed to; also what
                                      * a package is written from */
} ScnValidateResult;

/*********************************************************
 *NAME:          scnIssueAdd
 *PURPOSE:
 *  Appends one problem to the list, with line 0 for the
 *  attribution pass to fill in. key may be "" where no key
 *  applies. A NULL out is a no-op; a list already full
 *  counts the problem in dropped and keeps the ones it has.
 *********************************************************/
void scnIssueAdd(ScnValidateResult *out, const char *key,
                 const char *fmt, ...);

/* Where a problem the parse found is said: one line to the operator and a
 * copy in soft for whoever asked, and an issue on sink when a validator is
 * collecting them. Each of the three is optional — the host passes a soft
 * buffer and no sink, and a caller that wants neither passes nothing. */
typedef struct {
    char              *soft;
    size_t             softLen;
    ScnValidateResult *sink;
} ScnParseReport;

#endif /* SCENARIO_ISSUES_H */
