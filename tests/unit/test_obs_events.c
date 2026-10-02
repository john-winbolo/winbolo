/*
 * The event translator the gym and the in-game ML brain share
 * (obsBuildEventsFrom in obs_builder.c): which events it reads, and what each
 * one turns into.
 *
 *   obs_events_every_type_classified — every event type up to EVENT_LAST has
 *       an answer in obsEventIsRead, so a new type cannot reach the server's
 *       queue without somebody deciding whether the observation reads it; and
 *       no server-only type ever produces a sound.
 *   obs_events_kills — a kill needs a victim other than the agent, and an
 *       ally killed is not a kill.
 *   obs_events_hits — a hit dealt is one of the agent's own shells landing
 *       on another tank, not its mine or a pillbox's shell; a hit sound on its
 *       own credits nothing.
 *   obs_events_builders — the agent's own builder lost, and an enemy builder
 *       the agent killed.
 *   obs_events_sound_ids — a sound keeps its id, and the ids that have a
 *       sound type of their own get it.
 *   obs_events_pill_killed — a pillbox counts only when the agent killed it.
 *
 * Every case feeds a hand-built event list straight in, so none needs a game.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "input_packet.h"
#include "client_enums.h"   /* sndEffects */
#include "obs_builder.h"
#include "test_harness.h"

#define OE_SELF   0
#define OE_ALLY   1
#define OE_ENEMY  2
#define OE_ENEMY2 3
#define OE_TX     100   /* the agent's map square */
#define OE_TY     100

static GameEvent oeEvent(uint8_t type, uint8_t d0, uint8_t d1, uint8_t d2,
                         uint8_t d3, uint8_t d4) {
    GameEvent e;
    memset(&e, 0, sizeof(e));
    e.type = type;
    e.data[0] = d0;
    e.data[1] = d1;
    e.data[2] = d2;
    e.data[3] = d3;
    e.data[4] = d4;
    return e;
}

/* Translate one event list from the agent's seat, as the gym does when
 * fromServer is set and as the in-game brain does when it is not. */
static void oeBuild(const GameEvent *events, int count, bool fromServer,
                    WinBoloObs *obs) {
    memset(obs, 0, sizeof(*obs));
    obsBuildEventsFrom(events, count, fromServer, OE_SELF,
                       (PlayerBitMap)(1u << OE_ALLY), OE_TX, OE_TY, obs);
}

static int oeCount(const WinBoloObs *obs, uint8_t ev) {
    int n = 0;
    for (int i = 0; i < obs->num_events; i++) {
        if (obs->events[i] == ev) n++;
    }
    return n;
}

int run_obs_events_every_type_classified(void) {
    WinBoloObs *obs = calloc(1, sizeof(*obs));
    UT_ASSERT(obs != NULL);

    for (int t = 1; t <= EVENT_LAST; t++) {
        UT_ASSERT_MSG(obsEventIsRead((uint8_t)t) != OBS_EVENT_UNKNOWN,
                      "event type %d has no answer in obsEventIsRead; decide "
                      "whether the ML observation reads it", t);
    }
    UT_ASSERT_MSG(obsEventIsRead((uint8_t)(EVENT_LAST + 1)) == OBS_EVENT_UNKNOWN,
                  "obsEventIsRead names a type past EVENT_LAST; move EVENT_LAST");

    /* The in-game brain never receives a server-only event, so a sound built
       from one would be heard in training and never in a game. Fill every
       byte with the agent's own slot and square so any reading of the payload
       lands in range. */
    for (int t = 1; t <= EVENT_LAST; t++) {
        GameEvent e;
        if (!gameEventIsLocal((uint8_t)t)) continue;
        e.type = (uint8_t)t;
        for (int b = 0; b < GAME_EVENT_MAX_DATA; b++) {
            e.data[b] = (b == 0 || b == 3) ? OE_SELF : OE_TX;
        }
        oeBuild(&e, 1, true, obs);
        UT_ASSERT_MSG(obs->num_sounds == 0,
                      "server-only event type %d produced a sound", t);
    }

    free(obs);
    return 0;
}

