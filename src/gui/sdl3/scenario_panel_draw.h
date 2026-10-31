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

/* -------------------------------------------------------
 * The Scenario Text window
 *
 * The status line and the announcement are drawn in one
 * window of their own, the Scenario Text window, and not at
 * a place on the game view. The lines are laid out in a
 * fixed source rectangle measured in panel units, and that
 * rectangle is scaled into the window with one factor for
 * both axes, as large as the window allows, and centred in
 * it. The text is never stretched.
 *
 * The source rectangle is the width of the classic game
 * view at zoom 1 (15 squares of 16 units), so the share
 * across the view that an announcement's position names is
 * a share across this width. It has a row for each row of
 * text the round has used so far: the status line's on top,
 * then the announcements', oldest first, as many as have
 * been up at once. The rows are counted over the whole round
 * and not frame by frame, so the text does not change size
 * when a line comes and goes. A line too wide for the source
 * rectangle is broken into rows at its spaces; a row still
 * too wide is made smaller, by itself, until it fits across.
 * ------------------------------------------------------- */

/* The source rectangle's width, in panel units. */
#define SCN_TEXT_SRC_W_UNITS 240.0f

/* A line's letters, in panel units: the height the announcement was always
 * drawn at. */
#define SCN_TEXT_LINE_UNITS 20.0f

/* How far the outline reaches past the letters on each side, in panel units. */
#define SCN_TEXT_OUTLINE_UNITS 1.0f

/* The space kept between the lines and the edges of the source rectangle,
 * and between the two rows, in panel units. */
#define SCN_TEXT_PAD_UNITS 2.0f

/* The height of one row: the letters and the outline above and below them. */
#define SCN_TEXT_ROW_UNITS (SCN_TEXT_LINE_UNITS + 2.0f * SCN_TEXT_OUTLINE_UNITS)

/* The most rows there are: the status line and up to four announcements at
 * once (SCN_ANNOUNCE_STACK_MAX in client_sim.h), each of a row or two once
 * its text is broken to fit across. A row past this many is not drawn. */
#define SCN_TEXT_MAX_ROWS 10

/* The source rectangle's height for a number of rows. Fewer than one row
 * counts as one, and more than SCN_TEXT_MAX_ROWS as that many. */
static inline float scnTextSourceHeight(int rows) {
    if (rows < 1) rows = 1;
    if (rows > SCN_TEXT_MAX_ROWS) rows = SCN_TEXT_MAX_ROWS;
    return 2.0f * SCN_TEXT_PAD_UNITS + (float)rows * SCN_TEXT_ROW_UNITS +
           (float)(rows - 1) * SCN_TEXT_PAD_UNITS;
}

/* The top of a row in the source rectangle, row 0 at the top. */
static inline float scnTextRowTop(int row) {
    if (row < 0) row = 0;
    return SCN_TEXT_PAD_UNITS +
           (float)row * (SCN_TEXT_ROW_UNITS + SCN_TEXT_PAD_UNITS);
}

/* How the source rectangle lands in the window: screen pixels for one
 * source unit, and the screen point the source rectangle's top-left corner
 * goes to. */
typedef struct ScnTextFit {
    float scale;
    float originX, originY;
} ScnTextFit;

/*********************************************************
 *NAME:          scnTextFitRect
 *PURPOSE:
 *  Scales a source rectangle into a window rectangle as
 *  large as it goes with one factor for both axes, so the
 *  shape of the text is kept, and centres it in the window.
 *  The spare space is on the left and right when the window
 *  is wider than the source, and above and below when it is
 *  taller. A source or window with no area gives a scale of
 *  0 at the window's top-left corner.
 *
 *ARGUMENTS:
 *  srcW/srcH - the source rectangle's size, in units
 *  winX/winY - the window's top-left corner, in pixels
 *  winW/winH - the window's size, in pixels
 *********************************************************/
static inline ScnTextFit scnTextFitRect(float srcW, float srcH, float winX,
                                        float winY, float winW, float winH) {
    ScnTextFit fit;
    fit.scale   = 0.0f;
    fit.originX = winX;
    fit.originY = winY;
    if (srcW <= 0.0f || srcH <= 0.0f || winW <= 0.0f || winH <= 0.0f) {
        return fit;
    }
    fit.scale = winW / srcW;
    if (winH / srcH < fit.scale) fit.scale = winH / srcH;
    fit.originX = winX + (winW - srcW * fit.scale) * 0.5f;
    fit.originY = winY + (winH - srcH * fit.scale) * 0.5f;
    return fit;
}

