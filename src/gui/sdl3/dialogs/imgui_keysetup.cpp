/*
 * Copyright (c) 1998-2008 John Morrison.
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
 * Name:          imgui_keysetup.cpp
 * Purpose:       Standalone blocking key setup dialog.
 *                Runs its own ImGui context and SDL event
 *                loop, same pattern as imgui_settings.cpp.
 *********************************************************/

#include <SDL3/SDL.h>

#include "imgui.h"
#include "../../imgui_theme.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui_dialog_utils.h"

extern "C" {
#include "../sdl3draw.h"
#include "global.h"
#include "../bg_game.h"
#include "../input.h"
#include "../../winbolo.h"
#include "../../lang.h"
#include "imgui_keysetup.h"

extern bool useAutoslow;
extern bool useAutohide;
}

/* -------------------------------------------------------
 * Key Setup field enum and helpers — ported from
 * sdl3imgui.cpp.  Duplicated here because the two
 * contexts (blocking dialog vs. in-game modal) have
 * different lifecycle requirements.
 * ------------------------------------------------------- */

enum KeySetupField {
    ksNone = -1,
    ksForward, ksBackward, ksTurnLeft, ksTurnRight,
    ksShoot, ksLayMine, ksGunIncrease, ksGunDecrease,
    ksTankView, ksPillView,
    ksScrollUp, ksScrollDown, ksScrollLeft, ksScrollRight,
    ksQuickTree, ksQuickRoad, ksQuickWall, ksQuickPillbox, ksQuickMine,
};

static keyItems      s_keys;
static bool          s_autoSlowdown;
static bool          s_autoGunsight;
static KeySetupField s_waiting = ksNone;

static const char *scancodeLabel(int scancode) {
    const char *name = SDL_GetScancodeName((SDL_Scancode)scancode);
    if (name && name[0] != '\0') return name;
    return langGetText(STR_DLGKEYSETUP_NONE_VAL);
}

static int *fieldPtr(KeySetupField f, keyItems *ki) {
    switch (f) {
        case ksForward:     return &ki->kiForward;
        case ksBackward:    return &ki->kiBackward;
        case ksTurnLeft:    return &ki->kiLeft;
        case ksTurnRight:   return &ki->kiRight;
        case ksShoot:       return &ki->kiShoot;
        case ksLayMine:     return &ki->kiLayMine;
        case ksGunIncrease: return &ki->kiGunIncrease;
        case ksGunDecrease: return &ki->kiGunDecrease;
        case ksTankView:    return &ki->kiTankView;
        case ksPillView:    return &ki->kiPillView;
        case ksScrollUp:    return &ki->kiScrollUp;
        case ksScrollDown:  return &ki->kiScrollDown;
        case ksScrollLeft:  return &ki->kiScrollLeft;
        case ksScrollRight: return &ki->kiScrollRight;
        case ksQuickTree:   return &ki->kiQuickTree;
        case ksQuickRoad:   return &ki->kiQuickRoad;
        case ksQuickWall:   return &ki->kiQuickWall;
        case ksQuickPillbox:return &ki->kiQuickPillbox;
        case ksQuickMine:   return &ki->kiQuickMine;
        default:            return nullptr;
    }
}

static void keyRow(const char *label, KeySetupField field) {
    int *ptr = fieldPtr(field, &s_keys);
    if (!ptr) return;

    bool waiting = (s_waiting == field);

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(label);

    ImGui::TableSetColumnIndex(1);
    if (waiting) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s",
                           langGetText(STR_DLGKEYSETUP_PRESSAKEY));
    } else {
        ImGui::TextUnformatted(scancodeLabel(*ptr));
    }

    ImGui::TableSetColumnIndex(2);
    ImGui::PushID((int)field);
    if (waiting) {
        if (ImGui::SmallButton(langGetText(STR_CANCEL))) {
            s_waiting = ksNone;
        }
        imguiHandOnHover();
    } else {
        if (ImGui::SmallButton(langGetText(STR_DLGKEYSETUP_CHANGE))) {
            s_waiting = field;
        }
        imguiHandOnHover();
    }
    ImGui::PopID();
}

/* -------------------------------------------------------
 * Main blocking dialog
 * ------------------------------------------------------- */

