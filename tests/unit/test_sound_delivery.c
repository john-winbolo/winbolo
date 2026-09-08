/*
 * Sound events: wire payload and in-process delivery (test_sound_delivery.c).
 *
 * Nothing under tests/ reached EVENT_SOUND before this file. Two off-line
 * CODE-VERIFIABLE pins:
 *
 *   1. The three sound events — EVENT_SOUND, EVENT_SOUND_TANK_HIT and
 *      EVENT_SOUND_SHOOT — carry a four-byte [soundId, mx, my, sourcePlayer]
 *      payload that survives packGameEvent / unpackGameEvent unchanged, and
 *      nothing past those four bytes crosses.
 *
 *   2. serverSimBuildSnapshot's sound block decides per recipient: sounds are
 *      culled by map-square distance at SDIST_NONE, a player's own shot is
 *      skipped, bubbles reach only the player losing the ammo, and a tank hit
 *      reaches the player hit whatever the range.
 *
 * The builder keeps the closest event of each sound id, so every arm stages its
 * own events and builds once. Where two events in the same build have to be
 * told apart in the result they are given different sound ids; where the arm is
 * about one of them being dropped before the dedup ever sees it, they share one.
 *
 * Both tests drive ut_make_running_sim (slot 0) plus a second human at slot 1,
 * position the slot-0 tank with tankSetWorld so the listener square is known,
 * and reach the sim's event buffer directly (the unittests profile permits
 * T2-internal access).
 */

#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"    /* ServerSim::events / eventCount — the staging point */
#include "game_sim.h"               /* GameSim.tanks */
#include "tank.h"                   /* tankSetWorld */
#include "client_enums.h"           /* sndEffects — bubbles, manLayingMineNear */
#include "sounddist.h"              /* SDIST_SOFT / SDIST_NONE */
#include "input_packet.h"
#include "transport_udp_internal.h" /* packGameEvent / unpackGameEvent */
#include "test_harness.h"

/* The listener square every arm measures from. Well inside the map on both
 * axes so a sound 60 squares along X still lands on a real square. */
#define SD_LISTENER_MX 100
#define SD_LISTENER_MY 100

/* Gaps the arms use, in map squares: inside SDIST_NONE, past it, and far
 * enough past it that only the tank-hit-to-self path can carry a sound. */
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
    /* Both hits are SD_GAP_FAR squares away, past SDIST_NONE. The one on slot 0
     * skips the distance cull because slot 0 is the player hit; the one on slot
     * 1 does not. */
    {
        const BYTE farMX = (BYTE)(SD_LISTENER_MX + SD_GAP_FAR);

        sdResetEvents(sim);
        sdAddSound(sim, EVENT_SOUND_TANK_HIT, (uint8_t)hitTankNear, farMX,
                   SD_LISTENER_MY, 0);
        sdAddSound(sim, EVENT_SOUND_TANK_HIT, (uint8_t)hitTankNear, farMX,
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
    }

    /* ---- manLayingMineNear on the listener's row ------------------------- */
    /* The builder culls every sound by distance alone, so a mine being laid
     * SD_GAP_HEARD squares along the listener's row is inside SDIST_NONE and is
     * delivered. What the listener then hears is decided in clientSoundDist,
     * whose rule for this one sound is gapX <= SDIST_SOFT || gapY <= SDIST_SOFT
     * (sounddist.c:164) — an OR where bubbles, the other near-only sound, uses
     * AND (sounddist.c:117) — so a zero Y gap makes it play at near volume the
     * whole way out. This arm pins that reach as it stands. */
    {
        const BYTE mineMX = (BYTE)(SD_LISTENER_MX + SD_GAP_HEARD);

        sdResetEvents(sim);
        sdAddSound(sim, EVENT_SOUND, (uint8_t)manLayingMineNear, mineMX,
                   SD_LISTENER_MY, 1);
        n = sdBuild(sim, 0, ev);

        hit = sdFind(ev, n, EVENT_SOUND, (uint8_t)manLayingMineNear);
        UT_ASSERT_MSG(hit != NULL,
                      "manLayingMineNear at %u,%u (%d squares along the "
                      "listener's row from %u,%u, Y gap 0) was not delivered — "
                      "%d event(s) came back, %d of them sound(s)",
                      (unsigned)mineMX, (unsigned)SD_LISTENER_MY, SD_GAP_HEARD,
                      (unsigned)SD_LISTENER_MX, (unsigned)SD_LISTENER_MY, n,
                      sdCountSounds(ev, n));
        UT_ASSERT_MSG(hit->data[1] == mineMX && hit->data[2] == SD_LISTENER_MY,
                      "manLayingMineNear came back for square %u,%u, staged at "
                      "%u,%u", hit->data[1], hit->data[2], (unsigned)mineMX,
                      (unsigned)SD_LISTENER_MY);
    }

    serverSimDestroy(sim);
    return 0;
}