/* A rectangle in screen pixels. */
typedef struct ScnTextBand {
    float x, y, w, h;
} ScnTextBand;

/*********************************************************
 *NAME:          scnTextTabletBand
 *PURPOSE:
 *  Where tablet mode draws the scenario text: a band at
 *  the top middle of its game view, inset from the top
 *  edge. The band is the source rectangle at the zoom's
 *  scale. A band wider than the room it has is made
 *  smaller, keeping its shape, until it fits.
 *
 *  The room is the view's width. When the scenario panel's
 *  square is shown in the view's top-right corner, the room
 *  ends one inset left of the square, and the band is
 *  centred in what is left, so it never covers the square.
 *  A square outside the view's width does not change the
 *  room.
 *
 *  A band with no room, or a view or source with no area,
 *  has a width and height of 0.
 *
 *ARGUMENTS:
 *  viewX/viewY - the game view's top-left, in pixels
 *  viewW       - the game view's width, in pixels
 *  srcW/srcH   - the source rectangle, in units
 *  scale       - pixels per unit at the zoom
 *  inset       - the space from the top edge, and kept
 *                from the square, in pixels
 *  haveSlot    - the panel's square is shown
 *  slotX       - the square's left edge, in pixels
 *********************************************************/
static inline ScnTextBand scnTextTabletBand(float viewX, float viewY,
                                            float viewW, float srcW,
                                            float srcH, float scale,
                                            float inset, bool haveSlot,
                                            float slotX) {
    ScnTextBand band;
    float       right = viewX + viewW;
    float       room;

    if (inset < 0.0f) inset = 0.0f;
    band.x = viewX;
    band.y = viewY + inset;
    band.w = 0.0f;
    band.h = 0.0f;
    if (viewW <= 0.0f || srcW <= 0.0f || srcH <= 0.0f || scale <= 0.0f) {
        return band;
    }
    if (haveSlot && slotX > viewX && slotX < right) {
        right = slotX - inset;
    }
    room = right - viewX;
    if (room <= 0.0f) return band;

    band.w = srcW * scale;
    band.h = srcH * scale;
    if (band.w > room) {
        band.h *= room / band.w;
        band.w  = room;
    }
    band.x = viewX + (room - band.w) * 0.5f;
    return band;
}

/*********************************************************
 *NAME:          scnTextPointerOnHandle
 *PURPOSE:
 *  Whether a point is on the Scenario Text window's bar or
 *  on its resize grip: the two places where the window
 *  takes the mouse. The bar is the strip directly above
 *  the text area, as wide as the text area and barH high,
 *  so its bottom edge is the text area's top edge and it
 *  covers no text. It holds the gear and the X. The grip
 *  is the gripSide square in the text area's bottom-right
 *  corner. Each rectangle includes its top and left edges
 *  and not its bottom and right edges, as ImGui's own
 *  rectangle test does. A text area with no area has no
 *  bar and no grip. A barH of 0 means no bar.
 *
 *ARGUMENTS:
 *  x/y      - the text area's top-left corner, in pixels
 *  w/h      - the text area's size, in pixels
 *  barH     - the bar's height, in pixels
 *  gripSide - the grip's side, in pixels
 *  px/py    - the point, in pixels
 *********************************************************/
static inline bool scnTextPointerOnHandle(float x, float y, float w, float h,
                                          float barH, float gripSide,
                                          float px, float py) {
    if (w <= 0.0f || h <= 0.0f) return false;
    if (px < x || px >= x + w) return false;
    if (barH > 0.0f && py >= y - barH && py < y) return true;
    if (gripSide > 0.0f && px >= x + w - gripSide &&
        py >= y + h - gripSide && py < y + h) {
        return true;
    }
    return false;
}

