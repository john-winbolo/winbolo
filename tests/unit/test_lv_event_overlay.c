/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The sums behind the event markers of the log viewer's seek slider
 * (lv_event_overlay.h).
 *
 *   lv_event_overlay_slider — a time is put where the slider's handle is
 *       when playback is at that time: at both ends, in the middle, with
 *       the lobby hidden, and in a slider too narrow for its handle.
 *   lv_event_overlay_clusters — markers nearer than one marker width become
 *       one; the one a point is on; the team most of a marker's events name.
 *   lv_event_overlay_choice — the chosen kinds go to the preferences text
 *       and back; a name no build knows is ignored; an empty choice stays
 *       empty.
 *   lv_event_overlay_picture — a pillbox event and a base event have the
 *       map's own picture, by kind and team: which sheet, which cell; the
 *       picture of a marker of several events; when it is drawn sharp.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lv_event_overlay.h"
#include "lv_team_sprite.h"
#include "test_harness.h"

static bool ovl_near(float a, float b) {
    float d = a - b;
    if (d < 0.0f) d = -d;
    return d < 0.001f;
}

int run_lv_event_overlay_slider(void) {
    /* A slider frame from 100 to 500 with ImGui's default 12 px handle: the
       middle of the handle moves from 100 + 2 + 6 to 500 - 2 - 6. */
    const float x0 = 100.0f, x1 = 500.0f, grab = 12.0f;

    UT_ASSERT_MSG(ovl_near(lvOverlayGrabSize(x0, x1, grab), 12.0f), "handle width");
    UT_ASSERT_MSG(ovl_near(lvOverlaySliderX(x0, x1, grab, 0.0f), 108.0f),
                  "start: %f", lvOverlaySliderX(x0, x1, grab, 0.0f));
    UT_ASSERT_MSG(ovl_near(lvOverlaySliderX(x0, x1, grab, 1.0f), 492.0f),
                  "end: %f", lvOverlaySliderX(x0, x1, grab, 1.0f));
    UT_ASSERT_MSG(ovl_near(lvOverlaySliderX(x0, x1, grab, 0.5f), 300.0f),
                  "middle: %f", lvOverlaySliderX(x0, x1, grab, 0.5f));
    UT_ASSERT_MSG(ovl_near(lvOverlaySliderX(x0, x1, grab, 0.25f), 204.0f),
                  "quarter: %f", lvOverlaySliderX(x0, x1, grab, 0.25f));
    /* A value off the slider is held at its end. */
    UT_ASSERT_MSG(ovl_near(lvOverlaySliderX(x0, x1, grab, -2.0f), 108.0f), "under 0");
    UT_ASSERT_MSG(ovl_near(lvOverlaySliderX(x0, x1, grab, 7.0f), 492.0f), "over 1");

    /* A frame with no room for the handle: the handle is what fits, and
       every time is in the middle of the frame. */
    UT_ASSERT_MSG(ovl_near(lvOverlayGrabSize(0.0f, 10.0f, grab), 6.0f), "cut handle");
    UT_ASSERT_MSG(ovl_near(lvOverlaySliderX(0.0f, 10.0f, grab, 0.0f), 5.0f), "narrow, start");
    UT_ASSERT_MSG(ovl_near(lvOverlaySliderX(0.0f, 10.0f, grab, 1.0f), 5.0f), "narrow, end");
    UT_ASSERT_MSG(ovl_near(lvOverlayGrabSize(0.0f, 3.0f, grab), 0.0f), "no room at all");
    UT_ASSERT_MSG(ovl_near(lvOverlaySliderX(0.0f, 3.0f, grab, 0.5f), 1.5f), "3 px frame");

    /* The value of a time: the whole file. */
    UT_ASSERT_MSG(ovl_near(lvOverlayRatio(0, 0, 60000), 0.0f), "first ms");
    UT_ASSERT_MSG(ovl_near(lvOverlayRatio(60000, 0, 60000), 1.0f), "last ms");
    UT_ASSERT_MSG(ovl_near(lvOverlayRatio(30000, 0, 60000), 0.5f), "half way");
    UT_ASSERT_MSG(ovl_near(lvOverlayRatio(90000, 0, 60000), 1.0f), "past the end");
    /* With the lobby hidden the shown stretch starts at the game start. */
    UT_ASSERT_MSG(ovl_near(lvOverlayRatio(20000, 20000, 40000), 0.0f), "game start");
    UT_ASSERT_MSG(ovl_near(lvOverlayRatio(5000, 20000, 40000), 0.0f), "in the lobby");
    UT_ASSERT_MSG(ovl_near(lvOverlayRatio(40000, 20000, 40000), 0.5f), "half the game");
    UT_ASSERT_MSG(ovl_near(lvOverlayRatio(60000, 20000, 40000), 1.0f), "end of the game");
    /* Nothing loaded. */
    UT_ASSERT_MSG(ovl_near(lvOverlayRatio(500, 0, 0), 0.0f), "no length");
    /* A log of more than 24 days still gives a value in 0..1. */
    UT_ASSERT_MSG(ovl_near(lvOverlayRatio(0xFFFFFFFFu, 0, 0xFFFFFFFFu), 1.0f), "longest log");

    /* The two together: an event half way through is under the handle at
       half way. */
    UT_ASSERT_MSG(ovl_near(lvOverlaySliderX(x0, x1, grab, lvOverlayRatio(40000, 20000, 40000)),
                       300.0f), "time to x");
    return 0;
}

