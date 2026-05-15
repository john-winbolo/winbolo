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
 * Name:          imgui_skins.cpp
 * Purpose:       ImGui Skins selection dialog.
 *                ImGui skins dialog.
 *********************************************************/

#include <cstring>
#include <cstdio>
#include <vector>
#include <string>

#include <SDL3/SDL.h>

#include "imgui.h"
#include "../../imgui_theme.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui_dialog_utils.h"

extern "C" {
#include "../sdl3draw.h"
#include "../../gamefront.h"
#include "../../skins.h"
#include "../../lang.h"
#include "imgui_skins.h"
}

static const int DIALOG_W = 420;
static const int DIALOG_H = 400;


/* Read a value from a simple ini file: [Skin] section, given key.
 * Returns empty string if not found. */
static std::string readSkinIni(const char *iniPath, const char *key) {
    FILE *f = fopen(iniPath, "r");
    if (!f) return "";

    bool inSection = false;
    char line[512];
    std::string result;

    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        if (line[0] == '[') {
            inSection = (SDL_strncasecmp(line, "[Skin]", 6) == 0);
            continue;
        }
        if (inSection) {
            char *eq = strchr(line, '=');
            if (eq) {
                *eq = '\0';
                /* Trim key */
                char *k = line;
                while (*k == ' ' || *k == '\t') k++;
                char *kend = eq - 1;
                while (kend > k && (*kend == ' ' || *kend == '\t')) *kend-- = '\0';

                if (SDL_strcasecmp(k, key) == 0) {
                    char *v = eq + 1;
                    while (*v == ' ' || *v == '\t') v++;
                    result = v;
                    break;
                }
            }
        }
    }
    fclose(f);
    return result;
}

/* Scan the skins directory and return a list of skin base names. */
static std::vector<std::string> scanSkins() {
    std::vector<std::string> skins;

    int count = 0;
    char **files = SDL_GlobDirectory(SKIN_DIR_STRING, "*.zip", 0, &count);
    if (files) {
        for (int i = 0; i < count; i++) {
            std::string name(files[i]);
            size_t dot = name.rfind('.');
            if (dot != std::string::npos) name = name.substr(0, dot);
            skins.push_back(name);
        }
        SDL_free(files);
    }

    count = 0;
    files = SDL_GlobDirectory(SKIN_DIR_STRING, "*.wsf", 0, &count);
    if (files) {
        for (int i = 0; i < count; i++) {
            std::string name(files[i]);
            size_t dot = name.rfind('.');
            if (dot != std::string::npos) name = name.substr(0, dot);
            skins.push_back(name);
        }
        SDL_free(files);
    }

    return skins;
}

struct SkinInfo {
    std::string name;
    std::string author;
    std::string notes;
};

static SkinInfo getSkinInfo() {
    SkinInfo info;
    if (skinsIsLoaded()) {
        char skinsDir[512];
        skinsGetSkinDirectory(skinsDir);

        char iniPath[512];
        SDL_snprintf(iniPath, sizeof(iniPath), "%sskin.ini", skinsDir);

        info.name = readSkinIni(iniPath, "Name");
        info.author = readSkinIni(iniPath, "Author");
        info.notes = readSkinIni(iniPath, "Notes");

        if (info.name.empty()) info.name = langGetText(STR_DLGSKIN_NA);
        if (info.author.empty()) info.author = langGetText(STR_DLGSKIN_NA);
        if (info.notes.empty()) info.notes = langGetText(STR_DLGSKIN_NA);
    } else {
        info.name = langGetText(STR_DLGLANG_NAME);
        info.author = langGetText(STR_DLGLANG_AUTHOR);
        info.notes = langGetText(STR_DLGLANG_NOTES);
    }
    return info;
}

