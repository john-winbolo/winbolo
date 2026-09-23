/* A stationary client's released angle must survive a delayed release and
 * subsequent server snapshots. Short input gaps must not add unacknowledged
 * turns; long gaps may substitute inputs, but must not execute them twice. */
#include <math.h>

#include "bolo_map.h"
#include "client_net.h"
#include "client_sim.h"
#include "client_sim_internal.h"
#include "server_sim.h"
#include "server_sim_internal.h"
#include "tank.h"
#include "test_harness.h"

#define TURN_MAP_POS 32
#define TURN_WORLD_POS (TURN_MAP_POS * 256 + 128)

static InputPacket turn_input(ClientSim *cs, uint32_t tick, uint8_t buttons) {
	InputPacket pkt = {0};
	pkt.tick = tick;
	pkt.playerNum = clientSimGetMyPlayerNum(cs);
	pkt.buttons = buttons;
	if (tick % 2) {
		clientSimKeysTick(cs, &pkt);
	} else {
		clientSimGameTick(cs, &pkt, FALSE);
	}
	return pkt;
}

static void place_turn_tank(GameSim *gs, BYTE slot) {
	gs->rules.turn_grass = 1.0f;
	gs->inStartFind = FALSE;
	tankSetWorld(gs, &gs->tanks[slot], TURN_WORLD_POS, TURN_WORLD_POS, 128, FALSE);
	tankSetDestroyed(&gs->tanks[slot], FALSE);
	tankSetDeathWait(&gs->tanks[slot], 0);
	tankSetArmour(&gs->tanks[slot], 40);
	tankSetOnBoat(&gs->tanks[slot], FALSE);
	tankSetSpeed(&gs->tanks[slot], 0);
	tankSetFirstLeft(&gs->tanks[slot], 0);
	tankSetFirstRight(&gs->tanks[slot], 0);
}

/* gap_frames delays input delivery, not client prediction. The local
 * transport is used only to join and fetch real server-built snapshots;
 * server ticks and input delivery are explicitly controlled by this test. */
