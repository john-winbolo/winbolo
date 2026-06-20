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
 *Name:          Discovery (mDNS internals)
 *Filename:      discovery_mdns.h
 *Purpose:
 *  Socket-free parse seam shared between the mDNS browser
 *  (discovery_mdns.c) and its unit test. The public async
 *  entry points live in discovery.h and stay wire-format-free;
 *  this header exposes the record accumulator + the
 *  resolved-instance -> DiscoveryServer fill so the test can
 *  drive the real parse path over a loopback socket without
 *  opening a multicast socket. mdns.h is confined to the
 *  browser TU and this internal header — it never reaches
 *  the public discovery.h surface.
 *********************************************************/

#ifndef DISCOVERY_MDNS_H
#define DISCOVERY_MDNS_H

#include "discovery.h"  /* DiscoveryServer */
#include "mdns.h"       /* mdns_entry_type_t etc. (header-only) */

/* Upper bound on TXT key/value pairs kept from one response. The contract
 * carries 11 keys; the slack absorbs unknown keys without overflowing. */
#define DISCOVERY_MDNS_TXT_MAX 24
#define DISCOVERY_MDNS_KEY_MAX 16
#define DISCOVERY_MDNS_VAL_MAX 64

typedef struct {
  char key[DISCOVERY_MDNS_KEY_MAX];
  char value[DISCOVERY_MDNS_VAL_MAX];
} DiscoveryMdnsTxt;

/* Accumulated SRV / A / TXT for one service instance, gathered from a single
 * response datagram. Self-contained (TXT strings copied in), so it stays
 * valid after the receive buffer is reused. */
typedef struct {
  bool             haveSrv;      /* an SRV for our service was seen */
  unsigned short   port;         /* SRV port (host byte order)      */
  bool             haveAddr;     /* an A record was seen            */
  struct in_addr   addr;         /* A record address                */
  bool             haveFrom;     /* sender address captured         */
  struct in_addr   from;         /* recvfrom source (A fallback)    */
  DiscoveryMdnsTxt txt[DISCOVERY_MDNS_TXT_MAX];
  size_t           txtCount;
} DiscoveryMdnsResolved;

/* mdns_record_callback_fn. Accumulates one datagram's SRV/A/TXT records for
 * the _winbolo._udp.local service into *user_data (a DiscoveryMdnsResolved
 * that the caller zero-inits per datagram). Touches no socket — pure parse
 * over the supplied buffer. Returns 0 (never stops parsing early). */
int discoveryMdnsAccumulate(int sock, const struct sockaddr *from, size_t addrlen,
                            mdns_entry_type_t entry, uint16_t query_id, uint16_t rtype,
                            uint16_t rclass, uint32_t ttl, const void *data, size_t size,
                            size_t name_offset, size_t name_length, size_t record_offset,
                            size_t record_length, void *user_data);

/* Socket-free: map a resolved instance to a DiscoveryServer following the TXT
 * key schema (map/ver/players/bases/pills/pass/mines/game/ai/lobby/locked;
 * SRV port -> port; A or sender -> address). Returns false (out untouched)
 * when no SRV was resolved. Unknown/missing keys leave their field default. */
bool discoveryMdnsFillServer(const DiscoveryMdnsResolved *r, DiscoveryServer *out);

#endif /* DISCOVERY_MDNS_H */
