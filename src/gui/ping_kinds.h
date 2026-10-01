/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
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
 * Every renderer can draw it: the marker names use the Sarasa faces, which
 * carry the glyph, and the edge bars draw from the ImGui atlas, which has it
 * added by name (imguiBoloGlyphRanges in src/gui/imgui_fonts.h) because it
 * sits in General Punctuation, outside the ranges that atlas is built over.
 * An ellipsis an atlas has no glyph for would come out as a missing-glyph
 * box, which is what the `ellipsis` argument to pingDisplayName below is for
 * — a caller whose font cannot manage U+2026 passes its own. No renderer
 * needs that today. */
#define PING_NAME_ELLIPSIS "\xE2\x80\xA6"

/* How a ping's name is drawn against the tank labels it borrows its face and
 * its black shadow from. Both renderers read these: the world marker, which
 * draws the name through the tank label's own drawer, and the off-screen edge
 * bars, which draw it with the ImGui atlas.
 *
 * PING_NAME_SCALE is a fraction of the tank label's text size, so it follows
 * the font and the zoom like the label does. Smaller is the point: a name over
 * the map should not be read as a tank's at a glance, and the marker names a
 * spot on the ground rather than something driving around on it.
 *
 * PING_NAME_GREY is the 0-255 grey the text itself is drawn at: tank labels
 * are 200-grey, ping names sit a little brighter than that but short of pure
 * white so the two read as different things; the size difference
 * (PING_NAME_SCALE) does most of the telling-apart. The shadow under it is the
 * label's own — same offset, same black — because the terrain under a name
 * runs from black sea to pale road either way. */
#define PING_NAME_SCALE 0.8f
#define PING_NAME_GREY  225

/* A buffer this big always holds a shortened name whole: every character kept
 * at UTF-8's maximum four bytes, the ellipsis's three, and the terminator. A
 * caller passing its own ellipsis longer than that sizes its own buffer. */
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

/* The blink. A ping sits on top of the map, and the map is what the player
 * needs to keep reading — the tile it names, and whatever is moving over it.
 * So the marker is off for most of each cycle rather than covering that tile
 * for its whole five seconds.
 *
 * One part on to three parts off. Written as a period and an on-time rather
 * than as a ratio so the two can be tuned apart.
 *
 * Not in game ticks, though the ratio came from thinking in them: a tick is
 * 20 ms (GAME_NUMGAMETICKS_SEC), so one on and three off would be an 80 ms
 * cycle — a flicker rather than a blink, and at that rate a player reads it
 * as a dim marker instead of a blinking one. A quarter second on, three
 * quarters off, is the same ratio at a rate the eye resolves. */
#define PING_BLINK_PERIOD_MS 1000
#define PING_BLINK_ON_MS      250

/* The FIRST flash is held longer than the rest. It is the one that has to be
 * noticed — a quarter second is enough to repeat a mark the player has
 * already seen, and not enough to catch an eye that was looking somewhere
 * else when the ping landed. The arrival ring runs inside this, finishing at
 * 250 ms with the flash still solid for another 250. */
#define PING_BLINK_FIRST_ON_MS 500

/* How much of each flash is spent arriving and leaving. The flash ramps up
 * over this, holds, then ramps down over it again, so the marker breathes
 * rather than being cut in and out.
 *
 * Taken out of the on-time, not added to it: two of these plus the hold is
 * PING_BLINK_ON_MS, so a flash is still a quarter second whatever this is
 * set to. It has to stay under half the on-time or there is no hold left;
 * the code clamps rather than trusting that. */
#define PING_BLINK_FADE_MS     80

/* How many times the world marker flashes before it gives the tile back for
 * good. Three says "look here" and then stops asking.
 *
 * This ends the WORLD marker early, at three periods, where the ping itself
 * lives PING_DISPLAY_MS. The steady markers — the off-screen edge one and
 * the overview's dot — run the full life as before. Neither of those covers
 * anything, so there is no reason to take them away sooner, and the edge
 * marker is the one still pointing at a ping that is off the screen. */
#define PING_BLINK_COUNT        3

/* The arrival ring: a ring in the ping's own colour that closes onto the
 * marker square once, as the ping lands, and is gone before the first flash
 * is. It says "something happened HERE, now" in a way a marker that is simply
 * on cannot — the eye is drawn by the movement, not by the mark.
 *
 * ONCE, not once a flash. The stage times below are read against the ping's
 * AGE, the way pingWorldMarkerAlpha reads it, so the ring runs on the ping's
 * own clock from the moment it arrived and needs no wall clock: a recording
 * replays the ring it showed live. The second and third flashes are then a
 * plain marker, which is the point — three rings would be three arrivals.
 *
 * The two stages are ring_band.h's: a close from START to END over
 * PING_RING_MS, then a pulse out to PULSE and back over PING_RING_PULSE_MS
 * with a fade across it. Same shape as the map overview's respawn ring, and
 * deliberately its own numbers — the two are tuned apart, and this one is
 * about a fifth as long.
 *
 * PING_RING_MS + PING_RING_PULSE_MS is PING_BLINK_ON_MS exactly: the ring
 * lives entirely inside the first flash and ends as that flash ends, so the
 * whole marker leaves the tile at once and there is nothing left of the ring
 * to fight the two flashes that follow. Raising the sum past PING_BLINK_ON_MS
 * would leave the ring drawing into the gap where the marker has deliberately
 * given the tile back, so keep it at or under.
 *
 * Radii in MAP SQUARES, like the overview's, and the weight too: the drawer
 * multiplies by the view's own tile size, so the ring is the same size in map
 * terms at 1x as at 4x rather than a thick ring round a tiny square. The
 * minimum weight is the floor under that — below about two pixels a band
 * stops reading as a ring. START is five squares across, wide enough to catch
 * the eye off to the side of where the player is looking; END sits just
 * outside the corners of the marker square (half a square is 0.707 out at the
 * corner), so the ring lands ON the square rather than inside it. */
