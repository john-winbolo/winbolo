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

#include "imgui.h"
#include "imgui_markdown.h"

#include "imgui_about.h"
#include "imgui_dialog_utils.h"   /* imguiOpenUrl, imguiHandOnHover,
                                     imguiPopupFadeAlpha */
#include "dialog_footer.h"

extern "C" {
#include "../../lang.h"
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

/* ---------------------------------------------------------------- helpers */

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
    }
    static float s_fade = 0.0f;
    bool open = true;
    ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f, 0.0f),
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
    ImGui::Separator();
    ImGui::TextDisabled("%s", langGetText(STR_DLGABOUT_BOLOCOPYRIGHT));

    ImGui::Spacing();
    ImGui::TextLinkOpenURL("www.winbolo.com", "https://www.winbolo.com/");
    ImGui::SameLine();
    ImGui::TextUnformatted("   ");
    ImGui::SameLine();
    ImGui::TextLinkOpenURL("www.winbolo.net", "https://www.winbolo.net/");

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
