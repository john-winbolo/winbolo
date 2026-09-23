#include <math.h>

#include "server_sim.h"
#include "server_sim_internal.h"
#include "bolo_map.h"
#include "tank.h"
#include "test_harness.h"
#include "../../src/gui/sdl3/turn_tap.h"

typedef struct SightPixel {
	BYTE mx, my, px, py;
} SightPixel;

static SightPixel sight_pixel(ServerSim *sim, float angle) {
	SightPixel result;
	tank tk = sim->sim.tanks[0];
	tankGetGunsightAt(&sim->sim, &tk, tk->x, tk->y, angle,
	                 &result.mx, &result.my, &result.px, &result.py);
	return result;
}

static bool same_pixel(SightPixel a, SightPixel b) {
	return a.mx == b.mx && a.my == b.my && a.px == b.px && a.py == b.py;
}

static void place_tank(ServerSim *sim, float angle, int offset) {
	mapSetPos(&sim->sim, &sim->sim.mp, 40, 40, GRASS, FALSE, FALSE);
	sim->sim.rules.turn_grass = 1.0f;
	tankSetWorld(&sim->sim, &sim->sim.tanks[0], 40 * 256 + 128 + offset,
	             40 * 256 + 128 + (15 - offset), angle, FALSE);
	tankSetDestroyed(&sim->sim.tanks[0], FALSE);
	tankSetDeathWait(&sim->sim.tanks[0], 0);
	tankSetOnBoat(&sim->sim.tanks[0], FALSE);
	tankSetFirstLeft(&sim->sim.tanks[0], 0);
	tankSetFirstRight(&sim->sim.tanks[0], 0);
}

int run_visible_turn_tap(void) {
	ServerSim *sim = ut_make_running_sim("Turner");
	int angle, offset, range, direction;
	UT_ASSERT(sim != NULL && sim->sim.tanks[0] != NULL);
	for (range = 2; range <= 14; range += 6) {
		for (offset = 0; offset < 16; offset++) {
			for (angle = 0; angle < 256; angle++) {
				for (direction = 0; direction < 2; direction++) {
					float after, amount, previous;
					SightPixel before;
					place_tank(sim, (float)angle, offset);
					tankSetGunsightLength(&sim->sim.tanks[0], (BYTE)range);
					before = sight_pixel(sim, (float)angle);
					tankTurn(&sim->sim, &sim->sim.tanks[0], 40, 40,
					         direction ? TRIGHT : TLEFT);
					after = tankGetAngle(&sim->sim.tanks[0]);
					UT_ASSERT_MSG(!same_pixel(before, sight_pixel(sim, after)),
					              "invisible tap: angle=%d offset=%d range=%d right=%d",
					              angle, offset, range, direction);
					amount = direction ? after - angle : angle - after;
					if (amount < 0) amount += 256;
					UT_ASSERT(amount >= 0.125f && amount < 4.0f);
					/* One less fine-turn increment should still be invisible:
					 * the minimum nudge must not skip a visible position. */
					previous = after + (direction ? -0.125f : 0.125f);
					if (previous < 0) previous += 256;
					if (previous > 256) previous -= 256;
					UT_ASSERT(same_pixel(before, sight_pixel(sim, previous)));
				}
			}
		}
	}
	serverSimDestroy(sim);
	return 0;
}

int run_visible_turn_held_and_reset(void) {
	ServerSim *sim = ut_make_running_sim("Turner");
	int direction, tick;
	UT_ASSERT(sim != NULL && sim->sim.tanks[0] != NULL);
	for (direction = 0; direction < 2; direction++) {
		tankButton held = direction ? TRIGHT : TLEFT;
		tankButton opposite = direction ? TLEFT : TRIGHT;
		SightPixel before;
		float angle;
		place_tank(sim, 128.0f, 8);
		tankSetGunsightLength(&sim->sim.tanks[0], 2);
		tankTurn(&sim->sim, &sim->sim.tanks[0], 40, 40, held);
		for (tick = 2; tick <= 20; tick++) {
			float step;
			angle = tankGetAngle(&sim->sim.tanks[0]);
			tankTurn(&sim->sim, &sim->sim.tanks[0], 40, 40, held);
			step = fabsf(tankGetAngle(&sim->sim.tanks[0]) - angle);
			UT_ASSERT(fabsf(step - (tick <= 6 ? 0.125f : 1.0f)) < 0.0001f);
		}
		angle = tankGetAngle(&sim->sim.tanks[0]);
		tankTurn(&sim->sim, &sim->sim.tanks[0], 40, 40, TNONE);
		UT_ASSERT(tankGetAngle(&sim->sim.tanks[0]) == angle);
		before = sight_pixel(sim, angle);
		tankTurn(&sim->sim, &sim->sim.tanks[0], 40, 40, held);
		UT_ASSERT(!same_pixel(before, sight_pixel(sim, tankGetAngle(&sim->sim.tanks[0]))));
		before = sight_pixel(sim, tankGetAngle(&sim->sim.tanks[0]));
		tankTurn(&sim->sim, &sim->sim.tanks[0], 40, 40, opposite);
		UT_ASSERT(!same_pixel(before, sight_pixel(sim, tankGetAngle(&sim->sim.tanks[0]))));
		place_tank(sim, 128.0f, 8);
		sim->sim.rules.turn_grass = 0;
		tankTurn(&sim->sim, &sim->sim.tanks[0], 40, 40, held);
		UT_ASSERT(tankGetAngle(&sim->sim.tanks[0]) == 128.0f);
	}
	serverSimDestroy(sim);
	return 0;
}

int run_turn_tap_input_edges(void) {
	TurnTapState state = {0};
	int i;
	/* A complete press/release between samples is delivered once. */
	turnTapEvent(&state, true, true);
	turnTapEvent(&state, false, true);
	UT_ASSERT(turnTapRead(&state, false));
	UT_ASSERT(!turnTapRead(&state, false));
	/* A second press with an unsampled release gets a distinct turn start. */
	turnTapEvent(&state, true, true);
	UT_ASSERT(turnTapRead(&state, true));
	turnTapEvent(&state, false, true);
	turnTapEvent(&state, true, true);
	UT_ASSERT(!turnTapRead(&state, true));
	UT_ASSERT(turnTapRead(&state, true));
	/* OS repeat never injects releases into a continuous held turn. */
	for (i = 0; i < 20; i++) {
		turnTapEvent(&state, true, true);
		UT_ASSERT(turnTapRead(&state, true));
	}
	turnTapEvent(&state, false, true);
	UT_ASSERT(!turnTapRead(&state, false));
	/* Two queued taps remain two separate starts. */
	for (i = 0; i < 2; i++) {
		turnTapEvent(&state, true, true);
		turnTapEvent(&state, false, true);
	}
	UT_ASSERT(turnTapRead(&state, false));
	UT_ASSERT(!turnTapRead(&state, false));
	UT_ASSERT(turnTapRead(&state, false));
	UT_ASSERT(!turnTapRead(&state, false));
	/* Rejected UI input and focus resets cannot leave a queued turn. */
	turnTapEvent(&state, true, false);
	turnTapEvent(&state, false, false);
	UT_ASSERT(!turnTapRead(&state, false));
	turnTapEvent(&state, true, true);
	turnTapReset(&state);
	UT_ASSERT(!turnTapRead(&state, false));
	return 0;
}
