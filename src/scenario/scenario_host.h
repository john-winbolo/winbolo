/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Scenario Host
 *Filename:      scenario_host.h
 *Author:        John Morrison
 *Purpose:
 *  What a frontend sees of a scenario: attach one to a sim
 *  from a map path, detach it, ask whether one is active
 *  and what it calls itself.
 *
 *  This header is included by binaries that compile under
 *  gui and runtime_only, which see src/bolo/public/ and
 *  nothing else, so it includes only public headers and
 *  names no type from src/bolo/scenario_api/. The write
 *  funnel, the op types and the manifest the host parses
 *  are the library's own business and stay inside it.
 *********************************************************/

#ifndef SCENARIO_HOST_H
#define SCENARIO_HOST_H

#include <stdbool.h>
#include <stddef.h>

#include "server_sim.h" /* ServerSim, and MAX_TANKS / MAX_PILLS / MAX_BASES /
                         * MAX_STARTS through global.h and types.h */

/* The API version this server implements. A sidecar states the version it
 * was written against as scenario.api; one written against a newer server
 * than this is refused rather than half-understood. */
#define SCENARIO_API_VERSION 1

/* Tags and regions, at the sizes the scenario table is specified with. A
 * tag names a pill, base or start; a region names a rectangle of map
 * squares. Both are read into the manifest here and mean nothing to the
 * engine yet. */
#define SCN_TAG_LEN          32
#define SCN_TAGS_PER_ENTITY   4
#define SCN_REGIONS_MAX      64
#define SCN_REGION_NAME_LEN  32

/* The text fields of the scenario table. The name is what a lobby row
 * shows and the description what a tooltip or an info line shows, so they
 * are sized for a line rather than for prose. */
#define SCN_SCENARIO_NAME_LEN 64
#define SCN_SCENARIO_DESC_LEN 256

/* scenario.game names a game type ("open", "tournament", "strict"), kept
 * as the text the file gave. */
#define SCN_GAME_NAME_LEN 24

/* A brain named by a lobby team, as a path or a "package:NAME". The same
 * length the spawn op carries a brain in; scenario_host.c holds the two
 * against each other where it can see both. */
#define SCN_BRAIN_LEN 256

/* Room for every rule the table can name. scenario_host.c checks this
 * covers the rule list, so a rule added to the list cannot overflow it. */
#define SCN_MANIFEST_RULES_MAX 128

/* A map path plus the sidecar suffix. Map paths reach the server from a
 * command line, so this matches what those buffers hold. */
#define SCN_SIDECAR_PATH_MAX 2304

/* The sidecar a map is looked for beside: X.map is accompanied by
 * X.scenario.lua. */
#define SCN_SIDECAR_SUFFIX ".scenario.lua"

/* The most a sidecar may hold. It is hand-written Lua, so a megabyte is far
 * more than one ever needs and still well inside the four a whole packaged
 * scenario is allowed on the wire. A file above it is refused rather than
 * read, because at that size it is not a script. */
#define SCN_SIDECAR_MAX_BYTES (1024 * 1024)

/* How many hook or policy calls may raise in a row before the scenario is
 * switched off for the rest of the round. Any call that returns normally
 * puts the count back to zero, so this counts a script that is failing
 * every time rather than one that fails now and then.
 *
 * A round the scenario is off for runs no more of its Lua: hooks are
 * skipped, policies answer the classic rule, and the players are told once.
 * It is off for the round and not for the attachment — the next round start
 * boots a fresh VM and begins again at zero. */
#define SCN_ERROR_LIMIT 20

/* How many events the host holds between one tick and the next, across
 * both of the server's channels. Each of the two subscriber callbacks
 * copies an event in and returns; the one drain at the end of the tick
 * empties exactly what was waiting when it started.
 *
 * An event that arrives with the queue full is dropped rather than
 * overwriting one, and each drop counts toward SCN_ERROR_LIMIT: a round
 * that produces more events in one tick than this is producing them faster
 * than a script can answer them, and the numbers say so rather than the
 * oldest of them going quietly. */
