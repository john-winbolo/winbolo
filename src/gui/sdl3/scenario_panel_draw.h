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

#include "wire_limits.h"    /* PACKET_MAX_CHAT_MESSAGE, the status line's cap */
#include "scenario_panel.h" /* the decoded list, and GAME_NUMTOTALTICKS_SEC
                             * by way of global.h */

#ifdef __cplusplus
extern "C" {
#endif

/* Text height, in panel units, for the three sizes a text, name or timer
 * primitive asks for. Large is double small. Units rather than pixels because the square scales
 * with the game's zoom and the writing in it has to scale with the square:
 * a script that fits a line across the panel at one zoom fits it at every
 * other. */
#define SCN_PANEL_TEXT_NORMAL_UNITS 11.0f
#define SCN_PANEL_TEXT_LARGE_UNITS  16.0f
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

/* The most bytes the status line's full text can hold: the script's line,
 * the gap before the countdown, and the countdown. */
#define SCN_STATUS_LINE_MAX (PACKET_MAX_CHAT_MESSAGE + 1 + 4 + \
                             SCN_PANEL_TIMER_TEXT_MAX)

/*********************************************************
 *NAME:          scnStatusLineText
 *PURPOSE:
 *  The status line as it is drawn: the script's text, and
 *  when it asked for a countdown, four spaces and the time
 *  left until that tick as a panel countdown shows it
 *  ("Wave 3/10    2:44"). The countdown stops at 0:00.
 *
 *  False, with out empty, when there is no line to draw.
 *
 *ARGUMENTS:
 *  text    - the script's line, or NULL
 *  endsAt  - the tick counted to, or SCN_STATUS_NO_COUNTDOWN
 *  now     - the tick the viewer is on
 *  out     - receives the text, always terminated
 *  outSize - bytes of out, including the terminator
 *********************************************************/
