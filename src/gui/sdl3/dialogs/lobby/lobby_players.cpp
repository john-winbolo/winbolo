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
 * Name:          lobby_players.cpp
 * Purpose:       The lobby's team-grouped player list. The
 *                team headers with their naming pool and
 *                add/remove bot controls, the per-row
 *                columns (tank badge, name and tags, gear,
 *                ping, start, ready pill and the remove /
 *                kick X), and the drag-and-drop that moves
 *                a player between teams. The bot AI-config
 *                sub-row a row's gear expands. The
 *                spectator group below the teams. The kick
 *                and make-host confirmations. The
 *                ranked-shape eligibility check and its
 *                tooltip, shared with the Ready button. And
 *                the "Allow New Players" row above the
 *                list, which carries the Ranked and
 *                auto-lock toggles and the team-balance
 *                popup.
 *********************************************************/

#include <cstring>  /* memcpy / strncpy — name truncation, bot-name edit buffer */

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_internal.h"  /* ImGui::CloseButton, GetCurrentWindow, ImMax */
#include "lobby_internal.h"
#include "dialog_footer.h"   /* WBUI cancel styling + CancelKeyPressed */
#include "imgui_keysetup.h"  /* imguiKeySetupOpenInGame — the push-to-talk row's Change button */
#include "imgui_settings.h"  /* imguiSettingsVoice* — the shared voice controls */
#include "../../wb_theme.h"  /* g_theme / wbThemeColor — team tints, tag pills */
extern "C" {
#include "client_sim.h"      /* ClientSim + lobby getters; MAX_TANKS, BrainList, PingBand */
#include "client_net.h"      /* clientSimNetSend* — kick, host transfer, claim start */
#include "bolo_rand.h"       /* bolo_rand_below — random pool for a team's first bot */
#include "lobby_bot_pools.h" /* lobbyBotPoolCount / Label / Pick */
#include "start_sides.h"     /* START_SIDE_* / startSideBits / Accepts / Eligible / IsCentre — team start sides */
#include "client_command.h"  /* START_CLAIM_TEAM_SIDE — the start dropdown's Team side action */
#include "../../lobby_start_markers.h"  /* lobbyStartHolderSlot; lobbyTeamSide, lobbySideNameId / CompassId, lobbyClosedMaskForTeam */
#include "../../../../bolo/public/wire_limits.h"  /* LOBBY_LOCK_* / LST_* */
#include "../../../lang.h"   /* langGetText / MessageArgs / STR_*; PLAYER_FLAG_* */
#include "../../../gamefront.h"  /* gameFrontSetChosenBotDifficulty */
#include "../../../ui_mode.h"    /* uiShouldUseControllerMode */
#include "../../sdl3draw.h"      /* sdl3DrawGetRenderer */
#include "../../sdl3imgui.h"     /* renderPlayerName / drawCountryFlagWithTip */
#include "../../flags.h"         /* flagsGetTexture / FLAG_HEIGHT */
#include "../../map_preview_popup.h"  /* mapPreviewPopupFocusMapSquare */
#include "../../../../winbolonet/winbolonet_core.h"  /* winbolonetIsRunning */
#if defined(WINBOLO_VOICE)
#include "../../../voice.h"   /* voiceGetTalkingMap — lobby mic icons;
                               * the own-row voice sub-row's state */
#endif
}

#if defined(WINBOLO_VOICE)
extern "C" {
  /* Voice apply/persist helpers — winbolo.c on the desktop, main_wasm.c in the
     browser build. No header declares them; each caller names the ones it
     uses, the same way imgui_settings.cpp does. */
  void windowSetVoiceEnabled(bool on);
  void windowSetVoiceVolume(float gain);
}
#endif

/* Player-list state: bot-row expansion, the tab-cycle's forced selection
 * and the two per-row confirm dialogs. */
typedef struct LobbyPlayersState {
    /* Currently-expanded bot slot for the AiConfig sub-row, or -1. */
    int expandedBotSlot = -1;

    /* Whether the local player's voice sub-row is open. A bool rather than a
     * slot index like the bots': there is only ever one own row. */
    bool voiceRowExpanded = false;

    /* Tab the trigger/shoulder tab-cycle wants selected next frame in the
     * tabbed lobby layout, or -1 for "no forced selection". Set from the
     * LT/RT (or native L1/R1) shift, applied via ImGuiTabItemFlags_SetSelected,
     * then cleared after the tab bar. */
    int forceTab = -1;

    /* Kick-confirm dialog state. Populated when an authorised player picks
     * "Kick" from a row's right-click context menu; the modal at the bottom
     * of lobbyRenderTeamGroupedPlayers reads it on the next frame. */
    int  kickPendingSlot     = -1;
    char kickPendingName[64] = {0};
    bool kickPendingOpen     = false;
    int  makeHostPendingSlot     = -1;
    char makeHostPendingName[64] = {0};
    bool makeHostPendingOpen     = false;
} LobbyPlayersState;

static LobbyPlayersState s_players = {};

/* Core sets this from the shoulder tab-cycle and clears it after the tab bar.
 * -1 means "no forced selection". LobbyChooserState has a field of the same
 * name and type; that one is chooser-internal and has no accessor. */
int *lobbyPlayersForceTab(void) {
    return &s_players.forceTab;
}

void lobbyPlayersReset(void) {
    s_players = LobbyPlayersState{};
}

/* Forward decl — defined below the team renderer. */
static void renderBotAiConfig(ClientSim *cs,
                              int slot, int teamId, float s);
#if defined(WINBOLO_VOICE)
static void renderOwnVoiceConfig(ClientSim *cs, float s, float contentW);
#endif

/* ── Layout A — team-grouped player list ──────────────────────────
 * Renders players grouped under team headers with color tints from
 * WbTheme. Replaces the flat 5-column table with the mockup's
 * "team containers" model. Sized to fit inside the calling child
 * window. Returns nothing — purely UI. */
void lobbyRenderLockBadge(void);

LobbyRankedEligibility lobbyComputeRankedEligibility(ClientSim *cs) {
    LobbyRankedEligibility r = {false, 0, 0, 0};
    int teamSizes[17] = {0};
    for (int i = 0; i < MAX_TANKS; i++) {
        const ClientLobbySlot *ls = clientSimGetLobbySlot(cs, (BYTE)i);
        if (!ls || !ls->connected || ls->isBot) continue;
        uint8_t t = ls->teamNumber;
        if (t == 0 || t > 16) continue;
        if (teamSizes[t] == 0) r.teamsInUse++;
        teamSizes[t]++;
    }
    for (int t = 1; t <= 16; t++) {
        if (teamSizes[t] == 0) continue;
        if (r.firstSize == 0) r.firstSize = teamSizes[t];
        else                  r.secondSize = teamSizes[t];
    }
    r.sizesEligible = (r.teamsInUse == 2) &&
                      (r.firstSize == r.secondSize) &&
                      (r.firstSize >= 1 && r.firstSize <= 3);
    return r;
}
void lobbyRankedShapeTooltip(const LobbyRankedEligibility &r) {
    MessageArgs args = {};
    args.number  = r.teamsInUse;
    SDL_strlcpy(args.string1,
                langGetText(r.teamsInUse == 1 ? STR_DLGLOBBY_RANKED_SHAPE_TEAM
                                               : STR_DLGLOBBY_RANKED_SHAPE_TEAMS),
                sizeof(args.string1));
    args.number2 = r.firstSize;
    args.number3 = r.secondSize;
    ImGui::SetTooltip("%s",
        langGetTextFmt(STR_DLGLOBBY_RANKED_SHAPE_TIP, &args));
}

/* True when myPlayerNum holds the lobby host role. Keeps the
 * host-identity test uniform across the lobby UI. */
bool lobbyIsHost(ClientSim *cs, int myPlayerNum) {
    return myPlayerNum >= 0 && myPlayerNum == clientSimGetLobbyHostSlot(cs);
}

/* ── Team start sides ─────────────────────────────────────────────
 * The lobby mirror carries each team's START_SIDE_* choice and every
 * slot's reservation; the map preview cache carries the start positions
 * and their bounding box. Between them the header selector and the row
 * start cell can answer the questions the server's lobby start pick
 * answers, with one gap: the client cannot see which start squares are
 * deep sea, so "valid" here means on the map's start list and not held
 * by a connected slot. What the cell shows is a forecast; the server's
 * answer is the reservation that lands in CTRL_LOBBY_SLOT.
 *
 * The side rules themselves — lobbyTeamSide, lobbyClosedMaskForTeam and
 * the side name and letter ids — are the shared helpers in
 * lobby_start_markers.h, so the map previews read the same ones; the
 * cache accessor lobbyStartSideMask lives with the cache in
 * lobby_map_preview.cpp. */

/* Starts a side would offer a team: for a side, every start it accepts
 * (its own side plus the centre band); for Any, every start outside the
 * sides the other teams chose. Holders are not subtracted — this is the
 * selector tooltip's "N starts for M players" figure. */
static int lobbyCountSideStarts(ClientSim *cs, int teamId, BYTE side) {
    const LobbyMapPreviewState *mp = lobbyMapPreview();
    BYTE closedMask = lobbyClosedMaskForTeam(cs, teamId);
    int n = 0;
    for (int k = 1; k <= (int)mp->startCount; k++) {
        if (startSideEligible(lobbyStartSideMask(k), side, closedMask)) n++;
    }
    return n;
}

/* True while a start the slot could still be placed on is free: on the
 * cached start list, eligible for its team's side given the other teams'
 * sides, and held by no connected slot other than this one. */
static bool lobbyFreeEligibleStartExists(ClientSim *cs, int slot, int teamId) {
    const LobbyMapPreviewState *mp = lobbyMapPreview();
    BYTE side       = lobbyTeamSide(cs, teamId);
    BYTE closedMask = lobbyClosedMaskForTeam(cs, teamId);
    for (int k = 1; k <= (int)mp->startCount; k++) {
        if (!startSideEligible(lobbyStartSideMask(k), side, closedMask)) continue;
        int holder = lobbyStartHolderSlot(cs, k);
        if (holder < 0 || holder == slot) return true;
    }
    return false;
}

/* Tooltip for a side in the header's Start: selector — the starts that
 * side offers the team against its member count, how many members that
 * leaves to start at sea, and that a side change re-picks everyone. */
static void lobbyTeamSideTooltip(ClientSim *cs, int teamId, BYTE side, int members) {
    int starts = lobbyCountSideStarts(cs, teamId, side);
    int sea    = members - starts;
    if (sea < 0) sea = 0;
    MessageArgs args = {};
    SDL_strlcpy(args.string1, langGetText(lobbySideNameId(side)), sizeof(args.string1));
    args.number  = starts;
    args.number2 = members;
    args.number3 = sea;
    ImGui::SetTooltip("%s", langGetTextFmt(STR_DLGLOBBY_TOOLTIP_TEAM_SIDE, &args));
}

/* Compact "Allow New Players:  [ ] Now   [ ] During game" row. Host
 * only and multiplayer only (single-player has no UDP listener). Used
 * to live inside lobbyRenderTeamGroupedPlayers; hoisted to the parent so
 * the PlayerPanel and MapPanel top edges stay aligned. */
