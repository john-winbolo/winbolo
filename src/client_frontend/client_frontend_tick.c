/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "client_frontend_tick.h"

#include <stdint.h>

#include "brainsHandler.h"  /* brainHandlerIsBrainRunning */
#include "client_net.h"     /* clientBuildInputPacket, clientSimNet* */
#include "clientmutex.h"    /* clientMutexWaitFor / clientMutexRelease */
#include "gamefront.h"      /* gameFrontGetPlayerNum */
#include "input.h"          /* keyItems, inputGetKeys, gunsight consume, ... */
#include "input_packet.h"   /* InputPacket, INPUT_FLAG_GUNSIGHT_SHIFT, INPUT_FLAG_AUTOSLOW */

/* Frontend globals defined once per client binary (winbolo.c on desktop,
 * main_wasm.c on web). */
extern keyItems keys;
extern bool isInMenu;

/* Tick number stamped on outgoing InputPackets. Its parity is the wire
 * contract for intent — even = game tick, odd = keys tick (the server routes
 * each input by tick % 2, server_sim.c) — so the keys/game cadence below is
 * derived from this counter instead of tracked in a separate flag. A separate
 * flag can drift out of phase with the counter (the wasm in-canvas lobby used
 * to flip it without advancing the counter), after which every game-intent
 * packet carries an odd tick and the server silently ignores the fire action
 * for the rest of the game. */
static uint32_t simTickCounter = 0;

void clientFrontTickReset(void) {
  simTickCounter = 0;
}

bool clientFrontRunTickStep(ClientSim *cs) {
  bool brainRunning = brainHandlerIsBrainRunning();
  BYTE myPlayerNum = gameFrontGetPlayerNum();
  netStatus ns = clientSimGetNetStatus(cs);

  if (ns == netLobby || ns == netLobbyCountdown) {
    /* Lobby/countdown: just tick the transport to receive packets. The
     * counter does not advance here, so the first running step is always a
     * game step on an even tick number — same state as after
     * clientFrontTickReset. */
    clientSimNetTick(cs);
    return false;
  }

  if ((simTickCounter % 2) == 1) {
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
      if (inputAutoSlowdownAssist()) pkt.flags |= INPUT_FLAG_AUTOSLOW;
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
    if (inputAutoSlowdownAssist()) pkt.flags |= INPUT_FLAG_AUTOSLOW;
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
  return true;
}