static inline bool scnStatusLineText(const char *text, uint32_t endsAt,
                                     uint32_t now, char *out,
                                     size_t outSize) {
    char timer[SCN_PANEL_TIMER_TEXT_MAX];

    if (out == NULL || outSize == 0) return false;
    out[0] = '\0';
    if (text == NULL || text[0] == '\0') return false;
    if (endsAt == SCN_STATUS_NO_COUNTDOWN) {
        snprintf(out, outSize, "%s", text);
        return true;
    }
    scnPanelTimerText(now, endsAt, (uint8_t)SCN_PANEL_TIMER_DOWN, timer,
                      sizeof(timer));
    snprintf(out, outSize, "%s    %s", text, timer);
    return true;
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

/* A rectangle in screen pixels, x0/y0 the top-left corner and x1/y1 the
 * bottom-right one. */
typedef struct ScnAnnounceRect {
    float x0, y0, x1, y1;
} ScnAnnounceRect;

/*********************************************************
 *NAME:          scnStatusPlace
 *PURPOSE:
 *  Where the status line's top-left corner goes: at the
 *  top of the game view, centred over it, and clear of the
 *  things already drawn up there (the scenario panel, the
 *  vote widgets, the HUD column of the full screen map).
 *
 *  The line stays on the top row when it can. If a thing
 *  is in the way it moves sideways, as little as it has
 *  to, into the free space beside it. If the line does not
 *  fit beside it, the line drops to just below it and the
 *  same search runs again on that row. The line never
 *  drops past floorY, which the caller puts just above the
 *  player's own tank in the middle of the view: a line
 *  over the tank is worse than a line over a panel. When
 *  no row above the floor fits, the line goes centred on
 *  the top row, over whatever is there.
 *
 *  A line wider than the view is centred over the view and
 *  runs out past both of its edges.
 *
 *ARGUMENTS:
 *  view      - the game view
 *  textW     - the line's width, outline included
 *  textH     - the line's height, outline included
 *  inset     - the space above the line on the top row
 *  gap       - the space kept between the line and a thing
 *  floorY    - the lowest the line's bottom edge may go
 *  obstacles - the things to keep clear of; may be NULL
 *  count     - how many there are
 *  outX/outY - receive the line's top-left corner
 *********************************************************/
static inline void scnStatusPlace(ScnAnnounceRect view, float textW,
                                  float textH, float inset, float gap,
                                  float floorY,
                                  const ScnAnnounceRect *obstacles,
                                  int count, float *outX, float *outY) {
    const float centreX = (view.x0 + view.x1) * 0.5f;
    const float topY    = view.y0 + inset;
    float spanX0 = view.x0;
    float spanX1 = view.x1;
    float lowestY;
    float y;
    int   pass;

    if (textW > spanX1 - spanX0) {
        spanX0 = centreX - textW * 0.5f;
        spanX1 = centreX + textW * 0.5f;
    }
    lowestY = view.y1 - textH;
    if (floorY - textH < lowestY) lowestY = floorY - textH;
    if (lowestY < topY) lowestY = topY;
    if (obstacles == NULL) count = 0;

    y = topY;
    /* Each pass either places the line or drops below one more thing, so
     * one pass more than there are things always finishes. */
    for (pass = 0; pass <= count && y <= lowestY; pass++) {
        float bestX    = 0.0f;
        float bestDist = -1.0f;
        float nextY    = 0.0f;
        bool  haveNext = false;
        int   c;
        int   i;

        /* The places the line could go on this row: centred, and just
         * either side of each thing. */
        for (c = -1; c < 2 * count; c++) {
            float x;
            float dist;
            bool  clear = true;

            if (c < 0) {
                x = centreX - textW * 0.5f;
            } else if ((c & 1) == 0) {
                x = obstacles[c / 2].x0 - gap - textW;
            } else {
                x = obstacles[c / 2].x1 + gap;
            }
            if (x < spanX0) x = spanX0;
            if (x > spanX1 - textW) x = spanX1 - textW;

            for (i = 0; i < count; i++) {
                const ScnAnnounceRect *o = &obstacles[i];
                if (x < o->x1 + gap && x + textW > o->x0 - gap &&
                    y < o->y1 + gap && y + textH > o->y0 - gap) {
                    clear = false;
                    break;
                }
            }
            if (!clear) continue;

            dist = (x + textW * 0.5f) - centreX;
            if (dist < 0.0f) dist = -dist;
            if (bestDist < 0.0f || dist < bestDist) {
                bestDist = dist;
                bestX    = x;
            }
        }

        if (bestDist >= 0.0f) {
            if (outX != NULL) *outX = bestX;
            if (outY != NULL) *outY = y;
            return;
        }

        /* Nothing fits on this row: drop to just below the highest bottom
         * edge of the things on it. */
        for (i = 0; i < count; i++) {
            const ScnAnnounceRect *o = &obstacles[i];
            if (y < o->y1 + gap && y + textH > o->y0 - gap) {
                const float below = o->y1 + gap;
                if (!haveNext || below < nextY) {
                    nextY    = below;
                    haveNext = true;
                }
            }
        }
        if (!haveNext || nextY <= y) break;
        y = nextY;
    }

    if (outX != NULL) *outX = centreX - textW * 0.5f;
    if (outY != NULL) *outY = topY;
}

/* How far down the game view an announcement with no position has the top
 * of its letters, as a share of the view's height. This is where every
 * announcement went before a script could say where: in the upper third,
 * clear of the player's own tank in the middle. */
#define SCN_ANNOUNCE_DOWN 0.28f

/*********************************************************
 *NAME:          scnAnnounceDefaultPlace
 *PURPOSE:
 *  Where an announcement with no position goes: centred
 *  across the view, with the top of its letters
 *  SCN_ANNOUNCE_DOWN of the way down. Nothing moves it. A
 *  line wider than the view runs out past both edges.
 *
 *ARGUMENTS:
 *  view      - the game view
 *  boxW      - the line's width, outline included
 *  outline   - how far the outline reaches past the letters
 *  outX/outY - receive the box's top-left corner; the
 *              letters go one outline in from it
 *********************************************************/
static inline void scnAnnounceDefaultPlace(ScnAnnounceRect view, float boxW,
                                           float outline, float *outX,
                                           float *outY) {
    if (outX != NULL) *outX = (view.x0 + view.x1) * 0.5f - boxW * 0.5f;
    if (outY != NULL) {
        *outY = view.y0 + (view.y1 - view.y0) * SCN_ANNOUNCE_DOWN - outline;
    }
}

/* A position byte as a share of the view, 0 to 1. A byte past
 * SCN_ANNOUNCE_POS_MAX reads as the far edge. */
static inline float scnAnnouncePosShare(uint8_t b) {
    if (b > (uint8_t)SCN_ANNOUNCE_POS_MAX) b = (uint8_t)SCN_ANNOUNCE_POS_MAX;
    return (float)b / (float)SCN_ANNOUNCE_POS_MAX;
}

/* One axis of scnAnnounceAt: the box's low edge with its centre at
 * lo + share * (hi - lo), kept inset inside lo..hi. A box too big for the
 * room is centred on it. */
static inline float scnAnnounceAxis(float lo, float hi, float share,
                                    float size, float inset) {
    const float minEdge = lo + inset;
    const float maxEdge = hi - inset - size;
    float       edge    = lo + share * (hi - lo) - size * 0.5f;

    if (maxEdge < minEdge) return (lo + hi) * 0.5f - size * 0.5f;
    if (edge < minEdge) edge = minEdge;
    if (edge > maxEdge) edge = maxEdge;
    return edge;
}

/*********************************************************
 *NAME:          scnAnnounceAt
 *PURPOSE:
 *  Where an announcement with a position goes: its centre
 *  on the point the script named, as shares of the view
 *  across and down, then moved the least it takes to keep
 *  the whole box inset inside the view, so no edge of the
 *  line is cut off. A line too wide for the view is
 *  centred across it.
 *
 *  The one thing it keeps clear of is the status line,
 *  which also sits at the top: a box that comes within one
 *  gap of the status line's band drops to one gap below it.
 *  The caller hands the band the full width of the view, so
 *  this happens wherever the box sits across it. It does
 *  not move for panels or windows, and it may sit on the
 *  player's tank; the script chose the spot.
 *
 *ARGUMENTS:
 *  view       - the game view
 *  boxW/boxH  - the line's size, outline included
 *  posX/posY  - the centre as position bytes
 *  inset      - the space kept from the view's edges
 *  statusBand - the status line's band, or NULL for none
 *  gap        - the space kept below the status line
 *  outX/outY  - receive the box's top-left corner
 *********************************************************/
static inline void scnAnnounceAt(ScnAnnounceRect view, float boxW,
                                 float boxH, uint8_t posX, uint8_t posY,
                                 float inset,
                                 const ScnAnnounceRect *statusBand,
                                 float gap, float *outX, float *outY) {
    const float x = scnAnnounceAxis(view.x0, view.x1,
                                    scnAnnouncePosShare(posX), boxW, inset);
    float       y = scnAnnounceAxis(view.y0, view.y1,
                                    scnAnnouncePosShare(posY), boxH, inset);

    if (statusBand != NULL && y < statusBand->y1 + gap &&
        y + boxH > statusBand->y0 - gap) {
        y = statusBand->y1 + gap;
    }
    if (outX != NULL) *outX = x;
    if (outY != NULL) *outY = y;
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
