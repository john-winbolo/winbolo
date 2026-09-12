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
 * Name:          lobby_settings.cpp
 * Purpose:       The lobby's editable game-settings panel.
 *                The collapsing header and its open state —
 *                the Steam Deck default, and the fold-away
 *                and restore across the post-game view's
 *                edge — plus the form body that renders the
 *                game type, computer-player policy, hidden
 *                mines, time limit, open-host and password
 *                controls and dispatches each edit to the
 *                host.
 *********************************************************/

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_internal.h"  /* ImGui::GetCurrentWindow — the body's font scale */
#include "lobby_internal.h"
#include "dialog_footer.h"   /* WBUI::DialogFooter — the Visibility popup's Close */
extern "C" {
#include "client_sim.h"
#include "client_net.h"  /* clientSimNetSendLobbyOpenHost / SetPassword */
#include "../../../lang.h"
#include "../../../ui_mode.h"  /* uiModeIsSteamDeck — the header's default state */
#include "../../sdl3draw.h"    /* sdl3DrawGetRenderer — the summary's sprites */
}

/* ── Layout A — editable game settings panel ──────────────────────
 * Renders the four mockup setting groups (Game Type / AI / Other /
 * Time Limit). Locked settings render disabled with a lock badge.
 * Edits dispatch as PACKET_LOBBY_SET_SETTING via the new wire
 * commands. Host-only or anyone if openHost. */
/* Forward decl — the form body is defined just after the panel, but the
 * panel (and the controller Settings tab) call it. */
void lobbyRenderGameSettingsBody(ClientSim *cs, int myPlayerNum, float s);

/* Width for an InputInt that carries step buttons and still shows its
 * number. ImGui lays the two buttons out inside the item width — a frame
 * height each, with an inner spacing in front of each — and gives the field
 * whatever is left, which a flat width leaves as almost nothing once the
 * font is scaled up and the frames grow with it. So ask for the digits and
 * add the buttons on rather than the other way round. `widest` is the
 * longest value the box has to hold. */
static float lobbyStepInputWidth(const char *widest) {
    const ImGuiStyle &st = ImGui::GetStyle();
    return ImGui::CalcTextSize(widest).x + st.FramePadding.x * 2.0f
         + (ImGui::GetFrameHeight() + st.ItemInnerSpacing.x) * 2.0f;
}

/* Open state of the settings CollapsingHeader. File-scope rather than a
 * panel-local static because the lobby's post-game edge handler below
 * drives it from outside the panel. */
typedef struct LobbySettingsState {
    bool open     = true;
    bool openInit = false;
    /* Set while the header sits collapsed on the post-game view's initiative,
     * so the recap clearing knows there is something to put back. */
    bool autoCollapsed = false;
    bool preCollapse   = true;
    /* Last frame's post-game-view flag, so the handler below can act on the
     * transitions into and out of that view rather than every frame. */
    bool prevShowLastRound = false;
} LobbySettingsState;

static LobbySettingsState s_settings = {};

/* Default collapsed on the Steam Deck: its small screen needs the
 * vertical room for the player list and the Ready button, and the
 * map / game-type summary is already on the lobby's top status bar.
 * Desktop keeps it open. Runs from whichever of the panel or the
 * post-game edge handler comes first, so a first-frame init can't
 * clobber an auto-collapse that already happened. */
static void lobbySettingsHeaderInit(void) {
    if (s_settings.openInit) return;
    s_settings.open     = !uiModeIsSteamDeck();
    s_settings.openInit = true;
}

/* The post-game recap needs the vertical room the settings form takes,
 * so entering the post-game view folds the header away — once, on the
 * edge, never re-forced per frame, so the chevron still re-opens it.
 *
 * Restore policy on the way out (countdown clears the summary): put back
 * the pre-collapse state only if the header is still exactly as the
 * auto-collapse left it. Re-opening it during the recap clears the flag,
 * so a manual choice outranks the remembered state and survives into the
 * next round. */
void lobbySettingsPostGameEdge(bool showLastRound) {
    lobbySettingsHeaderInit();
    if (showLastRound && !s_settings.prevShowLastRound) {
        s_settings.preCollapse   = s_settings.open;
        /* Already collapsed → nothing was taken away, nothing to give back. */
        s_settings.autoCollapsed = s_settings.open;
        s_settings.open          = false;
    } else if (showLastRound) {
        if (s_settings.open) s_settings.autoCollapsed = false;
    } else if (s_settings.prevShowLastRound) {
        if (s_settings.autoCollapsed) s_settings.open = s_settings.preCollapse;
        s_settings.autoCollapsed = false;
    }
    s_settings.prevShowLastRound = showLastRound;
}

