/*
 * Sound delivery over the wire (test_sound_delivery_wire.c).
 *
 * The two delivery paths do not cull sounds the same way. The snapshot builder
 * culls by map-square distance against SDIST_NONE
 * (server_sim_snapshot.c:1267); the UDP drain culls by rect membership,
 * !inAnyViewport over the recipient's viewport set
 * (transport_udp_server.c:6747). A wire client therefore stops hearing at the
 * rect's 27-square half-extent while a host hears out to 40.
 *
 * This case drives that difference over real sockets. One wire client and no
 * other players, with the pill and base view policies turned off so that
 * whatever the loaded map owns cannot grant a rect: the drain culls against the
 * whole rect set, so an allied pill or base near a sound would carry it through
 * by accident and prove nothing. The recipient's rect count is asserted to be
 * one — its own tank screen — before any arm runs.
 *
 * Sounds are staged into the sim's event buffer and handed to the real
 * transportUdpServerDrainEvents, the same producer a running frame uses. What
 * arrived is read off the client's brain-event buffer: clientSimApplyGameEvents
 * buffers all three sound types for every recipient whatever the client type
 * (client_snapshot.c:274-303), and nothing on this path drains that buffer, so
 * it is the record of what the wire delivered. Per the harness contract the
 * assertions are "converges within N pumps", never a packet trace.
 */

#include <stdint.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* ServerSim::eventCount, ViewportRect, inAnyViewport */
#include "client_sim.h"
#include "client_net.h"
#include "client_connect_state.h"
#include "client_enums.h"          /* sndEffects */
#include "view_policy.h"           /* viewCategoryPill / viewPolicyOff */
#include "sounddist.h"             /* SDIST_NONE */
#include "input_packet.h"
#include "transport_udp.h"         /* the server drain + the download-complete test hook */
#include "threads.h"
#include "test_harness.h"
#include "loopback_harness.h"

/* Convergence ceilings on a clean path. Bounds to report a failure against,
 * not expected counts. */
#define SND_CONNECT_MAX 4000
#define SND_READY_MAX   4000
#define SND_DELIVER_MAX 400

/* How long a sound the drain culled is given to turn up anyway. Sounds ride
 * the best-effort channel, which is carried on the next snapshot, so a
 * delivered one arrives in a handful of pumps. */
#define SND_QUIET_PUMPS 400

/* Gaps from the recipient's tank, in map squares. The viewport rect reaches 27
 * squares (SNAPSHOT_SCREEN_SIZE / 2 + SNAPSHOT_VIEWPORT_MARGIN), so 10 is
 * inside it and 30 is outside while still inside SDIST_NONE — the range the
 * two paths disagree over. 60 is past both. */
#define SND_GAP_INSIDE  10
#define SND_GAP_OUTSIDE 30
#define SND_GAP_FAR     60

/* Nothing on a clean path draws from the impairment stream; the seed keeps the
 * run reproducible regardless. */
#define SND_SEED 0x50D1CEu

static bool sndConnected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

/* The server flips its own download-complete flag only once the map's bytes are
 * acked back on CHANNEL_BULK. The drain skips a still-downloading slot, so wait
 * for it. */
static bool sndServerReady(LoopbackHarness *h, void *user) {
    (void)user;
    return transportUdpServerTestDownloadComplete(
        (int)clientSimGetMyPlayerNum(h->cs));
}

static bool sndHaveTank(LoopbackHarness *h, void *user) {
    BYTE mx = 0, my = 0;
    (void)user;
    return clientSimGetMyTankMapPos(h->cs, &mx, &my) == TRUE;
}

/* Whether a sound of this type and id has reached the client. */
static bool sndArrived(LoopbackHarness *h, uint8_t type, uint8_t soundId) {
    const GameEvent *ev = clientSimGetBrainEvents(h->cs);
    int n = clientSimGetBrainEventCount(h->cs);
    int i;

    if (ev == NULL) return false;
    for (i = 0; i < n; i++) {
        if (ev[i].type == type && ev[i].data[0] == soundId) return true;
    }
    return false;
}

typedef struct {
    uint8_t type;
    uint8_t soundId;
} SndWait;