int run_lv_event_overlay_clusters(void) {
    LvOverlayCluster c[8];
    int n;

    /* No events. */
    UT_ASSERT_MSG(lvOverlayClusters(NULL, 0, 13.0f, c, 8) == 0, "no list");
    {
        static const float none[1] = {0.0f};
        UT_ASSERT_MSG(lvOverlayClusters(none, 0, 13.0f, c, 8) == 0, "empty list");
    }

    /* Far apart: one marker each. */
    {
        static const float xs[] = {10.0f, 40.0f, 80.0f};
        n = lvOverlayClusters(xs, 3, 13.0f, c, 8);
        UT_ASSERT_MSG(n == 3, "apart: %d", n);
        UT_ASSERT_MSG(c[1].first == 1 && c[1].count == 1 && ovl_near(c[1].x, 40.0f), "apart, second");
    }

    /* Three in one marker width, then one exactly a width from the first
       (not nearer than a width: its own marker), then two together. */
    {
        static const float xs[] = {10.0f, 10.0f, 22.9f, 23.0f, 100.0f, 104.0f};
        n = lvOverlayClusters(xs, 6, 13.0f, c, 8);
        UT_ASSERT_MSG(n == 3, "mixed: %d", n);
        UT_ASSERT_MSG(c[0].first == 0 && c[0].count == 3 && ovl_near(c[0].x, 10.0f), "first marker");
        UT_ASSERT_MSG(c[1].first == 3 && c[1].count == 1 && ovl_near(c[1].x, 23.0f), "second marker");
        UT_ASSERT_MSG(c[2].first == 4 && c[2].count == 2 && ovl_near(c[2].x, 100.0f), "third marker");
        /* No two markers are nearer than a width. */
        UT_ASSERT_MSG(c[1].x - c[0].x >= 13.0f && c[2].x - c[1].x >= 13.0f, "markers overlap");
    }

    /* A chain of events 5 apart is not one long marker: each marker takes
       a width's worth. */
    {
        float xs[20];
        int i, total = 0;
        for (i = 0; i < 20; i++) xs[i] = 5.0f * (float)i;
        n = lvOverlayClusters(xs, 20, 13.0f, c, 8);
        UT_ASSERT_MSG(n == 7, "chain: %d", n);
        for (i = 0; i < n; i++) total += c[i].count;
        UT_ASSERT_MSG(total == 20, "chain holds %d events", total);
        UT_ASSERT_MSG(c[0].count == 3 && c[6].count == 2, "chain sizes");
        /* No room for all the markers: the first ones are kept. */
        n = lvOverlayClusters(xs, 20, 13.0f, c, 2);
        UT_ASSERT_MSG(n == 2 && c[1].first == 3, "cut list: %d", n);
    }

    /* Everything at one place (a slider with no room). */
    {
        static const float xs[] = {5.0f, 5.0f, 5.0f, 5.0f};
        n = lvOverlayClusters(xs, 4, 13.0f, c, 8);
        UT_ASSERT_MSG(n == 1 && c[0].count == 4, "one place: %d", n);
    }

    /* The marker a point is on. */
    {
        static const LvOverlayCluster m[] = {{10.0f, 0, 1}, {23.0f, 1, 2}, {100.0f, 3, 1}};
        UT_ASSERT_MSG(lvOverlayHit(m, 3, 10.0f, 6.5f) == 0, "on the first");
        UT_ASSERT_MSG(lvOverlayHit(m, 3, 3.5f, 6.5f) == 0, "its left edge");
        UT_ASSERT_MSG(lvOverlayHit(m, 3, 3.0f, 6.5f) == -1, "left of it");
        /* Between two that touch: the nearer one. */
        UT_ASSERT_MSG(lvOverlayHit(m, 3, 16.0f, 6.5f) == 0, "nearer the first");
        UT_ASSERT_MSG(lvOverlayHit(m, 3, 17.0f, 6.5f) == 1, "nearer the second");
        UT_ASSERT_MSG(lvOverlayHit(m, 3, 60.0f, 6.5f) == -1, "in a gap");
        UT_ASSERT_MSG(lvOverlayHit(m, 3, 106.0f, 6.5f) == 2, "on the last");
        UT_ASSERT_MSG(lvOverlayHit(m, 0, 10.0f, 6.5f) == -1, "no markers");
        UT_ASSERT_MSG(lvOverlayHit(NULL, 3, 10.0f, 6.5f) == -1, "no list");
    }

    /* The team of a marker. */
    {
        int counts[LV_OVERLAY_TEAMS];
        memset(counts, 0, sizeof(counts));
        UT_ASSERT_MSG(lvOverlayMajorityTeam(counts) == -1, "no events");
        counts[LV_OVERLAY_TEAMS - 1] = 9;
        UT_ASSERT_MSG(lvOverlayMajorityTeam(counts) == -1, "only events with no team");
        counts[4] = 1;
        UT_ASSERT_MSG(lvOverlayMajorityTeam(counts) == 4, "one team");
        counts[2] = 3;
        UT_ASSERT_MSG(lvOverlayMajorityTeam(counts) == 2, "the larger team");
        counts[4] = 3;
        UT_ASSERT_MSG(lvOverlayMajorityTeam(counts) == 2, "level: the lower number");
        UT_ASSERT_MSG(lvOverlayMajorityTeam(NULL) == -1, "no counts");
    }
    return 0;
}

