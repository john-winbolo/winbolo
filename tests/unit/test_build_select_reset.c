/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * The build selection is per-game state. A ClientSim survives the lobby
 * cycle, so CTRL_GAME_PHASE_RUNNING has to put it back on trees, or a
 * player who picked buildings last round starts the next one on them.
 */

#include <string.h>

#include "global.h"
#include "client_sim.h"
#include "client_sim_control.h"
#include "control_event.h"
#include "test_harness.h"

int run_build_select_reset_on_running_phase(void) {
    ClientSim *cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);

    clientSimSetCurrentBuildSelect(cs, BsBuilding);
    UT_ASSERT_MSG(clientSimGetCurrentBuildSelect(cs) == BsBuilding,
                  "setup failed — the selection did not change to buildings");

    ControlEvent phase;
    memset(&phase, 0, sizeof(phase));
    phase.type = CTRL_GAME_PHASE_RUNNING;
    clientSimApplyControl(cs, &phase);

    UT_ASSERT_MSG(clientSimGetCurrentBuildSelect(cs) == BsTrees,
                  "the running phase must put the selection back on trees, got %d",
                  (int)clientSimGetCurrentBuildSelect(cs));

    clientSimDestroy(cs);
    return 0;
}
