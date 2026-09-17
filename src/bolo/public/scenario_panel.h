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
 *Name:          Scenario Panel Display List
 *Filename:      scenario_panel.h
 *Author:        John Morrison
 *Purpose:
 *  The scenario panel's on-the-wire display list and the one shared
 *  function that decodes and validates it. A scenario never draws on
 *  a client: it sends a list of drawing primitives as data, and each
 *  frontend interprets that list with its own 2D calls. The list is
 *  the contract; the drawing is the frontend's.
 *
 *  Parsing lives here, once, so a malformed list is refused
 *  identically everywhere and no frontend invents its own layout.
 *
 *  Dependency-light: only global.h (MAX_TANKS) and platform_types.h
 *  (BOLO_STATIC_ASSERT), both public, plus the C standard headers. No
 *  sim internals, no scenario surface, no SDL — so it compiles into
 *  the standalone log viewer, the wasm target, Android and iOS, none
 *  of which link the bolo sim.
 *********************************************************/
#ifndef SCENARIO_PANEL_H
#define SCENARIO_PANEL_H

#include <stddef.h>
#include <stdint.h>

#include "global.h"         /* MAX_TANKS — the slot a name primitive names */
#include "platform_types.h" /* BOLO_STATIC_ASSERT */

#ifdef __cplusplus
extern "C" {
#endif

#define SCN_PANEL_UNITS      128   /* the logical square, origin top-left */
#define SCN_PANEL_IDS        4     /* panel ids 0..3 */
#define SCN_PANEL_TEXT_MAX   48    /* bytes of one text primitive's string */
#define SCN_PANEL_ITEMS_MAX  128   /* most primitives one list may hold */
#define SCN_PANEL_COLOURS    16    /* the palette's size */
#define SCN_MARKERS_MAX      16    /* marker ids 0..15, kept by id */

/* Byte capacity of one panel display list. The list rides a single
 * control segment, which carries CHANNEL_CONTROL_SEG (1024) bytes
 * (channel_mux.h) and rejects a message larger than that. Of those,
 * the channel frame spends type(1) + bodyLen(2), and the panel event's
 * own header spends target(1) + panel(1) + len(2):
 *   1024 - 1 - 2 - 1 - 1 - 2 = 1017
 * server_sim_scenario.c pins this against CHANNEL_CONTROL_SEG, which
 * it can see and this header cannot.
 *
 * It lives here rather than beside the op payloads because both the op
 * that carries a list and the control event that delivers one are
 * measured against it, and control_event.h is public: it cannot read
 * scenario_api/. One number, so the two cannot drift apart. */
#define SCN_PANEL_MAX 1017

/* The primitives. These values travel, so a recording written today
 * reads the same tomorrow: a number here is never reused or shifted. */
typedef enum {
    SCN_PANEL_OP_RECT   = 1,
    SCN_PANEL_OP_LINE   = 2,
    SCN_PANEL_OP_TEXT   = 3,
    SCN_PANEL_OP_NAME   = 4,
    SCN_PANEL_OP_SPRITE = 5,
    SCN_PANEL_OP_BAR    = 6,
    SCN_PANEL_OP_TIMER  = 7
} ScnPanelOpcode;

/* The palette. A script picks an index, so a skin or a dark mode maps
 * the sixteen entries rather than a script choosing RGB. Index 0 draws
 * nothing, and the four reserved entries parse and draw as nothing
 * until a skin gives them a colour. */
typedef enum {
    SCN_PANEL_COLOUR_NONE = 0, SCN_PANEL_COLOUR_BLACK, SCN_PANEL_COLOUR_WHITE,
    SCN_PANEL_COLOUR_GREY, SCN_PANEL_COLOUR_GREY_DARK, SCN_PANEL_COLOUR_RED,
    SCN_PANEL_COLOUR_GREEN, SCN_PANEL_COLOUR_BLUE, SCN_PANEL_COLOUR_YELLOW,
    SCN_PANEL_COLOUR_ORANGE, SCN_PANEL_COLOUR_CYAN, SCN_PANEL_COLOUR_MAGENTA,
    SCN_PANEL_COLOUR_RESERVED_12, SCN_PANEL_COLOUR_RESERVED_13,
    SCN_PANEL_COLOUR_RESERVED_14, SCN_PANEL_COLOUR_RESERVED_15
} ScnPanelColour;

/* Text height, in the frontend's own font. */
typedef enum {
    SCN_PANEL_SIZE_SMALL  = 0,
    SCN_PANEL_SIZE_NORMAL = 1
} ScnPanelTextSize;

/* Which way text and timers sit about their x. */
typedef enum {
    SCN_PANEL_ALIGN_LEFT   = 0,
    SCN_PANEL_ALIGN_CENTRE = 1,
    SCN_PANEL_ALIGN_RIGHT  = 2
} ScnPanelAlign;

/* A timer counts down to its tick or up from it; the client works the
 * minutes and seconds out against its own tick, so a countdown is one
 * message rather than one a tick. */
typedef enum {
    SCN_PANEL_TIMER_DOWN = 0,
    SCN_PANEL_TIMER_UP   = 1
} ScnPanelTimerMode;

/* A map marker's shape. SQUARE marks a map square, FOLLOW rides a
 * player slot, and CLEAR removes the id the event names. These sit
 * beside the panel's own enums because a frontend draws both from the
 * same public header and neither needs the sim. */
typedef enum {
    SCN_MARKER_KIND_SQUARE = 0,
    SCN_MARKER_KIND_FOLLOW = 1,
    SCN_MARKER_KIND_CLEAR  = 2
} ScnMarkerKind;

/* Whose score a scenario score row is: one player's, or one team's. */
typedef enum {
    SCN_SCORE_KIND_PLAYER = 0,
    SCN_SCORE_KIND_TEAM   = 1
} ScnScoreKind;

/* The byte layout.
 *
 * A list is a bare sequence of primitives with no count prefix — the
 * byte length bounds it, and a zero-length list is a valid empty list
 * that clears the panel. Each primitive is one opcode byte followed by
 * fixed operands, multi-byte fields big-endian, matching the record
 * layouts in log.h.
 *
 *   Primitive  Opcode  Operands after the opcode                       Total
 *   rect       1       x, y, w, h, colour, fill                        7
 *   line       2       x0, y0, x1, y1, colour                          6
 *   text       3       x, y, colour, size, align, len, then len bytes  7 + len
 *   name       4       x, y, colour, size, align, slot                 7
 *   sprite     5       x, y, tile                                      4
 *   bar        6       x, y, w, h, colour, value (u16), max (u16)      10
 *   timer      7       x, y, colour, size, align, mode, tick (u32)     11
 *
 * tile is a tilenum.h id and fits a byte: the largest naming a slot on the
 * sheet is CRATER_RIGHT at 181, and TANK_TRANSPARENT, the sentinel above
 * every slot, is 255.
 * tick is a game tick as serverSimGetTick answers it. A bar's value and
 * max are 16-bit so a bar can show a real total rather than one capped
 * at 255. */

/* One decoded primitive. */
typedef struct {
    uint8_t op;                     /* a ScnPanelOpcode */
    union {
        struct { uint8_t x, y, w, h, colour, fill; } rect;
        struct { uint8_t x0, y0, x1, y1, colour; } line;
        struct { uint8_t x, y, colour, size, align, len;
                 char text[SCN_PANEL_TEXT_MAX + 1]; } text;   /* always terminated */
        struct { uint8_t x, y, colour, size, align, slot; } name;
        struct { uint8_t x, y, tile; } sprite;
        struct { uint8_t x, y, w, h, colour; uint16_t value, max; } bar;
        struct { uint8_t x, y, colour, size, align, mode; uint32_t tick; } timer;
    } u;
} ScnPanelItem;

/* One decoded list, as a frontend draws it. */
typedef struct {
    uint8_t      count;
    ScnPanelItem items[SCN_PANEL_ITEMS_MAX];
} ScnPanelList;

/* Why a list was refused, so a caller can map one onto its own result
 * code and a test can say which rule fired. */
typedef enum {
    SCN_PANEL_OK = 0,
    SCN_PANEL_ERR_TRUNCATED,  /* a primitive runs past the end of the bytes */
    SCN_PANEL_ERR_OPCODE,     /* 0, or past the last primitive */
    SCN_PANEL_ERR_RANGE,      /* an operand outside its range */
    SCN_PANEL_ERR_TEXT,       /* a text length past SCN_PANEL_TEXT_MAX, or a byte that is not printable */
    SCN_PANEL_ERR_TOO_MANY    /* more than SCN_PANEL_ITEMS_MAX primitives */
} ScnPanelResult;

/* Decode and validate a display list. Writes *out only on SCN_PANEL_OK;
 * out may not be NULL. len 0 gives an empty list.
 *
 * A list is taken whole or not at all: a bad byte anywhere in the
 * stream leaves *out untouched, so a frontend never draws the front
 * half of a list whose tail was malformed. A NULL out answers
 * SCN_PANEL_ERR_RANGE rather than writing through it, and a NULL bytes
 * with a non-zero len answers SCN_PANEL_ERR_TRUNCATED.
 *
 * Coordinates, widths, heights and tile ids take any byte value:
 * clipping the square and skipping a tile the skin does not define are
 * the drawer's business, not the parser's. */
ScnPanelResult scnPanelParse(const uint8_t *bytes, uint16_t len, ScnPanelList *out);

/* Encode a decoded list. Answers the bytes written, or 0 when the list
 * does not fit cap or holds an item scnPanelParse would refuse.
 *
 * An empty list also answers 0, having written nothing; a caller telling
 * the two apart reads list->count. Both directions answer to one set of
 * operand rules, so a list this writes is a list scnPanelParse takes
 * back unchanged. */
uint16_t scnPanelWrite(const ScnPanelList *list, uint8_t *bytes, uint16_t cap);

/* Every operand the layout spends one byte on has to fit one. */
BOLO_STATIC_ASSERT(SCN_PANEL_UNITS <= 256, scn_panel_units_fit_a_byte);
BOLO_STATIC_ASSERT(MAX_TANKS <= 256, scn_panel_slot_fits_a_byte);
BOLO_STATIC_ASSERT(SCN_PANEL_TEXT_MAX <= 255, scn_panel_text_len_fits_a_byte);
BOLO_STATIC_ASSERT(SCN_PANEL_COLOURS <= 256, scn_panel_colour_fits_a_byte);
/* And the decoded count is a byte too. */
BOLO_STATIC_ASSERT(SCN_PANEL_ITEMS_MAX <= 255, scn_panel_count_fits_a_byte);

#ifdef __cplusplus
}
#endif

#endif /* SCENARIO_PANEL_H */