void lobbyRenderGameSettingsPanel(ClientSim *cs,
                                    int myPlayerNum, float s) {
    const bool spectator = clientSimIsSpectator(cs);
    bool effectiveHost = !spectator && (lobbyIsHost(cs, myPlayerNum) || clientSimGetLobbyOpenHost(cs) ||
                         (myPlayerNum >= 0 && myPlayerNum < MAX_TANKS &&
                          (clientSimGetLobbySlot(cs, (BYTE)(myPlayerNum))->clientFlags
                           & PLAYER_FLAG_ADMIN)));

    /* Non-privileged players get nothing — neither the form nor the
     * CollapsingHeader. The same map / game-type / pill-count /
     * AI-policy info is already on the lobby's top status bar, so
     * showing a disabled-out duplicate just clutters the view. */
    if (!effectiveHost) {
        return;
    }

    /* Drive the CollapsingHeader's open state explicitly so a "Hide
     * Settings" button at the bottom of the panel — and the post-game
     * auto-collapse — can fold it away once the host is happy with the
     * configuration. The chevron still toggles it either way; see
     * lobbySettingsHeaderInit / lobbySettingsPostGameEdge above. */
    lobbySettingsHeaderInit();
    ImGui::SetNextItemOpen(s_settings.open, ImGuiCond_Always);
    /* Capture screen-Y of the header before drawing so the
     * right-aligned openHost control can be overlaid on the same
     * line via SetCursorScreenPos. */
    ImVec2 headerStart = ImGui::GetCursorScreenPos();
    bool headerOpen = ImGui::CollapsingHeader(langGetText(STR_DLGLOBBY_SETTINGS_HEADER));

    /* When the host has flipped on "Allow all players to change
     * settings", surface that on the header so non-host players
     * understand why the form is interactive for them. Right-anchored
     * within the header's row using the captured headerStart Y plus
     * the row's right edge minus the label width. */
    if (clientSimGetLobbyOpenHost(cs)) {
        const char *noteText = langGetText(STR_DLGLOBBY_OPENHOST_NOTE);
        ImVec2 textSize = ImGui::CalcTextSize(noteText);
        float headerH   = ImGui::GetFrameHeight();
        float availW    = ImGui::GetWindowContentRegionMax().x
                        - ImGui::GetWindowContentRegionMin().x;
        ImVec2 winPos   = ImGui::GetWindowPos();
        float pad       = ImGui::GetStyle().FramePadding.x;
        ImVec2 notePos(
            winPos.x + ImGui::GetWindowContentRegionMin().x + availW
                - textSize.x - pad,
            headerStart.y + (headerH - textSize.y) * 0.5f);
        ImGui::GetWindowDrawList()->AddText(
            notePos, IM_COL32(180, 180, 180, 220), noteText);
    }

    if (!headerOpen) {
        s_settings.open = false;
        return;
    }
    s_settings.open = true;

    /* The openHost ("Allow all players to change settings") state is
     * intentionally invisible to regular players — they shouldn't even
     * know they got their edit access via that particular toggle, only
     * that the settings happen to be interactable for them. The actual
     * editable checkbox is rendered for host/admin only in the "Other"
     * column below. */

    lobbyRenderGameSettingsBody(cs, myPlayerNum, s);
}

/* The game-settings form proper (game type / AI policy / mines / time
 * limit / password). Split out of lobbyRenderGameSettingsPanel so the
 * controller Settings tab can render it flat, without the desktop
 * collapsing-header chrome. Host-gated by every caller. */
