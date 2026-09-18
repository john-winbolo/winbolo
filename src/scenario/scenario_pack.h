/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Scenario Pack
 *Filename:      scenario_pack.h
 *Author:        John Morrison
 *Purpose:
 *  Puts the scenario beside a map into the map: the script
 *  is read, the manifest is derived from the table that
 *  script declares, and the two go into a WBSC container
 *  written on to the end of the map file.
 *
 *  Nothing writes a manifest by hand. It comes from the
 *  parse the validator already does, so what a package says
 *  about itself and what its script says are one read of one
 *  file, and the check the host makes at load — the manifest
 *  against the script's own table — cannot be failed by a
 *  package written here.
 *
 *  This is in the library rather than in the server's main
 *  so the four things that are easy to get wrong can be
 *  tested: what is refused, what the manifest holds, where a
 *  container already on the file goes, and what a pack that
 *  stops halfway leaves behind.
 *********************************************************/

#ifndef SCENARIO_PACK_H
#define SCENARIO_PACK_H

#include <stdbool.h>
#include <stddef.h>

/* Pack the scenario beside mapPath into mapPath itself: derive the manifest
   from the script's scenario table, build the container, and write the map
   followed by it. Any container already on the file is replaced, so packing
   twice gives the same bytes. False with err set and the file untouched when
   there is nothing to pack or the script has problems. */
bool scnPackMap(const char *mapPath, char *err, size_t errLen);

#endif /* SCENARIO_PACK_H */