int run_lv_event_overlay_choice(void) {
    char text[256];
    int kind;

    /* Each kind alone, and every set of kinds, comes back as it went. */
    for (kind = 0; kind < LV_EVK_COUNT; kind++) {
        lvOverlayMaskToText(1u << kind, text, sizeof(text));
        UT_ASSERT_MSG(strcmp(text, lvEventKindKey(kind)) == 0, "kind %d is \"%s\"", kind, text);
        UT_ASSERT_MSG(lvOverlayMaskFromText(text) == (1u << kind), "kind %d reads back", kind);
    }
    {
        unsigned mask;
        for (mask = 0; mask <= LV_OVERLAY_ALL_MASK; mask++) {
            lvOverlayMaskToText(mask, text, sizeof(text));
            UT_ASSERT_MSG(lvOverlayMaskFromText(text) == mask, "set %u is \"%s\"", mask, text);
        }
    }

    /* The default: what changes the score. */
    lvOverlayMaskToText(LV_OVERLAY_DEFAULT_MASK, text, sizeof(text));
    UT_ASSERT_MSG(strcmp(text, "pill_captured,pill_stolen,base_captured,base_stolen,"
                               "builder_lost,tank_killed") == 0, "default is \"%s\"", text);

    /* An empty choice has a name of its own and stays empty. */
    lvOverlayMaskToText(0, text, sizeof(text));
    UT_ASSERT_MSG(strcmp(text, LV_OVERLAY_KEY_NONE) == 0, "empty is \"%s\"", text);
    UT_ASSERT_MSG(lvOverlayMaskFromText(LV_OVERLAY_KEY_NONE) == 0, "none reads as a kind");
    UT_ASSERT_MSG(lvOverlayMaskFromText("") == 0, "empty text");
    UT_ASSERT_MSG(lvOverlayMaskFromText(NULL) == 0, "no text");

    /* Names from a newer build are ignored; the ones known are kept. */
    UT_ASSERT_MSG(lvOverlayMaskFromText("chat,pill_placed,tank_killed") ==
                  ((1u << LV_EVK_CHAT) | (1u << LV_EVK_TANK_KILLED)), "unknown name");
    UT_ASSERT_MSG(lvOverlayMaskFromText("pill_placed") == 0, "only an unknown name");
    /* "all" is the Events panel's filter, not a kind. */
    UT_ASSERT_MSG(lvOverlayMaskFromText("all") == 0, "all reads as a kind");
    /* Spaces, empty parts and a name twice. */
    UT_ASSERT_MSG(lvOverlayMaskFromText(" chat , ,game,,chat,") ==
                  ((1u << LV_EVK_CHAT) | (1u << LV_EVK_GAME)), "untidy text");
    /* A part of a name is not the name. */
    UT_ASSERT_MSG(lvOverlayMaskFromText("cha,tank") == 0, "half a name");
    /* A name longer than any kind's. */
    UT_ASSERT_MSG(lvOverlayMaskFromText("a_very_long_name_that_no_kind_of_event_has_at_all,chat") ==
                  (1u << LV_EVK_CHAT), "long name");

    /* A buffer too small holds whole names only. */
    {
        char small[14];
        lvOverlayMaskToText((1u << LV_EVK_CHAT) | (1u << LV_EVK_GAME) | (1u << LV_EVK_PLAYER),
                            small, sizeof(small));
        UT_ASSERT_MSG(strcmp(small, "player,chat") == 0, "small buffer: \"%s\"", small);
        lvOverlayMaskToText(1u << LV_EVK_PILL_CAPTURED, small, 4);
        UT_ASSERT_MSG(strcmp(small, "non") == 0 || small[0] == '\0', "tiny buffer: \"%s\"", small);
    }
    return 0;
}

