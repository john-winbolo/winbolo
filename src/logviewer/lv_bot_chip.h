/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Log Viewer bot chip rule
 *Filename:      lv_bot_chip.h
 *Purpose:
 *  Which of the game's two chips the Log Viewer's panels
 *  draw in front of the name of a computer player
 *  (imgui_lv_icons.cpp draws it). The game draws the green
 *  chip for a bot on the player's side and the red chip for
 *  any other bot. The viewer has a player only while the
 *  view of one player is on (the perspective).
 *
 *  Header only, with no includes but the C library's, so the
 *  viewer and the unit tests use the same rule.
 *********************************************************/

#ifndef LV_BOT_CHIP_H
#define LV_BOT_CHIP_H

#include <stdbool.h>

typedef enum LvBotChip {
    LV_BOT_CHIP_NONE = 0,  /* not a bot: no chip, and no room kept for one */
    LV_BOT_CHIP_RED,       /* a bot that is not on the viewed player's side */
    LV_BOT_CHIP_GREEN      /* the viewed player, when a bot, or a bot allied to it */
} LvBotChip;

/* perspective is true while the view of one player is on; own is true for
 * that player, allied for a player allied to it. With no perspective every
 * bot has the red chip: it is the chip the game draws when it does not know
 * the sides. */
static inline LvBotChip lvBotChipFor(bool isBot, bool perspective, bool own, bool allied) {
    if (!isBot) return LV_BOT_CHIP_NONE;
    if (perspective && (own || allied)) return LV_BOT_CHIP_GREEN;
    return LV_BOT_CHIP_RED;
}

#endif /* LV_BOT_CHIP_H */
