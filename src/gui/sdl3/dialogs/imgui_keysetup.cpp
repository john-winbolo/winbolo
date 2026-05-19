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
#include "../../gamefront.h"   /* gameFrontPutPrefs — persist on OK */
#include "client_sim.h"        /* clientSim{Get,Set}Tank{AutoSlowdown,AutoHideGunsight} */
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
 * Shared form body — drawn by BOTH the standalone blocking
 * dialog (imguiKeySetupShow) AND the in-game popup wrapper
 * (imguiKeySetupRenderInGamePopup). The form layout, key-
 * capture state machine, and OK/Cancel semantics live here
 * once; each wrapper handles the surrounding context-specific
 * setup (own ImGui context vs BeginPopupModal).
 *
 * Returns:  1 = OK clicked (state has been committed)
 *          -1 = Cancel clicked / Escape pressed
 *           0 = still showing this frame
 *
 * On OK, this function commits the shared file-static state
 * to the frontend globals (useAutoslow / useAutohide), pushes
 * the typed keys via windowSetKeys, optionally pushes the
 * auto-slowdown / auto-gunsight flags onto the live tank when
 * cs != NULL (in-game path — pre-game cs is always NULL since
 * no tank exists yet), and flushes everything to INI via
 * gameFrontPutPrefs so the choice is durable immediately.
 * ------------------------------------------------------- */
static int renderFormBody(struct ClientSim *cs) {
    if (s_waiting != ksNone) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s",
                           langGetText(STR_DLGKEYSETUP_PRESS_OR_CANCEL));
        ImGui::Separator();
    }

    /* Scrollable region containing all binding rows */
    float footerH = ImGui::GetFrameHeightWithSpacing() * 3.0f +
                    ImGui::GetStyle().ItemSpacing.y * 2.0f;
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

    bool busy = (s_waiting != ksNone);
    if (busy) ImGui::BeginDisabled();

    int result = 0;
    if (ImGui::Button(langGetText(STR_OK), ImVec2(120, 0))) {
        windowSetKeys(&s_keys);
        useAutoslow = s_autoSlowdown;
        useAutohide = s_autoGunsight;
        if (cs != NULL) {
            /* In-game path: push the new flags onto the live tank so
             * the next sim tick respects them. Pre-game (cs==NULL) skips
             * this — no tank exists yet, and the next clientSimSetupSelf
             * path applies useAutoslow via frontEndApplyLocalTankPrefs. */
            clientSimSetTankAutoSlowdown(cs, s_autoSlowdown);
            clientSimSetTankAutoHideGunsight(cs, s_autoGunsight);
        }
        gameFrontPutPrefs(&s_keys);
        s_waiting = ksNone;
        result = 1;
    }
    imguiHandOnHover();
    ImGui::SameLine();
    if (ImGui::Button(langGetText(STR_CANCEL), ImVec2(120, 0))) {
        s_waiting = ksNone;
        result = -1;
    }
    imguiHandOnHover();

    if (busy) ImGui::EndDisabled();

    /* Escape / Cmd+W / Cmd+. = cancel (when not capturing a key). */
    if (!busy && (ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                  (ImGui::IsKeyPressed(ImGuiKey_W) && IMGUI_PRIMARY_KEY_DOWN())
#ifdef __APPLE__
                  || (ImGui::IsKeyPressed(ImGuiKey_Period) && ImGui::GetIO().KeySuper)
#endif
                 )) {
        result = -1;
    }

    return result;
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

        int formRc = renderFormBody(/*cs=*/nullptr);
        if (formRc == 1)  { result = 1;  running = false; }
        if (formRc == -1) { result = 0;  running = false; }

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

/* -------------------------------------------------------
 * In-game popup wrapper — same form, BeginPopupModal style
 * so it can sit inside the running game's ImGui context.
 *
 * sdl3ImguiShowKeySetup() (or the equivalent menu hook)
 * calls imguiKeySetupOpenInGame to flip the pending flag,
 * then each main-game render frame calls
 * imguiKeySetupRenderInGamePopup(cs) to draw / dismiss the
 * popup. State seeding happens once on the first render
 * frame after the trigger so we can read the live tank's
 * current values (not yet known at trigger time).
 *
 * Key capture is split across the event pump and renderForm.
 * sdl3ImguiProcessEvents intercepts SDL_EVENT_KEY_DOWN while
 * a row is in capture mode and routes it via
 * imguiKeySetupHandleInGameScancode — same pattern the
 * standalone dialog uses in its own event loop, just plumbed
 * through helper functions because we don't own the loop.
 * ------------------------------------------------------- */
static bool  s_inGameShowRequested = false;
static float s_inGameFadeAlpha     = 0.0f;

extern "C" void imguiKeySetupOpenInGame(void) {
    s_inGameShowRequested = true;
}

extern "C" void imguiKeySetupRenderInGamePopup(struct ClientSim *cs) {
    char title[128];
    snprintf(title, sizeof(title), "%s###keysetup",
             langGetText(STR_DLGKEYSETUP_TITLE));

    if (s_inGameShowRequested) {
        ImGui::OpenPopup(title);
        s_inGameShowRequested = false;
        windowGetKeys(&s_keys);
        /* Seed checkboxes from the live tank so the dialog opens
         * showing what the tank currently has — this matches the
         * old in-game popup's behavior. */
        s_autoSlowdown = cs ? clientSimGetTankAutoSlowdown(cs) : useAutoslow;
        s_autoGunsight = cs ? clientSimGetTankAutoHideGunsight(cs) : useAutohide;
        s_waiting      = ksNone;
    }

    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(420, 560), ImGuiCond_Always);

    bool open = true;
    if (!ImGui::BeginPopupModal(title, &open,
                                ImGuiWindowFlags_NoResize |
                                ImGuiWindowFlags_NoMove)) {
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                        imguiPopupFadeAlpha(&s_inGameFadeAlpha));

    int rc = renderFormBody(cs);
    if (rc != 0) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::PopStyleVar();
    ImGui::EndPopup();
}

extern "C" bool imguiKeySetupIsCapturingInGameKey(void) {
    return s_waiting != ksNone;
}

extern "C" void imguiKeySetupHandleInGameScancode(int scancode) {
    if (s_waiting == ksNone) return;
    if (scancode == SDL_SCANCODE_ESCAPE) {
        s_waiting = ksNone;
        return;
    }
    int *ptr = fieldPtr(s_waiting, &s_keys);
    if (ptr) *ptr = scancode;
    s_waiting = ksNone;
}
