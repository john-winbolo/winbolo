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
 * Name:          lobby_recap.cpp
 * Purpose:       The between-rounds "Last round" panel,
 *                built from the client's stored end-of-round
 *                summary: the sortable scoreboard table with
 *                its sprite column headers, the per-slot name
 *                and team-tint helpers the rows and the awards
 *                share, the award and highlight label lookups,
 *                the collapsible highlight-clip section with
 *                its seek and clip-export controls, and the
 *                awards ribbon with its pinned leaders and the
 *                handful drawn from the summary's own bytes.
 *                Also the panel's two view toggles, the inline
 *                lock badge shown beside a server-locked
 *                setting, and the skip-map vote button with
 *                its running count.
 *********************************************************/

#include <cstdio>   /* snprintf — highlight header count, per-row clip ids */
#include <cstring>  /* memset — MessageArgs for the skip-vote count */

#include <SDL3/SDL.h>

#include "imgui.h"
#include "lobby_internal.h"
#include "../../wb_theme.h"  /* g_theme / wbThemeColor — team tints, lock badge */
extern "C" {
#include "client_sim.h"      /* ClientSim + lobby getters; MAX_TANKS, ClientLobbySlot */
#include "client_net.h"      /* clientSimNetSendMapSkipVote */
#include "../../../../bolo/public/round_stats_derive.h"  /* roundStatsPickAwardSubsetExcluding; pulls round_stats.h — POSTGAME_STATS_ENABLED, AWARD_*, HL_*, RoundStatsSummary */
#include "../../../../bolo/public/wire_limits.h"  /* LOBBY_LOCK_MAP */
#include "../../../lang.h"       /* langGetText / langGetTextFmt / MessageArgs / STR_*; PLAYER_FLAG_* */
#include "../../../gamefront.h"  /* gameFrontGetPlayerNum — your own scoreboard row */
#include "../../../tiles.h"      /* map sprites for the scoreboard column headers */
#include "../../sdl3imgui.h"     /* renderPlayerName / drawCountryFlagWithTip */
}

/* ── Layout A — "Last round" recap ────────────────────────────────
 * Between-rounds scoreboard + awards built from the client's stored
 * end-of-round summary (clientSimGetLastRoundStats). The summary is
 * set at game over and cleared on the next countdown; the recap body
 * below is shown only while it exists — via a button-triggered popup
 * on the mouse path and a tab in the controller layout. Read-only;
 * never blocks readying up or other lobby controls. */

/* AwardId → localized label id. */
static langid lastRoundAwardLabel(uint8_t awardId) {
    switch (awardId) {
        case AWARD_MOST_KILLS:         return STR_DLGLOBBY_AWARD_MOST_KILLS;
        case AWARD_MOST_DEATHS:        return STR_DLGLOBBY_AWARD_MOST_DEATHS;
        case AWARD_BEST_KD:            return STR_DLGLOBBY_AWARD_BEST_KD;
        case AWARD_MOST_BASE_CAPTURES: return STR_DLGLOBBY_AWARD_MOST_BASE;
        case AWARD_MOST_PILL_CAPTURES: return STR_DLGLOBBY_AWARD_MOST_PILL;
        case AWARD_NEMESIS:            return STR_DLGLOBBY_AWARD_NEMESIS;
        case AWARD_DEMOLITION:         return STR_DLGLOBBY_AWARD_DEMOLITION;
        case AWARD_SHARPSHOOTER:       return STR_DLGLOBBY_AWARD_SHARPSHOOTER;
        case AWARD_WARMONGER:          return STR_DLGLOBBY_AWARD_WARMONGER;
        case AWARD_SURVIVOR:           return STR_DLGLOBBY_AWARD_SURVIVOR;
        case AWARD_ENGINEER:           return STR_DLGLOBBY_AWARD_ENGINEER;
        case AWARD_SAPPER:             return STR_DLGLOBBY_AWARD_SAPPER;
        case AWARD_FISH_FOOD:          return STR_DLGLOBBY_AWARD_FISH_FOOD;
        case AWARD_LGM_HUNTER:         return STR_DLGLOBBY_AWARD_LGM_HUNTER;
        case AWARD_CANNON_FODDER:      return STR_DLGLOBBY_AWARD_CANNON_FODDER;
        case AWARD_LUMBERJACK:         return STR_DLGLOBBY_AWARD_LUMBERJACK;
        case AWARD_WASTEFUL:           return STR_DLGLOBBY_AWARD_WASTEFUL;
        case AWARD_BIGGEST_FUMBLE:     return STR_DLGLOBBY_AWARD_BIGGEST_FUMBLE;
        default:                       return STR_DLGLOBBY_AWARD_MOST_KILLS;
    }
}

/* Ratio awards carry value ×100 on the wire (see round_stats.h). */
static bool lastRoundAwardIsRatio(uint8_t awardId) {
    return awardId == AWARD_BEST_KD ||
           awardId == AWARD_SHARPSHOOTER ||
           awardId == AWARD_SURVIVOR;
}

/* A slot's team tint, mirroring the roster's team-color resolution
 * (clientSimGetLobbyTeamColor + theme palette). Unassigned/invalid
 * teams fall back to the default text color. */
static ImU32 lastRoundTeamTint(ClientSim *cs, uint8_t teamNumber) {
    if (teamNumber >= 1 && teamNumber <= 16) {
        uint8_t colorIdx = clientSimGetLobbyTeamColor(cs, (BYTE)teamNumber);
        if (clientSimGetLobbyTeamInUse(cs, (BYTE)teamNumber) && colorIdx < 8) {
            return g_theme->teamColors[colorIdx];
        }
        return g_theme->teamColors[(teamNumber - 1) & 7];
    }
    return ImGui::GetColorU32(ImGuiCol_Text);
}