static bool sndDelivered(LoopbackHarness *h, void *user) {
    const SndWait *w = (const SndWait *)user;
    return sndArrived(h, w->type, w->soundId);
}

/* The recipient's tank square as the server holds it — the square both culls
 * measure from. */
static bool sndListenerSquare(LoopbackHarness *h, BYTE slot, BYTE *mx,
                              BYTE *my) {
    WORLD wx = 0, wy = 0;
    bool ok;

    threadsWaitForMutex();
    ok = serverSimGetTankState(h->sim, slot, &wx, &wy);
    threadsReleaseMutex();
    if (!ok) return false;
    *mx = (BYTE)(wx >> 8);
    *my = (BYTE)(wy >> 8);
    return true;
}

/* Stage one sound the way the sim callbacks build it and run the real drain
 * over it. When builderEv is non-NULL the in-process snapshot builder runs over
 * the same staged event first, so an arm can compare the two paths on one
 * sound; builderCount then comes back holding how many events it emitted. */
static void sndStage(LoopbackHarness *h, BYTE slot, uint8_t type,
                     uint8_t soundId, BYTE mx, BYTE my, BYTE who,
                     GameEvent *builderEv, int *builderCount) {
    GameEvent ev;
    SnapshotHeader hdr;
    TankSnapshot tk[MAX_TANKS];
    ShellSnapshot sh[MAX_SNAPSHOT_SHELLS];
    TkExplosionSnapshot te[MAX_SNAPSHOT_TK_EXPLOSIONS];
    BaseSnapshot bo[MAX_SNAPSHOT_BASES];
    PillSnapshot po[MAX_SNAPSHOT_PILLS];

    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.data[0] = soundId;
    ev.data[1] = mx;
    ev.data[2] = my;
    ev.data[3] = who;
    memset(&hdr, 0, sizeof(hdr));

    threadsWaitForMutex();
    h->sim->eventCount = 0;
    serverSimAddEvent(h->sim, &ev);
    if (builderEv != NULL) {
        serverSimBuildSnapshot(h->sim, slot, &hdr, tk, MAX_TANKS, sh,
                               MAX_SNAPSHOT_SHELLS, te,
                               MAX_SNAPSHOT_TK_EXPLOSIONS, bo,
                               MAX_SNAPSHOT_BASES, po, MAX_SNAPSHOT_PILLS,
                               builderEv, MAX_SNAPSHOT_EVENTS, false);
    }
    transportUdpServerDrainEvents(h->sim);
    /* The next running tick clears this itself; clearing it here keeps the
     * event from being drained twice if one never runs. */
    h->sim->eventCount = 0;
    threadsReleaseMutex();

    if (builderCount != NULL) {
        *builderCount = (int)hdr.reliableEventCount;
    }
}

/* Whether the builder emitted a sound of this type and id. */
static bool sndInBuild(const GameEvent *ev, int n, uint8_t type,
                       uint8_t soundId) {
    int i;
    for (i = 0; i < n; i++) {
        if (ev[i].type == type && ev[i].data[0] == soundId) return true;
    }
    return false;
}

