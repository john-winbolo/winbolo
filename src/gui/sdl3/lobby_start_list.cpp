/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*********************************************************
 * Name:          lobby_start_list.cpp
 * Purpose:       Focusable list of map start slots — the
 *                non-spatial start picker. Each start is a
 *                Selectable row coloured by ownership;
 *                activating a free row claims that start for
 *                the local player via the same
 *                clientSimNetSendLobbyClaimStart path the
 *                map-marker picker uses.
 *********************************************************/

#include <SDL3/SDL.h>

#include "imgui.h"

extern "C" {
#include "global.h"
#include "../lang.h"
#include "client_sim.h"   /* ClientSim, ClientLobbySlot, clientSimGetLobbySlot */
#include "client_net.h"   /* clientSimNetSendLobbyClaimStart */
}
#include "lobby_start_markers.h"   /* lobbyStartHolderSlot/Classify/OwnerColor */
#include "lobby_start_list.h"

int lobbyStartListRender(ClientSim *cs, int myPlayerNum, int startCount) {
    if (!cs) return 0;
    if (startCount > MAX_STARTS) startCount = MAX_STARTS;
    if (startCount < 0) startCount = 0;

    int focused = 0;
    for (int i = 1; i <= startCount; i++) {
        int holder = lobbyStartHolderSlot(cs, i);
        bool free   = (holder < 0);

        /* Occupant: the holder's name when claimed, "(open)" when free.
         * Names are runtime data, drawn directly. The leading "#N" is the
         * start number (runtime), not a fixed caption — no English to
         * localise here. */
        const char *who = free
            ? langGetText(STR_DLGLOBBY_START_OPEN)
            : clientSimGetLobbySlot(cs, (BYTE)holder)->playerName;
        char label[96];
        SDL_snprintf(label, sizeof(label), "#%d  %s##start%d", i, who, i);

        /* Colour the row by ownership relative to the local player. */
        LobbyStartOwner o = lobbyStartClassify(cs, holder, myPlayerNum);
        ImGui::PushStyleColor(ImGuiCol_Text, lobbyStartOwnerColor(o));
        /* Every row stays a plain focusable Selectable so the pad can
         * D-pad through (and the map highlight the focused slot) regardless
         * of occupancy. Only a free start the local player can take has an
         * effect on activate — self-claim / self-move only; activating an
         * occupied row does nothing, mirroring the mouse marker picker. */
        bool activated  = ImGui::Selectable(label);
        bool actionable = free && myPlayerNum >= 0;
        ImGui::PopStyleColor();

        if (ImGui::IsItemFocused()) focused = i;
        if (activated && actionable) {
            clientSimNetSendLobbyClaimStart(cs, (BYTE)myPlayerNum, (BYTE)i);
        }
    }
    return focused;
}