/* Resolve and render a slot's name inline: bots use the bot-badge tint
 * plus the BOT tag (as in the roster); humans are tinted by team color.
 * Falls back to a placeholder when the slot has no name.
 *
 * Country flag and platform/WBN badges come first, on the same line, the
 * way the roster renders them. Both hang off the lobby slot, so a player
 * who left before the recap (no slot) simply gets the placeholder name on
 * its own — nothing to draw and nothing to misalign. */
static void lastRoundRenderName(ClientSim *cs, uint8_t slot, bool isBot) {
    const ClientLobbySlot *ls =
        (slot < MAX_TANKS) ? clientSimGetLobbySlot(cs, (BYTE)slot) : nullptr;
    const char *name = (ls && ls->playerName[0])
                           ? ls->playerName
                           : langGetText(STR_DLGLOBBY_LASTROUND_NOPLAYER);
    if (ls) {
        if (drawCountryFlagWithTip(ls->countryCode)) {
            ImGui::SameLine();
        }
        /* Badges only: the name itself is drawn below with its team tint.
         * Bots are skipped — the BOT tag already says what they are. The
         * WBN shield is meaningless off the network, so drop it there. */
        if (!isBot) {
            uint8_t pflags = ls->clientFlags;
            if (clientSimIsSinglePlayer(cs) || clientSimIsLanOnly(cs)) {
                pflags &= ~PLAYER_FLAG_WBN_VERIFIED;
            }
            renderPlayerName(NULL, pflags, ls->clientType, "", false);
        }
    }
    if (isBot) {
        ImGui::TextColored(wbThemeColor(g_theme->botBadge), "%s", name);
        ImGui::SameLine(0, 4.0f);
        ImGui::TextDisabled("%s", langGetText(STR_DLGLOBBY_TAG_BOT));
    } else {
        ImGui::TextColored(
            ImGui::ColorConvertU32ToFloat4(
                lastRoundTeamTint(cs, ls ? ls->teamNumber : 0)),
            "%s", name);
    }
}

/* Resolve a slot to a display name without rendering it, for the clip
 * lines that build a whole sentence through langGetTextFmt. Same
 * fallback as lastRoundRenderName. */
static const char *lastRoundSlotName(ClientSim *cs, uint8_t slot) {
    const ClientLobbySlot *ls =
        (slot < MAX_TANKS) ? clientSimGetLobbySlot(cs, (BYTE)slot) : nullptr;
    return (ls && ls->playerName[0])
               ? ls->playerName
               : langGetText(STR_DLGLOBBY_LASTROUND_NOPLAYER);
}

/* Highlight clip → localized line-format id. Some types choose between
 * two formats on the clip's own fields — a drowning with or without
 * carried pills, a team's swing with or without a slot to name — so this
 * takes the clip rather than just its type. Types the scorer does not
 * select yet land on the generic label. */
static langid lastRoundHighlightLabel(const HighlightWindow *h) {
    switch (h->type) {
        case HL_AWARD:
            return (h->awardId == AWARD_NEMESIS)
                       ? STR_DLGLOBBY_HL_AWARD_VS_FMT
                       : STR_DLGLOBBY_HL_AWARD_FMT;
        case HL_CLUSTER_WIPE:    return STR_DLGLOBBY_HL_WIPE_FMT;
        case HL_OBJECTIVE_STEAL: return STR_DLGLOBBY_HL_STEAL_FMT;
        case HL_MULTI_LGM:       return STR_DLGLOBBY_HL_LGM_FMT;
        case HL_FUMBLE:          return STR_DLGLOBBY_HL_FUMBLE_FMT;
        case HL_RARE_DEATH:
            return (h->value > 0) ? STR_DLGLOBBY_HL_DROWN_PILLS_FMT
                                  : STR_DLGLOBBY_HL_DROWN_FMT;
        case HL_PICKUP_SPREE:    return STR_DLGLOBBY_HL_PICKUP_FMT;
        /* A busy stretch of the round is nobody's, so its line names no
         * player — both actors are NEUTRAL on this type. */
        case HL_ACTION_DENSITY:  return STR_DLGLOBBY_HL_DENSITY_FMT;
        /* Ground taken off a team belongs to the team that took it;
         * actorA only names a slot standing in for it, and is NEUTRAL
         * when the gaining team has no one to point at. */
        case HL_BREAKTHROUGH:
            return (h->actorA < MAX_TANKS)
                       ? STR_DLGLOBBY_HL_COLLAPSE_FMT
                       : STR_DLGLOBBY_HL_COLLAPSE_NOACTOR_FMT;
        case HL_TURNING_POINT:
            return (h->actorA < MAX_TANKS)
                       ? STR_DLGLOBBY_HL_TURNING_FMT
                       : STR_DLGLOBBY_HL_TURNING_NOACTOR_FMT;
        default:                 return STR_DLGLOBBY_HL_GENERIC;
    }
}

/* Recap panel view state: the two per-summary view toggles the panel keeps
 * between rounds. Both belong to the summary on screen, so lobbyRecapReset()
 * puts them back whenever that summary goes away. */
typedef struct LobbyRecapState {
    /* Which of the two views the desktop map panel is showing between rounds.
     * False is the recap, where every new summary starts; the button at the top
     * of the panel flips it. */
    bool showMap        = false;

    /* Whether the highlight-clip list is expanded. Per-summary like showMap,
     * and closed to begin with: the clips are a place to go looking once
     * something in the round is worth finding again, and the replay above them
     * is what the recap is for. Folded away, the reel gets the rows' height. */
    bool showHighlights = false;
} LobbyRecapState;

static LobbyRecapState s_recap = {};

