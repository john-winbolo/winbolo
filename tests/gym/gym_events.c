/*
 * Drives the gym through its public API and counts what the agent observes.
 *
 * The unit tests (tests/unit/test_obs_events.c) feed made-up event lists to
 * the translator; this checks the part they cannot reach, that events raised
 * by a real game get through the gym's own event handling to the
 * observation. Every event and sound type seen is printed, so a run doubles
 * as the live count to look at when a type goes missing.
 *
 * The agent plays alone on tests/test_arena.map: it shoots into the forest
 * and buildings around its start, then sends its builder out to lay mines.
 * The assertions are what that script has to produce, and what playing alone
 * must never produce:
 *   - shells landing make sounds, and they carry their game sound id
 *   - the builder laying a mine is heard as a mine being placed
 *   - nobody else is in the game, so there is no kill and no hit dealt
 *   - nothing produces the never-raised shell-fired sound type
 *
 * It resets the game a few times first: a reset must leave the agent alone on
 * the map, in slot 0, which is the slot every read in the gym assumes.
 *
 * Usage: WinBoloGymEvents <map>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "winbolo_gym.h"

#define GE_STEPS_SHOOT  600
#define GE_STEPS_BUILD  900
#define GE_NUM_SND      7
#define GE_NUM_EVENTS   15

static const char *geSoundNames[GE_NUM_SND] = {
    "shoot", "explosion", "hit_tank", "mine_place", "generic", "lgm_lost",
    "shell_fired"
};

static const char *geEventNames[GE_NUM_EVENTS] = {
    "hit_dealt", "kill", "death", "hit_received", "pill_captured", "pill_lost",
    "base_captured", "base_lost", "lgm_lost", "assist_man_dead",
    "assist_no_tree", "assist_buildtank", "enemy_lgm_killed", "ally_killed",
    "pill_killed"
};

int main(int argc, char **argv) {
    WinBoloGym *g;
    WinBoloObs *obs;
    WinBoloAction act;
    long sounds[GE_NUM_SND] = {0};
    long events[GE_NUM_EVENTS] = {0};
    long soundsWithId = 0;
    int failures = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <map>\n", argv[0]);
        return 2;
    }
    if (winbolo_obs_size() != (int)sizeof(WinBoloObs)) {
        fprintf(stderr, "FAIL obs size %d, header says %d\n",
                winbolo_obs_size(), (int)sizeof(WinBoloObs));
        return 1;
    }

    obs = calloc(1, sizeof(*obs));
    g = winbolo_create(argv[1], WBGYM_GAME_OPEN);
    if (obs == NULL || g == NULL) {
        fprintf(stderr, "FAIL could not create a game on %s\n", argv[1]);
        return 1;
    }
    for (int r = 0; r < 3; r++) {
        int tanks = 0, selfId = -1;
        memset(&act, 0, sizeof(act));
        winbolo_reset(g, obs);
        winbolo_step(g, &act, obs);
        for (int i = 0; i < obs->num_entities; i++) {
            if (obs->entities[i].type != WBGYM_ENT_TANK) continue;
            tanks++;
            if (obs->entities[i].flags & WBGYM_FLAG_IS_SELF) selfId = obs->entities[i].id;
        }
        if (tanks != 1 || selfId != 0) {
            fprintf(stderr, "FAIL after reset %d: %d tanks on the map, agent in "
                    "slot %d; want 1 tank, slot 0\n", r + 1, tanks, selfId);
            failures++;
        }
    }

    for (int step = 0; step < GE_STEPS_SHOOT + GE_STEPS_BUILD; step++) {
        memset(&act, 0, sizeof(act));
        if (step < GE_STEPS_SHOOT) {
            /* Turn on the spot, firing, so shells land in the buildings and
               forest around the start. */
            act.turn = 1;
            act.shoot = 1;
        } else if (step % 40 == 0) {
            /* Lay a mine a few squares away, in a different direction each
               time so at least one lands on ground that takes one. */
            static const int offs[8][2] = {
                {3, 0}, {0, 3}, {-3, 0}, {0, -3}, {3, 3}, {-3, 3}, {3, -3}, {-3, -3}
            };
            int k = (step / 40) % 8;
            act.build_action = WBGYM_BUILD_MINE;
            act.build_rx = offs[k][0];
            act.build_ry = offs[k][1];
        }
        winbolo_step(g, &act, obs);

        for (int i = 0; i < obs->num_sounds; i++) {
            if (obs->sounds[i].type < GE_NUM_SND) sounds[obs->sounds[i].type]++;
            if (obs->sounds[i].sound_id != 0) soundsWithId++;
        }
        for (int i = 0; i < obs->num_events; i++) {
            if (obs->events[i] < GE_NUM_EVENTS) events[obs->events[i]]++;
        }
    }

    printf("sounds:");
    for (int i = 0; i < GE_NUM_SND; i++) printf(" %s=%ld", geSoundNames[i], sounds[i]);
    printf(" (with a sound id: %ld)\nevents:", soundsWithId);
    for (int i = 0; i < GE_NUM_EVENTS; i++) printf(" %s=%ld", geEventNames[i], events[i]);
    printf("\n");

    if (sounds[WBGYM_SND_GENERIC] == 0 || soundsWithId == 0) {
        fprintf(stderr, "FAIL no shell landing was heard with its sound id\n");
        failures++;
    }
    if (sounds[WBGYM_SND_MINE_PLACE] == 0) {
        fprintf(stderr, "FAIL the builder laying mines was never heard\n");
        failures++;
    }
    if (events[WBGYM_EVENT_KILL] != 0 || events[WBGYM_EVENT_HIT_DEALT] != 0) {
        fprintf(stderr, "FAIL a lone agent was credited with a kill or a hit\n");
        failures++;
    }
    if (sounds[WBGYM_SND_SHELL_FIRED] != 0) {
        fprintf(stderr, "FAIL the never-raised shell-fired sound type appeared\n");
        failures++;
    }

    winbolo_destroy(g);
    free(obs);
    return failures == 0 ? 0 : 1;
}
