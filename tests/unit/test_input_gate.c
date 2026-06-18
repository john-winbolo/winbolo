/*
 * Taxonomy predicate for the in-game input gate (input_gate.h).
 * gameInputSuspended() must suspend the polled in-game readers only for
 * blocking surfaces (text input, a focus-stealing modal panel, a popup/menu
 * on the stack, or a defocused window) and must never suspend for the
 * transient notifications — an open alliance request or a vote widget.
 */
#include "input_gate.h"
#include "test_harness.h"

int run_input_gate_taxonomy(void) {
    /* alliance notification visible, nothing else → not suspended
       (the regression that forced every reader to TNONE) */
    {
        InputGateState s = { false, false, false, true, false, true };
        UT_ASSERT_MSG(!gameInputSuspended(&s),
                      "alliance notification must not suspend in-game input");
    }

    /* vote visible, nothing else → not suspended */
    {
        InputGateState s = { false, false, false, false, true, true };
        UT_ASSERT_MSG(!gameInputSuspended(&s),
                      "vote widget must not suspend in-game input");
    }

    /* text input active → suspended */
    {
        InputGateState s = { true, false, false, false, false, true };
        UT_ASSERT_MSG(gameInputSuspended(&s),
                      "active text input must suspend in-game input");
    }

    /* blocking modal open → suspended */
    {
        InputGateState s = { false, true, false, false, false, true };
        UT_ASSERT_MSG(gameInputSuspended(&s),
                      "blocking modal panel must suspend in-game input");
    }

    /* menu/popup on the stack → suspended */
    {
        InputGateState s = { false, false, true, false, false, true };
        UT_ASSERT_MSG(gameInputSuspended(&s),
                      "open menu/popup must suspend in-game input");
    }

    /* app window unfocused → suspended */
    {
        InputGateState s = { false, false, false, false, false, false };
        UT_ASSERT_MSG(gameInputSuspended(&s),
                      "unfocused window must suspend in-game input");
    }

    /* normal in-game baseline: focused, nothing open → not suspended */
    {
        InputGateState s = { false, false, false, false, false, true };
        UT_ASSERT_MSG(!gameInputSuspended(&s),
                      "focused window with no overlays must not suspend input");
    }

    return 0;
}
