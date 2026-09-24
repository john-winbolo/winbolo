/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*********************************************************
 *Name:          Scripts Record
 *Filename:      scripts_record.h
 *Author:        John Morrison
 *Purpose:
 *  The scripts.json member of a logged .wbv archive: a JSON
 *  description of the scripts a round ran — the composed
 *  rules, the regions and one row per script with its
 *  manifest. The scenario host builds the text at round boot
 *  and hands it to the sim; logStop writes it beside
 *  attribution.trk. A round that ran no script has no text
 *  and its recording has no member. The log viewer reads it.
 *
 *  Layout: docs/replay-format.md, "scripts.json".
 *
 *  Public leaf: no includes, no sim internals.
 *********************************************************/
#ifndef SCRIPTS_RECORD_H
#define SCRIPTS_RECORD_H

/* Zip member name for the scripts description inside the .wbv archive. */
#define SCRIPTS_RECORD_MEMBER "scripts.json"

/* Most bytes of text the sim holds for the member. Text over this is not
 * stored, so it is never written. */
#define SCN_RECORD_TEXT_MAX (256u * 1024u)

#endif /* SCRIPTS_RECORD_H */
