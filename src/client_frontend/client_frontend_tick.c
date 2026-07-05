#include "client_frontend_tick.h"

#include <stdint.h>

#include "brainsHandler.h"  /* brainHandlerIsBrainRunning */
#include "client_net.h"     /* clientBuildInputPacket, clientSimNet* */
#include "clientmutex.h"    /* clientMutexWaitFor / clientMutexRelease */
#include "gamefront.h"      /* gameFrontGetPlayerNum */
#include "input.h"          /* keyItems, inputGetKeys, gunsight consume, ... */
#include "input_packet.h"   /* InputPacket, INPUT_FLAG_GUNSIGHT_SHIFT */

/* Frontend globals defined once per client binary (winbolo.c on desktop,
 * main_wasm.c on web). */
extern keyItems keys;
extern bool isInMenu;

static bool justKeysFlag = false;
static uint32_t simTickCounter = 0;

void clientFrontTickReset(void) {
  justKeysFlag = false;
  simTickCounter = 0;
}

bool clientFrontRunTickStep(ClientSim *cs) {
  bool brainRunning = brainHandlerIsBrainRunning();
  BYTE myPlayerNum = gameFrontGetPlayerNum();
  netStatus ns = clientSimGetNetStatus(cs);

  if (ns == netLobby || ns == netLobbyCountdown) {
    /* Lobby/countdown: just tick the transport to receive packets. Keep the
     * cadence flipping so the first running step lands on a keys tick. */
    clientSimNetTick(cs);
    justKeysFlag = !justKeysFlag;
    return false;
  }

  if (justKeysFlag) {
    /* Keys tick */
    tankButton tb = 0;
    if (!brainRunning) {
      tb = inputGetKeys(cs, &keys, isInMenu);
    } else {
      inputScroll(cs, &keys, isInMenu);
    }
    InputPacket pkt;
    clientBuildInputPacket(cs, &pkt, tb, false, false, brainRunning, false,
                           myPlayerNum, simTickCounter);
    if (!brainRunning) {
      uint8_t gsAdj = inputConsumeGunsightAdj();
      if (gsAdj) pkt.flags |= ((gsAdj & 0x3) << INPUT_FLAG_GUNSIGHT_SHIFT);
    }
    clientMutexWaitFor();
    clientSimKeysTick(cs, &pkt);
    clientMutexRelease();
    clientSimNetRecordInput(cs, &pkt);
    /* Pump the transport on the keys half only when it does not advance the
     * server itself. An active local transport (wasm SP) runs serverSimTick
     * inside tick(), so a second pump here would step the sim twice per
     * frame. Passive-local (desktop SP — host timer thread ticks the server)
     * and UDP clients keep the pump: for them tick() only pulls/applies a
     * snapshot, and sampling at ~10ms against the server's ~20ms tick is
     * what guarantees no per-tick snapshot events are missed (see the dedup
     * note in transport_local.c). */
    if (!clientSimTransportTicksServer(cs)) {
      clientSimNetTick(cs);
    }
    simTickCounter++;
    justKeysFlag = false;
    return false;
  }

  /* Game tick */
  tankButton tb = 0;
  bool isShoot = false;
  bool isMine = false;
  if (!brainRunning) {
    tb = inputGetKeys(cs, &keys, isInMenu);
    isShoot = inputIsFireKeyPressed(&keys, isInMenu);
    isMine = inputIsMineKeyPressed(&keys, isInMenu);
  } else {
    inputScroll(cs, &keys, isInMenu);
  }
  InputPacket pkt;
  clientBuildInputPacket(cs, &pkt, tb, isShoot, isMine, brainRunning, true,
                         myPlayerNum, simTickCounter);
  if (!brainRunning) {
    uint8_t gsAdj = inputConsumeGunsightAdj();
    if (gsAdj) pkt.flags |= ((gsAdj & 0x3) << INPUT_FLAG_GUNSIGHT_SHIFT);
  }
  clientMutexWaitFor();
  clientSimGameTick(cs, &pkt, brainRunning);
  clientMutexRelease();
  clientSimNetSendInput(cs, &pkt);
  clientSimNetTick(cs);
  clientMutexWaitFor();
  clientSimDisplayTick(cs, brainRunning);
  clientMutexRelease();
  simTickCounter++;
  justKeysFlag = true;
  return true;
}
