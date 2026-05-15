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
 * Name:          imgui_messagebox.cpp
 * Purpose:       Blocking ImGui message box with icon and
 *                configurable buttons (OK / Yes-No /
 *                Yes-No-Cancel).  Icons are loaded from
 *                data/ui/dialog-{info,warning,error}.svg.
 *
 *                Renders as a centred modal popup on top of
 *                the current window content (captured as a
 *                background texture with a dark overlay).
 *                Creates a dedicated ImGui context so it
 *                can be called from anywhere — game loop,
 *                lobby, or other dialogs.
 *********************************************************/

#include <cstring>

#include <SDL3/SDL.h>

#include "imgui.h"
#include "../../imgui_theme.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui_dialog_utils.h"
#include "nanosvg.h"
#include "nanosvgrast.h"

extern "C" {
#include "../sdl3draw.h"
#include "../../lang.h"
#include "imgui_messagebox.h"
}

static const int ICON_SIZE = 48;
static const float DIALOG_WIDTH_FRACTION = 0.5f;
static const float DIALOG_MIN_W = 420.0f;
static const float DIALOG_MAX_W = 700.0f;

static const char *iconPathForType(ImguiMsgType type) {
    switch (type) {
        case IMGUI_MSG_WARNING: return "data/ui/dialog-warning.svg";
        case IMGUI_MSG_ERROR:   return "data/ui/dialog-error.svg";
        default:                return "data/ui/dialog-info.svg";
    }
}

/* Capture the current backbuffer as a texture for use as a background.
 * Returns NULL if capture fails (caller should fall back to solid bg). */
static SDL_Texture *captureBackbuffer(SDL_Renderer *renderer) {
    SDL_Surface *surface = SDL_RenderReadPixels(renderer, NULL);
    if (!surface) return nullptr;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    return tex;
}

/* Render the message box content (icon, text, buttons).
 * Returns -1 while open, or a IMGUI_MSG_RESULT_* value when a button is pressed. */
static int renderMessageBoxContent(const char *message, ImguiMsgButtons buttons,
                                   SDL_Texture *iconTex, float scale, bool *focusBtn) {
    int result = -1;

    float iconDisplaySize = ICON_SIZE * scale;
    if (iconTex) {
        ImGui::Image((ImTextureID)iconTex,
                     ImVec2(iconDisplaySize, iconDisplaySize));
        ImGui::SameLine();
    }

    /* Vertically centre text beside icon */
    float textStartY = ImGui::GetCursorPosY();
    float iconMidY = textStartY + (iconDisplaySize * 0.5f) -
                     (ImGui::GetTextLineHeight() * 0.5f);
    if (iconTex && iconMidY > textStartY) {
        ImGui::SetCursorPosY(iconMidY);
    }

    float textRegionW = ImGui::GetContentRegionAvail().x;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + textRegionW);
    imguiTextWrappedWithLinks(message ? message : "");
    ImGui::PopTextWrapPos();

    /* Push cursor below the icon if the text was shorter */
    float afterTextY = ImGui::GetCursorPosY();
    float afterIconY = textStartY + iconDisplaySize +
                       ImGui::GetStyle().ItemSpacing.y;
    if (afterIconY > afterTextY) {
        ImGui::SetCursorPosY(afterIconY);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    /* Buttons */
    float btnW = 80.0f;
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float availW = ImGui::GetContentRegionAvail().x;

    if (buttons == IMGUI_MSG_OK) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availW - btnW) / 2.0f);
        if (*focusBtn) { ImGui::SetKeyboardFocusHere(); *focusBtn = false; }
        if (ImGui::Button(langGetText(STR_OK), ImVec2(btnW, 0))) {
            result = IMGUI_MSG_RESULT_OK;
        }
        imguiHandOnHover();
    } else if (buttons == IMGUI_MSG_YES_NO) {
        float totalW = btnW * 2 + spacing;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availW - totalW) / 2.0f);
        if (*focusBtn) { ImGui::SetKeyboardFocusHere(); *focusBtn = false; }
        if (ImGui::Button(langGetText(STR_YES), ImVec2(btnW, 0))) {
            result = IMGUI_MSG_RESULT_YES;
        }
        imguiHandOnHover();
        ImGui::SameLine();
        if (ImGui::Button(langGetText(STR_NO), ImVec2(btnW, 0))) {
            result = IMGUI_MSG_RESULT_NO;
        }
        imguiHandOnHover();
    } else { /* IMGUI_MSG_YES_NO_CANCEL */
        float totalW = btnW * 3 + spacing * 2;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availW - totalW) / 2.0f);
        if (*focusBtn) { ImGui::SetKeyboardFocusHere(); *focusBtn = false; }
        if (ImGui::Button(langGetText(STR_YES), ImVec2(btnW, 0))) {
            result = IMGUI_MSG_RESULT_YES;
        }
        imguiHandOnHover();
        ImGui::SameLine();
        if (ImGui::Button(langGetText(STR_NO), ImVec2(btnW, 0))) {
            result = IMGUI_MSG_RESULT_NO;
        }
        imguiHandOnHover();
        ImGui::SameLine();
        if (ImGui::Button(langGetText(STR_CANCEL), ImVec2(btnW, 0))) {
            result = IMGUI_MSG_RESULT_CANCEL;
        }
        imguiHandOnHover();
    }

    return result;
}

