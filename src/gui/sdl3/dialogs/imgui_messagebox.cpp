/*
 * Copyright (c) 1998-2026 John Morrison.
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
#include "imgui_nav_outline.h"
#include "imgui_keycap.h"
#include "dialog_footer.h"
#include "nanosvg.h"
#include "nanosvgrast.h"
#include "../imgui_steam_nav.h"

extern "C" {
#include "../sdl3draw.h"
#include "../sdl3imgui.h"
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
        case IMGUI_MSG_NONE:    return NULL;
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

    /* Focus the primary action so Enter activates it. The helper draws
     * the buttons; we set focus on the next item before that call so the
     * first button in the cluster receives focus. */
    if (*focusBtn) { ImGui::SetKeyboardFocusHere(); *focusBtn = false; }

    if (buttons == IMGUI_MSG_OK) {
        int f = WBUI::DialogFooter(/*cancelLabel*/ nullptr,
                                   langGetText(STR_OK));
        if (f == WBUI::FOOTER_CONFIRM || f == WBUI::FOOTER_CANCEL) {
            result = IMGUI_MSG_RESULT_OK;
        }
    } else if (buttons == IMGUI_MSG_YES_NO) {
        /* 2-button: [No][Yes] — Yes is the affirmative/primary, No is the
         * cancel-equivalent. Matches the spec's [Cancel][Confirm] shape. */
        int f = WBUI::DialogFooter(langGetText(STR_NO),
                                   langGetText(STR_YES));
        if (f == WBUI::FOOTER_CONFIRM) result = IMGUI_MSG_RESULT_YES;
        else if (f == WBUI::FOOTER_CANCEL) result = IMGUI_MSG_RESULT_NO;
    } else { /* IMGUI_MSG_YES_NO_CANCEL */
        /* 3-button: [Cancel (stay)][No (destructive — discard)][Yes (primary — save)]. */
        int f = WBUI::DialogFooter3(langGetText(STR_CANCEL),
                                    langGetText(STR_NO),
                                    langGetText(STR_YES));
        if (f == WBUI::FOOTER_CONFIRM)         result = IMGUI_MSG_RESULT_YES;
        else if (f == WBUI::FOOTER_DESTRUCTIVE) result = IMGUI_MSG_RESULT_NO;
        else if (f == WBUI::FOOTER_CANCEL)     result = IMGUI_MSG_RESULT_CANCEL;
    }

    return result;
}

/* Helpers for the rich (segment-based) renderer. */

/* A flat element flattened from segments: a single word, a glyph, or
 * a hard break.  Width/height are measured up-front so wrap and row
 * height come out of the same numbers used to draw. */
enum RichElemKind { RICH_WORD, RICH_GLYPH_PNG, RICH_GLYPH_KEYCAP, RICH_HARD_BREAK };

typedef struct RichElem {
    RichElemKind kind;
    const char  *text;        /* RICH_WORD: pointer into segment text. */
    int          textLen;     /* RICH_WORD: byte length (no NUL). */
    SDL_Texture *glyph;       /* RICH_GLYPH_PNG. */
    const char  *keycapLabel; /* RICH_GLYPH_KEYCAP. */
    float        w, h;
} RichElem;

#define RICH_ELEM_MAX 512

/* Flatten segments into elements.  TEXT segments split on whitespace;
 * '\n' becomes a HARD_BREAK; glyphs become single elements sized to
 * `glyphSize` square. */
