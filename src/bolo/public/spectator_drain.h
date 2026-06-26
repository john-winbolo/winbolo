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

/*
 * spectator_drain — a dependency-free seam for pulling the captured spectator
 * seed blob and forward records off a bolo-world ClientSim.
 *
 * The logviewer-world spectator host needs to drain the records the client
 * captured while connected as a tankless spectator, but it cannot include
 * client_sim.h: that header transitively pulls viewport_types.h, whose
 * `struct screenObj` (a 2-D array) hard-conflicts with backend.h's `screenObj`
 * (a BYTE*) in any logviewer translation unit. This header is the neutral
 * crossing point — it pulls only <stdint.h>/<stdbool.h>/<stddef.h> and names
 * no bolo or logviewer type.
 *
 * The handle is an opaque `void *` that the caller obtains as a ClientSim *;
 * the wrappers (implemented bolo-side in client_sim.c) cast it back and forward
 * to the matching clientSimSpectator* accessor. SpecDrainRecord mirrors
 * ClientSpectatorRecord field-for-field.
 *
 * Ownership: both specDrainTakeSeed (*outBlob) and specDrainPopRecord
 * (out->payload) transfer heap ownership to the caller, which must free() it.
 */

#ifndef SPECTATOR_DRAIN_H
#define SPECTATOR_DRAIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One captured forward record, mirroring ClientSpectatorRecord. The server's
 * 9-byte transport header is already stripped. payload is owned by the caller
 * (free it) and is NULL when payloadLen is 0. */
typedef struct {
  bool      isKeyframe;
  uint32_t  gameTick;
  uint32_t  segment;
  uint8_t  *payload;     /* caller-owned; NULL when payloadLen == 0 */
  uint32_t  payloadLen;
} SpecDrainRecord;

/* handle is a ClientSim * passed as an opaque void *. */

/* Pump the spectator transport one step (forwards to clientSimNetTick): the
 * logviewer-world host calls this each frame to service the connection and
 * refill the seed/record queues without including client_sim.h. */
void     specDrainPump(void *handle);

/* True once the captured seed blob has been fully received. */
bool     specDrainSeedReady(void *handle);

/* Hand the seed blob to the caller, transferring ownership (*outBlob must be
 * freed). Returns false (outputs untouched) if no seed is ready; the feed no
 * longer holds the seed after a successful take. */
bool     specDrainTakeSeed(void *handle, uint8_t **outBlob, uint32_t *outLen);

/* Number of forward records currently queued. */
uint32_t specDrainRecordCount(void *handle);

/* Latest cold-start countdown the server sent while the delayed ring fills.
 * Returns true and writes *outRemaining (remaining game ticks, ~50/sec) once a
 * countdown has arrived; false (untouched) before the first one. Lets the host
 * show a "spectating begins in N" overlay during the pre-seed wait. */
bool     specDrainCountdown(void *handle, uint32_t *outRemaining);

/* Pop the oldest queued forward record into *out (ownership of out->payload
 * passes to the caller — free it). Returns false (out untouched) when the
 * queue is empty. */
bool     specDrainPopRecord(void *handle, SpecDrainRecord *out);

/* Game-info decoded from a spectator seed's control snapshot. A spectator
 * never receives the lobby-settings control packet on the wire, but the seed's
 * sync-replay control slice carries the same lobby-settings event, so the host
 * can recover the map name / game settings from the seed itself. mapName is
 * NUL-terminated; haveInfo is false when the seed has no lobby-settings event.
 * Only the fields the lobby-settings event carries are filled — version /
 * maxPlayers / password aren't in it and stay zero. */
typedef struct {
  bool    haveInfo;
  char    mapName[64];       /* server map name is <= MAP_STR_SIZE (36) */
  uint8_t gameType;
  uint8_t allowHiddenMines;
  uint8_t ai;
} SpecSeedInfo;

/* Decode the lobby/game-info from a raw spectator seed blob
 * ([u32 bodyLen BE][world body][u32 ctrlLen BE][control snapshot]) into *out.
 * Walks the control snapshot's [u16 type][u16 len][body] records for the
 * lobby-settings event and decodes it. Returns true (and sets out->haveInfo)
 * when that event is found; false (out zeroed) otherwise. Pure byte decode —
 * no ClientSim handle needed. */
bool     specSeedDecodeInfo(const uint8_t *seed, size_t seedLen, SpecSeedInfo *out);

#endif /* SPECTATOR_DRAIN_H */
