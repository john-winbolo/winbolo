/*
 * Compiled under the gui include profile, which must not resolve this
 * header: the scenario write funnel and the policy vtable are
 * server-authoritative entry points, and a frontend that wants to
 * change the world sends a command instead.
 *
 * The target that builds this file is EXCLUDE_FROM_ALL and is expected
 * to fail. The CTest entry that builds it is marked WILL_FAIL, so this
 * file compiling successfully is the failure.
 */

#include "server_sim_scenario.h"

int scenario_api_under_gui(void);

int scenario_api_under_gui(void) {
    return (int)SCN_OP_UNSUPPORTED;
}