/* Core reads this to pick which view the right-hand panel shows, and flips it
 * from the button at the top of that panel. */
bool *lobbyRecapShowMap(void) {
    return &s_recap.showMap;
}

void lobbyRecapReset(void) {
    s_recap = LobbyRecapState{};
}

/* Is the right panel showing the replay reel right now? Exactly the negation
 * of the panel's own showMapPanel test (`!lobbyShowLastRound || s_recap.showMap`
 * at the ##MapPanel render): the reel is up when a last-round summary exists
 * AND the panel has not been flipped to its Map tab. With the recap withheld
 * at build level there is no reel, so this is constant false — matching the
 * same #if the lobby uses to compute lobbyShowLastRound. */
bool lobbyRecapReelVisible(ClientSim *cs) {
#if POSTGAME_STATS_ENABLED
    return (clientSimGetLastRoundStats(cs) != NULL) && !s_recap.showMap;
#else
    (void)cs;
    return false;
#endif
}

/* How many awards the recap draws at random, on top of the ones it always
 * leads with. A round can win all eighteen, and a list that long buries the
 * ones worth reading. */
static const int RECAP_AWARDS_RANDOM = 4;

/* Skull for the scoreboard's death columns, drawn square at text height and
 * tinted to the text colour so it sits with the other header art rather than
 * shouting. Returns false when the asset is missing, which is the caller's
 * cue to fall back to the column's written label. */
static bool lastRoundDrawSkull(void) {
    if (!lobbyIcons()->skull) return false;
    float sz = ImGui::GetTextLineHeight();
    ImGui::ImageWithBg((ImTextureID)lobbyIcons()->skull, ImVec2(sz, sz),
                       ImVec2(0, 0), ImVec2(1, 1),
                       ImVec4(0, 0, 0, 0),
                       ImGui::GetStyleColorVec4(ImGuiCol_Text));
    return true;
}

/* Container-less recap body, in reading order: the round's replay reel, its
 * highlight clips, the scoreboard table, then a handful of the round's awards.
 * Renders no chrome and decides nothing about visibility — the caller (the
 * desktop lobby's right column / the controller layout's Last round tab)
 * gates it on clientSimGetLastRoundStats and supplies the surrounding
 * container. */
