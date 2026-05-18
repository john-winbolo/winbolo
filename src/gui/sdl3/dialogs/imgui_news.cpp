/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          imgui_news
 * Filename:      imgui_news.cpp
 * Purpose:
 *   Welcome-screen news popup implementation: polls
 *   wbn_news_fetch, drives the consent / modal state
 *   machine, renders item bodies as Markdown with images
 *   routed through news_image_cache (WBN-allowlisted) and
 *   links routed through a scheme-filtered SDL_OpenURL
 *   helper.
 *********************************************************/

#include "imgui_news.h"

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_markdown.h"

#include "../news_image_cache.h"

extern "C" {
#include "../../../winbolonet/http.h"
#include "../../../winbolonet/wbn_news.h"
#include "../../../winbolonet/winbolonet_core.h"
#include "../../../winbolonet/wbn_prefs_path.h"
}

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef _WIN32
/* Provided by posix_stubs.c. Forward-declared (rather than including
 * posix_stubs.h) so this TU doesn't pull src/server onto the dialog
 * include path. Matches the pattern in wbn_country_cache.c. */
extern "C" {
void preferencesGetPreferenceFile(char *dest);
unsigned int GetPrivateProfileString(const char *section, const char *key,
                                     const char *def, char *out,
                                     unsigned int outSize,
                                     const char *filePath);
int WritePrivateProfileString(const char *section, const char *key,
                              const char *value, const char *filePath);
}
#endif

/* ================================================================
 * Process-wide state.
 * ================================================================ */

static WbnNewsFetch         *s_fetch        = nullptr;
static bool                  s_kicked       = false;
static bool                  s_modalOpen    = false;
static bool                  s_consentOpen  = false;
static bool                  s_modalOpenCalled   = false; /* edge tracker for ImGui::OpenPopup */
static bool                  s_consentOpenCalled = false;
static bool                  s_dontAutoShow = false;
static int                   s_maxIdAtOpen  = 0;
static bool                  s_resultCollected = false;
static int                   s_maxId        = 0;
static std::vector<WbnNewsItem> s_sortedItems; /* id-desc */

/* ================================================================
 * INI helpers — [NEWS] AutoShow and [NEWS] LastSeenId.
 * Prefs-path resolution mirrors wbn_country_cache.c.
 * ================================================================ */

static void resolvePrefsPath(char *out, size_t outSize) {
    if (!out || outSize == 0) return;
#ifdef _WIN32
    const char *p = winbolonetCorePrefsPath();
    if (!p) p = "";
    strncpy(out, p, outSize - 1);
    out[outSize - 1] = '\0';
#else
    preferencesGetPreferenceFile(out);
#endif
}

static const char *newsPrefGetAutoShow(void) {
    static char s_buf[16];
    char prefs[FILENAME_MAX];
    resolvePrefsPath(prefs, sizeof(prefs));
    GetPrivateProfileString("NEWS", "AutoShow", "unset",
                            s_buf, (unsigned int)sizeof(s_buf), prefs);
    /* Normalise unrecognised values to "unset". */
    if (strcmp(s_buf, "show") != 0 && strcmp(s_buf, "dontShow") != 0) {
        strncpy(s_buf, "unset", sizeof(s_buf) - 1);
        s_buf[sizeof(s_buf) - 1] = '\0';
    }
    return s_buf;
}

static void newsPrefSetAutoShow(const char *value) {
    if (!value) return;
    char prefs[FILENAME_MAX];
    resolvePrefsPath(prefs, sizeof(prefs));
    WritePrivateProfileString("NEWS", "AutoShow", value, prefs);
}

static int newsPrefGetLastSeenId(void) {
    char prefs[FILENAME_MAX];
    char buf[32] = {0};
    resolvePrefsPath(prefs, sizeof(prefs));
    GetPrivateProfileString("NEWS", "LastSeenId", "0",
                            buf, (unsigned int)sizeof(buf), prefs);
    return SDL_atoi(buf);
}

static void newsPrefSetLastSeenId(int id) {
    char prefs[FILENAME_MAX];
    char buf[32];
    resolvePrefsPath(prefs, sizeof(prefs));
    SDL_snprintf(buf, sizeof(buf), "%d", id);
    WritePrivateProfileString("NEWS", "LastSeenId", buf, prefs);
}

/* ================================================================
 * URL helpers — scheme-filtered SDL_OpenURL.
 * ================================================================ */

static bool urlHasHttpScheme(const char *url) {
    if (!url) return false;
    /* Case-insensitive prefix match against "http://" or "https://". */
    static const char *kPrefixes[] = { "http://", "https://" };
    for (const char *prefix : kPrefixes) {
        size_t plen = strlen(prefix);
        size_t i;
        for (i = 0; i < plen; i++) {
            if (url[i] == '\0') break;
            if (tolower((unsigned char)url[i]) !=
                tolower((unsigned char)prefix[i])) {
                break;
            }
        }
        if (i == plen) return true;
    }
    return false;
}

