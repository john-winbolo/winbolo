/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*********************************************************
 *Name:          View Policy
 *Filename:      view_policy.h
 *Author:        John Morrison
 *Purpose:
 *  Operator-chosen visibility rules for pillboxes, bases
 *  and allied tanks. Set at server startup or from the
 *  lobby, and broadcast to remote clients via the
 *  lobby-settings control event so the lobby UI and the
 *  server browser can reflect them.
 *********************************************************/

#ifndef VIEW_POLICY_H
#define VIEW_POLICY_H

/* Values are ordered so a zero-initialized server (and a client that
 * decodes a lobby-settings payload that predates the fields) defaults
 * to ALWAYS — today's pill behaviour. */
typedef enum {
    viewPolicyAlways = 0,   /* zero = wire default = today's pill behaviour */
    viewPolicyKey    = 1,
    viewPolicyDecay  = 2,
    viewPolicyOff    = 3
} ViewPolicy;

typedef enum {
    viewCategoryPill = 0,
    viewCategoryBase = 1,
    viewCategoryAlly = 2,
    VIEW_CATEGORY_COUNT
} ViewCategory;

/* Which block of squares the map overview keeps live around the player's
 * own tank. Expanded is everything the classic 15x15 view could scroll
 * to; Classic narrows it to the window that view is actually showing;
 * None does not offer the map overview or the full-screen map at all.
 * Zero is today's behaviour, so a zero-initialized server and a client
 * decoding a payload without the field both land on Expanded.
 *
 * None is last so Expanded and Classic keep the wire values they have
 * always had. A client built before None existed clamps an unknown value
 * against its own COUNT and reads 2 as Expanded — see
 * client_sim_control.c — so it offers the overview on a server that has
 * turned it off. That is the old behaviour of every such client (classic
 * mode was only ever a convenience rule, never a guarantee about a
 * modified client), so it is not worth a protocol bump; nothing else in
 * this file's history has taken one for an appended enum value. */
typedef enum {
    overviewWindowExpanded = 0,
    overviewWindowClassic  = 1,
    overviewWindowNone     = 2,
    OVERVIEW_WINDOW_COUNT
} OverviewWindow;

/* Whether anything stops the player seeing inside that block. A selector
 * rather than a bool so another rule can join it without a second
 * setting. Zero is today's behaviour. */
typedef enum {
    lineOfSightOff               = 0,
    lineOfSightBuildingsAndTrees = 1,
    LINE_OF_SIGHT_COUNT
} LineOfSightMode;

/* ---------------------------------------------------------------------
 * The rules a stock server runs.
 *
 * Two different questions in this codebase are both answered with a
 * set of ViewPolicy values, and the two answers are not the same set.
 * Only the first one is named here:
 *
 *   A. What a server nobody has configured actually runs: pill Key,
 *      base Off, ally Off, the classic overview window, sight off.
 *      That is what the five macros below hold, and all they hold.
 *
 *   B. What a reader assumes of a sender that named no rules — the
 *      back-compatibility answer for an INFO packet, an mDNS TXT
 *      record or a tracker row from before the fields existed: pill
 *      Always, base Off, ally Always, the expanded window, sight off.
 *      Those values are pinned by the wire (they are the zero encoding,
 *      and what every server did before the fields existed), so they
 *      must NOT follow A when A changes.
 *
 * B merely resembles A. Writing A's values at a B site is how a
 * shipped bug got in, which is why these are named STOCK and not
 * DEFAULT: at a wire fallback the question is "what did a sender that
 * said nothing mean", and no macro here answers that question.
 *
 * USE these wherever the code seeds, describes or compares against what
 * an unconfigured server runs: serverSimInit, the client's pre-lobby
 * seed in clientSimCreate, the gameFrontView* globals and their INI
 * fallbacks, the dedicated server's -pillview / -baseview / -allyview
 * and -overviewwindow fallbacks, the headless option table, the
 * browser's "differs from stock" tag, and overviewViewInputsDefaults in
 * overview_map.c.
 *
 * DO NOT use these at:
 *   - infoPacketReadViewPolicies and infoPacketReadViewPolicies2 in
 *     netpacks.h, the TXT-record fill in discovery_mdns.c, the
 *     readIntFieldDef fallbacks in wbn_serverlist.c, or pingServer's
 *     pre-ping fill in imgui_gamebrowser.cpp. All meaning B.
 *   - the NULL-handle returns of the serverSimGet and clientSimGet
 *     view accessors. Also meaning B, and server_sim.h documents them
 *     as "what a reader assumes of a sender that named no policy, not
 *     what a sim starts on".
 *   - serverSimSetClassicMode, which states classic mode's own set. It
 *     coincides with A today; it is not the same statement.
 *
 * Adding a visibility setting? Give it a STOCK macro here, and then
 * decide separately — and write down — what a reader should assume when
 * the wire does not carry it.
 * --------------------------------------------------------------------- */
#define VIEW_POLICY_STOCK_PILL viewPolicyKey
#define VIEW_POLICY_STOCK_BASE viewPolicyOff
#define VIEW_POLICY_STOCK_ALLY viewPolicyOff
#define OVERVIEW_WINDOW_STOCK  overviewWindowClassic
#define LINE_OF_SIGHT_STOCK    lineOfSightOff

#define VIEW_DECAY_DEFAULT_SECS 30
#define VIEW_DECAY_MIN_SECS 5
#define VIEW_DECAY_MAX_SECS 600
#define VIEW_DECAY_NEAR_TILES 9       /* Chebyshev distance in map squares */
#define VIEW_DECAY_FADE_SECS 5        /* overview fade window before expiry */

#endif /* VIEW_POLICY_H */
