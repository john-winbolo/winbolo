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
 * Name:          lobby_status.cpp
 * Purpose:       The lobby's connectivity badge and reject
 *                toast. The badge is the reachability icon
 *                and its short status text on the status
 *                bar, plus the detail modal behind it that
 *                carries the port-test button and the
 *                result of the last manual probe. The toast
 *                is the one-line pill that surfaces the
 *                server's rejection of a lobby command and
 *                the X that clears it.
 *********************************************************/

#include <SDL3/SDL.h>

#include "imgui.h"
#include "lobby_internal.h"
#include "dialog_footer.h"      /* WBUI::DialogFooter — the detail modal's Close */
#include "../../wb_theme.h"     /* g_theme / wbThemeColor — the toast's pill colour */
extern "C" {
#include "client_sim.h"
#include "../../../lang.h"
#include "../../../../server/server_lifecycle.h"  /* portmap info + manual probe */
}

/* ── Layout A — connectivity badge (icon + short text + Test btn) ─
 * Self-contained block extracted from the inline lobby chrome so it
 * can be rendered at top-right of the status bar (Layout A
 * placement) instead of below the settings line.
 *
 * Renders nothing when port-mapping status is DISABLED. Owns its own
 * popup modal for the detail view; clicking the icon/text opens it. */
