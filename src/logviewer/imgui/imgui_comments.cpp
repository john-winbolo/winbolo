/*
 * imgui_comments.cpp - ImGui WinBolo.net comments window for Log Viewer
 *
 * Copyright (c) 2024
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Optional, hidden by default panel that lists comments on the loaded log
 * (if it has a WBN key) and lets a signed-in user post new ones. The token
 * is read from the shared WinBolo.ini [WINBOLO.NET] Token slot — there is
 * no sign-in flow inside LogViewer.
 */

#include "imgui_comments.h"
#include "imgui_main_menu.h"
#include "imgui_context.h"
#include "platform_config.h"
#include "imgui.h"
#include "../../gui/lang.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include "../../winbolonet/wbn_comments.h"
#include "../../winbolonet/http.h"
}

/* ---- State ---- */
static char                s_wbn_key[33]     = "";    /* current log's key */
static char                s_fetched_for[33] = "";    /* key the cache is for */
static WbnCommentsFetch   *s_fetch           = nullptr;
static bool                s_fetch_complete  = false;
static int                 s_fetch_status    = 0;
static char                s_fetch_err[256]  = "";

struct CachedComment {
    char username[64];
    char comment[512];
    char time_formatted[32];
    int  rating;
};
static CachedComment      *s_cached     = nullptr;
static size_t              s_cached_len = 0;
static size_t              s_cached_cap = 0;

static WbnCommentPost     *s_post           = nullptr;
static char                s_post_msg[256]  = "";
static int                 s_post_status    = 0;
static char                s_comment_text[512] = "";
static int                 s_comment_rating = 0;

static void clearCache(void) {
    free(s_cached);
    s_cached = nullptr;
    s_cached_len = 0;
    s_cached_cap = 0;
}

extern "C" void lv_imgui_comments_init(void) {
    s_wbn_key[0] = '\0';
    s_fetched_for[0] = '\0';
    if (s_fetch) { wbn_comments_fetch_free(s_fetch); s_fetch = nullptr; }
    if (s_post)  { wbn_comments_post_free(s_post);   s_post  = nullptr; }
    s_fetch_complete = false;
    s_fetch_status = 0;
    s_fetch_err[0] = '\0';
    s_post_msg[0] = '\0';
    s_post_status = 0;
    s_comment_text[0] = '\0';
    s_comment_rating = 0;
    clearCache();
}

extern "C" void lv_imgui_comments_shutdown(void) {
    if (s_fetch) { wbn_comments_fetch_free(s_fetch); s_fetch = nullptr; }
    if (s_post)  { wbn_comments_post_free(s_post);   s_post  = nullptr; }
    clearCache();
}

extern "C" void lv_imgui_comments_set_key(const char *wbnKey) {
    if (!wbnKey || wbnKey[0] == '\0' || strlen(wbnKey) != 32) {
        s_wbn_key[0] = '\0';
        return;
    }
    if (strncmp(s_wbn_key, wbnKey, sizeof(s_wbn_key)) == 0) return;
    strncpy(s_wbn_key, wbnKey, sizeof(s_wbn_key) - 1);
    s_wbn_key[sizeof(s_wbn_key) - 1] = '\0';

    /* Drop any cached results — they're for the previous log. */
    s_fetched_for[0] = '\0';
    if (s_fetch) { wbn_comments_fetch_free(s_fetch); s_fetch = nullptr; }
    s_fetch_complete = false;
    s_fetch_status = 0;
    s_fetch_err[0] = '\0';
    clearCache();
    /* Don't cancel an in-flight post — it may still complete against the
     * old key and we just won't auto-refresh that one. */
}

/* ---- Fetch lifecycle ---- */

static void kickFetchIfNeeded(void) {
    if (s_wbn_key[0] == '\0') return;
    if (s_fetch || s_fetch_complete) return;
    s_fetch = wbn_comments_fetch_start(s_wbn_key);
    if (!s_fetch) {
        /* HTTP not initialised yet, or empty key — nothing to do. */
        return;
    }
    strncpy(s_fetched_for, s_wbn_key, sizeof(s_fetched_for) - 1);
    s_fetched_for[sizeof(s_fetched_for) - 1] = '\0';
}

static void pollFetch(void) {
    if (!s_fetch || !wbn_comments_fetch_done(s_fetch)) return;

    const WbnComment *raw = nullptr;
    size_t count = 0;
    int status = wbn_comments_fetch_result(s_fetch, &raw, &count,
                                           s_fetch_err, sizeof(s_fetch_err));

    clearCache();
    if (count > 0 && raw) {
        s_cached = (CachedComment *)calloc(count, sizeof(CachedComment));
        if (s_cached) {
            s_cached_cap = count;
            for (size_t i = 0; i < count; i++) {
                strncpy(s_cached[i].username, raw[i].username, sizeof(s_cached[i].username) - 1);
                strncpy(s_cached[i].comment,  raw[i].comment,  sizeof(s_cached[i].comment)  - 1);
                strncpy(s_cached[i].time_formatted, raw[i].time_formatted,
                        sizeof(s_cached[i].time_formatted) - 1);
                s_cached[i].rating = raw[i].rating;
            }
            s_cached_len = count;
        }
    }

    s_fetch_status = status;
    s_fetch_complete = true;

    wbn_comments_fetch_free(s_fetch);
    s_fetch = nullptr;
}

static void pollPost(void) {
    if (!s_post || !wbn_comments_post_done(s_post)) return;

    s_post_status = wbn_comments_post_result(s_post, s_post_msg, sizeof(s_post_msg));
    wbn_comments_post_free(s_post);
    s_post = nullptr;

    if (s_post_status == 200 || s_post_status == 201) {
        s_comment_text[0] = '\0';
        s_comment_rating = 0;
        /* Refresh the comment list so the new entry shows up. */
        s_fetch_complete = false;
        s_fetched_for[0] = '\0';
        clearCache();
    }
}

