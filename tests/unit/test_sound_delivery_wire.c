/*
 * Sound delivery over the wire (test_sound_delivery_wire.c).
 *
 * Both delivery paths now cull sounds the same way. soundTierAndDirection
 * measures the sound against the recipient's own tank square and reports
 * whether it is inside SDIST_NONE; the UDP drain and the in-process snapshot
 * builder both call it, so a wire client hears exactly as far as a host does.
 * The viewport rects have no say in it any more.
 *
 * What the drain puts on the wire is [soundId, tier, bearing, sourcePlayer] —
 * a near/far band and a coarse compass bearing in place of the map square the
 * sound was raised at.
 *
 * The arms:
 *   - a sound 10 squares out is delivered;
 *   - one at 30 is delivered by both paths, though it sits outside the
 *     recipient's only rect — the range parity;
 *   - one at 45, past SDIST_NONE, reaches neither path;
 *   - a tank hit on the recipient arrives from 60 squares out, since that one
 *     deliberately skips the range cull;
 *   - a delivered sound carries no map square, whether its square is inside the
 *     recipient's rect or outside it;
 *   - manLayingMineNear 30 squares away is dropped, because it has no far
 *     variant for the client to play.
 *
 * One wire client and no other players, with the pill and base view policies
 * turned off so that whatever the loaded map owns cannot grant a rect. The
 * recipient's rect count is asserted to be one — its own tank screen — before
 * any arm runs. Sound no longer consults the rects, but the payload arm still
 * has to know which side of the rect each of its squares sits on.
 *
 * run_sound_tier_playback runs on the same fixture and carries on past the
 * wire: it asserts which sound the client's frontend was handed. A sound 10
 * squares out plays the near variant, one 30 squares out plays the far
 * variant, and manLayingMineNear 10 squares out plays itself.
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
#include "input_packet.h"          /* SOUND_TIER_* / SOUND_DIR_* */
#include "transport_udp.h"         /* the server drain + the download-complete test hook */
#include "threads.h"
#include "test_harness.h"
#include "loopback_harness.h"

/* Convergence ceilings on a clean path. Bounds to report a failure against,
 * not expected counts. */
#define SND_CONNECT_MAX 4000
#define SND_READY_MAX   4000
#define SND_DELIVER_MAX 400

/* How long a sound the drain dropped is given to turn up anyway. Sounds ride
 * the best-effort channel, which is carried on the next snapshot, so a
 * delivered one arrives in a handful of pumps. */
#define SND_QUIET_PUMPS 400

/* Gaps from the recipient's tank, in map squares. 10 and 30 are both inside
 * SDIST_NONE (40), so both are audible; 45 is past it and silent. 30 also sits
 * outside the viewport rect's 19-square half-extent
 * (SNAPSHOT_SCREEN_SIZE / 2 + SNAPSHOT_VIEWPORT_MARGIN), which is what makes it
 * the range arm: it is audible while sitting outside every rect the recipient
 * has. 60 is past everything, and only a tank hit on the recipient itself
 * reaches from there. */
#define SND_GAP_INSIDE  10
#define SND_GAP_OUTSIDE 30
#define SND_GAP_SILENT  45
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

/* The event a sound of this type and id reached the client as, or NULL. */
static const GameEvent *sndFind(LoopbackHarness *h, uint8_t type,
                                uint8_t soundId) {
    const GameEvent *ev = clientSimGetBrainEvents(h->cs);
    int n = clientSimGetBrainEventCount(h->cs);
    int i;

    if (ev == NULL) return NULL;
    for (i = 0; i < n; i++) {
        if (ev[i].type == type && ev[i].data[0] == soundId) return &ev[i];
    }
    return NULL;
}

/* Whether a sound of this type and id has reached the client. */
static bool sndArrived(LoopbackHarness *h, uint8_t type, uint8_t soundId) {
    return sndFind(h, type, soundId) != NULL;
}

typedef struct {
    uint8_t type;
    uint8_t soundId;
} SndWait;

static bool sndDelivered(LoopbackHarness *h, void *user) {
    const SndWait *w = (const SndWait *)user;
    return sndArrived(h, w->type, w->soundId);
}

/* The recipient's tank square as the server holds it — the square the range
 * cull measures from. */
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

/* Where a sound turned up in what the client's frontend was handed, or -1. */
static int sndPlayedIndex(int soundId) {
    int n = ut_sound_count();
    int i;
    for (i = 0; i < n; i++) {
        if (ut_sound_get(i) == soundId) return i;
    }
    return -1;
}

