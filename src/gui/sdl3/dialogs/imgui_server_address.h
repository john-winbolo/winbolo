/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/********************************************************
 *Name:          imgui_server_address
 *Filename:      imgui_server_address.h
 *Purpose:
 *  Shared rendering for the server address shown on the lobby and network
 *  information screens. Resolves the address to a hostname via the existing
 *  background DNS lookup thread (visual only — never changes the connected
 *  host) and draws it as a clickable join-link that copies a
 *  winbolo://host:port URL to the clipboard.
 *
 *  Header-only (static inline) so it can be shared between imgui_lobby.cpp
 *  and sdl3imgui.cpp without a new translation unit / CMake entry.
 *********************************************************/

#ifndef IMGUI_SERVER_ADDRESS_H
#define IMGUI_SERVER_ADDRESS_H

#include <SDL3/SDL.h>
#include <string.h>

#include "imgui.h"
#include "imgui_dialog_utils.h" /* imguiHandOnHover (C++ inline helpers) */

/* These are C headers. This header may be pulled in ahead of a translation
 * unit's own extern "C" include of client_sim.h (e.g. imgui_lobby.cpp), and
 * because of include guards whichever include is first wins the linkage. Wrap
 * them here so the clientSim* symbols always get C linkage regardless of order
 * — otherwise they mangle as C++ and fail to link. global.h must come first:
 * client_sim.h depends on its types. */
extern "C" {
#include "global.h"
#include "platform_net.h" /* sockets, inet_ntoa/inet_pton, struct in_addr */
#include "client_sim.h"
#include "../../../server/server_lifecycle.h" /* ServerPortmapInfo, NAT punch */
}

/*********************************************************
 *NAME:          guiServerDisplayAddress
 *PURPOSE:
 *  Computes the IPv4 address (and port) to show for the current server.
 *  Mirrors the display rules used elsewhere: a LAN host's loopback self-join
 *  is replaced with the LAN-routable IPv4, and an Internet host's private
 *  address is replaced with the router-side external mapping when one is known.
 *
 *  Returns false for single-player games or when no usable address exists
 *  (callers should then show their own placeholder); true otherwise, with
 *  dispIp / dispPort filled.
 *********************************************************/
static inline bool guiServerDisplayAddress(ClientSim *cs, char *dispIp,
                                           size_t ipLen, unsigned *dispPort) {
  if (cs == NULL || dispIp == NULL || ipLen == 0) {
    return false;
  }
  if (clientSimIsSinglePlayer(cs)) {
    return false;
  }

  struct in_addr srvAddr = clientSimGetServerAddress(cs);
  const char *addrStr = inet_ntoa(srvAddr);
  if (addrStr == NULL) {
    return false;
  }
  SDL_strlcpy(dispIp, addrStr, ipLen);
  if (dispPort != NULL) {
    *dispPort = clientSimGetServerPort(cs);
  }

  /* LAN host self-joins via loopback (127.0.0.1) — display the actual
   * LAN-routable IPv4 instead so it's useful to read off to a player on the
   * same network. Local helper: UDP socket + "connect" to a public address
   * (no packets sent, just a routing-table lookup) + getsockname. */
  if (clientSimIsLanOnly(cs) && strcmp(dispIp, "127.0.0.1") == 0) {
    bolo_socket_t sk = socket(AF_INET, SOCK_DGRAM, 0);
    if (sk != BOLO_INVALID_SOCKET) {
      struct sockaddr_in tgt;
      memset(&tgt, 0, sizeof(tgt));
      tgt.sin_family = AF_INET;
      tgt.sin_port = htons(53);
      inet_pton(AF_INET, "8.8.8.8", &tgt.sin_addr);
      if (connect(sk, (struct sockaddr *)&tgt, sizeof(tgt)) == 0) {
        struct sockaddr_in loc;
        memset(&loc, 0, sizeof(loc));
#ifdef _WIN32
        int slen = (int)sizeof(loc);
#else
        socklen_t slen = sizeof(loc);
#endif
        char lanIp[INET_ADDRSTRLEN];
        lanIp[0] = '\0';
        if (getsockname(sk, (struct sockaddr *)&loc, &slen) == 0 &&
            inet_ntop(AF_INET, &loc.sin_addr, lanIp, sizeof(lanIp)) != NULL &&
            lanIp[0] != '\0' && strcmp(lanIp, "127.0.0.1") != 0) {
          SDL_strlcpy(dispIp, lanIp, ipLen);
        }
      }
      closesocket(sk);
    }
  }

  /* When we are the host on an Internet game, the client-side server address
   * is loopback / private — replace it with the router-side external address
   * learned from libplum's UPnP/PCP mapping or the tracker's reflexive probe,
   * so the host sees the address remote players actually connect to. Skipped
   * for LAN-only and SP games (no external mapping runs there). */
  if (!clientSimIsLanOnly(cs) && serverInstanceIsNatPunchActive()) {
    ServerPortmapInfo pm;
    serverInstanceGetPortmapInfo(&pm);
    if (pm.externalIp[0] != '\0' && pm.externalPort != 0) {
      SDL_strlcpy(dispIp, pm.externalIp, ipLen);
      if (dispPort != NULL) {
        *dispPort = (unsigned)pm.externalPort;
      }
    }
  }

  if (dispIp[0] == '\0' || strcmp(dispIp, "0.0.0.0") == 0) {
    return false;
  }
  return true;
}

/*********************************************************
 *NAME:          guiServerAddressLink
 *PURPOSE:
 *  Renders the server address at the current cursor position as a clickable
 *  join-link. Shows the reverse-DNS hostname when one has resolved (falling
 *  back to the raw IP), and copies a winbolo://host:port URL to the clipboard
 *  on click. Kicks off the asynchronous reverse lookup the first time a given
 *  address is seen (the lookup runs on the existing DNS thread; the request is
 *  a no-op when that thread isn't running).
 *********************************************************/
static inline void guiServerAddressLink(ClientSim *cs, const char *dispIp,
                                        unsigned dispPort) {
  /* Kick off (debounced inside ClientSim) the async reverse lookup. */
  clientSimRequestServerHostname(cs, dispIp);

  /* Prefer the resolved hostname for both the visible token and the URL. */
  char host[256];
  const char *shown = dispIp;
  if (clientSimGetServerHostname(cs, dispIp, host, sizeof(host))) {
    shown = host;
  }

  char token[320];
  SDL_snprintf(token, sizeof(token), "%s:%u", shown, dispPort);
  char url[400];
  SDL_snprintf(url, sizeof(url), "winbolo://%s:%u", shown, dispPort);

  ImGui::TextUnformatted(token);
  if (ImGui::IsItemHovered()) {
    /* Underline on hover so it reads as a link. */
    ImVec2 mn = ImGui::GetItemRectMin();
    ImVec2 mx = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(mn.x, mx.y - 1.0f),
                                        ImVec2(mx.x, mx.y - 1.0f),
                                        ImGui::GetColorU32(ImGuiCol_Text));
    imguiHandOnHover();
    ImGui::SetTooltip("Click to copy join link");
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      SDL_SetClipboardText(url);
    }
  }
}

#endif /* IMGUI_SERVER_ADDRESS_H */
