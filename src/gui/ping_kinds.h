/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Ping Kinds
 *Filename:      ping_kinds.h
 *Purpose:
 *  The one table of what each smart ping looks like:
 *  colour, icon file and how long a received ping stays on
 *  screen. Every drawer reads it — the pie menu, the world
 *  marker, the off-screen edge marker, the message line and
 *  the replay viewer — so a kind can never be one colour in
 *  the pie and another on the map.
 *
 *  The kind values themselves are on the wire and live in
 *  src/bolo/public/input_packet.h (PING_KIND_*) next to
 *  EVENT_PING; this header only decorates them. Add a kind
 *  there and a row here in the same change — the table is
 *  sized by PING_KIND_COUNT, so a row too many fails the
 *  build, and run_ping_event_wire (tests/unit/test_ping.c)
 *  catches the row too few that would otherwise zero-fill
 *  into a nameless black icon.
 *
 *  No SDL and no ImGui: the log viewer draws with
 *  SDL_Renderer and the game's overlay with an ImGui draw
 *  list, so the colours are plain bytes both can build their
 *  own value out of.
 *********************************************************/

#ifndef WINBOLO_PING_KINDS_H
#define WINBOLO_PING_KINDS_H

#include <stddef.h>         /* size_t */
#include <string.h>         /* memcpy, strlen */

#include "input_packet.h"   /* PING_KIND_*, PING_KIND_COUNT */
#include "lang.h"           /* MESSAGE_PING_* / STR_PING_* — the per-kind text */