void lobbyRenderLastRoundBody(ClientSim *cs, float s) {
    const RoundStatsSummary *st = clientSimGetLastRoundStats(cs);
    if (!st) {
#if !BOLO_MOBILE
        lobbyReelEnd();
#endif
        return;
    }

#if !BOLO_MOBILE
    /* Height of the container the body is about to fill, and where it starts,
     * so the tail of this function can see how much of it went unused and feed
     * that back into the reel. */
    const float bodyAvailH = ImGui::GetContentRegionAvail().y;
    const float bodyStartY = ImGui::GetCursorPosY();

    lobbyRenderReel(cs, st, s);
#endif

    /* ── Highlight clips ─────────────────────────────────────────── */
    /* Read-only list, in the order the server selected them (already
     * chronological). Each line is a round-relative timestamp plus a
     * one-phrase description.
     *
     * Behind an expand that starts closed, so the reel above gets the height
     * the rows would have taken and the scoreboard stays in view. The count
     * rides on the header because a closed section otherwise says nothing
     * about whether opening it is worth it — parenthesised digits after the
     * translated noun, not a sentence, so there is no new string to
     * translate. The ### keeps the widget's id off the changing count. */
    ImGui::Separator();
    if (st->highlightCount == 0) {
        /* A quiet round selects no clips — normal, not an error. Nothing to
         * fold away, so this stays the plain header it always was rather than
         * an expand that opens on one disabled line. */
        ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_HL_HEADER));
        ImGui::TextDisabled("%s", langGetText(STR_DLGLOBBY_HL_NONE));
    } else {
        int hc = st->highlightCount;
        if (hc > ROUND_STATS_HIGHLIGHTS_WIRE_MAX) {
            hc = ROUND_STATS_HIGHLIGHTS_WIRE_MAX;
        }
        char hlHeader[96];
        snprintf(hlHeader, sizeof(hlHeader), "%s (%d)###recapHighlights",
                 langGetText(STR_DLGLOBBY_HL_HEADER), hc);
        /* Driven from our own flag rather than ImGui's storage, so the next
         * round's recap starts closed again instead of inheriting this one's
         * state. */
        ImGui::SetNextItemOpen(s_recap.showHighlights, ImGuiCond_Always);
        s_recap.showHighlights = ImGui::CollapsingHeader(hlHeader);
        if (s_recap.showHighlights) {
            for (int i = 0; i < hc; i++) {
                const HighlightWindow *h = &st->highlights[i];
                /* The summary carries the clip's round-relative milliseconds, so
                 * the round clock is a plain division. The tick fields it also
                 * carries are the scorer's own units and do not convert at any
                 * rate this side knows. */
                unsigned secs = (unsigned)(h->startMs / 1000u);
#if !BOLO_MOBILE
                /* While a reel is up the whole row is a seek target: a selectable
                 * underneath for the hit area and controller focus, with the row's
                 * own two-tone text drawn back over it (text is not interactive,
                 * so it does not steal the hover). With no reel to seek there is
                 * no selectable at all — the text still renders everywhere, it
                 * just does not look clickable when it isn't. */
                if (lvEmbedIsActive()) {
                    ImVec2 rowPos = ImGui::GetCursorPos();
                    char rowId[16];
                    snprintf(rowId, sizeof(rowId), "##clip%d", i);
                    /* The export button sits on top of this at the far end of the
                     * row; without the overlap the selectable underneath keeps the
                     * hover and the button can never be pressed. */
                    ImGui::SetNextItemAllowOverlap();
                    if (ImGui::Selectable(rowId, false, 0,
                                          ImVec2(0, ImGui::GetTextLineHeight()))) {
                        /* Round-relative ms, the same base the reel's own window
                         * is measured in and the same one the timestamp above is
                         * divided out of. */
                        lvEmbedSeekToClip(h->startMs, h->mapX, h->mapY);
                    }
                    ImGui::SetCursorPos(rowPos);
                }
#endif
                ImGui::TextDisabled("%02u:%02u", secs / 60u, secs % 60u);
                ImGui::SameLine();

                MessageArgs args = {};
                SDL_strlcpy(args.playerName, lastRoundSlotName(cs, h->actorA),
                            sizeof(args.playerName));
                SDL_strlcpy(args.otherName, lastRoundSlotName(cs, h->actorB),
                            sizeof(args.otherName));
                args.number = (int)h->value;
                if (h->type == HL_AWARD) {
                    /* An award-anchored clip reuses that award's own label. */
                    SDL_strlcpy(args.string1,
                                langGetText(lastRoundAwardLabel(h->awardId)),
                                sizeof(args.string1));
                }
                /* Names render plain inside the sentence, not team-tinted, so
                 * the clip reads as one phrase. */
                ImGui::TextUnformatted(
                    langGetTextFmt(lastRoundHighlightLabel(h), &args));

#if !BOLO_MOBILE && BOLO_RECAP_CLIP_GIF
                /* Export control, right-aligned so the rows keep a column of them
                 * however long the sentences run. Sized to one text line, so the
                 * row stays the height the selectable underneath was given: the
                 * icon takes the line height and the frame padding loses its
                 * vertical half, which is what SmallButton does for a caption.
                 * The glyph says what the control produces and the tooltip names
                 * the format; with no icon loaded the format's name is the
                 * caption, as it was before. */
                if (lvEmbedIsActive()) {
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x -
                                         lobbyClipGifButtonWidth());
                    ImGui::PushID(i);
                    if (lobbyClipGifButton("##clipgif", true)) {
                        lobbyClipGifStartClip(h, clientSimGetMapName(cs));
                    }
                    ImGui::PopID();
                }
#endif
            }
        }
    }

    /* ── Scoreboard ordering ─────────────────────────────────────── */
    /* The table sorts on whichever column its header was last clicked, so the
     * order can only be built once the specs are readable — inside the table,
     * below. What lives here is what the comparison is made of.
     *
     * Default order, and the tie-break under every other column: kills desc,
     * then fewest deaths, then slot. */
    int n = st->playerCount;
    if (n > MAX_TANKS) n = MAX_TANKS;
    int order[MAX_TANKS];
    auto scoreBefore = [](const RoundPlayerSummary *a,
                          const RoundPlayerSummary *b) {
        if (a->kills != b->kills)   return a->kills > b->kills;
        if (a->deaths != b->deaths) return a->deaths < b->deaths;
        return a->slot < b->slot;
    };
    /* Every column but the name counts something, so one number reads them
     * all. Column 0 sorts by name and never reaches this. Signed and wide
     * enough to hold every column: a scenario's own score is the one that can
     * go below zero, and reading it as an unsigned would sort the negatives
     * to the top. */
    auto colValue = [st](const RoundPlayerSummary *p, int col) -> long long {
        switch (col) {
            case 1:  return p->kills;
            case 2:  return p->deaths;
            case 3:  return p->baseCaptures;
            case 4:  return p->pillCaptures;
            case 5:  return p->dmgDealt;
            case 6:  return p->builds;
            case 7:  return p->lgmKills;
            case 8:  return p->lgmDeaths;
            case 9:  return (p->slot < MAX_TANKS) ? st->scenarioScore[p->slot] : 0;
            default: return 0;
        }
    };

    /* A scenario's own scoreboard column, shown only when a scenario set at
     * least one score this round. The title is the scenario's own — whatever
     * its score op wrote — and falls back to a plain word when a scenario
     * scored without naming the column. */
    const bool  hasScn = st->hasScenarioScore;
    const char *scnTitle =
        (hasScn && st->scenarioScoreLabel[0] != '\0')
            ? st->scenarioScoreLabel
            : langGetText(STR_DLGLOBBY_LASTROUND_COL_SCNSCORE);
    const int   colCount = hasScn ? 10 : 9;

    /* Whether a scenario wrote this seat's row at all. A row it scored zero
     * and a row it never touched both read 0, so the mask is the only thing
     * that tells them apart: a cleared bit means there is no number to show,
     * not a number that happens to be nothing. */
    auto scnScored = [st](uint8_t slot) {
        return slot < MAX_TANKS &&
               (st->scenarioScoreMask & (uint16_t)(1u << slot)) != 0;
    };

    /* awardId (1..AWARD_COUNT) → index into awards[], -1 when unwon. */
    int ac = st->awardCount;
    if (ac > AWARD_COUNT) ac = AWARD_COUNT;
    int awardIdx[AWARD_COUNT + 1];
    for (int i = 0; i <= AWARD_COUNT; i++) awardIdx[i] = -1;
    for (int i = 0; i < ac; i++) {
        uint8_t id = st->awards[i].awardId;
        if (id >= 1 && id <= AWARD_COUNT) awardIdx[id] = i;
    }

    /* Widest count each column will actually print this round. A fixed-width
     * column clips, so a column has to cover its own numbers — but only the
     * ones that are there, not a worst case this round never reached. */
    unsigned colMax[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        const RoundPlayerSummary *pp = &st->players[i];
        if ((unsigned)pp->kills        > colMax[1]) colMax[1] = pp->kills;
        if ((unsigned)pp->deaths       > colMax[2]) colMax[2] = pp->deaths;
        if ((unsigned)pp->baseCaptures > colMax[3]) colMax[3] = pp->baseCaptures;
        if ((unsigned)pp->pillCaptures > colMax[4]) colMax[4] = pp->pillCaptures;
        if ((unsigned)pp->dmgDealt     > colMax[5]) colMax[5] = pp->dmgDealt;
        if ((unsigned)pp->builds       > colMax[6]) colMax[6] = pp->builds;
        if ((unsigned)pp->lgmKills     > colMax[7]) colMax[7] = pp->lgmKills;
        if ((unsigned)pp->lgmDeaths    > colMax[8]) colMax[8] = pp->lgmDeaths;
    }

    /* Every column that counts something the map draws is headed by that
     * sprite alone, with the written name on the tooltip, so the column only
     * has to hold its icon and its numbers — the Name column is stretch-sized
     * and gets back everything the written labels used to cost. Icons are
     * text-height tall and keep their source aspect, so these follow the UI
     * scale without a hard-coded pixel anywhere. */
    const ImGuiStyle &sty = ImGui::GetStyle();
    const float iconH = ImGui::GetTextLineHeight();
    const float lgmW  = iconH * (float)LGM_WIDTH / (float)LGM_HEIGHT;
    /* What the header spends on the sort arrow, which ImGui draws hard against
     * the right edge of the sorted column's cell — the same width TableHeader
     * reserves for it. On a column only as wide as its icon that would be the
     * icon's own pixels, so the sorted column asks for the arrow as well. */
    const float arrowW =
        SDL_truncf(ImGui::GetFontSize() * 0.65f + sty.FramePadding.x);

    /* A stat column's static width, and the floor the stretchy Name column is
     * not allowed to fall below — scaled like every other width here.
     *
     * "If there's room": eight static columns plus a readable Name column is
     * wider than the recap panel at the default lobby size, so the static width
     * is taken only when the panel can pay for it, and the content-derived
     * width below is used when it cannot. Neither depends on which column is
     * sorted, so the choice only ever changes when the lobby is resized. */
    const float kRecapStatColW = 75.0f * s;
    const float kRecapNameMinW = 120.0f * s;
    const bool  statColsStatic =
        ImGui::GetContentRegionAvail().x >=
        kRecapNameMinW + kRecapStatColW * (float)(colCount - 1);

    auto iconColWidth = [&](int col, float iconExtent) {
        char buf[16];
        SDL_snprintf(buf, sizeof(buf), "%u", colMax[col]);
        float w = ImGui::CalcTextSize(buf).x;
        if (iconExtent > w) w = iconExtent;
        /* Charged to EVERY stat column, not just the sorted one. ImGui draws
         * the arrow hard against the right edge of the sorted cell, so paying
         * for it only there made the sorted column wider than its neighbours:
         * moving the sort grew one column, shrank another, and slid every
         * column between them sideways. Reserving it everywhere costs one
         * arrow's width per column and holds the layout still. */
        w += arrowW;
        /* One pixel of slop: an icon sized to exactly fill the cell would
         * otherwise be at the mercy of rounding at the clip edge. */
        w += sty.CellPadding.x * 2.0f + 1.0f;
        if (statColsStatic && w < kRecapStatColW) w = kRecapStatColW;
        return w;
    };

    /* The scenario column has no sprite to head it, so it has to hold its
     * title as well as its numbers, and its numbers are signed. Measured the
     * same way as the others otherwise — widest thing it prints this round,
     * plus the sort arrow and the cell padding. Only the scored rows are
     * measured: an unscored row prints nothing, so its number is not one of
     * the widths this column has to cover. */
    auto scnColWidth = [&]() {
        char buf[16];
        float w = ImGui::CalcTextSize(scnTitle).x;
        for (int i = 0; i < n; i++) {
            uint8_t slot = st->players[i].slot;
            if (!scnScored(slot)) continue;
            SDL_snprintf(buf, sizeof(buf), "%d", (int)st->scenarioScore[slot]);
            float cw = ImGui::CalcTextSize(buf).x;
            if (cw > w) w = cw;
        }
        w += arrowW + sty.CellPadding.x * 2.0f + 1.0f;
        if (statColsStatic && w < kRecapStatColW) w = kRecapStatColW;
        return w;
    };

    /* Sortable: a click sorts on that column, a second click reverses it. The
     * counting columns lead with their biggest, which is the answer anyone
     * clicking them is after; names lead A→Z. Kills is where the table starts,
     * so an untouched scoreboard reads the way it always did. */
    const ImGuiTableColumnFlags statCol = ImGuiTableColumnFlags_WidthFixed |
                                          ImGuiTableColumnFlags_PreferSortDescending;
    if (n > 0 &&
        ImGui::BeginTable("##lastRoundScore", colCount,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_NoHostExtendX |
                              ImGuiTableFlags_Sortable)) {
        ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_LASTROUND_COL_NAME),
                                ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_LASTROUND_COL_KILLS),
                                statCol | ImGuiTableColumnFlags_DefaultSort,
                                iconColWidth(1, iconH));
        ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_LASTROUND_COL_DEATHS),
                                statCol, iconColWidth(2, iconH));
        ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_LASTROUND_COL_BASE),
                                statCol, iconColWidth(3, iconH));
        ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_LASTROUND_COL_PILL),
                                statCol, iconColWidth(4, iconH));
        ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_LASTROUND_COL_DMG),
                                statCol, iconColWidth(5, iconH));
        ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_LASTROUND_COL_BUILDS),
                                statCol, iconColWidth(6, iconH));
        ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_LASTROUND_COL_LGMK),
                                statCol, iconColWidth(7, lgmW));
        ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_LASTROUND_COL_LGMD),
                                statCol,
                                iconColWidth(8, lgmW + sty.ItemInnerSpacing.x +
                                                iconH));
        if (hasScn) {
            ImGui::TableSetupColumn(scnTitle, statCol, scnColWidth());
        }

        /* Header row drawn by hand: every column that counts something the
         * map draws is headed by that sprite instead of a word, with the
         * written name on the tooltip. Only Name keeps its text — no sprite
         * says "name", and none is needed: the column already reads as what
         * it is. Deaths reuses the skull the LGM Deaths column ends with, and
         * Dmg the widest frame of a shell burst, which is the map's own
         * picture of damage being done. TableHeader is still submitted for
         * every column, with an empty label where the icon speaks, so the
         * cell keeps its header background, hover, sort click and id path;
         * the id comes from the column index the way TableHeadersRow does
         * it. The scenario column is the exception among the counting
         * columns: no sprite says what a scenario counts, so it keeps its
         * written title the way Name does. */
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        for (int c = 0; c < colCount; c++) {
            ImGui::TableSetColumnIndex(c);
            const char *label = ImGui::TableGetColumnName(c);
            bool drewIcon = false;
            switch (c) {
                case 1:
                    imguiDrawTileIcon(TANK_SELF_0_X, TANK_SELF_0_Y);
                    drewIcon = true;
                    break;
                case 2:
                    drewIcon = lastRoundDrawSkull();
                    break;
                case 3:
                    imguiDrawTileIcon(BASE_GOOD_X, BASE_GOOD_Y);
                    drewIcon = true;
                    break;
                case 4:
                    imguiDrawTileIcon(PILL_EVIL15_X, PILL_EVIL15_Y);
                    drewIcon = true;
                    break;
                case 5:
                    imguiDrawTileIcon(EXPLOSION4_X, EXPLOSION4_Y);
                    drewIcon = true;
                    break;
                case 6:
                    imguiDrawTileIcon(BUILD_SINGLE_X, BUILD_SINGLE_Y);
                    drewIcon = true;
                    break;
                case 7:
                    imguiDrawAtlasIcon(LGM0_X, LGM0_Y, LGM_WIDTH, LGM_HEIGHT);
                    drewIcon = true;
                    break;
                case 8:
                    /* Man then skull — the pair reads as "little men lost",
                     * against column 7's bare man for the ones you killed.
                     * Without the skull the pair is ambiguous, so that case
                     * falls back to the written label. */
                    imguiDrawAtlasIcon(LGM0_X, LGM0_Y, LGM_WIDTH, LGM_HEIGHT);
                    ImGui::SameLine(0.0f, sty.ItemInnerSpacing.x);
                    drewIcon = lastRoundDrawSkull();
                    break;
                default:
                    break;
            }
            if (drewIcon) ImGui::SameLine(0.0f, 0.0f);
            ImGui::PushID(c);
            ImGui::TableHeader(drewIcon ? "" : label);
            ImGui::PopID();
            if (drewIcon) imguiHelpTooltip(label);
        }

        /* Rows in the order the header row just asked for. Reading the specs
         * after the headers rather than before them is what makes a click
         * land on the frame it happened rather than the one after. A column
         * that ties falls back to the default order, so equal counts still
         * come out best-round-first instead of shuffling. */
        int sortCol = 1;
        bool sortAsc = false;
        if (ImGuiTableSortSpecs *specs = ImGui::TableGetSortSpecs()) {
            if (specs->SpecsCount > 0) {
                sortCol = specs->Specs[0].ColumnIndex;
                sortAsc = specs->Specs[0].SortDirection ==
                          ImGuiSortDirection_Ascending;
            }
            specs->SpecsDirty = false;
        }

        auto rowBefore = [&](const RoundPlayerSummary *a,
                             const RoundPlayerSummary *b) {
            if (sortCol == 0) {
                int c = SDL_strcasecmp(lastRoundSlotName(cs, a->slot),
                                       lastRoundSlotName(cs, b->slot));
                if (c != 0) return sortAsc ? (c < 0) : (c > 0);
            } else {
                /* The scenario column is the one with rows that hold no
                 * number at all. They sort below every scored row whichever
                 * way the arrow points, so reversing the column reverses the
                 * scores instead of bringing the blank cells to the top. */
                if (sortCol == 9) {
                    bool sa = scnScored(a->slot), sb = scnScored(b->slot);
                    if (sa != sb) return sa;
                }
                long long va = colValue(a, sortCol), vb = colValue(b, sortCol);
                if (va != vb) return sortAsc ? (va < vb) : (va > vb);
            }
            return scoreBefore(a, b);
        };
        for (int i = 0; i < n; i++) order[i] = i;
        for (int i = 1; i < n; i++) {
            int j = i;
            while (j > 0 && rowBefore(&st->players[order[j]],
                                      &st->players[order[j - 1]])) {
                int t = order[j - 1]; order[j - 1] = order[j]; order[j] = t;
                j--;
            }
        }

        const BYTE mySlot = gameFrontGetPlayerNum();
        for (int r = 0; r < n; r++) {
            const RoundPlayerSummary *p = &st->players[order[r]];
            ImGui::TableNextRow();
            /* Lift your own line off the alternating row background so it
             * is findable at a glance. Background only — the team tint has
             * to stay the row's text colour. */
            if (p->slot == mySlot) {
                ImVec4 mine = ImGui::GetStyleColorVec4(ImGuiCol_Header);
                mine.w = 0.38f;
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                       ImGui::GetColorU32(mine));
            }
            ImGui::TableSetColumnIndex(0);
            /* Row hit area. The previous attempt hand-rolled a hover band from
             * GetWindowContentRegionMin/Max plus IsWindowHovered, and never
             * fired — inside a table those are the wrong window and the wrong
             * coordinate space. This is the canonical ImGui table row-click
             * recipe instead: a label-less Selectable spanning every column,
             * submitted first so the cells draw on top of it, with
             * AllowOverlap so they keep their own hit-testing. It also brings
             * its own hover highlight, which is the affordance. */
            ImGui::PushID(r);
            bool rowClicked = ImGui::Selectable(
                "##recapRow", false,
                ImGuiSelectableFlags_SpanAllColumns |
                    ImGuiSelectableFlags_AllowOverlap,
                ImVec2(0.0f, 0.0f));
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
            ImGui::PopID();
            if (rowClicked) {
                lobbyRecapRowJump(cs, (int)p->slot, p->isBot != 0);
            }
            ImGui::SameLine(0.0f, 0.0f);
            lastRoundRenderName(cs, p->slot, p->isBot != 0);
            ImGui::TableSetColumnIndex(1); ImGui::Text("%u", (unsigned)p->kills);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%u", (unsigned)p->deaths);
            ImGui::TableSetColumnIndex(3); ImGui::Text("%u", (unsigned)p->baseCaptures);
            ImGui::TableSetColumnIndex(4); ImGui::Text("%u", (unsigned)p->pillCaptures);
            ImGui::TableSetColumnIndex(5); ImGui::Text("%u", (unsigned)p->dmgDealt);
            ImGui::TableSetColumnIndex(6); ImGui::Text("%u", (unsigned)p->builds);
            ImGui::TableSetColumnIndex(7); ImGui::Text("%u", (unsigned)p->lgmKills);
            ImGui::TableSetColumnIndex(8); ImGui::Text("%u", (unsigned)p->lgmDeaths);
            if (hasScn && scnScored(p->slot)) {
                ImGui::TableSetColumnIndex(9);
                ImGui::Text("%d", (int)st->scenarioScore[p->slot]);
            }
        }
        ImGui::EndTable();
    }

    /* ── The scenario's team scores ──────────────────────────────── */
    /* The table above has a row per player and no team summary row to hang a
     * team's score on, so the teams a scenario scored are listed under it,
     * tinted the way the roster tints a team. Team numbers run from 1, and
     * the mask says which of them the scenario wrote — a team it put on zero
     * gets its line like any other, because zero is a score and the mask is
     * what says so. */
    if (hasScn) {
        bool anyTeam = false;
        for (int team = 1; team < MAX_TANKS; team++) {
            if ((st->scenarioTeamScoreMask & (uint16_t)(1u << team)) == 0) {
                continue;
            }
            if (!anyTeam) {
                ImGui::Separator();
                ImGui::TextUnformatted(scnTitle);
                anyTeam = true;
            }
            MessageArgs args = {};
            args.number = team;
            char teamName[64];
            SDL_strlcpy(teamName, langGetTextFmt(STR_DLGLOBBY_TEAM_HEADER, &args),
                        sizeof(teamName));
            ImGui::Bullet();
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  lastRoundTeamTint(cs, (uint8_t)team));
            ImGui::TextUnformatted(teamName);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::Text("%d", (int)st->scenarioTeamScore[team]);
        }
    }

    /* ── Awards ribbon ───────────────────────────────────────────── */
    /* Headed like the clip list above it, so the lines below read as their own
     * section rather than as a tail on the scoreboard. */
    ImGui::Separator();
    ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_LASTROUND_AWARDS));

    /* Renders one award line: label — winner [owned subject] (value). */
    auto renderAward = [&](int idx) {
        const AwardResult *aw = &st->awards[idx];
        ImGui::Bullet();
        ImGui::TextUnformatted(langGetText(lastRoundAwardLabel(aw->awardId)));
        ImGui::SameLine();
        ImGui::TextDisabled("-");
        ImGui::SameLine();
        if (aw->awardId == AWARD_NEMESIS && aw->subjectSlot < MAX_TANKS) {
            /* "X owned Y": names rendered plain so the relationship reads
             * as one phrase. */
            const ClientLobbySlot *w =
                (aw->winnerSlot < MAX_TANKS)
                    ? clientSimGetLobbySlot(cs, (BYTE)aw->winnerSlot)
                    : nullptr;
            const ClientLobbySlot *v =
                clientSimGetLobbySlot(cs, (BYTE)aw->subjectSlot);
            MessageArgs args = {};
            SDL_strlcpy(args.string1,
                        (w && w->playerName[0])
                            ? w->playerName
                            : langGetText(STR_DLGLOBBY_LASTROUND_NOPLAYER),
                        sizeof(args.string1));
            SDL_strlcpy(args.string2,
                        (v && v->playerName[0])
                            ? v->playerName
                            : langGetText(STR_DLGLOBBY_LASTROUND_NOPLAYER),
                        sizeof(args.string2));
            ImGui::TextUnformatted(
                langGetTextFmt(STR_DLGLOBBY_LASTROUND_NEMESIS_FMT, &args));
        } else {
            lastRoundRenderName(cs, aw->winnerSlot, aw->winnerIsBot != 0);
        }
        ImGui::SameLine();
        if (lastRoundAwardIsRatio(aw->awardId)) {
            ImGui::TextDisabled("(%.2f)", aw->value / 100.0);
        } else {
            ImGui::TextDisabled("(%u)", (unsigned)aw->value);
        }
    };

    /* The three the ribbon always leads with: the two objective captures a
     * round is actually won on, and the builder hunt. A round that did not
     * award one just does not show that line — the rest keep their order. */
    static const uint8_t pinnedAwards[] = {
        AWARD_MOST_BASE_CAPTURES, AWARD_MOST_PILL_CAPTURES, AWARD_LGM_HUNTER
    };
    const int pinnedCount = (int)(sizeof(pinnedAwards) / sizeof(pinnedAwards[0]));
    for (int i = 0; i < pinnedCount; i++) {
        if (awardIdx[pinnedAwards[i]] >= 0) renderAward(awardIdx[pinnedAwards[i]]);
    }

    /* A busy round wins most of the eighteen, which reads as a wall of text and
     * buries the ones worth reading. The rest of the ribbon is a fixed handful
     * of the others, drawn from the summary's own bytes so every client shows
     * the same ones and a re-render never reshuffles. The three above are held
     * out of the draw so none of them can come up twice. */
    uint8_t picks[RECAP_AWARDS_RANDOM];
    int pickCount = roundStatsPickAwardSubsetExcluding(st, pinnedAwards,
                                                       pinnedCount, picks,
                                                       RECAP_AWARDS_RANDOM);
    for (int i = 0; i < pickCount; i++) {
        renderAward(picks[i]);
    }