/* The sheet and the cell of the marker of an event of this kind, done by a
   team with this palette entry (hasTeam false: no team). */
static LvMapItemSprite eo_marker(int kind, bool hasTeam, int palette) {
    return lvPanelItemSprite(lvOverlayKindPicture(kind) == LV_OVERLAY_PIC_BASE, hasTeam, palette);
}

int run_lv_event_overlay_picture(void) {
    LvMapItemSprite s;
    int kind, palette;

    /* Which kinds have a picture. */
    UT_ASSERT_MSG(lvOverlayKindPicture(LV_EVK_PILL_CAPTURED) == LV_OVERLAY_PIC_PILL, "pill captured");
    UT_ASSERT_MSG(lvOverlayKindPicture(LV_EVK_PILL_STOLEN) == LV_OVERLAY_PIC_PILL, "pill stolen");
    UT_ASSERT_MSG(lvOverlayKindPicture(LV_EVK_BASE_CAPTURED) == LV_OVERLAY_PIC_BASE, "base captured");
    UT_ASSERT_MSG(lvOverlayKindPicture(LV_EVK_BASE_STOLEN) == LV_OVERLAY_PIC_BASE, "base stolen");
    for (kind = LV_EVK_BUILDER_LOST; kind < LV_EVK_COUNT; kind++) {
        UT_ASSERT_MSG(lvOverlayKindPicture(kind) == LV_OVERLAY_PIC_NONE, "the others keep an icon");
    }
    UT_ASSERT_MSG(lvOverlayKindPicture(-1) == LV_OVERLAY_PIC_NONE, "no kind: no picture");

    /* A team's pillbox: its row of items.bmp, the column of full armour. */
    for (palette = 0; palette < LV_TEAM_SHEET_ROWS; palette++) {
        int row = (palette == LV_TEAM_PALETTE_RED) ? LV_TEAM_SHEET_ROW_ORANGE : palette;
        s = eo_marker(LV_EVK_PILL_CAPTURED, true, palette);
        UT_ASSERT_MSG(!s.fromAtlas && s.col == 15 && s.row == row, "a team's pillbox, whole");
        s = eo_marker(LV_EVK_PILL_STOLEN, true, palette);
        UT_ASSERT_MSG(!s.fromAtlas && s.col == 15 && s.row == row, "stolen: the same picture");
        s = eo_marker(LV_EVK_BASE_CAPTURED, true, palette);
        UT_ASSERT_MSG(!s.fromAtlas && s.col == LV_ITEMS_COL_BASE && s.row == row, "a team's base");
        s = eo_marker(LV_EVK_BASE_STOLEN, true, palette);
        UT_ASSERT_MSG(!s.fromAtlas && s.col == LV_ITEMS_COL_BASE && s.row == row, "stolen: the same");
        /* The same cell the map draws for that team. */
        UT_ASSERT_MSG(s.row == lvMapBaseSprite(false, palette).row, "the map's own row");
    }
    /* A red team is orange here as on the map. */
    s = eo_marker(LV_EVK_PILL_CAPTURED, true, LV_TEAM_PALETTE_RED);
    UT_ASSERT_MSG(s.row == LV_TEAM_SHEET_ROW_ORANGE, "no team is drawn in the red row");

    /* No team: the game's own tiles. The red pillbox, the neutral base. */
    s = eo_marker(LV_EVK_PILL_CAPTURED, false, 3);
    UT_ASSERT_MSG(s.fromAtlas && s.tile == LV_TILE_PILL_EVIL_15, "the game's red pillbox, whole");
    s = eo_marker(LV_EVK_BASE_CAPTURED, false, 3);
    UT_ASSERT_MSG(s.fromAtlas && s.tile == LV_TILE_BASE_NEUTRAL, "the game's neutral base");

    /* A marker of several events. */
    {
        int pills[3] = { LV_EVK_PILL_CAPTURED, LV_EVK_PILL_STOLEN, LV_EVK_PILL_CAPTURED };
        int bases[2] = { LV_EVK_BASE_STOLEN, LV_EVK_BASE_CAPTURED };
        int mixed[2] = { LV_EVK_PILL_CAPTURED, LV_EVK_BASE_CAPTURED };
        int other[2] = { LV_EVK_PILL_CAPTURED, LV_EVK_CHAT };
        int chat[2] = { LV_EVK_CHAT, LV_EVK_CHAT };
        UT_ASSERT_MSG(lvOverlayClusterPicture(pills, 3) == LV_OVERLAY_PIC_PILL, "only pillboxes");
        UT_ASSERT_MSG(lvOverlayClusterPicture(bases, 2) == LV_OVERLAY_PIC_BASE, "only bases");
        UT_ASSERT_MSG(lvOverlayClusterPicture(mixed, 2) == LV_OVERLAY_PIC_NONE, "both: none");
        UT_ASSERT_MSG(lvOverlayClusterPicture(other, 2) == LV_OVERLAY_PIC_NONE, "with chat: none");
        UT_ASSERT_MSG(lvOverlayClusterPicture(chat, 2) == LV_OVERLAY_PIC_NONE, "no picture kinds");
        UT_ASSERT_MSG(lvOverlayClusterPicture(pills, 0) == LV_OVERLAY_PIC_NONE, "none at all");
    }

    /* Sharp at a whole number of times 16 pixels, blended at any other. */
    UT_ASSERT_MSG(lvItemPictureSharp(16.0f) && lvItemPictureSharp(32.0f) && lvItemPictureSharp(48.0f),
                  "16, 32, 48: sharp");
    UT_ASSERT_MSG(!lvItemPictureSharp(12.0f) && !lvItemPictureSharp(13.0f) && !lvItemPictureSharp(24.0f) &&
                  !lvItemPictureSharp(26.0f) && !lvItemPictureSharp(8.0f) && !lvItemPictureSharp(19.5f),
                  "any other size: blended");
    return 0;
}

