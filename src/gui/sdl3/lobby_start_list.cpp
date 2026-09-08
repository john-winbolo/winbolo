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
#include "lobby_start_markers.h"   /* lobbyStartHolderSlot/Classify/OwnerColor, side rules */
#include "lobby_start_list.h"

int lobbyStartListRender(ClientSim *cs, int myPlayerNum, int startCount,
                         const BYTE *sideMasks) {
    if (!cs) return 0;
    if (startCount > MAX_STARTS) startCount = MAX_STARTS;
    if (startCount < 0) startCount = 0;

    /* The local player's side rules, looked up once: a free start they keep
     * the player off is shown but cannot be taken, the same as the mouse
     * picker's blocked click. A spectator has no team and no rule. */
    int  myTeam     = lobbySlotTeam(cs, myPlayerNum);
    BYTE mySide     = lobbyTeamSide(cs, myTeam);
    BYTE closedMask = lobbyClosedMaskForTeam(cs, myTeam);

    int focused = 0;
    for (int i = 1; i <= startCount; i++) {
        int  holder  = lobbyStartHolderSlot(cs, i);
        bool free    = (holder < 0);
        bool offSide = sideMasks != NULL &&
                       lobbyStartOffSideMasked(sideMasks[i], mySide, closedMask);

        /* Occupant: the holder's name when claimed, "(open)" when free.
         * Names are runtime data, drawn directly. The leading "#N" is the
         * start number (runtime), not a fixed caption — no English to
         * localise here. An off-side start carries the same suffix the
         * player list's dropdown puts on one. */
        const char *who;
        if (free) {
            who = langGetText(STR_DLGLOBBY_START_OPEN);
        } else {
            const ClientLobbySlot *slot = clientSimGetLobbySlot(cs, (BYTE)holder);
            who = slot ? slot->playerName : langGetText(STR_DLGLOBBY_START_OPEN);
        }
        char label[128];
        if (offSide) {
            SDL_snprintf(label, sizeof(label), "#%d  %s %s##start%d", i, who,
                         langGetText(STR_DLGLOBBY_START_OFFSIDE_SUFFIX), i);
        } else {
            SDL_snprintf(label, sizeof(label), "#%d  %s##start%d", i, who, i);
        }

        /* Colour the row by ownership relative to the local player; an
         * off-side row at half alpha, the way both map previews dim one. */
        LobbyStartOwner o = lobbyStartClassify(cs, holder, myPlayerNum);
        ImU32 rowColor = lobbyStartOwnerColor(o);
        if (offSide) rowColor = lobbyStartDimColor(rowColor);
        ImGui::PushStyleColor(ImGuiCol_Text, rowColor);
        /* Every row stays a plain focusable Selectable so the pad can
         * D-pad through (and the map highlight the focused slot) regardless
         * of occupancy. Only a free start the local player can take has an
         * effect on activate — self-claim / self-move only; activating an
         * occupied or off-side row does nothing, mirroring the mouse marker
         * picker. */
        bool activated  = ImGui::Selectable(label);
        bool actionable = free && myPlayerNum >= 0 && !offSide;
        ImGui::PopStyleColor();

        if (ImGui::IsItemFocused()) focused = i;
        /* Say why a focused off-side row does nothing — the pad has no hover,
         * so focus stands in for it. */
        if (offSide && (ImGui::IsItemFocused() || ImGui::IsItemHovered())) {
            MessageArgs args = {};
            args.number = i;
            ImGui::SetTooltip("%s", langGetTextFmt(STR_STARTPICK_TIP_OFFSIDE, &args));
        }
        if (activated && actionable) {
            clientSimNetSendLobbyClaimStart(cs, (BYTE)myPlayerNum, (BYTE)i);
        }
    }
    return focused;
}
