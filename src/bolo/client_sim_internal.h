/*********************************************************
 * client_sim_internal.h
 *
 * Marker include for ClientSim implementation files.
 * Files that include this header are allowed to access
 * ClientSim struct fields directly. For now it simply
 * chains to the public header; after U4.b.7 the full
 * struct definition moves here and client_sim.h becomes
 * a forward declaration only.
 *
 * Allowed includers: src/bolo/client_sim.c,
 * src/bolo/client_sim_control.c, src/bolo/client_snapshot.c,
 * src/bolo/transport_udp_client.c. All other callers must
 * include client_sim.h and use the public accessor API.
 *********************************************************/
#ifndef CLIENT_SIM_INTERNAL_H
#define CLIENT_SIM_INTERNAL_H

#include "client_sim.h"

#endif /* CLIENT_SIM_INTERNAL_H */