int run_obs_events_kills(void) {
    WinBoloObs *obs = calloc(1, sizeof(*obs));
    GameEvent e;
    UT_ASSERT(obs != NULL);

    /* [killer, killed, deathCause, carriedPills] */
    e = oeEvent(EVENT_TANK_KILLED, OE_SELF, OE_ENEMY, LAST_DEATH_BY_SHELL, 0, 0);
    oeBuild(&e, 1, true, obs);
    UT_ASSERT(oeCount(obs, WBGYM_EVENT_KILL) == 1);
    UT_ASSERT(oeCount(obs, WBGYM_EVENT_DEATH) == 0);

    /* Driving onto its own mine names the dying tank as the killer. */
    e = oeEvent(EVENT_TANK_KILLED, OE_SELF, OE_SELF, LAST_DEATH_BY_MINES, 0, 0);
    oeBuild(&e, 1, true, obs);
    UT_ASSERT(oeCount(obs, WBGYM_EVENT_KILL) == 0);
    UT_ASSERT(oeCount(obs, WBGYM_EVENT_DEATH) == 1);

    /* Drowning, the same shape. */
    e = oeEvent(EVENT_TANK_KILLED, OE_SELF, OE_SELF, LAST_DEATH_BY_DEEPSEA, 0, 0);
    oeBuild(&e, 1, true, obs);
    UT_ASSERT(oeCount(obs, WBGYM_EVENT_KILL) == 0);
    UT_ASSERT(oeCount(obs, WBGYM_EVENT_DEATH) == 1);

    e = oeEvent(EVENT_TANK_KILLED, OE_SELF, OE_ALLY, LAST_DEATH_BY_SHELL, 0, 0);
    oeBuild(&e, 1, true, obs);
    UT_ASSERT(oeCount(obs, WBGYM_EVENT_KILL) == 0);
    UT_ASSERT(oeCount(obs, WBGYM_EVENT_ALLY_KILLED) == 1);

    e = oeEvent(EVENT_TANK_KILLED, OE_ENEMY, OE_SELF, LAST_DEATH_BY_SHELL, 0, 0);
    oeBuild(&e, 1, true, obs);
    UT_ASSERT(oeCount(obs, WBGYM_EVENT_KILL) == 0);
    UT_ASSERT(oeCount(obs, WBGYM_EVENT_DEATH) == 1);

    /* Two other players. */
    e = oeEvent(EVENT_TANK_KILLED, OE_ENEMY, OE_ENEMY2, LAST_DEATH_BY_SHELL, 0, 0);
    oeBuild(&e, 1, true, obs);
    UT_ASSERT(obs->num_events == 0);

    free(obs);
    return 0;
}