void lobbyRenderGameSettingsBody(ClientSim *cs, int myPlayerNum, float s) {
    const bool spectator = clientSimIsSpectator(cs);
    /* Same effective-host test the panel computes, recomputed here so the
     * per-control disabled state is identical whether the body renders in
     * the desktop collapsing header or the controller Settings tab. */
    bool effectiveHost = !spectator && (lobbyIsHost(cs, myPlayerNum) || clientSimGetLobbyOpenHost(cs) ||
                         (myPlayerNum >= 0 && myPlayerNum < MAX_TANKS &&
                          (clientSimGetLobbySlot(cs, (BYTE)(myPlayerNum))->clientFlags
                           & PLAYER_FLAG_ADMIN)));

    /* Settings body uses a smaller font than the rest of the lobby so
     * the 3-column form doesn't dominate the visual hierarchy. */
    float settingsOldScale = ImGui::GetCurrentWindow()->FontWindowScale;
    ImGui::SetWindowFontScale(settingsOldScale * 0.85f);

    ImGui::Spacing();
    /* Set by the Visibility button under the time limit below. The popup is
     * opened after the columns close, next to the BeginPopupModal that
     * answers it. */
    bool openVisibility = false;
    ImGui::Columns(3, "##settingsCols", false);

    /* ── Game Type ──────────────────────────────────────────── */
    bool gtLocked = (clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_GAME_TYPE) != 0;
    {
        ImGui::Text("%s", langGetText(STR_DLGLOBBY_GAMETYPE_LBL));
        if (gtLocked) lobbyRenderLockBadge();
        bool disable = !effectiveHost || gtLocked;
        if (disable) ImGui::BeginDisabled();
        const char *items[] = {
            langGetText(STR_DLGGAMESETUP_RADIO1),
            langGetText(STR_DLGGAMESETUP_RADIO2),
            langGetText(STR_DLGGAMESETUP_RADIO3),
        };
        bool rankedNow = clientSimGetLobbyRanked(cs);
        for (int i = 0; i < 3; i++) {
            /* gameType enum is 1-based (gameOpen=1, gameTournament=2,
             * gameStrictTournament=3), so the array index → enum
             * mapping is i+1. The previous (gameType)i comparison
             * read the wrong row as "checked" — Open showed as
             * Unknown, Tournament showed as Open, etc. */
            int enumVal = i + 1;
            /* Ranked games forbid the "Open" type — grey it out. */
            bool optDisabled = rankedNow && (gameType)enumVal == gameOpen;
            if (optDisabled) ImGui::BeginDisabled();
            char rid[80];
            SDL_snprintf(rid, sizeof(rid), "%s##gt%d", items[i], i);
            bool checked = (clientSimGetLobbyGameType(cs) == (gameType)enumVal);
            if (ImGui::RadioButton(rid, checked) && !checked) {
                uint8_t v = (uint8_t)enumVal;
                lobbySendSetting(cs, LST_GAME_TYPE, &v, 1);
            }
            if (optDisabled) ImGui::EndDisabled();
        }
        if (disable) ImGui::EndDisabled();
    }
    ImGui::NextColumn();

    /* ── Computer Players ────────────────────────────────────── */
    bool aiLocked = (clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_AI_POLICY) != 0;
    {
        ImGui::Text("%s", langGetText(STR_DLGLOBBY_AI_SECTION_LBL));
        if (aiLocked) lobbyRenderLockBadge();
        /* Ranked games force "No computer tanks" — disable the
         * whole AI block since none of the alternatives are valid. */
        bool disable = !effectiveHost || aiLocked
                        || clientSimGetLobbyRanked(cs);
        if (disable) ImGui::BeginDisabled();
        const char *items[] = {
            langGetText(STR_DLGLOBBY_AI_NONE),
            langGetText(STR_DLGLOBBY_AI_ALLOW),
            langGetText(STR_DLGLOBBY_AI_ADVANTAGE),
            langGetText(STR_DLGLOBBY_AI_FULLADV),
        };
        for (int i = 0; i < 4; i++) {
            /* Same trick as the Game Type radios — embed the label so
             * the whole row is clickable. */
            char rid[80];
            SDL_snprintf(rid, sizeof(rid), "%s##ai%d", items[i], i);
            bool checked = (clientSimGetLobbyAiType(cs) == (uint8_t)i);
            if (ImGui::RadioButton(rid, checked) && !checked) {
                uint8_t v = (uint8_t)i;
                lobbySendSetting(cs, LST_AI_POLICY, &v, 1);
            }
        }
        if (disable) ImGui::EndDisabled();
    }
    ImGui::NextColumn();

    /* ── Other (mines / time limit / autoLockOnGameStart) ────── */
    {
        ImGui::Text("%s", langGetText(STR_DLGLOBBY_OTHER_LBL));

        bool minesLocked = (clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_MINES) != 0;
        bool minesV = clientSimIsLobbyHiddenMines(cs);
        bool minesDisabled = !effectiveHost || minesLocked;
        if (minesDisabled) ImGui::BeginDisabled();
        if (ImGui::Checkbox(langGetText(STR_DLGGAMESETUP_HIDDENMINES), &minesV)) {
            uint8_t v = minesV ? 1 : 0;
            lobbySendSetting(cs, LST_HIDDEN_MINES, &v, 1);
        }
        if (minesDisabled) ImGui::EndDisabled();
        if (minesLocked) lobbyRenderLockBadge();

        /* "Allow all players to change settings" — toggles openHost
         * (the same flag that gates per-team manage-bots authority).
         * Visible to host / admin only. Regular players who got their
         * edit access via this very toggle don't see the control or
         * any read-only mirror of it — surfacing it would just
         * advertise the mechanism and tempt them to flip it off
         * (which they can't anyway, but the absence avoids the
         * noise). Sits next to "Allow Hidden Mines" so the related
         * authority-gating controls cluster together at the top of
         * the Other column. */
        if (!clientSimIsSinglePlayer(cs)) {
            bool isHostLocal = lobbyIsHost(cs, myPlayerNum);
            bool isAdminLocal = (myPlayerNum >= 0 && myPlayerNum < MAX_TANKS &&
                                 (clientSimGetLobbySlot(cs, (BYTE)(myPlayerNum))->clientFlags
                                  & PLAYER_FLAG_ADMIN));
            if (isHostLocal || isAdminLocal) {
                bool oh = clientSimGetLobbyOpenHost(cs);
                bool openHostLocked = (clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_OPEN_HOST) != 0;
                if (openHostLocked) ImGui::BeginDisabled();
                if (ImGui::Checkbox(langGetText(STR_DLGLOBBY_OPENHOST_CB),
                                    &oh)) {
                    clientSimNetSendLobbyOpenHost(cs, oh);
                }
                if (openHostLocked) ImGui::EndDisabled();
                if (openHostLocked) lobbyRenderLockBadge();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    if (openHostLocked) {
                        ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_TOOLTIP_RANKED_LOCKED));
                    } else {
                        ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_OPENHOST_TOOLTIP));
                    }
                }
            }
        }

        bool timeLocked = (clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_TIME_LIMIT) != 0;
        bool timeV = clientSimGetLobbyTimeLimit(cs) > 0;
        bool timeDisabled = !effectiveHost || timeLocked;
        if (timeDisabled) ImGui::BeginDisabled();
        if (ImGui::Checkbox(langGetText(STR_DLGGAMESETUP_TIMELIMIT), &timeV)) {
            uint8_t v = timeV ? 1 : 0;
            lobbySendSetting(cs, LST_TIME_LIMIT, &v, 1);
        }
        if (timeV) {
            int mins = clientSimGetLobbyTimeLimit(cs) > 0
                ? (int)(clientSimGetLobbyTimeLimit(cs) / (50 * 60))
                : 30;
            ImGui::SameLine();
            /* LOBBY_TIME_MINUTES_MAX is four digits. */
            ImGui::SetNextItemWidth(lobbyStepInputWidth("0000"));
            if (ImGui::InputInt("##tmin", &mins, 1, 5,
                                ImGuiInputTextFlags_EnterReturnsTrue)) {
                if (mins < LOBBY_TIME_MINUTES_MIN) mins = LOBBY_TIME_MINUTES_MIN;
                if (mins > LOBBY_TIME_MINUTES_MAX) mins = LOBBY_TIME_MINUTES_MAX;
                uint8_t v[2] = { (uint8_t)((mins >> 8) & 0xFF),
                                 (uint8_t)(mins & 0xFF) };
                lobbySendSetting(cs, LST_TIME_MINUTES, v, 2);
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_TIMELIMIT_MIN));
        }
        if (timeDisabled) ImGui::EndDisabled();
        if (timeLocked) lobbyRenderLockBadge();

        /* Visibility, under the time limit and in the column the rest of
         * the odds and ends sit in. Everything it edits is in a popup, so
         * a button is all the room it takes on the form. Opened after the
         * columns close, where the popup itself is declared. */
        if (ImGui::Button(langGetText(STR_DLGLOBBY_VISIBILITY_LBL))) {
            openVisibility = true;
        }

        /* Password protection — host or admin only (NOT openHost;
         * we don't want random connected players to be able to lock
         * the host out of their own server). MP only — SP has no
         * remote clients to keep out. */
        if (!clientSimIsSinglePlayer(cs)) {
            bool isHostLocal  = lobbyIsHost(cs, myPlayerNum);
            bool isAdminLocal = (myPlayerNum >= 0 && myPlayerNum < MAX_TANKS &&
                                 (clientSimGetLobbySlot(cs, (BYTE)(myPlayerNum))->clientFlags
                                  & PLAYER_FLAG_ADMIN));
            if (isHostLocal || isAdminLocal) {
                /* Two pieces of UI state, both UI-layer-owned (the
                 * server never echoes the password value, so we
                 * can't reconstruct them from sim state):
                 *   s_pwOn   — the "Password" checkbox's intent.
                 *              Toggled directly by the click,
                 *              persists across frames.
                 *   s_pwBuf  — the typed password. When the user
                 *              unchecks we wipe it AND clear the
                 *              server-side value. */
                static char s_pwBuf[200] = {0};
                static bool s_pwOn       = false;

                bool pwLocked = (clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_PASSWORD) != 0;
                if (pwLocked) ImGui::BeginDisabled();
                if (ImGui::Checkbox(langGetText(STR_DLGLOBBY_PASSWORD_CB), &s_pwOn)) {
                    if (!s_pwOn) {
                        s_pwBuf[0] = '\0';
                        clientSimNetSendLobbySetPassword(cs, "");
                    }
                    /* Checking with an empty buffer doesn't send
                     * anything yet — wait for the user to type. */
                }
                if (pwLocked) ImGui::EndDisabled();
                if (pwLocked) lobbyRenderLockBadge();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    if (pwLocked) {
                        ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_TOOLTIP_RANKED_LOCKED));
                    } else {
                        ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_PASSWORD_TOOLTIP));
                    }
                }
                if (s_pwOn && !pwLocked) {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(180.0f * s);
                    if (ImGui::InputText("##serverpw", s_pwBuf,
                                         sizeof(s_pwBuf),
                                         ImGuiInputTextFlags_Password |
                                         ImGuiInputTextFlags_EnterReturnsTrue)) {
                        clientSimNetSendLobbySetPassword(cs, s_pwBuf);
                    }
                    /* Send on blur too so the host doesn't have to
                     * remember to hit Enter. ImGui surfaces this via
                     * IsItemDeactivatedAfterEdit. */
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        clientSimNetSendLobbySetPassword(cs, s_pwBuf);
                    }
                }
            }
        }

        /* "Allow new players" / "Disallow new players once game has
         * started" moved to the team-list header (right of "+ Add Team")
         * so all join-related controls live in one row. See
         * lobbyRenderTeamGroupedPlayers. */

    }

    ImGui::Columns(1);

    /* ── Visibility (pillboxes / bases / allied tanks) ─────────────
     * The editor behind the button under the time limit, because the three
     * rows took more room on the lobby surface than the rest of the form
     * put together. The rows themselves are unchanged, they just live
     * in the popup now: one per category, a 4-way combo plus a
     * decay-seconds input that is only enabled while that row's combo reads
     * Decay, each edit sending the 3-byte [policy][decay hi][decay lo]
     * payload for that category's LST_* id. What the rows are set to is on
     * the lobby's header line, which the host reads along with everyone
     * else, so the form carries no second copy. */
    if (openVisibility) {
        ImGui::OpenPopup("###visibility");
    }

    /* Same ID scope as the OpenPopup above — BeginPopupModal only finds
     * a popup opened at its own scope. The title is composed so the
     * translated label is the caption and everything after ### is the
     * ID, which a translation therefore cannot change. */
    {
        char visTitle[128];
        SDL_snprintf(visTitle, sizeof(visTitle), "%s###visibility",
                     langGetText(STR_DLGLOBBY_VISIBILITY_LBL));
        static bool s_visOpen = true; s_visOpen = true;
        if (ImGui::BeginPopupModal(visTitle, &s_visOpen,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            struct ViewRow {
                int          label;
                ViewCategory cat;
                uint8_t      lst;
                uint32_t     lockBit;
                const char  *id;
            };
            static const ViewRow rows[] = {
                { STR_DLGLOBBY_VIEW_PILL, viewCategoryPill,
                  LST_PILL_VIEW, LOBBY_LOCK_PILL_VIEW, "pill" },
                { STR_DLGLOBBY_VIEW_BASE, viewCategoryBase,
                  LST_BASE_VIEW, LOBBY_LOCK_BASE_VIEW, "base" },
                { STR_DLGLOBBY_VIEW_ALLY, viewCategoryAlly,
                  LST_ALLY_VIEW, LOBBY_LOCK_ALLY_VIEW, "ally" },
            };
            const char *modes[] = {
                langGetText(STR_DLGLOBBY_VIEW_ALWAYS),
                langGetText(STR_DLGLOBBY_VIEW_KEY),
                langGetText(STR_DLGLOBBY_VIEW_DECAY),
                langGetText(STR_DLGLOBBY_VIEW_OFF),
            };
            auto sendView = [&](uint8_t lst, int policy, int secs) {
                if (secs < VIEW_DECAY_MIN_SECS) secs = VIEW_DECAY_MIN_SECS;
                if (secs > VIEW_DECAY_MAX_SECS) secs = VIEW_DECAY_MAX_SECS;
                uint8_t v[3] = { (uint8_t)policy,
                                 (uint8_t)((secs >> 8) & 0xFF),
                                 (uint8_t)(secs & 0xFF) };
                lobbySendSetting(cs, lst, v, 3);
            };

            /* Classic mode sits above the three rows because it owns
             * them: while it is on the server writes pill Key, base Off
             * and ally Off and refuses an edit to any of the three, so
             * the rows below go disabled rather than letting the host
             * click a combo and get a silent reject. */
            bool classic = clientSimGetClassicMode(cs);
            bool classicLocked =
                (clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_CLASSIC_MODE) != 0;
            bool classicDisabled = !effectiveHost || classicLocked;
            if (classicDisabled) ImGui::BeginDisabled();
            if (ImGui::Checkbox(langGetText(STR_DLGLOBBY_CLASSIC_MODE_CB),
                                &classic)) {
                uint8_t v = classic ? 1 : 0;
                lobbySendSetting(cs, LST_CLASSIC_MODE, &v, 1);
            }
            /* Read the hover before the badge draws, so the tooltip
             * belongs to the checkbox and not to the badge — which
             * carries its own "locked by the server" tooltip. */
            bool classicHovered =
                ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
            if (classicDisabled) ImGui::EndDisabled();
            if (classicLocked) lobbyRenderLockBadge();
            if (classicHovered) {
                ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_CLASSIC_MODE_TIP));
            }
            ImGui::Separator();

            for (int r = 0; r < 3; r++) {
                const ViewRow &row = rows[r];
                bool locked = (clientSimGetLobbyServerLocks(cs) & row.lockBit) != 0;
                bool disable = !effectiveHost || locked || classic;
                int policy = (int)clientSimGetViewPolicy(cs, row.cat);
                int secs   = (int)clientSimGetViewDecaySecs(cs, row.cat);
                if (secs < VIEW_DECAY_MIN_SECS) secs = VIEW_DECAY_DEFAULT_SECS;

                ImGui::PushID(row.id);
                if (disable) ImGui::BeginDisabled();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(langGetText(row.label));
                ImGui::SameLine();
                ImGui::SetNextItemWidth(120.0f * s);
                if (ImGui::Combo("##mode", &policy, modes, 4)) {
                    sendView(row.lst, policy, secs);
                }
                /* Held until after EndDisabled so the tooltip is drawn at
                 * full contrast rather than dimmed with the row. */
                bool comboHovered =
                    ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
                /* The seconds box stays on screen for every mode so the row
                 * doesn't reflow as the host tries the options; it is only
                 * interactive while the row is on Decay. */
                ImGui::SameLine();
                ImGui::BeginDisabled(policy != (int)viewPolicyDecay);
                /* VIEW_DECAY_MAX_SECS is three digits; the fourth is room
                 * to type into before the setter clamps it back. */
                ImGui::SetNextItemWidth(lobbyStepInputWidth("0000"));
                if (ImGui::InputInt("##decay", &secs, 1, 5,
                                    ImGuiInputTextFlags_EnterReturnsTrue)) {
                    sendView(row.lst, policy, secs);
                }
                ImGui::SameLine();
                ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_VIEW_DECAY_SECS));
                ImGui::EndDisabled();
                if (disable) ImGui::EndDisabled();
                if (locked) lobbyRenderLockBadge();
                /* Classic mode gets no badge — a server lock and a
                 * classic-mode grey-out are different reasons for the
                 * same disabled row, and only the lock is badged. Say
                 * why in a tooltip instead, when classic mode is the
                 * only thing holding the row. */
                if (classic && !locked && effectiveHost && comboHovered) {
                    ImGui::SetTooltip("%s",
                                      langGetText(STR_DLGLOBBY_CLASSIC_MODE_TIP));
                }
                ImGui::PopID();
            }

            /* Under the three rows because it is a plain on/off rule
             * rather than a view policy. Classic mode holds it off the
             * same way it holds the rows above: the server refuses an
             * edit while classic mode is on, so the box goes disabled
             * rather than letting the host click it for nothing. */
            bool trees = clientSimGetAlliesInTrees(cs);
            bool treesLocked =
                (clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_ALLIES_IN_TREES) != 0;
            bool treesDisabled = !effectiveHost || treesLocked || classic;
            if (treesDisabled) ImGui::BeginDisabled();
            if (ImGui::Checkbox(langGetText(STR_DLGLOBBY_ALLIES_TREES_CB),
                                &trees)) {
                uint8_t v = trees ? 1 : 0;
                lobbySendSetting(cs, LST_ALLIES_IN_TREES, &v, 1);
            }
            /* Hover read before the badge draws, as above. */
            bool treesHovered =
                ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
            if (treesDisabled) ImGui::EndDisabled();
            if (treesLocked) lobbyRenderLockBadge();
            if (treesHovered) {
                bool classicIsWhy = classic && !treesLocked && effectiveHost;
                ImGui::SetTooltip(
                    "%s", langGetText(classicIsWhy
                                          ? STR_DLGLOBBY_CLASSIC_MODE_TIP
                                          : STR_DLGLOBBY_ALLIES_TREES_TIP));
            }

            /* Which block of squares the map overview keeps live round
             * the player's own tank. A combo rather than a tick box
             * because the choice is a named mode, and the index is the
             * wire byte, so the entries are in enum order. Classic mode
             * holds it the way it holds the rows above: the server
             * refuses the edit while it is on. */
            const char *windows[] = {
                langGetText(STR_DLGLOBBY_WINDOW_EXPANDED),
                langGetText(STR_DLGLOBBY_WINDOW_CLASSIC),
            };
            int  window = (int)clientSimGetOverviewWindow(cs);
            bool windowLocked =
                (clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_OVERVIEW_WINDOW) != 0;
            bool windowDisabled = !effectiveHost || windowLocked || classic;
            ImGui::PushID("overviewwindow");
            if (windowDisabled) ImGui::BeginDisabled();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_OVERVIEW_WINDOW));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f * s);
            if (ImGui::Combo("##window", &window, windows,
                             (int)OVERVIEW_WINDOW_COUNT)) {
                uint8_t v = (uint8_t)window;
                lobbySendSetting(cs, LST_OVERVIEW_WINDOW, &v, 1);
            }
            /* Hover read before the badge draws, as above. */
            bool windowHovered =
                ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
            if (windowDisabled) ImGui::EndDisabled();
            if (windowLocked) lobbyRenderLockBadge();
            if (windowHovered) {
                bool classicIsWhy = classic && !windowLocked && effectiveHost;
                ImGui::SetTooltip(
                    "%s", langGetText(classicIsWhy
                                          ? STR_DLGLOBBY_CLASSIC_MODE_TIP
                                          : STR_DLGLOBBY_OVERVIEW_WINDOW_TIP));
            }
            ImGui::PopID();

            /* What stops the player seeing inside that block. A tick box
             * because there is one rule to turn on, though the wire
             * carries a selector so another rule can join it. */
            bool sight =
                clientSimGetLineOfSight(cs) != (uint8_t)lineOfSightOff;
            bool sightLocked =
                (clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_LINE_OF_SIGHT) != 0;
            bool sightDisabled = !effectiveHost || sightLocked || classic;
            if (sightDisabled) ImGui::BeginDisabled();
            if (ImGui::Checkbox(langGetText(STR_DLGLOBBY_LINE_OF_SIGHT_CB),
                                &sight)) {
                uint8_t v = (uint8_t)(sight ? lineOfSightBuildingsAndTrees
                                            : lineOfSightOff);
                lobbySendSetting(cs, LST_LINE_OF_SIGHT, &v, 1);
            }
            bool sightHovered =
                ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
            if (sightDisabled) ImGui::EndDisabled();
            if (sightLocked) lobbyRenderLockBadge();
            if (sightHovered) {
                bool classicIsWhy = classic && !sightLocked && effectiveHost;
                ImGui::SetTooltip(
                    "%s", langGetText(classicIsWhy
                                          ? STR_DLGLOBBY_CLASSIC_MODE_TIP
                                          : STR_DLGLOBBY_LINE_OF_SIGHT_TIP));
            }

            /* A real Close button, not just the title-bar X: ImGui's
             * NavCancel leaves modals open, so a controller needs
             * something focusable to leave by. */
            if (WBUI::DialogFooter(/*cancelLabel*/ NULL,
                                   /*confirmLabel*/ langGetText(STR_CLOSE))
                != WBUI::FOOTER_NONE) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    /* Restore the larger lobby-font scale before the Hide button so
     * its text isn't shrunk to the 0.85x the columns body uses. The
     * game-settings section's content auto-sizes to fit the columns
     * above — no explicit padding under it. */
    ImGui::SetWindowFontScale(settingsOldScale);

    /* "Hide Settings" — temporarily disabled. The form itself is
     * collapsible via its CollapsingHeader's chevron, so the
     * standalone button was redundant. Keeping the code in a
     * commented block so it's easy to restore if the section
     * tries to grow back. */
