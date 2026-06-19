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
 *Name:          Log internals
 *Filename:      log_internal.h
 *Author:        John Morrison
 *Purpose:
 *  T2 entry points of the replay-log writer that take sim
 *  internals. These cannot live in public/log.h (T1) because
 *  they reference SpectatorRing, a T2 type.
 *
 *  T2 (sim internals): includable within src/bolo/,
 *  src/server/, and tests/unit/ only.
 *********************************************************/

#ifndef LOG_INTERNAL_H
#define LOG_INTERNAL_H

#include "global.h"
#include "types.h"
#include "server_sim.h"
#include "spectator_ring.h"

/* Upper bound on the plaintext snapshot body (everything after the
 * LOG_EVENT_SNAPSHOT marker). startDelay and timeLeft are 4 bytes each; the
 * pill, base and start blocks are a 1-byte length plus a BYTE-max payload; each
 * of MAX_TANKS player blocks is the same shape. A map run is a 4-byte header
 * plus its body, and the smallest run covers a single square, so the map is
 * bounded by one worst-case 5-byte run per map square plus the deep-sea
 * terminator. */
#define LOG_SNAPSHOT_BODY_MAX                                                  \
  (2 * (int)sizeof(int32_t) + 3 * (1 + 0xFF) + MAX_TANKS * (1 + 0xFF) +        \
   MAP_ARRAY_SIZE * MAP_ARRAY_SIZE * 5 + 8)

/* Register a spectator ring fed by the log writer. While a ring is
 * registered, each logWriteTick records one ring tick for the registered
 * sim — a keyframe (full snapshot body) when the ring asks for one, else
 * that tick's accumulated event bytes. Passing NULL for either argument
 * disables the tap and clears the per-tick event accumulator. */
void logSetSpectatorRing(SpectatorRing *ring, ServerSim *sim);

/* Write the snapshot body (everything after the LOG_EVENT_SNAPSHOT marker)
 * as plaintext into out, returning the byte count or -1 if cap is too small.
 * Exposed so callers can build a keyframe identical to the .wbv snapshot. */
int logSerializeSnapshotBody(ServerSim *ssim, BYTE *out, int cap);

#endif /* LOG_INTERNAL_H */
