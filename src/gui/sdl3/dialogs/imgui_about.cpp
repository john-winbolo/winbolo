/*
 * About modal: WinBolo version, copyright, web links, and "Third Party
 * Notices" / "Authors" markdown popups.
 *
 * The popup is renderable inside any active ImGui context — both the
 * welcome screen (clicking the bottom-right version label) and the in-game
 * UI (Help \xE2\x86\x92 About / mac app menu) call into the same body, so
 * the dialog looks identical from either entry point.
 *
 * Markdown is rendered via the vendored imgui_markdown header. Source files
 * (THIRD_PARTY_NOTICES.md, AUTHORS.md) are copied into data/ by CMake at
 * build time and loaded lazily via SDL_GetBasePath().
 */

#include <SDL3/SDL.h>

#include <string>

#include "stb_image.h"
#include "imgui.h"
#include "imgui_markdown.h"

#include "imgui_about.h"
#include "imgui_dialog_utils.h"   /* imguiOpenUrl, imguiHandOnHover,
                                     imguiPopupFadeAlpha */
#include "dialog_footer.h"

extern "C" {
#include "../../lang.h"
#include "../sdl3draw.h"          /* sdl3DrawGetRenderer */
}

/* ---------------------------------------------------------------- state
 * All popup flags are file-scope so any active ImGui context can drive
 * the popup. MD content is cached after first load. */
static bool        s_showAbout      = false;
static bool        s_showThirdParty = false;
static bool        s_showAuthors    = false;

/* About-modal version-line stage: 0 short, 1 full, 2 transient "Copied!". */
static int         s_verStage       = 0;
static double      s_copiedAt       = 0.0;

static std::string s_thirdPartyMd;
static std::string s_authorsMd;

/* One-frame flag set by aboutPopupCloseAll(); reset at the end of the
 * next render pass. */
static bool        s_closeAll       = false;

/* ---------------------------------------------------------- logo + shimmer
 * The shield logo on the left of the modal gets a 1.5s "sun glint" sweep on
 * popup open and on each hover-enter. Both textures (the source logo and the
 * offscreen composite target) are lazy-loaded on first render and cached for
 * the program lifetime — the underlying SDL renderer outlives every open of
 * the modal, so caching is safe. */
static const Uint64 SHIMMER_DURATION_MS = 1500;

static SDL_Texture *s_logoTex        = nullptr;
static float        s_logoNativeW    = 0.0f;
static float        s_logoNativeH    = 0.0f;
static SDL_Texture *s_shimmerTarget  = nullptr;
static int          s_shimmerTargetW = 0;
static int          s_shimmerTargetH = 0;
static Uint64       s_shimmerStartMs = 0;   /* 0 = idle */
static bool         s_wasLogoHovered = false;

/* ---------------------------------------------------------------- helpers */

/* Load a PNG from data/ via SDL_IOFromFile + stb_image. Same lookup the
 * welcome screen uses — falls back to "data/<filename>" if the bare name
 * misses (Android APK assets vs desktop layout). */
static SDL_Texture *loadPng(SDL_Renderer *renderer, const char *filename) {
    SDL_IOStream *io = SDL_IOFromFile(filename, "rb");
    if (!io) {
        char path[512];
        SDL_snprintf(path, sizeof(path), "data/%s", filename);
        io = SDL_IOFromFile(path, "rb");
    }
    if (!io) return nullptr;

    Sint64 size = SDL_GetIOSize(io);
    if (size <= 0) { SDL_CloseIO(io); return nullptr; }
    unsigned char *buf = (unsigned char *)SDL_malloc((size_t)size);
    if (!buf) { SDL_CloseIO(io); return nullptr; }
    SDL_ReadIO(io, buf, (size_t)size);
    SDL_CloseIO(io);

    int w, h, channels;
    unsigned char *data = stbi_load_from_memory(buf, (int)size, &w, &h, &channels, 4);
    SDL_free(buf);
    if (!data) return nullptr;

    SDL_Surface *surf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, data, w * 4);
    if (!surf) { stbi_image_free(data); return nullptr; }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_DestroySurface(surf);
    stbi_image_free(data);
    return tex;
}