int run_sound_delivery_wire_cull(void) {
    LoopbackHarness h;
    ViewportRect vps[MAX_VIEWPORTS];
    GameEvent builderEv[MAX_SNAPSHOT_EVENTS];
    SndWait wait;
    BYTE slot, other;
    BYTE listenMX = 0, listenMY = 0;
    BYTE soundMX;
    int builderCount = 0;
    int dir, n, at;

    if (loopbackHarnessStart(&h, "Snd", /*lobbyMode*/ false,
                             /*impairSpec*/ NULL, SND_SEED) != true) {
        loopbackHarnessStop(&h);
        UT_FAIL("harness start (sound delivery wire) failed");
    }
    if (loopbackHarnessPumpUntil(&h, SND_CONNECT_MAX, sndConnected, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client never reached CONNECTED within %d pumps",
                SND_CONNECT_MAX);
    }
    if (loopbackHarnessPumpUntil(&h, SND_READY_MAX, sndServerReady, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the server never saw the map download acked within %d pumps",
                SND_READY_MAX);
    }
    if (loopbackHarnessPumpUntil(&h, SND_READY_MAX, sndHaveTank, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client never got a tank within %d pumps", SND_READY_MAX);
    }

    slot = clientSimGetMyPlayerNum(h.cs);
    if (slot >= MAX_TANKS || !sndListenerSquare(&h, slot, &listenMX, &listenMY)) {
        loopbackHarnessStop(&h);
        UT_FAIL("no slot or server-side tank position after convergence "
                "(slot %u)", (unsigned)slot);
    }
    other = (BYTE)(slot == 0 ? 1 : 0);

    /* With the pill and base categories off, nothing the map happens to hand
     * the recipient can grant a rect, and there is no second player to grant an
     * ally one — so the rect set is the recipient's own tank screen and nothing
     * else. Without that the range arms mean nothing: the drain culls against
     * the whole set, so an allied item near a sound would carry it through. */
    threadsWaitForMutex();
    serverSimSetViewPolicy(h.sim, viewCategoryPill, viewPolicyOff,
                           VIEW_DECAY_DEFAULT_SECS);
    serverSimSetViewPolicy(h.sim, viewCategoryBase, viewPolicyOff,
                           VIEW_DECAY_DEFAULT_SECS);
    threadsReleaseMutex();

    n = serverSimBuildViewports(h.sim, slot, vps, MAX_VIEWPORTS);
    if (n != 1) {
        loopbackHarnessStop(&h);
        UT_FAIL("the recipient has %d viewport rect(s), expected 1 (its own "
                "tank screen) — an allied rect would carry a culled sound "
                "through", n);
    }

    /* Put every sound on the recipient's row, on whichever side of it has room
     * for the longest gap. */
    dir = ((int)listenMX + SND_GAP_FAR <= 255) ? 1 : -1;
    if ((int)listenMX + dir * SND_GAP_FAR < 0 ||
        (int)listenMX + dir * SND_GAP_FAR > 255) {
        loopbackHarnessStop(&h);
        UT_FAIL("the tank at %u,%u has no room for a sound %d squares away on "
                "either side", (unsigned)listenMX, (unsigned)listenMY,
                SND_GAP_FAR);
    }

    /* ---- Outside the rect, inside the sound model ------------------------ */
    /* The two paths disagree here: the builder delivers this sound and the
     * drain does not. */
    soundMX = (BYTE)((int)listenMX + dir * SND_GAP_OUTSIDE);
    if (inAnyViewport(vps, n, soundMX, listenMY)) {
        loopbackHarnessStop(&h);
        UT_FAIL("the square %u,%u, %d from the tank at %u,%u, is inside the "
                "recipient's rect — the arm proves nothing",
                (unsigned)soundMX, (unsigned)listenMY, SND_GAP_OUTSIDE,
                (unsigned)listenMX, (unsigned)listenMY);
    }
    clientSimSetBrainEventCount(h.cs, 0);
    sndStage(&h, slot, EVENT_SOUND, (uint8_t)bigExplosionNear, soundMX,
             listenMY, other, builderEv, &builderCount);

    if (!sndInBuild(builderEv, builderCount, EVENT_SOUND,
                    (uint8_t)bigExplosionNear)) {
        loopbackHarnessStop(&h);
        UT_FAIL("the in-process builder dropped a sound at %u,%u, %d squares "
                "from the tank at %u,%u and inside SDIST_NONE %d — it emitted "
                "%d event(s)", (unsigned)soundMX, (unsigned)listenMY,
                SND_GAP_OUTSIDE, (unsigned)listenMX, (unsigned)listenMY,
                SDIST_NONE, builderCount);
    }

    loopbackHarnessPumpUntil(&h, SND_QUIET_PUMPS, NULL, NULL);
    if (sndArrived(&h, EVENT_SOUND, (uint8_t)bigExplosionNear)) {
        loopbackHarnessStop(&h);
        UT_FAIL("a sound at %u,%u, %d squares from the tank at %u,%u and "
                "outside its only rect, reached the wire client over %d pumps "
                "(the in-process builder delivers it; the drain's rect cull "
                "does not)", (unsigned)soundMX, (unsigned)listenMY,
                SND_GAP_OUTSIDE, (unsigned)listenMX, (unsigned)listenMY,
                SND_QUIET_PUMPS);
    }

    /* ---- Inside the rect ------------------------------------------------- */
    /* The tank may have drifted over the window above, so take its square and
     * its rects again rather than reusing the ones the first arm ran against. */
    if (!sndListenerSquare(&h, slot, &listenMX, &listenMY)) {
        loopbackHarnessStop(&h);
        UT_FAIL("lost the server-side tank position before the near arm");
    }
    n = serverSimBuildViewports(h.sim, slot, vps, MAX_VIEWPORTS);
    soundMX = (BYTE)((int)listenMX + dir * SND_GAP_INSIDE);
    if (!inAnyViewport(vps, n, soundMX, listenMY)) {
        loopbackHarnessStop(&h);
        UT_FAIL("the square %u,%u, %d from the tank at %u,%u, is outside the "
                "recipient's %d rect(s) — the arm proves nothing",
                (unsigned)soundMX, (unsigned)listenMY, SND_GAP_INSIDE,
                (unsigned)listenMX, (unsigned)listenMY, n);
    }
    clientSimSetBrainEventCount(h.cs, 0);
    sndStage(&h, slot, EVENT_SOUND, (uint8_t)farmingTreeNear, soundMX,
             listenMY, other, NULL, NULL);

    wait.type = EVENT_SOUND;
    wait.soundId = (uint8_t)farmingTreeNear;
    at = loopbackHarnessPumpUntil(&h, SND_DELIVER_MAX, sndDelivered, &wait);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("a sound at %u,%u, %d squares from the tank at %u,%u and "
                "inside its rect, never reached the wire client within %d "
                "pumps (%d brain event(s) buffered)", (unsigned)soundMX,
                (unsigned)listenMY, SND_GAP_INSIDE, (unsigned)listenMX,
                (unsigned)listenMY, SND_DELIVER_MAX,
                clientSimGetBrainEventCount(h.cs));
    }

    /* ---- A tank hit on the recipient, past every rect -------------------- */
    /* The drain sends a hit to the player hit whatever the range, so this one
     * arrives from 60 squares out with no rect covering it. */
    if (!sndListenerSquare(&h, slot, &listenMX, &listenMY)) {
        loopbackHarnessStop(&h);
        UT_FAIL("lost the server-side tank position before the tank-hit arm");
    }
    soundMX = (BYTE)((int)listenMX + dir * SND_GAP_FAR);
    n = serverSimBuildViewports(h.sim, slot, vps, MAX_VIEWPORTS);
    if (inAnyViewport(vps, n, soundMX, listenMY)) {
        loopbackHarnessStop(&h);
        UT_FAIL("the square %u,%u, %d from the tank at %u,%u, is inside the "
                "recipient's %d rect(s) — the arm proves nothing",
                (unsigned)soundMX, (unsigned)listenMY, SND_GAP_FAR,
                (unsigned)listenMX, (unsigned)listenMY, n);
    }
    clientSimSetBrainEventCount(h.cs, 0);
    sndStage(&h, slot, EVENT_SOUND_TANK_HIT, (uint8_t)hitTankNear, soundMX,
             listenMY, slot, NULL, NULL);

    wait.type = EVENT_SOUND_TANK_HIT;
    wait.soundId = (uint8_t)hitTankNear;
    at = loopbackHarnessPumpUntil(&h, SND_DELIVER_MAX, sndDelivered, &wait);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("a hit on the recipient at %u,%u, %d squares from its tank at "
                "%u,%u and outside every rect, never reached it within %d "
                "pumps (%d brain event(s) buffered)", (unsigned)soundMX,
                (unsigned)listenMY, SND_GAP_FAR, (unsigned)listenMX,
                (unsigned)listenMY, SND_DELIVER_MAX,
                clientSimGetBrainEventCount(h.cs));
    }

    if (clientSimGetConnectState(h.cs) != CLIENT_CONNECT_CONNECTED) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client left CONNECTED (state %d) during the exchange",
                (int)clientSimGetConnectState(h.cs));
    }

    loopbackHarnessStop(&h);
    return 0;
}
