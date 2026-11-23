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

/* The colour of a ping in the lobby: the player list and the Bot Host rows
 * use the same rule (pingColorRole, ping_display.h). */

#ifndef LOBBY_PING_COLOR_H
#define LOBBY_PING_COLOR_H

#include <stdint.h>
#include "imgui.h"
#include "ping_display.h"     /* pingColorRole */
#include "../../wb_theme.h"   /* g_theme / wbThemeColor */

static inline ImVec4 lobbyPingColor(uint16_t pingMs) {
    switch (pingColorRole(pingMs)) {
        case PING_COLOR_ONLINE:    return wbThemeColor(g_theme->statusOnline);
        case PING_COLOR_HIGH_PING: return wbThemeColor(g_theme->statusHighPing);
        default:                   return wbThemeColor(g_theme->statusDisconnected);
    }
}

#endif /* LOBBY_PING_COLOR_H */