static void newsOpenUrlSchemeFiltered(const char *url) {
    if (!url || !*url) return;
    if (!urlHasHttpScheme(url)) {
        fprintf(stderr,
                "WinBolo.net news: refusing link with non-http scheme: %s\n",
                url);
        return;
    }
    SDL_OpenURL(url);
}

/* ================================================================
 * Markdown callbacks.
 * ================================================================ */

static void copyLinkBuf(const ImGui::MarkdownLinkCallbackData &data,
                        char *out, size_t outSize) {
    if (!out || outSize == 0) return;
    int n = data.linkLength;
    if (n < 0) n = 0;
    if ((size_t)n >= outSize) n = (int)outSize - 1;
    SDL_memcpy(out, data.link, (size_t)n);
    out[n] = '\0';
}

static ImGui::MarkdownImageData newsImageCallback(
    ImGui::MarkdownLinkCallbackData data) {
    if (!data.isImage || data.linkLength <= 0) {
        return ImGui::MarkdownImageData{};
    }
    char url[2048];
    copyLinkBuf(data, url, sizeof(url));
    SDL_Texture *tex = newsImageGet(url);
    if (!tex) return ImGui::MarkdownImageData{};

    float w = 0.0f, h = 0.0f;
    SDL_GetTextureSize(tex, &w, &h);
    ImGui::MarkdownImageData out;
    out.isValid         = true;
    out.useLinkCallback = false;
    out.user_texture_id = (ImTextureID)tex;
    out.size            = ImVec2(w, h);
    return out;
}

static void newsLinkCallback(ImGui::MarkdownLinkCallbackData data) {
    if (data.isImage) return;
    char url[2048];
    copyLinkBuf(data, url, sizeof(url));
    newsOpenUrlSchemeFiltered(url);
}

/* ================================================================
 * One-shot collection of the fetch result.
 * ================================================================ */

static void collectFetchResultOnce(void) {
    if (s_resultCollected || !s_fetch) return;
    if (!wbn_news_fetch_done(s_fetch)) return;

    const WbnNewsItem *raw = nullptr;
    size_t n = 0;
    int status = wbn_news_fetch_result(s_fetch, &raw, &n);
    if (status == 200 && n > 0) {
        s_sortedItems.assign(raw, raw + n);
        std::sort(s_sortedItems.begin(), s_sortedItems.end(),
                  [](const WbnNewsItem &a, const WbnNewsItem &b) {
                      return a.id > b.id;
                  });
        s_maxId = s_sortedItems.front().id;
        /* Propagate the resolved country code to the SP / LAN /
         * tutorial cache. Called once per program run, before the
         * AutoShow / consent decision. */
        winbolonetSetCountryCode(wbn_news_fetch_country_code(s_fetch));
    }
    s_resultCollected = true;
}

/* ================================================================
 * Modal + consent renderers.
 * ================================================================ */

