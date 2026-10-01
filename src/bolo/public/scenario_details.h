/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Scenario Script Details
 *Filename:      scenario_details.h
 *Author:        John Morrison
 *Purpose:
 *  What one script file says about itself beyond its name
 *  and description, packed into one blob a lobby is sent:
 *  the rules its manifest sets and what its callbacks do.
 *
 *    [ruleCount 1]
 *    ruleCount rows of [rule 1][value 8]
 *    then the callbacks blob (scenario_callbacks.h) to the
 *    end, which may be no bytes at all
 *
 *  A value is the IEEE 754 double's bits, big-endian. A
 *  script that sets no rule and describes no callback is
 *  no bytes at all, so it costs nothing where it travels.
 *
 *  The rules are the file's OWN table, as its author wrote
 *  it, and never what a round composed out of several
 *  files. The lobby's details dialog works out on its own
 *  side which script wins a rule two of them set, from the
 *  order of the list it is showing, so the same blob serves
 *  a directory row nobody has picked, a row of the host's
 *  unsent draft and a row of the round's committed list.
 *
 *  The blob belongs to the file. A client asks the server
 *  for one file's blob by name (PACKET_LOBBY_SCENARIO_
 *  DETAILS_REQ, answered as BULK_KIND_SCENARIO_DETAILS) and
 *  keeps it per file, so the map's own script and every
 *  directory mod are found the same way, whichever list
 *  names them.
 *
 *  Here in public/ for the reason scenario_callbacks.h is:
 *  the scenario directory, the server sim, the client sim
 *  and the gui's details dialog all read these bytes and
 *  share no other header. Inline, so a reader links
 *  nothing for it.
 *********************************************************/

#ifndef SCENARIO_DETAILS_H
#define SCENARIO_DETAILS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "platform_types.h"     /* BOLO_STATIC_ASSERT */
#include "scenario_callbacks.h" /* the blob the rules are followed by */
#include "sim_rules_names.h"    /* SIM_RULE_COUNT — what a rule byte indexes */

/* One rule row: the rule's index and its value. */
#define SCN_DETAILS_RULE_BYTES 9

/* The whole blob at its largest: every rule there is, once each, and a full
 * callbacks block. A manifest names each rule at most once, so the rule count
 * is the most rows a file can have. */
#define SCN_DETAILS_MAX \
    (1 + SIM_RULE_COUNT * SCN_DETAILS_RULE_BYTES + SCN_CALLBACKS_BLOB_MAX)

/* The rule count and each rule's index travel as one byte. */
BOLO_STATIC_ASSERT(SIM_RULE_COUNT <= 255, scn_details_rule_fits_a_byte);

/* An empty blob. Rules go on with scnDetailsAddRule and then the callbacks
 * with scnDetailsAddCallbacks, in that order; scnDetailsFinish turns a blob
 * that holds neither back into no bytes at all. */
static inline void scnDetailsBegin(uint8_t *out, size_t *len) {
    out[0] = 0;
    *len   = 1;
}

/* One rule onto the blob. False, and the blob left as it was, for a rule
 * past the list, a blob with no room, or one the callbacks already follow. */
static inline bool scnDetailsAddRule(uint8_t *out, size_t cap, size_t *len,
                                     int rule, double value) {
    uint64_t bits;
    size_t   at;
    int      k;

    if (out == NULL || len == NULL || rule < 0 || rule >= SIM_RULE_COUNT) {
        return false;
    }
    at = *len;
    if (at != 1u + (size_t)out[0] * SCN_DETAILS_RULE_BYTES ||
        out[0] == 255u || at + SCN_DETAILS_RULE_BYTES > cap) {
        return false;
    }
    memcpy(&bits, &value, sizeof(bits));
    out[at++] = (uint8_t)rule;
    for (k = 7; k >= 0; k--) {
        out[at++] = (uint8_t)((bits >> (k * 8)) & 0xFFu);
    }
    out[0]++;
    *len = at;
    return true;
}