void lobbyRenderConnectivityBadge(SDL_Renderer *renderer, float s) {
    ServerPortmapInfo pm;
    serverInstanceGetPortmapInfo(&pm);

    lobbyLoadStatusIconsOnce(renderer, s);

    SDL_Texture *icon       = nullptr;
    const char  *shortText  = nullptr;
    ImVec4       color;
    const char  *detailFmt  = nullptr;

    switch (pm.status) {
        case SERVER_PORTMAP_DISABLED:
            return;  /* nothing to show */
        case SERVER_PORTMAP_PENDING:
            icon      = lobbyIcons()->info;
            shortText = langGetText(STR_DLGLOBBY_PORTMAP_CHECKING);
            color     = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
            detailFmt = langGetText(STR_DLGLOBBY_PORTMAP_DETAIL_PENDING);
            break;
        case SERVER_PORTMAP_SUCCEEDED:
            icon      = lobbyIcons()->success;
            shortText = langGetText(STR_DLGLOBBY_PORTMAP_ACCESSIBLE);
            color     = ImVec4(0.4f, 0.8f, 0.4f, 1.0f);
            detailFmt = langGetText(STR_DLGLOBBY_PORTMAP_DETAIL_SUCCEEDED);
            break;
        case SERVER_PORTMAP_HOLE_PUNCH_OK:
            icon      = lobbyIcons()->success;
            shortText = langGetText(STR_DLGLOBBY_PORTMAP_ACCESSIBLE);
            color     = ImVec4(0.4f, 0.8f, 0.4f, 1.0f);
            detailFmt = langGetText(STR_DLGLOBBY_PORTMAP_DETAIL_HOLE_PUNCH);
            break;
        case SERVER_PORTMAP_SYMMETRIC_NAT:
            icon      = lobbyIcons()->error;
            shortText = langGetText(STR_DLGLOBBY_PORTMAP_UNREACHABLE);
            color     = ImVec4(0.9f, 0.4f, 0.3f, 1.0f);
            detailFmt = langGetText(STR_DLGLOBBY_PORTMAP_DETAIL_SYMMETRIC);
            break;
        case SERVER_PORTMAP_FAILED:
            icon      = lobbyIcons()->error;
            shortText = langGetText(STR_DLGLOBBY_PORTMAP_UNREACHABLE);
            color     = ImVec4(0.9f, 0.4f, 0.3f, 1.0f);
            detailFmt = langGetText(STR_DLGLOBBY_PORTMAP_DETAIL_FAILED);
            break;
    }

    if (!shortText) return;

    /* Right-align the badge to the available width so all states
     * ("Checking server reachability...", "Server accessible",
     * "Server unreachable") share the same right edge regardless of
     * text length. The caller still SameLine()s us onto the status
     * row; we just absorb the slack on our left. */
    float iconSize    = icon ? 18.0f * s : 0.0f;
    float spacingX    = icon ? ImGui::GetStyle().ItemSpacing.x : 0.0f;
    float textW       = ImGui::CalcTextSize(shortText).x;
    float badgeWidth  = iconSize + spacingX + textW;
    float availW      = ImGui::GetContentRegionAvail().x;
    if (availW > badgeWidth) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availW - badgeWidth));
    }

    ImGui::BeginGroup();
    if (icon) {
        ImGui::Image((ImTextureID)icon, ImVec2(iconSize, iconSize));
        ImGui::SameLine();
        float textOffset = (iconSize - ImGui::GetTextLineHeight()) * 0.5f;
        if (textOffset > 0.0f) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + textOffset);
        }
    }
    ImGui::TextColored(color, "%s", shortText);
    ImGui::EndGroup();

    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    char popupTitle[128];
    SDL_snprintf(popupTitle, sizeof(popupTitle), "%s##portmap_details",
                 langGetText(STR_DLGLOBBY_PORTMAP_POPUP_TITLE));

    if (ImGui::IsItemClicked()) {
        ImGui::OpenPopup(popupTitle);
    }

    static bool s_pmOpen = true; s_pmOpen = true;
    if (ImGui::BeginPopupModal(popupTitle, &s_pmOpen,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ServerPortmapInfo pm2;
        serverInstanceGetPortmapInfo(&pm2);
        ImGui::PushTextWrapPos(420.0f * s);
        switch (pm2.status) {
            case SERVER_PORTMAP_SUCCEEDED: {
                MessageArgs args = {};
                SDL_strlcpy(args.string1, pm2.externalIp, sizeof(args.string1));
                args.number = (int)(unsigned)pm2.externalPort;
                ImGui::Text("%s",
                    langGetTextFmt(STR_DLGLOBBY_PORTMAP_DETAIL_SUCCEEDED, &args));
                break;
            }
            case SERVER_PORTMAP_SYMMETRIC_NAT:
            case SERVER_PORTMAP_FAILED: {
                MessageArgs args = {};
                args.number = (int)(unsigned)(pm2.internalPort != 0
                                              ? pm2.internalPort : 27500);
                ImGui::Text("%s",
                    langGetTextFmt(pm2.status == SERVER_PORTMAP_SYMMETRIC_NAT
                                       ? STR_DLGLOBBY_PORTMAP_DETAIL_SYMMETRIC
                                       : STR_DLGLOBBY_PORTMAP_DETAIL_FAILED,
                                   &args));
                break;
            }
            default:
                ImGui::TextUnformatted(detailFmt);
                break;
        }
        ImGui::PopTextWrapPos();
        ImGui::Spacing();

        /* Live re-probe button + result lives inside the dialog now —
         * keeps the lobby header tidy and groups the test with the
         * status detail that describes what's being tested. */
        ManualProbeState mps = serverInstanceGetManualProbeState();
        /* Disable while the manual probe is in flight too — prevents the
         * user re-triggering a probe that hasn't reported back yet. */
        bool canTest = (pm2.status != SERVER_PORTMAP_DISABLED &&
                        pm2.status != SERVER_PORTMAP_PENDING &&
                        mps != MANUAL_PROBE_IN_PROGRESS);
        if (!canTest) ImGui::BeginDisabled();
        if (ImGui::Button(langGetText(STR_DLGLOBBY_TEST_CONNECTIVITY))) {
            serverInstanceTriggerManualProbe();
        }
        if (!canTest) ImGui::EndDisabled();
        if (!canTest &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_CONN_TEST_TIP));
        }
        if (mps != MANUAL_PROBE_IDLE) {
            ImGui::SameLine();
            switch (mps) {
                case MANUAL_PROBE_IN_PROGRESS:
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                                       "%s", langGetText(STR_DLGLOBBY_TEST_TESTING));
                    break;
                case MANUAL_PROBE_SUCCESS:
                    ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f),
                                       "%s", langGetText(STR_DLGLOBBY_TEST_REACHABLE));
                    break;
                case MANUAL_PROBE_TIMEOUT:
                    ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.3f, 1.0f),
                                       "%s", langGetText(STR_DLGLOBBY_TEST_NO_REPLY));
                    break;
                case MANUAL_PROBE_IDLE:
                    break;
            }
        }
        /* [Close] only — affirmative "I'm done viewing", not cancel.
         * Use confirm slot so it gets default primary styling. */
        int closeFooter = WBUI::DialogFooter(/*cancelLabel*/ nullptr,
                                             /*confirmLabel*/ langGetText(STR_CLOSE));
        if (closeFooter != WBUI::FOOTER_NONE) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

