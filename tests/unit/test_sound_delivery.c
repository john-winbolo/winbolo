/*
 * Sound events: wire payload and in-process delivery (test_sound_delivery.c).
 *
 * Three off-line CODE-VERIFIABLE pins:
 *
 *   1. The three sound events — EVENT_SOUND, EVENT_SOUND_TANK_HIT and
 *      EVENT_SOUND_SHOOT — carry a four-byte payload that survives
 *      packGameEvent / unpackGameEvent unchanged, and nothing past those four
 *      bytes crosses.
 *
 *   2. serverSimBuildSnapshot's sound block decides per recipient: sounds are
 *      culled by map-square distance at SDIST_NONE, a player's own shot is
 *      skipped, bubbles reach only the player losing the ammo, a tank hit
 *      reaches the player hit whatever the range, and a near-only sound whose
 *      tier came back far is not sent at all.
 *
 *   3. What the middle two payload bytes hold. A human recipient is sent a tier
 *      and a compass bearing measured against its own tank, never the sound's
 *      map square; a bot recipient is sent the square.
 *
 * The builder keeps the closest event of each sound id, so every arm stages its
 * own events and builds once. Where two events in the same build have to be
 * told apart in the result they are given different sound ids; where the arm is
 * about one of them being dropped before the dedup ever sees it, they share one.
 *
 * The two builder tests drive ut_make_running_sim (slot 0) plus a second human
 * at slot 1, position the slot-0 tank with tankSetWorld so the listener square
 * is known, and reach the sim's event buffer directly (the unittests profile
 * permits T2-internal access).
 */

#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"    /* ServerSim::events / eventCount — the staging point */
#include "game_sim.h"               /* GameSim.tanks */
#include "tank.h"                   /* tankSetWorld */
#include "client_enums.h"           /* sndEffects — bubbles, manLayingMineNear */
#include "view_policy.h"            /* viewCategory* / viewPolicyOff */
#include "sounddist.h"              /* SDIST_SOFT / SDIST_NONE */
#include "input_packet.h"
#include "transport_udp_internal.h" /* packGameEvent / unpackGameEvent */
#include "test_harness.h"

/* The listener square every arm measures from. Well inside the map on both
 * axes so a sound 60 squares along X still lands on a real square. */
#define SD_LISTENER_MX 100
#define SD_LISTENER_MY 100

/* Gaps the arms use, in map squares: inside SDIST_SOFT, inside SDIST_NONE but
 * past SDIST_SOFT, past SDIST_NONE, and far enough past it that only the
 * tank-hit-to-self path can carry a sound. */
#define SD_GAP_NEAR   10
#define SD_GAP_HEARD  30
#define SD_GAP_SILENT 45
#define SD_GAP_FAR    60

/* Stage one sound event the way the sim callbacks build it:
 * data = [soundId, mx, my, sourcePlayer]. */
static void sdAddSound(ServerSim *sim, uint8_t type, uint8_t soundId, BYTE mx,
                       BYTE my, BYTE who) {
    GameEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.data[0] = soundId;
    ev.data[1] = mx;
    ev.data[2] = my;
    ev.data[3] = who;
    serverSimAddEvent(sim, &ev);
}

/* Drop whatever the sim is holding so a build's event list is exactly what the
 * arm staged. A sim that has never ticked holds nothing, but an arm that
 * follows another one does. */
static void sdResetEvents(ServerSim *sim) {
    sim->eventCount = 0;
    sim->mapEventCount = 0;
}

/* One snapshot build for `slot`, returning the emitted events in `evOut` and
 * their count. Everything else the build fills is scratch. */
static int sdBuild(ServerSim *sim, BYTE slot, GameEvent *evOut) {
    SnapshotHeader hdr;
    TankSnapshot tk[MAX_TANKS];
    ShellSnapshot sh[MAX_SNAPSHOT_SHELLS];
    TkExplosionSnapshot te[MAX_SNAPSHOT_TK_EXPLOSIONS];
    BaseSnapshot bo[MAX_SNAPSHOT_BASES];
    PillSnapshot po[MAX_SNAPSHOT_PILLS];

    memset(&hdr, 0, sizeof(hdr));
    serverSimBuildSnapshot(sim, slot, &hdr, tk, MAX_TANKS, sh,
                           MAX_SNAPSHOT_SHELLS, te, MAX_SNAPSHOT_TK_EXPLOSIONS,
                           bo, MAX_SNAPSHOT_BASES, po, MAX_SNAPSHOT_PILLS,
                           evOut, MAX_SNAPSHOT_EVENTS, false);
    return (int)hdr.reliableEventCount;
}