/* Guard against re-entrant calls (e.g. lobby + game loop both detecting
 * the same disconnect). */
static bool s_messageBoxActive = false;

extern "C" int imguiMessageBoxEx(const char *title, const char *message,
                                  ImguiMsgType type, ImguiMsgButtons buttons) {
    SDL_Window *window = sdl3DrawGetWindow();
    SDL_Renderer *renderer = sdl3DrawGetRenderer();
    if (!window || !renderer) return IMGUI_MSG_RESULT_OK;

    /* Prevent double-showing from overlapping detection paths. */
    if (s_messageBoxActive) return IMGUI_MSG_RESULT_OK;
    s_messageBoxActive = true;

    /* Save caller's ImGui context (may be NULL). */
    ImGuiContext *callerCtx = ImGui::GetCurrentContext();

    /* Save logical presentation (Android sets one for the game view) */
    int savedLogW = 0, savedLogH = 0;
    SDL_RendererLogicalPresentation savedLogMode = SDL_LOGICAL_PRESENTATION_DISABLED;
    dialogSaveLogicalPresentation(renderer, &savedLogW, &savedLogH, &savedLogMode);

    int screenW, screenH;
    SDL_GetWindowSize(window, &screenW, &screenH);
    if (screenW <= 0 || screenH <= 0) { screenW = 1024; screenH = 768; }
    float s = dialogComputeScale(screenW, screenH);

    /* Capture the current backbuffer so we can show it as the background
     * behind the dimmed modal overlay. */
    SDL_Texture *bgTex = captureBackbuffer(renderer);

    /* Create a dedicated ImGui context for the messagebox.
     * This avoids conflicts with any caller context that may be mid-frame. */
    ImGuiContext *msgCtx = ImGui::CreateContext();
    ImGui::SetCurrentContext(msgCtx);
    imguiRegisterPlatformOpenUrl();

    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    imguiApplyBoloTheme();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    dialogApplyScaling(s);

    SDL_Texture *iconTex = imguiLoadSvgIcon(renderer, iconPathForType(type), ICON_SIZE);

    /* Compute dialog width: 50% of window, clamped to [420, 700] */
    float dialogW = (float)screenW * DIALOG_WIDTH_FRACTION;
    if (dialogW < DIALOG_MIN_W) dialogW = DIALOG_MIN_W;
    if (dialogW > DIALOG_MAX_W) dialogW = DIALOG_MAX_W;
    dialogW *= s;

    int result = -1;
    bool focusBtn = true;
    bool openedPopup = false;
    const char *popupId = title ? title : "Message";

    while (result < 0) {
        Uint64 frameCapStart = dialogFrameCapBegin();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (ev.type == SDL_EVENT_QUIT ||
                (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                 ev.window.windowID == SDL_GetWindowID(window))) {
                result = (buttons == IMGUI_MSG_YES_NO_CANCEL) ?
                         IMGUI_MSG_RESULT_CANCEL : IMGUI_MSG_RESULT_OK;
            } else if (ev.type == SDL_EVENT_KEY_DOWN) {
                SDL_Keycode k = ev.key.key;
                /* Enter activates the default (affirmative) button. */
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    result = (buttons == IMGUI_MSG_OK)
                             ? IMGUI_MSG_RESULT_OK
                             : IMGUI_MSG_RESULT_YES;
                /* Escape dismisses: OK on single-button dialogs, and
                 * the "negative" button on multi-button dialogs. */
                } else if (k == SDLK_ESCAPE) {
                    if (buttons == IMGUI_MSG_OK)
                        result = IMGUI_MSG_RESULT_OK;
                    else if (buttons == IMGUI_MSG_YES_NO)
                        result = IMGUI_MSG_RESULT_NO;
                    else
                        result = IMGUI_MSG_RESULT_CANCEL;
                }
            }
        }

        /* Draw background: captured backbuffer + dark overlay */
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);
        if (bgTex) {
            SDL_RenderTexture(renderer, bgTex, NULL, NULL);
            /* Semi-transparent dark overlay to dim the background */
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 140);
            SDL_FRect dimRect = { 0, 0, (float)screenW, (float)screenH };
            SDL_RenderFillRect(renderer, &dimRect);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        dialogOverrideFramebufferScale(renderer);
        ImGui::NewFrame();

        /* Full-screen invisible host window for the modal. */
        int winW, winH;
        SDL_GetWindowSize(window, &winW, &winH);
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)winW, (float)winH));
        ImGui::Begin("##MsgBoxHost", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoInputs);

        if (!openedPopup) {
            ImGui::OpenPopup(popupId);
            openedPopup = true;
        }

        ImGui::SetNextWindowSizeConstraints(ImVec2(dialogW, 0),
                                            ImVec2(dialogW, FLT_MAX));
        static float s_fadeMsgBox = 0.0f;
        if (ImGui::BeginPopupModal(popupId, nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoMove)) {
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                                imguiPopupFadeAlpha(&s_fadeMsgBox));
            /* Centre the modal in the window */
            ImVec2 modalSize = ImGui::GetWindowSize();
            ImGui::SetWindowPos(
                ImVec2(((float)winW - modalSize.x) * 0.5f,
                       ((float)winH - modalSize.y) * 0.5f));

            int r = renderMessageBoxContent(message, buttons, iconTex,
                                            s, &focusBtn);
            if (r >= 0) {
                result = r;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleVar();
            ImGui::EndPopup();
        }

        ImGui::End(); /* ##MsgBoxHost */

        ImGui::Render();
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
        dialogFrameCapEnd(frameCapStart);
    }

    if (iconTex) SDL_DestroyTexture(iconTex);

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(msgCtx);

    /* Leave the backbuffer holding the undimmed snapshot we captured
     * when we opened. Without this, a subsequent back-to-back modal
     * would capture our dim+dialog framebuffer, dim it again, and the
     * dialogs would progressively darken (e.g. the 4-page tutorial
     * intro would fade to grey by page four). Draw without presenting
     * so the on-screen display keeps the last dialog frame until the
     * next dialog or render pass replaces it. */
    if (bgTex) {
        SDL_RenderTexture(renderer, bgTex, NULL, NULL);
        SDL_DestroyTexture(bgTex);
    }

    /* Restore logical presentation */
    dialogRestoreLogicalPresentation(renderer, savedLogW, savedLogH, savedLogMode);

    /* Restore the caller's context (may be NULL if none existed). */
    ImGui::SetCurrentContext(callerCtx);

    SDL_FlushEvent(SDL_EVENT_QUIT);
    s_messageBoxActive = false;
    return result;
}

extern "C" void imguiMessageBox(const char *message, const char *title) {
    imguiMessageBoxEx(title, message, IMGUI_MSG_INFO, IMGUI_MSG_OK);
}