void lobbyRenderAllowNewPlayersRow(ClientSim *cs,
                                     int myPlayerNum, float s) {
    const bool spectator = clientSimIsSpectator(cs);
    bool isHost = lobbyIsHost(cs, myPlayerNum);
    bool isLocalAdmin = (myPlayerNum >= 0 && myPlayerNum < MAX_TANKS &&
                        (clientSimGetLobbySlot(cs, (BYTE)(myPlayerNum))->clientFlags
                         & PLAYER_FLAG_ADMIN));
    bool effectiveHost = !spectator && (isHost || clientSimGetLobbyOpenHost(cs) || isLocalAdmin);
    /* Diagnostic: log entry state on every call, throttled to changes
     * only. Fires BEFORE the early-return so we can see whether the
     * function is reached at all and which condition trips the
     * early-out. */
    /* Row stays visible to non-host players too so they can see the
     * Ranked Game indicator — but the "Allow New Players" controls
     * and the Ranked toggle itself stay disabled for them. */
    if (!clientSimHasTransport(cs) || clientSimIsSinglePlayer(cs)) return;

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Allow New Players:");
    if (!effectiveHost) ImGui::BeginDisabled();
    bool allowJoin = clientSimGetLobbyAllowNewPlayers(cs);
    ImGui::SameLine();
    char allowNowId[64];
    SDL_snprintf(allowNowId, sizeof(allowNowId), "%s##allowNow", langGetText(STR_DLGLOBBY_ALLOW_NOW));
    if (ImGui::Checkbox(allowNowId, &allowJoin)) {
        clientSimNetSendLockToggle(cs, allowJoin);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_TOOLTIP_ALLOWNOW));
    }
    bool autoLockLocked = (clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_AUTO_LOCK_ON_GAME) != 0;
    bool rankedForcesAutoLock = clientSimGetLobbyRanked(cs);
    bool duringGame = !clientSimGetLobbyAutoLockOnGameStart(cs);
    if (rankedForcesAutoLock) duringGame = false;
    bool autoLockDisabled = autoLockLocked || rankedForcesAutoLock;
    if (autoLockDisabled) ImGui::BeginDisabled();
    ImGui::SameLine();
    char allowDuringId[64];
    SDL_snprintf(allowDuringId, sizeof(allowDuringId), "%s##allowDuring", langGetText(STR_DLGLOBBY_ALLOW_DURING));
    if (ImGui::Checkbox(allowDuringId, &duringGame)) {
        uint8_t v = duringGame ? 0 : 1;  /* invert */
        lobbySendSetting(cs, LST_AUTO_LOCK_ON_GAME, &v, 1);
    }
    if (autoLockDisabled) ImGui::EndDisabled();
    if (autoLockLocked) {
        ImGui::SameLine(0.0f, 4.0f * s);
        lobbyRenderLockBadge();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (rankedForcesAutoLock) {
            ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_TOOLTIP_RANKED_AUTOLOCK));
        } else if (!autoLockLocked) {
            ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_TOOLTIP_ALLOWDURING));
        }
    }
    if (!effectiveHost) ImGui::EndDisabled();

    /* "Ranked game" — sits to the right of the Allow-New-Players
     * controls so it's prominent on the lobby's top row. Visible to
     * every player so newcomers can see whether they're walking into
     * a ranked match; only host / admin / openHost-empowered players
     * can toggle. Server enforces the actual bots-off / no-Open-type
     * constraints regardless of who tries to flip the related
     * controls. Toggling the flag also clears everyone's ready bit
     * via the existing auto-unready broadcast path.
     *
     * Ranked is a winbolo.net feature (results are reported to WBN
     * for ladder ranking and the tracker advertises the flag), so
     * the toggle is hidden entirely in LAN / direct-IP / single-
     * player games AND in Internet games where the host process
     * isn't signed in to WBN. Non-host clients learn the host's
     * WBN-availability via lobbyWbnAvailable in CTRL_LOBBY_SETTINGS. */
    if (!clientSimIsSinglePlayer(cs) && !clientSimIsLanOnly(cs) &&
        clientSimGetLobbyWbnAvailable(cs)) {
        ImGui::SameLine(0.0f, 16.0f * s);
        bool rankedV = clientSimGetLobbyRanked(cs);

        /* Ranked toggle policy:
         *   - Host-only.
         *   - Blocked while bots are present so the host explicitly
         *     removes them first (no silent kick on flip-on).
         *   - Always allow turning Ranked OFF, even if the shape
         *     drifted while it was on.
         *   - Shape (1v1 / 2v2 / 3v3) is NOT a gate here — the host
         *     may want the lobby flagged Ranked so it shows up under
         *     that filter in the games list while players join.
         *     Eligibility is enforced at start time by disabling the
         *     Ready button (server silently drops bad Ready requests). */
        int botCount = 0;
        for (int i = 0; i < MAX_TANKS; i++) {
            const ClientLobbySlot *ls = clientSimGetLobbySlot(cs, (BYTE)i);
            if (ls && ls->connected && ls->isBot) botCount++;
        }
        bool botsBlock = (botCount > 0) && !rankedV;
        LobbyRankedEligibility re = lobbyComputeRankedEligibility(cs);

        bool rankedLocked = (clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_RANKED) != 0;
        bool canToggle = effectiveHost && !botsBlock && !rankedLocked;
        if (!canToggle) ImGui::BeginDisabled();
        char rankedId[64];
        SDL_snprintf(rankedId, sizeof(rankedId), "%s##ranked", langGetText(STR_DLGLOBBY_RANKED));
        if (ImGui::Checkbox(rankedId, &rankedV)) {
            uint8_t v = rankedV ? 1 : 0;
            lobbySendSetting(cs, LST_RANKED, &v, 1);
        }
        if (!canToggle) ImGui::EndDisabled();
        if (rankedLocked) lobbyRenderLockBadge();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (rankedLocked) {
                ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_TOOLTIP_RANKED_LOCKED));
            } else if (!effectiveHost) {
                ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_TOOLTIP_RANKED_NOTHOST));
            } else if (botsBlock) {
                ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_TOOLTIP_RANKED_BOTS));
            } else if (rankedV && !re.sizesEligible) {
                lobbyRankedShapeTooltip(re);
            } else {
                ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_TOOLTIP_RANKED_INFO));
            }
        }
    }

    /* "Balance from WBN" — sits to the right of the Ranked checkbox.
     * Host-only. Opens a confirm popup explaining that teams will be
     * cleared and replaced with two skill-balanced teams; if bots are
     * present the popup offers a third button to kick them so the
     * matchup is pure-human. After confirm, the existing
     * BALANCE_REQUEST -> BALANCE_PROPOSAL -> Apply/Dismiss flow runs
     * normally (proposal shows on every client; host applies or
     * dismisses). */
    {
        int connectedCount = 0;
        int botCount = 0;
        for (int i = 0; i < MAX_TANKS; i++) {
            const ClientLobbySlot *ls = clientSimGetLobbySlot(cs, (BYTE)i);
            if (!ls || !ls->connected) continue;
            connectedCount++;
            if (ls->isBot) botCount++;
        }
        bool proposalActive = clientSimIsBalanceProposalActive(cs);
        bool enoughForBalance = (connectedCount >= 2);
        bool canBalance = effectiveHost && enoughForBalance && !proposalActive;
        char kBalancePopup[64];
        SDL_snprintf(kBalancePopup, sizeof(kBalancePopup), "%s##balwbn",
                     langGetText(STR_DLGLOBBY_BAL_POPUP_TITLE));

        /* Status feedback for the most recent Balance request — read
         * after the popup confirm sets s_balReqSentMs, displayed in
         * a small label next to the outer Balance button. */
        static uint64_t   s_balReqSentMs         = 0;
        static const char *s_balLastResultText   = NULL;
        static ImVec4     s_balLastResultColor   = ImVec4(1, 1, 1, 1);
        static uint64_t   s_balLastResultUntilMs = 0;

        /* Balance-from-WBN visibility:
         *   - Hidden in SP / LAN-only (WBN never runs there).
         *   - Hidden when the host process isn't signed in to WBN
         *     (server's wbnRunning guard would drop every click).
         *   - Hidden for non-host / non-admin clients — only the
         *     player who can act on the proposal needs to see the
         *     request affordance. effectiveHost covers host + admin
         *     + openHost-empowered slots. */
        if (effectiveHost &&
            !clientSimIsSinglePlayer(cs) && !clientSimIsLanOnly(cs) &&
            clientSimGetLobbyWbnAvailable(cs)) {
        ImGui::SameLine(0.0f, 12.0f * s);
        if (!canBalance) ImGui::BeginDisabled();
        char balBtnLabel[64];
        SDL_snprintf(balBtnLabel, sizeof(balBtnLabel), "%s##balwbn_btn",
                     langGetText(STR_DLGLOBBY_BAL_BTN));
        bool outerClicked = ImGui::Button(balBtnLabel);
        if (!canBalance) ImGui::EndDisabled();

        /* Short status to the right of the button so the host gets
         * feedback on the last Balance request. s_balReqSentMs is
         * set when the popup's confirm button fires; the live
         * "Asking WBN…" message shows for up to 8 s while we wait.
         * On a successful proposal we flip to "Proposal ready" for
         * 6 s; on timeout we surface "No response from WBN" for 8 s
         * so the host knows the request didn't land. */
        {
            uint64_t now = SDL_GetTicks();
            const char *liveText = NULL;
            ImVec4 liveColor;
            if (s_balReqSentMs != 0) {
                uint64_t arrivedMs =
                    clientSimGetLastBalanceProposalArrivedMs(cs);
                uint64_t failedMs =
                    clientSimGetLastBalanceFailedMs(cs);
                if (arrivedMs != 0 && arrivedMs >= s_balReqSentMs) {
                    /* Server auto-applies the split now, so we use the
                     * one-shot timestamp the dispatcher latches when the
                     * proposal event arrives — proposalActive itself is
                     * cleared on the same mutex hold, so reading it
                     * here would always miss the success transition. */
                    s_balLastResultText  = langGetText(STR_DLGLOBBY_BAL_STATUS_BALANCED);
                    s_balLastResultColor = ImVec4(0.4f, 0.8f, 0.4f, 1.0f);
                    s_balLastResultUntilMs = now + 6000;
                    s_balReqSentMs = 0;
                } else if (failedMs != 0 && failedMs >= s_balReqSentMs) {
                    /* Server told us the WBN call returned without a
                     * usable proposal — flip to the failure pill now
                     * instead of waiting out the 8 s NOREPLY clock. */
                    s_balLastResultText  = langGetText(STR_DLGLOBBY_BAL_STATUS_FAILED);
                    s_balLastResultColor = ImVec4(0.9f, 0.4f, 0.3f, 1.0f);
                    s_balLastResultUntilMs = now + 8000;
                    s_balReqSentMs = 0;
                } else if (now - s_balReqSentMs < 8000) {
                    liveText  = langGetText(STR_DLGLOBBY_BAL_STATUS_ASKING);
                    liveColor = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
                } else {
                    s_balLastResultText  = langGetText(STR_DLGLOBBY_BAL_STATUS_NOREPLY);
                    s_balLastResultColor = ImVec4(0.9f, 0.4f, 0.3f, 1.0f);
                    s_balLastResultUntilMs = now + 8000;
                    s_balReqSentMs = 0;
                }
            }
            const char *showText = liveText ? liveText :
                (s_balLastResultText && now < s_balLastResultUntilMs)
                    ? s_balLastResultText : NULL;
            ImVec4 showColor = liveText ? liveColor : s_balLastResultColor;
            if (showText) {
                ImGui::SameLine(0.0f, 8.0f * s);
                ImGui::TextColored(showColor, "%s", showText);
            }
        }
        if (outerClicked) {
            ImGui::OpenPopup(kBalancePopup);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (!effectiveHost) {
                ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_BAL_TOOLTIP_NOTHOST));
            } else if (!enoughForBalance) {
                ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_BAL_TOOLTIP_NOTENOUGH));
            } else {
                ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_BAL_TOOLTIP_INFO));
            }
        }

        /* The confirm popup body. ImGui::BeginPopupModal is the modal
         * variant — it dims everything underneath and traps focus
         * until the user picks a button. */
        ImGui::SetNextWindowSize(ImVec2(420.0f * s, 0.0f), ImGuiCond_Appearing);
        static bool s_balOpen = true; s_balOpen = true;
        if (ImGui::BeginPopupModal(kBalancePopup, &s_balOpen,
                                    ImGuiWindowFlags_AlwaysAutoResize
                                    | ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::TextWrapped("%s", langGetText(STR_DLGLOBBY_BAL_POPUP_BODY));
            if (botCount > 0) {
                ImGui::Spacing();
                if (botCount == 1) {
                    ImGui::TextWrapped("%s", langGetText(STR_DLGLOBBY_BAL_POPUP_BOT_SINGULAR));
                } else {
                    MessageArgs args = {};
                    args.number = botCount;
                    ImGui::TextWrapped("%s", langGetTextFmt(STR_DLGLOBBY_BAL_POPUP_BOT_PLURAL, &args));
                }
            }
            ImGui::Spacing();

            float btnW = 140.0f * s;
            float btnH = 0.0f;
            WBUI::PushCancelStyle();
            bool balCancel = ImGui::Button("Cancel##balcancel", ImVec2(btnW, btnH));
            WBUI::PopCancelStyle();
            if (balCancel || WBUI::CancelKeyPressed()) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            uint8_t teamSize = (uint8_t)(connectedCount / 2);
            if (teamSize == 0) teamSize = 1;
            if (botCount > 0) {
                char balBotsLabel[64], balHumanLabel[64];
                SDL_snprintf(balBotsLabel,  sizeof(balBotsLabel),  "%s##balbots",
                             langGetText(STR_DLGLOBBY_BAL_BOTS_INCLUDED));
                SDL_snprintf(balHumanLabel, sizeof(balHumanLabel), "%s##balhuman",
                             langGetText(STR_DLGLOBBY_BAL_HUMANS_ONLY));
                if (ImGui::Button(balBotsLabel, ImVec2(btnW, btnH))) {
                    s_balReqSentMs = SDL_GetTicks();
                    s_balLastResultText = NULL;
                    clientSimNetSendBalanceRequest(cs, teamSize, /*includeBots=*/true);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button(balHumanLabel, ImVec2(btnW, btnH))) {
                    s_balReqSentMs = SDL_GetTicks();
                    s_balLastResultText = NULL;
                    clientSimNetSendBalanceRequest(cs, teamSize, /*includeBots=*/false);
                    ImGui::CloseCurrentPopup();
                }
            } else {
                char balGoLabel[64];
                SDL_snprintf(balGoLabel, sizeof(balGoLabel), "%s##balgo",
                             langGetText(STR_DLGLOBBY_BAL_GO));
                if (ImGui::Button(balGoLabel, ImVec2(btnW, btnH))) {
                    s_balReqSentMs = SDL_GetTicks();
                    s_balLastResultText = NULL;
                    clientSimNetSendBalanceRequest(cs, teamSize, /*includeBots=*/false);
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
        } /* end if (winbolonetIsRunning()) — Balance-from-WBN gating */
    }
}

/* Copy src into out, truncating with a trailing "..." if it's wider than
 * maxW pixels. Keeps long player names from overflowing the name column and
 * pushing the start dropdown into the Ready button on small windows. */
static void lobbyTruncateName(const char *src, float maxW, char *out, size_t outSz) {
    if (outSz == 0) return;
    if (!src) { out[0] = '\0'; return; }
    if (maxW <= 1.0f || ImGui::CalcTextSize(src).x <= maxW) {
        SDL_strlcpy(out, src, outSz);
        return;
    }
    float budget = maxW - ImGui::CalcTextSize("...").x;
    int len = (int)SDL_strlen(src);
    int n = 0;
    float w = 0.0f;
    while (n < len) {
        float cw = ImGui::CalcTextSize(src + n, src + n + 1).x;
        if (w + cw > budget) break;
        w += cw;
        n++;
    }
    if (n > (int)outSz - 4) n = (int)outSz - 4;
    if (n < 0) n = 0;
    memcpy(out, src, (size_t)n);
    out[n] = '\0';
    SDL_strlcat(out, "...", outSz);
}

/* Connected spectators, rendered as their own names list below the
 * team groups. Spectators are tankless watchers — no team, ready
 * state, ping, or host controls — so each row is just the badges a
 * player gets (country flag + WBN/Steam/platform icons) followed by
 * the name. Reads the client-side spectator roster mirrored from the
 * server's per-spectator CTRL_SPECTATOR_SLOT events. Renders nothing
 * when nobody is watching.
 *
 * myPlayerNum is accepted for symmetry with the player renderers; a
 * spectating viewer occupies no player slot, so there is no "you"
 * highlight here. The roster is walked by index until the accessor
 * returns NULL — that NULL is the bound (the roster size constant
 * lives in an internal transport header the GUI does not include). */
static void renderSpectatorGroup(ClientSim *cs, int myPlayerNum, float s) {
    (void)myPlayerNum;
    (void)s;

    int count = 0;
    for (int idx = 0; ; idx++) {
        const ClientSpectatorSlot *sp = clientSimGetSpectatorSlot(cs, (uint8_t)idx);
        if (sp == NULL) break;
        if (sp->connected) count++;
    }
    if (count == 0) return;

    /* Header in the Unassigned-tray style: disabled text with a count. */
    {
        MessageArgs args = {};
        args.number = count;
        ImGui::TextDisabled("%s", langGetTextFmt(STR_DLGLOBBY_SPECTATORS_FMT, &args));
    }

    for (int idx = 0; ; idx++) {
        const ClientSpectatorSlot *sp = clientSimGetSpectatorSlot(cs, (uint8_t)idx);
        if (sp == NULL) break;
        if (!sp->connected) continue;

        ImGui::Bullet();

        /* Country flag, guarded exactly like the player rows: skip the
         * empty and "XX" unknown sentinels, and only draw when the flag
         * texture is available. */
        const char *cc = sp->countryCode;
        if (cc[0] != '\0' && !(cc[0] == 'X' && cc[1] == 'X') && flagsGetTexture(cc)) {
            drawCountryFlagWithTip(cc);
            ImGui::SameLine();
        }

        /* WBN/Steam/platform badges. Mask the WBN globe in single-player
         * / LAN-only sessions just like the player rows — those sessions
         * have no WBN identity to vouch for. renderPlayerName with an
         * empty name draws only the badge run and leaves the cursor on
         * the same line, so the name text follows inline. */
        uint8_t pflags = sp->clientFlags;
        if (clientSimIsSinglePlayer(cs) || clientSimIsLanOnly(cs)) {
            pflags &= ~PLAYER_FLAG_WBN_VERIFIED;
        }
        renderPlayerName(NULL, pflags, sp->clientType, "", false);

        ImGui::Text("%s", sp->playerName);
    }
}

/* True when the lobby's right panel is currently showing the replay reel
 * rather than the map. Defined below, next to the recap view statics it
 * reads; declared here because the player list is rendered before them. */
bool lobbyRecapReelVisible(ClientSim *cs);

/* Clicking a player's name jumps to that player in whichever view is on
 * screen — the replay reel in the post-game recap, the map preview otherwise.
 *
 * Which view: the inline ##MapPanel preview is a fixed fit of the whole map
 * bounding box and has no camera — every start is already on screen there
 * and there is nothing to centre — so the jump drives the zoom popup, which
 * does have a camera and is already what clicking the inline preview opens.
 * Position data is the lobby slot's claimed start index (1-based, 0xFF when
 * unclaimed) resolved through the start cache lobbyRebuildStartCompassCache
 * fills from the map bytes, so no extra decompression happens per frame.
 *
 * Call immediately after the name text. Hover and click are tested on that
 * text item instead of an overlaid InvisibleButton, so the trailing tag-pill
 * SameLine chain and every other control in the row (gear, ping, kick, team
 * and start dropdowns) keep their own hit areas — a real widget overlapping
 * the name wins the hover test, which is the behaviour we want.
 *
 * The affordance is an underline plus the hand cursor rather than a tooltip:
 * it needs no new string, so nothing ships untranslated. Players with no
 * claimed start get no affordance at all, since there is nowhere to go. */
static void lobbyNameJumpToPlayer(ClientSim *cs, int slot) {
    const ClientLobbySlot *ls = clientSimGetLobbySlot(cs, (BYTE)slot);
    if (!ls || !ls->connected) return;

    /* The jump targets whatever view the player is actually looking at, so
     * "show me them" always moves the thing on screen. The right panel is
     * showing the replay reel exactly when the recap is up and its Map tab is
     * not — the negation of the panel's own showMapPanel — in which case the
     * reel travels to their tank, the same follow the stats-table rows use.
     * On the map view (Map tab chosen, or an ordinary pre-game lobby with no
     * recap at all) it stays the map-preview popup jump to their claimed
     * start. */
    const bool reelView = lobbyRecapReelVisible(cs);

    if (reelView) {
        /* No tank in the replay right now is the reel's existing no-target
         * case: no affordance, nothing moves. */
        if (!ls->playerName[0]) return;
        if (!ImGui::IsItemHovered()) return;
#if !BOLO_MOBILE
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImVec2 rmn = ImGui::GetItemRectMin();
        ImVec2 rmx = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(rmn.x, rmx.y - 1.0f), ImVec2(rmx.x, rmx.y - 1.0f),
            ImGui::GetColorU32(ImGuiCol_Text), 1.0f);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            lvEmbedFocusPlayerByName(ls->playerName);
        }
#endif /* !BOLO_MOBILE */
        return;
    }

    if (!lobbyMapPreview()->popupCompressedData || lobbyMapPreview()->popupCompressedLen <= 0) return;
    int start1 = (int)ls->startIdx;
    if (start1 < 1 || start1 > (int)lobbyMapPreview()->startCount || start1 > MAX_STARTS) return;
    if (!ImGui::IsItemHovered()) return;

    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImVec2 mn = ImGui::GetItemRectMin();
    ImVec2 mx = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(mn.x, mx.y - 1.0f), ImVec2(mx.x, mx.y - 1.0f),
        ImGui::GetColorU32(ImGuiCol_Text), 1.0f);

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        int sqX = (int)lobbyMapPreview()->startMapX[start1];
        int sqY = (int)lobbyMapPreview()->startMapY[start1];
        /* Bounds are only the pre-parse framing hint; the explicit centre in
         * mapPreviewPopupFocusMapSquare overrides them once the map parses,
         * so a tight box around the target is all this needs. */
        mapPreviewPopupFocusMapSquare(lobbyMapPreview()->popupCompressedData, lobbyMapPreview()->popupCompressedLen,
                                      sqX - 8, sqY - 8, sqX + 8, sqY + 8,
                                      sqX, sqY);
    }
}

/* Clicking a row in the post-game recap's stats table zooms the replay reel
 * onto that player's tank — the video overview, not the map preview. (The
 * left-hand player-list name click keeps going to the map preview popup; the
 * two entry points answer different questions and deliberately differ.)
 *
 * Position source: the logviewer's own live player table, which carries every
 * tank's map square and sub-tile pixel at the current playback moment. That is
 * strictly better than anything the recap summary has — RoundPlayerSummary is
 * counters only, no coordinates — and it means the view tracks the player as
 * playback continues rather than jumping once and going stale.
 *
 * The bridge is the player NAME: the lobby's slot numbering and the log's
 * player numbering are separate spaces, and the name is the only key both
 * carry. Bots are included; they have tanks in the replay like anyone else.
 *
 * No tank at the current replay time (dead, or not yet joined) — the reel is
 * left exactly where the player had it, rather than being thrown at (0,0) or
 * at a stale last-known spot that no longer shows anything. Scrubbing to a
 * moment where they are alive and clicking again then works. */
void lobbyRecapRowJump(ClientSim *cs, int slot, bool isBot) {
    (void)isBot;
    if (slot < 0 || slot >= MAX_TANKS) return;
    const ClientLobbySlot *ls = clientSimGetLobbySlot(cs, (BYTE)slot);
    if (!ls || !ls->playerName[0]) return;
#if !BOLO_MOBILE
    lvEmbedFocusPlayerByName(ls->playerName);
#endif /* !BOLO_MOBILE */
}