/* Composite the shield logo onto `target` with an additive "sun glint" band
 * traversing left-to-right at progress `t` in [0,1]. The band is built from
 * 16 vertical strips of `logo` re-drawn with SDL_BLENDMODE_ADD and Gaussian-
 * weighted alpha — re-drawing the logo (rather than a plain quad) makes the
 * additive contribution automatically masked by the logo's own alpha, so
 * the glint only brightens pixels inside the shield silhouette. */
static void composeShimmerFrame(SDL_Renderer *r, SDL_Texture *target,
                                SDL_Texture *logo, int w, int h, float t) {
    SDL_Texture *prevTarget = SDL_GetRenderTarget(r);

    SDL_BlendMode prevTexBlend;
    SDL_GetTextureBlendMode(logo, &prevTexBlend);
    Uint8 prevR = 255, prevG = 255, prevB = 255;
    SDL_GetTextureColorMod(logo, &prevR, &prevG, &prevB);
    Uint8 prevA = 255;
    SDL_GetTextureAlphaMod(logo, &prevA);

    SDL_SetRenderTarget(r, target);
    SDL_SetRenderClipRect(r, NULL);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
    SDL_RenderClear(r);

    /* Pass 1: opaque logo */
    SDL_SetTextureBlendMode(logo, SDL_BLENDMODE_BLEND);
    SDL_SetTextureColorMod(logo, 255, 255, 255);
    SDL_SetTextureAlphaMod(logo, 255);
    SDL_RenderTexture(r, logo, NULL, NULL);

    /* Pass 2: additive glint band */
    const int NSTRIPS = 16;
    const float bandHalfW = (float)w * 0.25f;
    const float stripW    = (bandHalfW * 2.0f) / (float)NSTRIPS;
    const float sigma     = bandHalfW * 0.5f;
    const float centerX   = -bandHalfW + t * ((float)w + 2.0f * bandHalfW);

    SDL_SetTextureBlendMode(logo, SDL_BLENDMODE_ADD);
    for (int i = 0; i < NSTRIPS; i++) {
        float offset = ((float)i - (float)(NSTRIPS - 1) * 0.5f) * stripW;
        float ratio  = offset / sigma;
        float gauss  = SDL_expf(-ratio * ratio);
        Uint8 alpha  = (Uint8)(gauss * 200.0f);
        if (alpha == 0) continue;

        SDL_Rect clip;
        clip.x = (int)(centerX + offset - stripW * 0.5f);
        clip.y = 0;
        clip.w = (int)stripW + 1;
        clip.h = h;
        if (clip.x + clip.w <= 0 || clip.x >= w) continue;

        SDL_SetRenderClipRect(r, &clip);
        SDL_SetTextureAlphaMod(logo, alpha);
        SDL_RenderTexture(r, logo, NULL, NULL);
    }

    /* Reset clip — we leak it to the original target otherwise, and SDL's
     * clip state is global rather than per-target. */
    SDL_SetRenderClipRect(r, NULL);

    /* Restore */
    SDL_SetTextureAlphaMod(logo, prevA);
    SDL_SetTextureColorMod(logo, prevR, prevG, prevB);
    SDL_SetTextureBlendMode(logo, prevTexBlend);
    SDL_SetRenderTarget(r, prevTarget);
}

static void ensureLogoLoaded(SDL_Renderer *r) {
    if (s_logoTex || !r) return;
    s_logoTex = loadPng(r, "smalllogo-transparent.png");
    if (s_logoTex) {
        SDL_GetTextureSize(s_logoTex, &s_logoNativeW, &s_logoNativeH);
    }
}



