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
 *Name:          Spectator Drain Stubs
 *Filename:      spectator_drain_stub.c
 *Purpose:
 *  No-op stubs for the spectator_drain.h seam, linked only into
 *  the standalone Log Viewer.
 *
 *  logviewer.c carries spectatorRun, which drives the live
 *  spectator feed through this seam. The seam's real bodies live
 *  in src/bolo/client_sim.c (forwarding to the bolo-world
 *  ClientSim), which the WinBolo client links via bolo_static.
 *  The standalone Log Viewer compiles logviewer.c but links no
 *  bolo sim, and never spectates (it only opens .wbv files), so
 *  these stubs resolve the seam symbols. They report "no seed,
 *  no records" so the loop would idle were spectatorRun ever
 *  reached here.
 *********************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "spectator_drain.h"

void specDrainPump(void *handle) {
  (void)handle;
}

bool specDrainSeedReady(void *handle) {
  (void)handle;
  return false;
}

bool specDrainTakeSeed(void *handle, uint8_t **outBlob, uint32_t *outLen) {
  (void)handle;
  (void)outBlob;
  (void)outLen;
  return false;
}

uint32_t specDrainRecordCount(void *handle) {
  (void)handle;
  return 0;
}

bool specDrainCountdown(void *handle, uint32_t *outRemaining) {
  (void)handle;
  (void)outRemaining;
  return false;
}

bool specDrainPopRecord(void *handle, SpecDrainRecord *out) {
  (void)handle;
  (void)out;
  return false;
}

bool specDrainLiveResumed(void *handle) {
  (void)handle;
  return false;
}

bool specSeedDecodeInfo(const uint8_t *seed, size_t seedLen, SpecSeedInfo *out) {
  (void)seed;
  (void)seedLen;
  if (out != NULL) {
    out->haveInfo = false;
  }
  return false;
}