int run_obs_events_hits(void) {
    WinBoloObs *obs = calloc(1, sizeof(*obs));
    GameEvent e;
    UT_ASSERT(obs != NULL);

    /* [victim, attacker, cause, amount, pill] */
    e = oeEvent(EVENT_TANK_HIT, OE_ENEMY, OE_SELF, LAST_DEATH_BY_SHELL, 5, 0xFF);
    oeBuild(&e, 1, true, obs);
    UT_ASSERT(oeCount(obs, WBGYM_EVENT_HIT_DEALT) == 1);
    UT_ASSERT(oeCount(obs, WBGYM_EVENT_HIT_RECEIVED) == 0);

    e = oeEvent(EVENT_TANK_HIT, OE_SELF, OE_ENEMY, LAST_DEATH_BY_MINES, 10, 0xFF);
    oeBuild(&e, 1, true, obs);
    UT_ASSERT(oeCount(obs, WBGYM_EVENT_HIT_RECEIVED) == 1);
    UT_ASSERT(oeCount(obs, WBGYM_EVENT_HIT_DEALT) == 0);

    /* The agent's own mine damaging an enemy is not a shot that landed. */
    e = oeEvent(EVENT_TANK_HIT, OE_ENEMY, OE_SELF, LAST_DEATH_BY_MINES, 10, 0xFF);
    oeBuild(&e, 1, true, obs);
    UT_ASSERT(oeCount(obs, WBGYM_EVENT_HIT_DEALT) == 0);

    /* Nor is a pillbox's shell, whoever the event names. */
    e = oeEvent(EVENT_TANK_HIT, OE_ENEMY, OE_SELF, LAST_DEATH_BY_SHELL, 5, 3);
    oeBuild(&e, 1, true, obs);
    UT_ASSERT(oeCount(obs, WBGYM_EVENT_HIT_DEALT) == 0);

    /* A pillbox or another tank hitting someone else. */
    e = oeEvent(EVENT_TANK_HIT, OE_ENEMY, OE_ENEMY2, LAST_DEATH_BY_SHELL, 5, 0xFF);
    oeBuild(&e, 1, true, obs);
    UT_ASSERT(obs->num_events == 0);

    /* The hit sound names the tank hit and nobody else: from the server's
       queue it is a sound and never a hit, whoever was hit. */
    e = oeEvent(EVENT_SOUND_TANK_HIT, hitTankNear, OE_TX + 3, OE_TY, OE_ENEMY, 0);
    oeBuild(&e, 1, true, obs);
    UT_ASSERT(obs->num_events == 0);
    UT_ASSERT(obs->num_sounds == 1);
    UT_ASSERT(obs->sounds[0].type == WBGYM_SND_HIT_TANK);
    UT_ASSERT(obs->sounds[0].allegiance == WBGYM_ALLEG_ENEMY);

    e = oeEvent(EVENT_SOUND_TANK_HIT, hitTankNear, OE_TX, OE_TY, OE_SELF, 0);
    oeBuild(&e, 1, true, obs);
    UT_ASSERT(obs->num_events == 0);

    /* From a client's list, where EVENT_TANK_HIT never arrives, the agent's
       own hit sound stands in for a hit received, and another tank's hit
       sound still credits nothing. */
    oeBuild(&e, 1, false, obs);
    UT_ASSERT(oeCount(obs, WBGYM_EVENT_HIT_RECEIVED) == 1);

    e = oeEvent(EVENT_SOUND_TANK_HIT, hitTankNear, OE_TX + 3, OE_TY, OE_ENEMY, 0);
    oeBuild(&e, 1, false, obs);
    UT_ASSERT(obs->num_events == 0);

    free(obs);
    return 0;
}

int run_obs_events_builders(void) {
    WinBoloObs *obs = calloc(1, sizeof(*obs));
    GameEvent e;
    UT_ASSERT(obs != NULL);

    /* [victim, killer, quiet] */
    e = oeEvent(EVENT_LGM_LOST, OE_SELF, OE_ENEMY, 0, 0, 0);
    oeBuild(&e, 1, true, obs);
    UT_ASSERT(oeCount(obs, WBGYM_EVENT_LGM_LOST) == 1);
    UT_ASSERT(oeCount(obs, WBGYM_EVENT_ENEMY_LGM_KILLED) == 0);

    e = oeEvent(EVENT_LGM_LOST, OE_ENEMY, OE_SELF, 0, 0, 0);
    oeBuild(&e, 1, true, obs);
    UT_ASSERT(oeCount(obs, WBGYM_EVENT_ENEMY_LGM_KILLED) == 1);
    UT_ASSERT(oeCount(obs, WBGYM_EVENT_LGM_LOST) == 0);

    e = oeEvent(EVENT_LGM_LOST, OE_ALLY, OE_SELF, 0, 0, 0);
    oeBuild(&e, 1, true, obs);
    UT_ASSERT(obs->num_events == 0);

    e = oeEvent(EVENT_LGM_LOST, OE_ENEMY, NEUTRAL, 0, 0, 0);
    oeBuild(&e, 1, true, obs);
    UT_ASSERT(obs->num_events == 0);

    free(obs);
    return 0;
}

