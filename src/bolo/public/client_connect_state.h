/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * client_connect_state.h
 *
 * Connection-state enum used by the network-binding layer of ClientSim.
 * Surfaces in clientSimGetConnectState() to let callers run the join
 * handshake without holding a Transport pointer themselves.
 */

#ifndef CLIENT_CONNECT_STATE_H
#define CLIENT_CONNECT_STATE_H

typedef enum {
  CLIENT_CONNECT_DISCONNECTED,
  CLIENT_CONNECT_JOINING,
  CLIENT_CONNECT_DOWNLOADING_MAP,
  CLIENT_CONNECT_CONNECTED,
  CLIENT_CONNECT_ERROR,
  CLIENT_CONNECT_SERVER_SHUTDOWN,
  CLIENT_CONNECT_KICKED,
  /* Tankless spectator: the join was accepted with the 0xFF no-slot
   * sentinel, so no tank slot was claimed, no map was downloaded, and the
   * client is not in the live snapshot-apply pipeline. The client sits here
   * awaiting (and then consuming) the spectator seed + forward feed; it never
   * transitions to CONNECTED because it is not a player. Appended last so the
   * existing values keep their numbering. */
  CLIENT_CONNECT_SPECTATING
} ClientConnectState;

/* Legacy spellings — the transport_udp.h identifiers continue to work
 * during the transition. A later cleanup retires these. */
typedef ClientConnectState UdpClientJoinState;
#define UDP_CLIENT_DISCONNECTED    CLIENT_CONNECT_DISCONNECTED
#define UDP_CLIENT_JOINING         CLIENT_CONNECT_JOINING
#define UDP_CLIENT_DOWNLOADING_MAP CLIENT_CONNECT_DOWNLOADING_MAP
#define UDP_CLIENT_CONNECTED       CLIENT_CONNECT_CONNECTED
#define UDP_CLIENT_ERROR           CLIENT_CONNECT_ERROR
#define UDP_CLIENT_SERVER_SHUTDOWN CLIENT_CONNECT_SERVER_SHUTDOWN
#define UDP_CLIENT_KICKED          CLIENT_CONNECT_KICKED
#define UDP_CLIENT_SPECTATING      CLIENT_CONNECT_SPECTATING

#endif /* CLIENT_CONNECT_STATE_H */