extern "C" int imguiKeySetupShow(void) {
    SDL_Window *window = sdl3DrawGetWindow();
    SDL_Renderer *renderer = sdl3DrawGetRenderer();
    if (!window || !renderer) return 0;

    /* Save logical presentation */
    int savedLogW = 0, savedLogH = 0;
    SDL_RendererLogicalPresentation savedLogMode = SDL_LOGICAL_PRESENTATION_DISABLED;
    dialogSaveLogicalPresentation(renderer, &savedLogW, &savedLogH, &savedLogMode);

    int screenW, screenH;
    SDL_GetWindowSize(window, &screenW, &screenH);
    if (screenW <= 0 || screenH <= 0) { screenW = 1024; screenH = 768; }
    float s = dialogComputeScale(screenW, screenH);

#if !BOLO_MOBILE
    dialogSetWindowSize(window, 1024, 768);
    dialogSetWindowTitle(window, langGetText(STR_DLGKEYSETUP_WINTITLE));
    SDL_SetWindowResizable(window, true);
#endif
    dialogRestorePosition(window);
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    imguiRegisterPlatformOpenUrl();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    imguiApplyBoloTheme();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    dialogApplyScaling(s);

    /* Load current bindings */
    windowGetKeys(&s_keys);
    s_autoSlowdown = useAutoslow;
    s_autoGunsight = useAutohide;
    s_waiting = ksNone;

    /* Background game */
    BgGame *bg = bgGameGetShared();
    bool hasBg = (bg != nullptr);
    Uint64 lastTickTime = SDL_GetTicks();

    bool running = true;
    int result = 0;

    while (running) {
        Uint64 frameCapStart = dialogFrameCapBegin();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            /* Key capture — intercept before ImGui sees it */
            if (s_waiting != ksNone && ev.type == SDL_EVENT_KEY_DOWN &&
                ev.key.windowID == SDL_GetWindowID(window)) {
                SDL_Scancode sc = ev.key.scancode;
                if (sc == SDL_SCANCODE_ESCAPE) {
                    s_waiting = ksNone;
                } else {
                    int *ptr = fieldPtr(s_waiting, &s_keys);
                    if (ptr) *ptr = (int)sc;
                    s_waiting = ksNone;
                }
                continue;
            }

            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (dialogHandleDevicePresetEvent(window, &ev)) continue;
            dialogHandleWindowMoveResize(window, &ev);
            if (ev.type == SDL_EVENT_QUIT ||
                (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                 ev.window.windowID == SDL_GetWindowID(window))) {
                running = false;
            }
        }

        /* Tick the background game */
        if (hasBg && !bg->paused) {
            bgGameTickFixed(bg, &lastTickTime);
        } else if (hasBg && bg->paused) {
            lastTickTime = SDL_GetTicks();
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        dialogResetTextInputArea(window);
        dialogOverrideFramebufferScale(renderer);
        ImGui::NewFrame();

        int winW, winH;
        SDL_GetWindowSize(window, &winW, &winH);

        /* Transparent full-screen host window */
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)winW, (float)winH));
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::Begin("##KeySetupBg", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        /* Centered overlay panel */
        float panelW = 460.0f * s, panelH = 560.0f * s;
        if (panelW > (float)winW * 0.95f) panelW = (float)winW * 0.95f;
        if (panelH > (float)winH * 0.95f) panelH = (float)winH * 0.95f;

        ImGui::SetNextWindowPos(ImVec2(((float)winW - panelW) * 0.5f, ((float)winH - panelH) * 0.5f));
        ImGui::SetNextWindowSize(ImVec2(panelW, panelH));
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGui::Begin("##KeySetupPanel", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse);

        /* Title */
        {
            ImGui::SetWindowFontScale(1.4f);
            const char *title = langGetText(STR_DLGKEYSETUP_TITLE);
            ImVec2 textSize = ImGui::CalcTextSize(title);
            ImGui::SetCursorPosX((panelW - textSize.x) * 0.5f);
            ImGui::Text("%s", title);
            ImGui::SetWindowFontScale(1.0f);
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (s_waiting != ksNone) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s",
                               langGetText(STR_DLGKEYSETUP_PRESS_OR_CANCEL));
            ImGui::Separator();
        }

        /* Scrollable region containing all binding rows */
        float footerH = ImGui::GetFrameHeightWithSpacing() * 3.0f + ImGui::GetStyle().ItemSpacing.y * 2.0f;
        ImGui::BeginChild("##bindings", ImVec2(0.0f, -footerH), false);

        constexpr ImGuiTableFlags tflags =
            ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit |
            ImGuiTableFlags_RowBg;

        auto section = [&](const char *sectionTitle) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "%s", sectionTitle);
            ImGui::BeginTable(sectionTitle, 3, tflags, ImVec2(-1, 0));
            ImGui::TableSetupColumn(langGetText(STR_DLGKEYSETUP_COL_ACTION),
                                    ImGuiTableColumnFlags_WidthFixed, 140.0f);
            ImGui::TableSetupColumn(langGetText(STR_DLGKEYSETUP_COL_KEY),
                                    ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("",        ImGuiTableColumnFlags_WidthFixed,  68.0f);
        };
        auto endSection = [&]() { ImGui::EndTable(); };

        section(langGetText(STR_DLGKEYSETUP_DRIVETANK));
        keyRow(langGetText(STR_DLGKEYSETUP_FASTER),    ksForward);
        keyRow(langGetText(STR_DLGKEYSETUP_SLOWER),    ksBackward);
        keyRow(langGetText(STR_DLGKEYSETUP_TURNLEFT),  ksTurnLeft);
        keyRow(langGetText(STR_DLGKEYSETUP_TURNRIGHT), ksTurnRight);
        endSection();

        section(langGetText(STR_DLGKEYSETUP_WEAPONS));
        keyRow(langGetText(STR_DLGKEYSETUP_SHOOT),    ksShoot);
        keyRow(langGetText(STR_DLGKEYSETUP_LAYMINE),  ksLayMine);
        endSection();

        section(langGetText(STR_DLGKEYSETUP_GUNRANGE));
        keyRow(langGetText(STR_DLGKEYSETUP_INCREASE), ksGunIncrease);
        keyRow(langGetText(STR_DLGKEYSETUP_DECREASE), ksGunDecrease);
        endSection();

        section(langGetText(STR_DLGKEYSETUP_VIEW));
        keyRow(langGetText(STR_DLGKEYSETUP_TANKVIEW), ksTankView);
        keyRow(langGetText(STR_DLGKEYSETUP_PILLVIEW), ksPillView);
        endSection();

        section(langGetText(STR_DLGKEYSETUP_SCROLL));
        keyRow(langGetText(STR_DLGKEYSETUP_SCROLLUP),    ksScrollUp);
        keyRow(langGetText(STR_DLGKEYSETUP_SCROLLDOWN),  ksScrollDown);
        keyRow(langGetText(STR_DLGKEYSETUP_SCROLLLEFT),  ksScrollLeft);
        keyRow(langGetText(STR_DLGKEYSETUP_SCROLLRIGHT), ksScrollRight);
        endSection();

        section(langGetText(STR_DLGKEYSETUP_QUICKKEYS));
        keyRow(langGetText(STR_DLGKEYSETUP_TREE),         ksQuickTree);
        keyRow(langGetText(STR_DLGKEYSETUP_ROAD),         ksQuickRoad);
        keyRow(langGetText(STR_DLGKEYSETUP_WALL),         ksQuickWall);
        keyRow(langGetText(STR_DLGKEYSETUP_QUICKPILLBOX), ksQuickPillbox);
        keyRow(langGetText(STR_DLGKEYSETUP_QUICKMINE),    ksQuickMine);
        endSection();

        ImGui::EndChild();

        ImGui::Separator();
        ImGui::Checkbox(langGetText(STR_DLGKEYSETUP_AUTOSLOWDOWN), &s_autoSlowdown);
        ImGui::SameLine();
        ImGui::Checkbox(langGetText(STR_DLGKEYSETUP_AUTOGUNSIGHT), &s_autoGunsight);
        ImGui::Spacing();

        /* OK / Cancel — disabled while a key-capture is pending */
        bool busy = (s_waiting != ksNone);
        if (busy) ImGui::BeginDisabled();

        if (ImGui::Button(langGetText(STR_OK), ImVec2(120, 0))) {
            windowSetKeys(&s_keys);
            useAutoslow = s_autoSlowdown;
            useAutohide = s_autoGunsight;
            s_waiting = ksNone;
            result = 1;
            running = false;
        }
        imguiHandOnHover();
        ImGui::SameLine();
        if (ImGui::Button(langGetText(STR_CANCEL), ImVec2(120, 0))) {
            s_waiting = ksNone;
            result = 0;
            running = false;
        }
        imguiHandOnHover();

        if (busy) ImGui::EndDisabled();

        /* Also allow Escape to cancel (when not capturing a key) */
        if (!busy && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            result = 0;
            running = false;
        }

        ImGui::End(); /* ##KeySetupPanel */
        ImGui::End(); /* ##KeySetupBg */

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);

        /* Render background game with overlay */
        if (hasBg) {
            bgGameRender(bg, renderer, winW, winH);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 140);
            SDL_FRect overlayRect = { 0, 0, (float)winW, (float)winH };
            SDL_RenderFillRect(renderer, &overlayRect);
        }

        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
        dialogFrameCapEnd(frameCapStart);
    }

    dialogDismissKeyboard(window);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    dialogRestoreLogicalPresentation(renderer, savedLogW, savedLogH, savedLogMode);

#if !BOLO_MOBILE
    SDL_SetWindowResizable(window, true);
#endif

    SDL_FlushEvent(SDL_EVENT_QUIT);

    return result;
}