/*********************************************************
 *NAME:          scnTextPrintedRecently
 *PURPOSE:
 *  Whether a line was on screen in the Scenario Text
 *  window within the last holdMs milliseconds. The window
 *  stays up for that time after its last line goes, and
 *  not after it, so an empty window does not stay on the
 *  game view.
 *
 *ARGUMENTS:
 *  everPrinted - a line has been on screen this round
 *  lastMs      - the SDL tick, in ms, a line was last on
 *                screen
 *  nowMs       - the SDL tick now, in ms
 *  holdMs      - how long the window stays up after its
 *                last line, in ms
 *********************************************************/
static inline bool scnTextPrintedRecently(bool everPrinted, uint64_t lastMs,
                                          uint64_t nowMs, uint64_t holdMs) {
    if (!everPrinted) return false;
    if (nowMs < lastMs) return true;
    return nowMs - lastMs < holdMs;
}

/*********************************************************
 *NAME:          scnTextWindowShown
 *PURPOSE:
 *  Whether the desktop Scenario Text window is up this
 *  frame. It is up while the player has it shown and there
 *  is a line to show, or a line was on screen a short time
 *  ago (scnTextPrintedRecently), so the player can still
 *  grab it just after a line goes. It is also up while the
 *  player is busy with it (a drag, its settings, its close
 *  confirm), so that is not cut off. With no recent line
 *  and nothing busy it is not up, so no empty window stays
 *  on the game view.
 *
 *ARGUMENTS:
 *  shown    - the player has the window shown
 *  recent   - a line was on screen a short time ago
 *  haveText - there is a status line or an announcement
 *  busy     - the player is busy with the window
 *********************************************************/
static inline bool scnTextWindowShown(bool shown, bool recent,
                                      bool haveText, bool busy) {
    return shown && (recent || haveText || busy);
}

/*********************************************************
 *NAME:          scnTextEdgeShown
 *PURPOSE:
 *  Whether the Scenario Text window draws its faint edge.
 *  With a line to show, the edge is drawn when there is a
 *  backing (background opacity above 0%): at 0% the text
 *  stands on the map with no frame round it. With no line,
 *  the window draws no backing, and the edge is drawn only
 *  while the bar and the grip show, at any background
 *  opacity, so the player can see the window's size while
 *  the pointer is on it.
 *
 *ARGUMENTS:
 *  haveText       - there is a status line or an
 *                   announcement
 *  bgPct          - the background opacity, 0 to 100
 *  handlesVisible - the bar and the grip show
 *********************************************************/
static inline bool scnTextEdgeShown(bool haveText, int bgPct,
                                    bool handlesVisible) {
    if (haveText) return bgPct > 0;
    return handlesVisible;
}

/*********************************************************
 *NAME:          scnTextSavePlace
 *PURPOSE:
 *  Whether the Scenario Text window writes its place to
 *  the preferences this frame, when the place differs from
 *  the saved one. A move or a resize drag saves it. A
 *  window the player has moved before also saves the place
 *  it is pushed to when it is kept inside the main window,
 *  but only while it has a line to show: a window with no
 *  line never changes the saved place by itself.
 *
 *ARGUMENTS:
 *  dragging  - the bar or the grip is being dragged
 *  everMoved - a place has been saved before
 *  haveText  - there is a status line or an announcement
 *********************************************************/
static inline bool scnTextSavePlace(bool dragging, bool everMoved,
                                    bool haveText) {
    return dragging || (everMoved && haveText);
}

/* A position byte as a share of the view, 0 to 1. A byte past
 * SCN_ANNOUNCE_POS_MAX reads as the far edge. */
static inline float scnAnnouncePosShare(uint8_t b) {
    if (b > (uint8_t)SCN_ANNOUNCE_POS_MAX) b = (uint8_t)SCN_ANNOUNCE_POS_MAX;
    return (float)b / (float)SCN_ANNOUNCE_POS_MAX;
}

/*********************************************************
 *NAME:          scnTextLineAcross
 *PURPOSE:
 *  Where one line goes across the source rectangle, and
 *  how much smaller it is drawn. The line's centre goes at
 *  the share across that the script named, then the line
 *  is moved the least it takes to keep it one pad inside
 *  the edges. A line too wide for the room between the pads
 *  is made smaller by itself, keeping its shape, until it
 *  fits that room exactly.
 *
 *ARGUMENTS:
 *  srcW      - the source rectangle's width, in units
 *  lineW     - the line's width at full size, outline
 *              included, in units
 *  share     - the centre's share across, 0 to 1
 *  outX      - receives the line's left edge, in units
 *  outShrink - receives the factor the line is drawn at,
 *              1 for full size
 *********************************************************/
