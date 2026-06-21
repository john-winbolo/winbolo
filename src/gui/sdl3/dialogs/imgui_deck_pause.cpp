/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          imgui_deck_pause.cpp
 * Purpose:       Steam Deck pause overlay. A controller-driven
 *                modal popup that mirrors the desktop menu bar
 *                (which is hidden on Deck) — Resume, Settings,
 *                Configure Keys, Players, Send Message, Leave
 *                Game, Quit.  Drawn through the main ImGui
 *                context so it picks up the global gamepad-nav
 *                config (D-pad / stick + A/B).
 *********************************************************/

#include "imgui.h"
#include "imgui_deck_pause.h"
#include "../../lang.h"
#include "../sdl3imgui.h"
#include "../glyphs.h"
extern "C" {
#include "client_sim.h"
#include "../../../steam/steam_input_actions.h"
}

extern "C" void windowSetQuitting(void);
extern "C" void windowNewGame(void);

static bool s_open        = false;
static bool s_pendingOpen = false;
/* One-shot: focus Resume on the next frame after popup-open so the
   highlighted default selection is visible from frame 1. */
static bool s_focusFirst  = false;

void deckPauseOpen(void) {
    s_pendingOpen = true;
}

bool deckPauseIsOpen(void) {
    return s_open;
}

/* Pause-overlay button with optional glyph icon.  Steam Input glyph
   (Path A) takes precedence; falls back to the Xelu atlas (Path B)
   when Steam Input is unavailable, and to plain text when neither
   path resolves.  The button itself is the hit area; the glyph image
   is decoration on the same row to the left.  Width adjusted so the
   row total matches `size.x`. */
static bool pauseButton(const char *action_name,
                        const char *label,
                        ImVec2 size) {
    SDL_Texture *glyph = action_name ? glyphForActionAuto(action_name) : NULL;
    ImGui::PushID(label);
    bool clicked = false;
    if (glyph) {
        const float h = ImGui::GetFrameHeight();
        ImGui::Image((ImTextureID)glyph, ImVec2(h, h));
        ImGui::SameLine();
        const float remain = size.x - h - ImGui::GetStyle().ItemSpacing.x;
        clicked = ImGui::Button(label, ImVec2(remain, size.y));
    } else {
        clicked = ImGui::Button(label, size);
    }
    ImGui::PopID();
    return clicked;
}

void deckPauseRender(struct ClientSim *cs) {
    if (s_pendingOpen) {
        ImGui::OpenPopup("Quick menu###Pause");
        ImGui::SetNavCursorVisible(true);
        s_pendingOpen = false;
        s_open        = true;
        s_focusFirst  = true;
    }
    if (!s_open) return;

    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f,
                                   vp->Pos.y + vp->Size.y * 0.5f),
                            ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_AlwaysAutoResize;

    /* In the lobby, Players / Send Message / Leave Game don't apply —
       the lobby has its own player list + chat, and there's no game to
       leave.  Collapse to Resume / Settings / Configure Keys / Quit. */
    const bool inLobby = (cs != nullptr && clientSimIsInLobby(cs));

    /* &s_open gives the modal a title-bar X close button.  ImGui's
       NavCancel does NOT auto-close modals (imgui.cpp line ~14949 — it
       skips windows with the Modal flag), so we also detect B/Escape
       inside the popup and close it manually. */
    if (ImGui::BeginPopupModal("Quick menu###Pause", &s_open, flags)) {
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

        if (s_focusFirst) {
            ImGui::SetKeyboardFocusHere();
            s_focusFirst = false;
        }
        /* Resume gets the Pause/Start glyph — same physical button that
           opened the menu, the one the player most needs to identify. */
        if (pauseButton(SI_ACTION_PAUSE, "Resume", btnSize)) {
            ImGui::CloseCurrentPopup();
            s_open = false;
        }
        if (pauseButton(NULL, langGetText(STR_MENU_SETTINGS), btnSize)) {
            sdl3ImguiShowSettings();
            ImGui::CloseCurrentPopup();
            s_open = false;
        }
        if (pauseButton(NULL, langGetText(STR_MENU_SETKEYS), btnSize)) {
            sdl3ImguiShowKeySetup();
            ImGui::CloseCurrentPopup();
            s_open = false;
        }
        if (!inLobby) {
            if (pauseButton(NULL, langGetText(STR_MENU_PLAYERS), btnSize)) {
                sdl3ImguiShowPlayersPanel(true);
                ImGui::CloseCurrentPopup();
                s_open = false;
            }
            /* Send Message inherits the QuickChat glyph — same intent. */
            if (pauseButton(SI_ACTION_QUICK_CHAT,
                            langGetText(STR_MENU_SEND_MESSAGE), btnSize)) {
                sdl3ImguiShowSendMsg(true);
                ImGui::CloseCurrentPopup();
                s_open = false;
            }
            if (pauseButton(NULL, langGetText(STR_MENU_LEAVE_GAME), btnSize)) {
                windowNewGame();
                ImGui::CloseCurrentPopup();
                s_open = false;
            }
        }
        if (pauseButton(NULL, langGetText(STR_MENU_EXIT), btnSize)) {
            windowSetQuitting();
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
