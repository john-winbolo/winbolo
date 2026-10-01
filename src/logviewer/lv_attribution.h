/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*********************************************************
 *Name:          Log Viewer Attribution Track
 *Filename:      lv_attribution.h
 *Purpose:
 *  Parse-and-hold the attribution.trk member the server writes
 *  into a .wbv (see attribution_track.h). The log viewer loads
 *  the member after log.dat, validates its header, and keeps the
 *  header plus the raw record stream behind these accessors.
 *  Parse only — no record interpretation or stat derivation here.
 *  A .wbv without the member, or with a newer track version,
 *  leaves the accessors reporting "absent".
 *********************************************************/
#ifndef LV_ATTRIBUTION_H
#define LV_ATTRIBUTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "attribution_track.h"

/* Parse a raw attribution-track member (the bytes of the zip member, i.e.
 * AttrTrackHeader followed by the packed record stream). Returns true and
 * stores the track if the header is valid and the version is supported;
 * returns false and clears otherwise. Takes a private copy of the bytes. */
bool lvAttributionParseMember(const uint8_t *data, size_t len);

/* Drop any parsed track (also called before each new parse). */
void lvAttributionClear(void);

/* NULL when no valid track is loaded. */
const AttrTrackHeader *lvAttributionGetHeader(void);

/* The packed record stream that followed the header; NULL/0 when absent. */
const uint8_t *lvAttributionGetRecords(size_t *outLen, uint32_t *outCount);

#endif /* LV_ATTRIBUTION_H */
