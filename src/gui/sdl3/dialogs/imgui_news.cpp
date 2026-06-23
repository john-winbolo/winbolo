/*
 * Copyright (c) 1998-2026 John Morrison.
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
#include "imgui_nav_outline.h"

#include "../news_image_cache.h"

extern "C" {
#include "../../../winbolonet/http.h"
#include "../../../winbolonet/wbn_news.h"
#include "../../../winbolonet/winbolonet_core.h"
#include "../../../common/prefs.h"
#include "../../lang.h"
#include "../../ui_mode.h"
}

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

/* ================================================================
 * Process-wide state.
 * ================================================================ */

static WbnNewsFetch         *s_fetch        = nullptr;
static bool                  s_kicked       = false;
static bool                  s_modalOpen    = false;
static bool                  s_consentOpen  = false;
static bool                  s_modalOpenCalled   = false; /* edge tracker for ImGui::OpenPopup */
static bool                  s_seedScrollFocus   = false; /* one-shot: focus the scroll child on open (controller mode) */
static bool                  s_consentOpenCalled = false;
static bool                  s_dontAutoShow = false;
static int                   s_maxIdAtOpen  = 0;
static bool                  s_resultCollected = false;
static bool                  s_autoShowDecided = false;
static int                   s_maxId        = 0;
static std::vector<WbnNewsItem> s_sortedItems; /* id-desc */

/* ================================================================
 * Prefs helpers — [NEWS] AutoShow and [NEWS] LastSeenId.
 * ================================================================ */

const char *newsPrefGetAutoShow(void) {
    static char s_buf[16];
    s_buf[0] = '\0';
    prefsGetString("NEWS", "AutoShow", "unset",
                   s_buf, (unsigned int)sizeof(s_buf));
    /* Normalise unrecognised values to "unset". */
    if (strcmp(s_buf, "show") != 0 && strcmp(s_buf, "dontShow") != 0) {
        strncpy(s_buf, "unset", sizeof(s_buf) - 1);
        s_buf[sizeof(s_buf) - 1] = '\0';
    }
    return s_buf;
}

void newsPrefSetAutoShow(const char *value) {
    if (!value) return;
    prefsSetString("NEWS", "AutoShow", value);
}

static int newsPrefGetLastSeenId(void) {
    char buf[32] = {0};
    prefsGetString("NEWS", "LastSeenId", "0",
                   buf, (unsigned int)sizeof(buf));
    return SDL_atoi(buf);
}