#define SCN_EVENT_QUEUE_MAX 256

typedef struct ScenarioHost ScenarioHost;

/*********************************************************
 *NAME:          scenarioHostAttach
 *PURPOSE:
 *  Looks for a sidecar beside mapPath, reads it, boots a VM,
 *  runs its chunk and reads its scenario table. Registers
 *  itself on the sim, so the round start that follows applies
 *  the scenario's rules.
 *
 *  The file is read once, here. The host keeps the bytes and
 *  every later round runs those, so editing the file while a
 *  server is up changes nothing until something asks the host
 *  to read it again.
 *
 *  Returns NULL when there is no sidecar, which is the
 *  ordinary case and not an error: err is left empty and the
 *  server runs a plain map. Returns NULL on a sidecar that
 *  cannot be used — one too large to be a script, a syntax
 *  error, an error raised by the chunk, no scenario table, or
 *  an api newer than this server — and writes one operator
 *  line to err saying which, with the file and the line where
 *  Lua has one.
 *
 *  err may be NULL only when errLen is 0.
 *********************************************************/
ScenarioHost *scenarioHostAttach(ServerSim *sim, const char *mapPath,
                                 char *err, size_t errLen);

/*********************************************************
 *NAME:          scenarioHostReload
 *PURPOSE:
 *  Reads the sidecar from disk again and, if the new bytes
 *  are usable, keeps them in place of the ones the host was
 *  holding. Usable means: inside SCN_SIDECAR_MAX_BYTES, the
 *  chunk loads and runs, a scenario table comes out of it,
 *  and its api is not above this server's.
 *
 *  Checked in a Lua state of its own before anything is
 *  swapped, so a bad edit changes nothing: on any failure
 *  this returns false, writes one operator line to err, and
 *  leaves the running scenario exactly as it was.
 *
 *  On success the new bytes take effect at the next round
 *  start. The round in progress keeps the table it began
 *  with, so a caller telling an operator what happened
 *  should say so.
 *********************************************************/
bool scenarioHostReload(ScenarioHost *h, char *err, size_t errLen);

/*********************************************************
 *NAME:          scenarioHostDetach
 *PURPOSE:
 *  Takes the host off the sim, closes its VM, drops the
 *  sidecar bytes it was holding and frees it.
 *  NULL is a no-op, so a caller that attached nothing can
 *  detach unconditionally on the way out.
 *********************************************************/
void scenarioHostDetach(ScenarioHost *h);

/*********************************************************
 *NAME:          scenarioHostIsActive
 *PURPOSE:
 *  Whether a scenario is attached and running. False for a
 *  NULL host, so a caller need not test both.
 *********************************************************/
bool scenarioHostIsActive(const ScenarioHost *h);

/*********************************************************
 *NAME:          scenarioHostName
 *PURPOSE:
 *  The scenario's name, as the lobby shows it. "" when no
 *  scenario is attached; never NULL.
 *********************************************************/
const char *scenarioHostName(const ScenarioHost *h);

/*********************************************************
 *NAME:          scenarioHostDescription
 *PURPOSE:
 *  The scenario's description line. "" when no scenario is
 *  attached; never NULL.
 *********************************************************/
const char *scenarioHostDescription(const ScenarioHost *h);

/*********************************************************
 *NAME:          scenarioHostSidecarPath
 *PURPOSE:
 *  The sidecar file the host read, for an operator asking
 *  which file is in play. "" when no scenario is attached;
 *  never NULL.
 *********************************************************/
const char *scenarioHostSidecarPath(const ScenarioHost *h);

/*********************************************************
 *NAME:          scenarioHostLastError
 *PURPOSE:
 *  The most recent operator line the host produced after
 *  attach — a rule the funnel refused, a key that names no
 *  rule, a round start the cached chunk failed on.
 *  "" when the scenario has had nothing to say; never NULL.
 *
 *  Attach reports through its own err buffer rather than
 *  here, because a failed attach returns no host to ask.
 *********************************************************/
const char *scenarioHostLastError(const ScenarioHost *h);

#endif /* SCENARIO_HOST_H */