#if 0
    {
        const char *label = "Hide Settings";
        ImVec4 baseBtn = ImGui::GetStyleColorVec4(ImGuiCol_Button);
        ImVec4 baseTxt = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImVec4(baseBtn.x, baseBtn.y, baseBtn.z, baseBtn.w * 0.35f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(baseBtn.x, baseBtn.y, baseBtn.z, baseBtn.w * 0.65f));
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImVec4(baseTxt.x, baseTxt.y, baseTxt.z, baseTxt.w * 0.65f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(ImGui::GetStyle().FramePadding.x, 2.0f * s));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
        float btnW = ImGui::CalcTextSize(label).x + 16.0f * s;
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - btnW);
        if (ImGui::Button(label)) {
            s_settings.open = false;
        }
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);
    }
#endif
}

/* ── Visibility summary ───────────────────────────────────────────
 * The three view policies plus the allies-in-trees rule on one line, read
 * only: pill, base, allied tank and tank-in-forest sprite, each followed by
 * what it is set to, with the seconds added under Decay and the whole group
 * dimmed under Off. Drawn on the lobby header line, which is how a joiner
 * or spectator — who never sees the settings panel — finds out what the
 * host chose; the host reads the same line above the panel rather than a
 * second copy inside it. Hovering a group names the category and its
 * setting in full. */
