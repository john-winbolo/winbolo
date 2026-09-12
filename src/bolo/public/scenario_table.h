/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Scenario Table
 *Filename:      scenario_table.h
 *Author:        John Morrison
 *Purpose:
 *  The flat key/value table a bot is handed: the init
 *  table serverSimCreateBot carries down to the brain's
 *  BRAIN_INIT Lua global, and the hint table a scenario
 *  sends a bot that is already running.
 *
 *  Here rather than in scenario_api/ because it is the
 *  type of a parameter on server_sim.h's bot constructor,
 *  which frontends call — a public call cannot take a type
 *  only the scenario surface can name. scenario_defs.h
 *  includes this header and builds its op payloads on the
 *  same type; there is one definition.
 *********************************************************/

#ifndef SCENARIO_TABLE_H
#define SCENARIO_TABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The init and hint tables: how many pairs, and how long a key and a
 * value may be. */
#define SCN_TABLE_MAX         16
#define SCN_TABLE_KEY_LEN     24
#define SCN_TABLE_VALUE_LEN   64

typedef struct {
    char key[SCN_TABLE_KEY_LEN];
    char value[SCN_TABLE_VALUE_LEN];
} ScnKV;

typedef struct {
    uint8_t count;
    ScnKV   kv[SCN_TABLE_MAX];
} ScnTable;

/* Empties the table. A table with no pairs means the same as no table
 * at all: the brain's BRAIN_INIT is an empty Lua table either way. */
void scnTableClear(ScnTable *t);

/* Stores value under key, replacing the value an existing key holds.
 * Returns false and changes nothing when t is NULL, the key is empty,
 * either string is too long for its field, or a new key would be the
 * SCN_TABLE_MAX + 1st. The caller decides whether that is fatal. */
bool scnTableSet(ScnTable *t, const char *key, const char *value);

/* The value stored under key, or NULL when the key is absent. */
const char *scnTableGet(const ScnTable *t, const char *key);

/* Fills out from one line of "key=value" text: tokens separated by ','
 * or ';', each either "key=value" or a bare "flag", which is stored
 * with the value "1". Surrounding spaces are dropped. Clears out
 * first, so a NULL or empty text leaves an empty table. Returns false
 * when a token did not fit — out then holds the tokens that did.
 *
 * This is the mapping the -bot-init [arg] suffix goes through, so
 * "[ammoless;deprive=100]" reaches the brain as
 * BRAIN_INIT.ammoless == "1" and BRAIN_INIT.deprive == "100". */
bool scnTableFromArgText(const char *text, ScnTable *out);

/* Writes the table back out in scnTableFromArgText's form
 * ("key=value;key=value") for a log line. Always NUL-terminates;
 * truncates rather than overruns. An empty table writes "". */
void scnTableFormat(const ScnTable *t, char *out, size_t outCap);

#ifdef __cplusplus
}
#endif

#endif /* SCENARIO_TABLE_H */
