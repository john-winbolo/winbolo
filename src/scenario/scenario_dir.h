/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Scenario Dir
 *Filename:      scenario_dir.h
 *Purpose:
 *  What a server has to offer out of its scenarios
 *  directory, independently of any map.
 *
 *  Two kinds of file are read. A .scenario is a WBSC
 *  container held on its own rather than appended to a map,
 *  and its manifest.json is read straight out of it — no
 *  Lua runs to list one. A loose .lua is read the way the
 *  validator reads a script beside a map: the chunk's top
 *  level runs once in a state with a game table that answers
 *  nothing, and the scenario table it declares is the
 *  manifest. Anything else in the directory is skipped.
 *
 *  A script with problems against it is still listed. What
 *  the list is for is saying what is there, and a host who
 *  cannot see a file they just dropped in has no way to
 *  learn why; -validate is where the problems are read.
 *
 *  Nothing here selects a scenario or starts a round.
 *********************************************************/

#ifndef SCENARIO_DIR_H
#define SCENARIO_DIR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "scenario_defs.h" /* ScnDirEntry and its three lengths: the sim is
                            * handed these, so the type is the shared one
                            * rather than one of this library's own */
#include "scenario_manifest.h" /* ScenarioManifest — one row is filled from
                                * one, and the type is a typedef of an
                                * unnamed struct, so it cannot be named
                                * ahead of itself */

/* The two extensions a scenario is offered as. */
#define SCN_SCENARIO_PACKAGE_EXT ".scenario"
#define SCN_SCENARIO_SCRIPT_EXT  ".lua"

/*********************************************************
 *NAME:          scnDirList
 *PURPOSE:
 *  Every scenario in dir, in file-name order. Answers how
 *  many entries were written, or -1 when the directory
 *  cannot be read — which is not a fault on its own: a
 *  server with no scenarios directory offers no mods, and
 *  that is the ordinary case.
 *
 *  A file that cannot be read, or whose manifest will not
 *  parse, is left out and named in one console line. The
 *  files after it are still listed.
 *********************************************************/
int scnDirList(const char *dir, ScnDirEntry *out, int max);

/*********************************************************
 *NAME:          scnDirEntryFromManifest
 *PURPOSE:
 *  One row of the list, filled from a manifest already in
 *  hand. The listing above is one caller; the other is the
 *  committed map's own script, which is published as a row
 *  of the lobby's list and is in no directory.
 *
 *  file is the name the row carries, and is a file name
 *  rather than a path.
 *
 *  Here rather than written twice so that the row a map's
 *  script shows is the row the same file would show if it
 *  sat in the scenarios directory.
 *********************************************************/
void scnDirEntryFromManifest(ScnDirEntry *e, const char *file,
                             const ScenarioManifest *m);

#endif /* SCENARIO_DIR_H */