/* ---- Render ---- */

extern "C" void lv_imgui_comments_window(void) {
    if (!lv_g_show_comments_window) {
        return;
    }

    {
        ImGuiCond cond = lv_g_reset_window_positions ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
        ImVec2 vp = ImGui::GetMainViewport()->Size;
        ImGui::SetNextWindowSize(ImVec2(420, 360), cond);
        ImGui::SetNextWindowPos(ImVec2(vp.x - 420 - 10, 60), cond);
    }

    char title[128];
    snprintf(title, sizeof(title), "%s###comments", langGetText(STR_LV_WIN_COMMENTS));
    if (ImGui::Begin(title, &lv_g_show_comments_window, ImGuiWindowFlags_NoCollapse)) {
        /* Reposition window relative to right/bottom edge when viewport is resized */
        float dx, dy;
        if (lv_imgui_context_get_resize_delta(&dx, &dy)) {
            ImVec2 pos = ImGui::GetWindowPos();
            float old_vp_x = ImGui::GetMainViewport()->Size.x - dx;
            float old_vp_y = ImGui::GetMainViewport()->Size.y - dy;
            ImVec2 size = ImGui::GetWindowSize();
            if (pos.x + size.x * 0.5f > old_vp_x * 0.5f) ImGui::SetWindowPos(ImVec2(pos.x + dx, pos.y + dy));
            else if (pos.y + size.y * 0.5f > old_vp_y * 0.5f) ImGui::SetWindowPos(ImVec2(pos.x, pos.y + dy));
        }
        {
            ImVec2 pos = ImGui::GetWindowPos();
            ImVec2 size = ImGui::GetWindowSize();
            ImVec2 vp = ImGui::GetMainViewport()->Size;
            ImVec2 new_pos = pos;
            if (new_pos.x + size.x > vp.x) new_pos.x = vp.x - size.x;
            if (new_pos.y + size.y > vp.y) new_pos.y = vp.y - size.y;
            if (new_pos.x < 0) new_pos.x = 0;
            if (new_pos.y < 0) new_pos.y = 0;
            if (new_pos.x != pos.x || new_pos.y != pos.y) ImGui::SetWindowPos(new_pos);
        }

        if (s_wbn_key[0] == '\0') {
            ImGui::TextWrapped("%s", langGetText(STR_LV_INFO_NO_WBN_KEY));
            ImGui::End();
            return;
        }

        kickFetchIfNeeded();
        pollFetch();
        pollPost();

        /* ---- Comment list ---- */
        ImGui::BeginChild("CommentsList",
                          ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 3.5f),
                          true);
        if (!s_fetch_complete && s_fetch) {
            ImGui::TextDisabled("%s", langGetText(STR_DLGWBN_LOADINGDETAIL));
        } else if (s_fetch_complete && s_fetch_status != 200) {
            const char *err = s_fetch_err[0] ? s_fetch_err : langGetText(STR_DLGWBN_NETERR);
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", err);
        } else if (s_cached_len == 0) {
            ImGui::TextDisabled("%s", langGetText(STR_DLGWBN_NOCOMMENTS));
        } else {
            for (size_t i = 0; i < s_cached_len; i++) {
                const CachedComment &c = s_cached[i];
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.85f, 1.0f, 1.0f));
                ImGui::TextUnformatted(c.username);
                ImGui::PopStyleColor();
                if (c.time_formatted[0]) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("- %s", c.time_formatted);
                }
                if (c.rating > 0) {
                    ImGui::SameLine();
                    ImGui::Text("(%d/10)", c.rating);
                }
                ImGui::TextWrapped("  %s", c.comment);
                ImGui::Spacing();
            }
        }
        ImGui::EndChild();

        /* ---- Post form ---- */
        char wbnToken[256] = {0};
        lv_platform_config_get_string("WINBOLO.NET", "Token", "", wbnToken, sizeof(wbnToken));
        bool loggedIn = wbnToken[0] != '\0';

        if (!loggedIn) {
            ImGui::TextWrapped("%s", langGetText(STR_LV_INFO_SIGNIN_TO_COMMENT));
        } else {
            ImGui::SetNextItemWidth(60);
            ImGui::Combo("##cmtRating", &s_comment_rating,
                         "None\0 1\0 2\0 3\0 4\0 5\0 6\0 7\0 8\0 9\0 10\0");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##cmtText", langGetText(STR_DLGWBN_HINT_COMMENT),
                                     s_comment_text, sizeof(s_comment_text));

            bool canPost = s_comment_text[0] != '\0' && s_post == nullptr;
            if (!canPost) ImGui::BeginDisabled();
            if (ImGui::Button(langGetText(STR_DLGWBN_POST))) {
                s_post_status = 0;
                s_post_msg[0] = '\0';
                s_post = wbn_comments_post_start(s_wbn_key, wbnToken,
                                                 s_comment_text, s_comment_rating);
            }
            if (!canPost) ImGui::EndDisabled();

            if (s_post) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", langGetText(STR_DLGWBN_LOADINGDETAIL));
            } else if (s_post_status == 200 || s_post_status == 201) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.3f, 1, 0.3f, 1), "%s",
                                   langGetText(STR_DLGWBN_POSTED));
            } else if (s_post_status != 0) {
                ImGui::SameLine();
                const char *err = s_post_msg[0] ? s_post_msg : langGetText(STR_DLGWBN_NETERR);
                ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", err);
            }
        }
    }
    ImGui::End();
}
