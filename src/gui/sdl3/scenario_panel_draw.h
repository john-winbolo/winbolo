/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Scenario Panel Drawer
 *Filename:      scenario_panel_draw.h
 *Purpose:
 *  Turns one decoded display list into pixels with ImGui's
 *  2D calls. A scenario sends the list as data and every
 *  frontend interprets it with its own drawing: the list is
 *  the contract, and this is the ImGui frontends' half of
 *  it.
 *
 *  What it depends on is what the desktop client, the log
 *  viewer and the wasm target all already compile: ImGui,
 *  public/scenario_panel.h and the tile-sheet lookup in
 *  sprite_positions.c. No ClientSim, no sim headers and no
 *  lang table — a text primitive carries the script's own
 *  bytes and is not localised, and a name primitive is
 *  resolved through the callback below rather than by
 *  reading a roster this file cannot see.
 *
 *  Plain C, and free of ImGui, so the timer's arithmetic
 *  below can be held to a table of strings by a test that
 *  links no renderer. The drawing body is C++ because
 *  ImGui is, but nothing in this header is.
 *********************************************************/
#ifndef SCENARIO_PANEL_DRAW_H
#define SCENARIO_PANEL_DRAW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "scenario_panel.h" /* the decoded list, and GAME_NUMTOTALTICKS_SEC
                             * by way of global.h */