#ifdef __cplusplus
extern "C" {
#endif

/* PING_DISPLAY_MS / PING_FADE_MS come in with input_packet.h: the client's
 * ping ring expires on them too, so they are not a drawing-only choice.
 *
 * The off-screen edge marker: a bar this thick, laid along the border of the
 * game view where the line from the viewer's tank to the ping leaves it. */
#define PING_EDGE_THICKNESS_PX 10.0f

/* How long that bar is, and how big the icon on it, both from how far away
 * the ping is: near is a big marker, far is a small one, so the size alone
 * says roughly how much of the map is between the player and it.
 *
 * The two distances are in map squares from the viewer's tank. Eight is about
 * half the classic 15x15 view — a ping that has only just gone off the edge —
 * and 64 is a quarter of the map away, past which there is nothing left to
 * tell apart. Between them pingEdgeSizeFactor (ping_edge.h) ramps the size,
 * and the pixel sizes below are at the view's own scale, like every other
 * number here.
 *
 * The icon's minimum is the floor the whole thing is designed around: below
 * about 12 px the glyphs stop reading as anything and the marker is a
 * coloured dash. The name under the bar does not shrink with it. */
#define PING_EDGE_NEAR_TILES      8.0f   /* map squares: at or under, biggest */
#define PING_EDGE_FAR_TILES      64.0f   /* map squares: at or over, smallest */
#define PING_EDGE_LENGTH_MIN_PX  14.0f   /* bar length at PING_EDGE_FAR_TILES */
#define PING_EDGE_LENGTH_MAX_PX  52.0f   /* bar length at PING_EDGE_NEAR_TILES */
#define PING_EDGE_ICON_MIN_PX    12.0f   /* icon at PING_EDGE_FAR_TILES */
#define PING_EDGE_ICON_MAX_PX    30.0f   /* icon at PING_EDGE_NEAR_TILES */
/* Air between the bar and the icon, and between the icon and the name. */
#define PING_EDGE_GAP_PX          3.0f

typedef struct {
    unsigned char r, g, b;   /* the kind's colour, opaque */
    const char   *iconFile;  /* under data/ui/ping/, monochrome white on
                                transparent — tinted with the colour above at
                                draw time, never used as authored */
} PingKindStyle;

/* Indexed by PING_KIND_*. Colours are picked to stay apart from each other
 * and from the map: the terrain is green and brown, so the "assist" green is
 * pushed bright and cold, and every colour is drawn over a dark disc so it
 * reads on grass as well as on water. */
static const PingKindStyle kPingKindStyles[PING_KIND_COUNT] = {
    /* PING_KIND_STANDARD    */ { 255, 255, 255, "standard.svg"   },
    /* PING_KIND_CAUTION     */ { 255, 214,   0, "caution.svg"    },
    /* PING_KIND_ASSIST      */ {  60, 220,  90, "assist.svg"     },
    /* PING_KIND_ATTACK      */ { 235,  60,  50, "attack.svg"     },
    /* PING_KIND_ON_MY_WAY   */ {  60, 190, 255, "onmyway.svg"    },
    /* PING_KIND_BOT_COMMAND */ { 200, 110, 255, "botcommand.svg" }
};

/* The style for a kind, with an unknown kind falling back to the standard
 * ping rather than reading off the end — a ping from a newer build reaching
 * an older one should still draw something. */
static inline const PingKindStyle *pingKindStyle(unsigned char kind) {
    if (kind >= PING_KIND_COUNT) kind = PING_KIND_STANDARD;
    return &kPingKindStyles[kind];
}

/* The newswire line a received ping of this kind posts ("{player}: Attack!").
 * A kind this build does not know falls back to the plain ping line for the
 * same reason pingKindStyle falls back to the standard style. */
static inline langid pingKindMessageId(unsigned char kind) {
    switch (kind) {
    case PING_KIND_CAUTION:     return MESSAGE_PING_CAUTION;
    case PING_KIND_ASSIST:      return MESSAGE_PING_ASSIST;
    case PING_KIND_ATTACK:      return MESSAGE_PING_ATTACK;
    case PING_KIND_ON_MY_WAY:   return MESSAGE_PING_ON_MY_WAY;
    case PING_KIND_BOT_COMMAND: return MESSAGE_PING_BOT_COMMAND;
    default:                    return MESSAGE_PING_STANDARD;
    }
}

/* The kind's own name, for the pie menu's slice labels and the key-setup
 * rows. Same unknown-kind fallback. */
static inline langid pingKindNameId(unsigned char kind) {
    switch (kind) {
    case PING_KIND_CAUTION:     return STR_PING_CAUTION;
    case PING_KIND_ASSIST:      return STR_PING_ASSIST;
    case PING_KIND_ATTACK:      return STR_PING_ATTACK;
    case PING_KIND_ON_MY_WAY:   return STR_PING_ON_MY_WAY;
    case PING_KIND_BOT_COMMAND: return STR_PING_BOT_COMMAND;
    default:                    return STR_PING_STANDARD;
    }
}

/* How much of the sender's name a ping marker shows. Counts CHARACTERS, not
 * bytes: six Cyrillic letters are twelve bytes and are still six characters,
 * and a name is never cut inside a UTF-8 sequence. A name at or under this is
 * drawn as it is.
 *
 * The marker is a hint on the map, not a scoreboard — a long name under it
 * covers the ground the ping is pointing at, and the edge bars are laid along
 * a border where a long name runs into the next one. The message line in the
 * newswire keeps the whole name, so nothing is lost. */
#define PING_NAME_MAX_CHARS 6

/* What a shortened name ends in. U+2026 HORIZONTAL ELLIPSIS, spelled out in
 * UTF-8 so this header needs no wide literals.
 *
 * The ASCII form is for a renderer whose font has no U+2026 to draw: the
 * marker names use the Sarasa faces, which carry it, but the edge bars are
 * drawn with the ImGui atlas, and that is built over Latin, Cyrillic, Greek
 * and Vietnamese only (imguiBoloGlyphRanges in src/gui/imgui_fonts.h) — a
 * U+2026 there would come out as a missing-glyph box. */
#define PING_NAME_ELLIPSIS       "\xE2\x80\xA6"
#define PING_NAME_ELLIPSIS_ASCII "..."

/* A buffer this big always holds a shortened name whole: every character kept
 * at UTF-8's maximum four bytes, the longer of the two ellipses, and the
 * terminator. */
#define PING_NAME_DISPLAY_MAX (PING_NAME_MAX_CHARS * 4 + 3 + 1)

/*********************************************************
*NAME:          pingDisplayName
*PURPOSE:
*  The name a ping marker draws: `name` unchanged when it is
*  PING_NAME_MAX_CHARS characters or fewer, and otherwise its
*  first PING_NAME_MAX_CHARS characters followed by the
*  ellipsis. The one copy of that rule — the world marker,
*  the overview, the replay viewer and the off-screen edge
*  bars all shorten a name through here, so they cannot
*  disagree about where it stops.
*
*  Characters are UTF-8 code points, counted by their lead
*  bytes, so the cut always lands on a character boundary.
*  Nothing is written past `outSize`; a buffer too small for
*  the whole result loses whole characters off the end rather
*  than half of one.
*
*ARGUMENTS:
*  name     - the sender's name, or NULL for none
*  ellipsis - what a shortened name ends in, or NULL for
*             PING_NAME_ELLIPSIS
*  out      - buffer to write into, PING_NAME_DISPLAY_MAX to
*             be sure of the whole result
*  outSize  - size of that buffer in bytes
*
*RETURNS:
*  out, holding the name to draw, or "" when there is no
*  buffer to write into
*********************************************************/
static inline const char *pingDisplayName(const char *name,
                                          const char *ellipsis,
                                          char *out, size_t outSize) {
    size_t bytes = 0;   /* the whole name, in bytes */
    size_t chars = 0;   /* the whole name, in characters */
    size_t cut = 0;     /* byte the character past the cap starts at */
    size_t keep;
    size_t tailLen;
    const char *tail;

    if (out == NULL || outSize == 0) return "";
    out[0] = '\0';
    if (name == NULL) return out;

    /* A byte that is not a continuation byte (10xxxxxx) starts a character. */
    while (name[bytes] != '\0') {
        if (((unsigned char)name[bytes] & 0xC0) != 0x80) {
            if (chars == PING_NAME_MAX_CHARS) cut = bytes;
            chars++;
        }
        bytes++;
    }

    if (chars <= PING_NAME_MAX_CHARS) {
        keep = bytes;
        tail = "";
    } else {
        keep = cut;
        tail = (ellipsis != NULL) ? ellipsis : PING_NAME_ELLIPSIS;
    }
    tailLen = strlen(tail);

    /* Whatever the caller's buffer is, the result fits in it. The ellipsis
     * goes first if even it will not fit, then whole characters come off the
     * end until the rest does. */
    if (tailLen + 1 > outSize) {
        tail = "";
        tailLen = 0;
    }
    while (keep + tailLen + 1 > outSize) {
        do {
            keep--;
        } while (keep > 0 && ((unsigned char)name[keep] & 0xC0) == 0x80);
    }

    memcpy(out, name, keep);
    memcpy(out + keep, tail, tailLen);
    out[keep + tailLen] = '\0';
    return out;
}

/* 0..1 opacity for a ping `ageMs` old: solid until the fade window, then a
 * straight ramp to nothing. Returns 0 once the ping has expired, so a caller
 * that draws whatever this returns needs no separate expiry test. */
static inline float pingDisplayAlpha(int ageMs) {
    if (ageMs < 0) return 0.0f;
    if (ageMs >= PING_DISPLAY_MS) return 0.0f;
    if (ageMs <= PING_DISPLAY_MS - PING_FADE_MS) return 1.0f;
    return (float)(PING_DISPLAY_MS - ageMs) / (float)PING_FADE_MS;
}

#ifdef __cplusplus
}
#endif

#endif /* WINBOLO_PING_KINDS_H */
