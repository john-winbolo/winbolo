/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef NAT_PORTMAP_H
#define NAT_PORTMAP_H

#include "global.h"

typedef struct {
  bool           mapped;
  char           method[16];
  char           externalIp[64];
  unsigned short externalPort;
  unsigned short internalPort;
} NatPortMap;

/* Async — kicks off discovery + mapping on a worker thread inside libplum.
 * Caller polls map->mapped to detect completion; map->method[0] is '\0'
 * while still in progress.  Idempotent: a second call for the same
 * NatPortMap is a no-op until natPortMapRelease. */
void natPortMapRequest(unsigned short internalPort, NatPortMap *out);

/* Drop the mapping.  Safe to call when not mapped.  Best-effort; the
 * gateway lease will expire on its own if the release packet is lost. */
void natPortMapRelease(NatPortMap *map);

/* Renew before lease expiry.  libplum handles renewal internally so this
 * is a no-op there, but the entry point exists so callers don't need to
 * know which library backs the wrapper. */
void natPortMapRenewIfNeeded(NatPortMap *map);

#endif /* NAT_PORTMAP_H */
