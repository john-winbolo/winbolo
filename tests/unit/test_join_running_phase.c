/*
 * A mid-game joiner's sync replay delivers CTRL_GAME_PHASE_RUNNING
 * (which flips inLobby to false) and then CTRL_LOBBY_SETTINGS. The
 * server fills the settings event's inLobby from lobbyEnabled, so on a
 * lobby-capable server it carries inLobby=true mid-game. The settings
 * apply path used to adopt that field, overwriting the running flip and
 * dropping the joiner onto the lobby screen.
 *
 * This test pins the fix: the CTRL_LOBBY_SETTINGS handler no longer
 * touches netStat or inLobby — the CTRL_GAME_PHASE_* events own them.
 * A positive control replays PHASE_LOBBY before the same settings event
 * to prove lobby state still tracks the phase; the settings event just
 * no longer overrides it.
 */

#include <stdint.h>
#include <string.h>

#include "global.h"
#include "client_sim.h"
#include "client_sim_internal.h"
#include "client_sim_control.h"
#include "control_event.h"
#include "test_harness.h"

static ClientSim *fresh_client_sim(void) {
    ClientSim *cs = clientSimAlloc();
    if (cs == NULL) return NULL;
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, 0);
    return cs;
}

/* Build a CTRL_LOBBY_SETTINGS event as the server fills it mid-game on
 * a lobby-capable server: inLobby=true, netStat=netRunning. */
static void fill_running_settings(ControlEvent *evt) {
    memset(evt, 0, sizeof(*evt));
    evt->type = CTRL_LOBBY_SETTINGS;
    evt->u.lobbySettings.netStat = netRunning;
    evt->u.lobbySettings.hasLobby = true;
}

int run_join_running_phase_not_lobby(void) {
    /* Replay order for a mid-game joiner: PHASE_RUNNING then SETTINGS. */
    ClientSim *cs = fresh_client_sim();
    UT_ASSERT(cs != NULL);

    ControlEvent phase;
    memset(&phase, 0, sizeof(phase));
    phase.type = CTRL_GAME_PHASE_RUNNING;
    clientSimApplyControl(cs, &phase);

    ControlEvent settings;
    fill_running_settings(&settings);
    clientSimApplyControl(cs, &settings);

    UT_ASSERT_MSG(clientSimIsInLobby(cs) == false,
                  "settings event with inLobby=true must not override the "
                  "PHASE_RUNNING flip — joiner landed in the lobby");
    UT_ASSERT_MSG(clientSimGetNetStatus(cs) == netRunning,
                  "net status should still be running after the settings "
                  "event, got %d", (int)clientSimGetNetStatus(cs));
    clientSimDestroy(cs);

    /* Positive control: PHASE_LOBBY then the same settings event. Lobby
     * state still tracks the phase — the settings event no longer drives
     * it either way, so this proves the assertion above isn't vacuous. */
    cs = fresh_client_sim();
    UT_ASSERT(cs != NULL);

    memset(&phase, 0, sizeof(phase));
    phase.type = CTRL_GAME_PHASE_LOBBY;
    clientSimApplyControl(cs, &phase);

    fill_running_settings(&settings);
    clientSimApplyControl(cs, &settings);

    UT_ASSERT_MSG(clientSimIsInLobby(cs) == true,
                  "lobby state should track PHASE_LOBBY after the settings "
                  "event");
    clientSimDestroy(cs);
    return 0;
}