static int flattenSegments(const TutorialSeg *segments, int segCount,
                           float glyphSize, RichElem *out, int max) {
    int n = 0;
    for (int i = 0; i < segCount && n < max; ++i) {
        const TutorialSeg *seg = &segments[i];
        if (seg->kind == TUTORIAL_SEG_TEXT) {
            const char *p = seg->text;
            if (!p) continue;
            while (*p && n < max) {
                if (*p == '\n') {
                    /* Collapse newlines: a lone '\n' acts as a space so the
                       source's fixed-width line breaks reflow to the box; two
                       or more in a row are paragraph breaks and become hard
                       breaks (preserving the blank line between paragraphs). */
                    int runlen = 0;
                    while (*p == '\n') { runlen++; p++; }
                    if (runlen >= 2) {
                        for (int b = 0; b < runlen && n < max; ++b) {
                            out[n].kind = RICH_HARD_BREAK;
                            out[n].w = 0;
                            out[n].h = ImGui::GetTextLineHeight();
                            n++;
                        }
                    }
                    continue;
                }
                if (*p == ' ' || *p == '\t') { p++; continue; }
                const char *w = p;
                while (*w && *w != ' ' && *w != '\t' && *w != '\n') w++;
                int len = (int)(w - p);
                ImVec2 ts = ImGui::CalcTextSize(p, w);
                out[n].kind = RICH_WORD;
                out[n].text = p;
                out[n].textLen = len;
                out[n].w = ts.x;
                out[n].h = ImGui::GetTextLineHeight();
                n++;
                p = w;
            }
        } else if (seg->kind == TUTORIAL_SEG_GLYPH_PNG) {
            out[n].kind = RICH_GLYPH_PNG;
            out[n].glyph = seg->glyph;
            out[n].w = glyphSize;
            out[n].h = glyphSize;
            n++;
        } else if (seg->kind == TUTORIAL_SEG_GLYPH_KEYCAP) {
            out[n].kind = RICH_GLYPH_KEYCAP;
            out[n].keycapLabel = seg->keycapLabel ? seg->keycapLabel : "?";
            out[n].w = glyphSize;
            out[n].h = glyphSize;
            n++;
        }
    }
    return n;
}

/* Render a rich-content message box (segments) and return -1 while
 * open or a button-press result.  Layout matches renderMessageBoxContent
 * for the icon and button rows. */