static void renderConsentDialog(void) {
    if (!s_consentOpenCalled) {
        ImGui::OpenPopup("WinBolo News##consent");
        s_consentOpenCalled = true;
    }
    ImVec2 vp = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowPos(ImVec2(vp.x * 0.5f, vp.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("WinBolo News##consent", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextWrapped(
            "WinBolo can show you new posts from winbolo.net when you "
            "launch the game. We will check WBN once at startup and only "
            "open this window when there is something new.");
        ImGui::Spacing();
        if (ImGui::Button("Show news")) {
            newsPrefSetAutoShow("show");
            s_consentOpen       = false;
            s_consentOpenCalled = false;
            s_modalOpen         = true;
            s_maxIdAtOpen       = s_maxId;
            s_dontAutoShow      = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't show")) {
            newsPrefSetAutoShow("dontShow");
            s_consentOpen       = false;
            s_consentOpenCalled = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

static void renderItem(const WbnNewsItem &item) {
    /* Compose "# title\n\nbody\n" so imgui_markdown renders the
     * title as an H1 with the body underneath in one Markdown
     * call per item. */
    std::string md;
    md.reserve(strlen(item.title) + (item.body_md ? strlen(item.body_md) : 0) + 8);
    md.append("# ");
    md.append(item.title);
    md.append("\n\n");
    if (item.body_md) md.append(item.body_md);
    md.append("\n");

    ImGui::MarkdownConfig cfg;
    cfg.linkCallback  = &newsLinkCallback;
    cfg.imageCallback = &newsImageCallback;
    ImGui::Markdown(md.c_str(), md.size(), cfg);

    /* Comments link — TextLink renders without auto-opening so we can
     * route the click through our scheme-filtered helper rather than
     * the unconditional Platform_OpenInShellFn that TextLinkOpenURL
     * would use. */
    char label[64];
    SDL_snprintf(label, sizeof(label), "Comments: %d", item.comments);
    if (ImGui::TextLink(label)) {
        newsOpenUrlSchemeFiltered(item.url);
    }

    ImGui::Separator();
}

static void finaliseClose(void) {
    if (s_maxIdAtOpen > 0) {
        newsPrefSetLastSeenId(s_maxIdAtOpen);
    }
    s_modalOpen       = false;
    s_modalOpenCalled = false;
}

static void renderNewsModal(void) {
    if (!s_modalOpenCalled) {
        ImGui::OpenPopup("News##popup");
        s_modalOpenCalled = true;
    }
    ImVec2 vp = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowSize(ImVec2(vp.x * 0.8f, vp.y * 0.8f),
                             ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(vp.x * 0.5f, vp.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    bool open = true;
    if (!ImGui::BeginPopupModal("News##popup", &open,
                                ImGuiWindowFlags_NoCollapse |
                                ImGuiWindowFlags_NoSavedSettings)) {
        /* ImGui has already torn the popup down (e.g. the title-bar X
         * was clicked on a previous frame and BeginPopupModal flipped
         * `open` to false then auto-dismissed). Finalise our state so
         * we stop thinking the modal is up. */
        finaliseClose();
        return;
    }

    const float footerH = ImGui::GetFrameHeightWithSpacing() + 4.0f;

    if (ImGui::BeginChild("scroll", ImVec2(0.0f, -footerH), true,
                          ImGuiWindowFlags_None)) {
        if (s_sortedItems.empty()) {
            ImGui::TextUnformatted("Loading…");
        } else {
            for (const auto &item : s_sortedItems) {
                renderItem(item);
            }
        }
    }
    ImGui::EndChild();

    /* Footer row: checkbox left-aligned, Close right-aligned. */
    bool prev = s_dontAutoShow;
    ImGui::Checkbox("Don't auto-show news in future", &s_dontAutoShow);
    if (s_dontAutoShow != prev) {
        newsPrefSetAutoShow(s_dontAutoShow ? "dontShow" : "show");
    }

    const float closeBtnW = 80.0f;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - closeBtnW);
    bool closeClicked = ImGui::Button("Close", ImVec2(closeBtnW, 0));
    bool escClosed    = ImGui::IsKeyPressed(ImGuiKey_Escape);

    bool shouldClose = !open || closeClicked || escClosed;
    if (shouldClose) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();

    if (shouldClose) {
        finaliseClose();
    }
}

/* ================================================================
 * Public surface.
 * ================================================================ */

extern "C" bool newsPopupIsOpen(void) {
    return s_modalOpen || s_consentOpen;
}

extern "C" bool newsPopupHasUnread(void) {
    if (!s_resultCollected || s_sortedItems.empty()) return false;
    return s_maxId > newsPrefGetLastSeenId();
}

extern "C" void newsPopupKickFetch(void) {
    if (s_kicked) return;
    s_kicked = true;
    /* The welcome screen runs before any game UI calls httpCreate, so the
     * news fetch worker would otherwise see an empty wbnBaseUrl and emit
     * CURLE_URL_MALFORMAT. httpCreate is reentrant via libcurl's global-
     * init ref-counting; the wbn_browser dialog still owns its own
     * create/destroy pair. */
    httpCreate();
    s_fetch  = wbn_news_fetch_start();
}

extern "C" void newsPopupOpenManual(void) {
    s_dontAutoShow = (strcmp(newsPrefGetAutoShow(), "dontShow") == 0);
    s_maxIdAtOpen  = s_maxId;
    s_modalOpen    = true;
}

extern "C" void newsPopupShutdown(void) {
    if (s_fetch) {
        wbn_news_fetch_free(s_fetch);
        s_fetch = nullptr;
    }
    newsImageCacheShutdown();
    s_kicked            = false;
    s_modalOpen         = false;
    s_consentOpen       = false;
    s_modalOpenCalled   = false;
    s_consentOpenCalled = false;
    s_dontAutoShow      = false;
    s_maxIdAtOpen       = 0;
    s_resultCollected   = false;
    s_maxId             = 0;
    s_sortedItems.clear();
    s_sortedItems.shrink_to_fit();
}

extern "C" void newsPopupTick(void) {
    /* Always try to collect the fetch result before deciding what to
     * render, so a manually-opened modal can transition from "Loading…"
     * to populated content the frame after the worker lands.
     * collectFetchResultOnce short-circuits on !s_fetch / !done /
     * already-collected, so this is cheap to call every frame. */
    collectFetchResultOnce();

    if (s_modalOpen)   { renderNewsModal();     return; }
    if (s_consentOpen) { renderConsentDialog(); return; }
    if (!s_fetch || !wbn_news_fetch_done(s_fetch)) return;

    if (s_sortedItems.empty()) return;

    const char *pref = newsPrefGetAutoShow();
    int lastSeen = newsPrefGetLastSeenId();
    if (strcmp(pref, "unset") == 0) {
        s_consentOpen = true;
        return;
    }
    if (strcmp(pref, "show") == 0 && s_maxId > lastSeen) {
        s_modalOpen    = true;
        s_maxIdAtOpen  = s_maxId;
        s_dontAutoShow = false;
    }
    /* "dontShow" → leave closed; the welcome-screen mini-button's
     * unread dot (via newsPopupHasUnread) signals fresh items. */
}
