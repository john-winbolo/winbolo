/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
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