static int renderRichMessageBoxContent(const TutorialSeg *segments, int segCount,
                                       ImguiMsgButtons buttons,
                                       SDL_Texture *iconTex, float scale,
                                       bool *focusBtn) {
    int result = -1;

    float iconDisplaySize = ICON_SIZE * scale;
    if (iconTex) {
        ImGui::Image((ImTextureID)iconTex,
                     ImVec2(iconDisplaySize, iconDisplaySize));
        ImGui::SameLine();
    }

    float textStartY = ImGui::GetCursorPosY();
    float textLineH  = ImGui::GetTextLineHeight();
    float iconMidY = textStartY + (iconDisplaySize * 0.5f) -
                     (textLineH * 0.5f);
    if (iconTex && iconMidY > textStartY) {
        ImGui::SetCursorPosY(iconMidY);
    }

    float glyphSize  = textLineH * 2.0f;
    float wordSpace  = ImGui::GetStyle().ItemInnerSpacing.x;
    float lineSpace  = ImGui::GetStyle().ItemSpacing.y;

    /* Wrap right edge in screen space — the popup's content right. */
    ImVec2 layoutOrigin = ImGui::GetCursorScreenPos();
    float wrapRight     = layoutOrigin.x + ImGui::GetContentRegionAvail().x;
    float rowStartX     = layoutOrigin.x;

    static RichElem elems[RICH_ELEM_MAX];
    int nElems = flattenSegments(segments, segCount, glyphSize,
                                 elems, RICH_ELEM_MAX);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
    float penY = layoutOrigin.y;

    /* Two-pass per row: walk elements forward to find the row end and
       its height, then draw the row at that height with each element
       vertically centred.  Row ends at HARD_BREAK, end of input, or
       when adding the next element would cross wrapRight. */
    int i = 0;
    while (i < nElems) {
        int rowStart = i;
        float rowW = 0;
        float rowH = textLineH;
        bool hardBreak = false;
        while (i < nElems) {
            const RichElem *e = &elems[i];
            if (e->kind == RICH_HARD_BREAK) {
                hardBreak = true;
                break;
            }
            float advance = (i == rowStart ? 0 : wordSpace) + e->w;
            if (i != rowStart && rowW + advance > wrapRight - rowStartX) break;
            rowW += advance;
            if (e->h > rowH) rowH = e->h;
            i++;
        }

        float drawX = rowStartX;
        for (int k = rowStart; k < i; ++k) {
            const RichElem *e = &elems[k];
            if (k > rowStart) drawX += wordSpace;
            float yOffset = (rowH - e->h) * 0.5f;
            ImVec2 p = ImVec2(drawX, penY + yOffset);
            switch (e->kind) {
                case RICH_WORD:
                    dl->AddText(p, textCol, e->text, e->text + e->textLen);
                    break;
                case RICH_GLYPH_PNG:
                    dl->AddImage((ImTextureID)e->glyph, p,
                                 ImVec2(p.x + e->w, p.y + e->h));
                    break;
                case RICH_GLYPH_KEYCAP:
                    drawProceduralKeycapAt(p, e->h, e->keycapLabel);
                    break;
                case RICH_HARD_BREAK:
                    break;
            }
            drawX += e->w;
        }

        penY += rowH + lineSpace;
        if (hardBreak) i++;
    }

    /* Reserve the consumed vertical space inside ImGui so the
       separator and buttons start beneath the rendered rows.  Subtract
       one trailing line-spacing — the final row added it but no row
       follows, and ImGui::Spacing() below contributes its own gap. */
    float consumedY = penY - layoutOrigin.y;
    if (nElems > 0) consumedY -= lineSpace;
    if (consumedY < 0) consumedY = 0;
    ImGui::Dummy(ImVec2(wrapRight - rowStartX, consumedY));

    float afterTextY = ImGui::GetCursorPosY();
    float afterIconY = textStartY + iconDisplaySize +
                       ImGui::GetStyle().ItemSpacing.y;
    if (afterIconY > afterTextY) {
        ImGui::SetCursorPosY(afterIconY);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    /* Buttons — same layout as the plain-text variant. */
    float btnW = 80.0f;
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float availW = ImGui::GetContentRegionAvail().x;

    if (buttons == IMGUI_MSG_OK) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availW - btnW) / 2.0f);
        if (*focusBtn) { ImGui::SetKeyboardFocusHere(); *focusBtn = false; }
        if (ImGui::Button(langGetText(STR_OK), ImVec2(btnW, 0))) {
            result = IMGUI_MSG_RESULT_OK;
        }
    } else if (buttons == IMGUI_MSG_YES_NO) {
        float totalW = btnW * 2 + spacing;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availW - totalW) / 2.0f);
        if (*focusBtn) { ImGui::SetKeyboardFocusHere(); *focusBtn = false; }
        if (ImGui::Button(langGetText(STR_YES), ImVec2(btnW, 0))) {
            result = IMGUI_MSG_RESULT_YES;
        }
        ImGui::SameLine();
        if (ImGui::Button(langGetText(STR_NO), ImVec2(btnW, 0))) {
            result = IMGUI_MSG_RESULT_NO;
        }
    } else {
        float totalW = btnW * 3 + spacing * 2;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availW - totalW) / 2.0f);
        if (*focusBtn) { ImGui::SetKeyboardFocusHere(); *focusBtn = false; }
        if (ImGui::Button(langGetText(STR_YES), ImVec2(btnW, 0))) {
            result = IMGUI_MSG_RESULT_YES;
        }
        ImGui::SameLine();
        if (ImGui::Button(langGetText(STR_NO), ImVec2(btnW, 0))) {
            result = IMGUI_MSG_RESULT_NO;
        }
        ImGui::SameLine();
        if (ImGui::Button(langGetText(STR_CANCEL), ImVec2(btnW, 0))) {
            result = IMGUI_MSG_RESULT_CANCEL;
        }
    }

    return result;
}

/* In-loop counterpart to imguiMessageBoxRich: fills the caller's already-
 * begun popup with the segment body + OK button on the main context.  Used
 * by the tutorial overlay, which renders every frame rather than spinning a
 * private blocking event loop, so the game's input gate and solo-pause path
 * apply while a message is up. */