void lobbyRenderTeamGroupedPlayers(ClientSim *cs,
                                     int myPlayerNum, float s, bool isHost) {
    const bool spectator = clientSimIsSpectator(cs);
    /* Lazy-load the badge / bot-cpu icons. Used to be done inside
     * lobbyRenderConnectivityBadge, but we now skip that in SP / LAN-only
     * mode where the badge has nothing to report — the bot-cpu PNGs
     * still need to come up though, so trigger it here too. The
     * helper is idempotent (s_icons.attempted guard). */
    {
        SDL_Renderer *r = sdl3DrawGetRenderer();
        if (r) lobbyLoadStatusIconsOnce(r, s);
    }

    /* Helper to count members per team for header strings. */
    int memberCount[16] = {0};
    int botCount[16]    = {0};
    int unassignedCount = 0;
    for (int i = 0; i < MAX_TANKS; i++) {
        if (!clientSimGetLobbySlot(cs, (BYTE)(i))->connected) continue;
        uint8_t t = clientSimGetLobbySlot(cs, (BYTE)(i))->teamNumber;
        if (t == 0) {
            unassignedCount++;
        } else if (t < 16) {
            memberCount[t]++;
            if (clientSimGetLobbySlot(cs, (BYTE)(i))->isBot) botCount[t]++;
        }
    }

    /* Walk teams 1..15, render those with members. Then unassigned. */
    bool isLocalAdmin = (myPlayerNum >= 0 && myPlayerNum < MAX_TANKS &&
                         (clientSimGetLobbySlot(cs, (BYTE)(myPlayerNum))->clientFlags
                          & PLAYER_FLAG_ADMIN));
    bool effectiveHost = !spectator && (isHost || clientSimGetLobbyOpenHost(cs) || isLocalAdmin);

    /* "Allow new players" row is rendered by the caller above the panels
     * so the PlayerPanel and MapPanel top edges stay aligned in Y.
     * "+ Add Team" lives in the footer below the teams list (see
     * the bottom of this function). */
    (void)effectiveHost;  /* still used by the footer "Add Team" below */

    /* Start-picker column alignment. The per-row start dropdown is
     * centered in the horizontal gap between the right edge of the
     * widest player name(+HOST/ADMIN/BOT tag) across every row and the
     * left edge of the Ready/Not-ready cell, so all the dropdowns line
     * up in one column for everyone. Measured this frame, applied the
     * next (a one-frame lag is invisible for a near-static lobby). */
    static float s_startComboCenterX = 0.0f;
    static float s_startComboReadyLeft = 0.0f;  /* Ready cell left edge (last frame) */
    /* Screen X the local player's VOICE gear last drew at, and the ImGui frame
     * it drew on. A bot's config gear lines up with it, so the two sit in one
     * column down the list instead of each landing where its own badge run
     * ended — a bot row's run is one badge, a human's is four or five.
     *
     * Deriving the X from the column's width instead does NOT work: the column
     * reserves room for a flag and every badge, and a real row usually draws
     * fewer than that, so the voice gear sits left of the reserved slot. That
     * put the bot gear about five to ten pixels right of it.
     *
     * The frame stamp is what keeps a tracked value honest. The gear draws only
     * on the local player's own row, only in a voice build — so it can stop
     * drawing at any time, when that row scrolls out of view or voice is turned
     * off. An undated static would then hold a position from an older, wider
     * layout, and since the table is NoClip the bot gear would draw on top of
     * the player name. Accepting it only while it is at most one frame old
     * makes it self-heal: rows above the local player's use last frame's value,
     * rows below use this frame's, and two quiet frames drop us to the
     * fallback. */
    static float s_voiceGearX = 0.0f;
    static int   s_voiceGearFrame = -1000;
    const float  appliedStartCenterX = s_startComboCenterX;
    const float  appliedReadyLeft    = s_startComboReadyLeft;
    float startColNameMaxRight = 0.0f;  /* widest name(+tag) right edge */
    float startColPingRightX   = 0.0f;  /* ping/gear column right edge */
    float startColReadyLeftX   = 0.0f;  /* Ready cell left edge */

    for (int teamId = 1; teamId < 16; teamId++) {
        /* Teams 1 and 2 are always rendered (the lobby's two default
         * sides) — the host always has somewhere to drop the first
         * bot. Teams 3..15 render when they have members OR when the
         * host has explicitly added them via "+ Add Team"
         * (lobbyTeamInUse=1). Without the in_use check, freshly added
         * teams would vanish on the same frame because they have no
         * members yet. */
        bool persistTeam = (teamId == 1 || teamId == 2)
                        || clientSimGetLobbyTeamInUse(cs, (BYTE)(teamId));
        if (!persistTeam && memberCount[teamId] == 0) continue;

        /* Team color from theme; falls back to gray for un-themed teams. */
        ImU32 tc;
        uint8_t colorIdx = clientSimGetLobbyTeamColor(cs, (BYTE)(teamId));
        if (clientSimGetLobbyTeamInUse(cs, (BYTE)(teamId)) && colorIdx < 8) {
            tc = g_theme->teamColors[colorIdx];
        } else {
            /* Default per-team color: cycle through palette by teamId. */
            tc = g_theme->teamColors[(teamId - 1) & 7];
        }

        /* Team child window — uses the default ChildBg from the theme.
         * Team color is reserved for the header strip only (see below)
         * so the body stays neutral and the team identity reads as a
         * banner rather than a flood fill. */
        char teamFrame[32];
        SDL_snprintf(teamFrame, sizeof(teamFrame), "##team%d", teamId);
        ImGui::BeginChild(teamFrame,
                          ImVec2(0, 0),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders |
                              ImGuiChildFlags_NavFlattened);

        /* Color header strip — fills the header row's full width with
         * the team color at low alpha, drawn under the header widgets
         * via the window draw list. Renders before the swatch so the
         * widgets sit on top of it. */
        ImVec2 stripStart = ImGui::GetCursorScreenPos();
        float  stripH     = ImGui::GetTextLineHeightWithSpacing() + 4.0f * s;
        float  contentW   = ImGui::GetContentRegionAvail().x;
        ImU32  stripCol   = (tc & 0x00FFFFFF) | (0x28 << 24);
        ImGui::GetWindowDrawList()->AddRectFilled(
            stripStart,
            ImVec2(stripStart.x + contentW, stripStart.y + stripH),
            stripCol, 2.0f);

        /* Team identity badge moved off the team header; each player
         * row now renders a green (self/ally) or red (enemy) tank
         * icon to the left of its name, so team affiliation reads
         * per-row instead of just in the header.  Pad a few pixels
         * before the "Team N" label so it doesn't hug the left edge. */
        ImGui::Dummy(ImVec2(6.0f * s, 0));
        ImGui::SameLine(0.0f, 0.0f);

        /* Team name — always "Team N", non-editable. Renaming was
         * dropped per UX feedback; the number is enough identity
         * alongside the colored tank badge.  AlignTextToFramePadding
         * vertically centers both text spans with the frame-padded
         * widgets on the same row (matches the "Bot Naming:" label). */
        char defaultName[16];
        {
            MessageArgs args = {};
            args.number = teamId;
            SDL_snprintf(defaultName, sizeof(defaultName), "%s", langGetTextFmt(STR_DLGLOBBY_TEAM_HEADER, &args));
        }
        /* Header geometry, hoisted out of the host block below because the
         * shed after the team name has to know how much room the right-hand
         * controls will take before it decides what the left side may draw. */
        const float comboW      = 200.0f * s;
        /* Gap between the Bot Naming combo and the Add Bot button — just a
         * normal widget-pair spacing so the dropdown sits directly next to
         * Add Bot rather than being pushed off to the middle of the row. */
        const float namingShift = 6.0f * s;
        const float botBtnW     = 95.0f * s;
        const float xBtnW       = 22.0f * s;
        const float gap         = 6.0f * s;
        const float labelW      = ImGui::CalcTextSize(langGetText(STR_DLGLOBBY_BOT_NAMING)).x;
        /* The "Start:" side selector, left of Bot Naming in the host group. */
        const float sideComboW  = 90.0f * s;
        const float sideLabelW  = ImGui::CalcTextSize(langGetText(STR_DLGLOBBY_TEAM_SIDE)).x;
        /* Bots are only addable when the server's AI policy allows it
         * (lobbyAiType != aiNone) AND the server has at least one brain on
         * disk to assign. Both fields are mirrored from the server
         * dynamically, so the button and the Bot Naming controls disappear /
         * reappear without a reconnect when -ai policy or brains/ changes. */
        bool botsAllowed = (clientSimGetLobbyAiType(cs) != 0) &&
                           (clientSimGetLobbyBrainList(cs)->count > 0);
        int  humanCount  = memberCount[teamId] - botCount[teamId];
        bool showXBtn    = (teamId >= 3) && (humanCount == 0);
        bool showNaming  = effectiveHost && botsAllowed && botCount[teamId] > 0;
        /* Side selector: host only, and only when the map has starts. */
        bool showSide    = effectiveHost && lobbyMapPreview()->startCount > 0;
        bool showJoin    = !spectator && myPlayerNum >= 0 && myPlayerNum < MAX_TANKS &&
                           clientSimGetLobbySlot(cs, (BYTE)(myPlayerNum))->teamNumber != teamId;
        bool showTeamCount = true;

        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(tc));
        ImGui::Text("%s", defaultName);
        ImGui::PopStyleColor();

        /* Built before it is drawn — the shed below measures this line to
         * decide whether it still clears the buttons. */
        char membersLine[96];
        {
            char membersStr[64];
            if (memberCount[teamId] == 1) {
                SDL_snprintf(membersStr, sizeof(membersStr), "%s",
                             langGetText(STR_DLGLOBBY_TEAM_MEMBERS_1));
            } else {
                MessageArgs args = {};
                args.number = memberCount[teamId];
                SDL_snprintf(membersStr, sizeof(membersStr), "%s",
                             langGetTextFmt(STR_DLGLOBBY_TEAM_MEMBERS_N, &args));
            }
            const char *botsPart = "";
            if (botCount[teamId] == 1)      botsPart = langGetText(STR_DLGLOBBY_TEAM_1BOT);
            else if (botCount[teamId] > 1)  botsPart = langGetText(STR_DLGLOBBY_TEAM_NBOTS);
            const char *sep = botCount[teamId] > 0 ? " · " : "";
            SDL_snprintf(membersLine, sizeof(membersLine), "%s%s%s",
                         membersStr, sep, botsPart);
        }

        /* Header shed. The host's bot controls are pinned to the panel's
         * right edge, so whatever the left side draws past their left edge
         * ends up underneath them. Measure both sides against the panel's own
         * content width and give up the optional pieces in order — the Start
         * side selector, then the Bot Naming pool, then the member count,
         * then Join Team — so a narrow players column keeps the header
         * readable instead of piling it on itself. */
        {
            const ImGuiStyle &hs = ImGui::GetStyle();
            float effBotBtnW = botsAllowed ? botBtnW : 0.0f;
            float effBotGap  = botsAllowed ? gap     : 0.0f;
            float groupBaseW = effectiveHost ? (effBotBtnW + effBotGap + xBtnW) : 0.0f;
            float namingW    = labelW + gap + comboW + namingShift;
            float sideW      = sideLabelW + gap + sideComboW + namingShift;
            float countW     = hs.ItemSpacing.x + ImGui::CalcTextSize(membersLine).x;
            float joinW      = showJoin
                               ? hs.ItemSpacing.x + hs.FramePadding.x * 2.0f
                                 + ImGui::CalcTextSize(langGetText(STR_DLGLOBBY_JOIN_TEAM)).x
                               : 0.0f;
            float needW = 6.0f * s + ImGui::CalcTextSize(defaultName).x
                        + groupBaseW + gap;
            if (showSide && contentW < needW + sideW + (showNaming ? namingW : 0.0f)
                                       + countW + joinW) {
                showSide = false;
            }
            if (showSide) needW += sideW;
            if (showNaming && contentW < needW + namingW + countW + joinW) {
                showNaming = false;
            }
            if (showNaming) needW += namingW;
            showTeamCount = (contentW >= needW + countW + joinW);
            if (showTeamCount) needW += countW;
            if (showJoin && contentW < needW + joinW) showJoin = false;
        }

        if (showTeamCount) {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", membersLine);
        }

        /* "Join Team" — moves the local player to this team. Available
         * to every client regardless of permissions (you can always
         * move yourself) and only rendered when you're not already on
         * this team. */
        if (showJoin) {
            ImGui::SameLine();
            char joinId[64];
            SDL_snprintf(joinId, sizeof(joinId), "%s##j%d", langGetText(STR_DLGLOBBY_JOIN_TEAM), teamId);
            if (ImGui::SmallButton(joinId)) {
                lobbySendTeamSet(cs,
                                 (uint8_t)myPlayerNum, (uint8_t)teamId);
            }
            if (ImGui::IsItemHovered()) {
                MessageArgs args = {};
                args.number = teamId;
                ImGui::SetTooltip("%s", langGetTextFmt(STR_DLGLOBBY_TOOLTIP_JOIN, &args));
            }
        }

        /* Per-team "+ Bot" button (always visible to the host) plus an
         * optional "Start:" side selector (only when the map has starts),
         * an optional "Bot Naming:" pool dropdown (only when the team has
         * at least one bot — picking a pool before any bot exists has
         * nothing to apply to) and an optional X (clear) button on
         * the right edge.
         *
         * The X is shown only for teams 3..15 (teams 1 and 2 are
         * persistent and never removable) and only when the team has
         * no humans on it — clearing a team with humans would orphan
         * them. We still reserve its width even when hidden so the
         * "+ Bot" button stays at the same X coordinate across teams
         * that do/don't render an X. */
        if (effectiveHost) {
            /* Always reserve the X width so "+ Bot" sits at the same
             * X position across teams with/without an X. */
            /* When showing the Bot Naming controls, leave a wider
             * gap (namingShift) between the dropdown and the Add Bot
             * button so the label + dropdown sit further left and
             * the dropdown has room to be wider. */
            float effBotBtnW = botsAllowed ? botBtnW : 0.0f;
            float effBotGap  = botsAllowed ? gap    : 0.0f;
            float groupW = effBotBtnW + effBotGap + xBtnW
                         + (showNaming
                            ? labelW + gap + comboW + namingShift
                            : 0)
                         + (showSide
                            ? sideLabelW + gap + sideComboW + namingShift
                            : 0);
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - groupW);
            if (showSide) {
                /* "Start:" side selector — Any / North / East / South /
                 * West. A pick goes out as a team-meta command through
                 * lobbySendTeamSide, which reads the team's colour, pool
                 * and name back first so the side is the only field that
                 * changes. Hovering the closed combo, or an entry in it,
                 * names what that side costs the team. */
                BYTE curSide = clientSimGetLobbyTeamStartSide(cs, (BYTE)(teamId));
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", langGetText(STR_DLGLOBBY_TEAM_SIDE));
                ImGui::SameLine(0.0f, gap);
                char sideId[16];
                SDL_snprintf(sideId, sizeof(sideId), "##side%d", teamId);
                ImGui::SetNextItemWidth(sideComboW);
                if (ImGui::BeginCombo(sideId, langGetText(lobbySideNameId(curSide)))) {
                    for (int sd = START_SIDE_ANY; sd < START_SIDE_COUNT; sd++) {
                        bool sel = (sd == (int)curSide);
                        if (ImGui::Selectable(langGetText(lobbySideNameId((BYTE)sd)), sel)) {
                            lobbySendTeamSide(cs, (uint8_t)teamId, (uint8_t)sd);
                        }
                        if (ImGui::IsItemHovered()) {
                            lobbyTeamSideTooltip(cs, teamId, (BYTE)sd, memberCount[teamId]);
                        }
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                } else if (ImGui::IsItemHovered()) {
                    lobbyTeamSideTooltip(cs, teamId, curSide, memberCount[teamId]);
                }
                ImGui::SameLine(0.0f, namingShift);
            }
            int curPool = clientSimGetLobbyTeamPool(cs, (BYTE)(teamId));
            if (curPool < 0 || curPool >= lobbyBotPoolCount()) curPool = 0;
            if (showNaming) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", langGetText(STR_DLGLOBBY_BOT_NAMING));
                ImGui::SameLine(0.0f, gap);
                char poolId[16];
                SDL_snprintf(poolId, sizeof(poolId), "##pool%d", teamId);
                ImGui::SetNextItemWidth(comboW);
                if (ImGui::BeginCombo(poolId, lobbyBotPoolLabel(curPool))) {
                    for (int p = 0; p < lobbyBotPoolCount(); p++) {
                        bool sel = (p == curPool);
                        /* Disable pools already claimed by some other
                         * in_use team — server enforces uniqueness; the
                         * UI mirrors it so users see what's available. */
                        bool takenElsewhere = false;
                        for (int ot = 1; ot < MAX_TANKS; ot++) {
                            if (ot == teamId) continue;
                            if (clientSimGetLobbyTeamInUse(cs, (BYTE)ot) &&
                                clientSimGetLobbyTeamPool(cs, (BYTE)ot) == p) {
                                takenElsewhere = true;
                                break;
                            }
                        }
                        ImGuiSelectableFlags flags = takenElsewhere
                            ? ImGuiSelectableFlags_Disabled : 0;
                        if (ImGui::Selectable(lobbyBotPoolLabel(p), sel, flags)) {
                            const char *nameForMeta = clientSimGetLobbyTeamInUse(cs, (BYTE)(teamId))
                                ? clientSimGetLobbyTeamName(cs, (BYTE)(teamId)) : defaultName;
                            lobbySendTeamPool(cs, (uint8_t)teamId,
                                              (uint8_t)p, nameForMeta);
                        }
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine(0.0f, namingShift);
            }
            /* Empty-label button used as a click target; the icon
             * and the "Add Bot" text are drawn ourselves on top via
             * WindowDrawList. This lets us shift the text right by
             * a precise pixel offset so the centered label clears
             * the overlaid bot-cpu glyph cleanly. */
            bool addBtnClicked = false;
            ImVec2 addBtnPos = ImGui::GetCursorScreenPos();
            bool addBtnPending = lobbyAddBotPending(cs);
            if (botsAllowed) {
                char addId[24];
                SDL_snprintf(addId, sizeof(addId), "##ab%d", teamId);
                if (addBtnPending) ImGui::BeginDisabled();
                addBtnClicked = ImGui::Button(addId, ImVec2(botBtnW, 0));
                if (addBtnPending) ImGui::EndDisabled();
            {
                float btnH = ImGui::GetFrameHeight();
                ImDrawList *dl = ImGui::GetWindowDrawList();
                const char *addLbl = langGetText(STR_DLGLOBBY_ADDBOT_LBL);
                ImVec2 textSz = ImGui::CalcTextSize(addLbl);
                ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
                /* Centered horizontally, then shifted right by 10px
                 * so it never collides with the icon on the left. */
                ImVec2 textPos(addBtnPos.x + (botBtnW - textSz.x) * 0.5f
                                            + 10.0f * s,
                               addBtnPos.y + (btnH - textSz.y) * 0.5f);
                dl->AddText(textPos, textCol, addLbl);
            }
            /* Green for "your team" buttons, red for the others —
             * matches the bot-cpu glyph rendered per row. */
            uint8_t myTeamHdr = (!spectator && myPlayerNum >= 0 && myPlayerNum < MAX_TANKS)
                                ? clientSimGetLobbySlot(cs, (BYTE)(myPlayerNum))->teamNumber : 0;
            SDL_Texture *addBtnIcon = (myTeamHdr != 0 && teamId == myTeamHdr)
                                      ? lobbyIcons()->botCpuGreen
                                      : lobbyIcons()->botCpuRed;
            if (addBtnIcon) {
                float btnH    = ImGui::GetFrameHeight();
                float imgSz   = ImGui::GetFontSize();
                /* Tucked a few pixels in from the left edge so the
                 * icon doesn't hug the button border. The button is
                 * wide enough that the centered "Add Bot" label
                 * clears the icon naturally. */
                ImVec2 iconPos(addBtnPos.x + 8.0f * s,
                               addBtnPos.y + (btnH - imgSz) * 0.5f);
                ImGui::GetWindowDrawList()->AddImage(
                    (ImTextureID)addBtnIcon,
                    iconPos,
                    ImVec2(iconPos.x + imgSz, iconPos.y + imgSz));
            }
            }  /* end botsAllowed AddBot block */
            if (addBtnClicked) {
                /* When the first bot is added to a team, randomize
                 * the pool so the name comes from a varied source
                 * instead of always pool 0. Teams 1/2 are pre-marked
                 * in_use at server init, so gating on in_use never
                 * fired — gate on "no bots yet" instead. Subsequent
                 * bots in the same team reuse the chosen pool; the
                 * host can still override via the dropdown. */
                int effectivePool = curPool;
                if (botCount[teamId] == 0 && lobbyBotPoolCount() > 0) {
                    effectivePool = (int)bolo_rand_below((uint32_t)lobbyBotPoolCount());
                    const char *nameForMeta = clientSimGetLobbyTeamInUse(cs, (BYTE)(teamId))
                        ? clientSimGetLobbyTeamName(cs, (BYTE)(teamId)) : defaultName;
                    lobbySendTeamPool(cs, (uint8_t)teamId,
                                      (uint8_t)effectivePool, nameForMeta);
                }
                lobbySendAddBotDebounced(cs, effectivePool, (uint8_t)teamId);
            }
            if (ImGui::IsItemHovered()) {
                MessageArgs args = {};
                args.number = teamId;
                ImGui::SetTooltip("%s", langGetTextFmt(STR_DLGLOBBY_TOOLTIP_ADDBOT, &args));
            }
            /* Pin the X to the right edge so it lines up across teams.
             * Use ImGui::CloseButton (imgui_internal.h) — the same
             * widget ImGui uses for window-close buttons. Renders a
             * proper crossed line "×" with hover + active styling,
             * sized to the current font. */
            if (botsAllowed) {
                ImGui::SameLine(0.0f, gap);
            }
            ImGui::AlignTextToFramePadding();
            if (showXBtn) {
                char rmStrId[24];
                SDL_snprintf(rmStrId, sizeof(rmStrId), "##rmt%d", teamId);
                ImGuiID rmId = ImGui::GetID(rmStrId);
                ImVec2 closePos = ImGui::GetCursorScreenPos();
                /* Vertically center the close button within the row
                 * height. CloseButton draws a FontSize × FontSize box;
                 * frame height is taller. */
                float frameH = ImGui::GetFrameHeight();
                float fs = ImGui::GetFontSize();
                closePos.y += (frameH - fs) * 0.5f;
                if (ImGui::CloseButton(rmId, closePos)) {
                    /* Clear the team's metadata and any bot members. */
                    for (int i = 0; i < MAX_TANKS; i++) {
                        if (clientSimGetLobbySlot(cs, (BYTE)(i))->connected
                            && clientSimGetLobbySlot(cs, (BYTE)(i))->isBot
                            && clientSimGetLobbySlot(cs, (BYTE)(i))->teamNumber == teamId) {
                            lobbySendRemoveBot(cs, (uint8_t)i);
                        }
                    }
                    lobbySendTeamClear(cs, (uint8_t)teamId);
                }
                if (ImGui::IsItemHovered()) {
                    MessageArgs args = {};
                    args.number = teamId;
                    ImGui::SetTooltip("%s", langGetTextFmt(STR_DLGLOBBY_TOOLTIP_RMTEAM, &args));
                }
                /* CloseButton uses an explicit screen pos and only calls
                 * ItemAdd (no ItemSize), so the layout cursor is NOT
                 * advanced. Without this Dummy the following BeginTable
                 * starts at the cursor's pre-CloseButton X (right side
                 * of the header) and renders the entire bot table off
                 * to the right of the team panel with squished columns.
                 * The Dummy reserves the X-button's slot in the line so
                 * the next item line-wraps normally. */
                ImGui::Dummy(ImVec2(xBtnW, 0));
            } else {
                /* Hold the slot so + Bot's X position is stable. */
                ImGui::Dummy(ImVec2(xBtnW, 0));
            }
        }

        /* Member rows — proper ImGui Table.
         *
         * BordersInnerH gives us the inter-row dividers natively
         * (one between each row, none above the first / below the
         * last). RowBg + TableSetBgColor lets us paint the "you"
         * row a darker tint without manually computing rectangles.
         * SizingStretchProp makes the name column absorb leftover
         * space while the icon columns stay fixed.
         *
         * Column layout: [tank | name+icons | ping | ready | X]. */
        SDL_Renderer *r = sdl3DrawGetRenderer();
        uint8_t myTeam = (!spectator && myPlayerNum >= 0 && myPlayerNum < MAX_TANKS)
                         ? clientSimGetLobbySlot(cs, (BYTE)(myPlayerNum))->teamNumber : 0;
        char tableId[32];
        SDL_snprintf(tableId, sizeof(tableId), "##members%d", teamId);

        /* Column shed. The fixed columns keep their width under
         * SizingStretchProp, so once the players column is narrower than they
         * add up to they start landing on each other — give them up one at a
         * time instead, in order of what a lobby can most do without: the
         * identity icons, the start picker, ping/gear, the tank badge, the
         * name tags, then the kick X. The floor is the player name and the
         * ready pill, which stay at every width.
         *
         * Each threshold is the running total of what is still kept at that
         * point plus the name column's own floor, built from the same widths
         * TableSetupColumn uses below so a column resize carries them along.
         * Measured against the panel's real content width rather than the
         * divider's offset, so a small lobby window sheds the same way a
         * divider dragged left does, and scaled by s throughout so it lands
         * the same at any DPI. */
        const float kColTankW    = 60.0f * s;
        /* The gear's own width. BOTH gears live in the icons column — a bot's
         * config gear beside its bot-cpu badge, and (in a voice build) the
         * local player's voice gear after the mic — so this is needed in
         * either build. Sized for the form that will actually draw, which is
         * known here: the SmallButton in controller mode, the font-sized icon
         * otherwise. */
        const float kColGearW    = uiShouldUseControllerMode()
                                 ? ImGui::CalcTextSize(">").x
                                   + ImGui::GetStyle().FramePadding.x * 2.0f
                                 : ImGui::GetFontSize();
#if defined(WINBOLO_VOICE)
        /* The microphone cell follows the badge run, so the icons column
         * carries one more LOBBY_WBN_ICON_SIZE icon plus the spacing before
         * it, and the local player's row carries the voice gear after that,
         * with a spacing of its own. That raises needIconsCol, so the column
         * sheds at a slightly wider window than it does without voice — the
         * wider run needs the room, and the whole column still goes at once.
         * The gear draws on one row, but every team's table takes the width:
         * the team panels are stacked, so a column one width in your team
         * and another in the rest would not line up.
         *
         * Sized for the form that will draw, which is known here: the
         * SmallButton in controller mode, the font-sized icon otherwise. */
        const float kColIconsW   = 96.0f * s + (float)LOBBY_WBN_ICON_SIZE * s
                                 + ImGui::GetStyle().ItemSpacing.x
                                 + kColGearW
                                 + ImGui::GetStyle().ItemSpacing.x;
#else
        /* No voice build, but a BOT row still puts its config gear in this
         * column beside the bot-cpu badge, so the gear's width is kept here
         * either way. */
        const float kColIconsW   = 96.0f * s
                                 + ImGui::GetStyle().ItemSpacing.x
                                 + kColGearW;
#endif
        const float kColPingW    = 50.0f * s;
        const float kColReadyW   = 80.0f * s;
        const float kColXW       = 44.0f * s;
        /* The spacer column exists to hold the start dropdown, so what it
         * needs is that dropdown plus the clearance kept around it. */
        const float kColStartW   = 96.0f * s + 8.0f * s;
        /* About eight characters and an ellipsis — the least a name can say
         * and still tell two players apart. */
        const float kColNameMinW = 64.0f * s;
        /* What lobbyTruncateName already reserves for a HOST/ADMIN/BOT pill;
         * the pill shares the name's cell, so at the floor it is the name's
         * own room it would be taking. */
        const float kNameTagW    = 56.0f * s;
        const float cellPadW     = ImGui::GetStyle().CellPadding.x * 2.0f;
        const float needXCol     = kColNameMinW + kColReadyW + kColXW + cellPadW * 3.0f;
        const float needNameTags = needXCol  + kNameTagW;
        const float needTankCol  = needXCol  + kColTankW  + cellPadW;
        const float needPingCol  = needTankCol + kColPingW  + cellPadW;
        const float needStartCol = needPingCol + kColStartW + cellPadW;
        const float needIconsCol = needStartCol + kColIconsW + cellPadW;
        const bool showXCol      = contentW >= needXCol;
        const bool showNameTags  = contentW >= needNameTags;
        const bool showTankCol   = contentW >= needTankCol;
        const bool showPingCol   = contentW >= needPingCol;
        const bool showStartCol  = contentW >= needStartCol;
        const bool showIconsCol  = contentW >= needIconsCol;
        /* A bot's name / mode / difficulty pills go at the same width the
         * start column goes: the row is shedding detail by then, and those
         * three are detail. The BOT pill itself stays with the name tags. */
        const bool showBotDetailTags = showNameTags && showStartCol;
#if defined(WINBOLO_VOICE)
        /* Who is producing voice right now, read once for the whole list
         * rather than per row. */
        const PlayerBitMap talkingMap = voiceGetTalkingMap();
#endif
        /* Disabled is the master hide flag — the column takes no width and
         * ImGui skips every widget submitted into it, so the remaining
         * columns get the room back. The row blocks below still guard their
         * own content: the direct draw-list work (tags, pills, gear, promote
         * glyph) is not covered by that skip. */
        const ImGuiTableColumnFlags kShedCol = ImGuiTableColumnFlags_Disabled;

        ImGuiTableFlags tableFlags = ImGuiTableFlags_BordersInnerH
                                   | ImGuiTableFlags_RowBg
                                   | ImGuiTableFlags_SizingStretchProp
                                   | ImGuiTableFlags_PadOuterX
                                   /* NoClip so the expanded bot's
                                    * AiConfig form (rendered in col 0)
                                    * is allowed to extend visually
                                    * across the other columns. */
                                   | ImGuiTableFlags_NoClip;
        if (ImGui::BeginTable(tableId, 7, tableFlags)) {
            /* Wide column 0 so the tank icon sits well inside the
             * team panel (not crammed against the left edge). The
             * icon is positioned with a leading SetCursorPosX nudge
             * inside the cell.
             *
             * Column 1 hosts the per-row identity icons (country
             * flag + platform/WBN/Steam badges for humans, the
             * bot-cpu glyph for bots) — extracted from the name
             * column so the name text always starts at a fixed X.
             *
             * Two stretch columns (name + spacer) with weights 3:1
             * park the ping/gear column at roughly 3/4 across the
             * row instead of flush against the ready/X cluster. */
            ImGui::TableSetupColumn("##tank",   ImGuiTableColumnFlags_WidthFixed |
                                                (showTankCol ? 0 : kShedCol), kColTankW);
            /* Wide enough for the worst case: country flag + platform +
             * WBN-verified shield + Steam badge (16 + 3×14 px plus
             * inter-icon spacing), so the verified badge can't spill into
             * the name column. */
            ImGui::TableSetupColumn("##icons",  ImGuiTableColumnFlags_WidthFixed |
                                                (showIconsCol ? 0 : kShedCol), kColIconsW);
            ImGui::TableSetupColumn("##name",   ImGuiTableColumnFlags_WidthStretch, 3.0f);
            ImGui::TableSetupColumn("##ping",   ImGuiTableColumnFlags_WidthFixed |
                                                (showPingCol ? 0 : kShedCol), kColPingW);
            ImGui::TableSetupColumn("##spacer", ImGuiTableColumnFlags_WidthStretch |
                                                (showStartCol ? 0 : kShedCol), 1.0f);
            ImGui::TableSetupColumn("##ready",  ImGuiTableColumnFlags_WidthFixed, kColReadyW);
            ImGui::TableSetupColumn("##x",      ImGuiTableColumnFlags_WidthFixed |
                                                (showXCol ? 0 : kShedCol), kColXW);

            /* Drive striping ourselves (per-player, not per-table-row)
             * so the bot's expanded AiConfig sub-row inherits the same
             * background as its parent bot row — keeps the odd/even
             * stripe stable when the gear is toggled. */
            const ImU32 stripeA = ImGui::GetColorU32(ImGuiCol_TableRowBg);
            const ImU32 stripeB = ImGui::GetColorU32(ImGuiCol_TableRowBgAlt);
            int playerIdx = 0;

            for (int i = 0; i < MAX_TANKS; i++) {
                if (!clientSimGetLobbySlot(cs, (BYTE)(i))->connected) continue;
                if (clientSimGetLobbySlot(cs, (BYTE)(i))->teamNumber != teamId) continue;

                bool isMe   = (!spectator && i == myPlayerNum);
                bool isBot  = clientSimGetLobbySlot(cs, (BYTE)(i))->isBot;
                bool isSelf = isMe;
                bool isAlly = (myTeam != 0 && clientSimGetLobbySlot(cs, (BYTE)(i))->teamNumber == myTeam);

                /* Row height = the tallest single widget we render
                 * in the row plus a touch of breathing room. Widgets
                 * are positioned per-cell via absolute Y so each one
                 * is centered on the row's vertical midline regardless
                 * of its own height. */
                const float tankSz  = 18.0f * s;
                const float closeSz = ImGui::GetFontSize();
                const float rowH    = ImMax(ImGui::GetFrameHeight(),
                                            ImMax(tankSz, 22.0f * s));

                ImGui::TableNextRow(0, rowH);
                const ImU32 rowStripe = (playerIdx & 1) ? stripeB : stripeA;
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, rowStripe);
                if (isMe) {
                    /* Translucent dark tint that highlights the local
                     * player's row, painted on top of the stripe. */
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
                                           IM_COL32(0, 0, 0, 64));
                }
                playerIdx++;

                /* Per-cell vertical centering: capture the row's top
                 * Y on entering each cell, then set cursor.y to
                 * rowTopY + (rowH - widgetH)/2 immediately before
                 * every widget. SameLine() resets cursor.y to the
                 * line's min Y; we override it again per widget so
                 * widgets of different heights (flags 11px, icons
                 * 14px, text ~13px, tank 18px) each end up centered
                 * on the row midline. */
                float rowTopY;
                auto cyAbs = [&](float h) {
                    float y = rowTopY + (rowH - h) * 0.5f;
                    if (y < rowTopY) y = rowTopY;
                    ImGui::SetCursorPosY(y);
                };
                /* Text-specific centering: ImGui's text line box has a
                 * couple of pixels of "ascent whitespace" above the
                 * cap line and an almost-empty descender below, so a
                 * pure line-height-based centering makes the visible
                 * glyph mass sit too low (~8px top / ~5px bottom in a
                 * 22px row). Bias the cursor up by ~12% of font size
                 * (about 1.5–2 px) so the visible glyphs end up
                 * centered. */
                auto cyTextAbs = [&]() {
                    float h = ImGui::GetTextLineHeight();
                    float bias = ImGui::GetFontSize() * 0.12f;
                    float y = rowTopY + (rowH - h) * 0.5f - bias;
                    if (y < rowTopY) y = rowTopY;
                    ImGui::SetCursorPosY(y);
                };

                /* ── Column 0: drag handle + tank icon ─────────────── */
                ImGui::TableSetColumnIndex(0);
                rowTopY = ImGui::GetCursorPosY();

                /* Drag handle — drives the "drag a player onto a team"
                 * flow. Authority: only effectiveHost (host / admin /
                 * openHost-empowered editor) can drag. Plain players
                 * change their own team via the per-team "Join" button
                 * to keep the row UI uncluttered.
                 * Server re-validates on PACKET_LOBBY_TEAM_SET.
                 * Rendered as a 4-arrow "move" cross centered in the
                 * row, before the tank icon. */
                bool canDragThis = effectiveHost && showTankCol;
                if (canDragThis) {
                    const float handleS = 14.0f * s;
                    cyAbs(handleS);
                    ImVec2 hPos = ImGui::GetCursorScreenPos();
                    char hId[24];
                    SDL_snprintf(hId, sizeof(hId), "##drag%d", i);
                    ImGui::InvisibleButton(hId, ImVec2(handleS, handleS));
                    ImU32 lineCol = ImGui::IsItemHovered()
                        ? IM_COL32(230, 230, 230, 230)
                        : IM_COL32(140, 140, 140, 200);
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    float cx = hPos.x + handleS * 0.5f;
                    float cy = hPos.y + handleS * 0.5f;
                    float a  = handleS * 0.45f;
                    float h  = handleS * 0.20f;
                    float th = 1.2f;
                    /* Crossbars. */
                    dl->AddLine(ImVec2(cx - a, cy), ImVec2(cx + a, cy),
                                lineCol, th);
                    dl->AddLine(ImVec2(cx, cy - a), ImVec2(cx, cy + a),
                                lineCol, th);
                    /* Four arrowheads, one per crossbar tip. */
                    dl->AddLine(ImVec2(cx - a, cy),
                                ImVec2(cx - a + h, cy - h), lineCol, th);
                    dl->AddLine(ImVec2(cx - a, cy),
                                ImVec2(cx - a + h, cy + h), lineCol, th);
                    dl->AddLine(ImVec2(cx + a, cy),
                                ImVec2(cx + a - h, cy - h), lineCol, th);
                    dl->AddLine(ImVec2(cx + a, cy),
                                ImVec2(cx + a - h, cy + h), lineCol, th);
                    dl->AddLine(ImVec2(cx, cy - a),
                                ImVec2(cx - h, cy - a + h), lineCol, th);
                    dl->AddLine(ImVec2(cx, cy - a),
                                ImVec2(cx + h, cy - a + h), lineCol, th);
                    dl->AddLine(ImVec2(cx, cy + a),
                                ImVec2(cx - h, cy + a - h), lineCol, th);
                    dl->AddLine(ImVec2(cx, cy + a),
                                ImVec2(cx + h, cy + a - h), lineCol, th);
                    if (ImGui::BeginDragDropSource(
                            ImGuiDragDropFlags_SourceAllowNullID)) {
                        uint8_t slot = (uint8_t)i;
                        ImGui::SetDragDropPayload("WB_LOBBY_PLAYER",
                                                  &slot, sizeof(slot));
                        ImGui::TextUnformatted(
                            clientSimGetLobbySlot(cs, (BYTE)(i))->playerName[0]
                            ? clientSimGetLobbySlot(cs, (BYTE)(i))->playerName
                            : "(slot)");
                        ImGui::EndDragDropSource();
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                        ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_TOOLTIP_DRAG));
                    }
                    ImGui::SameLine(0.0f, 4.0f * s);
                    /* Reset Y for the tank-icon block below. */
                    ImGui::SetCursorPosY(rowTopY);
                }
                {
                    SDL_Texture *tankTex = nullptr;
                    if (r) {
                        if (isSelf) {
                            tankTex = lobbyGetTankSelf04Texture(r);  /* black self */
                        } else if (isAlly) {
                            tankTex = lobbyGetTankGood04Texture(r);  /* green ally */
                            if (!tankTex) tankTex = lobbyGetTankSelf04Texture(r);
                        } else {
                            tankTex = lobbyGetTankEvil04Texture(r);  /* red enemy */
                        }
                    }
                    if (tankTex && showTankCol) {
                        /* Indent the tank inside its cell so it
                         * doesn't sit flush with the team panel's
                         * left edge. */
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 28.0f * s);
                        /* Apply the same optical bias used for text:
                         * the tank PNG has a sliver of empty pixels
                         * along its bottom (cannon points up), so a
                         * pure geometric center makes it sit visibly
                         * low in the row. Nudge up ~12% of font size
                         * to match the centered look of the labels. */
                        cyAbs(tankSz);
                        float tankBias = ImGui::GetFontSize() * 0.12f;
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - tankBias);
                        ImGui::Image((ImTextureID)tankTex, ImVec2(tankSz, tankSz));
                    }
                }

                /* A bot's mode and difficulty, resolved once for the whole row.
                 *
                 * Computed HERE, above the icons column, because the difficulty
                 * is drawn there now as chips beside the tank icon. It used to
                 * be a tag beside the name, which is why this used to sit
                 * further down.
                 *
                 * All of it comes from lobby state the server already syncs to
                 * every client: the brain catalogue and index, and
                 * LobbyBotConfig's mode and difficulty indices, resolved against
                 * the brain's own modes.txt. So a joiner sees what the host
                 * sees. */
                const char *botModeTag = "";
                const char *botDiffTag = "";
                int botLevel = BOT_DIFFICULTY_HARD;
                int botMode  = 0;
                /* How many of the three chips are lit, 1..3 — or 0 when the
                 * brain declares no levels for this mode. 0 means "no
                 * difficulty": the row shows the single badge it always had,
                 * with no extra chips, no hover and nothing to cycle. */
                int botDiffChips = 0;
                const BrainModes *botModes = NULL;
                if (isBot) {
                    botModes = lobbyBotModesFor(cs, i);
                    lobbyBotModeAndLevel(cs, i, &botMode, &botLevel);
                    const int botLevelCount =
                        (botModes != NULL) ? botModes->modes[botMode].levelCount
                                           : 3;
                    if (botLevelCount > 0) {
                        if (botModes == NULL ||
                            lobbyBotModeUsesLangLevels(botModes, botMode)) {
                            botDiffTag =
                                langGetText(lobbyBotDifficultyLabelId((uint8_t)botLevel));
                        } else {
                            botDiffTag =
                                botModes->modes[botMode].levels[botLevel].label;
                        }
                        /* DECLARED by the brain, per level — not inferred from
                         * the level's position in the list. The index fallback
                         * covers the synthesized default mode a brain with no
                         * manifest is given. */
                        if (botModes != NULL && botLevel >= 0 &&
                            botLevel < botLevelCount) {
                            botDiffChips =
                                botModes->modes[botMode].levels[botLevel].chips;
                        } else if (botLevel == BOT_DIFFICULTY_EASY) {
                            botDiffChips = 1;
                        } else if (botLevel == BOT_DIFFICULTY_MEDIUM) {
                            botDiffChips = 2;
                        } else {
                            botDiffChips = 3;
                        }
                        if (botDiffChips < 1) botDiffChips = 1;
                        if (botDiffChips > 3) botDiffChips = 3;
                    }
                    if (botModes != NULL &&
                        !lobbyBotModeIsDefault(botModes, botMode)) {
                        /* Name the mode too, so a row in the survival scenario
                         * says so on the row itself. */
                        botModeTag = botModes->modes[botMode].label;
                    }
                }

                /* The bot's config gear, drawn wherever the cursor already is.
                 *
                 * In a lambda because it has TWO homes. Its own is the icons
                 * column, beside the bot badge and lined up with the human
                 * rows' voice gear. But the icons column carries the LARGEST
                 * shed threshold of any column, so it is the first to go as the
                 * panel narrows — and when it goes the gear would go with it,
                 * leaving a band of panel widths where the rest of the row is
                 * still fully drawn and yet the host cannot open any bot's
                 * config, because nothing else writes expandedBotSlot. The ping
                 * column takes it back for that band, which is where it lived
                 * before this move.
                 *
                 * Shown only to a client with lobby-edit authority (host /
                 * openHost / admin); anyone else gets no control rather than a
                 * dead one. Controller mode draws the visible ">"/"v" toggle
                 * instead of the invisible icon button, so it is reachable by
                 * gamepad and shows a focus ring — A expands the AiConfig
                 * sub-row, whose widgets (name, Codebase, mode, difficulty)
                 * then navigate like any other dialog control.
                 *
                 * The CALLER positions X before calling this; the lambda only
                 * ever draws at the current cursor. The two sites differ: the
                 * icons-column one lines the gear up with the human rows' voice
                 * gear, and the ping-column one draws at that cell's own
                 * cursor, which is where the gear used to sit. */
                auto drawBotGear = [&]() {
                    if (lobbyIcons()->settings && !uiShouldUseControllerMode()) {
                        float iconSize = ImGui::GetFontSize();
                        cyAbs(iconSize);
                        /* settings.svg renders 5px above / 2px below with pure
                         * geometric centering — the gear sits slightly low in
                         * the row. Nudge up ~12% of font size (about 1.5px) so
                         * it matches the optical center the text and tank
                         * widgets use. */
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY()
                                             - ImGui::GetFontSize() * 0.12f);
                        ImVec2 iconStart = ImGui::GetCursorScreenPos();
                        char btnId[24];
                        SDL_snprintf(btnId, sizeof(btnId), "##cfg%d", i);
                        bool clicked = ImGui::InvisibleButton(btnId,
                                                              ImVec2(iconSize, iconSize));
                        /* Grey so it reads as a secondary action — the primary
                         * focus is the name and status badges — and full white
                         * on hover so it lights up. */
                        ImU32 gearTint = ImGui::IsItemHovered()
                            ? IM_COL32_WHITE
                            : IM_COL32(180, 180, 180, 200);
                        ImGui::GetWindowDrawList()->AddImage(
                            (ImTextureID)lobbyIcons()->settings,
                            iconStart,
                            ImVec2(iconStart.x + iconSize, iconStart.y + iconSize),
                            ImVec2(0, 0), ImVec2(1, 1), gearTint);
                        if (clicked) {
                            s_players.expandedBotSlot =
                                (s_players.expandedBotSlot == i) ? -1 : i;
                        }
                        if (ImGui::IsItemHovered()) {
                            lobbyGearTooltip(cs, i, s);
                        }
                    } else {
                        cyAbs(ImGui::GetFrameHeight());
                        char fallId[24];
                        SDL_snprintf(fallId, sizeof(fallId), "%s##cfg%d",
                                     s_players.expandedBotSlot == i ? "v" : ">", i);
                        if (ImGui::SmallButton(fallId)) {
                            s_players.expandedBotSlot =
                                (s_players.expandedBotSlot == i) ? -1 : i;
                        }
                        if (ImGui::IsItemHovered()) {
                            lobbyGearTooltip(cs, i, s);
                        }
                    }
                };

                /* ── Column 1: identity icons (flag/platform/bot) ── */
                ImGui::TableSetColumnIndex(1);
                rowTopY = ImGui::GetCursorPosY();
                /* This cell's right edge, read before anything is drawn into
                 * it. The bot gear falls back to it when no voice gear has been
                 * seen to align with. */
                const float iconsCellRightX = ImGui::GetCursorScreenPos().x +
                                              ImGui::GetContentRegionAvail().x;
                if (isBot && showIconsCol) {
                    /* Green for bots on the local player's team (incl.
                     * the local player's own bots), red for bots on
                     * any other team. Fall back to the neutral
                     * bot-cpu.svg if a tinted variant is missing.
                     * Drawn at the same size as the tank icon so the
                     * two badges line up visually across rows. */
                    SDL_Texture *botTex = isAlly ? lobbyIcons()->botCpuGreen
                                                 : lobbyIcons()->botCpuRed;
                    if (botTex) {
                        /* THREE CHIPS, immediately right of the tank icon. The
                         * first is the bot badge that has always been here; the
                         * other two are the difficulty, moved off the name tags
                         * (Andrew: "moving the chips from the tag mode to be now
                         * on the left side just to the right of the tank icon").
                         *
                         * A lit chip is the TEAM's colour — green for an ally,
                         * red for anyone else — so a run is all green or all red
                         * and never a mix. An unlit chip is the greyscale art.
                         * The difficulty therefore reads as how many of the
                         * three are coloured, and the colour itself still says
                         * which side the bot is on.
                         *
                         * A brain that declares no levels draws ONE chip. There
                         * is no difficulty to show, so the run is the plain
                         * badge it has always been. */
                        SDL_Texture *dimTex = lobbyIcons()->botCpuGrey;
                        const int   chipN   = (botDiffChips > 0) ? 3 : 1;
                        const float chipGap = 2.0f * s;
                        const float runW    = tankSz * (float)chipN
                                            + chipGap * (float)(chipN - 1);
                        cyAbs(tankSz);
                        /* Visible pixel mass in the bot-cpu art needs a small
                         * upward nudge to land centred on the row. */
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 2.0f);
                        ImVec2 runStart = ImGui::GetCursorScreenPos();
                        ImDrawList *chipDl = ImGui::GetWindowDrawList();
                        for (int c = 0; c < chipN; c++) {
                            SDL_Texture *t = (c < botDiffChips || chipN == 1)
                                           ? botTex : dimTex;
                            if (t == NULL) t = botTex;
                            ImVec2 a(runStart.x + (tankSz + chipGap) * (float)c,
                                     runStart.y);
                            chipDl->AddImage((ImTextureID)t, a,
                                             ImVec2(a.x + tankSz, a.y + tankSz),
                                             ImVec2(0, 0), ImVec2(1, 1),
                                             IM_COL32_WHITE);
                        }
                        /* ONE item over the whole run: it carries the hover that
                         * names the difficulty and the click that cycles it. A
                         * client that may not edit gets a Dummy instead — same
                         * size, still hoverable, but not a control it cannot
                         * use. */
                        bool chipClicked = false;
                        if (effectiveHost && botDiffChips > 0) {
                            char chipId[24];
                            SDL_snprintf(chipId, sizeof(chipId), "##diffchip%d", i);
                            chipClicked = ImGui::InvisibleButton(
                                chipId, ImVec2(runW, tankSz));
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                            }
                        } else {
                            ImGui::Dummy(ImVec2(runW, tankSz));
                        }
                        if (botDiffTag[0] && ImGui::IsItemHovered()) {
                            char chipTip[96];
                            SDL_snprintf(chipTip, sizeof(chipTip), "%s: %s",
                                         langGetText(STR_DLGLOBBY_BOTCFG_DIFFICULTY),
                                         botDiffTag);
                            ImGui::SetTooltip("%s", chipTip);
                        }
                        /* A click cycles to the next level of THIS mode and
                         * wraps; the mode decides how many there are. Sent
                         * through the same lobbySendBotConfig the gear's
                         * dropdown uses, and remembered as the standing
                         * preference the same way, so the two routes cannot
                         * disagree about what was picked. */
                        if (chipClicked && botModes != NULL) {
                            const BrainMode *lvlMode = &botModes->modes[botMode];
                            if (lvlMode->levelCount > 0) {
                                int next = (botLevel + 1) % lvlMode->levelCount;
                                lobbySendBotConfig(
                                    cs, (uint8_t)i, (uint8_t)botMode,
                                    (uint8_t)next,
                                    clientSimGetLobbyBotPersonality(cs, (BYTE)i),
                                    clientSimGetLobbySlot(cs, (BYTE)i)->playerName);
                                gameFrontSetChosenBotModeAndLevel(
                                    lvlMode->key, lvlMode->levels[next].key);
                            }
                        }
                    }
                    /* The config gear, right after the bot badge — the same
                     * column and the same place in the run that a human row
                     * puts the local player's voice gear (Andrew: "we might as
                     * well match the bot positions with that"). It used to sit
                     * in the ping column, a long way right of the human one,
                     * which made two gears on adjacent rows read as unrelated
                     * controls.
                     *
                     * Shown only to a client with lobby-edit authority (host /
                     * openHost / admin); anyone else gets no control rather
                     * than a dead one. Controller mode draws the visible
                     * ">"/"v" toggle instead of the invisible icon button, so
                     * it is reachable by gamepad and shows a focus ring — A
                     * expands the AiConfig sub-row, whose widgets (name,
                     * Codebase, mode, difficulty) then navigate like any other
                     * dialog control. */
                    if (effectiveHost) {
                        if (botTex) {
                            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x);
                        }
                        /* Put it in the SAME column as the human rows' voice
                         * gear, rather than wherever this row's badge run
                         * happened to end — a bot row's run is one badge long
                         * and a human's is four or five, so without this the
                         * two gears stagger down the list.
                         *
                         * The voice gear's X is DERIVED from this cell, not
                         * observed from the row that drew it. Observing it meant
                         * keeping last frame's reading in a static, and that
                         * reading goes stale in every case where the local
                         * player's row is not drawn this frame — scrolled out of
                         * the list, voice switched off, the splitter dragged
                         * meanwhile. A stale X from a wider layout sits right of
                         * this cell, and the table is NoClip, so the gear and its
                         * button would land on top of the player name. Deriving
                         * it cannot go stale and has no one-frame lag.
                         *
                         * Where the voice gear sits: kColIconsW ends with
                         * [mic][ItemSpacing][gear][ItemSpacing] in a voice build
                         * and [ItemSpacing][gear] without one, so the gear's left
                         * edge is the cell's right edge back by one gear, and by
                         * one more spacing when voice is compiled in.
                         *
                         * Never pulled left of where the badge ended, so a narrow
                         * column degrades to "straight after the badge" instead
                         * of drawing the gear on top of it. */
                        {
                            /* Prefer the REAL voice gear position, observed on
                             * the local player's row. Deriving it from the
                             * column's width instead lands about five to ten
                             * pixels right of it: the column reserves room for
                             * a flag and every badge, and a real row usually
                             * draws fewer than that, so the voice gear sits
                             * left of the reserved slot.
                             *
                             * Taken only while it is at most ONE frame old.
                             * Rows above the local player's read last frame's
                             * value, rows below read this frame's. Two frames
                             * with no voice gear — that row scrolled out of the
                             * list, or voice was switched off — and we fall
                             * back rather than trust a stale position. */
                            const int gearAge =
                                ImGui::GetFrameCount() - s_voiceGearFrame;
                            float gearX;
                            if (s_voiceGearX > 0.0f && gearAge <= 1) {
                                gearX = s_voiceGearX;
                            } else {
#if defined(WINBOLO_VOICE)
                                gearX = iconsCellRightX
                                      - ImGui::GetStyle().ItemSpacing.x
                                      - kColGearW;
#else
                                gearX = iconsCellRightX - kColGearW;
#endif
                            }
                            /* Never past this cell's right edge: the table is
                             * NoClip, so a position carried over from a wider
                             * layout would draw the gear, and its button, on
                             * top of the player name. And never left of where
                             * the badge run ended, so a narrow column degrades
                             * to "straight after the badge" instead of drawing
                             * over it. */
                            const float gearMaxX = iconsCellRightX - kColGearW;
                            if (gearX > gearMaxX) gearX = gearMaxX;
                            float afterBadgeX = ImGui::GetCursorScreenPos().x;
                            if (gearX < afterBadgeX) gearX = afterBadgeX;
                            ImGui::SetCursorScreenPos(
                                ImVec2(gearX, ImGui::GetCursorScreenPos().y));
                        }
                        drawBotGear();
                    }
                } else if (!isBot && showIconsCol) {
                    /* Same 2px upward nudge applied to text / tank /
                     * chip / gear — keeps every glyph in the row
                     * landing on a consistent optical center. */
                    const float iconBiasY = 2.0f;
                    const char *cc = clientSimGetLobbySlot(cs, (BYTE)(i))->countryCode;
                    if (cc[0] != '\0' && !(cc[0] == 'X' && cc[1] == 'X') && flagsGetTexture(cc)) {
                        cyAbs((float)FLAG_HEIGHT);
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - iconBiasY);
                        drawCountryFlagWithTip(cc);
                        ImGui::SameLine();
                    }
                    /* All WBN/Steam/platform icons in renderPlayerName are
                     * LOBBY_WBN_ICON_SIZE tall — center them as one block. */
                    cyAbs((float)LOBBY_WBN_ICON_SIZE);
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - iconBiasY);
                    {
                        /* Hide the WBN globe in local-only sessions —
                         * single-player has no WBN session and a LAN-only
                         * host would similarly skip WBN registration. Keep
                         * the Steam + platform badges since those signal
                         * what the player is running, not who they are on
                         * the leaderboard. */
                        uint8_t pflags = clientSimGetLobbySlot(cs, (BYTE)(i))->clientFlags;
                        if (clientSimIsSinglePlayer(cs) || clientSimIsLanOnly(cs)) {
                            pflags &= ~PLAYER_FLAG_WBN_VERIFIED;
                        }
                        renderPlayerName(NULL,
                                         pflags,
                                         clientSimGetLobbySlot(cs, (BYTE)(i))->clientType,
                                         "", false);
#if defined(WINBOLO_VOICE)
                        /* Voice state and the mute toggle. Lobby voice
                         * is all-talk, so this shows for every player. The
                         * slot's own flags, not pflags: the WBN masking
                         * above has nothing to say about the microphone.
                         * The true is what marks this as the lobby: it is
                         * the one place a player with no microphone is
                         * drawn, since picking who to play with is when
                         * knowing they cannot talk matters. */
                        renderPlayerMicCell(cs, i,
                                            clientSimGetLobbySlot(cs, (BYTE)(i))->clientFlags,
                                            talkingMap, isSelf,
                                            (float)LOBBY_WBN_ICON_SIZE, true);
                        /* Gear beside your own microphone, expanding the
                         * voice sub-row below. Your row only: nobody else's
                         * microphone is yours to change. Controller mode
                         * renders the visible ">"/"v" toggle (the SmallButton
                         * path below) instead of the invisible icon button,
                         * so it's reachable by gamepad nav and shows a focus
                         * ring — A expands the sub-row, whose widgets are
                         * then navigable like any other dialog control. */
                        if (isSelf) {
                            ImGui::SameLine();
                            if (lobbyIcons()->settings && !uiShouldUseControllerMode()) {
                                float iconSize = ImGui::GetFontSize();
                                cyAbs(iconSize);
                                /* settings.svg renders 5px above / 2px below
                                 * with pure geometric centering — the gear
                                 * sits slightly low in the row. Nudge up
                                 * ~12% of font size (about 1.5px) so it
                                 * matches the optical center used by the
                                 * text and tank widgets. */
                                ImGui::SetCursorPosY(ImGui::GetCursorPosY()
                                                     - ImGui::GetFontSize() * 0.12f);
                                ImVec2 iconStart = ImGui::GetCursorScreenPos();
                                /* The column every bot gear lines up with. */
                                s_voiceGearX     = iconStart.x;
                                s_voiceGearFrame = ImGui::GetFrameCount();
                                bool clicked = ImGui::InvisibleButton(
                                    "##voicecfg", ImVec2(iconSize, iconSize));
                                /* Grey so the gear reads as a secondary
                                 * action, full white on hover so it lights
                                 * up under the mouse — the same tint the
                                 * bot gear uses. */
                                ImU32 gearTint = ImGui::IsItemHovered()
                                    ? IM_COL32_WHITE
                                    : IM_COL32(180, 180, 180, 200);
                                ImGui::GetWindowDrawList()->AddImage(
                                    (ImTextureID)lobbyIcons()->settings,
                                    iconStart,
                                    ImVec2(iconStart.x + iconSize, iconStart.y + iconSize),
                                    ImVec2(0, 0), ImVec2(1, 1), gearTint);
                                if (clicked) {
                                    s_players.voiceRowExpanded = !s_players.voiceRowExpanded;
                                }
                                if (ImGui::IsItemHovered()) {
                                    ImGui::SetTooltip("%s",
                                        langGetText(STR_DLGLOBBY_TOOLTIP_VOICE));
                                }
                            } else {
                                cyAbs(ImGui::GetFrameHeight());
                                s_voiceGearX     = ImGui::GetCursorScreenPos().x;
                                s_voiceGearFrame = ImGui::GetFrameCount();
                                if (ImGui::SmallButton(s_players.voiceRowExpanded
                                                       ? "v##voicecfg"
                                                       : ">##voicecfg")) {
                                    s_players.voiceRowExpanded = !s_players.voiceRowExpanded;
                                }
                                if (ImGui::IsItemHovered()) {
                                    ImGui::SetTooltip("%s",
                                        langGetText(STR_DLGLOBBY_TOOLTIP_VOICE));
                                }
                            }
                        }
#endif
                    }
                }

                /* A bot's extra name tags: which bot it is, which mode it
                 * is in when that is not the default one, and how hard it
                 * plays. All of it comes from lobby state the server already
                 * syncs to every client (the brain catalogue + index, and
                 * LobbyBotConfig's mode + difficulty indices, resolved
                 * against the brain's own modes.txt), so joiners see the
                 * same run of tags the host does. Computed before the name
                 * is truncated because the truncation has to reserve room
                 * for them. */
                /* The mode and the difficulty are resolved higher up now, above
                 * the icons column, because the difficulty chips draw there. */

                /* ── Column 2: name + inline tags ────────────────── */
                ImGui::TableSetColumnIndex(2);
                rowTopY = ImGui::GetCursorPosY();
                cyTextAbs();
                /* Truncate the name to its column width (less room reserved for
                 * a HOST/ADMIN/BOT tag) so it can't overflow and push the start
                 * dropdown into the Ready button on small windows. */
                {
                    bool rowHasTag = showNameTags &&
                        ((i == clientSimGetLobbyHostSlot(cs)) || isBot ||
                         (!isBot && (clientSimGetLobbySlot(cs, (BYTE)(i))->clientFlags
                                     & PLAYER_FLAG_ADMIN)));
                    float nameAvail  = ImGui::GetContentRegionAvail().x;
                    float tagReserve = rowHasTag ? kNameTagW : 0.0f;
                    /* A bot row carries three tags, not one — four in a
                     * non-default mode: BOT, the bot's name ("GoalHunter"),
                     * the mode and its difficulty. Reserve what they
                     * actually measure so the name gives up the room
                     * instead of the tags spilling into the start dropdown.
                     * Same recipe drawNameTag uses below: 70% font, 6px pad
                     * each side, 6px gap before each pill. */
                    if (showBotDetailTags && isBot && botModeTag[0]) {
                        /* The MODE tag is the only extra a bot row carries here
                         * now. The codebase tag and the difficulty tag are both
                         * gone: the difficulty moved to the chips in the icons
                         * column, and the codebase is in the gear form. Reserve
                         * only what is still drawn, or the name gives up
                         * characters for tags that no longer exist. Recipe
                         * matches drawNameTag: 70% font, 6px pad each side, 6px
                         * gap before the pill. */
                        tagReserve += ImGui::CalcTextSize(botModeTag).x * 0.70f
                                    + 12.0f * s + 6.0f * s;
                    }
                    char nameBuf[64];
                    lobbyTruncateName(clientSimGetLobbySlot(cs, (BYTE)(i))->playerName,
                                      nameAvail - tagReserve, nameBuf, sizeof(nameBuf));
                    if (isBot) {
                        ImGui::PushStyleColor(ImGuiCol_Text,
                                              wbThemeColor(g_theme->botBadge));
                        ImGui::Text("%s", nameBuf);
                        ImGui::PopStyleColor();
                    } else if (isMe) {
                        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "%s", nameBuf);
                    } else {
                        ImGui::Text("%s", nameBuf);
                    }
                    lobbyNameJumpToPlayer(cs, i);
                }


                /* Inline tag pills after the name. Drawn via
                 * WindowDrawList so we can size them tightly and
                 * tint each one independently (HOST = yellow,
                 * BOT = muted blue-gray). Same pill recipe as the
                 * READY/NOT READY badge but at 70% font size. */
                auto drawNameTag = [&](const char *lbl, ImU32 bg, ImU32 fg,
                                       ImU32 border = 0) {
                    const float tagScale = 0.70f;
                    float tagFontSz = ImGui::GetFontSize() * tagScale;
                    ImVec2 baseSz = ImGui::CalcTextSize(lbl);
                    ImVec2 textSz(baseSz.x * tagScale, tagFontSz);
                    float padX = 6.0f * s;
                    float padY = 2.0f * s;
                    float pillW = textSz.x + padX * 2.0f;
                    float pillH = textSz.y + padY * 2.0f;
                    ImGui::SameLine(0.0f, 6.0f * s);
                    cyAbs(pillH);
                    /* Nudge HOST / BOT / ADMIN name-tags up 1px so
                     * they sit a touch above the row centerline,
                     * which lines them up better with the cap-height
                     * of the player name. */
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 1.0f);
                    ImVec2 pos = ImGui::GetCursorScreenPos();
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    /* Square corners on the name tags so they read as
                     * "labels", not status pills like READY. */
                    dl->AddRectFilled(pos,
                                      ImVec2(pos.x + pillW, pos.y + pillH),
                                      bg, 0.0f);
                    if (border != 0) {
                        dl->AddRect(pos,
                                    ImVec2(pos.x + pillW, pos.y + pillH),
                                    border, 0.0f, 0, 1.0f);
                    }
                    dl->AddText(ImGui::GetFont(), tagFontSz,
                                ImVec2(pos.x + padX, pos.y + padY),
                                fg, lbl);
                    ImGui::Dummy(ImVec2(pillW, pillH));
                };
                /* A tag's hover: an ordinary tooltip. The row's bot tags are
                 * abbreviations — "GH", three chips, a one-word mode — so the
                 * hover spells them out ("Codebase: GoalHunter").
                 *
                 * This used to draw the hover as a PILL in the tag's own
                 * colours. It read badly (Andrew: "the tag of 'Difficulty:
                 * Easy' looks weird"), so it is now the same plain tooltip
                 * every other control in the lobby uses. Call it straight after
                 * the tag, while that tag is still the current item. */
                auto tagTooltip = [&](const char *lbl) {
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", lbl);
                };
                if (showNameTags && i == clientSimGetLobbyHostSlot(cs)) {
                    /* Badge follows the current host slot. Themable bg /
                     * border / text triple lives in wb_theme.cpp. */
                    drawNameTag(langGetText(STR_DLGLOBBY_TAG_HOST),
                                g_theme->hostTagBg,
                                g_theme->hostTagText,
                                g_theme->hostTagBorder);
                }
                if (showNameTags && !isBot && i != clientSimGetLobbyHostSlot(cs) &&
                    (clientSimGetLobbySlot(cs, (BYTE)(i))->clientFlags & PLAYER_FLAG_ADMIN)) {
                    /* IP-matched admin (server -admins). Shown beside the
                     * name like HOST but in a distinct teal so it reads
                     * as a separate "host-level authority" badge. */
                    drawNameTag(langGetText(STR_DLGLOBBY_TAG_ADMIN),
                                IM_COL32(70, 160, 175, 255),
                                IM_COL32(10, 30, 35, 255));
                }
                if (showNameTags && isBot) {
                    drawNameTag(langGetText(STR_DLGLOBBY_TAG_BOT),
                                g_theme->botTagBg,
                                g_theme->botTagText,
                                g_theme->botTagBorder);
                    /* Which bot, then how hard. Both wear the BOT tag's
                     * themed colours so the run reads as one group; the
                     * difficulty is the one that changes, so it goes last
                     * where the eye lands after the name. */
                    char tagTip[BRAIN_LIST_NAME_LEN + 48];
                    /* No codebase tag any more. "GH" beside every bot said
                     * little once every bot was a GoalHunter, and the gear
                     * form's Codebase dropdown is the honest place for it. */
                    if (showBotDetailTags && botModeTag[0]) {
                        /* The mode, when it is not the default one — one word
                         * ("Survival"). Its own indigo, NOT the difficulty's
                         * amber set it used to borrow: the difficulty tag sits
                         * immediately beside it and is amber at Medium, so the
                         * two ran together (Andrew: "the mode tag orangey is
                         * the same as the medium tag orangey which looks bad").
                         * Indigo is taken by nothing else in the run — host is
                         * yellow, admin teal, BOT blue-grey, difficulty
                         * green/amber/red. The brain's own tag is drawn
                         * immediately before this one and is the only colour
                         * that could collide, but it takes botTagBg as its
                         * fill, so the risk is low. A literal triple, the way
                         * the ADMIN badge above does it — both would be better
                         * as WbTheme fields, since g_theme is swappable and a
                         * literal survives a theme change. */
                        const ImU32 kModeBg     = IM_COL32( 64,  72, 120, 255);
                        const ImU32 kModeText   = IM_COL32(198, 206, 255, 255);
                        const ImU32 kModeBorder = IM_COL32(104, 116, 190, 255);
                        drawNameTag(botModeTag, kModeBg, kModeText, kModeBorder);
                        SDL_snprintf(tagTip, sizeof(tagTip), "%s: %s",
                                     langGetText(STR_DLGLOBBY_BOTCFG_MODE),
                                     botModeTag);
                        tagTooltip(tagTip);
                    }
                    /* No difficulty tag any more either. It is the three chips
                     * beside the tank icon now, in the icons column, where it
                     * carries its own hover and its own click-to-cycle. */
                }
                /* Track the rightmost name/tag edge across all rows so the
                 * start dropdowns can line up in a shared column. The last
                 * item drawn here is the name text or its trailing tag pill. */
                startColNameMaxRight = ImMax(startColNameMaxRight,
                                             ImGui::GetItemRectMax().x);

                /* ── Column 3: ping (humans only) ────────────────────
                 * The bot's config gear used to live here. It now sits in the
                 * icons column beside the bot-cpu badge, where a human row puts
                 * the voice gear, so the two line up. A bot row leaves this
                 * cell empty. */
                ImGui::TableSetColumnIndex(3);
                rowTopY = ImGui::GetCursorPosY();
                if (showPingCol && !isBot) {
                    if (clientSimGetLobbySlot(cs, (BYTE)(i))->pingMs > 0) {
                        cyTextAbs();
                        ImVec4 pingColor;
                        switch (pingBandClassify(clientSimGetLobbySlot(cs, (BYTE)(i))->pingMs)) {
                            case PING_BAND_GOOD: pingColor = wbThemeColor(g_theme->statusOnline); break;
                            case PING_BAND_FAIR: pingColor = wbThemeColor(g_theme->statusHighPing); break;
                            default:             pingColor = wbThemeColor(g_theme->statusDisconnected); break;
                        }
                        ImGui::TextColored(pingColor, "%dms", (int)clientSimGetLobbySlot(cs, (BYTE)(i))->pingMs);
                    }
                } else if (showPingCol && isBot && effectiveHost && !showIconsCol) {
                    /* The icons column has been shed, so the gear's own home is
                     * gone — but the row is otherwise intact and the host still
                     * has to be able to configure this bot. Draw it here, where
                     * it lived before it moved. Nothing else sets
                     * expandedBotSlot, so without this the config is
                     * unreachable at these widths. */
                    drawBotGear();
                }

                /* ── Column 4: reserved map start, shown as the compass
                 * octant (N/NE/.../C) of where that start sits on the
                 * map, or — when the slot holds no reservation. Reuses
                 * the otherwise-empty spacer column so the label sits
                 * between the ping and ready cells without a table-wide
                 * column reshuffle. The octant comes from the cached
                 * s_mapPreview.startCompassId table (rebuilt on map change). */
                ImGui::TableSetColumnIndex(4);
                rowTopY = ImGui::GetCursorPosY();
                /* Spacer-column left edge == right edge of the ping/gear
                 * column. Used as a floor for the dropdown's left bound so
                 * it never overlaps the ping text / bot gear when names are
                 * short. Constant across rows; capture once. */
                if (showStartCol && startColPingRightX == 0.0f) {
                    startColPingRightX = ImGui::GetCursorScreenPos().x;
                }
                if (showStartCol) {
                    const ClientLobbySlot *cslot = clientSimGetLobbySlot(cs, (BYTE)(i));
                    uint8_t sIdx = cslot->startIdx;
                    const BYTE teamSide   = lobbyTeamSide(cs, teamId);
                    const bool hasSide    = startSideBits(teamSide) != 0;
                    const BYTE closedMask = lobbyClosedMaskForTeam(cs, teamId);
                    const bool holdsStart = cslot->connected && sIdx != 0xFF &&
                                            sIdx <= MAX_STARTS &&
                                            lobbyMapPreview()->startCompassId[sIdx] != 0;
                    /* An empty slot is placed at game start while a start its
                     * team can use is still free; once none is, it starts at
                     * sea beside a teammate. This is a forecast: the client
                     * cannot see which start squares are deep sea, so "free"
                     * here means on the map's start list and not held by a
                     * connected slot. The server's answer is the reservation
                     * that lands in CTRL_LOBBY_SLOT. */
                    const bool atSea = cslot->connected && !holdsStart &&
                                       lobbyMapPreview()->startCount > 0 &&
                                       !lobbyFreeEligibleStartExists(cs, i, teamId);
                    /* A held start the team's side rejects is a host override. */
                    const bool offSide = holdsStart && hasSide &&
                                         !startSideAccepts(lobbyStartSideMask(sIdx), teamSide);
                    /* "Sea · N" on a side team, "Sea" on an Any team. */
                    char seaLbl[32];
                    if (hasSide) {
                        SDL_snprintf(seaLbl, sizeof(seaLbl), "%s \xC2\xB7 %s",
                                     langGetText(STR_DLGLOBBY_START_SEA),
                                     langGetText(lobbySideCompassId(teamSide)));
                    } else {
                        SDL_snprintf(seaLbl, sizeof(seaLbl), "%s",
                                     langGetText(STR_DLGLOBBY_START_SEA));
                    }
                    /* A host edits any connected row; a non-host edits only
                     * its own. Everyone else sees the read-only compass.
                     * No optimistic apply — selecting just sends the
                     * command; the marker moves when CTRL_LOBBY_SLOT lands. */
                    bool canEditStart = cslot->connected && (effectiveHost || isMe);
                    /* Hover text for the cell just drawn: why an empty slot
                     * reads Sea, that a held start is off-side, or plain
                     * Unassigned behind the read-only dash. */
                    auto startCellTooltip = [&]() {
                        if (!ImGui::IsItemHovered()) return;
                        if (atSea) {
                            ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_TOOLTIP_START_SEA));
                        } else if (offSide) {
                            MessageArgs args = {};
                            SDL_strlcpy(args.string1, langGetText(lobbySideNameId(teamSide)),
                                        sizeof(args.string1));
                            ImGui::SetTooltip("%s", langGetTextFmt(STR_DLGLOBBY_TOOLTIP_START_OFFSIDE, &args));
                        } else if (!canEditStart && cslot->connected && !holdsStart) {
                            ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_START_UNASSIGNED));
                        }
                    };
                    if (!canEditStart) {
                        const char *startLbl = "—";
                        if (holdsStart) {
                            startLbl = langGetText(lobbyMapPreview()->startCompassId[sIdx]);
                        } else if (atSea) {
                            startLbl = seaLbl;
                        }
                        cyTextAbs();
                        if (appliedStartCenterX > 0.0f) {
                            float tw = ImGui::CalcTextSize(startLbl).x;
                            ImVec2 sp = ImGui::GetCursorScreenPos();
                            sp.x = appliedStartCenterX - tw * 0.5f;
                            ImGui::SetCursorScreenPos(sp);
                        }
                        ImGui::TextDisabled("%s", startLbl);
                        startCellTooltip();
                    } else {
                        char preview[64];
                        if (holdsStart) {
                            SDL_snprintf(preview, sizeof(preview),
                                         "#%u \xC2\xB7 %s", (unsigned)sIdx,
                                         langGetText(lobbyMapPreview()->startCompassId[sIdx]));
                        } else if (atSea) {
                            SDL_snprintf(preview, sizeof(preview), "%s", seaLbl);
                        } else {
                            SDL_snprintf(preview, sizeof(preview), "%s",
                                         langGetText(STR_DLGLOBBY_START_UNASSIGNED));
                        }
                        const float comboW = 96.0f * s;
                        cyAbs(ImGui::GetFrameHeight());
                        if (appliedStartCenterX > 0.0f) {
                            ImVec2 sp = ImGui::GetCursorScreenPos();
                            sp.x = appliedStartCenterX - comboW * 0.5f;
                            /* Scootch left if the centered combo would overlap
                             * the Ready cell on the right. */
                            if (appliedReadyLeft > 0.0f &&
                                sp.x + comboW > appliedReadyLeft - 4.0f * s)
                                sp.x = appliedReadyLeft - 4.0f * s - comboW;
                            ImGui::SetCursorScreenPos(sp);
                        }
                        char comboId[24];
                        SDL_snprintf(comboId, sizeof(comboId), "##start%d", i);
                        ImGui::SetNextItemWidth(comboW);
                        /* A slot bound for sea reads in the disabled style;
                         * the colour is popped straight after BeginCombo so
                         * the entries keep the normal text colour. */
                        if (atSea) {
                            ImGui::PushStyleColor(ImGuiCol_Text,
                                                  ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                        }
                        bool startOpen = ImGui::BeginCombo(comboId, preview);
                        if (atSea) ImGui::PopStyleColor();
                        if (!startOpen) {
                            startCellTooltip();
                        } else {
                            /* Starts in three groups: the starts the team may
                             * hold on its own side, then the centre band,
                             * then — host only, after a separator — the
                             * starts the side rules keep the team off, each
                             * marked off-side. A player picking for
                             * themselves never sees that last group: the
                             * server rejects such a claim with
                             * CMD_REJECT_INVALID. An Any team has no side of
                             * its own, so its first group is every start off
                             * the sides the other teams chose and the centre
                             * group is empty. */
                            bool offSideSep = false;
                            for (int group = 0; group < 3; group++) {
                                if (group == 2 && !effectiveHost) break;
                                for (int k = 1; k <= MAX_STARTS; k++) {
                                    if (lobbyMapPreview()->startCompassId[k] == 0) continue;
                                    BYTE mask = lobbyStartSideMask(k);
                                    int kGroup = 0;
                                    if (lobbyStartOffSideMasked(mask, teamSide, closedMask)) kGroup = 2;
                                    else if (hasSide && startSideIsCentre(mask))          kGroup = 1;
                                    if (kGroup != group) continue;
                                    /* Connected holders of start k, if any,
                                     * and their names — everyone but this
                                     * row, since "(Name)" says who else is
                                     * already there. */
                                    int holders[MAX_TANKS];
                                    int nHold = lobbyStartHolders(cs, k, holders, MAX_TANKS);
                                    const char *others[MAX_TANKS];
                                    int nOthers = 0;
                                    for (int h = 0; h < nHold; h++) {
                                        if (holders[h] == i) continue;
                                        others[nOthers++] =
                                            clientSimGetLobbySlot(cs, (BYTE)holders[h])->playerName;
                                    }
                                    bool occupiedByOther = (nOthers > 0);
                                    /* Non-host self-claim: only free starts
                                     * + own, unless starts can be shared —
                                     * then joining someone is an ordinary
                                     * pick and the server allows it. */
                                    if (!effectiveHost && occupiedByOther &&
                                        !lobbySharedStartsEnabled()) continue;
                                    if (group == 2 && !offSideSep) {
                                        ImGui::Separator();
                                        offSideSep = true;
                                    }
                                    char entry[160];
                                    if (occupiedByOther) {
                                        char who[96];
                                        lobbyStartHolderNameLabel(others, nOthers,
                                                                  who, sizeof(who));
                                        SDL_snprintf(entry, sizeof(entry),
                                                     "#%u \xC2\xB7 %s (%s)", (unsigned)k,
                                                     langGetText(lobbyMapPreview()->startCompassId[k]),
                                                     who);
                                    } else {
                                        SDL_snprintf(entry, sizeof(entry),
                                                     "#%u \xC2\xB7 %s", (unsigned)k,
                                                     langGetText(lobbyMapPreview()->startCompassId[k]));
                                    }
                                    if (group == 2) {
                                        size_t used = strlen(entry);
                                        SDL_snprintf(entry + used, sizeof(entry) - used, " %s",
                                                     langGetText(STR_DLGLOBBY_START_OFFSIDE_SUFFIX));
                                    }
                                    bool selected = (sIdx == (uint8_t)k);
                                    if (ImGui::Selectable(entry, selected)) {
                                        clientSimNetSendLobbyClaimStart(cs, (BYTE)i, (BYTE)k);
                                    }
                                    /* Outline this start on the preview while its
                                     * dropdown entry is hovered. */
                                    if (ImGui::IsItemHovered()) lobbyMapPreview()->hoveredStartChoice = k;
                                    if (selected) ImGui::SetItemDefaultFocus();
                                }
                            }
                            /* Two actions, neither a start. Team side empties
                             * the slot and the server re-picks it on the
                             * team's side at once (sea when the side is
                             * full); Unassign empties it and leaves it free
                             * until the next lobby event moves reservations.
                             * Sea is never a pick, only the outcome of a
                             * full side. */
                            ImGui::Separator();
                            char teamSideLbl[48];
                            if (hasSide) {
                                SDL_snprintf(teamSideLbl, sizeof(teamSideLbl), "%s \xC2\xB7 %s",
                                             langGetText(STR_DLGLOBBY_START_TEAM_SIDE),
                                             langGetText(lobbySideCompassId(teamSide)));
                            } else {
                                SDL_snprintf(teamSideLbl, sizeof(teamSideLbl), "%s",
                                             langGetText(STR_DLGLOBBY_START_AUTO));
                            }
                            if (ImGui::Selectable(teamSideLbl, false)) {
                                clientSimNetSendLobbyClaimStart(cs, (BYTE)i, START_CLAIM_TEAM_SIDE);
                            }
                            bool relSel = (sIdx == 0xFF);
                            if (ImGui::Selectable(langGetText(STR_DLGLOBBY_START_UNASSIGN), relSel)) {
                                clientSimNetSendLobbyClaimStart(cs, (BYTE)i, 0xFF);
                            }
                            if (relSel) ImGui::SetItemDefaultFocus();
                            ImGui::EndCombo();
                        }
                    }
                }

                /* ── Column 5: ready / not ready badge (humans only — bots
                 *   are always ready and don't need a pill) ─────────────── */
                ImGui::TableSetColumnIndex(5);
                rowTopY = ImGui::GetCursorPosY();
                /* Ready column geometry. The pill is right-aligned within
                 * this fixed-width column so it sits snug against the right
                 * of the row (just left of the kick-X), with extra breathing
                 * room after the start dropdown. matches TableSetupColumn. */
                const float readyColW   = 80.0f * s;
                const float readyInsetX = 4.0f * s;
                if (!isBot) {
                    bool isReady = clientSimGetLobbySlot(cs, (BYTE)(i))->ready;
                    const char *lbl = isReady ? langGetText(STR_DLGLOBBY_PILL_READY) : langGetText(STR_DLGLOBBY_PILL_NOTREADY);
                    /* Render the badge text at 80% of the row font
                     * size — a touch smaller than the player name
                     * so the pill reads as a status tag rather than
                     * a primary label. */
                    const float pillFontScale = 0.80f;
                    float pillFontSz = ImGui::GetFontSize() * pillFontScale;
                    ImVec2 baseSz = ImGui::CalcTextSize(lbl);
                    ImVec2 textSz(baseSz.x * pillFontScale, pillFontSz);
                    float padX  = 7.0f * s;
                    float padY  = 3.0f * s;
                    float pillW = textSz.x + padX * 2.0f;
                    float pillH = textSz.y + padY * 2.0f;
                    cyAbs(pillH);
                    /* Nudge the READY / NOT READY pill up 2px so it
                     * lines up with the cap-height of adjacent text
                     * rather than the row's geometric center. */
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 2.0f);
                    ImVec2 pillPos = ImGui::GetCursorScreenPos();
                    /* Right-align within the column; clamp so a very wide
                     * pill never spills out the left of the cell. */
                    float cellLeftX = pillPos.x;
                    pillPos.x = cellLeftX + readyColW - pillW - readyInsetX;
                    if (pillPos.x < cellLeftX) pillPos.x = cellLeftX;
                    /* The pill's left edge is the right bound of the gap the
                     * start dropdown centers in. Track the leftmost (widest
                     * pill) across rows so the dropdown clears every pill. */
                    if (startColReadyLeftX == 0.0f || pillPos.x < startColReadyLeftX)
                        startColReadyLeftX = pillPos.x;
                    ImU32 bgCol = isReady ? IM_COL32(42, 80, 44, 255)
                                          : IM_COL32(58, 58, 58, 255);
                    ImU32 fgCol = isReady ? IM_COL32(120, 210, 120, 255)
                                          : IM_COL32(180, 180, 180, 255);
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    dl->AddRectFilled(pillPos,
                                      ImVec2(pillPos.x + pillW,
                                             pillPos.y + pillH),
                                      bgCol, pillH * 0.5f);
                    dl->AddText(ImGui::GetFont(), pillFontSz,
                                ImVec2(pillPos.x + padX,
                                       pillPos.y + padY),
                                fgCol, lbl);
                    ImGui::Dummy(ImVec2(pillW, pillH));
                }

                /* ── Column 6: remove bot X / kick player X (CloseButton) ──
                 * Bots get an immediate remove; humans (non-self, non-host)
                 * stage a kick-confirm modal. Both visible only to host /
                 * admin / openHost — server re-validates either way. */
                ImGui::TableSetColumnIndex(6);
                rowTopY = ImGui::GetCursorPosY();
                if (showXCol && isBot && effectiveHost) {
                    cyAbs(closeSz);
                    ImVec2 closePos = ImGui::GetCursorScreenPos();
                    char rbStr[24];
                    SDL_snprintf(rbStr, sizeof(rbStr), "##rb%d", i);
                    ImGuiID rbId = ImGui::GetID(rbStr);
                    if (ImGui::CloseButton(rbId, closePos)) {
                        lobbySendRemoveBot(cs, (uint8_t)i);
                        if (s_players.expandedBotSlot == i) s_players.expandedBotSlot = -1;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_TOOLTIP_RMBOT));
                    }
                } else if (showXCol && !isBot && !isMe &&
                           i != clientSimGetLobbyHostSlot(cs) && effectiveHost) {
                    cyAbs(closeSz);
                    ImVec2 basePos = ImGui::GetCursorScreenPos();
                    /* Host-only "Make host" promote button, drawn to the
                     * left of the kick X. openHost/admin (effectiveHost)
                     * can kick but must NOT transfer the host role, so
                     * this is gated on lobbyIsHost, not effectiveHost. */
                    if (lobbyIsHost(cs, myPlayerNum)) {
                        ImVec2 mhPos = basePos;
                        ImGui::SetCursorScreenPos(mhPos);
                        char mhStr[24];
                        SDL_snprintf(mhStr, sizeof(mhStr), "##mh%d", i);
                        bool mhClicked = ImGui::InvisibleButton(mhStr, ImVec2(closeSz, closeSz));
                        ImU32 mhTint = ImGui::IsItemHovered()
                            ? IM_COL32_WHITE : IM_COL32(180, 180, 180, 200);
                        ImDrawList *mhDl = ImGui::GetWindowDrawList();
                        /* Up-triangle "promote" glyph. Visual placeholder —
                         * the human may swap this for a crown later. */
                        ImVec2 apex(mhPos.x + closeSz * 0.50f, mhPos.y + closeSz * 0.20f);
                        ImVec2 bl  (mhPos.x + closeSz * 0.15f, mhPos.y + closeSz * 0.80f);
                        ImVec2 br  (mhPos.x + closeSz * 0.85f, mhPos.y + closeSz * 0.80f);
                        mhDl->AddTriangleFilled(apex, bl, br, mhTint);
                        if (mhClicked) {
                            s_players.makeHostPendingSlot = i;
                            SDL_strlcpy(s_players.makeHostPendingName,
                                        clientSimGetLobbySlot(cs, (BYTE)i)->playerName,
                                        sizeof(s_players.makeHostPendingName));
                            s_players.makeHostPendingOpen = true;
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_MAKE_HOST));
                        }
                        /* Shift the kick X right so the two controls sit
                         * side by side in the column. */
                        basePos.x += closeSz + 4.0f * s;
                    }
                    /* CloseButton takes an explicit position and only calls
                     * ItemAdd (not ItemSize), so do NOT move the layout
                     * cursor here: a SetCursorScreenPos past the content max
                     * leaves ImGui's IsSetPos flag unvalidated and trips the
                     * "SetCursorPos to extend boundaries" assert at cell end.
                     * The InvisibleButton above already grew the cell. */
                    ImVec2 closePos = basePos;
                    char kbStr[24];
                    SDL_snprintf(kbStr, sizeof(kbStr), "##kb%d", i);
                    ImGuiID kbId = ImGui::GetID(kbStr);
                    if (ImGui::CloseButton(kbId, closePos)) {
                        s_players.kickPendingSlot = i;
                        SDL_strlcpy(s_players.kickPendingName,
                                    clientSimGetLobbySlot(cs, (BYTE)i)->playerName,
                                    sizeof(s_players.kickPendingName));
                        s_players.kickPendingOpen = true;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_TOOLTIP_KICK));
                    }
                }

                /* AiConfig sub-row when the wrench is expanded.
                 * Rendered as its own table row, but we only put
                 * content in column 0 — the form's widgets render
                 * past the column boundary and visually span the
                 * full team panel. The horizontal border below this
                 * row groups the form with the bot above it via the
                 * normal inter-row line. */
                if (isBot && s_players.expandedBotSlot == i && effectiveHost) {
                    ImGui::TableNextRow();
                    /* Inherit the parent bot row's stripe color so the
                     * sub-row reads as a continuation of that row and
                     * doesn't disturb the odd/even pattern. */
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, rowStripe);
                    ImGui::TableSetColumnIndex(0);
                    renderBotAiConfig(cs, i, teamId, s);
                }
