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
#include "scenario_details.h" /* SCN_DETAILS_MAX — a file's details blob */

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

/* One listed file's details (scenario_details.h), named by the file they
 * belong to so they need not stay in step with the rows beside them: the
 * listing sorts its rows and leaves these where they were read. */
typedef struct {
    char     file[SCN_DIR_FILE_LEN];
    uint16_t len;                      /* 0 = the file declares nothing */
    uint8_t  bytes[SCN_DETAILS_MAX];
    /* The file's settings block (scenario_settings.h), which travels apart
       from the details because an older client would refuse a details blob
       with more bytes on the end. 0 = the file declares no setting. */
    uint16_t settingsLen;
    uint8_t  settings[SCN_SETTINGS_BLOB_MAX];
} ScnDirDetails;

/*********************************************************
 *NAME:          scnDirListDetails
 *PURPOSE:
 *  scnDirList, and each listed file's details as well, one
 *  ScnDirDetails per row written into details, which holds
 *  max of them. details NULL is scnDirList.
 *
 *  Kept off the rows on purpose. A row goes wherever the
 *  listing goes (the UDP list response, the lobby script
 *  list, the sim's own copy of the list) and the details
 *  are read by one dialog, one file at a time.
 *********************************************************/
int scnDirListDetails(const char *dir, ScnDirEntry *out,
                      ScnDirDetails *details, int max);

/*********************************************************
 *NAME:          scnDirReadPackage
 *PURPOSE:
 *  The manifest of the .scenario package at path, read the
 *  way the listing reads one: the manifest.json straight out
 *  of the container, with no Lua run. False for a file that
 *  cannot be read, is over the package cap, is not a
 *  container, or carries a manifest that will not parse; the
 *  reason goes to the console in one line.
 *
 *  The listing is one caller. The other is the check an
 *  uploaded package passes before it lands, so what is taken
 *  is what the listing would offer.
 *********************************************************/
bool scnDirReadPackage(const char *path, ScenarioManifest *out);

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

/*********************************************************
 *NAME:          scnDirSettingsFromManifest
 *PURPOSE:
 *  One file's settings block (scenario_settings.h), packed
 *  from a manifest already in hand into out, which holds
 *  cap bytes. Answers the length, 0 for a file that
 *  declares no setting.
 *********************************************************/
size_t scnDirSettingsFromManifest(uint8_t *out, size_t cap,
                                  const ScenarioManifest *m);

/*********************************************************
 *NAME:          scnDirDetailsFromManifest
 *PURPOSE:
 *  One file's details (scenario_details.h), packed from a
 *  manifest already in hand into out, which holds cap
 *  bytes: the rules the manifest sets, as its author wrote
 *  them, and its callbacks block with each row's type.
 *  Answers the length, 0 for a file that sets no rule and
 *  describes nothing.
 *
 *  The same two callers scnDirEntryFromManifest has: the
 *  listing, and the committed map's own script.
 *********************************************************/
size_t scnDirDetailsFromManifest(uint8_t *out, size_t cap,
                                 const ScenarioManifest *m);

#endif /* SCENARIO_DIR_H */