extern "C" bool imguiRichSegmentsBody(const TutorialSeg *segments,
                                      int segmentCount, bool *focusBtn) {
    /* Cache the info icon — the overlay redraws every frame, so reloading
       the SVG per frame would be wasteful. */
    static SDL_Texture *s_infoIcon = nullptr;
    static bool s_infoIconTried = false;
    if (!s_infoIconTried) {
        SDL_Renderer *renderer = sdl3DrawGetRenderer();
        const char *iconPath = iconPathForType(IMGUI_MSG_INFO);
        if (renderer && iconPath)
            s_infoIcon = imguiLoadSvgIcon(renderer, iconPath, ICON_SIZE);
        s_infoIconTried = true;
    }
    bool localFocus = focusBtn ? *focusBtn : false;
    /* Scale only feeds the icon size here — glyphs/text size off the main
       context's already-scaled GetTextLineHeight().  Match the icon to the UI
       scale so it isn't a tiny 48px stamp next to scaled-up text. */
    float scale = sdl3ImguiGetUiScale();
    if (scale <= 0.0f) scale = 1.0f;
    int r = renderRichMessageBoxContent(segments, segmentCount, IMGUI_MSG_OK,
                                        s_infoIcon, scale, &localFocus);
    if (focusBtn) *focusBtn = localFocus;
    return r >= 0;
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
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigNavCursorVisibleAlways = true;
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    imguiApplyBoloTheme();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    dialogApplyScaling(s);

    const char *iconPath = iconPathForType(type);
    SDL_Texture *iconTex = iconPath
        ? imguiLoadSvgIcon(renderer, iconPath, ICON_SIZE)
        : nullptr;

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
            dialogHandleGamepadCancelEvent(window, &ev);
            if (dialogHandleQuitEvent(window, &ev)) {
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
                /* Cmd+W (Mac) / Ctrl+W (other) — same effect as Esc */
                } else if (k == SDLK_W && (ev.key.mod & KMOD_PRIMARY)) {
                    if (buttons == IMGUI_MSG_OK)
                        result = IMGUI_MSG_RESULT_OK;
                    else if (buttons == IMGUI_MSG_YES_NO)
                        result = IMGUI_MSG_RESULT_NO;
                    else
                        result = IMGUI_MSG_RESULT_CANCEL;
#ifdef __APPLE__
                /* Cmd+. — same effect as Esc */
                } else if (k == SDLK_PERIOD && (ev.key.mod & SDL_KMOD_GUI)) {
                    if (buttons == IMGUI_MSG_OK)
                        result = IMGUI_MSG_RESULT_OK;
                    else if (buttons == IMGUI_MSG_YES_NO)
                        result = IMGUI_MSG_RESULT_NO;
                    else
                        result = IMGUI_MSG_RESULT_CANCEL;
#endif
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
        imguiSteamNavActivateMenuSet();
        imguiSteamNavFeedCurrentContext();

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

        dialogDrawNavOutline();
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

/* Password entry for joining a protected game. Same shell as
 * imguiMessageBoxEx (own context, captured backbuffer, dimmed overlay);
 * the body is the message, a masked InputText and a Cancel / OK footer.
 * Enter confirms, Escape cancels. */
extern "C" int imguiPasswordPrompt(const char *title, const char *message,
                                   char *out, size_t outCap) {
    SDL_Window *window = sdl3DrawGetWindow();
    SDL_Renderer *renderer = sdl3DrawGetRenderer();
    if (!window || !renderer || !out || outCap == 0) return IMGUI_MSG_RESULT_CANCEL;

    if (s_messageBoxActive) return IMGUI_MSG_RESULT_CANCEL;
    s_messageBoxActive = true;

    ImGuiContext *callerCtx = ImGui::GetCurrentContext();

    int savedLogW = 0, savedLogH = 0;
    SDL_RendererLogicalPresentation savedLogMode = SDL_LOGICAL_PRESENTATION_DISABLED;
    dialogSaveLogicalPresentation(renderer, &savedLogW, &savedLogH, &savedLogMode);

    int screenW, screenH;
    SDL_GetWindowSize(window, &screenW, &screenH);
    if (screenW <= 0 || screenH <= 0) { screenW = 1024; screenH = 768; }
    float s = dialogComputeScale(screenW, screenH);

    SDL_Texture *bgTex = captureBackbuffer(renderer);

    ImGuiContext *msgCtx = ImGui::CreateContext();
    ImGui::SetCurrentContext(msgCtx);
    imguiRegisterPlatformOpenUrl();

    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigNavCursorVisibleAlways = true;
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    imguiApplyBoloTheme();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    dialogApplyScaling(s);

    SDL_Texture *iconTex = imguiLoadSvgIcon(renderer, iconPathForType(IMGUI_MSG_INFO),
                                            ICON_SIZE);

    float dialogW = (float)screenW * DIALOG_WIDTH_FRACTION;
    if (dialogW < DIALOG_MIN_W) dialogW = DIALOG_MIN_W;
    if (dialogW > DIALOG_MAX_W) dialogW = DIALOG_MAX_W;
    dialogW *= s;

    /* Local entry buffer: the caller's buffer is only written on OK. The
     * wire carries at most MAP_STR_SIZE - 1 characters, and outCap is the
     * caller's cap on top of that. */
    char entry[MAP_STR_SIZE];
    entry[0] = '\0';
    size_t entryCap = sizeof(entry);
    if (outCap < entryCap) entryCap = outCap;

    int result = -1;
    bool focusField = true;
    bool openedPopup = false;
    const char *popupId = title ? title : "Password";

    while (result < 0) {
        Uint64 frameCapStart = dialogFrameCapBegin();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            dialogHandleGamepadCancelEvent(window, &ev);
            if (dialogHandleQuitEvent(window, &ev)) {
                result = IMGUI_MSG_RESULT_CANCEL;
            } else if (ev.type == SDL_EVENT_KEY_DOWN) {
                SDL_Keycode k = ev.key.key;
                /* Enter is handled by the InputText (EnterReturnsTrue) so a
                 * confirm always carries the text the field holds. */
                if (k == SDLK_ESCAPE) {
                    result = IMGUI_MSG_RESULT_CANCEL;
                } else if (k == SDLK_W && (ev.key.mod & KMOD_PRIMARY)) {
                    result = IMGUI_MSG_RESULT_CANCEL;
#ifdef __APPLE__
                } else if (k == SDLK_PERIOD && (ev.key.mod & SDL_KMOD_GUI)) {
                    result = IMGUI_MSG_RESULT_CANCEL;
#endif
                }
            }
        }

        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);
        if (bgTex) {
            SDL_RenderTexture(renderer, bgTex, NULL, NULL);
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
        imguiSteamNavActivateMenuSet();
        imguiSteamNavFeedCurrentContext();

        int winW, winH;
        SDL_GetWindowSize(window, &winW, &winH);
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)winW, (float)winH));
        ImGui::Begin("##PasswordHost", nullptr,
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
        static float s_fadePassword = 0.0f;
        if (ImGui::BeginPopupModal(popupId, nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoMove)) {
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                                imguiPopupFadeAlpha(&s_fadePassword));
            ImVec2 modalSize = ImGui::GetWindowSize();
            ImGui::SetWindowPos(
                ImVec2(((float)winW - modalSize.x) * 0.5f,
                       ((float)winH - modalSize.y) * 0.5f));

            float iconDisplaySize = ICON_SIZE * s;
            if (iconTex) {
                ImGui::Image((ImTextureID)iconTex,
                             ImVec2(iconDisplaySize, iconDisplaySize));
                ImGui::SameLine();
            }
            float textStartY = ImGui::GetCursorPosY();
            ImGui::BeginGroup();
            float textRegionW = ImGui::GetContentRegionAvail().x;
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + textRegionW);
            ImGui::TextUnformatted(message ? message : "");
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            if (focusField) { ImGui::SetKeyboardFocusHere(); focusField = false; }
            ImGui::SetNextItemWidth(-FLT_MIN);
            bool enter = ImGui::InputText("##password", entry, entryCap,
                                          ImGuiInputTextFlags_Password |
                                          ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::EndGroup();
            float afterTextY = ImGui::GetCursorPosY();
            float afterIconY = textStartY + iconDisplaySize +
                               ImGui::GetStyle().ItemSpacing.y;
            if (afterIconY > afterTextY) ImGui::SetCursorPosY(afterIconY);

            /* enterConfirms is off: Enter is consumed by the field above,
             * so the footer only reports clicks and gamepad accept. */
            int f = WBUI::DialogFooter(langGetText(STR_CANCEL),
                                       langGetText(STR_OK),
                                       /*enterConfirms*/ false);
            if (enter || f == WBUI::FOOTER_CONFIRM) {
                SDL_strlcpy(out, entry, outCap);
                result = IMGUI_MSG_RESULT_OK;
                ImGui::CloseCurrentPopup();
            } else if (f == WBUI::FOOTER_CANCEL) {
                result = IMGUI_MSG_RESULT_CANCEL;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleVar();
            ImGui::EndPopup();
        }

        ImGui::End(); /* ##PasswordHost */

        dialogDrawNavOutline();
        ImGui::Render();
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
        dialogFrameCapEnd(frameCapStart);
    }

    if (iconTex) SDL_DestroyTexture(iconTex);

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(msgCtx);

    /* Same as imguiMessageBoxEx: leave the undimmed snapshot in the
     * backbuffer so a following modal does not dim it twice. */
    if (bgTex) {
        SDL_RenderTexture(renderer, bgTex, NULL, NULL);
        SDL_DestroyTexture(bgTex);
    }

    dialogRestoreLogicalPresentation(renderer, savedLogW, savedLogH, savedLogMode);
    ImGui::SetCurrentContext(callerCtx);

    SDL_FlushEvent(SDL_EVENT_QUIT);
    s_messageBoxActive = false;
    return result;
}