/* The two variants of one staged sound. farId is -1 for a near-only sound. */
typedef struct {
    int nearId;
    int farId;
} SndVariants;

/* Either variant has been played, whichever the client picked. */
static bool sndPlayedEither(LoopbackHarness *h, void *user) {
    const SndVariants *v = (const SndVariants *)user;
    (void)h;
    if (sndPlayedIndex(v->nearId) >= 0) return true;
    return v->farId >= 0 && sndPlayedIndex(v->farId) >= 0;
}

int run_sound_delivery_wire_cull(void) {
    LoopbackHarness h;
    ViewportRect vps[MAX_VIEWPORTS];
    GameEvent builderEv[MAX_SNAPSHOT_EVENTS];
    const GameEvent *got;
    SndWait wait;
    BYTE slot, other;
    BYTE listenMX = 0, listenMY = 0;
    BYTE soundMX;
    int builderCount = 0;
    int dir, n, at, k;

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
     * else. The range arms no longer turn on that, but the payload arm reads
     * the rect to place a square inside it and another outside it, and a
     * fixture that is not what it claims would make that arm meaningless. */
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
                "tank screen)", n);
    }

    /* Put every sound on the recipient's row, on whichever side of it has room
     * for the longest gap. One side always has room: the map is 256 squares
     * across and the gap is under half of that. */
    dir = ((int)listenMX + SND_GAP_FAR <= 255) ? 1 : -1;

    /* ---- Outside the rect, inside the sound model ------------------------ */
    /* The range parity: both paths deliver this one, and the rect stops well
     * short of it, so nothing but the distance cull can be carrying it. */
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

    wait.type = EVENT_SOUND;
    wait.soundId = (uint8_t)bigExplosionNear;
    at = loopbackHarnessPumpUntil(&h, SND_DELIVER_MAX, sndDelivered, &wait);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("a sound at %u,%u, %d squares from the tank at %u,%u and "
                "inside SDIST_NONE %d, never reached the wire client within %d "
                "pumps — the in-process builder delivers it, so the two paths "
                "still disagree about range (%d brain event(s) buffered)",
                (unsigned)soundMX, (unsigned)listenMY, SND_GAP_OUTSIDE,
                (unsigned)listenMX, (unsigned)listenMY, SDIST_NONE,
                SND_DELIVER_MAX, clientSimGetBrainEventCount(h.cs));
    }

    /* ---- Past SDIST_NONE ------------------------------------------------- */
    /* Neither path carries this one: the helper reports out of range and both
     * callers drop it. */
    if (!sndListenerSquare(&h, slot, &listenMX, &listenMY)) {
        loopbackHarnessStop(&h);
        UT_FAIL("lost the server-side tank position before the silent arm");
    }
    soundMX = (BYTE)((int)listenMX + dir * SND_GAP_SILENT);
    clientSimSetBrainEventCount(h.cs, 0);
    sndStage(&h, slot, EVENT_SOUND, (uint8_t)mineExplosionNear, soundMX,
             listenMY, other, builderEv, &builderCount);

    if (sndInBuild(builderEv, builderCount, EVENT_SOUND,
                   (uint8_t)mineExplosionNear)) {
        loopbackHarnessStop(&h);
        UT_FAIL("the in-process builder delivered a sound at %u,%u, %d squares "
                "from the tank at %u,%u and past SDIST_NONE %d",
                (unsigned)soundMX, (unsigned)listenMY, SND_GAP_SILENT,
                (unsigned)listenMX, (unsigned)listenMY, SDIST_NONE);
    }

    loopbackHarnessPumpUntil(&h, SND_QUIET_PUMPS, NULL, NULL);
    if (sndArrived(&h, EVENT_SOUND, (uint8_t)mineExplosionNear)) {
        loopbackHarnessStop(&h);
        UT_FAIL("a sound at %u,%u, %d squares from the tank at %u,%u and past "
                "SDIST_NONE %d, reached the wire client over %d pumps",
                (unsigned)soundMX, (unsigned)listenMY, SND_GAP_SILENT,
                (unsigned)listenMX, (unsigned)listenMY, SDIST_NONE,
                SND_QUIET_PUMPS);
    }

    /* ---- Inside the rect ------------------------------------------------- */
    /* The tank may have drifted over the windows above, so take its square and
     * its rects again rather than reusing the ones an earlier arm ran
     * against. */
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
     * arrives from 60 squares out — past SDIST_NONE as well as past the rect.
     * First, the same hit on the other player from the same square: neither
     * path carries that one, which is what makes the exemption the player's
     * own and not a hole in the range cull. */
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
             listenMY, other, builderEv, &builderCount);

    if (sndInBuild(builderEv, builderCount, EVENT_SOUND_TANK_HIT,
                   (uint8_t)hitTankNear)) {
        loopbackHarnessStop(&h);
        UT_FAIL("the in-process builder delivered a hit on another player at "
                "%u,%u, %d squares from the tank at %u,%u and past SDIST_NONE "
                "%d — only a hit on the recipient itself skips the range cull",
                (unsigned)soundMX, (unsigned)listenMY, SND_GAP_FAR,
                (unsigned)listenMX, (unsigned)listenMY, SDIST_NONE);
    }

    loopbackHarnessPumpUntil(&h, SND_QUIET_PUMPS, NULL, NULL);
    if (sndArrived(&h, EVENT_SOUND_TANK_HIT, (uint8_t)hitTankNear)) {
        loopbackHarnessStop(&h);
        UT_FAIL("a hit on another player at %u,%u, %d squares from the tank at "
                "%u,%u and past SDIST_NONE %d, reached the wire client over %d "
                "pumps — only a hit on the recipient itself skips the range "
                "cull", (unsigned)soundMX, (unsigned)listenMY, SND_GAP_FAR,
                (unsigned)listenMX, (unsigned)listenMY, SDIST_NONE,
                SND_QUIET_PUMPS);
    }

    if (!sndListenerSquare(&h, slot, &listenMX, &listenMY)) {
        loopbackHarnessStop(&h);
        UT_FAIL("lost the server-side tank position before the self-hit arm");
    }
    soundMX = (BYTE)((int)listenMX + dir * SND_GAP_FAR);
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

    /* ---- What a delivered sound carries ---------------------------------- */
    /* The wire payload is [soundId, tier, bearing, sourcePlayer]: the map
     * square the sound was raised at does not reach the client. Run on both
     * sides of the rect, since the rect no longer decides anything about
     * sound. */
    {
        static const struct {
            int gap;
            uint8_t soundId;
            bool inRect;
        } payloadArms[2] = {
            { SND_GAP_INSIDE,  (uint8_t)manBuildingNear, true  },
            { SND_GAP_OUTSIDE, (uint8_t)shotTreeNear,    false },
        };

        for (k = 0; k < 2; k++) {
            if (!sndListenerSquare(&h, slot, &listenMX, &listenMY)) {
                loopbackHarnessStop(&h);
                UT_FAIL("lost the server-side tank position before the payload "
                        "arm at %d squares", payloadArms[k].gap);
            }
            n = serverSimBuildViewports(h.sim, slot, vps, MAX_VIEWPORTS);
            soundMX = (BYTE)((int)listenMX + dir * payloadArms[k].gap);

            /* A staged square below the largest bearing could pass for a tier
             * or a bearing byte, and the assertions below would prove nothing
             * about what was sent. */
            if (soundMX <= SOUND_DIR_NW) {
                loopbackHarnessStop(&h);
                UT_FAIL("the staged square %u is not above the largest bearing "
                        "value %d — the payload arm proves nothing",
                        (unsigned)soundMX, SOUND_DIR_NW);
            }
            if (inAnyViewport(vps, n, soundMX, listenMY) !=
                payloadArms[k].inRect) {
                loopbackHarnessStop(&h);
                UT_FAIL("the square %u,%u, %d from the tank at %u,%u, is %s the "
                        "recipient's %d rect(s); the arm wants it %s",
                        (unsigned)soundMX, (unsigned)listenMY,
                        payloadArms[k].gap, (unsigned)listenMX,
                        (unsigned)listenMY,
                        payloadArms[k].inRect ? "outside" : "inside", n,
                        payloadArms[k].inRect ? "inside" : "outside");
            }

            clientSimSetBrainEventCount(h.cs, 0);
            sndStage(&h, slot, EVENT_SOUND, payloadArms[k].soundId, soundMX,
                     listenMY, other, NULL, NULL);

            wait.type = EVENT_SOUND;
            wait.soundId = payloadArms[k].soundId;
            at = loopbackHarnessPumpUntil(&h, SND_DELIVER_MAX, sndDelivered,
                                          &wait);
            if (at < 0) {
                loopbackHarnessStop(&h);
                UT_FAIL("a sound at %u,%u, %d squares from the tank at %u,%u, "
                        "never reached the wire client within %d pumps (%d "
                        "brain event(s) buffered)", (unsigned)soundMX,
                        (unsigned)listenMY, payloadArms[k].gap,
                        (unsigned)listenMX, (unsigned)listenMY,
                        SND_DELIVER_MAX, clientSimGetBrainEventCount(h.cs));
            }

            got = sndFind(&h, EVENT_SOUND, payloadArms[k].soundId);
            if (got == NULL) {
                loopbackHarnessStop(&h);
                UT_FAIL("the sound at %u,%u went missing between the wait and "
                        "the read", (unsigned)soundMX, (unsigned)listenMY);
            }
            if (got->data[1] > SOUND_TIER_FAR) {
                loopbackHarnessStop(&h);
                UT_FAIL("the sound staged at %u,%u arrived with data[1] = %u, "
                        "which is not a tier (near %d, far %d)",
                        (unsigned)soundMX, (unsigned)listenMY,
                        (unsigned)got->data[1], SOUND_TIER_NEAR,
                        SOUND_TIER_FAR);
            }
            /* The staged square is above the largest bearing, checked before
             * the stage, so the two range checks above already rule out the
             * square reaching the client in either byte. */
            if (got->data[2] > SOUND_DIR_NW) {
                loopbackHarnessStop(&h);
                UT_FAIL("the sound staged at %u,%u arrived with data[2] = %u, "
                        "which is not a bearing (centre %d through NW %d)",
                        (unsigned)soundMX, (unsigned)listenMY,
                        (unsigned)got->data[2], SOUND_DIR_CENTRE,
                        SOUND_DIR_NW);
            }
        }
    }

    /* ---- A near-only sound past the near square -------------------------- */
    /* manLayingMineNear has no far variant, so a far one would be silence at
     * the recipient. Both paths drop it rather than send something the client
     * cannot play. */
    if (!sndListenerSquare(&h, slot, &listenMX, &listenMY)) {
        loopbackHarnessStop(&h);
        UT_FAIL("lost the server-side tank position before the near-only arm");
    }
    soundMX = (BYTE)((int)listenMX + dir * SND_GAP_OUTSIDE);
    clientSimSetBrainEventCount(h.cs, 0);
    sndStage(&h, slot, EVENT_SOUND, (uint8_t)manLayingMineNear, soundMX,
             listenMY, other, builderEv, &builderCount);

    if (sndInBuild(builderEv, builderCount, EVENT_SOUND,
                   (uint8_t)manLayingMineNear)) {
        loopbackHarnessStop(&h);
        UT_FAIL("the in-process builder delivered manLayingMineNear at %u,%u, "
                "%d squares from the tank at %u,%u and so past the near square "
                "at %d", (unsigned)soundMX, (unsigned)listenMY,
                SND_GAP_OUTSIDE, (unsigned)listenMX, (unsigned)listenMY,
                SDIST_SOFT);
    }

    loopbackHarnessPumpUntil(&h, SND_QUIET_PUMPS, NULL, NULL);
    if (sndArrived(&h, EVENT_SOUND, (uint8_t)manLayingMineNear)) {
        loopbackHarnessStop(&h);
        UT_FAIL("manLayingMineNear at %u,%u, %d squares from the tank at %u,%u "
                "and so past the near square at %d, reached the wire client "
                "over %d pumps", (unsigned)soundMX, (unsigned)listenMY,
                SND_GAP_OUTSIDE, (unsigned)listenMX, (unsigned)listenMY,
                SDIST_SOFT, SND_QUIET_PUMPS);
    }

    if (clientSimGetConnectState(h.cs) != CLIENT_CONNECT_CONNECTED) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client left CONNECTED (state %d) during the exchange",
                (int)clientSimGetConnectState(h.cs));
    }

    loopbackHarnessStop(&h);
    return 0;
}