extern "C" void imguiSkinsShow(void) {
    SDL_Window *window = sdl3DrawGetWindow();
    SDL_Renderer *renderer = sdl3DrawGetRenderer();
    if (!window || !renderer) return;

    /* Save logical presentation (Android sets one for the game view) */
    int savedLogW = 0, savedLogH = 0;
    SDL_RendererLogicalPresentation savedLogMode = SDL_LOGICAL_PRESENTATION_DISABLED;
    dialogSaveLogicalPresentation(renderer, &savedLogW, &savedLogH, &savedLogMode);

    /* Get screen size and compute UI scale */
    int screenW, screenH;
    SDL_GetWindowSize(window, &screenW, &screenH);
    if (screenW <= 0 || screenH <= 0) { screenW = 1024; screenH = 768; }
    float s = dialogComputeScale(screenW, screenH);

#if !BOLO_MOBILE
    dialogSetWindowSize(window, DIALOG_W, DIALOG_H);
    dialogSetWindowTitle(window, langGetText(STR_DLGSKIN_WINTITLE));
    SDL_SetWindowResizable(window, false);
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

    /* Save previous skin for cancel/restore */
    char prevSkin[FILENAME_MAX];
    skinsGetFileName(prevSkin);

    /* Build skin list */
    std::vector<std::string> skinFiles = scanSkins();

    /* Determine current selection index (0 = default/no skin) */
    char currentSkin[FILENAME_MAX];
    skinsGetFileName(currentSkin);
    int selectedIdx = 0;
    for (size_t i = 0; i < skinFiles.size(); i++) {
        if (skinFiles[i] == currentSkin) {
            selectedIdx = (int)(i + 1);
            break;
        }
    }

    SkinInfo info = getSkinInfo();
    const char *errorMsg = nullptr;
    bool running = true;

    char errPopupId[64];
    SDL_snprintf(errPopupId, sizeof(errPopupId), "%s##skins", langGetText(STR_ERR_TITLE));

    while (running) {
        Uint64 frameCapStart = dialogFrameCapBegin();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (dialogHandleDevicePresetEvent(window, &ev)) continue;
            dialogHandleWindowMoveResize(window, &ev);
            if (ev.type == SDL_EVENT_QUIT ||
                (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                 ev.window.windowID == SDL_GetWindowID(window))) {
                skinsLoadSkin(prevSkin);
                running = false;
            }
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        dialogOverrideFramebufferScale(renderer);
        ImGui::NewFrame();

        int winW, winH;
        SDL_GetWindowSize(window, &winW, &winH);
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)winW, (float)winH));
        ImGui::Begin("##Skins", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse);

        /* Skin list */
        ImGui::TextUnformatted(langGetText(STR_DLGSKIN_SELECT));
        float listH = 150.0f;
        if (ImGui::BeginListBox("##skinlist", ImVec2(-1, listH))) {
            if (ImGui::Selectable(langGetText(STR_DLGSKIN_NOSKIN), selectedIdx == 0)) {
                if (selectedIdx != 0) {
                    selectedIdx = 0;
                    skinsLoadSkin((char *)"");
                    info = getSkinInfo();
                }
            }
            for (size_t i = 0; i < skinFiles.size(); i++) {
                bool isSelected = (selectedIdx == (int)(i + 1));
                if (ImGui::Selectable(skinFiles[i].c_str(), isSelected)) {
                    if (selectedIdx != (int)(i + 1)) {
                        selectedIdx = (int)(i + 1);
                        char skinName[FILENAME_MAX];
                        SDL_strlcpy(skinName, skinFiles[i].c_str(), FILENAME_MAX);
                        if (skinsLoadSkin(skinName)) {
                            info = getSkinInfo();
                        } else {
                            errorMsg = langGetText(STR_DLGSKIN_LOADERR);
                            ImGui::OpenPopup(errPopupId);
                        }
                    }
                }
            }
            ImGui::EndListBox();
        }

        ImGui::Spacing();

        /* Info labels */
        ImGui::Text("%s   %s", langGetText(STR_DLGSKIN_NAME_LBL),   info.name.c_str());
        ImGui::Text("%s %s",   langGetText(STR_DLGSKIN_AUTHOR_LBL), info.author.c_str());
        ImGui::Text("%s  %s",  langGetText(STR_DLGSKIN_NOTES_LBL),  info.notes.c_str());

        ImGui::Spacing();
        ImGui::TextWrapped("%s", langGetText(STR_DLGSKIN_BLURB));

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        float btnW = 80.0f;
        float btnX = ((float)winW - btnW * 2 - 8.0f) / 2.0f;
        ImGui::SetCursorPosX(btnX);

        if (ImGui::Button(langGetText(STR_OK), ImVec2(btnW, 0))) {
            gameFrontReloadSkins();
            running = false;
        }

        ImGui::SameLine(0.0f, 8.0f);
        if (ImGui::Button(langGetText(STR_CANCEL), ImVec2(btnW, 0)) ||
            (ImGui::IsKeyPressed(ImGuiKey_Escape) &&
             !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup))) {
            skinsLoadSkin(prevSkin);
            running = false;
        }

        /* Error popup */
        if (ImGui::BeginPopupModal(errPopupId, nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("%s", errorMsg ? errorMsg : "");
            ImGui::Spacing();
            {
                char okBuf[64];
                snprintf(okBuf, sizeof(okBuf), "%s##err", langGetText(STR_OK));
                if (ImGui::Button(okBuf, ImVec2(80, 0))) {
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }

        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
        dialogFrameCapEnd(frameCapStart);
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    /* Restore logical presentation */
    dialogRestoreLogicalPresentation(renderer, savedLogW, savedLogH, savedLogMode);

#if !BOLO_MOBILE
    SDL_SetWindowResizable(window, true);
#endif

    SDL_FlushEvent(SDL_EVENT_QUIT);
}