#if !BOLO_MOBILE && BOLO_RECAP_WBN_RATING
    /* ── WinBolo.net rating & comments ───────────────────────────── */
    lobbyRenderRatingBlock(cs, st, s);
#endif

#if !BOLO_MOBILE && BOLO_RECAP_CLIP_GIF
    /* A clip export in progress. Drawn after the rows that start it, and after
     * the rest of the body: it is a popup, so it costs the layout nothing and
     * the slack measurement below still sees what the body really used. */
    lobbyClipGifRender(s);
#endif

#if !BOLO_MOBILE
    /* What the body did not use goes to the reel next frame. One frame of lag
     * is inherent — the cost of everything below the reel is only known once
     * it has been drawn — so switching between the desktop column and the
     * controller tab, which are different heights, shows a single frame at the
     * old size before this settles. Growing the reel shrinks the shortfall by
     * the same amount, so it converges instead of hunting. Bounded by the
     * container: with no replay to show, the reel draws nothing and there is
     * nothing to absorb the shortfall, so an unbounded total would climb for
     * as long as the recap is on screen. */
    lobbyReel()->recapSlack += bodyAvailH - (ImGui::GetCursorPosY() - bodyStartY);
    if (lobbyReel()->recapSlack < 0.0f)       lobbyReel()->recapSlack = 0.0f;
    if (lobbyReel()->recapSlack > bodyAvailH) lobbyReel()->recapSlack = bodyAvailH;
