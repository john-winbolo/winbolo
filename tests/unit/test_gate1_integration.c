/*
 * Integration coverage for two render-path invariants that the pure
 * interpolation tests (test_interp_render.c) and the viewTick math test
 * (test_viewtick_rewind.c) cover only at the unit level:
 *
 *   1. viewTick ±1-snapshot — over the real loopback transport, the displayed
 *      frame the client stamps as viewTick sits exactly one snapshot behind the
 *      newest applied snapshot (≤2 serverTicks), not an arbitrary age.  This is
 *      the end-to-end form of the displayed-tick invariant: fractional render
 *      interpolation places the drawn position between two integer ticks, so
 *      viewTick can name only the one it brackets.
 *
 *   2. Listen-server host no-op — on the in-process (local) host transport the
 *      per-frame render-prepare seam touches no snapshot state: it drains
 *      nothing, the host's own tank is never routed through remote-tank
 *      interpolation, and the reconcile counter does not move.  Calling it
 *      repeatedly with an advancing render clock leaves the host's own pose and
 *      recon count exactly as they were.
 *
 * Both run deterministically: scenario 1 on a clean (no-impairment) loopback
 * link where every snapshot is delivered, scenario 2 with no sim ticks between
 * the capture and the render-prepare calls.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "client_sim.h"
#include "client_sim_internal.h"   /* ClientState fields, MY_TANK */
#include "client_net.h"            /* clientSimGetViewTick, clientSimRenderPrepare */
#include "client_connect_state.h"
#include "input_packet.h"
#include "tank.h"                  /* tankGetWorld, tankGetAngle */
#include "test_harness.h"
#include "loopback_harness.h"

/* Clean-path connect + two applied snapshots land well inside this bound; it
 * is generous so a hang reads clearly as a failure rather than a slow pass. */
#define VIEWTICK_MAX 1200

#define HOST_WARMUP_TICKS  8
#define HOST_RENDER_FRAMES 10

static bool pred_connected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

int run_gate1_viewtick_loopback(void) {
    LoopbackHarness h;
    int connectedAt;
    int viewKnownAt = -1;
    int i;
    uint32_t viewTick, newest, prev;
    int32_t gap;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Viewer", /*lobbyMode*/ false,
                                       /*impairSpec*/ NULL, /*seed*/ 7u),
                  "harness start failed");

    connectedAt = loopbackHarnessPumpUntil(&h, VIEWTICK_MAX, pred_connected, NULL);
    if (connectedAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps", VIEWTICK_MAX);
    }

    /* Feed input each pump so the running sim advances and snapshots flow.
     * viewTick stays 0 until two snapshots have been applied; once it is
     * known, prevAppliedServerTick (== viewTick) is the displayed frame. */
    for (i = 1; i <= VIEWTICK_MAX; i++) {
        InputPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.tick = (uint32_t)i;
        pkt.playerNum = clientSimGetMyPlayerNum(h.cs);
        clientSimNetSendInput(h.cs, &pkt);
        loopbackHarnessPump(&h);
        if (clientSimGetViewTick(h.cs) > 0) {
            viewKnownAt = i;
            break;
        }
    }
    if (viewKnownAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("viewTick never became known within %d pumps", VIEWTICK_MAX);
    }

    viewTick = clientSimGetViewTick(h.cs);
    newest   = h.cs->clientState.lastAppliedServerTick;
    prev     = h.cs->clientState.prevAppliedServerTick;

    fprintf(stderr,
            "  gate1 viewTick: known after %d pump(s); viewTick=%u newest=%u\n",
            viewKnownAt, (unsigned)viewTick, (unsigned)newest);

    /* viewTick is exactly the second-newest applied snapshot's serverTick. */
    UT_ASSERT_MSG(viewTick == prev,
                  "viewTick %u != prevAppliedServerTick %u",
                  (unsigned)viewTick, (unsigned)prev);

    /* ±1-snapshot invariant: the displayed frame is one snapshot (2 serverTicks)
     * behind the newest applied snapshot on a clean link. */
    gap = (int32_t)(newest - viewTick);
    UT_ASSERT_MSG(gap >= 2 && gap <= 2,
                  "newest-viewTick gap %d outside one snapshot (2 serverTicks)",
                  gap);

    loopbackHarnessStop(&h);
    return 0;
}

int run_gate1_host_noop(void) {
    ServerSim *sim = ut_make_running_sim("Host");
    ClientSim *cs;
    WORLD x0, y0, x1, y1;
    TURNTYPE a0, a1;
    int recon0 = 0, recon1 = 0;
    uint32_t input_tick = 1;
    uint32_t now;
    int w, i;

    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    /* In-process listen-server host: the local transport binds the ClientSim to
     * the running ServerSim (the path the SP host / gym / headless use). */
    UT_ASSERT(clientSimConnectLocal(cs, sim, "Host", "", 0, 0));

    /* Warm up so the host's own tank is live with applied state. The active
     * local transport runs two server half-steps per clientSimNetTick, so feed
     * two tick numbers per net tick to keep the stream from starving. */
    for (w = 0; w < HOST_WARMUP_TICKS; w++) {
        InputPacket a, b;
        memset(&a, 0, sizeof(a)); a.tick = input_tick;     a.playerNum = 0;
        memset(&b, 0, sizeof(b)); b.tick = input_tick + 1; b.playerNum = 0;
        clientSimNetSendInput(cs, &a);
        clientSimNetSendInput(cs, &b);
        clientSimNetTick(cs);
        input_tick += 2;
    }

    UT_ASSERT_MSG(MY_TANK(cs) != NULL, "host own tank missing after warmup");

    /* Capture the host's own-tank pose and reconcile count. */
    tankGetWorld(&MY_TANK(cs), &x0, &y0);
    a0 = tankGetAngle(&MY_TANK(cs));
    clientSimGetReconcileStats(cs, &recon0, NULL, NULL, NULL);

    /* Render-prepare must be a strict no-op on the host: drainSnapshots is a
     * no-op for the local transport, and the own tank (the local player) is
     * skipped by remote-tank interpolation, so nothing routes back into the
     * tank or the reconcile path. Drive several frames with an advancing render
     * clock and confirm nothing moved. */
    now = 100000;
    for (i = 0; i < HOST_RENDER_FRAMES; i++) {
        now += 16;
        clientSimRenderPrepare(cs, now);
    }

    tankGetWorld(&MY_TANK(cs), &x1, &y1);
    a1 = tankGetAngle(&MY_TANK(cs));
    clientSimGetReconcileStats(cs, &recon1, NULL, NULL, NULL);

    UT_ASSERT_MSG(x1 == x0 && y1 == y0,
                  "host own tank moved on render-prepare: (%u,%u) -> (%u,%u)",
                  (unsigned)x0, (unsigned)y0, (unsigned)x1, (unsigned)y1);
    UT_ASSERT_MSG(a1 == a0, "host own tank angle moved on render-prepare");
    UT_ASSERT_MSG(recon1 == recon0,
                  "host recon count moved on render-prepare: %d -> %d",
                  recon0, recon1);

    clientSimDestroy(cs);
    serverSimDestroy(sim);
    return 0;
}
