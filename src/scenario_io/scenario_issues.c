/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Scenario Issues
 *Filename:      scenario_issues.c
 *Author:        John Morrison
 *Purpose:
 *  The list a check writes its problems into. One call, so
 *  the validator and the manifest reader append to a list
 *  the same way and a caller reads one list however it was
 *  filled.
 *********************************************************/

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "scenario_issues.h"

/* ── The list ─────────────────────────────────────────────────────── */

void scnIssueAdd(ScnValidateResult *out, const char *key,
                 const char *fmt, ...) {
    ScnValidateIssue *issue;
    va_list           ap;

    if (out == NULL) {
        return;
    }
    if (out->count >= SCN_VALIDATE_ISSUES_MAX) {
        /* A count that cannot wrap. Past the list's end the number is all
           there is left to say about the rest of them. */
        if (out->dropped < UINT16_MAX) {
            out->dropped++;
        }
        return;
    }

    issue = &out->issues[out->count];
    out->count++;
    memset(issue, 0, sizeof(*issue));
    snprintf(issue->key, sizeof(issue->key), "%s", (key != NULL) ? key : "");
    va_start(ap, fmt);
    vsnprintf(issue->message, sizeof(issue->message), fmt, ap);
    va_end(ap);
}

void scnWarnAdd(ScnValidateResult *out, const char *key,
                const char *fmt, ...) {
    ScnValidateIssue *warn;
    va_list           ap;
    uint16_t          i;

    if (out == NULL || out->warnCount >= SCN_VALIDATE_WARNINGS_MAX) {
        return;
    }
    if (key == NULL) {
        key = "";
    }
    for (i = 0; i < out->warnCount; i++) {
        if (strcmp(out->warnings[i].key, key) == 0) {
            return;
        }
    }

    warn = &out->warnings[out->warnCount];
    out->warnCount++;
    memset(warn, 0, sizeof(*warn));
    snprintf(warn->key, sizeof(warn->key), "%s", key);
    va_start(ap, fmt);
    vsnprintf(warn->message, sizeof(warn->message), fmt, ap);
    va_end(ap);
}
