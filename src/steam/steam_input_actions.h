/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * steam_input_actions.h — string constants for Steam Input actions.
 *
 * These names MUST exactly match the actions and action-sets declared
 * in controller_config/game_actions_4672140.vdf.  Steam Input does
 * string-based lookup; mismatches silently yield no input.
 */
#ifndef STEAM_INPUT_ACTIONS_H
#define STEAM_INPUT_ACTIONS_H

/* Action sets */
#define SI_SET_IN_GAME    "InGame"
#define SI_SET_MENU       "Menu"

/* Digital actions — InGame set */
#define SI_ACTION_FIRE          "fire"
#define SI_ACTION_MINE          "mine"
#define SI_ACTION_BUILD_CONFIRM "build_confirm"   /* X — send LGM to gunsight */
#define SI_ACTION_VIEW_CYCLE    "view_cycle"      /* Y — tank/pillbox/ally/LGM/base */
#define SI_ACTION_GUNSIGHT_DEC  "gunsight_dec"    /* LB — range -1 */
#define SI_ACTION_GUNSIGHT_INC  "gunsight_inc"    /* RB — range +1 */
#define SI_ACTION_BUILD_PREV    "build_prev"      /* D-pad UP — cycle build type backward */
#define SI_ACTION_BUILD_NEXT    "build_next"      /* D-pad DOWN — cycle forward */
#define SI_ACTION_BUILD_CURSOR_TOGGLE "build_cursor_toggle" /* R3 — enter/exit free build cursor */
#define SI_ACTION_QUICK_CHAT    "quick_chat"      /* D-pad LEFT */
#define SI_ACTION_PAUSE         "pause"           /* Start */
#define SI_ACTION_VIEW_PLAYERS  "view_players"    /* D-pad RIGHT — open Players panel */
#define SI_ACTION_BUILD_CANCEL  "build_cancel"    /* exit build mode without building */
#define SI_ACTION_LOCK_HEADING  "lock_heading"    /* hold — freeze tank facing */
#define SI_ACTION_TANK_VIEW     "tank_view"       /* recentre on / return to tank view */
/* Smart ping. The menu one is held — the pie opens under it and the right
 * stick (map_scroll) picks a sector — and the six below send one kind
 * outright. All unbound in the shipped controller configs; the player binds
 * what they want in the Steam configurator. */
#define SI_ACTION_PING_MENU        "ping_menu"
#define SI_ACTION_PING_STANDARD    "ping_standard"
#define SI_ACTION_PING_CAUTION     "ping_caution"
#define SI_ACTION_PING_ASSIST      "ping_assist"
#define SI_ACTION_PING_ATTACK      "ping_attack"
#define SI_ACTION_PING_ON_MY_WAY   "ping_on_my_way"
#define SI_ACTION_PING_BOT_COMMAND "ping_bot_command"

/* Analog actions — InGame set */
#define SI_ANALOG_TANK_MOVE     "tank_move"       /* Left stick */
#define SI_ANALOG_MAP_SCROLL    "map_scroll"      /* Right stick */

/* Digital actions — Menu set (used by ImGui dialogs and the pause overlay) */
#define SI_ACTION_MENU_ACCEPT   "menu_accept"     /* A */
#define SI_ACTION_MENU_CANCEL   "menu_cancel"     /* B */
#define SI_ACTION_MENU_NAV_UP   "menu_nav_up"
#define SI_ACTION_MENU_NAV_DOWN "menu_nav_down"
#define SI_ACTION_MENU_NAV_LEFT "menu_nav_left"
#define SI_ACTION_MENU_NAV_RIGHT "menu_nav_right"
#define SI_ACTION_MENU_TAB_LEFT  "menu_tab_left"   /* LT — previous tab */
#define SI_ACTION_MENU_TAB_RIGHT "menu_tab_right"  /* RT — next tab */

#endif /* STEAM_INPUT_ACTIONS_H */