/* The emitted event of this type carrying this sound id, or NULL. */
static const GameEvent *sdFind(const GameEvent *ev, int n, uint8_t type,
                               uint8_t soundId) {
    int i;
    for (i = 0; i < n; i++) {
        if (ev[i].type == type && ev[i].data[0] == soundId) {
            return &ev[i];
        }
    }
    return NULL;
}

/* How many of the emitted events are sounds, so an arm can say "one sound came
 * back" rather than only "the one I asked about came back". */
static int sdCountSounds(const GameEvent *ev, int n) {
    int i, count = 0;
    for (i = 0; i < n; i++) {
        if (ev[i].type == EVENT_SOUND || ev[i].type == EVENT_SOUND_TANK_HIT ||
            ev[i].type == EVENT_SOUND_SHOOT) {
            count++;
        }
    }
    return count;
}

/* 1. Codec round-trip: all four data bytes of each sound event survive, the
 *    packed length is the 1-byte type plus a 4-byte payload, and a fifth data
 *    byte set before packing does not cross. */
int run_sound_event_codec(void) {
    static const struct {
        uint8_t     type;
        const char *name;
        uint8_t     data[4];
    } cases[] = {
        { EVENT_SOUND,          "EVENT_SOUND",
          { (uint8_t)bigExplosionNear, 137,  42,  5 } },
        { EVENT_SOUND_TANK_HIT, "EVENT_SOUND_TANK_HIT",
          { (uint8_t)hitTankNear,       11, 209,  3 } },
        { EVENT_SOUND_SHOOT,    "EVENT_SOUND_SHOOT",
          { (uint8_t)shootNear,        250,  77, 12 } },
    };
    const int numCases = (int)(sizeof(cases) / sizeof(cases[0]));
    int t;

    for (t = 0; t < numCases; t++) {
        GameEvent in, out;
        uint8_t buf[GAME_EVENT_MAX_WIRE_SIZE];
        int packed, consumed;

        UT_ASSERT_MSG(gameEventDataSize(cases[t].type) == 4,
                      "%s must carry 4 data bytes, gameEventDataSize says %d",
                      cases[t].name, gameEventDataSize(cases[t].type));

        memset(&in, 0, sizeof(in));
        in.type = cases[t].type;
        in.data[0] = cases[t].data[0];
        in.data[1] = cases[t].data[1];
        in.data[2] = cases[t].data[2];
        in.data[3] = cases[t].data[3];
        in.data[4] = 0x5A;   /* past the payload — must not cross */

        packed = packGameEvent(buf, &in);
        UT_ASSERT_MSG(packed == 5,
                      "%s packed to %d byte(s), expected 5 (type + 4 data)",
                      cases[t].name, packed);

        memset(&out, 0xFF, sizeof(out));
        consumed = unpackGameEvent(buf, (size_t)packed, &out);
        UT_ASSERT_MSG(consumed == packed,
                      "%s unpacked %d byte(s) from a %d-byte buffer",
                      cases[t].name, consumed, packed);
        UT_ASSERT_MSG(out.type == cases[t].type,
                      "%s came back as type %u, expected %u",
                      cases[t].name, out.type, cases[t].type);
        UT_ASSERT_MSG(out.data[0] == cases[t].data[0],
                      "%s soundId came back %u, sent %u",
                      cases[t].name, out.data[0], cases[t].data[0]);
        UT_ASSERT_MSG(out.data[1] == cases[t].data[1],
                      "%s mx came back %u, sent %u",
                      cases[t].name, out.data[1], cases[t].data[1]);
        UT_ASSERT_MSG(out.data[2] == cases[t].data[2],
                      "%s my came back %u, sent %u",
                      cases[t].name, out.data[2], cases[t].data[2]);
        UT_ASSERT_MSG(out.data[3] == cases[t].data[3],
                      "%s player came back %u, sent %u",
                      cases[t].name, out.data[3], cases[t].data[3]);
        UT_ASSERT_MSG(out.data[4] == 0,
                      "%s carried a 5th data byte over the wire (%u) — the "
                      "payload is 4 bytes wide", cases[t].name, out.data[4]);
    }

    return 0;
}

/* 2. Who the snapshot builder hands a sound to, for a human recipient at slot
 *    0 with a second human at slot 1. */