#endif
}

/* ── Layout A — small inline lock badge ───────────────────────────
 * Renders an inline orange "[locked]" pill next to a setting name
 * when the server has flagged it in serverLocks. Cosmetic + tooltip. */
void lobbyRenderLockBadge(void) {
    ImGui::SameLine();
    if (lobbyIcons()->locked) {
        float sz = ImGui::GetTextLineHeight();
        ImGui::ImageWithBg((ImTextureID)lobbyIcons()->locked, ImVec2(sz, sz),
                          ImVec2(0, 0), ImVec2(1, 1),
                          ImVec4(0, 0, 0, 0),
                          wbThemeColor(g_theme->lockBadge));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, wbThemeColor(g_theme->lockBadge));
        ImGui::Text("%s", langGetText(STR_DLGLOBBY_LOCK_BADGE));
        ImGui::PopStyleColor();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_TOOLTIP_RANKED_LOCKED));
    }
}

/* ── Skip-map vote ────────────────────────────────────────────────
 * The toggle button plus the running "n of m" count. Draws nothing at
 * all unless the server offers the vote and the map is unlocked, so the
 * caller needs no gate of its own — including the leading spacer, which
 * would otherwise leave a gap on servers that never offer it.
 *
 * sameLine chooses between stacking it under the map info (what the map
 * panel wants) and setting it beside whatever precedes it on the row. */
