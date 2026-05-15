/*
 * wire_limits.h
 *
 * Public wire-protocol values that legitimately surface in GUI code:
 * payload-size caps (so input widgets can show the right limit) and
 * the version string used in compatibility checks. The wire format
 * itself stays internal in netpacks.h / bolo_packets.h.
 *
 * No #includes, no behaviour.
 */

#ifndef WIRE_LIMITS_H
#define WIRE_LIMITS_H

/* Maximum bytes per chat message payload on the wire. The chat input
 * widget in the desktop GUI sizes its buffer to this so the user sees
 * the cap as they type. Must match the wire layout in
 * transport_udp_client.c / transport_udp_server.c. */
#define PACKET_MAX_CHAT_MESSAGE 128

/* Max size of player name in join request. Surfaced publicly so the
 * client-side lobby slot mirror (ClientLobbySlot in client_sim.h) can
 * size its playerName[] buffer without pulling in the internal
 * netpacks.h wire-protocol header. */
#define PACKET_MAX_PLAYER_NAME 64

/* Version string used in info-packet responses and surfaced in the
 * server browser to gate "join" against version-skewed servers.
 * WINBOLO_VERSION is supplied as a compile definition by CMake. */
#define STRVER WINBOLO_VERSION

#endif /* WIRE_LIMITS_H */