/* The one list of ticked kinds the Controls panel ("Overlay events") and
   the Events panel (its filter) both use: which events a choice shows. */
int run_lv_event_kinds_filter(void) {
    int counts[LV_EVK_COUNT];
    int kind;
    unsigned mask;

    /* One bit a kind, in the order of the list. */
    for (kind = 0; kind < LV_EVK_COUNT; kind++) {
        UT_ASSERT_MSG(lvEventKindTicked(LV_OVERLAY_ALL_MASK, kind), "all: kind %d is shown", kind);
        UT_ASSERT_MSG(!lvEventKindTicked(0u, kind), "none: kind %d is not", kind);
        UT_ASSERT_MSG(lvEventKindTicked(1u << kind, kind), "its own bit: kind %d", kind);
        UT_ASSERT_MSG(!lvEventKindTicked(LV_OVERLAY_ALL_MASK & ~(1u << kind), kind),
                      "every kind but %d", kind);
    }
    UT_ASSERT_MSG(LV_EVK_COUNT == 10, "ten kinds (%d)", (int)LV_EVK_COUNT);
    /* A kind out of range is never shown, whatever the bits say. */
    UT_ASSERT_MSG(!lvEventKindTicked(0xFFFFFFFFu, -1), "kind -1");
    UT_ASSERT_MSG(!lvEventKindTicked(0xFFFFFFFFu, LV_EVK_COUNT), "one past the last kind");
    UT_ASSERT_MSG(!lvEventKindTicked(0xFFFFFFFFu, 40), "past the bits of the mask");

    /* A list of events through a choice: the rows of the Events panel, the
       markers of the lane. */
    {
        const int events[8] = { LV_EVK_GAME, LV_EVK_TANK_KILLED, LV_EVK_CHAT, LV_EVK_TANK_KILLED,
                                LV_EVK_PILL_CAPTURED, LV_EVK_CHAT, LV_EVK_HIGHLIGHT, LV_EVK_TANK_KILLED };
        int shown[8];
        int n = 0, i;
        mask = (1u << LV_EVK_TANK_KILLED) | (1u << LV_EVK_PILL_CAPTURED);
        for (i = 0; i < 8; i++) {
            if (lvEventKindTicked(mask, events[i])) shown[n++] = i;
        }
        UT_ASSERT_MSG(n == 4 && shown[0] == 1 && shown[1] == 3 && shown[2] == 4 && shown[3] == 7,
                      "tank kills and pillboxes, in the order of the list (%d)", n);
        memset(counts, 0, sizeof(counts));
        for (i = 0; i < 8; i++) counts[events[i]]++;
        UT_ASSERT_MSG(lvEventKindsTickedCount(mask, counts) == 4, "the number on the closed list");
        UT_ASSERT_MSG(lvEventKindsTickedCount(LV_OVERLAY_ALL_MASK, counts) == 8, "all of them");
        UT_ASSERT_MSG(lvEventKindsTickedCount(0u, counts) == 0, "none ticked");
        UT_ASSERT_MSG(lvEventKindsTickedCount(1u << LV_EVK_PLAYER, counts) == 0,
                      "a kind the log has no event of: an empty list");
        UT_ASSERT_MSG(lvEventKindsTickedCount(mask, NULL) == 0, "no counts");
    }

    /* What the closed filter says in the room it has: a combo adds 8 of
       padding and an arrow of 19 to its text. "All events (412)" is 112
       wide, "(412)" 35. */
    UT_ASSERT_MSG(lvEventsLabelPick(139.0f, 112.0f, 35.0f, 27.0f) == LV_EVENTS_LABEL_FULL, "exact fit");
    UT_ASSERT_MSG(lvEventsLabelPick(138.0f, 112.0f, 35.0f, 27.0f) == LV_EVENTS_LABEL_COUNT, "one short");
    UT_ASSERT_MSG(lvEventsLabelPick(94.0f, 112.0f, 35.0f, 27.0f) == LV_EVENTS_LABEL_COUNT,
                  "the narrowest sidebar (110 less its padding)");
    UT_ASSERT_MSG(lvEventsLabelPick(62.0f, 112.0f, 35.0f, 27.0f) == LV_EVENTS_LABEL_COUNT, "the number, exact");
    UT_ASSERT_MSG(lvEventsLabelPick(61.0f, 112.0f, 35.0f, 27.0f) == LV_EVENTS_LABEL_ARROW, "the arrow alone");
    UT_ASSERT_MSG(lvEventsLabelPick(0.0f, 112.0f, 35.0f, 27.0f) == LV_EVENTS_LABEL_ARROW, "no room");
    return 0;
}

