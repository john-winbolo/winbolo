/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          imgui_quickchat.h
 * Purpose:       Gamepad quick-chat preset menu.  Opened
 *                in-game via D-pad UP; sends short canned
 *                messages to all players, or hands off to
 *                the free-form Send Message dialog for
 *                free-text input via the Steam OSK.
 *********************************************************/

#ifndef IMGUI_QUICKCHAT_H
#define IMGUI_QUICKCHAT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ClientSim;

void quickChatOpen(void);                    /* call when D-pad UP pressed in-game */
void quickChatRender(struct ClientSim *cs);  /* call from render path */
bool quickChatIsOpen(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_QUICKCHAT_H */
