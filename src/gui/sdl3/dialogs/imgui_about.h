/*
 * About modal — version / copyright / links / docs popups.
 *
 * Renders in whatever ImGui context is current when the entry points are
 * called, so both the welcome screen (its own ImGui context) and the
 * in-game UI (sdl3imgui's ImGui context) can host the popup.
 *
 * - aboutPopupOpen(): mark the popup to open on the next aboutPopupRender call.
 * - aboutPopupRender(): submit the popup (and its child MD popups) for this frame.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void aboutPopupOpen(void);
void aboutPopupRender(void);

/* Force any open About / MD popups to close on their next render — used by
 * the in-game tablet tap-outside dismiss path. */
void aboutPopupCloseAll(void);

#ifdef __cplusplus
}
#endif
