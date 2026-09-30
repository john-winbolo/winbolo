/*
 * lv_presentation.h - a recording's scenario panels, scores, announcement
 * and map markers at the playhead
 *
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * What the recording's log_ScnPanel, log_ScnScore, log_ScnAnnounce and
 * log_ScnMarker records say at the playhead (layouts in
 * docs/replay-format.md), the slot teams log_TeamSet gives, and the accessors the viewer's drawing code reads
 * them through, with the server's game tick those records count in.
 *
 * Plain C with nothing but the public scenario_panel.h behind it, so a draw
 * can read these without backend.h, as lv_scripts.h does for the scripts.
 * logviewer.h includes it for everything else.
 */

#ifndef LV_PRESENTATION_H
#define LV_PRESENTATION_H

#include <stdbool.h>
#include <stdint.h>
#include "scenario_panel.h"   /* SCN_PANEL_MAX, SCN_MARKERS_MAX, MAX_TANKS */

#ifdef __cplusplus
extern "C" {
#endif

/* Team numbers run 1..LV_PRES_TEAMS-1. Team 0 is no team: as a destination
   it means everyone, and it has no score. */
#define LV_PRES_TEAMS         MAX_TANKS

/* Panel rows, the same rows the server keeps a panel in: row 0 is everyone,
   rows 1..15 are the teams and rows MAX_TANKS.. are the player slots. */
#define LV_PRES_PANEL_ROWS    (LV_PRES_TEAMS + MAX_TANKS)

/* A score's label, fifteen bytes and the terminator. */
#define LV_PRES_LABEL_LEN     16

/* The longest announcement line a record carries. */
#define LV_PRES_ANNOUNCE_MAX  128

/* One panel's display list for one audience, as the record carried it. The
   bytes have been through scnPanelParse once already; a draw parses them
   again into its own ScnPanelList. set is false for a row no record has
   filled and for one a record cleared. written is true once any record has
   landed on the row, a clear included, and ms is the log time the last one
   landed at: the client keeps one list and the newest record addressed to
   the player replaces it, so the draw picks between rows by ms. */
typedef struct {
  bool     set;
  bool     written;
  uint32_t ms;
  uint16_t len;
  uint8_t  bytes[SCN_PANEL_MAX];
} LvPresPanelRow;

/* A scenario's score for one player slot or one team. */
typedef struct {
  bool    valid;
  int32_t score;
  char    label[LV_PRES_LABEL_LEN];   /* always terminated */
} LvPresScore;

/* The centre-screen line. ms is the absolute log time the record landed at
   and ticks how long the server held it up for. */
typedef struct {
  bool     set;
  uint32_t ms;
  uint16_t ticks;
  BYTE     destTeam;
  BYTE     destPlayer;
  char     text[LV_PRES_ANNOUNCE_MAX + 1];   /* always terminated */
} LvPresAnnounce;

/* One destination's status line, kept per destination the way the panel
   rows are and chosen between the same way: set is false for a row no record
   has filled and for one a record cleared, written is true once any record
   has landed on the row, and ms is the log time the last one landed at.
   endsAt is the server tick the countdown runs to, or
   SCN_STATUS_NO_COUNTDOWN for none. */
typedef struct {
  bool     set;
  bool     written;
  uint32_t ms;
  uint32_t endsAt;
  char     text[LV_PRES_ANNOUNCE_MAX + 1];   /* always terminated */
} LvPresStatus;

/* One map marker, kept by id. kind is SCN_MARKER_KIND_SQUARE, which reads x
   and y, or SCN_MARKER_KIND_FOLLOW, which reads slot. A marker stays until a
   record clears its id. */
typedef struct {
  bool set;
  BYTE kind;
  BYTE destTeam;
  BYTE destPlayer;
  BYTE x, y, slot, colour;
} LvPresMarker;

typedef struct LvPresentation {
  LvPresPanelRow panels[LV_PRES_PANEL_ROWS];
  LvPresScore    playerScores[MAX_TANKS];
  LvPresScore    teamScores[LV_PRES_TEAMS];   /* [0] is never valid */
  LvPresAnnounce announce;
  LvPresMarker   markers[SCN_MARKERS_MAX];
  LvPresStatus   status[LV_PRES_PANEL_ROWS];   /* rows as the panels' */
  /* Each slot's lobby team as log_TeamSet last set it, 0 for no team. A
     slot no record has named is not known, and the team panels are then
     never drawn for it. This is the team a scenario addresses, which is not
     the alliance group lv_playersGetTeamId answers. */
  BYTE           team[MAX_TANKS];
  bool           teamKnown[MAX_TANKS];
} LvPresentation;

/* The stores at the playhead. Each answers NULL for a key out of range and
 * while no decoder exists; a pointer it answers stays valid until the next
 * tick, seek or load.
 *
 * lv_screenGetPanelRow takes the destination pair a record carries: (0, 0xFF)
 * for everyone, (team, 0xFF) for a team and a slot in destPlayer for one
 * player. lv_screenGetScore takes a ScnScoreKind and a slot or a team number.
 * lv_screenGetMarker takes a marker id, 0..SCN_MARKERS_MAX-1. */
const LvPresPanelRow *lv_screenGetPanelRow(BYTE destTeam, BYTE destPlayer);
const LvPresScore    *lv_screenGetScore(BYTE kind, BYTE target);
const LvPresAnnounce *lv_screenGetAnnounce(void);
const LvPresMarker   *lv_screenGetMarker(BYTE id);

/* The status line row for a destination pair, taken the way
 * lv_screenGetPanelRow takes one. */
const LvPresStatus   *lv_screenGetStatusRow(BYTE destTeam, BYTE destPlayer);

/* The status line to draw for the followed player at the playhead, chosen
 * from the everyone row, their team's row and their slot's row by the rule
 * lv_screenChoosePanelRow follows. NULL when there is nothing to draw. */
const LvPresStatus   *lv_screenFollowedStatus(void);

/* A slot's lobby team at the playhead, 0 for no team. False, with *team
 * left alone, for a slot out of range or one no log_TeamSet has named. */
bool lv_screenGetSlotTeam(BYTE slot, BYTE *team);

/* The player the viewer is following: the game view's camera tank, or the
 * player the overview takes its view from. A value of MAX_TANKS or more is
 * nobody. */
BYTE lv_screenFollowedSlot(void);

/* The panel row to draw, of the everyone row, the followed player's team row
 * and their slot row; a row that is not a candidate is passed as NULL. Of
 * the rows a record has written, the one with the greatest ms wins, and on
 * equal ms the slot row beats the team row, which beats the everyone row.
 * NULL when no candidate has been written or when the winner is a clear.
 * Reads nothing but its arguments. */
const LvPresPanelRow *lv_screenChoosePanelRow(const LvPresPanelRow *everyone,
                                              const LvPresPanelRow *team,
                                              const LvPresPanelRow *slot);

/* lv_screenChoosePanelRow over the followed player's three rows at the
 * playhead. NULL when there is nothing to draw. */
const LvPresPanelRow *lv_screenFollowedPanelRow(void);

/* Whether a marker is shown to the player in followedSlot: one to everyone
 * always is; one to a slot only to that slot; one to a team only to a slot
 * whose team log_TeamSet has named and matches. A pair naming both a slot and
 * a team is taken as the slot, as the panel rows are. False for a NULL or
 * unset marker and, for an addressed marker, for a followedSlot of MAX_TANKS
 * or more. Reads the marker and the slot teams and nothing else. */
bool lv_screenMarkerVisible(const LvPresMarker *m, BYTE followedSlot);

/* Where marker id is drawn for the followed player at the playhead, as a map
 * square, and its palette colour. A square marker is at its own (x, y); a
 * follow marker is on the square of its slot's tank, and is not drawn while
 * that slot has no tank on the map. False when there is nothing to draw. */
bool lv_screenMarkerPlace(BYTE id, BYTE *mx, BYTE *my, BYTE *colour);

/* The server's game tick at playback time ms: the clock a scenario's timer
 * target and an announcement's arrival are counted in, a hundred a second and
 * reset each round. Worked out from the recording's log_ServerTick records
 * and the writer ticks between them (screen.c's server-tick section). A
 * recording with none of those records answers ms / 10, which is right only
 * as far as playback time and the server's clock agree.
 *
 * lv_screenHasServerTick answers whether the recording has given at least one
 * such record, so a caller can tell the recorded clock from that estimate. */
uint32_t lv_screenServerTickAt(uint32_t ms);
bool     lv_screenHasServerTick(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* LV_PRESENTATION_H */