/* ── Layout A — server reject toast ───────────────────────────────
 * Surfaces the last CTRL_COMMAND_REJECTED (delivered via
 * clientSimApplyControl) as a one-line orange status pill. Auto-clears
 * after the user dismisses it (clicks the X) so subsequent rejects
 * re-trigger naturally. */
void lobbyRenderRejectToast(ClientSim *cs, float s) {
    if (clientSimGetLobbyLastRejectPacket(cs) == 0) return;
    const char *reason = langGetText(STR_DLGLOBBY_REJECT_DEFAULT);
    switch (clientSimGetLobbyLastRejectReason(cs)) {
        case  1: reason = langGetText(STR_DLGLOBBY_REJECT_NOTHOST);        break;  /* LOBBY_REJECT_NOT_HOST */
        case  2: reason = langGetText(STR_DLGLOBBY_REJECT_LOCKED);         break;  /* LOBBY_REJECT_LOCKED */
        case  3: reason = langGetText(STR_DLGLOBBY_REJECT_INVALID);        break;  /* LOBBY_REJECT_INVALID */
        case  7: reason = langGetText(STR_DLGLOBBY_REJECT_COOLDOWN);       break;  /* CMD_REJECT_COOLDOWN */
        case  8: reason = langGetText(STR_DLGLOBBY_REJECT_BAD_STATE);      break;  /* CMD_REJECT_BAD_STATE */
        case  9: reason = langGetText(STR_NAME_INVALID_EMPTY);             break;  /* CMD_REJECT_NAME_EMPTY */
        case 10: reason = langGetText(STR_NAME_INVALID_RESERVED_PREFIX);   break;  /* CMD_REJECT_NAME_RESERVED_PREFIX */
        case 11: reason = langGetText(STR_NAME_INVALID_RESERVED_SUFFIX);   break;  /* CMD_REJECT_NAME_RESERVED_SUFFIX */
        case 12: reason = langGetText(STR_NAME_INVALID_MIXED_SCRIPTS);     break;  /* CMD_REJECT_NAME_MIXED_SCRIPTS */
        case 13: reason = langGetText(STR_NAME_INVALID_CHARS);             break;  /* CMD_REJECT_NAME_INVALID */
        case 14: reason = langGetText(STR_DLGSETNAME_INUSE_ERR);           break;  /* CMD_REJECT_NAME_TAKEN */
        case 15: reason = langGetText(STR_DLGLOBBY_REJECT_BOT_LIMIT);      break;  /* CMD_REJECT_BOT_LIMIT */
        case 16: reason = langGetText(STR_DLGLOBBY_REJECT_SCENARIO);       break;  /* CMD_REJECT_SCENARIO */
        default: break;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, wbThemeColor(g_theme->lockBadge));
    {
        MessageArgs args = {};
        SDL_strlcpy(args.string1, reason, sizeof(args.string1));
        ImGui::Text("%s", langGetTextFmt(STR_DLGLOBBY_REJECT_FMT, &args));
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::SmallButton("X##rejdismiss")) {
        clientSimClearLobbyLastReject(cs);
    }
    (void)s;
}
