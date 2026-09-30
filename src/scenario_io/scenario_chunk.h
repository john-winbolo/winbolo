/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Scenario Chunk
 *Filename:      scenario_chunk.h
 *Author:        John Morrison
 *Purpose:
 *  Puts a scenario on to a map file: the manifest as JSON
 *  and the script beside it, framed as a WBSC container and
 *  written after the map data.
 *
 *  The manifest is written as it is handed over. No field is
 *  filled in on the way out — a table that states no game
 *  leaves the manifest's game empty, because the host holds
 *  a package's manifest against the table it was read from
 *  and anything added here would make a package that refuses
 *  itself.
 *
 *  Nothing here reads a script or decides whether one is fit
 *  to pack. The caller has a manifest and a script already;
 *  where they came from is its business.
 *********************************************************/

#ifndef SCENARIO_CHUNK_H
#define SCENARIO_CHUNK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "scenario_manifest.h" /* ScenarioManifest */

/* A map path plus the name the write goes through. Map paths reach the
 * server from a command line, so this matches what those buffers hold:
 * SCN_SCRIPT_PATH_MAX in scenario_host.h is the same number on the
 * runtime's side of the fence, which this file cannot see.
 * scenario_pack.c sees both and holds them against each other. */
#define SCN_IO_MAP_PATH_MAX 2304

/* Writes the scenario chunk on to the map file: the manifest as JSON and
   the script, into a WBSC container, replacing any container already on
   the file so packing twice gives the same bytes. False with err set and
   the file untouched on any failure. */
bool scnIoWriteMapChunk(const char *mapPath, const ScenarioManifest *m,
                        const char *script, size_t scriptLen,
                        char *err, size_t errLen);

/* Stamp a Workshop item id and author into a scenario file that already
   exists: a .scenario package, or a .map carrying a scenario chunk. Only
   manifest.json changes; every other entry and every manifest key this
   build does not know is kept. The write goes through a temporary file and
   a rename, so a failure leaves the file as it was. A loose .lua, a map
   with no chunk, and anything else answer false with err set. */
bool scnIoSetWorkshopId(const char *path, uint64_t id, uint64_t author,
                        char *err, size_t errLen);

#endif /* SCENARIO_CHUNK_H */
