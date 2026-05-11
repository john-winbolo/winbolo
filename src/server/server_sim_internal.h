/*********************************************************
 * server_sim_internal.h
 *
 * Marker include for ServerSim implementation files.
 * Files that include this header are allowed to access
 * ServerSim struct fields directly. For now it simply
 * chains to the public header; after U4.a.10 the full
 * struct definition moves here and server_sim.h becomes
 * a forward declaration only.
 *
 * Outside src/server/server_sim.c and
 * src/server/server_lifecycle.c, callers must include
 * server_sim.h and use the public accessor/mutator API.
 *********************************************************/
#ifndef SERVER_SIM_INTERNAL_H
#define SERVER_SIM_INTERNAL_H

#include "server_sim.h"

#endif /* SERVER_SIM_INTERNAL_H */
