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
     * becomes true while the connect state is still DOWNLOADING_MAP. */
    if (clientSimIsInLobby(cs)) {
      break;
    }
    clientSimNetTick(cs);
    SDL_Delay(20);
    waited++;
  }

  ClientConnectState finalState = clientSimGetConnectState(cs);
  return finalState == CLIENT_CONNECT_CONNECTED || clientSimIsInLobby(cs);
}
