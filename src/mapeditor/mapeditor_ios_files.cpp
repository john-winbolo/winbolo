/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_ios_files.cpp
 * Purpose:       See mapeditor_ios_files.h. SDL's file
 *                dialogs are the dummy backend on iOS and
 *                answer every request with an error, so the
 *                editor asks through this picker instead.
 *********************************************************/

#include <SDL3/SDL.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "imgui.h"
#include "mapeditor_ios_files.h"
#include "../gui/lang.h"

#define ME_IOS_FILES_POPUP "###meIosFiles"
#define ME_IOS_NAME_MAX    128

struct MeIosFileRow {
    std::string name;   /* shown */
    std::string path;   /* full path */
};

static bool s_pending = false;   /* a pick was asked for and not yet opened */
static bool s_save    = false;
static char s_ext[16];
static char s_name[ME_IOS_NAME_MAX];
static std::vector<MeIosFileRow> s_userRows;
static std::vector<MeIosFileRow> s_bundleRows;

bool meIosFilesDocumentsDir(char *out, size_t cap) {
    const char *docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    if (!docs || !docs[0] || cap == 0) return false;
    SDL_strlcpy(out, docs, cap);
    size_t n = strlen(out);
    while (n > 1 && out[n - 1] == '/') out[--n] = '\0';
    return true;
}

bool meIosFilesIsWritable(const char *path) {
    char docs[1024];
    if (!path || !path[0] || !meIosFilesDocumentsDir(docs, sizeof(docs))) return false;
    size_t n = strlen(docs);
    return strncmp(path, docs, n) == 0 && path[n] == '/';
}

/* The files in dir whose names end in ".<ext>", any case, sorted by name. */
static void meIosFilesScan(const char *dir, std::vector<MeIosFileRow> &rows) {
    rows.clear();
    char pattern[32];
    SDL_snprintf(pattern, sizeof(pattern), "*.%s", s_ext);
    int count = 0;
    char **names = SDL_GlobDirectory(dir, pattern, SDL_GLOB_CASEINSENSITIVE, &count);
    if (!names) return;
    for (int i = 0; i < count; i++) {
        MeIosFileRow row;
        row.name = names[i];
        row.path = std::string(dir) + "/" + names[i];
        rows.push_back(row);
    }
    SDL_free(names);
    std::sort(rows.begin(), rows.end(),
              [](const MeIosFileRow &a, const MeIosFileRow &b) {
                  return SDL_strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
              });
}

void meIosFilesBegin(bool save, const char *ext, const char *suggestedName) {
    s_save = save;
    SDL_strlcpy(s_ext, ext ? ext : "", sizeof(s_ext));
    s_name[0] = '\0';
    if (suggestedName && suggestedName[0]) {
        /* Only the file name: a suggestion may come as a full path. */
        const char *slash = strrchr(suggestedName, '/');
        SDL_strlcpy(s_name, slash ? slash + 1 : suggestedName, sizeof(s_name));
    }

    char docs[1024];
    s_userRows.clear();
    if (meIosFilesDocumentsDir(docs, sizeof(docs))) {
        meIosFilesScan(docs, s_userRows);
    }
    s_bundleRows.clear();
    if (!save) {
        /* The maps shipped in the app bundle, read-only. */
        const char *base = SDL_GetBasePath();
        char bundled[1024];
        SDL_snprintf(bundled, sizeof(bundled), "%sdata/maps", base ? base : "");
        meIosFilesScan(bundled, s_bundleRows);
    }
    s_pending = true;
}

bool meIosObjectDeleteButton(void) {
    ImGuiViewport *vp = ImGui::GetMainViewport();
    float margin = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - margin,
                                   vp->WorkPos.y + margin),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    bool tapped = false;
    if (ImGui::Begin("##meIosObjDelete", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav)) {
        float h = ImGui::GetFontSize() * 2.0f;
        tapped = ImGui::Button(langGetText(STR_MAPEDIT_DELETE), ImVec2(0.0f, h));
    }
    ImGui::End();
    return tapped;
}

/* A row tall enough to hit with a finger. */
static bool meIosFilesRow(const MeIosFileRow &row, int id) {
    ImGui::PushID(id);
    float h = ImGui::GetFontSize() * 2.0f;
    bool hit = ImGui::Selectable(row.name.c_str(), false, 0, ImVec2(0.0f, h));
    ImGui::PopID();
    return hit;
}