extern "C" int imguiMessageBoxRich(const char *title,
                                   const TutorialSeg *segments, int segmentCount,
                                   ImguiMsgType type, ImguiMsgButtons buttons) {
    SDL_Window *window = sdl3DrawGetWindow();
    SDL_Renderer *renderer = sdl3DrawGetRenderer();
    if (!window || !renderer) return IMGUI_MSG_RESULT_OK;

    if (s_messageBoxActive) return IMGUI_MSG_RESULT_OK;
    s_messageBoxActive = true;

    ImGuiContext *callerCtx = ImGui::GetCurrentContext();

    int savedLogW = 0, savedLogH = 0;
    SDL_RendererLogicalPresentation savedLogMode = SDL_LOGICAL_PRESENTATION_DISABLED;
    dialogSaveLogicalPresentation(renderer, &savedLogW, &savedLogH, &savedLogMode);

    int screenW, screenH;
    SDL_GetWindowSize(window, &screenW, &screenH);
    if (screenW <= 0 || screenH <= 0) { screenW = 1024; screenH = 768; }
    float s = dialogComputeScale(screenW, screenH);

    SDL_Texture *bgTex = captureBackbuffer(renderer);

    ImGuiContext *msgCtx = ImGui::CreateContext();
    ImGui::SetCurrentContext(msgCtx);

    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigNavCursorVisibleAlways = true;
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    imguiApplyBoloTheme();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    dialogApplyScaling(s);

    SDL_Texture *iconTex = imguiLoadSvgIcon(renderer, iconPathForType(type), ICON_SIZE);

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
            dialogHandleGamepadCancelEvent(window, &ev);
            if (dialogHandleQuitEvent(window, &ev)) {
                result = (buttons == IMGUI_MSG_YES_NO_CANCEL) ?
                         IMGUI_MSG_RESULT_CANCEL : IMGUI_MSG_RESULT_OK;
            } else if (ev.type == SDL_EVENT_KEY_DOWN) {
                SDL_Keycode k = ev.key.key;
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    result = (buttons == IMGUI_MSG_OK)
                             ? IMGUI_MSG_RESULT_OK
                             : IMGUI_MSG_RESULT_YES;
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

        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);
        if (bgTex) {
            SDL_RenderTexture(renderer, bgTex, NULL, NULL);
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
        imguiSteamNavActivateMenuSet();
        imguiSteamNavFeedCurrentContext();

        int winW, winH;
        SDL_GetWindowSize(window, &winW, &winH);
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)winW, (float)winH));
        ImGui::Begin("##MsgBoxHostRich", nullptr,
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
        if (ImGui::BeginPopupModal(popupId, nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoMove)) {
            ImVec2 modalSize = ImGui::GetWindowSize();
            ImGui::SetWindowPos(
                ImVec2(((float)winW - modalSize.x) * 0.5f,
                       ((float)winH - modalSize.y) * 0.5f));

            int r = renderRichMessageBoxContent(segments, segmentCount,
                                                buttons, iconTex, s, &focusBtn);
            if (r >= 0) {
                result = r;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::End();

        dialogDrawNavOutline();
        ImGui::Render();
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
        dialogFrameCapEnd(frameCapStart);
    }

    if (iconTex) SDL_DestroyTexture(iconTex);

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(msgCtx);

    /* See imguiMessageBoxEx for why we redraw without presenting. */
    if (bgTex) {
        SDL_RenderTexture(renderer, bgTex, NULL, NULL);
        SDL_DestroyTexture(bgTex);
    }

    dialogRestoreLogicalPresentation(renderer, savedLogW, savedLogH, savedLogMode);
    ImGui::SetCurrentContext(callerCtx);
    SDL_FlushEvent(SDL_EVENT_QUIT);
    s_messageBoxActive = false;
    return result;
}

extern "C" void imguiMessageBox(const char *message, const char *title) {
    imguiMessageBoxEx(title, message, IMGUI_MSG_INFO, IMGUI_MSG_OK);
}