/* The Events panel's own choice in the preferences ("Events.Kinds"), and the
   old one-kind filter ("Events.Filter") taken over when no list was saved. */
int run_lv_event_kinds_prefs(void) {
    char text[256];
    unsigned mask;
    int kind;

    /* Written and read back: every choice of one kind, all, none, a mix. */
    for (kind = 0; kind < LV_EVK_COUNT; kind++) {
        lvOverlayMaskToText(1u << kind, text, sizeof(text));
        UT_ASSERT_MSG(lvEventsMaskFromPrefs(text, "all") == (1u << kind), "kind %d alone (%s)", kind, text);
    }
    lvOverlayMaskToText(LV_OVERLAY_ALL_MASK, text, sizeof(text));
    UT_ASSERT_MSG(lvEventsMaskFromPrefs(text, "chat") == LV_OVERLAY_ALL_MASK, "all, read back");
    mask = (1u << LV_EVK_TANK_KILLED) | (1u << LV_EVK_CHAT) | (1u << LV_EVK_HIGHLIGHT);
    lvOverlayMaskToText(mask, text, sizeof(text));
    UT_ASSERT_MSG(lvEventsMaskFromPrefs(text, "all") == mask, "a mix, read back (%s)", text);
    /* None is a choice too, and is kept: it is not "nothing saved". */
    lvOverlayMaskToText(0u, text, sizeof(text));
    UT_ASSERT_MSG(strcmp(text, "none") == 0, "none is written as a word");
    UT_ASSERT_MSG(lvEventsMaskFromPrefs(text, "all") == 0u, "none, read back");
    UT_ASSERT_MSG(lvEventsMaskFromPrefs(text, lvEventKindKey(LV_EVK_CHAT)) == 0u,
                  "a saved list wins over the old filter");

    /* Nothing saved: every kind, the default. */
    UT_ASSERT_MSG(lvEventsMaskFromPrefs("", "all") == LV_OVERLAY_ALL_MASK, "a new install");
    UT_ASSERT_MSG(lvEventsMaskFromPrefs(NULL, NULL) == LV_OVERLAY_ALL_MASK, "no text at all");
    UT_ASSERT_MSG(lvEventsMaskFromPrefs("", "") == LV_OVERLAY_ALL_MASK, "an empty old filter");
    /* Nothing saved but the old filter: its one kind alone. */
    for (kind = 0; kind < LV_EVK_COUNT; kind++) {
        UT_ASSERT_MSG(lvEventsMaskFromPrefs("", lvEventKindKey(kind)) == (1u << kind),
                      "the old filter of kind %d", kind);
        UT_ASSERT_MSG(lvEventsMaskFromPrefs(NULL, lvEventKindKey(kind)) == (1u << kind),
                      "the old filter of kind %d, no list", kind);
    }
    UT_ASSERT_MSG(lvEventsMaskFromPrefs("", "a_newer_kind") == LV_OVERLAY_ALL_MASK,
                  "an old filter this build does not know: all");

    /* The panel's choice and the one of "Overlay events" are two texts: one
       does not read as the other's default. */
    UT_ASSERT_MSG(lvEventsMaskFromPrefs("", "all") != LV_OVERLAY_DEFAULT_MASK,
                  "the panel's default is all, not the lane's six");
    return 0;
}
