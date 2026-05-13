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
  CLIENT_CONNECT_SERVER_SHUTDOWN
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

#endif /* CLIENT_CONNECT_STATE_H */