int run_sound_delivery_builder(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    GameSim *gs;
    GameEvent ev[MAX_SNAPSHOT_EVENTS];
    const GameEvent *hit;
    WORLD lwx = 0, lwy = 0;
    int n;

    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);

    gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(gs->tanks[0] != NULL && gs->tanks[1] != NULL,
                  "slot-0/slot-1 tanks not both valid for positioning");

    /* Park the listener on a known square. The cull measures from wx >> 8, so
     * the square centre puts the listener exactly on SD_LISTENER_MX/MY. Slot 1
     * sits on the same square: it is the other sound source, and where its tank
     * stands changes nothing below. */
    {
        WORLD wx = (WORLD)(((int)SD_LISTENER_MX << M_W_SHIFT_SIZE) + MAP_SQUARE_MIDDLE);
        WORLD wy = (WORLD)(((int)SD_LISTENER_MY << M_W_SHIFT_SIZE) + MAP_SQUARE_MIDDLE);
        tankSetWorld(gs, &gs->tanks[0], wx, wy, 0, false);
        tankSetWorld(gs, &gs->tanks[1], wx, wy, 0, false);
    }
    UT_ASSERT_MSG(serverSimGetTankState(sim, 0, &lwx, &lwy),
                  "no tank state for the slot-0 listener");
    UT_ASSERT_MSG((BYTE)(lwx >> 8) == SD_LISTENER_MX &&
                  (BYTE)(lwy >> 8) == SD_LISTENER_MY,
                  "listener sits at %u,%u, expected %u,%u",
                  (unsigned)(BYTE)(lwx >> 8), (unsigned)(BYTE)(lwy >> 8),
                  (unsigned)SD_LISTENER_MX, (unsigned)SD_LISTENER_MY);

    /* ---- Distance cull: SDIST_NONE map squares on either axis ------------ */
    /* Both sounds sit on the listener's row, so the X gap alone decides.
     * Different sound ids so the dedup cannot be what dropped the far one. */
    {
        const BYTE heardMX  = (BYTE)(SD_LISTENER_MX + SD_GAP_HEARD);
        const BYTE silentMX = (BYTE)(SD_LISTENER_MX + SD_GAP_SILENT);

        sdResetEvents(sim);
        sdAddSound(sim, EVENT_SOUND, (uint8_t)bigExplosionNear, heardMX,
                   SD_LISTENER_MY, 1);
        sdAddSound(sim, EVENT_SOUND, (uint8_t)farmingTreeNear, silentMX,
                   SD_LISTENER_MY, 1);
        n = sdBuild(sim, 0, ev);

        UT_ASSERT_MSG(sdFind(ev, n, EVENT_SOUND,
                             (uint8_t)bigExplosionNear) != NULL,
                      "a sound at %u,%u (%d squares from the listener at %u,%u, "
                      "inside SDIST_NONE %d) was not delivered — %d event(s) "
                      "came back, %d of them sound(s)",
                      (unsigned)heardMX, (unsigned)SD_LISTENER_MY, SD_GAP_HEARD,
                      (unsigned)SD_LISTENER_MX, (unsigned)SD_LISTENER_MY,
                      SDIST_NONE, n, sdCountSounds(ev, n));
        UT_ASSERT_MSG(sdFind(ev, n, EVENT_SOUND,
                             (uint8_t)farmingTreeNear) == NULL,
                      "a sound at %u,%u (%d squares from the listener at %u,%u, "
                      "past SDIST_NONE %d) was delivered anyway — %d event(s) "
                      "came back, %d of them sound(s)",
                      (unsigned)silentMX, (unsigned)SD_LISTENER_MY,
                      SD_GAP_SILENT, (unsigned)SD_LISTENER_MX,
                      (unsigned)SD_LISTENER_MY, SDIST_NONE, n,
                      sdCountSounds(ev, n));
    }

    /* ---- Own shoot skipped ---------------------------------------------- */
    /* Both shots are on the listener's own square, so only who fired can
     * separate them. The listener's own shot is dropped before the dedup, so
     * the shot fired by slot 1 is the one that comes back. */
    {
        sdResetEvents(sim);
        sdAddSound(sim, EVENT_SOUND_SHOOT, (uint8_t)shootNear, SD_LISTENER_MX,
                   SD_LISTENER_MY, 0);
        sdAddSound(sim, EVENT_SOUND_SHOOT, (uint8_t)shootNear, SD_LISTENER_MX,
                   SD_LISTENER_MY, 1);
        n = sdBuild(sim, 0, ev);

        hit = sdFind(ev, n, EVENT_SOUND_SHOOT, (uint8_t)shootNear);
        UT_ASSERT_MSG(hit != NULL,
                      "the shot fired by slot 1 at %u,%u was not delivered to "
                      "slot 0 — %d event(s) came back, %d of them sound(s)",
                      (unsigned)SD_LISTENER_MX, (unsigned)SD_LISTENER_MY, n,
                      sdCountSounds(ev, n));
        UT_ASSERT_MSG(hit->data[3] == 1,
                      "slot 0 was sent the shoot sound of player %u at %u,%u — "
                      "its own shot must be skipped",
                      hit->data[3], (unsigned)SD_LISTENER_MX,
                      (unsigned)SD_LISTENER_MY);
        UT_ASSERT_MSG(sdCountSounds(ev, n) == 1,
                      "expected exactly 1 shoot sound for slot 0, got %d of %d "
                      "event(s)", sdCountSounds(ev, n), n);
    }

    /* ---- Bubbles reach only the player losing the ammo ------------------- */
    /* Both on the listener's square, so distance cannot be what separates
     * them; slot 1's is dropped before the dedup. */
    {
        sdResetEvents(sim);
        sdAddSound(sim, EVENT_SOUND, (uint8_t)bubbles, SD_LISTENER_MX,
                   SD_LISTENER_MY, 1);
        sdAddSound(sim, EVENT_SOUND, (uint8_t)bubbles, SD_LISTENER_MX,
                   SD_LISTENER_MY, 0);
        n = sdBuild(sim, 0, ev);

        hit = sdFind(ev, n, EVENT_SOUND, (uint8_t)bubbles);
        UT_ASSERT_MSG(hit != NULL,
                      "slot 0's own bubbles at %u,%u were not delivered — %d "
                      "event(s) came back, %d of them sound(s)",
                      (unsigned)SD_LISTENER_MX, (unsigned)SD_LISTENER_MY, n,
                      sdCountSounds(ev, n));
        UT_ASSERT_MSG(hit->data[3] == 0,
                      "slot 0 was sent player %u's bubbles at %u,%u — bubbles "
                      "go only to the player losing the ammo",
                      hit->data[3], (unsigned)SD_LISTENER_MX,
                      (unsigned)SD_LISTENER_MY);
        UT_ASSERT_MSG(sdCountSounds(ev, n) == 1,
                      "expected exactly 1 bubbles sound for slot 0, got %d of "
                      "%d event(s)", sdCountSounds(ev, n), n);
    }

    /* ---- A tank hit reaches the player hit at any range ------------------ */
    /* Both hits are past SDIST_NONE. The one on slot 0 skips the distance cull
     * because slot 0 is the player hit; the one on slot 1 does not. Slot 1's
     * sits nearer, at SD_GAP_SILENT, so if the cull were missing it would win
     * the closest-per-type dedup and the sender check below would fail rather
     * than the dedup quietly picking the right one anyway. Then slot 1's hit is
     * built on its own, to show it is dropped and not merely outvoted. */
    {
        const BYTE farMX = (BYTE)(SD_LISTENER_MX + SD_GAP_FAR);
        const BYTE otherMX = (BYTE)(SD_LISTENER_MX + SD_GAP_SILENT);

        sdResetEvents(sim);
        sdAddSound(sim, EVENT_SOUND_TANK_HIT, (uint8_t)hitTankNear, farMX,
                   SD_LISTENER_MY, 0);
        sdAddSound(sim, EVENT_SOUND_TANK_HIT, (uint8_t)hitTankNear, otherMX,
                   SD_LISTENER_MY, 1);
        n = sdBuild(sim, 0, ev);

        hit = sdFind(ev, n, EVENT_SOUND_TANK_HIT, (uint8_t)hitTankNear);
        UT_ASSERT_MSG(hit != NULL,
                      "a hit on slot 0 at %u,%u (%d squares from its tank at "
                      "%u,%u, past SDIST_NONE %d) was not delivered — %d "
                      "event(s) came back, %d of them sound(s)",
                      (unsigned)farMX, (unsigned)SD_LISTENER_MY, SD_GAP_FAR,
                      (unsigned)SD_LISTENER_MX, (unsigned)SD_LISTENER_MY,
                      SDIST_NONE, n, sdCountSounds(ev, n));
        UT_ASSERT_MSG(hit->data[3] == 0,
                      "slot 0 was sent the hit on player %u at %u,%u, %d "
                      "squares away — only the player hit skips the distance "
                      "cull", hit->data[3], (unsigned)farMX,
                      (unsigned)SD_LISTENER_MY, SD_GAP_FAR);
        UT_ASSERT_MSG(sdCountSounds(ev, n) == 1,
                      "expected exactly 1 tank-hit sound for slot 0, got %d of "
                      "%d event(s)", sdCountSounds(ev, n), n);

        sdResetEvents(sim);
        sdAddSound(sim, EVENT_SOUND_TANK_HIT, (uint8_t)hitTankNear, otherMX,
                   SD_LISTENER_MY, 1);
        n = sdBuild(sim, 0, ev);
        UT_ASSERT_MSG(sdCountSounds(ev, n) == 0,
                      "slot 0 was sent the hit on player 1 at %u,%u, %d squares "
                      "away and past SDIST_NONE %d, with no nearer hit to "
                      "outvote it — a hit on another player does not skip the "
                      "distance cull", (unsigned)otherMX,
                      (unsigned)SD_LISTENER_MY, SD_GAP_SILENT, SDIST_NONE);
    }

    /* ---- manLayingMineNear reaches the near tier only -------------------- */
    /* manLayingMineNear and bubbles have no far variant, so a far one would be
     * silence at the recipient. The builder drops those two for a human
     * recipient once the tier comes back far, whatever axis the gap is on: a
     * mine laid SD_GAP_HEARD squares along the listener's row is past
     * SDIST_SOFT and is not sent, one at SD_GAP_NEAR is inside it and is. */
    {
        const BYTE farMineMX  = (BYTE)(SD_LISTENER_MX + SD_GAP_HEARD);
        const BYTE nearMineMX = (BYTE)(SD_LISTENER_MX + SD_GAP_NEAR);

        sdResetEvents(sim);
        sdAddSound(sim, EVENT_SOUND, (uint8_t)manLayingMineNear, farMineMX,
                   SD_LISTENER_MY, 1);
        n = sdBuild(sim, 0, ev);

        UT_ASSERT_MSG(sdFind(ev, n, EVENT_SOUND,
                             (uint8_t)manLayingMineNear) == NULL,
                      "manLayingMineNear at %u,%u (%d squares along the "
                      "listener's row from %u,%u, past SDIST_SOFT %d) was "
                      "delivered — it has no far variant, so a far one is "
                      "silence; %d event(s) came back, %d of them sound(s)",
                      (unsigned)farMineMX, (unsigned)SD_LISTENER_MY,
                      SD_GAP_HEARD, (unsigned)SD_LISTENER_MX,
                      (unsigned)SD_LISTENER_MY, SDIST_SOFT, n,
                      sdCountSounds(ev, n));

        sdResetEvents(sim);
        sdAddSound(sim, EVENT_SOUND, (uint8_t)manLayingMineNear, nearMineMX,
                   SD_LISTENER_MY, 1);
        n = sdBuild(sim, 0, ev);

        hit = sdFind(ev, n, EVENT_SOUND, (uint8_t)manLayingMineNear);
        UT_ASSERT_MSG(hit != NULL,
                      "manLayingMineNear at %u,%u (%d squares along the "
                      "listener's row from %u,%u, inside SDIST_SOFT %d) was not "
                      "delivered — %d event(s) came back, %d of them sound(s)",
                      (unsigned)nearMineMX, (unsigned)SD_LISTENER_MY,
                      SD_GAP_NEAR, (unsigned)SD_LISTENER_MX,
                      (unsigned)SD_LISTENER_MY, SDIST_SOFT, n,
                      sdCountSounds(ev, n));
        UT_ASSERT_MSG(hit->data[1] == SOUND_TIER_NEAR,
                      "manLayingMineNear %d squares away came back tier %u, "
                      "expected SOUND_TIER_NEAR (%d)", SD_GAP_NEAR,
                      hit->data[1], SOUND_TIER_NEAR);
    }

    serverSimDestroy(sim);
    return 0;
}

