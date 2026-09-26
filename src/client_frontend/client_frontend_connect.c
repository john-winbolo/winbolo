#include "client_frontend_connect.h"

#include <SDL3/SDL.h>

#include "client_connect_state.h"  /* CLIENT_CONNECT_* */
#include "client_net.h"            /* clientSimNetTick, clientSimGetConnectState */

bool clientFrontAwaitJoin(ClientSim *cs, int timeoutTicks) {
  int waited = 0;

  while (waited < timeoutTicks) {
    ClientConnectState js = clientSimGetConnectState(cs);
    if (js != CLIENT_CONNECT_JOINING && js != CLIENT_CONNECT_DOWNLOADING_MAP) {
      break;
    }
    /* Enter the lobby the instant it opens. Lobby-enabled servers deliver
     * CTRL_LOBBY_SETTINGS via sync replay before the map chunks, so inLobby
     * becomes true while the connect state is still DOWNLOADING_MAP. When
     * the first JOIN_ACCEPT is lost the state is still JOINING here and the
     * player's slot is not known yet; it arrives with the server's resent
     * accept. That is why readers of the slot must read it live
     * (gameFrontGetPlayerNum) and not take a copy when this wait returns. */
    if (clientSimIsInLobby(cs)) {
      break;
    }
    clientSimNetTick(cs);
    SDL_Delay(20);
    waited++;
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

  return finalState == CLIENT_CONNECT_CONNECTED || inLobby;
}