/* The save path for the typed name, with the extension added when the name
   has none. False for a name that is empty or would leave Documents. */
static bool meIosFilesSavePath(char *out, size_t cap) {
    if (s_name[0] == '\0' || strchr(s_name, '/') || strcmp(s_name, ".") == 0 ||
        strcmp(s_name, "..") == 0) {
        return false;
    }
    char docs[1024];
    if (!meIosFilesDocumentsDir(docs, sizeof(docs))) return false;
    size_t nameLen = strlen(s_name);
    size_t extLen = strlen(s_ext);
    bool hasExt = nameLen > extLen + 1 && s_name[nameLen - extLen - 1] == '.' &&
                  SDL_strcasecmp(s_name + nameLen - extLen, s_ext) == 0;
    if (hasExt) {
        SDL_snprintf(out, cap, "%s/%s", docs, s_name);
    } else {
        SDL_snprintf(out, cap, "%s/%s.%s", docs, s_name, s_ext);
    }
    return true;
}

bool meIosFilesRender(char *outPath, size_t outCap) {
    if (s_pending) {
        ImGui::OpenPopup(ME_IOS_FILES_POPUP);
        s_pending = false;
    }

    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f,
                                   vp->Pos.y + vp->Size.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x * 0.8f, vp->Size.y * 0.8f),
                             ImGuiCond_Appearing);

    char title[128];
    SDL_snprintf(title, sizeof(title), "%s" ME_IOS_FILES_POPUP,
                 langGetText(s_save ? STR_MAPEDIT_MENU_SAVEAS
                                    : STR_MAPEDIT_MENU_OPEN));
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        return false;
    }

    bool done = false;
    outPath[0] = '\0';

    if (s_save) {
        ImGui::TextUnformatted(langGetText(STR_MAPEDIT_IOS_NAME));
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool enter = ImGui::InputText("##meIosName", s_name, sizeof(s_name),
                                      ImGuiInputTextFlags_EnterReturnsTrue);
        char path[1200];
        bool valid = meIosFilesSavePath(path, sizeof(path));
        if (valid && SDL_GetPathInfo(path, nullptr)) {
            ImGui::TextWrapped("%s", langGetText(STR_MAPEDIT_IOS_REPLACES));
        }
        ImGui::BeginDisabled(!valid);
        if (ImGui::Button(langGetText(STR_MAPEDIT_MENU_SAVE)) || (enter && valid)) {
            SDL_strlcpy(outPath, path, outCap);
            done = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(langGetText(STR_CANCEL))) done = true;

        /* Tapping an existing file takes its name, to save over it. */
        if (!s_userRows.empty()) {
            ImGui::SeparatorText(langGetText(STR_MAPEDIT_IOS_YOURFILES));
            ImGui::BeginChild("##meIosSaveList");
            for (size_t i = 0; i < s_userRows.size(); i++) {
                if (meIosFilesRow(s_userRows[i], (int)i)) {
                    SDL_strlcpy(s_name, s_userRows[i].name.c_str(), sizeof(s_name));
                }
            }
            ImGui::EndChild();
        }
    } else {
        if (ImGui::Button(langGetText(STR_CANCEL))) done = true;
        ImGui::BeginChild("##meIosOpenList");
        ImGui::SeparatorText(langGetText(STR_MAPEDIT_IOS_YOURFILES));
        if (s_userRows.empty()) {
            ImGui::TextWrapped("%s", langGetText(STR_MAPEDIT_IOS_NOFILES));
        }
        for (size_t i = 0; i < s_userRows.size() && !done; i++) {
            if (meIosFilesRow(s_userRows[i], (int)i)) {
                SDL_strlcpy(outPath, s_userRows[i].path.c_str(), outCap);
                done = true;
            }
        }
        if (!s_bundleRows.empty()) {
            ImGui::SeparatorText(langGetText(STR_MAPEDIT_IOS_BUILTIN));
            for (size_t i = 0; i < s_bundleRows.size() && !done; i++) {
                if (meIosFilesRow(s_bundleRows[i], 10000 + (int)i)) {
                    SDL_strlcpy(outPath, s_bundleRows[i].path.c_str(), outCap);
                    done = true;
                }
            }
        }
        ImGui::EndChild();
    }

    if (done) {
        ImGui::CloseCurrentPopup();
        s_userRows.clear();
        s_bundleRows.clear();
    }
    ImGui::EndPopup();
    return done;
}