int run_obs_events_sound_ids(void) {
    WinBoloObs *obs = calloc(1, sizeof(*obs));
    static const struct {
        uint8_t id;
        uint8_t type;
    } cases[] = {
        { manDyingNear,      WBGYM_SND_LGM_LOST   },
        { manDyingFar,       WBGYM_SND_LGM_LOST   },
        { mineExplosionNear, WBGYM_SND_EXPLOSION  },
        { mineExplosionFar,  WBGYM_SND_EXPLOSION  },
        { bigExplosionNear,  WBGYM_SND_EXPLOSION  },
        { bigExplosionFar,   WBGYM_SND_EXPLOSION  },
        { manLayingMineNear, WBGYM_SND_MINE_PLACE },
        { farmingTreeNear,   WBGYM_SND_GENERIC    },
        { shotBuildingFar,   WBGYM_SND_GENERIC    },
    };
    GameEvent e;
    UT_ASSERT(obs != NULL);

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        e = oeEvent(EVENT_SOUND, cases[i].id, OE_TX + 5, OE_TY - 2, OE_ENEMY, 0);
        oeBuild(&e, 1, true, obs);
        UT_ASSERT_MSG(obs->num_sounds == 1, "sound id %u", cases[i].id);
        UT_ASSERT_MSG(obs->sounds[0].type == cases[i].type,
                      "sound id %u gave type %u, wanted %u", cases[i].id,
                      obs->sounds[0].type, cases[i].type);
        UT_ASSERT(obs->sounds[0].sound_id == cases[i].id);
        UT_ASSERT(obs->sounds[0].rx == 5.0f && obs->sounds[0].ry == -2.0f);
    }

    /* Out of earshot. */
    e = oeEvent(EVENT_SOUND, manDyingFar, OE_TX + 40, OE_TY, OE_ENEMY, 0);
    oeBuild(&e, 1, true, obs);
    UT_ASSERT(obs->num_sounds == 0);

    /* Another tank firing keeps its id too; the agent's own shot is left out. */
    e = oeEvent(EVENT_SOUND_SHOOT, shootNear, OE_TX + 1, OE_TY, OE_ENEMY, 0);
    oeBuild(&e, 1, true, obs);
    UT_ASSERT(obs->num_sounds == 1);
    UT_ASSERT(obs->sounds[0].type == WBGYM_SND_SHOOT);
    UT_ASSERT(obs->sounds[0].sound_id == shootNear);
    e = oeEvent(EVENT_SOUND_SHOOT, shootNear, OE_TX, OE_TY, OE_SELF, 0);
    oeBuild(&e, 1, true, obs);
    UT_ASSERT(obs->num_sounds == 0);

    free(obs);
    return 0;
}

int run_obs_events_pill_killed(void) {
    WinBoloObs *obs = calloc(1, sizeof(*obs));
    GameEvent e;
    UT_ASSERT(obs != NULL);

    /* [index, attacker] */
    e = oeEvent(EVENT_PILL_KILLED, 4, OE_SELF, 0, 0, 0);
    oeBuild(&e, 1, true, obs);
    UT_ASSERT(oeCount(obs, WBGYM_EVENT_PILL_KILLED) == 1);

    e = oeEvent(EVENT_PILL_KILLED, 4, OE_ENEMY, 0, 0, 0);
    oeBuild(&e, 1, true, obs);
    UT_ASSERT(obs->num_events == 0);

    e = oeEvent(EVENT_PILL_KILLED, 4, NEUTRAL, 0, 0, 0);
    oeBuild(&e, 1, true, obs);
    UT_ASSERT(obs->num_events == 0);

    free(obs);
    return 0;
}