#ifdef __cplusplus
extern "C" {
#endif

/* Text height, in panel units, for the two sizes a text, name or timer
 * primitive asks for. Units rather than pixels because the square scales
 * with the game's zoom and the writing in it has to scale with the square:
 * a script that fits a line across the panel at one zoom fits it at every
 * other. */
#define SCN_PANEL_TEXT_NORMAL_UNITS 11.0f
#define SCN_PANEL_TEXT_SMALL_UNITS   8.0f

/* Longest string a timer renders, "M:SS" with the minutes unbounded, plus
 * its terminator. A round long enough to overrun this is longer than the
 * tick counter itself. */
#define SCN_PANEL_TIMER_TEXT_MAX 24

/*********************************************************
 *NAME:          scnPanelTimerText
 *PURPOSE:
 *  The minutes and seconds between two game ticks, as the
 *  timer primitive shows them.
 *
 *  A countdown reads the time from now until its tick, a
 *  count-up the time from its tick until now, and either
 *  reads 0:00 rather than going negative once it is on the
 *  wrong side of its target — a wave whose time has run out
 *  sits at zero until the script says something else.
 *
 *  Ticks run at GAME_NUMTOTALTICKS_SEC, a hundred a second,
 *  which is the clock the script counted its tick on.
 *  Minutes are not wrapped at sixty: an hour and a half
 *  reads 90:00, because a panel that showed 30:00 for it
 *  would be lying about the hour.
 *
 *  Arithmetic over its arguments and nothing else, which is
 *  what lets a unit test hold it to exact strings with no
 *  window, no renderer and no ImGui behind it.
 *
 *ARGUMENTS:
 *  now     - the tick the viewer is on
 *  target  - the tick the primitive carries
 *  mode    - a ScnPanelTimerMode
 *  out     - receives the text, always terminated
 *  outSize - bytes of out, including the terminator
 *********************************************************/
static inline void scnPanelTimerText(uint32_t now, uint32_t target,
                                     uint8_t mode, char *out, size_t outSize) {
    uint32_t     ticks;
    uint32_t     seconds;
    unsigned int mins, secs;

    if (out == NULL || outSize == 0) return;

    if (mode == (uint8_t)SCN_PANEL_TIMER_UP) {
        ticks = (now > target) ? (now - target) : 0u;
    } else {
        ticks = (target > now) ? (target - now) : 0u;
    }

    seconds = ticks / (uint32_t)GAME_NUMTOTALTICKS_SEC;
    mins    = (unsigned int)(seconds / 60u);
    secs    = (unsigned int)(seconds % 60u);
    snprintf(out, outSize, "%u:%02u", mins, secs);
}

/*********************************************************
 *NAME:          scnAnnounceRemaining
 *PURPOSE:
 *  Whether a scenario's announcement is still on screen,
 *  and how many ticks it has left.
 *
 *  An announcement carries the tick it landed at and how
 *  long it was asked to stay up, both on the tick the
 *  server counts in, and every viewer works the rest out
 *  against its own clock — the same bargain the timer
 *  primitive makes. Nothing to show is an empty text, a
 *  duration of nothing, or a clock that has reached the end
 *  of one; all three answer false and leave the remainder
 *  zero.
 *
 *  It sits beside the timer's text above because the two
 *  are the same kind of thing: the arithmetic under a
 *  scenario's presentation, with no renderer in it, which
 *  is what lets a test hold either to a table of answers.
 *
 *ARGUMENTS:
 *  text        - the announcement, or NULL
 *  arrivedTick - the tick it landed at
 *  ticks       - how long it was asked to stay up
 *  nowTick     - the tick the viewer is on
 *  outLeft     - receives the ticks left; may be NULL
 *********************************************************/
static inline bool scnAnnounceRemaining(const char *text, uint32_t arrivedTick,
                                        uint16_t ticks, uint32_t nowTick,
                                        uint32_t *outLeft) {
    uint32_t elapsed;

    if (outLeft != NULL) *outLeft = 0;
    if (text == NULL || text[0] == '\0' || ticks == 0) return false;

    /* A clock behind the arrival has not reached it yet, which is the whole
     * duration still to run rather than a negative age. */
    elapsed = (nowTick > arrivedTick) ? (nowTick - arrivedTick) : 0u;
    if (elapsed >= (uint32_t)ticks) return false;

    if (outLeft != NULL) *outLeft = (uint32_t)ticks - elapsed;
    return true;
}

/*********************************************************
 *NAME:          scnPanelColourRGBA
 *PURPOSE:
 *  The palette entry an index names, as RGBA bytes.
 *
 *  False for an index that draws nothing — 0, one of the
 *  four reserved entries, or one past the palette — and the
 *  outputs are left alone. Any output pointer may be NULL.
 *
 *  The table is in scenario_panel_draw.cpp and this is the
 *  way out of it, so the panel drawn through ImGui and the
 *  map marker drawn through the SDL renderer take their
 *  sixteen colours from one place rather than from two
 *  copies that would drift the first time a skin changed
 *  one.
 *
 *ARGUMENTS:
 *  index   - a ScnPanelColour
 *  r/g/b/a - receive the colour; each may be NULL
 *********************************************************/
bool scnPanelColourRGBA(uint8_t index, uint8_t *r, uint8_t *g, uint8_t *b,
                        uint8_t *a);

/* What the drawer needs from the frontend around it.
 *
 * playerName answers the name a slot is playing under, or NULL for a slot
 * nobody holds; the whole callback may be NULL where a frontend has no
 * roster to ask. Either way a name primitive draws nothing, which is what
 * keeps a script from sending names itself and what makes a rename show
 * through without an update.
 *
 * tick is the game tick the viewer is on, for the timer primitive. On the
 * client that is the last tick heard from the server, which is the clock the
 * scenario counted in.
 *
 * scale is screen pixels per panel unit: one at the game's zoom 1, and the
 * zoom from there.
 *
 * alpha is how solid the frontend wants the scenario's drawing, from 0 for
 * gone to 1 for as the script asked. It multiplies the alpha channel of
 * every colour this file resolves, so it fades what was drawn and never
 * touches a pixel nothing was drawn on: a square the script left empty is
 * as transparent at 0.2 as it is at 1. It is the scenario's drawing only.
 * A frontend's own chrome around the square — a border, a grip, a settings
 * window — is not on this list and does not fade with it, or a panel taken
 * to nothing would leave the player nothing to take it back with.
 *
 * There is no "unset": a zeroed env is a panel that draws nothing at all,
 * so a frontend that builds one of these must fill this in. 1.0f is the
 * value that means "as it was before this existed".
 *
 * tiles is the skin's tile sheet as the frontend's ImGui backend takes it —
 * the SDL_Texture the desktop client and the log viewer each keep their own
 * accessor for, which is why it arrives here rather than being fetched. NULL
 * draws no sprite. The sheet's own pixel size is not needed: the atlas
 * lookup is in the classic 1x layout and the sheet is that layout scaled
 * whole, so the scale cancels out of the texture coordinates.
 */
typedef struct {
    const char *(*playerName)(void *ctx, uint8_t slot); /* may be NULL */
    void       *ctx;
    uint32_t    tick;
    float       scale;
    float       alpha;
    void       *tiles;
} ScnPanelDrawEnv;

/*********************************************************
 *NAME:          scnPanelDraw
 *PURPOSE:
 *  Draws one decoded list into the current ImGui window's
 *  draw list, with the square's top-left at (originX,
 *  originY) in screen pixels.
 *
 *  Every primitive is clipped to the square. The parser
 *  lets a primitive start inside the square and run past
 *  its edge on purpose, so the clip is what keeps a rect
 *  the script placed badly from painting over the game.
 *
 *  A NULL list, an empty one, or a NULL env draws nothing.
 *
 *ARGUMENTS:
 *  list            - the decoded list to draw
 *  originX/originY - the square's top-left, in screen pixels
 *  env             - what the frontend around it supplies
 *********************************************************/
void scnPanelDraw(const ScnPanelList *list, float originX, float originY,
                  const ScnPanelDrawEnv *env);

#ifdef __cplusplus
}
#endif

#endif /* SCENARIO_PANEL_DRAW_H */