static int formatShortVer(char *buf, size_t buflen) {
    return SDL_snprintf(buf, buflen, "WinBolo v%s", WINBOLO_VERSION);
}

static int formatFullVer(char *buf, size_t buflen) {
    return SDL_snprintf(buf, buflen,
                        "WinBolo v%s \xC2\xB7 %s \xC2\xB7 %s",
                        WINBOLO_VERSION, WINBOLO_GIT_HASH, WINBOLO_BUILD_DATE);
}

/* imgui_markdown link callback. Only http/https URLs are passed to the
 * system browser; everything else (file:, javascript:, etc.) is ignored so
 * a maliciously-crafted MD file can't trigger arbitrary handlers. */
static void markdownLinkCb(ImGui::MarkdownLinkCallbackData data) {
    if (data.isImage) return;
    std::string url(data.link, data.linkLength);
    if (url.compare(0, 7, "http://") == 0 ||
        url.compare(0, 8, "https://") == 0) {
        imguiOpenUrl(url.c_str());
    }
}

/* Format callback: defer to the library default for colour + underline,
 * then add a hand cursor on hovered links — the library doesn't set one. */
static void markdownFormatCb(const ImGui::MarkdownFormatInfo &info,
                             bool start) {
    ImGui::defaultMarkdownFormatCallback(info, start);
    if (!start &&
        info.type == ImGui::MarkdownFormatType::LINK &&
        info.itemHovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
}

static const ImGui::MarkdownConfig &markdownConfig(void) {
    static ImGui::MarkdownConfig cfg;
    static bool init = false;
    if (!init) {
        cfg.linkCallback    = markdownLinkCb;
        cfg.tooltipCallback = nullptr;
        cfg.imageCallback   = nullptr;
        cfg.linkIcon        = "";
        cfg.userData        = nullptr;
        cfg.formatCallback  = markdownFormatCb;
        init = true;
    }
    return cfg;
}

/* Walk bare http://... / https://... URLs and rewrite them in MD link
 * syntax so imgui_markdown turns them into clickable links. Skips URLs
 * that are already inside `[text](url)` or `(url)` — detected by
 * checking the byte immediately before the scheme. Trailing punctuation
 * (.,;:) is left outside the link so sentences read naturally. */
static std::string autolinkBareUrls(const std::string &in) {
    std::string out;
    out.reserve(in.size() + 64);
    size_t i = 0, n = in.size();
    while (i < n) {
        size_t p = SIZE_MAX;
        size_t hit_http  = in.find("http://",  i);
        size_t hit_https = in.find("https://", i);
        if (hit_http  < p) p = hit_http;
        if (hit_https < p) p = hit_https;
        if (p == SIZE_MAX) { out.append(in, i, std::string::npos); break; }

        /* Find URL end: first whitespace or MD-significant delimiter. */
        size_t end = p;
        while (end < n) {
            char c = in[end];
            if (c == ' '  || c == '\t' || c == '\n' || c == '\r' ||
                c == ')'  || c == ']'  || c == '>'  || c == '<'  ||
                c == '\"' || c == '\'' || c == '`') break;
            end++;
        }
        /* Strip one or more trailing punctuation chars (URLs rarely end
         * in these — they're sentence punctuation). */
        while (end > p + 1) {
            char c = in[end - 1];
            if (c == '.' || c == ',' || c == ';' || c == ':' || c == '!' ||
                c == '?' || c == ')') { end--; continue; }
            break;
        }

        /* Skip if this URL is already inside a MD link construct:
         *   preceded by '(' (target of [text](url))
         *   preceded by '<' (autolink <url>)
         *   preceded by '[' and followed by ']('   (text of [url](url))
         */
        bool prevOpenParen   = (p > 0 && in[p - 1] == '(');
        bool prevAngle       = (p > 0 && in[p - 1] == '<');
        bool isLinkTextSlot  = (p > 0 && in[p - 1] == '[' &&
                                end + 1 < n &&
                                in[end] == ']' && in[end + 1] == '(');
        if (prevOpenParen || prevAngle || isLinkTextSlot) {
            out.append(in, i, end - i);
            i = end;
            continue;
        }

        out.append(in, i, p - i);
        std::string url(in, p, end - p);
        out.append("[").append(url).append("](").append(url).append(")");
        i = end;
    }
    return out;
}

/* Read data/<relpath> into a std::string. Returns empty on miss. Bare
 * URLs are auto-wrapped into MD link syntax so they become clickable in
 * the popup. */
static std::string loadDataFile(const char *relpath) {
    const char *basePath = SDL_GetBasePath();
    if (!basePath) basePath = "";
    char path[1024];
    SDL_snprintf(path, sizeof(path), "%sdata/%s", basePath, relpath);
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (!io) return std::string();
    Sint64 sz = SDL_GetIOSize(io);
    if (sz <= 0) { SDL_CloseIO(io); return std::string(); }
    std::string raw((size_t)sz, '\0');
    SDL_ReadIO(io, &raw[0], (size_t)sz);
    SDL_CloseIO(io);
    return autolinkBareUrls(raw);
}

static void openMarkdownPopup(const char *which) {
    if (SDL_strcmp(which, "third_party") == 0) {
        if (s_thirdPartyMd.empty()) {
            s_thirdPartyMd = loadDataFile("THIRD_PARTY_NOTICES.md");
            if (s_thirdPartyMd.empty()) {
                s_thirdPartyMd = "Third-party notices file not found.";
            }
        }
        s_showThirdParty = true;
    } else if (SDL_strcmp(which, "authors") == 0) {
        if (s_authorsMd.empty()) {
            s_authorsMd = loadDataFile("AUTHORS.md");
            if (s_authorsMd.empty()) {
                s_authorsMd = "Authors file not found.";
            }
        }
        s_showAuthors = true;
    }
}

/* ---------------------------------------------------------------- modals */

static void renderMarkdownPopupBody(const char *titleLabel,
                                    const char *popupId,
                                    bool *showFlag,
                                    const std::string &md,
                                    float *fadePhase);

static void renderAboutModalBody(void) {
    char title[128];
    SDL_snprintf(title, sizeof(title), "%s###about",
                 langGetText(STR_DLGABOUT_TITLE));
    if (s_showAbout) {
        ImGui::OpenPopup(title);
        s_showAbout = false;
        s_verStage  = 0;
        /* Kick off a sweep so the shimmer plays as the modal fades in. */
        s_shimmerStartMs = SDL_GetTicks();
        s_wasLogoHovered = false;
    }
    static float s_fade = 0.0f;
    bool open = true;
    ImGui::SetNextWindowSizeConstraints(ImVec2(480.0f, 0.0f),
                                        ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::BeginPopupModal(title, &open,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, imguiPopupFadeAlpha(&s_fade));

    if (s_closeAll) {
        ImGui::PopStyleVar();
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    /* Lazy-load the logo on first render — the renderer isn't available
     * before then on Android (welcome screen creates it). */
    SDL_Renderer *renderer = sdl3DrawGetRenderer();
    ensureLogoLoaded(renderer);

    /* Shimmer state update + offscreen composition. The displayed image is
     * either the bare logo or the offscreen-composited shimmer frame; ImGui
     * downsamples it from native size to displayLogoW/H. */
    Uint64 nowMs = SDL_GetTicks();
    bool sweeping = (s_shimmerStartMs != 0) &&
                    (nowMs - s_shimmerStartMs < SHIMMER_DURATION_MS);
    if (s_shimmerStartMs != 0 && !sweeping) s_shimmerStartMs = 0;

    ImTextureID logoImageTexID = (ImTextureID)s_logoTex;
    const float displayLogoW = 140.0f;
    float displayLogoH = 0.0f;
    if (s_logoTex && s_logoNativeW > 0.0f) {
        displayLogoH = s_logoNativeH * (displayLogoW / s_logoNativeW);
        if (sweeping && renderer) {
            int targetW = (int)s_logoNativeW;
            int targetH = (int)s_logoNativeH;
            if (!s_shimmerTarget ||
                s_shimmerTargetW != targetW || s_shimmerTargetH != targetH) {
                if (s_shimmerTarget) SDL_DestroyTexture(s_shimmerTarget);
                s_shimmerTarget = SDL_CreateTexture(renderer,
                                                    SDL_PIXELFORMAT_RGBA32,
                                                    SDL_TEXTUREACCESS_TARGET,
                                                    targetW, targetH);
                if (s_shimmerTarget) {
                    SDL_SetTextureBlendMode(s_shimmerTarget, SDL_BLENDMODE_BLEND);
                    s_shimmerTargetW = targetW;
                    s_shimmerTargetH = targetH;
                }
            }
            if (s_shimmerTarget) {
                float t = (float)(nowMs - s_shimmerStartMs) /
                          (float)SHIMMER_DURATION_MS;
                composeShimmerFrame(renderer, s_shimmerTarget, s_logoTex,
                                    s_shimmerTargetW, s_shimmerTargetH, t);
                logoImageTexID = (ImTextureID)s_shimmerTarget;
            }
        }
    }

    /* --- Header row: logo on left, version + credits on right --- */
    ImGui::BeginGroup();
    if (s_logoTex && displayLogoH > 0.0f) {
        ImGui::Image(logoImageTexID, ImVec2(displayLogoW, displayLogoH));
        /* Hover entry triggers a sweep — edge-detected so a slow drag
         * across the logo doesn't fire repeatedly, and a sweep already
         * in progress is left alone. */
        bool isHovered = ImGui::IsItemHovered();
        if (!sweeping && isHovered && !s_wasLogoHovered) {
            s_shimmerStartMs = nowMs;
        }
        s_wasLogoHovered = isHovered;
    } else {
        /* Reserve space so the right column lines up even if the logo
         * failed to load. */
        ImGui::Dummy(ImVec2(displayLogoW, 1.0f));
    }
    ImGui::EndGroup();

    ImGui::SameLine(0.0f, 16.0f);

    ImGui::BeginGroup();
    /* Snap "Copied!" stage back to the full form after a short beat. */
    if (s_verStage == 2 && ImGui::GetTime() - s_copiedAt > 1.8) {
        s_verStage = 1;
    }

    char verBuf[160];
    switch (s_verStage) {
        case 0:  formatShortVer(verBuf, sizeof(verBuf)); break;
        case 1:  formatFullVer(verBuf, sizeof(verBuf));  break;
        default: SDL_strlcpy(verBuf, "Copied!", sizeof(verBuf)); break;
    }
    if (ImGui::Selectable(verBuf, false,
                          ImGuiSelectableFlags_DontClosePopups)) {
        if (s_verStage == 0) {
            s_verStage = 1;
        } else if (s_verStage == 1) {
            char clip[160];
            formatFullVer(clip, sizeof(clip));
            SDL_SetClipboardText(clip);
            s_verStage = 2;
            s_copiedAt = ImGui::GetTime();
        } else {
            s_verStage = 1;
        }
    }
    imguiHandOnHover();

    ImGui::Text("Copyright 1998-%s John Morrison", WINBOLO_BUILD_YEAR);
    ImGui::TextUnformatted(langGetText(STR_DLGABOUT_BOLOCOPYRIGHT));
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight() * 0.6f));
    ImGui::TextUnformatted("Additional 2.0 programming by Andrew Roth");

    ImGui::Spacing();
    ImGui::TextLinkOpenURL("www.winbolo.com", "https://www.winbolo.com/");
    ImGui::SameLine();
    ImGui::TextUnformatted("   ");
    ImGui::SameLine();
    ImGui::TextLinkOpenURL("www.winbolo.net", "https://www.winbolo.net/");
    ImGui::EndGroup();

    ImGui::Spacing();
    if (ImGui::Button("Third Party Notices")) {
        openMarkdownPopup("third_party");
    }
    imguiHandOnHover();
    ImGui::SameLine();
    if (ImGui::Button("Authors")) {
        openMarkdownPopup("authors");
    }
    imguiHandOnHover();
    ImGui::SameLine();
    if (ImGui::Button("Forums")) {
        imguiOpenUrl("https://www.winbolo.net/forums");
    }
    imguiHandOnHover();

    int f = WBUI::DialogFooter(/*cancelLabel*/ nullptr, langGetText(STR_OK));
    if (f != WBUI::FOOTER_NONE) ImGui::CloseCurrentPopup();

    /* Render the MD popups *inside* the About modal's scope so they nest
     * over it. With OpenPopup called from this stack frame, closing the
     * child popup returns to the still-open About modal instead of
     * dismissing everything. */
    static float s_fadeThird   = 0.0f;
    static float s_fadeAuthors = 0.0f;
    renderMarkdownPopupBody("Third Party Notices", "thirdparty",
                            &s_showThirdParty, s_thirdPartyMd, &s_fadeThird);
    renderMarkdownPopupBody("Authors", "authors",
                            &s_showAuthors, s_authorsMd, &s_fadeAuthors);

    ImGui::PopStyleVar();
    ImGui::EndPopup();
}

static void renderMarkdownPopupBody(const char *titleLabel,
                                    const char *popupId,
                                    bool *showFlag,
                                    const std::string &md,
                                    float *fadePhase) {
    char title[128];
    SDL_snprintf(title, sizeof(title), "%s###%s", titleLabel, popupId);
    if (*showFlag) {
        ImGui::OpenPopup(title);
        *showFlag = false;
    }
    bool open = true;
    ImVec2 vp = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowSize(ImVec2(SDL_min(720.0f, vp.x * 0.85f),
                                    SDL_min(540.0f, vp.y * 0.85f)),
                             ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(title, &open,
                                ImGuiWindowFlags_NoCollapse |
                                ImGuiWindowFlags_NoScrollbar |
                                ImGuiWindowFlags_NoScrollWithMouse)) {
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, imguiPopupFadeAlpha(fadePhase));

    if (s_closeAll) {
        ImGui::PopStyleVar();
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    /* DialogFooter draws Spacing + Separator + Spacing + button row.
     * Reserve enough body-trailing space that the child stops above the
     * Spacing line, so the outer window never needs its own scrollbar. */
    const ImGuiStyle &st  = ImGui::GetStyle();
    const float footerH   = st.ItemSpacing.y * 3.0f + 1.0f +
                            ImGui::GetFrameHeightWithSpacing();
    if (ImGui::BeginChild("##body", ImVec2(0.0f, -footerH), false,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        if (!md.empty()) {
            ImGui::Markdown(md.c_str(), md.size(), markdownConfig());
        }
    }
    ImGui::EndChild();
    int f = WBUI::DialogFooter(/*cancelLabel*/ nullptr, langGetText(STR_OK));
    if (f != WBUI::FOOTER_NONE) ImGui::CloseCurrentPopup();
    ImGui::PopStyleVar();
    ImGui::EndPopup();
}

/* ---------------------------------------------------------------- API */

extern "C" void aboutPopupOpen(void) {
    s_showAbout = true;
}

extern "C" void aboutPopupRender(void) {
    /* MD child popups are rendered inside renderAboutModalBody so they
     * nest over the About modal. */
    renderAboutModalBody();
    /* The close-all signal is one-frame; clear after each render pass. */
    s_closeAll = false;
}

extern "C" void aboutPopupCloseAll(void) {
    s_closeAll = true;
}
