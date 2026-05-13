/*
 * wire_limits.h
 *
 * Public size constants for the bolo wire protocol that legitimately
 * surface in GUI code (typically as buffer dimensions for input fields
 * that map onto wire packet payloads). The wire format itself stays
 * internal in netpacks.h / bolo_packets.h; only the payload-cap
 * constants live here.
 *
 * T4 leaf — no #includes, no behaviour.
 */

#ifndef WIRE_LIMITS_H
#define WIRE_LIMITS_H

/* Maximum bytes per chat message payload on the wire. The chat input
 * widget in the desktop GUI sizes its buffer to this so the user sees
 * the cap as they type. Must match the wire layout in
 * transport_udp_client.c / transport_udp_server.c. */
#define PACKET_MAX_CHAT_MESSAGE 128

#endif /* WIRE_LIMITS_H */
