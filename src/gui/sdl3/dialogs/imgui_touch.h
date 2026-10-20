/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          imgui_touch.h
 * Purpose:       Touch handling every ImGui context needs
 *                on iOS and the ImGui SDL3 backend does not
 *                give: the on-screen keyboard without
 *                auto-capitalisation or autocorrect (and as
 *                a password keyboard for the join password),
 *                and dragging a finger to scroll a list.
 *
 *                imguiTouchFrame() runs once per frame from
 *                dialogDrawNavOutline(), which every dialog
 *                frame loop and the in-game ImGui frame call
 *                just before ImGui::Render(), so no frame
 *                loop has to call it itself. Built for iOS
 *                only; elsewhere the calls below do nothing.
 *********************************************************/

#ifndef IMGUI_TOUCH_H
#define IMGUI_TOUCH_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__IPHONEOS__) || defined(__ANDROID__)

/* Once per frame, after the widgets and before ImGui::Render(). Puts the
 * keyboard handler on the current context if it is not there yet, and
 * scrolls the window under a finger that is dragging. */
void imguiTouchFrame(void);

/* True while the password prompt is up, so the keyboard it opens is a
 * password keyboard: hidden text, no suggestions. */
void imguiTouchSetPasswordInput(bool password);

#else

static inline void imguiTouchSetPasswordInput(bool password) { (void)password; }

#endif

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_TOUCH_H */
