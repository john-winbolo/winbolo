/* Pending range inputs must survive snapshots, even with no pose correction.
 * Exercise the live snapshot handler, including acknowledgement, tick parity,
 * range limits and the local-only gunsight visibility preference. */
#include <string.h>

#include "client_sim.h"
#include "client_net.h"
#include "client_sim_internal.h"
#include "client_snapshot.h"
#include "server_sim.h"
#include "tank.h"
#include "test_harness.h"

static void apply_range_snapshot(ClientSim *cs, uint32_t ack, BYTE range,
                                bool correct_angle) {
  SnapshotHeader hdr = {0};
  TankSnapshot snap = {0};
  tank *tk = &MY_TANK(cs);
  hdr.serverTick = cs->clientState.lastAppliedServerTick + 1;
  hdr.lastProcessedInput = ack;
  hdr.tankCount = 1;
  snap.playerNum = clientSimGetMyPlayerNum(cs);
  tankGetWorld(tk, &snap.worldX, &snap.worldY);
  snap.angle = (uint16_t)(tankGetAngle(tk) * 256.0f);
  if (correct_angle) snap.angle ^= 4096;
  snap.armour = 40;
  snap.gunsightLen = range;
  clientApplySnapshot(cs, &hdr, &snap, 1, NULL, 0, NULL, 0,
                      NULL, 0, NULL, 0, NULL, 0, snap.playerNum);
}

static void record_range(ClientSim *cs, uint32_t tick, uint8_t adjustment) {
  InputPacket pkt = {0};
  pkt.tick = tick;
  pkt.flags = adjustment << INPUT_FLAG_GUNSIGHT_SHIFT;
  clientStateRecordInput(&cs->clientState, &pkt);
}

int run_gunsight_reconciliation(void) {
  ServerSim *server = ut_make_running_sim("Host");
  ClientSim *cs = clientSimAlloc();
  unsigned int corrections;
  int correct_angle;
  UT_ASSERT(server != NULL && cs != NULL);
  UT_ASSERT(clientSimCreate(cs));
  UT_ASSERT(clientSimConnectLocal(cs, server, "Joiner", "", 0, 0));
  UT_ASSERT(MY_TANK(cs) != NULL);
  apply_range_snapshot(cs, 10, 8, FALSE);
  UT_ASSERT(cs->clientState.hasPredictedTank);

  /* Both branches must replay odd keys ticks and even game ticks exactly
   * once, starting from the acknowledged range rather than the local one. */
  for (correct_angle = 0; correct_angle <= 1; correct_angle++) {
    memset(cs->clientState.history, 0, sizeof(cs->clientState.history));
    record_range(cs, 10, 2); /* Already acknowledged: never replay. */
    record_range(cs, 11, 1);
    record_range(cs, 12, 1);
    record_range(cs, 13, 2);
    tankSetGunsightLength(&MY_TANK(cs), 9);
    corrections = cs->reconCountThisWindow;
    apply_range_snapshot(cs, 10, 8, correct_angle != 0);
    UT_ASSERT(tankGetGunsightLength(&MY_TANK(cs)) == 9);
    UT_ASSERT(cs->reconCountThisWindow == corrections + correct_angle);

    /* Another snapshot with the same ACK must not accumulate adjustments. */
    apply_range_snapshot(cs, 10, 8, FALSE);
    UT_ASSERT(tankGetGunsightLength(&MY_TANK(cs)) == 9);
    apply_range_snapshot(cs, 11, 9, FALSE);
    UT_ASSERT(tankGetGunsightLength(&MY_TANK(cs)) == 9);
    apply_range_snapshot(cs, 12, 10, FALSE);
    UT_ASSERT(tankGetGunsightLength(&MY_TANK(cs)) == 9);
    apply_range_snapshot(cs, 13, 9, FALSE);
    UT_ASSERT(tankGetGunsightLength(&MY_TANK(cs)) == 9);
    /* With nothing pending, the server's value wins. */
    apply_range_snapshot(cs, 13, 7, FALSE);
    UT_ASSERT(tankGetGunsightLength(&MY_TANK(cs)) == 7);
  }

  /* Clamp each adjustment in order (a net sum would give the wrong answer)
   * and use the configured limits rather than the classic constants. */
  cs->sim.rules.gunsight_min = 5;
  cs->sim.rules.gunsight_max = 11;
  tankSetAutoHideGunsight(&MY_TANK(cs), TRUE);
  tankSetGunsight(&cs->sim, &MY_TANK(cs), TRUE);
  record_range(cs, 21, 1);
  record_range(cs, 22, 2);
  apply_range_snapshot(cs, 20, 11, FALSE);
  UT_ASSERT(tankGetGunsightLength(&MY_TANK(cs)) == 10);
  UT_ASSERT(tankIsGunsightShow(&MY_TANK(cs)));

  /* Replaying an earlier decrease must not undo a later local hide. */
  tankSetGunsight(&cs->sim, &MY_TANK(cs), FALSE);
  record_range(cs, 23, 2);
  record_range(cs, 24, 1);
  apply_range_snapshot(cs, 22, 5, FALSE);
  UT_ASSERT(tankGetGunsightLength(&MY_TANK(cs)) == 6);
  UT_ASSERT(!tankIsGunsightShow(&MY_TANK(cs)));

  /* A reused ring-buffer slot is not a pending input for the missing tick. */
  record_range(cs, 25 + CLIENT_INPUT_HISTORY_SIZE, 1);
  record_range(cs, 26, 2);
  apply_range_snapshot(cs, 24, 8, FALSE);
  UT_ASSERT(tankGetGunsightLength(&MY_TANK(cs)) == 7);

  clientSimDestroy(cs);
  serverSimDestroy(server);
  return 0;
}
