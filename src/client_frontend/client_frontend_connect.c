/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "client_frontend_connect.h"

#include <SDL3/SDL.h>

#include "client_connect_state.h"  /* CLIENT_CONNECT_* */
#include "client_net.h"            /* clientSimNetTick, clientSimGetConnectState */

ClientFrontJoinStep clientFrontAwaitJoinStep(ClientSim *cs, int *waitedTicks,
                                             int timeoutTicks) {
  if (*waitedTicks < timeoutTicks) {
    ClientConnectState js = clientSimGetConnectState(cs);
    /* Enter the lobby the instant it opens. Lobby-enabled servers deliver
     * CTRL_LOBBY_SETTINGS via sync replay before the map chunks, so inLobby
     * becomes true while the connect state is still DOWNLOADING_MAP. When
     * the first JOIN_ACCEPT is lost the state is still JOINING here and the
     * player's slot is not known yet; it arrives with the server's resent
     * accept. That is why readers of the slot must read it live
     * (gameFrontGetPlayerNum) and not take a copy when this wait returns. */
    if ((js == CLIENT_CONNECT_JOINING || js == CLIENT_CONNECT_DOWNLOADING_MAP) &&
        !clientSimIsInLobby(cs)) {
      clientSimNetTick(cs);
      (*waitedTicks)++;
      return CLIENT_FRONT_JOIN_WAITING;
    }
  }

  ClientConnectState finalState = clientSimGetConnectState(cs);
  bool inLobby = clientSimIsInLobby(cs);

  /* Settle the landing disposition in one place for every frontend: entering
   * the lobby means the client renders the in-game lobby rather than the
   * running game. The transport already flips netStat to netLobby alongside
   * inLobby (client_sim_control.c CTRL_GAME_PHASE_LOBBY), so this is defensive
   * — but keeping it here means no caller re-derives it (and can't smuggle in a
   * premature mapDownloadComplete the way a forked copy once did). Map install
   * and the mapDownloadComplete flag stay transport-driven. */
  if (inLobby) {
    clientSimSetNetStatus(cs, netLobby);
  }

  return (finalState == CLIENT_CONNECT_CONNECTED || inLobby)
             ? CLIENT_FRONT_JOIN_LANDED
             : CLIENT_FRONT_JOIN_FAILED;
}

bool clientFrontAwaitJoin(ClientSim *cs, int timeoutTicks) {
  int waited = 0;
  ClientFrontJoinStep step;

  while ((step = clientFrontAwaitJoinStep(cs, &waited, timeoutTicks)) ==
         CLIENT_FRONT_JOIN_WAITING) {
#if defined(__IPHONEOS__)
    /* This wait runs on the main thread for up to its whole timeout. iOS
     * delivers the app's lifecycle callbacks through the run loop that
     * pumping events turns, and ends an app that leaves it unserviced too
     * long; the events gathered stay queued for the caller. */
    SDL_PumpEvents();
#endif
    SDL_Delay(20);
  }
  return step == CLIENT_FRONT_JOIN_LANDED;
}
