/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
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
#include "messages.h"   /* messageAddItem, messageUpdate */
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

/* A line is blank when it holds nothing but spaces. An empty string does not
 * count: it means frontEndMessages was never called. */
static int nbIsBlankLine(const char *line) {
    if (line == NULL || line[0] == '\0') return 0;
    for (const char *p = line; *p != '\0'; p++) {
        if (*p != ' ') return 0;
    }
    return 1;
}

/*
 * The newswire draws from its own copy of the two lines, which only hears
 * about a change through frontEndMessages. CTRL_GAME_PHASE_RUNNING blanks
 * the sim's lines, so it has to hand the blanked lines over too, or the last
 * game's text stays on the newswire until the first message of this one.
 */
int run_newswire_blanked_on_running_phase(void) {
    ClientSim *cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);

    MessageState *ms = clientSimGetMessages(cs);
    UT_ASSERT(ms != NULL);
    messageAddItem(ms, (char *)"TOPTEXT", (char *)"BOTTOMTEXT");
    /* Scroll the whole message in; each update shifts one cell and hands the
     * lines to the front end. */
    int guard = 0;
    while (ms->queueCount > 0 && guard++ < 1000) {
        messageUpdate(cs, ms);
    }
    UT_ASSERT_MSG(ms->queueCount == 0, "setup failed — the message did not scroll in");
    UT_ASSERT_MSG(strstr(ut_messages_last_top(), "TOPTEXT") != NULL,
                  "setup failed — the front end's top line is \"%s\"",
                  ut_messages_last_top());
    UT_ASSERT_MSG(strstr(ut_messages_last_bottom(), "BOTTOMTEXT") != NULL,
                  "setup failed — the front end's bottom line is \"%s\"",
                  ut_messages_last_bottom());

    ControlEvent phase;
    memset(&phase, 0, sizeof(phase));
    phase.type = CTRL_GAME_PHASE_RUNNING;
    clientSimApplyControl(cs, &phase);

    UT_ASSERT_MSG(nbIsBlankLine(ut_messages_last_top()),
                  "the running phase must hand the front end a blank top line, got \"%s\"",
                  ut_messages_last_top());
    UT_ASSERT_MSG(nbIsBlankLine(ut_messages_last_bottom()),
                  "the running phase must hand the front end a blank bottom line, got \"%s\"",
                  ut_messages_last_bottom());

    clientSimDestroy(cs);
    return 0;
}
