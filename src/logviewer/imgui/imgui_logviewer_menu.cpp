/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          imgui_logviewer_menu.cpp
 * Purpose:       Compact-mode popup menu for the log viewer.
 *                Triggered by Start in tablet / Deck mode;
 *                mirrors the desktop menu bar with a simple
 *                button list. Closes on B / Escape.
 *********************************************************/

#include "imgui.h"
#include "imgui_logviewer_menu.h"
#include "imgui_main_menu.h"
#include "imgui_dialogs.h"
#include "imgui_clip_gif.h"
#include "../../gui/lang.h"
#include "../../gui/sdl3/dialogs/clip_gif_controls.h"  /* clipGifPushLatched */
#include <SDL3/SDL.h>

extern "C" {
    #include "logviewer.h"
    #include "../../gui/ui_mode.h"
    void lv_windowOpenFile(char *cmdLine);
    bool lv_screenSpecIsLiveMode(void);
}

static bool           s_open        = false;
static bool           s_pendingOpen = false;
static LvCompactPanel s_activePanel = LV_PANEL_EVENTS;

void lvMenuOpen(void) {
    s_pendingOpen = true;
}

bool lvMenuIsOpen(void) {
    return s_open;
}

void lvMenuPollOpenInput(void) {
    if (s_open || s_pendingOpen) return;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)) {
        lvMenuOpen();
    }
}

LvCompactPanel lvCompactGetActivePanel(void) {
    return s_activePanel;
}

void lvCompactSetActivePanel(LvCompactPanel p) {
    s_activePanel = p;
}

static void push_quit(void) {
    SDL_Event quit_event;
    SDL_zero(quit_event);
    quit_event.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quit_event);
}

void lvMenuRender(struct LogViewerState *lv) {
    if (s_pendingOpen) {
        ImGui::OpenPopup("LogMenu");
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

    /* No &s_open — modal has no title-bar X close.  Cancel is B / Escape,
       handled manually below. */
    if (ImGui::BeginPopupModal("LogMenu", NULL, flags)) {
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

        const bool isLoaded     = (lv != nullptr && lv->isLoaded);
        const bool fromMainMenu = (lv != nullptr && lv->fromMainMenu);
        /* A live game watched from the game (spectatorRun): no Comments
         * panel, no other log to open, and leaving asks first, as Esc does. */
        const bool live         = lv_screenSpecIsLiveMode();

        if (isLoaded) {
            if (ImGui::Button("View Events", btnSize)) {
                s_activePanel = LV_PANEL_EVENTS;
                ImGui::CloseCurrentPopup();
                s_open = false;
            }
            if (ImGui::Button("View Game Info", btnSize)) {
                s_activePanel = LV_PANEL_GAME_INFO;
                ImGui::CloseCurrentPopup();
                s_open = false;
            }
            if (ImGui::Button("View Item Info", btnSize)) {
                s_activePanel = LV_PANEL_ITEM_INFO;
                ImGui::CloseCurrentPopup();
                s_open = false;
            }
            if (!live && ImGui::Button("View Comments", btnSize)) {
                s_activePanel = LV_PANEL_COMMENTS;
                ImGui::CloseCurrentPopup();
                s_open = false;
            }
            ImGui::Separator();
        }

        /* Recording mode, and the GIFs made here and in the game
         * (imgui_clip_gif.h). The recording mode button stays pressed while
         * the mode is on, as the movie camera on the Controls panel does. */
        if (lv_clip_gif_available()) {
            if (!live) {
                const bool on = lv_clip_gif_mode_on();
                ImGui::BeginDisabled(!lv_clip_gif_can_toggle_mode());
                clipGifPushLatched(on);
                const bool pressed = ImGui::Button(langGetText(STR_LV_RECORD_MODE), btnSize);
                clipGifPopLatched(on);
                ImGui::EndDisabled();
                if (pressed) {
                    lv_clip_gif_toggle_mode();
                    ImGui::CloseCurrentPopup();
                    s_open = false;
                }
            }
            if (ImGui::Button(langGetText(STR_LV_WIN_CLIPS), btnSize)) {
                lv_clip_gif_toggle_gallery();
                ImGui::CloseCurrentPopup();
                s_open = false;
            }
            ImGui::Separator();
        }

        if (live) {
            if (ImGui::Button(langGetText(STR_LV_MENU_LEAVE_SPECTATING), btnSize)) {
                lv_imgui_spectator_leave_request();
                ImGui::CloseCurrentPopup();
                s_open = false;
            }
        } else if (ImGui::Button("Open log file from WinBolo.net", btnSize)) {
            lv_imgui_open_wbn_browser();
            ImGui::CloseCurrentPopup();
            s_open = false;
        }
        /* Native file dialog isn't reachable from a controller and
           doesn't exist on mobile — Deck and tablet users get the
           WBN list above as the only entry point. */
#if !defined(__ANDROID__) && !defined(__IPHONEOS__)
        if (!live && !uiModeIsSteamDeck()) {
            if (ImGui::Button("Open from file system", btnSize)) {
                lv_windowOpenFile(NULL);
                ImGui::CloseCurrentPopup();
                s_open = false;
            }
        }
#endif
        if (live) {
            /* Leave spectating, above, is the way out. */
        } else if (fromMainMenu) {
            if (ImGui::Button(langGetText(STR_LV_RETURN_MAIN_MENU), btnSize)) {
                push_quit();
                ImGui::CloseCurrentPopup();
                s_open = false;
            }
        } else {
            if (ImGui::Button(langGetText(STR_MENU_EXIT), btnSize)) {
                logViewerRequestAppQuit();
                ImGui::CloseCurrentPopup();
                s_open = false;
            }
        }

        ImGui::PopStyleVar();
        ImGui::EndPopup();
    } else {
        s_open = false;
    }
}
