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

/*********************************************************
 * Name:          lv_host.h
 * Purpose:
 *   Internal seam between the two halves of the log viewer
 *   host: logviewer.c (modal viewer and spectator) and
 *   lv_embed.c (shared bring-up plus the embedded reel).
 *   Not for inclusion outside src/logviewer/.
 *********************************************************/

#ifndef LV_HOST_H
#define LV_HOST_H

#include <stdbool.h>
#include "logviewer.h"   /* LogViewerState */

struct SDL_Window;
struct SDL_Renderer;

/* The current viewer/embed/spectator session state. Defined in lv_embed.c —
 * it travels with the half of the host that every build compiles. NULL when
 * no session is live. */
extern LogViewerState *g_lv;

/* ImGui-free half of the host bring-up / shutdown. Shared by the standalone
 * viewer, the spectator host and the embed. */
bool lvHostSetupCore(struct SDL_Window *window, struct SDL_Renderer *renderer,
                     bool fromMainMenu);
void lvHostTeardownCommon(bool withImGui);

/* Host entry points the embed drives. Callers outside this pair of TUs
 * hand-declare these as extern (draw.c, imgui_controls.cpp,
 * imgui_main_menu.cpp); keep the signatures identical to those. */
void lv_updateSpeed(BYTE spd, int updateSlider);
void lv_windowNeedRedraw(void);
void lv_windowPlay(void);
void lv_windowPause(void);
/* true: lv_windowPlay starts no decoder timer, and the host steps the
 * decoder itself (the web viewer, from its main loop). */
void lv_windowSetHostRunsTicks(bool on);

#endif /* LV_HOST_H */
