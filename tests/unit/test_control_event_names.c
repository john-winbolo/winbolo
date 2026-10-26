/*
 * The shared ControlEventType name table (controlEventTypeName in
 * transport_control_codec.c).
 *
 *   every enumerator has a name, and the name is "CTRL_" and then the rest
 *   of the enumerator, so the spot checks below pin the spelling the
 *   --log-events baselines carry;
 *   no two enumerators share a name;
 *   a value outside the enum answers NULL, and the log form answers
 *   "<unknown>".
 */

#include <stdio.h>
#include <string.h>

#include "global.h"
#include "control_event.h"
#include "test_harness.h"

int run_control_event_names(void) {
    int i, j;

    for (i = 0; i < (int)CTRL_EVENT_TYPE_COUNT; i++) {
        const char *name = controlEventTypeName((ControlEventType)i);
        UT_ASSERT_MSG(name != NULL, "event type %d has no name", i);
        UT_ASSERT_MSG(strncmp(name, "CTRL_", 5) == 0,
                      "event type %d is named \"%s\"", i, name);
        UT_ASSERT(strcmp(controlEventDiagName((ControlEventType)i), name) == 0);
        for (j = 0; j < i; j++) {
            UT_ASSERT_MSG(strcmp(name,
                                 controlEventTypeName((ControlEventType)j)) != 0,
                          "event types %d and %d share \"%s\"", j, i, name);
        }
    }

    UT_ASSERT(strcmp(controlEventTypeName(CTRL_CHAT), "CTRL_CHAT") == 0);
    UT_ASSERT(strcmp(controlEventTypeName(CTRL_LOBBY_SETTINGS),
                     "CTRL_LOBBY_SETTINGS") == 0);
    UT_ASSERT(strcmp(controlEventTypeName(CTRL_GAME_PHASE_RUNNING),
                     "CTRL_GAME_PHASE_RUNNING") == 0);
    UT_ASSERT(strcmp(controlEventTypeName(CTRL_VOICE_EVERYONE),
                     "CTRL_VOICE_EVERYONE") == 0);

    UT_ASSERT(controlEventTypeName(CTRL_EVENT_TYPE_COUNT) == NULL);
    UT_ASSERT(controlEventTypeName((ControlEventType)255) == NULL);
    UT_ASSERT(strcmp(controlEventDiagName((ControlEventType)255),
                     "<unknown>") == 0);
    return 0;
}
