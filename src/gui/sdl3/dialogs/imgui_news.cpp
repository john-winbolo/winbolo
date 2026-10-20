/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
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
#include "../news_image_fit.h"

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

#include "../../../winbolonet/wbn_news_parser.h"

/* An item's body as handed to imgui_markdown: linked thumbnails
 * unwrapped into plain images (see newsUnwrapLinkedImages), with the
 * links they carried kept so a click on the image still opens them.
 * `md` is `unwrapped` with each image that failed to load turned into
 * a link (see refreshFailedImages). */
struct NewsBody {
    std::string                   unwrapped;
    std::string                   md;
    std::vector<WbnNewsImageLink> links;
};

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
static std::vector<NewsBody>    s_bodies;      /* parallel to s_sortedItems */
static unsigned                 s_failedSeen = 0; /* newsImageFailedCount at the last rebuild */

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
    /* A thumbnail that was wrapped in a link is clickable, whether or
     * not it has finished downloading. */
    const NewsBody *body = static_cast<const NewsBody *>(data.userData);
    const bool linked = body &&
        newsFindImageLink(body->links, data.link, (size_t)data.linkLength);

    ImGui::MarkdownImageData out;
    out.useLinkCallback = linked;

    /* imgui_markdown asks for every image in a post on every frame,
     * in view or not. Only an image ImGui will draw is asked for with
     * newsImageGet, which pins it in the cache and starts its
     * download. One scrolled out of view is only looked at: it keeps
     * its size in the layout, so the scroll position does not jump,
     * but the cache may drop it, and it is not downloaded until the
     * player scrolls to it. ImGui::IsRectVisible is the clip test
     * ImGui::Image itself makes before it adds anything to the draw
     * list, so an image it says is out of view is never drawn. The
     * callback runs inside the scroll child, with the cursor where
     * imgui_markdown will place the image. */
    SDL_Texture *tex = nullptr;
    float w = 0.0f, h = 0.0f;
    if (!newsImagePeek(url, &tex, &w, &h)) {
        /* Not loaded: imgui_markdown prints a line of text in its
         * place. Only a visible line starts the download. */
        if (!ImGui::IsRectVisible(ImVec2(1.0f, ImGui::GetTextLineHeight()))) {
            return out;
        }
        tex = newsImageGet(url);
        if (!tex) return out;
        SDL_GetTextureSize(tex, &w, &h);
    }

    /* Shrink the image to the width left on its line and the height of
     * the scroll area, so a wide one doesn't run off the right edge and
     * a tall one fits on screen. */
    const ImGuiStyle &style = ImGui::GetStyle();
    const float maxW = ImGui::GetWindowPos().x +
                       ImGui::GetWindowContentRegionMax().x -
                       ImGui::GetCursorScreenPos().x;
    const float maxH = ImGui::GetWindowHeight() - style.WindowPadding.y * 2.0f;
    newsImageFit(w, h, maxW, maxH, &w, &h);

    if (ImGui::IsRectVisible(ImVec2(w, h))) {
        tex = newsImageGet(url);
        if (!tex) return out;
    }

    out.isValid         = true;
    out.user_texture_id = (ImTextureID)tex;
    out.size            = ImVec2(w, h);
    return out;
}

/* The full-size picture a clicked thumbnail linked to, or nullptr. */
static const char *imageHref(const ImGui::MarkdownLinkCallbackData &data) {
    const NewsBody *body = static_cast<const NewsBody *>(data.userData);
    if (!body || data.linkLength <= 0) return nullptr;
    return newsFindImageLink(body->links, data.link, (size_t)data.linkLength);
}

static void newsLinkCallback(ImGui::MarkdownLinkCallbackData data) {
    if (data.isImage) {
        newsOpenUrlSchemeFiltered(imageHref(data));
        return;
    }
    char url[2048];
    copyLinkBuf(data, url, sizeof(url));
    newsOpenUrlSchemeFiltered(url);
}

/* imgui_markdown calls this while an image or link is hovered. Links
 * get their hand cursor from newsMarkdownFormatCallback; this gives a
 * clickable image the same. */
