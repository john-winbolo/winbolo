/*
 * Copyright (c) 1998-2008 John Morrison.
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
 * Name:          map_preview_popup.h
 * Purpose:       Reusable zoomable/pannable map preview
 *                popup for ImGui dialogs.
 *********************************************************/

#ifndef MAP_PREVIEW_POPUP_H
#define MAP_PREVIEW_POPUP_H

#include <SDL3/SDL.h>
#include "global.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Call after an ImGui::Image() thumbnail. If the image was clicked and
 * compressedData is non-NULL, opens the popup centered on the given bounds.
 * compressedData is the network-compressed map (bases+pills+starts+LZW). */
void mapPreviewPopupOnClick(const BYTE *compressedData, int compressedLen,
                            int boundsMinX, int boundsMinY,
                            int boundsMaxX, int boundsMaxY);

/* Like mapPreviewPopupOnClick but loads map data from a .map file on disk
 * instead of from compressed network data. */
void mapPreviewPopupOnClickFile(const char *mapPath,
                                int boundsMinX, int boundsMinY,
                                int boundsMaxX, int boundsMaxY);

/* Explicit open functions — call these when the caller handles click
 * detection itself (e.g. via ImGui::IsItemClicked()). */
void mapPreviewPopupOpenCompressed(const BYTE *compressedData, int compressedLen,
                                   int boundsMinX, int boundsMinY,
                                   int boundsMaxX, int boundsMaxY);
void mapPreviewPopupOpenFile(const char *mapPath,
                             int boundsMinX, int boundsMinY,
                             int boundsMaxX, int boundsMaxY);

/* Call once per frame BEFORE ImGui NewFrame — renders tiles to offscreen texture. */
void mapPreviewPopupRenderOffscreen(SDL_Renderer *renderer, int winW, int winH);

/* Call once per frame during ImGui rendering — renders the modal popup. */
void mapPreviewPopupRenderModal(SDL_Renderer *renderer);

/* Close the popup if open (e.g. when the underlying map changes).
 * Frees map data but keeps the tile atlas for fast re-open. */
void mapPreviewPopupClose(void);

/* True iff the popup window is currently open. Used by callers
 * (e.g. the lobby's MAP_CHANGE handler) to decide whether to
 * refresh the popup in place vs. just drop the old data. */
bool mapPreviewPopupIsOpen(void);

/* If the popup is open, swap its underlying compressed-map bytes
 * for the freshly-arrived ones — keeping the user's current zoom
 * and pan so the window seamlessly updates in place when the map
 * changes server-side. No-op if the popup is closed. */
void mapPreviewPopupRefreshOpen(const BYTE *compressedData, int compressedLen);

/* Returns true once after the user clicks "Change" inside the popup
 * (which also closes the popup). The lobby polls this each frame
 * and, on a true return, opens its Choose Map dialog. The flag is
 * latched, so a missed poll is fine — the next call still sees it. */
bool mapPreviewPopupConsumeChangeRequest(void);

/* When false, the "Change" button is hidden from the popup
 * footer — for lobby viewers without map-edit authority. Default
 * is true (legacy behaviour). Set per-frame from the lobby. */
void mapPreviewPopupSetShowChange(bool show);

struct ClientSim;
/* Per-frame lobby context for start labels / click-to-claim. cs == NULL
 * disables the overlay. effectiveHost gates nothing here (labels are
 * read-only and claims are self-only this phase) but is stored for the
 * later host arm-and-click enhancement. The context is consumed and
 * cleared by each mapPreviewPopupRenderModal, so a frame that doesn't
 * set it leaves the overlay off — keeping a stale ClientSim pointer
 * from leaking into non-lobby callers of the popup. */
void mapPreviewPopupSetStartPicker(struct ClientSim *cs, int myPlayerNum,
                                   bool effectiveHost);

/* Call on dialog exit to free all resources. */
void mapPreviewPopupDestroy(void);

#ifdef __cplusplus
}
#endif

#endif /* MAP_PREVIEW_POPUP_H */
