/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          imgui_tutorial_overlay.h
 * Purpose:       In-loop tutorial message overlay.  Replaces
 *                the old blocking imguiMessageBoxRich dialog
 *                sequence with a modal popup drawn through the
 *                main ImGui context — exactly like the Deck
 *                pause menu.  This lets the game's input gate
 *                suspend play and the solo-pause path freeze the
 *                sim while a tutorial message is up, so a held
 *                turn or a dismissing fire press no longer leaks
 *                into the game when the message closes.
 *********************************************************/

#ifndef IMGUI_TUTORIAL_OVERLAY_H
#define IMGUI_TUTORIAL_OVERLAY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ClientSim;

/* Queue a sequence of tutorial message string-ids (STR_TUTORIAL*) for
 * display.  Drawn as a modal popup through the main ImGui context.
 * Ignored if the overlay is already showing.  `onComplete` (may be NULL)
 * runs once, on the main thread, when the player dismisses the last
 * message in the sequence.  Copies `ids`; `count` is clamped. */
void tutorialOverlayShow(const uint16_t *ids, int count,
                         void (*onComplete)(void));

/* True while the overlay is open or pending-open this frame.  Fed into
 * the input gate and the solo-pause edge-detect. */
bool tutorialOverlayIsOpen(void);

/* Render one frame of the overlay (no-op when closed).  Call every
 * frame from the main render path, on the main thread. */
void tutorialOverlayRender(struct ClientSim *cs);

/* Drop any message left showing or queued, and its completion callback, so
 * the next tutorial starts with the overlay closed. */
void tutorialOverlayReset(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_TUTORIAL_OVERLAY_H */