#define PING_RING_START_SQ       2.5f   /* radius: five squares across */
#define PING_RING_END_SQ         0.8f   /* just outside the square's corners */
#define PING_RING_PULSE_SQ       1.1f   /* how far the pulse reopens */
#define PING_RING_MS             180    /* the close */
#define PING_RING_PULSE_MS        70    /* the pulse, and the fade with it */
#define PING_RING_WEIGHT_SQ      0.16f  /* map squares of stroke, across the band */
#define PING_RING_WEIGHT_MIN_PX  2.0f   /* however far the zoom is out */

/* How solid the ring is at its brightest. Above the marker's own
 * PING_MARKER_ALPHA, which is held down because the square and the icon sit
 * over ground the player has to keep reading; the ring is over that ground for
 * a quarter of a second and then never again, so it can afford to be the
 * brightest thing in the effect. Short of 1 so it still reads as drawn onto
 * the map rather than punched through it. */
#define PING_RING_ALPHA          0.85f

/* The sender's name stays up for the ping's WHOLE life, steady, while the
 * marker under it blinks three times and stops. The name answers "who" and
 * that answer does not change; blinking it made the player wait for a flash
 * to read it, and it vanished at three seconds while the ping was still
 * live. Held back to this so a permanent label does not compete with the
 * marker it belongs to. Its end-of-life fade is pingDisplayAlpha's. */
#define PING_NAME_ALPHA          0.67f

/* 0..1 opacity for a ping `ageMs` old: solid until the fade window, then a
 * straight ramp to nothing. Returns 0 once the ping has expired, so a caller
 * that draws whatever this returns needs no separate expiry test.
 *
 * Steady. Every marker but the one on the ground uses this: the off-screen
 * edge marker and the overview's dot sit on the border and on a map the
 * player is reading deliberately, where nothing is hidden by them and a
 * blink would only be noise. */
static inline float pingDisplayAlpha(int ageMs) {
    if (ageMs < 0) return 0.0f;
    if (ageMs >= PING_DISPLAY_MS) return 0.0f;
    if (ageMs <= PING_DISPLAY_MS - PING_FADE_MS) return 1.0f;
    return (float)(PING_DISPLAY_MS - ageMs) / (float)PING_FADE_MS;
}

/* The same, blinking. For the WORLD MARKER alone — the one drawn on the
 * ground in the game view, over the tile it names and over whatever is moving
 * across it. That marker is the only one that hides anything the player needs
 * to keep reading, so it is the only one that gets out of the way.
 *
 * Returns a hard 0 in the gaps rather than a low alpha: a marker drawn at any
 * opacity still obscures, and every caller already skips on `alpha <= 0`, so
 * zero means the tile is drawn untouched for three quarters of each cycle.
 *
 * The phase comes from the ping's own age, so each ping blinks on its own
 * clock from the moment it arrived. Two pings a moment apart therefore blink
 * out of step, which keeps them apart rather than pulsing as one. It also
 * needs no wall clock, so a recording replays the blink it showed live.
 *
 * The first phase is an ON one, so a ping is visible the instant it lands.
 * The end-of-life fade still applies, so the last second blinks and dims. */
static inline float pingWorldMarkerAlpha(int ageMs) {
    if (ageMs < 0) return 0.0f;
    /* Done flashing: the tile is the player's again for the rest of the
     * ping's life. Tested before the phase so the marker cannot come back
     * for a fourth time. */
    if (ageMs >= PING_BLINK_COUNT * PING_BLINK_PERIOD_MS) return 0.0f;
    {
        const int phase      = ageMs % PING_BLINK_PERIOD_MS;
        const int firstFlash = (ageMs < PING_BLINK_PERIOD_MS);
        const int onMs       = firstFlash ? PING_BLINK_FIRST_ON_MS
                                          : PING_BLINK_ON_MS;
        int       fade  = PING_BLINK_FADE_MS;
        float     env;
        if (phase >= onMs) return 0.0f;
        /* No hold left means the two ramps would overlap and the flash would
         * never reach full. Cap them at half the on-time each, which turns
         * the trapezoid into a triangle rather than something wrong. */
        if (fade > onMs / 2) fade = onMs / 2;
        /* The FIRST flash starts solid. It plays under the arrival ring, and
         * the ring's whole job is to say "look here, now" — a marker easing
         * in over the first 80 ms arrives after the thing pointing at it,
         * and the sender's name under it arrives later still. So the ping
         * and its name are up the instant it lands, and only the fade OUT
         * applies to this one. Every later flash ramps both ways; by then
         * the player has been told, and the marker is repeating itself.
         *
         * PING_RING_MS + PING_RING_PULSE_MS is PING_BLINK_ON_MS, so the ring
         * finishes exactly as this first flash starts to leave. */
        if (fade <= 0) {
            env = 1.0f;
        } else if (phase < fade && !firstFlash) {
            env = (float)phase / (float)fade;                      /* in  */
        } else if (phase >= onMs - fade) {
            env = (float)(onMs - phase) / (float)fade;             /* out */
        } else {
            env = 1.0f;                                            /* hold */
        }
        return env * pingDisplayAlpha(ageMs);
    }
}

#ifdef __cplusplus
}
#endif

#endif /* WINBOLO_PING_KINDS_H */