static void newsTooltipCallback(ImGui::MarkdownTooltipCallbackData data) {
    if (data.linkData.isImage && imageHref(data.linkData)) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
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
        s_bodies.clear();
        s_bodies.reserve(s_sortedItems.size());
        for (const auto &item : s_sortedItems) {
            NewsBody body;
            body.unwrapped = newsUnwrapLinkedImages(
                item.body_md ? std::string(item.body_md) : std::string(),
                body.links);
            body.unwrapped.append("\n");
            body.md = body.unwrapped;
            s_bodies.push_back(std::move(body));
        }
        /* Propagate the resolved country code to the SP / LAN /
         * tutorial cache. Called once per program run, before the
         * AutoShow / consent decision. */
        winbolonetSetCountryCode(wbn_news_fetch_country_code(s_fetch));
    }
    s_resultCollected = true;
}

/* imgui_markdown draws an image it has no texture for as
 * "( Image <url> not loaded )". Once an image has failed for good,
 * rebuild the bodies with it as a link instead, so it can still be
 * opened in the browser. Runs again only when another image fails. */
static void refreshFailedImages(void) {
    const unsigned failed = newsImageFailedCount();
    if (failed == s_failedSeen) return;
    s_failedSeen = failed;
    for (NewsBody &body : s_bodies) {
        body.md = newsLinkFailedImages(
            body.unwrapped, body.links,
            [](const std::string &url) { return newsImageIsFailed(url.c_str()); });
    }
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
#if defined(__IPHONEOS__) || defined(__ANDROID__)
    /* A phone's window is small enough for the auto-sized box to run off it,
     * so it is held inside the window and its text wraps to that width. */
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f),
                                        ImVec2(vp.x * 0.9f, vp.y * 0.9f));
#endif
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

static void renderItem(const WbnNewsItem &item, const NewsBody &body) {
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
    ImGui::MarkdownConfig cfg;
    cfg.linkCallback    = &newsLinkCallback;
    cfg.imageCallback   = &newsImageCallback;
    cfg.tooltipCallback = &newsTooltipCallback;
    cfg.formatCallback  = &newsMarkdownFormatCallback;
    cfg.userData        = const_cast<NewsBody *>(&body);
    ImGui::Markdown(body.md.c_str(), body.md.size(), cfg);

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
    newsImageCacheNewFrame();
    refreshFailedImages();

    ImVec2 vp = ImGui::GetMainViewport()->Size;
#if defined(__IPHONEOS__) || defined(__ANDROID__)
    /* Nearly the whole window on a phone: a fifth of a landscape phone's
     * height is more than the footer row takes. */
    ImGui::SetNextWindowSize(ImVec2(vp.x * 0.95f, vp.y * 0.92f),
                             ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), vp);
#else
    ImGui::SetNextWindowSize(ImVec2(vp.x * 0.8f, vp.y * 0.8f),
                             ImGuiCond_Appearing);
#endif
    ImGui::SetNextWindowPos(ImVec2(vp.x * 0.5f, vp.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    bool open = true;
    ImGuiWindowFlags popupFlags = ImGuiWindowFlags_NoCollapse |
                                  ImGuiWindowFlags_NoSavedSettings;
#if defined(__ANDROID__) || defined(__IPHONEOS__)
    /* A finger dragged on the body scrolls the articles (imgui_touch.cpp);
       without this the drag moved the popup instead, since ImGui takes a
       drag on a window's background as a move. */
    popupFlags |= ImGuiWindowFlags_NoMove;
#endif
    if (!ImGui::BeginPopupModal(title, &open, popupFlags)) {
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
            for (size_t i = 0; i < s_sortedItems.size(); i++) {
                const WbnNewsItem &item = s_sortedItems[i];
                /* Scope IDs per item so the "Comments: N" TextLink (and any
                 * widget imgui_markdown emits internally) doesn't collide
                 * with the same widget on a sibling item that happens to
                 * share its visible label — e.g. two posts both with zero
                 * comments. */
                ImGui::PushID(item.id);
                renderItem(item, s_bodies[i]);
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

#if defined(__IPHONEOS__) || defined(__ANDROID__)
    /* The touch UI draws larger text than the fixed desktop width allows. */
    const float closeBtnW = SDL_max(80.0f,
        ImGui::CalcTextSize(langGetText(STR_CLOSE)).x +
        ImGui::GetStyle().FramePadding.x * 2.0f);
#else
    const float closeBtnW = 80.0f;
#endif
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
    s_bodies.clear();
    s_bodies.shrink_to_fit();
    s_failedSeen = 0;
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