/* The callbacks blob onto the end. False for one that does not fit. */
static inline bool scnDetailsAddCallbacks(uint8_t *out, size_t cap,
                                          size_t *len, const uint8_t *cb,
                                          size_t cbLen) {
    if (out == NULL || len == NULL || (cbLen > 0 && cb == NULL) ||
        *len + cbLen > cap) {
        return false;
    }
    if (cbLen > 0) {
        memcpy(out + *len, cb, cbLen);
        *len += cbLen;
    }
    return true;
}

static inline void scnDetailsFinish(const uint8_t *out, size_t *len) {
    if (*len == 1 && out[0] == 0) {
        *len = 0;
    }
}

/* Whether len bytes are a blob: every rule row whole and naming a rule, and
 * whatever follows them a well-formed callbacks blob. Zero bytes is one.
 * Every reader of a blob off the wire asks this first, so the readers below
 * never have to. */
static inline bool scnDetailsValid(const uint8_t *blob, size_t len) {
    size_t rulesEnd;
    size_t i;

    if (len == 0) return true;
    if (blob == NULL || len > SCN_DETAILS_MAX) return false;
    rulesEnd = 1u + (size_t)blob[0] * SCN_DETAILS_RULE_BYTES;
    if (rulesEnd > len) return false;
    for (i = 0; i < blob[0]; i++) {
        if (blob[1 + i * SCN_DETAILS_RULE_BYTES] >= SIM_RULE_COUNT) {
            return false;
        }
    }
    return scnCallbacksBlobCount(blob + rulesEnd, len - rulesEnd) >= 0;
}

static inline int scnDetailsRuleCount(const uint8_t *blob, size_t len) {
    return (blob == NULL || len == 0) ? 0 : (int)blob[0];
}

/* Rule row i: its rule index and its value. False past the rows. */
static inline bool scnDetailsRuleAt(const uint8_t *blob, size_t len, int i,
                                    int *rule, double *value) {
    const uint8_t *row;
    uint64_t       bits = 0;
    double         v;
    int            k;

    if (i < 0 || i >= scnDetailsRuleCount(blob, len)) return false;
    row = blob + 1 + (size_t)i * SCN_DETAILS_RULE_BYTES;
    for (k = 1; k <= 8; k++) {
        bits = (bits << 8) | row[k];
    }
    memcpy(&v, &bits, sizeof(v));
    if (rule != NULL) *rule = row[0];
    if (value != NULL) *value = v;
    return true;
}

/* Whether the blob sets rule, and to what. */
static inline bool scnDetailsFindRule(const uint8_t *blob, size_t len,
                                      int rule, double *value) {
    int n = scnDetailsRuleCount(blob, len);
    int i;
    int r;

    for (i = 0; i < n; i++) {
        if (scnDetailsRuleAt(blob, len, i, &r, value) && r == rule) {
            return true;
        }
    }
    return false;
}

/* Which script wins a rule, when several are loaded in one order.
 *
 * blobs[i] is the details of the script at place i of the list, NULL for one
 * whose details are not known or that does not load (a mod on a server with
 * mods turned off). The first script on the list that sets the rule is the
 * one whose value plays, as scnComposeInto composes it, so this looks at the
 * places before `self` and answers the first that sets `rule`, with its value
 * in *value. -1 when none of them does, which means the script at `self`
 * sets the value that plays (if it sets one at all). A self of -1, a script
 * that is not on the list, is overridden by nothing. */
static inline int scnDetailsRuleWinner(const uint8_t *const *blobs,
                                       const size_t *lens, int self,
                                       int rule, double *value) {
    int i;

    if (blobs == NULL || lens == NULL) return -1;
    for (i = 0; i < self; i++) {
        if (blobs[i] != NULL &&
            scnDetailsFindRule(blobs[i], lens[i], rule, value)) {
            return i;
        }
    }
    return -1;
}

/* The callbacks blob inside it, which may be no bytes. */
static inline const uint8_t *scnDetailsCallbacks(const uint8_t *blob,
                                                 size_t len, size_t *cbLen) {
    size_t rulesEnd;

    *cbLen = 0;
    if (blob == NULL || len == 0) return NULL;
    rulesEnd = 1u + (size_t)blob[0] * SCN_DETAILS_RULE_BYTES;
    if (rulesEnd >= len) return NULL;
    *cbLen = len - rulesEnd;
    return blob + rulesEnd;
}

#endif /* SCENARIO_DETAILS_H */
