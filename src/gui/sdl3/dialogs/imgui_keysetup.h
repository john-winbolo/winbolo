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
 * Name:          imgui_keysetup.h
 * Purpose:       Standalone key setup dialog shown from
 *                the pre-game settings screen.
 *********************************************************/

#ifndef IMGUI_KEYSETUP_H
#define IMGUI_KEYSETUP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Forward decl — full type lives in src/bolo/public/client_sim.h.
 * Kept opaque here so the in-game popup wrapper doesn't pull the
 * full client_sim closure into every menu / event-pump caller. */
struct ClientSim;

/* Show the key setup dialog as a blocking modal loop (own ImGui
 * context, own SDL event loop). Used by the pre-game Settings →
 * Set Keys path where no main-game ImGui context is up. Returns 1
 * if keys were saved, 0 if cancelled. */
int  imguiKeySetupShow(void);

/* In-game popup variant — sits inside the running game's ImGui
 * context as a BeginPopupModal. Trigger with imguiKeySetupOpenInGame
 * (sets a pending flag, seeded from the current tank on the next
 * render call). Each render frame calls imguiKeySetupRenderInGamePopup
 * with the live ClientSim so the OK handler can push the new auto-
 * slowdown / auto-gunsight values onto the active tank directly.
 *
 * The two wrappers share the form body — sections, key rows,
 * checkboxes — by both calling into the static renderForm helper.
 * Form state (keys, checkbox values, in-progress key capture) is
 * shared file-static between the wrappers; only one keys dialog is
 * ever open at a time. */
void imguiKeySetupOpenInGame(void);
void imguiKeySetupRenderInGamePopup(struct ClientSim *cs);

/* Event-pump hooks — called from sdl3ImguiProcessEvents so the
 * raw SDL_EVENT_KEY_DOWN scancode gets routed into the dialog's
 * key-capture state instead of the game's input layer. Mirror of
 * the same intercept the standalone dialog does inside its own
 * event loop. */
bool imguiKeySetupIsCapturingInGameKey(void);
void imguiKeySetupHandleInGameScancode(int scancode);

/* Same idea for the Controller tab: while a controller row is armed,
 * the event pump routes a gamepad button-down (SDL_GamepadButton) or a
 * trigger (SDL_GamepadAxis) here to bind it. Escape cancels via the
 * scancode path above. */
bool imguiKeySetupIsCapturingInGamePad(void);
void imguiKeySetupHandleInGamePadButton(int sdlGamepadButton);
void imguiKeySetupHandleInGamePadTrigger(int sdlGamepadAxis);
void imguiKeySetupCancelInGamePad(void);

/* Embedded variant — draws the key-binding rows and the two
 * checkboxes inside a caller-owned ImGui context (the first-run
 * onboarding wizard), without this dialog's own OK/Cancel footer.
 * Seed the shared form state once with imguiKeySetupBeginEmbedded,
 * draw the rows each frame with imguiKeySetupRenderEmbedded, and
 * persist the result with imguiKeySetupCommitEmbedded when leaving
 * the step forward. Key capture reuses the in-game hooks above.
 * Pre-game only — there is no live tank to push values onto.
 *
 * reserveBottom is extra vertical space (pixels) to keep clear below
 * the scrollable binding list, on top of the checkbox footer, so the
 * caller's own controls (the wizard's Back/Skip/Next row) fit without
 * forcing a scrollbar on the host window. */
void imguiKeySetupBeginEmbedded(void);
void imguiKeySetupRenderEmbedded(float reserveBottom);
void imguiKeySetupCommitEmbedded(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_KEYSETUP_H */