static inline void scnTextLineAcross(float srcW, float lineW, float share,
                                     float *outX, float *outShrink) {
    const float room   = srcW - 2.0f * SCN_TEXT_PAD_UNITS;
    float       shrink = 1.0f;
    float       w      = lineW;
    float       x;

    if (share < 0.0f) share = 0.0f;
    if (share > 1.0f) share = 1.0f;
    if (lineW > room && lineW > 0.0f && room > 0.0f) {
        shrink = room / lineW;
        w      = room;
    }
    x = share * srcW - w * 0.5f;
    if (x > srcW - SCN_TEXT_PAD_UNITS - w) x = srcW - SCN_TEXT_PAD_UNITS - w;
    if (x < SCN_TEXT_PAD_UNITS) x = SCN_TEXT_PAD_UNITS;
    if (outX != NULL) *outX = x;
    if (outShrink != NULL) *outShrink = shrink;
}

/* The sizes the desktop panel's resize drag snaps to, as percents of the
 * size the game's own zoom gives the panel, smallest first. A hundred is
 * that size exactly, which is the size every scenario was laid out against.
 * When the default size below does not fit, the default is one of these,
 * so a player who resizes the panel from it starts on a size the drag
 * knows. */
#define SCN_PANEL_CARDINALS {25, 50, 75, 100, 125, 150, 200, 300}

/* The most of the game view's shorter side the default panel may cover.
 * At zoom 1 the view is 240 pixels, so this lets the default reach 192
 * pixels (150 percent) and keeps a strip of the map clear beside and
 * below it. The view must be at least 360 pixels (zoom 1.5) for the full
 * SCN_PANEL_DEFAULT_PX to fit. */
#define SCN_PANEL_DEFAULT_VIEW_SHARE 0.8f

/* The side in screen pixels of the default panel, at every zoom: 225
 * percent of the 128 units a list is written in at zoom 1, so that its
 * writing is easy to read. The percent this gives is often not a cardinal
 * (112.5 at zoom 2, 56.25 at zoom 4); the drag starts from it all the
 * same, and snaps only when it comes near a cardinal. At zoom 1 and 1.25
 * the view is too small for this side, and the default is the largest
 * cardinal that fits (150 percent at both). */
#define SCN_PANEL_DEFAULT_PX 288.0f

/*********************************************************
 *NAME:          scnPanelDefaultPlace
 *PURPOSE:
 *  Where a desktop scenario panel opens, and how big, when
 *  the player has saved neither: its top-left corner at the
 *  game view's top-left corner, in by inset on both sides.
 *  The corner is never above minTop: the panel's move bar
 *  sits directly above the panel, so minTop is the lowest
 *  the menu bar reaches plus the bar's height, and the bar
 *  is then never under the menu bar or off the top of the
 *  main window.
 *
 *  The side is SCN_PANEL_DEFAULT_PX when that fits, and the
 *  percent is that side over baseSide: 150 at zoom 1.5,
 *  112.5 at zoom 2, 75 at zoom 3, 56.25 at zoom 4, 37.5 at
 *  zoom 6, 28.125 at zoom 8. It is not rounded, so the side
 *  is exactly that many pixels. Over zoom 9 the percent is
 *  under the 25 of SCN_PANEL_SCALE_MIN, and the caller's
 *  size limits then hold the side at 25 percent.
 *
 *  When SCN_PANEL_DEFAULT_PX does not fit (a small display,
 *  or a zoom under 1.5), the percent is the largest cardinal
 *  from 100 up that fits: 150 (192 pixels) at zoom 1, and
 *  150 (240 pixels) at zoom 1.25.
 *
 *  A size fits when its side is no more than
 *  SCN_PANEL_DEFAULT_VIEW_SHARE of the view's shorter side
 *  and stays inside the display from the corner it is
 *  placed at.
 *
 *  False when no size fits (not SCN_PANEL_DEFAULT_PX, and
 *  not even 100 percent), or there is no view or no zoom
 *  size: the caller then uses its older placement. The
 *  outputs are left alone on false.
 *
 *ARGUMENTS:
 *  viewX0/viewY0 - the game view's top-left, in render
 *                  pixels
 *  viewX1/viewY1 - its bottom-right
 *  displayW/H    - the display, in render pixels
 *  baseSide      - the panel's side at 100 percent
 *  inset         - the gap from the view's corner
 *  minTop        - the least the top-left's y may be
 *  cardinals     - the cardinal percents, smallest first
 *  count         - how many cardinals there are
 *  outX/outY     - receive the panel's top-left
 *  outPercent    - receives the percent
 *********************************************************/