/* 3. The two payload shapes: what a human recipient is sent in data[1]/data[2]
 *    and what a bot is sent. Same fixture as the delivery arms — a listener at
 *    slot 0, a second player at slot 1, both parked on the listener square. */
int run_sound_payload_shape(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    GameSim *gs;
    GameEvent ev[MAX_SNAPSHOT_EVENTS];
    const GameEvent *hit;
    ViewportRect vps[MAX_VIEWPORTS];
    WORLD lwx = 0, lwy = 0;
    int c, n, nvp;

    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);

    gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(gs->tanks[0] != NULL && gs->tanks[1] != NULL,
                  "slot-0/slot-1 tanks not both valid for positioning");

    {
        WORLD wx = (WORLD)(((int)SD_LISTENER_MX << M_W_SHIFT_SIZE) + MAP_SQUARE_MIDDLE);
        WORLD wy = (WORLD)(((int)SD_LISTENER_MY << M_W_SHIFT_SIZE) + MAP_SQUARE_MIDDLE);
        tankSetWorld(gs, &gs->tanks[0], wx, wy, 0, false);
        tankSetWorld(gs, &gs->tanks[1], wx, wy, 0, false);
    }
    UT_ASSERT_MSG(serverSimGetTankState(sim, 0, &lwx, &lwy),
                  "no tank state for the slot-0 listener");
    UT_ASSERT_MSG((BYTE)(lwx >> 8) == SD_LISTENER_MX &&
                  (BYTE)(lwy >> 8) == SD_LISTENER_MY,
                  "listener sits at %u,%u, expected %u,%u",
                  (unsigned)(BYTE)(lwx >> 8), (unsigned)(BYTE)(lwy >> 8),
                  (unsigned)SD_LISTENER_MX, (unsigned)SD_LISTENER_MY);

    /* ---- Tier boundary at SDIST_SOFT ------------------------------------- */
    /* bigExplosionNear has a far variant, so neither side of the boundary is
     * dropped and both come back to be read. One build per case: the two share
     * a sound id and the dedup would otherwise keep only the nearer. */
    {
        static const struct {
            int         gap;
            uint8_t     tier;
            const char *name;
        } cases[] = {
            { SDIST_SOFT,     SOUND_TIER_NEAR, "SOUND_TIER_NEAR" },
            { SDIST_SOFT + 1, SOUND_TIER_FAR,  "SOUND_TIER_FAR"  },
        };

        for (c = 0; c < (int)(sizeof(cases) / sizeof(cases[0])); c++) {
            const BYTE soundMX = (BYTE)(SD_LISTENER_MX + cases[c].gap);

            sdResetEvents(sim);
            sdAddSound(sim, EVENT_SOUND, (uint8_t)bigExplosionNear, soundMX,
                       SD_LISTENER_MY, 1);
            n = sdBuild(sim, 0, ev);

            hit = sdFind(ev, n, EVENT_SOUND, (uint8_t)bigExplosionNear);
            UT_ASSERT_MSG(hit != NULL,
                          "a sound %d squares along the listener's row was not "
                          "delivered — %d event(s) came back, %d of them "
                          "sound(s)", cases[c].gap, n, sdCountSounds(ev, n));
            UT_ASSERT_MSG(hit->data[1] == cases[c].tier,
                          "a sound %d squares from the listener (SDIST_SOFT is "
                          "%d) came back tier %u, expected %s (%u)",
                          cases[c].gap, SDIST_SOFT, hit->data[1],
                          cases[c].name, cases[c].tier);
        }
    }

    /* ---- Direction, map-absolute with Y increasing southward -------------- */
    /* Axis cases have one component zero and diagonals equal magnitudes, so
     * each sits in the middle of its sector rather than on a boundary. */
    {
        static const struct {
            int         dx, dy;
            uint8_t     dir;
            const char *name;
        } cases[] = {
            {            0, -SD_GAP_NEAR, SOUND_DIR_N,      "SOUND_DIR_N"      },
            {  SD_GAP_NEAR, -SD_GAP_NEAR, SOUND_DIR_NE,     "SOUND_DIR_NE"     },
            {  SD_GAP_NEAR,            0, SOUND_DIR_E,      "SOUND_DIR_E"      },
            {  SD_GAP_NEAR,  SD_GAP_NEAR, SOUND_DIR_SE,     "SOUND_DIR_SE"     },
            {            0,  SD_GAP_NEAR, SOUND_DIR_S,      "SOUND_DIR_S"      },
            { -SD_GAP_NEAR,  SD_GAP_NEAR, SOUND_DIR_SW,     "SOUND_DIR_SW"     },
            { -SD_GAP_NEAR,            0, SOUND_DIR_W,      "SOUND_DIR_W"      },
            { -SD_GAP_NEAR, -SD_GAP_NEAR, SOUND_DIR_NW,     "SOUND_DIR_NW"     },
            {            0,            0, SOUND_DIR_CENTRE, "SOUND_DIR_CENTRE" },
        };

        for (c = 0; c < (int)(sizeof(cases) / sizeof(cases[0])); c++) {
            const BYTE soundMX = (BYTE)(SD_LISTENER_MX + cases[c].dx);
            const BYTE soundMY = (BYTE)(SD_LISTENER_MY + cases[c].dy);

            sdResetEvents(sim);
            sdAddSound(sim, EVENT_SOUND, (uint8_t)bigExplosionNear, soundMX,
                       soundMY, 1);
            n = sdBuild(sim, 0, ev);

            hit = sdFind(ev, n, EVENT_SOUND, (uint8_t)bigExplosionNear);
            UT_ASSERT_MSG(hit != NULL,
                          "a sound at %u,%u, %d,%d from the listener at %u,%u, "
                          "was not delivered — %d event(s) came back, %d of "
                          "them sound(s)", (unsigned)soundMX, (unsigned)soundMY,
                          cases[c].dx, cases[c].dy, (unsigned)SD_LISTENER_MX,
                          (unsigned)SD_LISTENER_MY, n, sdCountSounds(ev, n));
            UT_ASSERT_MSG(hit->data[2] == cases[c].dir,
                          "a sound %d,%d from the listener came back direction "
                          "%u, expected %s (%u) — map Y increases southward",
                          cases[c].dx, cases[c].dy, hit->data[2],
                          cases[c].name, cases[c].dir);
        }
    }

    /* ---- No map square reaches a human, in or out of its rects ------------ */
    /* The rect set decides nothing about sound any more, so both arms have to
     * hold. With every view category off the recipient's only rect is its own
     * tank screen, which makes the in/out claims exact — a pill the map happens
     * to own near the far square would otherwise grant a rect around it. */
    {
        static const struct {
            int         gap;
            const char *where;
        } cases[] = {
            { SD_GAP_NEAR,  "inside"  },
            { SD_GAP_HEARD, "outside" },
        };

        serverSimSetViewPolicy(sim, viewCategoryPill, viewPolicyOff,
                               VIEW_DECAY_DEFAULT_SECS);
        serverSimSetViewPolicy(sim, viewCategoryBase, viewPolicyOff,
                               VIEW_DECAY_DEFAULT_SECS);
        serverSimSetViewPolicy(sim, viewCategoryAlly, viewPolicyOff,
                               VIEW_DECAY_DEFAULT_SECS);

        nvp = serverSimBuildViewports(sim, 0, vps, MAX_VIEWPORTS);
        UT_ASSERT_MSG(nvp == 1,
                      "the listener has %d viewport rect(s), expected 1 (its "
                      "own tank screen) — the in/out arms below would prove "
                      "nothing", nvp);

        for (c = 0; c < (int)(sizeof(cases) / sizeof(cases[0])); c++) {
            const BYTE soundMX = (BYTE)(SD_LISTENER_MX + cases[c].gap);
            const bool wantIn = (c == 0);

            UT_ASSERT_MSG(inAnyViewport(vps, nvp, soundMX, SD_LISTENER_MY) ==
                          wantIn,
                          "the square %u,%u, %d from the listener at %u,%u, is "
                          "not %s its rect — the arm proves nothing",
                          (unsigned)soundMX, (unsigned)SD_LISTENER_MY,
                          cases[c].gap, (unsigned)SD_LISTENER_MX,
                          (unsigned)SD_LISTENER_MY, cases[c].where);

            sdResetEvents(sim);
            sdAddSound(sim, EVENT_SOUND, (uint8_t)bigExplosionNear, soundMX,
                       SD_LISTENER_MY, 1);
            n = sdBuild(sim, 0, ev);

            hit = sdFind(ev, n, EVENT_SOUND, (uint8_t)bigExplosionNear);
            UT_ASSERT_MSG(hit != NULL,
                          "a sound at %u,%u, %s the listener's rect and inside "
                          "SDIST_NONE %d, was not delivered — %d event(s) came "
                          "back, %d of them sound(s)", (unsigned)soundMX,
                          (unsigned)SD_LISTENER_MY, cases[c].where, SDIST_NONE,
                          n, sdCountSounds(ev, n));
            UT_ASSERT_MSG(hit->data[1] != soundMX,
                          "a sound staged at %u,%u, %s the listener's rect, "
                          "came back carrying its own map X in data[1]",
                          (unsigned)soundMX, (unsigned)SD_LISTENER_MY,
                          cases[c].where);
            UT_ASSERT_MSG(hit->data[2] != SD_LISTENER_MY,
                          "a sound staged at %u,%u, %s the listener's rect, "
                          "came back carrying its own map Y in data[2]",
                          (unsigned)soundMX, (unsigned)SD_LISTENER_MY,
                          cases[c].where);
            UT_ASSERT_MSG(hit->data[1] <= SOUND_TIER_FAR,
                          "a sound %s the listener's rect came back with %u in "
                          "data[1]; the tier runs to SOUND_TIER_FAR (%d)",
                          cases[c].where, hit->data[1], SOUND_TIER_FAR);
            UT_ASSERT_MSG(hit->data[2] <= SOUND_DIR_NW,
                          "a sound %s the listener's rect came back with %u in "
                          "data[2]; the direction runs to SOUND_DIR_NW (%d)",
                          cases[c].where, hit->data[2], SOUND_DIR_NW);
        }
    }

    /* ---- A bot recipient keeps the map square ---------------------------- */
    /* serverSimIsBot answers yes to a live bot pool entry or to a roster seat
     * marked as a bot, so slot 1 is turned into a bot by setting the pool
     * flag. serverSimAddBot is not an option here: it wants a real brain file,
     * and the unit tests stub the Lua brain entry points out entirely
     * (test_stubs.c). No tick runs while the flag is set, so the snapshot
     * build below is its only reader. */
    {
        const BYTE soundMX = (BYTE)(SD_LISTENER_MX + SD_GAP_NEAR);

        sim->botMgr.bots[1].active = true;

        sdResetEvents(sim);
        sdAddSound(sim, EVENT_SOUND, (uint8_t)bigExplosionNear, soundMX,
                   SD_LISTENER_MY, 0);
        n = sdBuild(sim, 1, ev);
        hit = sdFind(ev, n, EVENT_SOUND, (uint8_t)bigExplosionNear);

        sim->botMgr.bots[1].active = false;

        UT_ASSERT_MSG(hit != NULL,
                      "a sound at %u,%u, %d squares from the bot at %u,%u, was "
                      "not delivered — %d event(s) came back, %d of them "
                      "sound(s)", (unsigned)soundMX, (unsigned)SD_LISTENER_MY,
                      SD_GAP_NEAR, (unsigned)SD_LISTENER_MX,
                      (unsigned)SD_LISTENER_MY, n, sdCountSounds(ev, n));
        UT_ASSERT_MSG(hit->data[1] == soundMX && hit->data[2] == SD_LISTENER_MY,
                      "a bot was sent %u,%u for a sound staged at %u,%u — a bot "
                      "keeps the square, only a human is sent a tier and a "
                      "direction", hit->data[1], hit->data[2],
                      (unsigned)soundMX, (unsigned)SD_LISTENER_MY);
    }

    /* ---- A slot flagged with serverSimSetSoundSquares keeps it too ------- */
    /* The gym agent and the headless brain harness join as ordinary local
     * players and are never bot-manager bots, yet their observation code reads
     * the square. They mark their slot through the T1 setter; the flag has to
     * win over the human default, and clearing it has to restore that default
     * so a reused slot does not inherit it. A far manLayingMineNear is also
     * staged: it is dropped for a human, who has no far variant to play, but a
     * flagged recipient reads it as a position and gets it. */
    {
        const BYTE soundMX = (BYTE)(SD_LISTENER_MX + SD_GAP_NEAR);
        const BYTE mineMX = (BYTE)(SD_LISTENER_MX + SD_GAP_HEARD);
        const GameEvent *mine;

        serverSimSetSoundSquares(sim, 1, true);

        sdResetEvents(sim);
        sdAddSound(sim, EVENT_SOUND, (uint8_t)bigExplosionNear, soundMX,
                   SD_LISTENER_MY, 0);
        sdAddSound(sim, EVENT_SOUND, (uint8_t)manLayingMineNear, mineMX,
                   SD_LISTENER_MY, 0);
        n = sdBuild(sim, 1, ev);
        hit = sdFind(ev, n, EVENT_SOUND, (uint8_t)bigExplosionNear);
        mine = sdFind(ev, n, EVENT_SOUND, (uint8_t)manLayingMineNear);

        UT_ASSERT_MSG(hit != NULL,
                      "a sound %d squares from a slot flagged with "
                      "serverSimSetSoundSquares was not delivered — %d "
                      "event(s) came back", SD_GAP_NEAR, n);
        UT_ASSERT_MSG(hit->data[1] == soundMX && hit->data[2] == SD_LISTENER_MY,
                      "a slot flagged with serverSimSetSoundSquares was sent "
                      "%u,%u for a sound staged at %u,%u — the flag means it "
                      "keeps the square", hit->data[1], hit->data[2],
                      (unsigned)soundMX, (unsigned)SD_LISTENER_MY);
        UT_ASSERT_MSG(mine != NULL &&
                      mine->data[1] == mineMX && mine->data[2] == SD_LISTENER_MY,
                      "a far manLayingMineNear was %s a flagged slot — it reads "
                      "the square as a position and has no tier to be silent in",
                      mine == NULL ? "dropped for" : "reshaped for");

        serverSimSetSoundSquares(sim, 1, false);

        n = sdBuild(sim, 1, ev);
        hit = sdFind(ev, n, EVENT_SOUND, (uint8_t)bigExplosionNear);
        UT_ASSERT_MSG(hit != NULL && hit->data[1] <= SOUND_TIER_FAR &&
                      hit->data[2] <= SOUND_DIR_NW,
                      "clearing serverSimSetSoundSquares did not restore the "
                      "tier and direction a human is sent");
    }

    serverSimDestroy(sim);
    return 0;
}