#if defined(WINBOLO_VOICE)
                /* Voice sub-row when your own gear is expanded. Built the
                 * same way as the AiConfig one above: its own table row with
                 * content in column 0 only, which the NoClip table lets
                 * spread across the row, and the parent row's stripe so it
                 * reads as a continuation of your row. */
                if (isSelf && s_players.voiceRowExpanded) {
                    ImGui::TableNextRow();
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, rowStripe);
                    ImGui::TableSetColumnIndex(0);
                    renderOwnVoiceConfig(cs, s, contentW);
                }
#endif
            }
            ImGui::EndTable();
        }

        /* "+ Bot" button moved to the team header bar (right-aligned
         * next to the "Bot Naming:" pool dropdown) so all team-bot
         * controls live in one place. */

        ImGui::EndChild();
        /* Drop target: the team child window is the last item. Any
         * BeginDragDropSource elsewhere can drop a slot index here
         * to move that player onto this team. While a drag is over
         * the team, paint a translucent highlight along its border. */
        if (!spectator && ImGui::BeginDragDropTarget()) {
            ImGuiDragDropFlags flags = ImGuiDragDropFlags_AcceptBeforeDelivery;
            const ImGuiPayload *payload = ImGui::AcceptDragDropPayload(
                "WB_LOBBY_PLAYER", flags);
            if (payload) {
                /* Paint a thicker, brighter border while the payload
                 * hovers the team child. */
                ImVec2 itemMin = ImGui::GetItemRectMin();
                ImVec2 itemMax = ImGui::GetItemRectMax();
                ImU32 hl = (tc & 0x00FFFFFF) | (0xC0 << 24);
                ImGui::GetForegroundDrawList()->AddRect(
                    itemMin, itemMax, hl, 4.0f, 0, 3.0f);
                if (payload->IsDelivery() &&
                    payload->DataSize == (int)sizeof(uint8_t)) {
                    uint8_t fromSlot = *(const uint8_t *)payload->Data;
                    if (fromSlot < MAX_TANKS) {
                        lobbySendTeamSet(cs,
                                         fromSlot, (uint8_t)teamId);
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::Spacing();
    }

    /* Update the shared start-dropdown center from this frame's measured
     * bounds, for use next frame. The left bound is whichever sits further
     * right — the widest name(+tag) or the ping/gear column — plus a small
     * pad, so the dropdown is centered in the clear gap before Ready and
     * never collides with the ping text / bot gear. Guarded so a degenerate
     * frame (no rows, or no room, or the start column shed on a narrow
     * players column) leaves the previous value untouched rather than
     * snapping the column. */
    if (startColReadyLeftX > 0.0f && startColPingRightX > 0.0f) {
        float startColLeftBound =
            ImMax(startColNameMaxRight, startColPingRightX) + 8.0f * s;
        if (startColReadyLeftX > startColLeftBound) {
            s_startComboCenterX = (startColLeftBound + startColReadyLeftX) * 0.5f;
        }
        s_startComboReadyLeft = startColReadyLeftX;  /* for next-frame clamp */
    }

    /* "Add Team" lives at the bottom of the team list so it reads as
     * "+ another team after these ones" rather than a header action.
     * Picks the lowest unused teamId and sends a default-name TEAM_META
     * (SP path writes the TeamMetadata directly since the wire packet
     * has no SP equivalent). */
    if (effectiveHost) {
        /* Full-width "Add Team" affordance, styled to read as a subtle
         * "+1 row" prompt rather than a primary action — translucent
         * background and dimmed text so it doesn't dominate the team
         * list it sits beneath. */
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
        bool addTeamClicked = ImGui::Button(langGetText(STR_DLGLOBBY_ADD_TEAM), ImVec2(-1, 0));
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        if (addTeamClicked) {
            for (int t = 1; t < 16; t++) {
                if (memberCount[t] == 0 && !clientSimGetLobbyTeamInUse(cs, (BYTE)(t))) {
                    char defaultName[16];
                    {
                        MessageArgs args = {};
                        args.number = t;
                        SDL_snprintf(defaultName, sizeof(defaultName), "%s", langGetTextFmt(STR_DLGLOBBY_TEAM_HEADER, &args));
                    }
                    uint8_t color = (uint8_t)((t - 1) & 7);
                    clientSimNetSendLobbyTeamMeta(cs, (uint8_t)t,
                        color, 0 /*pool=classic*/, START_SIDE_ANY, defaultName);
                    break;
                }
            }
        }
        ImGui::Spacing();
    }

    /* Unassigned tray. */
    if (unassignedCount > 0) {
        {
            MessageArgs args = {};
            args.number = unassignedCount;
            ImGui::TextDisabled("%s", langGetTextFmt(STR_DLGLOBBY_UNASSIGNED_FMT, &args));
        }
        for (int i = 0; i < MAX_TANKS; i++) {
            if (!clientSimGetLobbySlot(cs, (BYTE)(i))->connected) continue;
            if (clientSimGetLobbySlot(cs, (BYTE)(i))->teamNumber != 0) continue;
            ImGui::Bullet();
            if (i == myPlayerNum) {
                MessageArgs args = {};
                SDL_strlcpy(args.playerName,
                            clientSimGetLobbySlot(cs, (BYTE)(i))->playerName,
                            sizeof(args.playerName));
                ImGui::Text("%s", langGetTextFmt(STR_DLGLOBBY_YOU_FMT, &args));
            } else {
                ImGui::Text("%s", clientSimGetLobbySlot(cs, (BYTE)(i))->playerName);
            }
        }
    }

    /* No bottom team picker — Layout A relies on drag-to-assign + the
     * default-team logic on the server. Players who want to switch
     * teams can be dragged by the host (planned) or via context menu. */

    /* Connected spectators, listed below the teams + unassigned tray. */
    renderSpectatorGroup(cs, myPlayerNum, s);

    /* Deferred-open kick-confirm modal. OpenPopup must happen in the
     * same ID scope as BeginPopupModal, so we set a flag inside the
     * team child windows and pop it open here at the outer scope. */
    if (s_players.kickPendingOpen) {
        ImGui::OpenPopup("##kickConfirm");
        s_players.kickPendingOpen = false;
    }
    if (ImGui::BeginPopupModal("##kickConfirm", NULL,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        {
            MessageArgs args = {};
            SDL_strlcpy(args.playerName, s_players.kickPendingName, sizeof(args.playerName));
            ImGui::Text("%s", langGetTextFmt(STR_DLGLOBBY_KICK_FMT, &args));
        }
        ImGui::Spacing();
        if (ImGui::Button(langGetText(STR_YES), ImVec2(80.0f * s, 0))) {
            if (s_players.kickPendingSlot > 0 && s_players.kickPendingSlot < MAX_TANKS) {
                clientSimNetSendLobbyKick(cs, (uint8_t)s_players.kickPendingSlot);
            }
            s_players.kickPendingSlot = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(langGetText(STR_CANCEL), ImVec2(80.0f * s, 0))) {
            s_players.kickPendingSlot = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (s_players.makeHostPendingOpen) {
        ImGui::OpenPopup("##makeHostConfirm");
        s_players.makeHostPendingOpen = false;
    }
    if (ImGui::BeginPopupModal("##makeHostConfirm", NULL,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        {
            MessageArgs args = {};
            SDL_strlcpy(args.playerName, s_players.makeHostPendingName, sizeof(args.playerName));
            ImGui::Text("%s", langGetTextFmt(STR_DLGLOBBY_MAKE_HOST_FMT, &args));
        }
        ImGui::Spacing();
        if (ImGui::Button(langGetText(STR_YES), ImVec2(80.0f * s, 0))) {
            if (s_players.makeHostPendingSlot >= 0 && s_players.makeHostPendingSlot < MAX_TANKS) {
                clientSimNetSendLobbyTransferHost(cs, (uint8_t)s_players.makeHostPendingSlot);
            }
            s_players.makeHostPendingSlot = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(langGetText(STR_CANCEL), ImVec2(80.0f * s, 0))) {
            s_players.makeHostPendingSlot = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

/* ── Layout A — bot AiConfig sub-row ──────────────────────────────
 * Inline panel under an expanded bot row showing the name override
 * with dice-reroll, difficulty dropdown, personality dropdown, and
 * close button. Edits dispatch as PACKET_LOBBY_BOT_CONFIG. */
static void renderBotAiConfig(ClientSim *cs,
                              int slot, int teamId, float s) {
    if (slot < 0 || slot >= MAX_TANKS) return;
    /* Render the whole AiConfig form at a smaller font so it reads
     * as a secondary control surface beneath the bot row. Saved +
     * restored so we don't bleed into the rest of the lobby. */
    float aicfgOldScale = ImGui::GetCurrentWindow()->FontWindowScale;
    ImGui::SetWindowFontScale(aicfgOldScale * 0.85f);

    /* Build the "currently used names" array for the dice reroll —
     * collect every bot name in the lobby so we don't collide. */
    const char *usedNames[MAX_TANKS];
    int usedCount = 0;
    for (int i = 0; i < MAX_TANKS; i++) {
        if (clientSimGetLobbySlot(cs, (BYTE)(i))->connected && clientSimGetLobbySlot(cs, (BYTE)(i))->isBot &&
            clientSimGetLobbySlot(cs, (BYTE)(i))->playerName[0]) {
            usedNames[usedCount++] = clientSimGetLobbySlot(cs, (BYTE)(i))->playerName;
        }
    }

    /* Edit form sits flush with the left edge of the team panel —
     * no internal indent. (The caller already strips the surrounding
     * 50px row indent so the form spans the full team width.) */
    ImGui::PushID(slot);

    /* Side-by-side groups (Name, Bot Code, Mode, Difficulty), wrapping
     * onto further rows when the team panel is too narrow for them.
     * BeginGroup + SameLine works inside the outer table cell (which is
     * NoClip-enabled), unlike a nested BeginTable which gets clipped to
     * the parent's narrow column width and squashes the controls. */
    char nameBuf[32];
    strncpy(nameBuf, clientSimGetLobbySlot(cs, (BYTE)(slot))->playerName, sizeof(nameBuf) - 1);
    nameBuf[sizeof(nameBuf) - 1] = '\0';
    bool nameChanged = false;
    bool diceClicked = false;
    const BrainList *bl = clientSimGetLobbyBrainList(cs);
    int pendingBrainPick = -1;

    /* Capture the form's top Y once and anchor every group there
     * explicitly via SetCursorScreenPos. Relying on SameLine alone
     * was producing a few-pixel drift (Name appearing higher than
     * Bot Code / Done) — probably because the Name group's
     * frame-bordered InputText shifts the group's "start Y" by
     * the FrameBorderSize when ItemAdd commits it. An explicit
     * anchor sidesteps that entirely. */
    ImVec2 formAnchor = ImGui::GetCursorScreenPos();
    /* Nudge the whole form 2px down (purely cosmetic — the
     * AiConfig content felt visually crowded against the bot row
     * above). The end-of-function SetCursorScreenPos subtracts the
     * same nudge so the parent container's total height is
     * unchanged. */
    const float kFormNudgeY = 2.0f;
    formAnchor.y += kFormNudgeY;

    /* ── Flow layout ──────────────────────────────────────────────────
     * The groups (Name, Bot Code, Mode, Difficulty) run left to right
     * along a row while they fit, and wrap onto a further row when they
     * do not. Each one is measured BEFORE it is placed, so a group that
     * has no room starts a new row instead of being drawn under the Done
     * button or off the right of the team panel.
     *
     * The right edge is worked out once and used by every row: the panel's
     * right edge, less the room the Done button keeps for itself and the
     * window's own padding. The button only sits on the top row, but a row
     * that stopped short of it and one that ran past it would not line up,
     * and the reserve is small enough not to cost a row anything. */
    const ImGuiStyle &fst = ImGui::GetStyle();
    const float kGroupGapX   =  24.0f * s;  /* between groups on a row */
    const float kRowGapY     =   8.0f * s;  /* between wrapped rows */
    const float kDoneReserve = 100.0f * s;  /* room kept for Done */
    const float winRightX =
        ImGui::GetWindowPos().x + ImGui::GetWindowSize().x;
    const float flowRightX = winRightX - kDoneReserve - fst.WindowPadding.x;

    float flowX       = formAnchor.x;  /* where the next group starts */
    float flowY       = formAnchor.y;
    float rowBottomY  = formAnchor.y;  /* lowest point on the current row */
    float formBottomY = formAnchor.y;  /* lowest point over every row */
    bool  rowHasGroup = false;

    /* Where a group of this width goes: beside the previous one, or at the
     * start of a new row when it would cross the right edge. The first
     * group on a row is placed however wide it is — there is nowhere
     * narrower to send it, and wrapping it would leave an empty row. */
    auto flowPlace = [&](float groupW) -> ImVec2 {
        if (rowHasGroup && flowX + groupW > flowRightX) {
            flowY = rowBottomY + kRowGapY;
            flowX = formAnchor.x;
            rowBottomY = flowY;
            rowHasGroup = false;
        }
        return ImVec2(flowX, flowY);
    };
    /* Called straight after the group's EndGroup, while it is still the
     * current item, so the cursor moves on by what the group really
     * measured — the width handed to flowPlace is only a forecast, and the
     * difficulty group's wrapped description makes it wider than its
     * combo. Also feeds the row's and the form's bottom. */
    auto flowPlaced = [&]() {
        const ImVec2 rmax = ImGui::GetItemRectMax();
        if (rmax.y > rowBottomY)  rowBottomY  = rmax.y;
        if (rmax.y > formBottomY) formBottomY = rmax.y;
        flowX = rmax.x + kGroupGapX;
        rowHasGroup = true;
    };

    /* Name group. */
    const float kNameInputW = 180.0f * s;
    const float diceBtnW =
        ImGui::CalcTextSize(langGetText(STR_DLGLOBBY_BOTCFG_REROLL)).x
        + fst.FramePadding.x * 2.0f;
    const float nameGroupW = ImMax(
        ImGui::CalcTextSize(langGetText(STR_DLGLOBBY_BOTCFG_NAME)).x,
        kNameInputW + fst.ItemSpacing.x + diceBtnW);
    ImGui::SetCursorScreenPos(flowPlace(nameGroupW));
    ImGui::BeginGroup();
    ImGui::TextDisabled("%s", langGetText(STR_DLGLOBBY_BOTCFG_NAME));
    ImGui::SetNextItemWidth(kNameInputW);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    nameChanged = ImGui::InputText("##botname", nameBuf, sizeof(nameBuf),
                                   ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleVar();
    ImGui::SameLine();
    diceClicked = ImGui::Button(langGetText(STR_DLGLOBBY_BOTCFG_REROLL));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_BOTCFG_REROLL_TIP));
    }
    ImGui::EndGroup();
    flowPlaced();

    /* Codebase group, next in the flow after the Name group.
     *
     * Always shown, even when the catalogue holds a single brain. It used to
     * hide itself at one entry, on the reasoning that a one-entry dropdown
     * invites "what else is there?" — but players are going to be able to add
     * their own brains, and a control that appears only once a second one is
     * installed is a control nobody knows to look for. Andrew: "we need a
     * Codebase dropdown, even if it's only GoalHunter right now (we will
     * allow users to add their own later)". */
    bool  botCodeShown  = (bl->count > 0);
    const float comboW  = 240.0f * s;
    if (botCodeShown) {
        const float bcGroupW = ImMax(
            ImGui::CalcTextSize(langGetText(STR_DLGLOBBY_BOTCFG_CODE)).x,
            comboW);
        ImGui::SetCursorScreenPos(flowPlace(bcGroupW));
        ImGui::BeginGroup();
        ImGui::TextDisabled("%s", langGetText(STR_DLGLOBBY_BOTCFG_CODE));
        uint8_t curIdx = clientSimGetLobbyBotBrain(cs, (BYTE)(slot));
        const BrainListEntry *curEntry =
            (curIdx != 0xFF && curIdx < bl->count) ? &bl->entries[curIdx] : NULL;
        char preview[BRAIN_LIST_NAME_LEN + BRAIN_LIST_VER_LEN + 8];
        if (curEntry) {
            if (curEntry->version[0])
                SDL_snprintf(preview, sizeof(preview), "%s (%s)",
                             curEntry->name, curEntry->version);
            else
                SDL_snprintf(preview, sizeof(preview), "%s", curEntry->name);
        } else {
            SDL_snprintf(preview, sizeof(preview), "%s", langGetText(STR_DLGLOBBY_BOTCFG_NONE));
        }
        ImGui::SetNextItemWidth(comboW);
        if (ImGui::BeginCombo("##botbrain", preview)) {
            for (int b = 0; b < bl->count; b++) {
                const BrainListEntry *e = &bl->entries[b];
                char label[BRAIN_LIST_NAME_LEN + BRAIN_LIST_VER_LEN + 8];
                if (e->version[0])
                    SDL_snprintf(label, sizeof(label), "%s (%s)",
                                 e->name, e->version);
                else
                    SDL_snprintf(label, sizeof(label), "%s", e->name);
                bool sel = (curEntry == e);
                if (ImGui::Selectable(label, sel)) {
                    pendingBrainPick = b;
                }
                /* Hover shows the short one-line tagline only. The longer
                 * about.txt description is intentionally not surfaced in the
                 * dropdown — it stays in the file for reference. */
                const LobbyBrainMeta *m = lobbyBrainMetaFor(e->name);
                if (m && m->tagline[0] && ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    lobbyDrawTagline(m->tagline, 320.0f * s);
                    ImGui::EndTooltip();
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::EndGroup();
        flowPlaced();
    }

    /* ── Mode and Difficulty groups ─────────────────────────────────────────
     * The brain says which modes it has and which difficulty levels each
     * of those modes offers (brains/<brain>/modes.txt, read locally on
     * every client — see lobbyBotModesFor). The lobby only picks indices:
     * Mode indexes the brain's mode list, Difficulty indexes the SELECTED
     * mode's level list, so switching mode re-fills the level dropdown.
     * For the default mode that list is Easy / Medium / Hard and the
     * indices are the same 0/1/2 the wire has always carried.
     *
     * Every mode and every level plays the same way for now; only the
     * wording and the tokens handed to the brain differ. */
    const BrainModes *modes = lobbyBotModesFor(cs, slot);
    int curMode = 0, curLevel = 0;
    lobbyBotModeAndLevel(cs, slot, &curMode, &curLevel);
    const BrainMode *modeSel = (modes != NULL) ? &modes->modes[curMode] : NULL;
    const uint8_t curPers = clientSimGetLobbyBotPersonality(cs, (BYTE)(slot));

    const float kModeComboW = 160.0f * s;
    if (modes != NULL) {
        const float mdGroupW = ImMax(
            ImGui::CalcTextSize(langGetText(STR_DLGLOBBY_BOTCFG_MODE)).x,
            kModeComboW);
        ImGui::SetCursorScreenPos(flowPlace(mdGroupW));
        ImGui::BeginGroup();
        ImGui::TextDisabled("%s", langGetText(STR_DLGLOBBY_BOTCFG_MODE));
        const char *modeItems[BRAIN_MODES_MAX];
        for (int m = 0; m < modes->modeCount; m++) {
            modeItems[m] = modes->modes[m].label;
        }
        int mode = curMode;
        ImGui::SetNextItemWidth(kModeComboW);
        if (ImGui::Combo("##botmode", &mode, modeItems, modes->modeCount) &&
            mode >= 0 && mode < modes->modeCount) {
            /* A mode change carries the new mode's own default level: the
             * old index means something different (or nothing) in the new
             * mode's list, so keeping it would show a level the player
             * never picked. */
            const BrainMode *nm = &modes->modes[mode];
            uint8_t lvl = (uint8_t)nm->defaultLevel;
            lobbySendBotConfig(cs, (uint8_t)slot, (uint8_t)mode, lvl, curPers,
                               clientSimGetLobbySlot(cs, (BYTE)(slot))->playerName);
            gameFrontSetChosenBotModeAndLevel(nm->key, nm->levels[lvl].key);
        }
        ImGui::EndGroup();
        flowPlaced();
    }

    const float kDiffComboW = 110.0f * s;
    /* No Difficulty group at all when the mode declares no levels: the brain
     * has one way of playing, so there is nothing to choose. The row's chip
     * tag is suppressed the same way. */
    if (modeSel != NULL && modeSel->levelCount > 0) {
        /* The description below the combo wraps to this group's own width,
         * so the combo alone decides where the group goes. */
        const float dfGroupW = ImMax(
            ImGui::CalcTextSize(langGetText(STR_DLGLOBBY_BOTCFG_DIFFICULTY)).x,
            kDiffComboW);
        const ImVec2 dfPos = flowPlace(dfGroupW);
        ImGui::SetCursorScreenPos(dfPos);
        ImGui::BeginGroup();
        ImGui::TextDisabled("%s", langGetText(STR_DLGLOBBY_BOTCFG_DIFFICULTY));
        /* The default mode's easy / medium / hard keep their translated
         * names; any other mode's levels are data and show their own label,
         * because there are no lang strings for something a brain invented. */
        const bool langLevels = lobbyBotModeUsesLangLevels(modes, curMode);
        const char *levelItems[BRAIN_LEVELS_MAX];
        for (int l = 0; l < modeSel->levelCount; l++) {
            levelItems[l] = langLevels
                ? langGetText(lobbyBotDifficultyLabelId((uint8_t)l))
                : modeSel->levels[l].label;
        }
        int diff = curLevel;
        ImGui::SetNextItemWidth(kDiffComboW);
        if (ImGui::Combo("##diff", &diff, levelItems, modeSel->levelCount) &&
            diff >= 0 && diff < modeSel->levelCount) {
            lobbySendBotConfig(cs, (uint8_t)slot, (uint8_t)curMode,
                (uint8_t)diff, curPers,
                clientSimGetLobbySlot(cs, (BYTE)(slot))->playerName);
            /* An explicit pick here is the player's standing preference —
             * persist it so it becomes the default on every future launch
             * (gospel; overrides the single-player skill guess). This is the
             * role the Bot Code dropdown used to play. */
            gameFrontSetChosenBotModeAndLevel(modeSel->key,
                                              modeSel->levels[diff].key);
        }
        /* Description of the CURRENT difficulty. Wrapped to whatever is
         * left between this group and the right edge of the row it landed
         * on — a group that wrapped onto a row of its own has the full
         * width. Capped so it doesn't turn into one very long line on a
         * wide window and floored so a narrow one still gets a readable
         * column (the text goes taller instead, which the row height
         * follows). */
        float descWrapW = flowRightX - dfPos.x;
        if (descWrapW > 300.0f * s) descWrapW = 300.0f * s;
        if (descWrapW < 140.0f * s) descWrapW = 140.0f * s;
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + descWrapW);
        if (langLevels) {
            ImGui::TextDisabled("%s",
                langGetText(lobbyBotDifficultyDescId((uint8_t)diff)));
        } else {
            /* No blurb exists for a manifest-defined level, so its label is
             * the honest thing to show. */
            ImGui::TextDisabled("%s", modeSel->levels[diff].label);
        }
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        flowPlaced();
    }

    if (diceClicked) {
        /* Pick a name from this team's pool, excluding all currently-used
         * bot names (incl. this one — we want a NEW name, not the same).
         * Reroll = pool-driven, so clear the manual-override flag. */
        int pool = (teamId > 0 && teamId < 16) ? clientSimGetLobbyTeamPool(cs, (BYTE)(teamId)) : 0;
        char pickBuf[32];
        lobbyBotPoolPick(pool, usedNames, usedCount, pickBuf, sizeof(pickBuf));
        lobbySendBotConfig(cs, (uint8_t)slot, (uint8_t)curMode,
            (uint8_t)curLevel, curPers, pickBuf);
        if (slot < MAX_TANKS) lobbyCommandBotNameOverridden()[slot] = false;
    }
    if (nameChanged) {
        /* Manual edit — pin the name so a later pool change doesn't
         * overwrite it. */
        lobbySendBotConfig(cs, (uint8_t)slot, (uint8_t)curMode,
            (uint8_t)curLevel, curPers, nameBuf);
        if (slot < MAX_TANKS) lobbyCommandBotNameOverridden()[slot] = true;
    }
    if (pendingBrainPick >= 0 && pendingBrainPick < bl->count) {
        /* Stash as the sticky default so subsequent Add Bot clicks inherit
         * this choice. Only reachable on a dev tree with more than one brain
         * (the combo is hidden otherwise); the persisted player preference is
         * the DIFFICULTY, written by the dropdown above, not the brain. */
        *lobbyBrainLastChosenIdx() = (uint8_t)pendingBrainPick;
        lobbySendSetBotBrain(cs, (uint8_t)slot,
                             (uint8_t)pendingBrainPick);
    }

    /* The Personality dropdown stays hidden — nothing consumes the field
     * yet. Kept in the code (and still wired through lobbySendBotConfig)
     * so flipping this flag to true is the only thing needed to show it
     * once the brain honours it. Difficulty graduated out of here: it is
     * its own group above. */
    const bool kShowAiOptions = false;

    /* ── Personality dropdown ─────────────────────────────────────
     * Joins the same flow as the other groups if it is ever turned on,
     * so it wraps with them rather than shoving them off the row. */
    if (kShowAiOptions) {
        const float kPersComboW = 110.0f * s;
        const float persGroupW = ImMax(
            ImGui::CalcTextSize(
                langGetText(STR_DLGLOBBY_BOTCFG_PERSONALITY)).x,
            kPersComboW);
        ImGui::SetCursorScreenPos(flowPlace(persGroupW));
        ImGui::BeginGroup();
        ImGui::TextDisabled("%s", langGetText(STR_DLGLOBBY_BOTCFG_PERSONALITY));
        const char *persItems[] = { langGetText(STR_DLGLOBBY_BOTCFG_NORMAL),
                                    langGetText(STR_DLGLOBBY_BOTCFG_AGGRESSIVE),
                                    langGetText(STR_DLGLOBBY_BOTCFG_DEFENSIVE),
                                    langGetText(STR_DLGLOBBY_BOTCFG_SNIPER) };
        int pers = clientSimGetLobbyBotPersonality(cs, (BYTE)(slot));
        if (pers < 0 || pers > 3) pers = 0;
        ImGui::SetNextItemWidth(kPersComboW);
        if (ImGui::Combo("##pers", &pers, persItems, 4)) {
            lobbySendBotConfig(cs, (uint8_t)slot, (uint8_t)curMode,
                (uint8_t)curLevel, (uint8_t)pers,
                clientSimGetLobbySlot(cs, (BYTE)(slot))->playerName);
        }
        ImGui::EndGroup();
        flowPlaced();
    }

    /* ── Done group — anchored to formAnchor.y so it sits on the same
     * top Y as the form's first row, wherever the groups below it wrap
     * to. The "Hello" spacer above the button is a temporary debug label;
     * change back to a blank string once alignment is confirmed. */
    {
        const char *doneLbl = langGetText(STR_DLGLOBBY_BOTCFG_DONE);
        float doneW = ImGui::CalcTextSize(doneLbl).x
                    + fst.FramePadding.x * 2.0f;
        float padR     = 20.0f * s;
        float targetScreenX = winRightX - doneW - padR;
        ImGui::SetCursorScreenPos(ImVec2(targetScreenX, formAnchor.y));
        ImGui::BeginGroup();
        /* Invisible spacer that advances the cursor exactly the
         * same amount as the TextDisabled("Bot Code") above its
         * combo — using TextDisabled(" ") keeps the metrics
         * identical so the Done button below stays vertically
         * aligned with the combo. */
        ImGui::TextDisabled(" ");
        ImGui::SetCursorScreenPos(ImVec2(targetScreenX,
                                         ImGui::GetCursorScreenPos().y));
        if (ImGui::Button(doneLbl)) {
            s_players.expandedBotSlot = -1;
        }
        ImGui::EndGroup();
    }

    /* Restore the cursor below every row the flow produced so any
     * subsequent widgets in the AiConfig sub-row land underneath, and the
     * table row grows to hold two or three rows exactly as it already grew
     * for the wrapped difficulty description. Subtract kFormNudgeY from
     * the final Y so the 2px we shifted the contents down doesn't grow the
     * parent container. */
    float bottomY = formBottomY;
    float doneBottomY = ImGui::GetItemRectMax().y;  /* the Done group */
    if (doneBottomY > bottomY) bottomY = doneBottomY;
    ImGui::SetCursorScreenPos(
        ImVec2(formAnchor.x, bottomY - kFormNudgeY + fst.ItemSpacing.y));

    ImGui::PopID();
    ImGui::Spacing();
    ImGui::SetWindowFontScale(aicfgOldScale);
}

#if defined(WINBOLO_VOICE)
/* ── Layout A — own voice sub-row ─────────────────────────────────
 * Inline panel under the local player's row, holding the voice controls in
 * three columns: the master switch, mute, microphone gain and the input
 * level on the left; the two device pickers in the middle; the mode, the
 * push-to-talk key, the microphone test and the output volume on the right.
 * A lobby cannot open the application settings dialog, so a control that is
 * not here cannot be reached from a lobby at all. Reads and writes the same
 * voice state that dialog does, through the same helpers. */
static void renderOwnVoiceConfig(ClientSim *cs, float s, float contentW) {
    /* First, and on every frame the sub-row is open: this is what holds the
       recording device open for the frame, so the level meter has a live
       level to show and the microphone test has a device to run on. The
       voice tick takes the hold back on any frame this is not called. */
    voiceSettingsSectionDrawn();

    const ClientLobbySlot *mySlot =
        clientSimGetLobbySlot(cs, clientSimGetMyPlayerNum(cs));
    /* The microphone comes from the slot's published flags, the same
     * place the mic cell beside this row reads it from, so the row and
     * the sub-row cannot disagree about whether there is one. */
    const bool hasMic = mySlot &&
                        (mySlot->clientFlags & PLAYER_FLAG_HAS_MIC) != 0;

    /* Three columns, and three however narrow the window gets: dropping to
       two would change this row's height, and every player below it would
       move. The controls get small instead. The panel's width is passed in
       because the sub-row draws in the table's first column — the width
       available here is that column's, not the panel's. */
    const float gap = ImGui::GetStyle().ItemSpacing.x * 3.0f;
    const float avail = contentW - ImGui::GetStyle().CellPadding.x * 2.0f;
    float colW = (avail - gap * 2.0f) / 3.0f;
    if (colW < 110.0f * s) colW = 110.0f * s;
    /* A control gets its column less whatever is drawn beside it on the same
       line, and the space between the two. */
    auto ctrlW = [colW, s](float besideW) {
        float w = colW - besideW - ImGui::GetStyle().ItemSpacing.x;
        return w < 60.0f * s ? 60.0f * s : w;
    };
    auto textW = [](langid id) {
        return ImGui::CalcTextSize(langGetText(id)).x;
    };

    /* SeparatorText measures itself against the room the cell gives it, and
       in the table's first column that is narrow enough to cut the heading
       short and leave no room for the rule. Both are drawn against the width
       the controls below use instead. */
    {
        const char *head = langGetText(STR_DLGSETTINGS_VOICE);
        const ImVec2 headPos = ImGui::GetCursorScreenPos();
        ImGui::TextUnformatted(head);
        const float ruleY = headPos.y + ImGui::GetTextLineHeight() * 0.5f;
        const float ruleX = headPos.x + ImGui::CalcTextSize(head).x
                          + ImGui::GetStyle().ItemSpacing.x;
        const float ruleEnd = headPos.x + avail;
        if (ruleEnd > ruleX) {
            ImGui::GetWindowDrawList()->AddLine(
                ImVec2(ruleX, ruleY), ImVec2(ruleEnd, ruleY),
                ImGui::GetColorU32(ImGuiCol_Separator), 1.0f * s);
        }
    }

    /* Read the master switch once: the line below, the checkbox that writes
       it and the BeginDisabled / EndDisabled around the rest all have to be
       told the same answer. The rest still draws, greyed, rather than being
       left out — what the microphone is doing is worth seeing even
       when nothing is being sent. */
    bool voiceOn = voiceIsEnabled();
    /* The server dropping voice reads the same way here as the master switch
       being off: the rest is greyed rather than left out, so the row keeps its
       shape and the reason is stated instead of being left to guess at. The
       switch is named first when both apply — it is the one the player can
       do something about. */
    const bool serverOff = voiceServerHasVoiceOff();
    /* Everything but the master switch itself is greyed by these two; the
       switch stays live, or voice could never be turned back on from here. */
    const bool greyed = !voiceOn || serverOff;
    if (!voiceOn) {
        ImGui::TextDisabled("%s", langGetText(STR_DLGLOBBY_VOICE_OFF));
    } else if (serverOff) {
        ImGui::TextDisabled("%s", langGetText(STR_DLGSETTINGS_VOICE_SERVER_OFF));
    } else if (!hasMic) {
        ImGui::TextDisabled("%s", langGetText(STR_PLAYER_TIP_VOICE_SELF_NOMIC));
    }

    /* ── Column 1: the switch, mute, gain, level ────────────────── */
    ImGui::BeginGroup();
    if (ImGui::Checkbox(langGetText(STR_DLGSETTINGS_VOICE_ENABLE), &voiceOn)) {
        windowSetVoiceEnabled(voiceOn);
    }
    if (greyed) ImGui::BeginDisabled();
    {
        /* Local, and independent of the mute key: the binding may be unset,
           and this is the way to mute without one. */
        bool muted = voiceIsSelfMuted();
        if (ImGui::Checkbox(langGetText(STR_DLGKEYSETUP_MUTEMIC), &muted)) {
            voiceSetSelfMuted(muted);
        }
        imguiSettingsVoiceMicGainSlider(
            ctrlW(textW(STR_DLGSETTINGS_VOICE_MICGAIN)));
        /* The meter ends on one of two words, and the column keeps room for
           the wider of them either way, so the columns beside it do not shift
           as transmission starts and stops. */
        const float txW = textW(STR_DLGSETTINGS_VOICE_TRANSMITTING);
        const float noTxW = textW(STR_DLGSETTINGS_VOICE_NOTTRANSMITTING);
        imguiSettingsVoiceLevelMeter(
            ctrlW(textW(STR_DLGSETTINGS_VOICE_LEVEL)
                  + (txW > noTxW ? txW : noTxW)
                  + ImGui::GetStyle().ItemSpacing.x));
    }
    if (greyed) ImGui::EndDisabled();
    ImGui::EndGroup();

    /* ── Column 2: the devices ──────────────────────────────────── */
    ImGui::SameLine(0.0f, gap);
    ImGui::BeginGroup();
    if (greyed) ImGui::BeginDisabled();
    /* Both draw nothing where there is no device to choose between, and the
       column is then empty — there is nothing else that belongs in it. */
    imguiSettingsVoiceDeviceCombo(true,
        ctrlW(textW(STR_DLGSETTINGS_VOICE_MICDEVICE)));
    imguiSettingsVoiceDeviceCombo(false,
        ctrlW(textW(STR_DLGSETTINGS_VOICE_OUTDEVICE)));
    if (greyed) ImGui::EndDisabled();
    ImGui::EndGroup();

    /* ── Column 3: mode, the key, the test, output volume ───────── */
    ImGui::SameLine(0.0f, gap);
    ImGui::BeginGroup();
    if (greyed) ImGui::BeginDisabled();
    {
        imguiSettingsVoiceModeCombo(ctrlW(textW(STR_DLGSETTINGS_VOICE_MODE)));

        /* Which key push to talk is on, and the way to change it — Key Setup
           opens over the lobby, so the commonest reason voice does nothing can
           be fixed without leaving. */
        if (voiceGetMode() == VOICE_MODE_PTT) {
            keyItems pttKeys;
            windowGetKeys(&pttKeys);
            const char *pttName =
                SDL_GetScancodeName((SDL_Scancode)pttKeys.kiPushToTalk);
            if (!pttName || pttName[0] == '\0') {
                pttName = langGetText(STR_DLGKEYSETUP_NONE_VAL);
            }
            ImGui::TextUnformatted(langGetText(STR_DLGSETTINGS_VOICE_PTTKEY));
            ImGui::SameLine();
            ImGui::TextUnformatted(pttName);
            ImGui::SameLine();
            if (ImGui::SmallButton(langGetText(STR_DLGKEYSETUP_CHANGE))) {
                imguiKeySetupOpenInGame();
            }
        }

        /* The test grows a Cancel button and a word beside its bar while it
           runs, so the bar is sized for that state; the idle button stands
           on its own and does not need the room. */
        const float recW = textW(STR_DLGSETTINGS_VOICE_MICTEST_RECORDING);
        const float playW = textW(STR_DLGSETTINGS_VOICE_MICTEST_PLAYING);
        imguiSettingsVoiceMicTest(
            ctrlW(ImGui::CalcTextSize(langGetText(STR_CANCEL)).x
                  + ImGui::GetStyle().FramePadding.x * 2.0f
                  + (recW > playW ? recW : playW)
                  + ImGui::GetStyle().ItemSpacing.x));

        float vol = voiceGetOutputVolume();
        ImGui::TextUnformatted(langGetText(STR_DLGSETTINGS_VOICE_VOLUME));
        ImGui::SameLine();
        /* Room kept for the readout at its widest, so the slider does not
           resize as the value is dragged. */
        ImGui::SetNextItemWidth(
            ctrlW(textW(STR_DLGSETTINGS_VOICE_VOLUME)
                  + ImGui::CalcTextSize("0.00x").x
                  + ImGui::GetStyle().ItemSpacing.x));
        /* Empty format, and a ## id, so the slider draws neither the value
           inside itself nor a label after it; both go beside it instead. */
        if (ImGui::SliderFloat("##voicevolume", &vol, 0.0f, 2.0f, "")) {
            windowSetVoiceVolume(vol);
        }
        ImGui::SameLine();
        ImGui::Text("%.2fx", vol);
    }
    if (greyed) ImGui::EndDisabled();
    ImGui::EndGroup();
}
#endif