static inline bool scnPanelDefaultPlace(float viewX0, float viewY0,
                                        float viewX1, float viewY1,
                                        float displayW, float displayH,
                                        float baseSide, float inset,
                                        float minTop,
                                        const int *cardinals, size_t count,
                                        float *outX, float *outY,
                                        float *outPercent) {
    const float viewW = viewX1 - viewX0;
    const float viewH = viewY1 - viewY0;
    const float x     = viewX0 + inset;
    const float y     = (viewY0 + inset < minTop) ? minTop : viewY0 + inset;
    float       room;
    float       best = -1.0f;
    size_t      i;

    if (viewW <= 0.0f || viewH <= 0.0f || baseSide <= 0.0f) return false;
    room = SCN_PANEL_DEFAULT_VIEW_SHARE * ((viewW < viewH) ? viewW : viewH);
    if (displayW - x < room) room = displayW - x;
    if (displayH - y < room) room = displayH - y;

    /* Half a pixel of slack in both tests, so a side that matches the
       room exactly is not lost to rounding. */
    if (SCN_PANEL_DEFAULT_PX <= room + 0.5f) {
        best = SCN_PANEL_DEFAULT_PX * 100.0f / baseSide;
    } else if (cardinals != NULL) {
        for (i = 0; i < count; i++) {
            const int   c    = cardinals[i];
            const float side = baseSide * (float)c / 100.0f;
            if (c < 100) continue;
            if (side > room + 0.5f) break;
            best = (float)c;
        }
    }
    if (best < 0.0f) return false;
    if (outX != NULL) *outX = x;
    if (outY != NULL) *outY = y;
    if (outPercent != NULL) *outPercent = best;
    return true;
}

/* The least width, in screen pixels, the Scenario Text window takes beside
 * a default scenario panel (scnTextBesidePanelPlace). At this width a line
 * of the 240-unit source is about 13 pixels high. With less room than this
 * the window keeps its older default, the top middle of the game view. */
#define SCN_TEXT_BESIDE_MIN_W_PX 160.0f

/*********************************************************
 *NAME:          scnTextBesidePanelPlace
 *PURPOSE:
 *  Where the Scenario Text window's text area goes, and how
 *  wide it is, when the player has saved no place for it
 *  and a scenario panel is at its own default place at the
 *  left of the game view: in the room from the panel's
 *  right edge to the game view's right edge.
 *
 *  The left edge is one inset right of the panel, and the
 *  right edge is one inset left of the view's right edge
 *  (or of the display's, when the display ends first). The
 *  top is the panel's top, so the text area lines up with
 *  the panel and the two move bars line up above them.
 *
 *  False when that room is narrower than minW, or the
 *  numbers give no room at all: the caller then keeps its
 *  older default. The outputs are left alone on false.
 *
 *ARGUMENTS:
 *  panelRight - the panel's right edge, in render pixels
 *  panelTop   - the panel's top edge (below its bar)
 *  viewRight  - the game view's right edge
 *  displayW   - the display's width
 *  inset      - the gap kept on both sides
 *  minW       - the least width the text area may have
 *  outX/outY  - receive the text area's top-left
 *  outW       - receives the text area's width
 *********************************************************/
static inline bool scnTextBesidePanelPlace(float panelRight, float panelTop,
                                           float viewRight, float displayW,
                                           float inset, float minW,
                                           float *outX, float *outY,
                                           float *outW) {
    const float right = ((viewRight < displayW) ? viewRight : displayW) - inset;
    const float x     = panelRight + inset;
    const float w     = right - x;
    if (w <= 0.0f || w < minW) return false;
    if (outX != NULL) *outX = x;
    if (outY != NULL) *outY = panelTop;
    if (outW != NULL) *outW = w;
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