void lobbyRenderMapSkipVote(ClientSim *cs, bool spectator, bool hasTransport,
                              float s, bool sameLine) {
    if (spectator || !clientSimIsMapSkipAvailable(cs) ||
        !clientSimIsInLobby(cs) ||
        (clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_MAP)) {
        return;
    }
    if (sameLine) {
        ImGui::SameLine();
    } else {
        ImGui::Spacing();
    }
    bool countdownActive = clientSimGetCountdownSeconds(cs) > 0;
    if (countdownActive) ImGui::BeginDisabled();
    bool voted = clientSimIsMapSkipMyVote(cs);
    const char *skipLabel = langGetText(
        voted ? STR_DLGLOBBY_CANCELSKIP : STR_DLGLOBBY_SKIPMAP);
    if (voted) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.4f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.5f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.7f, 0.3f, 0.05f, 1.0f));
    }
    if (ImGui::Button(skipLabel, ImVec2(100 * s, 0))) {
        clientSimSetMapSkipMyVote(cs, !clientSimIsMapSkipMyVote(cs));
        if (hasTransport) {
            clientSimNetSendMapSkipVote(cs);
        }
    }
    if (voted) {
        ImGui::PopStyleColor(3);
    }
    ImGui::SameLine();
    int skipCount = 0, humanCount = 0;
    for (int j = 0; j < MAX_TANKS; j++) {
        const ClientLobbySlot *jSlot = clientSimGetLobbySlot(cs, (BYTE)j);
        if (jSlot && jSlot->connected && !jSlot->isBot) {
            humanCount++;
            if (clientSimIsMapSkipVote(cs, (BYTE)j)) skipCount++;
        }
    }
    {
        MessageArgs vargs;
        memset(&vargs, 0, sizeof(vargs));
        vargs.number  = skipCount;
        vargs.number2 = humanCount;
        ImGui::TextUnformatted(
            langGetTextFmt(STR_DLGLOBBY_VOTES, &vargs));
    }
    if (countdownActive) ImGui::EndDisabled();
}
