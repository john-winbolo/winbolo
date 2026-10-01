/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          imgui_quickchat.cpp
 * Purpose:       Gamepad quick-chat preset menu.  Modal
 *                popup with short canned messages — sends
 *                via clientSimSendMessageAllPlayers() (the
 *                same path the free-form Send Message
 *                dialog uses).  Last entry hands off to
 *                the free-form dialog so players can type
 *                arbitrary text via the Steam OSK.
 *
 *                English literals only for V1; can be
 *                wired through lang.c later.  Lives in
 *                the main ImGui context, so it picks up
 *                the global gamepad-nav config.
 *********************************************************/

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_quickchat.h"
#include "../sdl3imgui.h"

extern "C" {
#include "client_sim.h"
}

static bool s_open        = false;
static bool s_pendingOpen = false;

void quickChatOpen(void) {
    if (s_open) return;
    s_pendingOpen = true;
}

bool quickChatIsOpen(void) {
    return s_open;
}

static void quickChatSend(struct ClientSim *cs, const char *msg) {
    if (!cs || !msg || msg[0] == '\0') return;
    /* clientSimSendMessageAllPlayers takes char* (not const) — copy to a
       local mutable buffer. PACKET_MAX_CHAT_MESSAGE caps wire payload. */
    char buf[256];
    SDL_strlcpy(buf, msg, sizeof(buf));
    clientSimSendMessageAllPlayers(cs, buf);
}

void quickChatRender(struct ClientSim *cs) {
    if (s_pendingOpen) {
        ImGui::OpenPopup("QuickChat");
        s_pendingOpen = false;
        s_open        = true;
    }
    if (!s_open) return;

    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f,
                                   vp->Pos.y + vp->Size.y * 0.5f),
                            ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_AlwaysAutoResize;

    /* &s_open gives the modal a title-bar X close button.  ImGui's
       NavCancel does NOT auto-close modals (imgui.cpp line ~14949 — it
       skips windows with the Modal flag), so we also detect B/Escape
       inside the popup and close it manually. */
    if (ImGui::BeginPopupModal("QuickChat", &s_open, flags)) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            ImGui::CloseCurrentPopup();
            s_open = false;
            ImGui::EndPopup();
            return;
        }
        const ImVec2 btnSize(320.0f, 0.0f);
        ImGuiStyle &style = ImGui::GetStyle();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(style.FramePadding.x, 8.0f));

        static const char *kPresets[] = {
            "Help!",
            "Attacking base",
            "Defending base",
            "On my way",
            "Affirmative",
            "Negative",
            "Enemy spotted",
            "Need ammo",
        };
        for (size_t i = 0; i < sizeof(kPresets) / sizeof(kPresets[0]); ++i) {
            if (ImGui::Button(kPresets[i], btnSize)) {
                quickChatSend(cs, kPresets[i]);
                ImGui::CloseCurrentPopup();
                s_open = false;
            }
        }

        ImGui::Separator();
        if (ImGui::Button("Type message...", btnSize)) {
            sdl3ImguiShowSendMsg(true);
            ImGui::CloseCurrentPopup();
            s_open = false;
        }

        ImGui::PopStyleVar();
        ImGui::EndPopup();
    } else {
        /* Popup closed externally (B button / Escape). */
        s_open = false;
    }
}
