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

/* Show one message: a scenario's popup (game.popup), drawn as a modal
 * popup through the main ImGui context. Tokens such as {ACCEL} or
 * {QUICK_TREE} are expanded (tutorial_text.h). Ignored if the overlay is
 * already showing, so the caller holds the text back until
 * tutorialOverlayIsOpen() is false. Copies `text`. */
void tutorialOverlayShowText(const char *text);

/* True while the overlay is open or pending-open this frame.  Fed into
 * the input gate and the solo-pause edge-detect. */
bool tutorialOverlayIsOpen(void);

/* Render one frame of the overlay (no-op when closed).  Call every
 * frame from the main render path, on the main thread. */
void tutorialOverlayRender(struct ClientSim *cs);

/* Drop any message left showing or queued, so the next game starts with the
 * overlay closed. */
void tutorialOverlayReset(void);

/* Close whatever the overlay is showing, because the round it belongs to
 * has ended. tutorialOverlayIsOpen()
 * is false from this call on; the ImGui popup itself is closed by the next
 * tutorialOverlayRender, before it would draw. The round-end screen and the
 * lobby take the window over and never draw this popup, so one left open
 * would sit frozen under them, unreadable and impossible to dismiss, and keep
 * a solo game paused. Closing it is the choice made here rather than keeping
 * it on top: what it said belongs to the round that has ended. */
void tutorialOverlayClose(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_TUTORIAL_OVERLAY_H */