static void newsPrefSetLastSeenId(int id) {
    char buf[32];
    SDL_snprintf(buf, sizeof(buf), "%d", id);
    prefsSetString("NEWS", "LastSeenId", buf);
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

static void newsMarkdownFormatCallback(
    const ImGui::MarkdownFormatInfo &info, bool start_) {
    ImGui::defaultMarkdownFormatCallback(info, start_);
    if (!start_ &&
        info.type == ImGui::MarkdownFormatType::LINK &&
        info.itemHovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
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
    /* ImGui matches popups by their full string (visible prefix + "##id"
     * tail are both hashed), so OpenPopup and BeginPopupModal MUST be
     * fed the same bytes. Build the title once per frame from the
     * translated prefix and the static "##consent" id suffix. */
    char title[96];
    SDL_snprintf(title, sizeof(title), "%s##consent",
                 langGetText(STR_DLGNEWS_CONSENT_TITLE));
    if (!s_consentOpenCalled) {
        ImGui::OpenPopup(title);
        s_consentOpenCalled = true;
    }
    /* Centre both horizontally AND vertically. AlwaysAutoResize means
     * the window size is only known after one frame of layout, so the
     * first-frame pivot uses size=(0,0). Use ImGuiCond_Always so the
     * pivot re-applies once the auto-resized size is known. */
    ImVec2 vp = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowPos(ImVec2(vp.x * 0.5f, vp.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(title, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextWrapped("%s", langGetText(STR_DLGNEWS_CONSENT_BODY1));
        ImGui::Spacing();
        ImGui::TextWrapped("%s", langGetText(STR_DLGNEWS_CONSENT_BODY2));
        ImGui::Spacing();
        if (ImGui::Button(langGetText(STR_DLGNEWS_SHOW))) {
            newsPrefSetAutoShow("show");
            s_consentOpen       = false;
            s_consentOpenCalled = false;
            s_modalOpen         = true;
            s_maxIdAtOpen       = s_maxId;
            s_dontAutoShow      = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(langGetText(STR_DLGNEWS_DONT_SHOW))) {
            newsPrefSetAutoShow("dontShow");
            s_consentOpen       = false;
            s_consentOpenCalled = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

static void renderItem(const WbnNewsItem &item) {
    /* Header band: filled background rect, scaled-up title, optional
     * dimmed date line beneath. Drawn manually rather than via
     * imgui_markdown's H1 so the band spans the full content width
     * and the date sits in the same visual block as the title. */
    ImGuiStyle &style = ImGui::GetStyle();
    const float padX = style.FramePadding.x * 2.0f;
    const float padY = style.FramePadding.y * 2.0f;
    const float availW = ImGui::GetContentRegionAvail().x;

    ImGui::SetWindowFontScale(1.5f);
    const float titleH = ImGui::GetTextLineHeight();
    ImGui::SetWindowFontScale(1.0f);

    const bool hasDate = (item.date[0] != '\0');
    const float dateGap = hasDate ? style.ItemSpacing.y * 0.5f : 0.0f;
    const float dateH   = hasDate ? ImGui::GetTextLineHeight() : 0.0f;
    const float headerH = padY * 2.0f + titleH + dateGap + dateH;

    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(
        cursor,
        ImVec2(cursor.x + availW, cursor.y + headerH),
        ImGui::GetColorU32(ImGuiCol_TableHeaderBg),
        4.0f);

    /* Reserve the band so the scroll child's content size is correct. */
    ImGui::Dummy(ImVec2(availW, headerH));

    /* Draw title (1.5x) and date (default scale, dimmed) inside the band. */
    ImGui::SetCursorScreenPos(ImVec2(cursor.x + padX, cursor.y + padY));
    ImGui::SetWindowFontScale(1.5f);
    ImGui::TextUnformatted(item.title);
    ImGui::SetWindowFontScale(1.0f);
    if (hasDate) {
        /* date wire format is "YYYY-MM-DD HH:MM:SS"; display the first 16
         * chars ("YYYY-MM-DD HH:MM") — seconds are noise for a news
         * byline. SDL_strlcpy gives us a NUL-terminated truncation even
         * if the server sends a shorter string. */
        char dateBuf[17];
        SDL_strlcpy(dateBuf, item.date, sizeof(dateBuf));
        ImGui::SetCursorScreenPos(ImVec2(cursor.x + padX,
                                         cursor.y + padY + titleH + dateGap));
        ImGui::TextDisabled("%s", dateBuf);
    }

    /* Park the cursor back at the bottom of the band, then add a deliberate
     * gap before the body markdown so the title block visually separates
     * from the post content. */
    ImGui::SetCursorScreenPos(ImVec2(cursor.x, cursor.y + headerH));
    ImGui::Dummy(ImVec2(0.0f, style.ItemSpacing.y * 1.5f));

    /* Body markdown — title is no longer embedded as an H1. */
    std::string md;
    if (item.body_md) md.assign(item.body_md);
    md.append("\n");

    ImGui::MarkdownConfig cfg;
    cfg.linkCallback   = &newsLinkCallback;
    cfg.imageCallback  = &newsImageCallback;
    cfg.formatCallback = &newsMarkdownFormatCallback;
    ImGui::Markdown(md.c_str(), md.size(), cfg);

    /* Comments link — TextLink renders without auto-opening so we can
     * route the click through our scheme-filtered helper rather than
     * the unconditional Platform_OpenInShellFn that TextLinkOpenURL
     * would use. */
    MessageArgs args = {};
    args.number = item.comments;
    if (uiShouldUseControllerMode()) {
        /* Controller mode: render the comments count as a plain, non-navigable
         * label so the d-pad scrolls the article body instead of snapping
         * between per-article comments links. */
        ImGui::TextDisabled("%s", langGetTextFmt(STR_DLGNEWS_COMMENTS_FMT, &args));
    } else if (ImGui::TextLink(langGetTextFmt(STR_DLGNEWS_COMMENTS_FMT, &args))) {
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
    /* Same matching constraint as the consent popup — build the title
     * once per frame so OpenPopup and BeginPopupModal hash to the same
     * string. */
    char title[96];
    SDL_snprintf(title, sizeof(title), "%s##popup",
                 langGetText(STR_DLGNEWS_TITLE));
    if (!s_modalOpenCalled) {
        ImGui::OpenPopup(title);
        s_modalOpenCalled  = true;
        s_seedScrollFocus  = true;
    }
    ImVec2 vp = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowSize(ImVec2(vp.x * 0.8f, vp.y * 0.8f),
                             ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(vp.x * 0.5f, vp.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    bool open = true;
    if (!ImGui::BeginPopupModal(title, &open,
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

    /* On the frame the modal opens in controller mode, focus the scroll child
     * so the d-pad scrolls the article body immediately — without it the user
     * must first navigate into the bordered child. The article body has no
     * navigable items, so ImGui falls back to directional-key scrolling. */
    if (s_seedScrollFocus && uiShouldUseControllerMode()) {
        ImGui::SetNextWindowFocus();
    }
    s_seedScrollFocus = false;

    if (ImGui::BeginChild("scroll", ImVec2(0.0f, -footerH), true,
                          ImGuiWindowFlags_None)) {
        if (s_sortedItems.empty()) {
            ImGui::TextUnformatted(langGetText(STR_DLGNEWS_LOADING));
        } else {
            for (const auto &item : s_sortedItems) {
                /* Scope IDs per item so the "Comments: N" TextLink (and any
                 * widget imgui_markdown emits internally) doesn't collide
                 * with the same widget on a sibling item that happens to
                 * share its visible label — e.g. two posts both with zero
                 * comments. */
                ImGui::PushID(item.id);
                renderItem(item);
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();

    /* Footer row: checkbox left-aligned, Close right-aligned. */
    bool prev = s_dontAutoShow;
    ImGui::Checkbox(langGetText(STR_DLGNEWS_DONT_AUTOSHOW), &s_dontAutoShow);
    if (s_dontAutoShow != prev) {
        newsPrefSetAutoShow(s_dontAutoShow ? "dontShow" : "show");
    }

    const float closeBtnW = 80.0f;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - closeBtnW);
    bool closeClicked = ImGui::Button(langGetText(STR_CLOSE), ImVec2(closeBtnW, 0));
    /* Escape inside the scrollable article child pops out of it via
       ImGui's NavCancel rather than closing the whole popup — matches the
       dialog footer / lobby cancel paths for controller nav. */
    bool escClosed    = ImGui::IsKeyPressed(ImGuiKey_Escape) &&
                        !dialogNavWasInsideSubRegionAtFrameStart();

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
    s_autoShowDecided   = false;
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

    /* The auto-show / consent decision runs at most once per program
     * run, once the fetch has landed with items. Without this gate,
     * every idle welcome-screen frame would re-read [NEWS] AutoShow
     * from the INI file. */
    if (s_autoShowDecided) return;
    s_autoShowDecided = true;

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