int run_sound_tier_playback(void) {
    LoopbackHarness h;
    SndVariants want;
    BYTE slot, other;
    BYTE listenMX = 0, listenMY = 0;
    BYTE soundMX;
    int dir, at, k, expected, unexpected;

    /* Each arm stages one sound at a gap and names the variant the client
     * should end up playing. The client is handed a tier, not the square, so
     * the variant it plays is the server's reading of the distance rather than
     * its own. */
    static const struct {
        uint8_t staged;
        int     gap;
        int     nearId;
        int     farId;   /* -1 for a sound with no far variant */
        bool    wantFar;
    } playbackArms[3] = {
        { (uint8_t)bigExplosionNear,  SND_GAP_INSIDE,  (int)bigExplosionNear,
          (int)bigExplosionFar,  false },
        { (uint8_t)bigExplosionNear,  SND_GAP_OUTSIDE, (int)bigExplosionNear,
          (int)bigExplosionFar,  true  },
        { (uint8_t)manLayingMineNear, SND_GAP_INSIDE,  (int)manLayingMineNear,
          -1,                    false },
    };

    if (loopbackHarnessStart(&h, "Tier", /*lobbyMode*/ false,
                             /*impairSpec*/ NULL, SND_SEED) != true) {
        loopbackHarnessStop(&h);
        UT_FAIL("harness start (sound tier playback) failed");
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

    /* Same fixture the cull case runs on: with the pill and base categories
     * off, nothing the loaded map owns can hand the recipient an extra rect. */
    threadsWaitForMutex();
    serverSimSetViewPolicy(h.sim, viewCategoryPill, viewPolicyOff,
                           VIEW_DECAY_DEFAULT_SECS);
    serverSimSetViewPolicy(h.sim, viewCategoryBase, viewPolicyOff,
                           VIEW_DECAY_DEFAULT_SECS);
    threadsReleaseMutex();

    /* Put every sound on the recipient's row, on whichever side of it has room
     * for the longest gap. One side always has room: the map is 256 squares
     * across and the gap is under half of that. */
    dir = ((int)listenMX + SND_GAP_OUTSIDE <= 255) ? 1 : -1;

    for (k = 0; k < 3; k++) {
        if (!sndListenerSquare(&h, slot, &listenMX, &listenMY)) {
            loopbackHarnessStop(&h);
            UT_FAIL("lost the server-side tank position before the arm at %d "
                    "squares", playbackArms[k].gap);
        }
        soundMX = (BYTE)((int)listenMX + dir * playbackArms[k].gap);

        ut_sound_reset();
        clientSimSetBrainEventCount(h.cs, 0);
        sndStage(&h, slot, EVENT_SOUND, playbackArms[k].staged, soundMX,
                 listenMY, other, NULL, NULL);

        want.nearId = playbackArms[k].nearId;
        want.farId = playbackArms[k].farId;
        at = loopbackHarnessPumpUntil(&h, SND_DELIVER_MAX, sndPlayedEither,
                                      &want);
        if (at < 0) {
            loopbackHarnessStop(&h);
            UT_FAIL("sound %u staged at %u,%u, %d squares from the tank at "
                    "%u,%u, was never played within %d pumps (%d sound(s) "
                    "played, %d brain event(s) buffered)",
                    (unsigned)playbackArms[k].staged, (unsigned)soundMX,
                    (unsigned)listenMY, playbackArms[k].gap,
                    (unsigned)listenMX, (unsigned)listenMY, SND_DELIVER_MAX,
                    ut_sound_count(), clientSimGetBrainEventCount(h.cs));
        }

        expected = playbackArms[k].wantFar ? playbackArms[k].farId
                                           : playbackArms[k].nearId;
        unexpected = playbackArms[k].wantFar ? playbackArms[k].nearId
                                             : playbackArms[k].farId;
        if (sndPlayedIndex(expected) < 0) {
            loopbackHarnessStop(&h);
            UT_FAIL("sound %u staged %d squares from the tank at %u,%u did "
                    "not play variant %d; %d sound(s) were played, the first "
                    "being %d", (unsigned)playbackArms[k].staged,
                    playbackArms[k].gap, (unsigned)listenMX,
                    (unsigned)listenMY, expected, ut_sound_count(),
                    ut_sound_get(0));
        }
        if (unexpected >= 0 && sndPlayedIndex(unexpected) >= 0) {
            loopbackHarnessStop(&h);
            UT_FAIL("sound %u staged %d squares from the tank at %u,%u played "
                    "variant %d as well as %d", (unsigned)playbackArms[k].staged,
                    playbackArms[k].gap, (unsigned)listenMX,
                    (unsigned)listenMY, unexpected, expected);
        }
    }

    if (clientSimGetConnectState(h.cs) != CLIENT_CONNECT_CONNECTED) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client left CONNECTED (state %d) during the exchange",
                (int)clientSimGetConnectState(h.cs));
    }

    loopbackHarnessStop(&h);
    return 0;
}
