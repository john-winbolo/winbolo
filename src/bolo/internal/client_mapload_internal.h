#ifndef BOLO_INTERNAL_CLIENT_MAPLOAD_INTERNAL_H
#define BOLO_INTERNAL_CLIENT_MAPLOAD_INTERNAL_H

#include "client_sim.h"
#include "types.h"

/* Decompress `buf`/`len` into the ClientSim's map/pills/bases/starts,
 * stash the map name, and prime the viewport + mine-visibility state.
 * Does NOT call clientSimCreate; the ClientSim must already be alive
 * and initialised. The caller is responsible for clientSimSetupSelf
 * and for the snapshot apply that follows. */
bool installCompressedMap(ClientSim *cs, const BYTE *buf, int len, const char *name);

#endif