void lobbyRenderVisibilitySummary(ClientSim *cs, float s) {
    /* The header line draws before the player list, which is what
     * usually loads the icon cache, so load it here first. The call is
     * idempotent and reloads the sprites if the renderer changed. */
    SDL_Renderer *renderer = sdl3DrawGetRenderer();
    if (renderer) lobbyLoadStatusIconsOnce(renderer, s);

    struct SummaryItem {
        int          label;
        ViewCategory cat;
        SDL_Texture *(*tex)(SDL_Renderer *);
    };
    static const SummaryItem items[] = {
        { STR_DLGLOBBY_VIEW_PILL, viewCategoryPill, lobbyGetPillbox15Texture },
        { STR_DLGLOBBY_VIEW_BASE, viewCategoryBase, lobbyGetBaseGoodTexture  },
        { STR_DLGLOBBY_VIEW_ALLY, viewCategoryAlly, lobbyGetTankGood04Texture },
    };
    const int words[] = {
        STR_DLGLOBBY_VIEW_ALWAYS,
        STR_DLGLOBBY_VIEW_KEY,
        STR_DLGLOBBY_VIEW_DECAY,
        STR_DLGLOBBY_VIEW_OFF,
    };

    /* Centring a sprite against its word takes two drops, not one: text is
     * drawn at the cursor plus the line's text-base offset — which the lobby
     * header line sets, because it carries framed items — while an image is
     * drawn at the cursor itself, so the sprite starts that much high before
     * either has been centred at all. On top of that whichever of the two is
     * shorter takes half the difference in height, so the pair reads as one
     * row however the font is scaled. */
    ImGuiWindow *win = ImGui::GetCurrentWindow();
    const float iconSize = 16.0f * s;
    const float textH    = ImGui::GetTextLineHeight();
    const float imgDrop  = (textH > iconSize) ? (textH - iconSize) * 0.5f : 0.0f;
    const float textDrop = (iconSize > textH) ? (iconSize - textH) * 0.5f : 0.0f;

    for (int i = 0; i < 3; i++) {
        const SummaryItem &it = items[i];
        if (i > 0) ImGui::SameLine(0, 12.0f * s);

        int policy = (int)clientSimGetViewPolicy(cs, it.cat);
        if (policy < 0 || policy > (int)viewPolicyOff) policy = (int)viewPolicyAlways;
        const char *word = langGetText(words[policy]);

        char valueText[96];
        if (policy == (int)viewPolicyDecay) {
            SDL_snprintf(valueText, sizeof(valueText), "%s %u %s", word,
                         (unsigned)clientSimGetViewDecaySecs(cs, it.cat),
                         langGetText(STR_DLGLOBBY_VIEW_DECAY_SECS));
        } else {
            SDL_snprintf(valueText, sizeof(valueText), "%s", word);
        }

        /* Off reads as "this category is switched off" by being faint,
         * rather than by a colour the icons would have to carry. */
        bool dim = (policy == (int)viewPolicyOff);
        if (dim) {
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                                ImGui::GetStyle().Alpha * 0.45f);
        }
        ImGui::BeginGroup();
        const float baseY = ImGui::GetCursorPosY();
        SDL_Texture *tex = renderer ? it.tex(renderer) : nullptr;
        if (tex) {
            ImGui::SetCursorPosY(baseY + win->DC.CurrLineTextBaseOffset + imgDrop);
            ImGui::Image((ImTextureID)tex, ImVec2(iconSize, iconSize));
        } else {
            /* Sprite missing — the category's own label stands in for it
             * so the line still says which policy belongs to what. */
            ImGui::SetCursorPosY(baseY + textDrop);
            ImGui::TextUnformatted(langGetText(it.label));
        }
        ImGui::SameLine(0, 4.0f * s);
        ImGui::SetCursorPosY(baseY + textDrop);
        ImGui::TextUnformatted(valueText);
        ImGui::EndGroup();
        if (dim) ImGui::PopStyleVar();

        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s: %s", langGetText(it.label), word);
        }
    }

    /* Allies in trees closes the line. It reads yes or no rather than a
     * policy word because it is a plain on/off rule, and its icon is an
     * allied tank standing on a forest square — the same tank sprite the
     * ally group above uses, so the pair reads as one subject. No is faint,
     * the way an Off policy is faint above. */
    {
        bool trees = clientSimGetAlliesInTrees(cs);
        const char *word = langGetText(trees ? STR_YES : STR_NO);
        ImGui::SameLine(0, 12.0f * s);
        if (!trees) {
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                                ImGui::GetStyle().Alpha * 0.45f);
        }
        ImGui::BeginGroup();
        const float baseY = ImGui::GetCursorPosY();
        SDL_Texture *forest = renderer ? lobbyGetForestTexture(renderer) : nullptr;
        SDL_Texture *tank   = renderer ? lobbyGetTankGood04Texture(renderer) : nullptr;
        if (forest && tank) {
            /* The tank goes on at three quarters of the square, centred: a
             * full-size one covers the tile it is standing in, which is the
             * one thing this icon has to show it is not doing. */
            ImGui::SetCursorPosY(baseY + win->DC.CurrLineTextBaseOffset + imgDrop);
            ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::Image((ImTextureID)forest, ImVec2(iconSize, iconSize));
            float inset = iconSize * 0.125f;
            ImGui::GetWindowDrawList()->AddImage(
                (ImTextureID)tank,
                ImVec2(at.x + inset, at.y + inset),
                ImVec2(at.x + iconSize - inset, at.y + iconSize - inset),
                ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 1.0f)));
        } else {
            /* Sprite missing — the label stands in for it, as above. */
            ImGui::SetCursorPosY(baseY + textDrop);
            ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_ALLIES_TREES_CB));
        }
        ImGui::SameLine(0, 4.0f * s);
        ImGui::SetCursorPosY(baseY + textDrop);
        ImGui::TextUnformatted(word);
        ImGui::EndGroup();
        if (!trees) ImGui::PopStyleVar();

        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s: %s",
                              langGetText(STR_DLGLOBBY_ALLIES_TREES_CB), word);
        }
    }
}