static int turn_gap_case(uint8_t held, int hold_ticks, int gap_frames,
                         uint8_t after_gap, bool long_gap) {
	ServerSim *sim = ut_make_running_sim("Host");
	ClientSim *cs = clientSimAlloc();
	InputPacket pending[8];
	BYTE slot;
	uint32_t next_tick = 1;
	uint32_t ack_before_gap, release_tick;
	float stopped_angle, rendered_angle;
	WORLD stopped_x, stopped_y;
	int i, frame, pending_count = long_gap ? 8 : 2;
	int x, y;
	UT_ASSERT(sim != NULL && cs != NULL);
	/* Prepare safe terrain before joining so the client's map and its
	 * authoritative map shadow both receive the same patch. */
	for (y = TURN_MAP_POS - 1; y <= TURN_MAP_POS + 1; y++) {
		for (x = TURN_MAP_POS - 1; x <= TURN_MAP_POS + 1; x++) {
			mapSetPos(&sim->sim, &sim->sim.mp, (BYTE)x, (BYTE)y, GRASS, FALSE, FALSE);
		}
	}
	UT_ASSERT(clientSimCreate(cs));
	UT_ASSERT(clientSimConnectLocal(cs, sim, "Turner", "", 0, 0));
	slot = clientSimGetMyPlayerNum(cs);
	place_turn_tank(&sim->sim, slot);
	place_turn_tank(&cs->sim, slot);
	UT_ASSERT(clientSimNetSyncSnapshot(cs));
	UT_ASSERT(cs->clientState.hasPredictedTank);

	/* Establish the stream, then turn. No drift is expected on a clean link. */
	for (frame = 0; frame < (4 + hold_ticks) / 2; frame++) {
		for (i = 0; i < 2; i++) {
			InputPacket pkt = turn_input(cs, next_tick++, frame < 2 ? 0 : held);
			serverSimApplyInput(sim, &pkt);
		}
		serverSimTick(sim);
		UT_ASSERT(clientSimNetSyncSnapshot(cs));
		clientSimDisplayTick(cs, FALSE);
	}
	UT_ASSERT(fabsf(tankGetAngle(&MY_TANK(cs)) - 128.0f) > 0.0f);
	ack_before_gap = sim->lastProcessedInput[slot];
	UT_ASSERT(ack_before_gap == next_tick - 1);
	UT_ASSERT_MSG(!tankIsDestroyed(&MY_TANK(cs)) && tankGetDeathWait(&MY_TANK(cs)) == 0,
	              "client cannot predict: destroyed=%d deathWait=%u",
	              tankIsDestroyed(&MY_TANK(cs)), tankGetDeathWait(&MY_TANK(cs)));
	UT_ASSERT(!cs->sim.paused && !sim->sim.paused);
	UT_ASSERT(mapGetPos(&cs->sim.mp, TURN_MAP_POS, TURN_MAP_POS) == GRASS);
	UT_ASSERT(mapGetPos(&sim->sim.mp, TURN_MAP_POS, TURN_MAP_POS) == GRASS);
	UT_ASSERT((held == INPUT_BTN_LEFT ? tankGetFirstLeft(&MY_TANK(cs)) :
	           tankGetFirstRight(&MY_TANK(cs))) == (hold_ticks < 6 ? hold_ticks : 6));

	/* The client predicts ahead and stops. With a long gap it keeps turning
	 * for six ticks before releasing, so the server's substitutes are correct
	 * guesses; retransmitting those inputs must not add more turning. */
	for (i = 0; i < pending_count; i++) {
		pending[i] = turn_input(cs, next_tick++, long_gap && i < 6 ? held : after_gap);
	}
	stopped_angle = tankGetAngle(&MY_TANK(cs));
	tankGetWorld(&MY_TANK(cs), &stopped_x, &stopped_y);
	UT_ASSERT(stopped_x == TURN_WORLD_POS && stopped_y == TURN_WORLD_POS);
	release_tick = next_tick - 1;
	for (frame = 0; frame < gap_frames; frame++) serverSimTick(sim);
	UT_ASSERT_MSG(sim->lastProcessedInput[slot] == ack_before_gap +
	              (long_gap ? gap_frames * 2 - STALL_ADVANCE_DRY_TICKS : 0),
	              "unexpected ACK during %d-frame gap: %u -> %u", gap_frames,
	              ack_before_gap, sim->lastProcessedInput[slot]);

	for (i = 0; i < pending_count; i++) serverSimApplyInput(sim, &pending[i]);
	for (frame = 0; frame < 6 && sim->lastProcessedInput[slot] < release_tick; frame++) {
		serverSimTick(sim);
		UT_ASSERT(clientSimNetSyncSnapshot(cs));
		UT_ASSERT_MSG(fabsf(tankGetAngle(&MY_TANK(cs)) - stopped_angle) < 0.001f,
		              "held=%u next=%u gap=%d hold=%d: snapshot moved stopped angle %.3f -> %.3f",
		              held, after_gap, gap_frames, hold_ticks, stopped_angle,
		              tankGetAngle(&MY_TANK(cs)));
	}
	UT_ASSERT(sim->lastProcessedInput[slot] >= release_tick);
	UT_ASSERT(fabsf(tankGetAngle(&sim->sim.tanks[slot]) - stopped_angle) < 0.001f);
	/* Let the display offset decay: hiding a correction temporarily is not
	 * enough. The rendered angle must still be the one the player chose. */
	for (i = 0; i < 10; i++) clientSimDisplayTick(cs, FALSE);
	clientSimGetRenderedTankPos(cs, NULL, NULL, &rendered_angle);
	UT_ASSERT(fabsf(rendered_angle - stopped_angle) < 0.001f);

	clientSimDestroy(cs);
	serverSimDestroy(sim);
	return 0;
}

int run_turn_release_no_overshoot(void) {
	int direction, gap_frames;
	for (direction = 0; direction < 2; direction++) {
		for (gap_frames = 0; gap_frames <= 2; gap_frames++) {
			UT_ASSERT(turn_gap_case(direction ? INPUT_BTN_RIGHT : INPUT_BTN_LEFT,
			                        12, gap_frames, 0, FALSE) == 0);
		}
	}
	return 0;
}

int run_turn_gap_preserves_ramp(void) {
	int direction, hold_ticks, reverse;
	for (direction = 0; direction < 2; direction++) {
		uint8_t held = direction ? INPUT_BTN_RIGHT : INPUT_BTN_LEFT;
		for (hold_ticks = 2; hold_ticks <= 12; hold_ticks += 10) {
			for (reverse = 0; reverse < 2; reverse++) {
				uint8_t after_gap = reverse ? (direction ? INPUT_BTN_LEFT : INPUT_BTN_RIGHT) : held;
				UT_ASSERT(turn_gap_case(held, hold_ticks, 2, after_gap, FALSE) == 0);
			}
		}
	}
	return 0;
}

int run_turn_long_gap_no_duplicate(void) {
	UT_ASSERT(turn_gap_case(INPUT_BTN_LEFT, 12, 3, 0, TRUE) == 0);
	UT_ASSERT(turn_gap_case(INPUT_BTN_RIGHT, 12, 3, 0, TRUE) == 0);
	return 0;
}
