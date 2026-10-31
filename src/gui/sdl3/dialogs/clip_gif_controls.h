/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*********************************************************
 * Name:          clip_gif_controls.h
 * Purpose:       The controls of recording mode, as the
 *                game's replay (lobby_reel.cpp) and the Log
 *                Viewer (imgui_clip_gif.cpp, imgui_controls.cpp)
 *                both draw them: the movie camera toggle, the
 *                GIF button, the cancel button, the length
 *                field, the zoomed timeline with the GIF's span
 *                on it, the span's tint on the main timeline,
 *                and the crop frame over the view.
 *
 *                The rules behind them, with no ImGui in them,
 *                are in clip_gif_core.h. Header only, so a
 *                target that builds one of the two needs no
 *                other file for it.
 *********************************************************/

#ifndef CLIP_GIF_CONTROLS_H
#define CLIP_GIF_CONTROLS_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "imgui.h"
#include "imgui_internal.h"  /* ImRect, ItemAdd, ButtonBehavior: the marker */

#include "clip_gif_core.h"
#include "clip_gif_gallery.h"    /* CLIP_GIF_GALLERY_BUILT, clipGifGalleryButton */
#include "imgui_dialog_utils.h"  /* imguiHelpTooltip */
#include "../../lang.h"          /* langGetText, STR_CLIPGIF_* */

/* The drawn glyphs of the toggle buttons. */
enum ClipGifGlyph {
    CLIP_GIF_GLYPH_CAMERA = 0,  /* a movie camera: recording mode */
    CLIP_GIF_GLYPH_EYE,         /* an eye: preview (not a triangle, which
                                 * is the replay's own play button) */
    CLIP_GIF_GLYPH_STOP,        /* a filled square: stop the preview */
    CLIP_GIF_GLYPH_CLOSE        /* a cross: leave recording mode */
};

/* The smallest crop frame, in drawn pixels, and how far from an edge a press
 * still takes hold of it. */
#define CLIP_GIF_CROP_MIN_PX  16.0f
#define CLIP_GIF_CROP_GRAB_PX 6.0f

/* The glyph in the square of side w at a, in colour col. Drawn with the draw
 * list, so it needs no art and follows the text colour and the UI scale. */
static inline void clipGifDrawGlyph(ImDrawList *dl, int glyph, ImVec2 a,
                                    float w, ImU32 col) {
    if (glyph == CLIP_GIF_GLYPH_CAMERA) {
        /* Two film reels side by side over the body, and the lens on the
         * right of the body. */
        const float reelR = w * 0.16f;
        const float reelY = a.y + reelR + w * 0.04f;
        dl->AddCircleFilled(ImVec2(a.x + w * 0.18f, reelY), reelR, col);
        dl->AddCircleFilled(ImVec2(a.x + w * 0.52f, reelY), reelR, col);
        const float bodyTop = a.y + w * 0.40f;
        const float bodyBot = a.y + w * 0.92f;
        const float bodyR   = a.x + w * 0.68f;
        dl->AddRectFilled(ImVec2(a.x, bodyTop), ImVec2(bodyR, bodyBot), col,
                          w * 0.06f);
        /* The lens: a short cone that opens out to the right. */
        const float midY = (bodyTop + bodyBot) * 0.5f;
        dl->AddQuadFilled(ImVec2(bodyR, midY - w * 0.08f),
                          ImVec2(a.x + w, midY - w * 0.24f),
                          ImVec2(a.x + w, midY + w * 0.24f),
                          ImVec2(bodyR, midY + w * 0.08f), col);
    } else if (glyph == CLIP_GIF_GLYPH_EYE) {
        /* Two lids that meet at the corners, and the pupil between them. */
        const float midY = a.y + w * 0.50f;
        const ImVec2 l(a.x, midY);
        const ImVec2 r(a.x + w, midY);
        dl->AddBezierQuadratic(l, ImVec2(a.x + w * 0.50f, midY - w * 0.62f),
                               r, col, 1.5f);
        dl->AddBezierQuadratic(l, ImVec2(a.x + w * 0.50f, midY + w * 0.62f),
                               r, col, 1.5f);
        dl->AddCircleFilled(ImVec2(a.x + w * 0.50f, midY), w * 0.17f, col);
    } else if (glyph == CLIP_GIF_GLYPH_CLOSE) {
        /* Two strokes corner to corner. */
        const float i = w * 0.18f;
        const float t = w * 0.14f > 1.5f ? w * 0.14f : 1.5f;
        dl->AddLine(ImVec2(a.x + i, a.y + i), ImVec2(a.x + w - i, a.y + w - i),
                    col, t);
        dl->AddLine(ImVec2(a.x + w - i, a.y + i), ImVec2(a.x + i, a.y + w - i),
                    col, t);
    } else {
        /* CLIP_GIF_GLYPH_STOP: a filled square. */
        dl->AddRectFilled(ImVec2(a.x + w * 0.16f, a.y + w * 0.16f),
                          ImVec2(a.x + w * 0.84f, a.y + w * 0.84f), col,
                          w * 0.06f);
    }
}

/* While on is true, a toggle that is on looks pressed: filled orange, as
 * the lobby's Skip Map button is while the player's vote is in, so it
 * reads apart from an off button in every theme, under the mouse too.
 * Pair each call with clipGifPopLatched(on) after the button. */
static inline void clipGifPushLatched(bool on) {
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.4f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(0.9f, 0.5f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(0.7f, 0.3f, 0.05f, 1.0f));
    }
}

/* Takes off what clipGifPushLatched(on) put on. */
static inline void clipGifPopLatched(bool on) {
    if (on) {
        ImGui::PopStyleColor(3);
    }
}

/* A button with a glyph that stays pressed while on is true. size 0 is the
 * game's square, a line of text high with the frame padding round it; any
 * other size is a button of that size with the glyph in its middle, a little
 * over half its height (as the Log Viewer's own icon buttons have it). tip,
 * when not NULL, is its tooltip. Returns true when pressed. */
static inline bool clipGifToggleButton(const char *id, bool on, int glyph,
                                       const char *tip,
                                       ImVec2 size = ImVec2(0.0f, 0.0f)) {
    const ImGuiStyle &sty = ImGui::GetStyle();
    const float lineH = ImGui::GetTextLineHeight();
    const bool  own   = (size.x > 0.0f && size.y > 0.0f);
    if (!own) {
        size = ImVec2(lineH + sty.FramePadding.x * 2.0f,
                      lineH + sty.FramePadding.y * 2.0f);
    }
    clipGifPushLatched(on);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::Button(id, size);
    clipGifPopLatched(on);
    ImVec2 a;
    float w;
    if (own) {
        w = floorf(size.y * 0.56f);
        if (w < 4.0f) w = 4.0f;
        a = ImVec2(floorf(p.x + (size.x - w) * 0.5f),
                   floorf(p.y + (size.y - w) * 0.5f));
    } else {
        w = lineH - 2.0f;
        a = ImVec2(p.x + sty.FramePadding.x + 1.0f,
                   p.y + sty.FramePadding.y + 1.0f);
    }
    clipGifDrawGlyph(ImGui::GetWindowDrawList(), glyph, a, w,
                     ImGui::GetColorU32(ImGuiCol_Text));
    if (tip != NULL) {
        imguiHelpTooltip(tip);
    }
    return clicked;
}

/* The GIF button's caption where it has no picture: the format's name, not
 * a word to translate. Also the title of the capture's progress window and
 * of the message boxes that say where a GIF went. */
static const char *const CLIP_GIF_BUTTON_TITLE = "GIF";

/* The Save clip button: picture where the caller has that icon loaded, else
 * the format's name. compact takes the vertical half of the frame padding
 * off, for a row one line of text high. (Not "small": the Windows RPC
 * headers define that as a type.) The tooltip is the Save clip text.
 * Returns true when pressed. */
static inline bool clipGifButton(const char *id, ImTextureID picture,
                                 bool compact) {
    const float lineH = ImGui::GetTextLineHeight();
    bool clicked;
    if (picture) {
        if (compact) {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                ImVec2(ImGui::GetStyle().FramePadding.x, 0.0f));
        }
        clicked = ImGui::ImageButton(id, picture, ImVec2(lineH, lineH),
                                     ImVec2(0, 0), ImVec2(1, 1),
                                     ImVec4(0, 0, 0, 0),
                                     ImGui::GetStyleColorVec4(ImGuiCol_Text));
        if (compact) {
            ImGui::PopStyleVar();
        }
    } else {
        /* id carries its own ## part, so the caption has the same hidden id
         * the picture's button has. */
        char label[48];
        snprintf(label, sizeof(label), "%s%s", CLIP_GIF_BUTTON_TITLE, id);
        clicked = compact ? ImGui::SmallButton(label) : ImGui::Button(label);
    }
    imguiHelpTooltip(langGetText(STR_CLIPGIF_SAVE));
    return clicked;
}

/* The width clipGifButton takes, for a caller that places it by hand. */
static inline float clipGifButtonWidth(bool havePicture) {
    return (havePicture ? ImGui::GetTextLineHeight()
                        : ImGui::CalcTextSize(CLIP_GIF_BUTTON_TITLE).x) +
           ImGui::GetStyle().FramePadding.x * 2.0f;
}

/* The Preview clip button: an eye glyph, or a stop glyph that stays pressed
 * while the preview plays (previewing). compact is as for clipGifButton.
 * Returns true when pressed. */
static inline bool clipGifPreviewButton(const char *id, bool previewing,
                                        bool compact) {
    if (compact) {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(ImGui::GetStyle().FramePadding.x, 0.0f));
    }
    bool clicked = clipGifToggleButton(
        id, previewing, previewing ? CLIP_GIF_GLYPH_STOP : CLIP_GIF_GLYPH_EYE,
        langGetText(previewing ? STR_CLIPGIF_PREVIEW_STOP
                               : STR_CLIPGIF_PREVIEW));
    if (compact) {
        ImGui::PopStyleVar();
    }
    return clicked;
}

/* The width clipGifPreviewButton takes: a square a line of text high, with
 * the frame padding round it. */
static inline float clipGifPreviewButtonWidth(void) {
    return ImGui::GetTextLineHeight() +
           ImGui::GetStyle().FramePadding.x * 2.0f;
}

/* The GIF length field: the length in seconds, the same length the span's
 * handles on the zoomed timeline set. It shows the length the GIF will have,
 * clamped to the time left after curMs (the span's start); what is typed is
 * kept as asked for, inside the
 * floor and the cap, and clamped again when it is used. ImGui keeps its own
 * text while the field is typed into, so the value written into it each
 * frame does not fight the typing. Disabled while locked. */
static inline void clipGifLenField(const char *id, uint32_t curMs,
                                   uint32_t totalMs, uint32_t *lenMs,
                                   bool locked) {
    float secs = clipGifLenToSeconds(clipGifLenClampMs(*lenMs, curMs, totalMs));
    ImGui::BeginDisabled(locked);
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("00.0").x +
                            ImGui::GetStyle().FramePadding.x * 2.0f);
    if (ImGui::InputFloat(id, &secs, 0.0f, 0.0f, "%.1f")) {
        /* The floor and the cap only: the time left is applied where the
         * length is used. */
        *lenMs = clipGifLenFromSeconds(secs, 0u, 0u);
    }
    imguiHelpTooltip(langGetText(STR_CLIPGIF_LENGTH_TIP));
    ImGui::EndDisabled();
}

/* The colour of the GIF's span, on the zoomed timeline and the main one: a
 * warm amber that reads apart from the slider's own colours in each theme. */
static inline ImU32 clipGifSpanColour(float alpha) {
    return IM_COL32(240, 170, 40, (int)(alpha * 255.0f + 0.5f));
}

/* The x range a seek slider's grab centre moves over, as ImGui lays out a
 * float slider in the frame barMin..barMax: 2 px in from each end of the
 * frame and then half the grab, so a time sits at the same x as the grab
 * would. */
static inline void clipGifSliderXRange(ImVec2 barMin, ImVec2 barMax,
                                       float *x0, float *x1) {
    const float grabPad = 2.0f;
    float grabW = ImGui::GetStyle().GrabMinSize;
    const float slideW = (barMax.x - barMin.x) - grabPad * 2.0f;
    if (grabW > slideW) grabW = slideW;
    *x0 = barMin.x + grabPad + grabW * 0.5f;
    *x1 = barMax.x - grabPad - grabW * 0.5f;
}

/* The GIF on the main timeline: a faint tint over the part the zoomed
 * timeline shows, and the span in the span's colour over it. Drawn only:
 * a press on the main timeline is the slider's, and the screen then puts the
 * span round the new time (clipGifSpanCentre). barMin/barMax are the frame
 * of the seek slider, which maps 0..totalMs on to it. Dimmer while locked
 * (a GIF is being made). Nothing before the span has a place. */
static inline void clipGifSpanTint(ImVec2 barMin, ImVec2 barMax,
                                   const ClipGifMode *mode, uint32_t totalMs,
                                   bool locked) {
    if (mode == NULL || !mode->spanSet || totalMs == 0 ||
        barMax.x <= barMin.x) {
        return;
    }
    float x0, x1;
    clipGifSliderXRange(barMin, barMax, &x0, &x1);
    const ClipGifRange span = clipGifSpanRange(mode, totalMs);
    const float zs = clipGifTimelineX(mode->zoom.startMs, totalMs, x0, x1);
    const float ze = clipGifTimelineX(mode->zoom.endMs, totalMs, x0, x1);
    float xs = clipGifTimelineX(span.startMs, totalMs, x0, x1);
    float xe = clipGifTimelineX(span.endMs, totalMs, x0, x1);
    /* At least 2 px, so a short GIF on a long round still shows. */
    if (xe - xs < 2.0f) {
        const float mid = (xs + xe) * 0.5f;
        xs = mid - 1.0f;
        xe = mid + 1.0f;
    }
    const float alpha = locked ? 0.45f : 1.0f;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(zs, barMin.y + 1.0f), ImVec2(ze, barMax.y - 1.0f),
                      ImGui::GetColorU32(ImGuiCol_Text, 0.10f * alpha));
    dl->AddRectFilled(ImVec2(xs, barMin.y + 1.0f), ImVec2(xe, barMax.y - 1.0f),
                      clipGifSpanColour(0.45f * alpha));
}

/* What the player did on the zoomed timeline this frame. The span's handles
 * are handled inside; these are the presses that move the playhead. */
typedef struct ClipGifZoomPressed {
    bool     held;    /* the timeline (not a handle) is held */
    bool     seek;    /* held, or let go this frame: the playhead goes to
                       * seekMs */
    uint32_t seekMs;
} ClipGifZoomPressed;

/* One handle of the span on the zoomed timeline: an item of its own on the
 * box hit, so it moves no cursor and takes no room. Mouse only, out of
 * keyboard and controller navigation, where it would read a mouse position
 * that is not there; the length field is how those set the length. */
static inline void clipGifSpanHandle(const ImRect &hit, ImGuiID hid,
                                     bool *hovered, bool *held) {
    *hovered = false;
    *held = false;
    if (ImGui::ItemAdd(hit, hid, nullptr, ImGuiItemFlags_NoNav)) {
        ImGui::ButtonBehavior(hit, hid, hovered, held,
                              ImGuiButtonFlags_NoNavFocus);
        if (*hovered || *held) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        if (!*held) {
            imguiHelpTooltip(langGetText(STR_CLIPGIF_LENGTH_TIP));
        }
    }
}

/* The handle of a span edge at x, from top to bot: a line with a notch at
 * the top, so it reads as a handle. */
static inline void clipGifDrawSpanHandle(ImDrawList *dl, float x, float top,
                                         float bot, bool hot, float alpha) {
    const ImU32 col = hot ? ImGui::GetColorU32(ImGuiCol_Text, alpha)
                          : clipGifSpanColour(alpha);
    const float lineW = hot ? 3.0f : 2.0f;
    dl->AddRectFilled(ImVec2(x - lineW * 0.5f, top),
                      ImVec2(x + lineW * 0.5f, bot), col);
    const float tri = ImGui::GetTextLineHeight() * 0.25f;
    dl->AddTriangleFilled(ImVec2(x - tri, top), ImVec2(x + tri, top),
                          ImVec2(x, top + tri), col);
}

/* The zoomed timeline of recording mode, on a line of its own at the cursor,
 * width wide (0 or less: the rest of the line) and a frame high. It shows
 * mode->zoom: the span in the span's colour with a handle at each end, the
 * playhead, and the times at its two ends. curMs and totalMs are the
 * replay's time now and its length.
 *
 * A handle dragged moves that end of the span (clipGifSpanDragEdge); held at
 * the end of the timeline it makes the timeline grow on that side
 * (clipGifZoomGrow). While no handle is held the timeline fits the span
 * (clipGifSpanRefit), which also puts the span at the playhead the first
 * time after recording mode turns on. A press or a drag anywhere else gives
 * the time under the mouse back, for the screen to move the playhead to;
 * the span does not move.
 *
 * spanLocked holds the handles still (a GIF is being made or the preview
 * plays); locked (a GIF is being made) holds the whole timeline still and
 * draws it dimmer. */
static inline ClipGifZoomPressed clipGifZoomBar(const char *id, float width,
                                                ClipGifMode *mode,
                                                uint32_t curMs,
                                                uint32_t totalMs,
                                                bool spanLocked, bool locked) {
    ClipGifZoomPressed r = { false, false, 0u };
    const ImGuiStyle &sty = ImGui::GetStyle();
    const float h = ImGui::GetFrameHeight();
    if (width <= 0.0f) {
        width = ImGui::GetContentRegionAvail().x;
    }
    const float hitW = ImGui::GetTextLineHeight() * 0.6f;
    if (mode == NULL || totalMs == 0 || width < hitW * 4.0f) {
        ImGui::Dummy(ImVec2(width > 1.0f ? width : 1.0f, h));
        return r;
    }
    ImGui::PushID(id);
    const ImGuiID startId = ImGui::GetID("##start");
    const ImGuiID endId   = ImGui::GetID("##end");
    const ImGuiID active  = ImGui::GetActiveID();
    const bool edgeHeld = !spanLocked && (active == startId || active == endId);
    if (!edgeHeld) {
        clipGifSpanRefit(mode, curMs, totalMs);
    }

    /* In from each end by half a handle, so a handle at an end of the
     * timeline can still be taken hold of. */
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 q(p.x + width, p.y + h);
    const float x0 = p.x + hitW * 0.5f;
    const float x1 = q.x - hitW * 0.5f;
    const ImGuiIO &io = ImGui::GetIO();

    /* The timeline itself: a press on it moves the playhead. The handles are
     * submitted after it and win the hover where the two meet. */
    bool barHover = false, barHeld = false, barLetGo = false;
    if (locked) {
        ImGui::Dummy(ImVec2(width, h));
    } else {
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##bar", ImVec2(width, h));
        barHover = ImGui::IsItemHovered();
        barHeld  = ImGui::IsItemActive();
        barLetGo = ImGui::IsItemDeactivated();
        if (!barHeld) {
            imguiHelpTooltip(langGetText(STR_CLIPGIF_ZOOM_TIP));
        }
    }

    ClipGifRange span = clipGifSpanRange(mode, totalMs);
    float xs = clipGifRangeX(span.startMs, mode->zoom, x0, x1);
    float xe = clipGifRangeX(span.endMs, mode->zoom, x0, x1);
    bool sHot = false, sHeld = false, eHot = false, eHeld = false;
    if (!spanLocked) {
        /* The two boxes meet at the middle of the span and do not cross,
         * so a short span still has both. */
        const float mid = (xs + xe) * 0.5f;
        const ImRect sHit(ImVec2(xs - hitW * 0.5f, p.y),
                          ImVec2(fminf(xs + hitW * 0.5f, mid), q.y));
        const ImRect eHit(ImVec2(fmaxf(xe - hitW * 0.5f, mid), p.y),
                          ImVec2(xe + hitW * 0.5f, q.y));
        clipGifSpanHandle(sHit, startId, &sHot, &sHeld);
        clipGifSpanHandle(eHit, endId, &eHot, &eHeld);
        const int edge = sHeld ? CLIP_GIF_EDGE_START
                       : eHeld ? CLIP_GIF_EDGE_END : 0;
        if (edge != 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const float mx = io.MousePos.x;
            clipGifSpanDragEdge(mode, edge,
                                clipGifRangeTime(mx, mode->zoom, x0, x1),
                                totalMs);
            /* Held at the end of the timeline: it grows on that side, by
             * this frame's share of CLIP_GIF_ZOOM_GROW_MS_PER_S. */
            if ((edge == CLIP_GIF_EDGE_END && mx >= x1) ||
                (edge == CLIP_GIF_EDGE_START && mx <= x0)) {
                uint32_t step = (uint32_t)(io.DeltaTime *
                                           (float)CLIP_GIF_ZOOM_GROW_MS_PER_S +
                                           0.5f);
                if (step < 1u) step = 1u;
                clipGifZoomGrow(&mode->zoom, clipGifSpanRange(mode, totalMs),
                                edge, step, totalMs);
            }
            span = clipGifSpanRange(mode, totalMs);
            xs = clipGifRangeX(span.startMs, mode->zoom, x0, x1);
            xe = clipGifRangeX(span.endMs, mode->zoom, x0, x1);
        }
    }
    if (barHeld || barLetGo) {
        r.held   = barHeld;
        r.seek   = true;
        r.seekMs = clipGifRangeTime(io.MousePos.x, mode->zoom, x0, x1);
    }

    /* Drawn over the frame: the span, the playhead, the handles, and the
     * times at the two ends. */
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float alpha = locked ? 0.45f : 1.0f;
    dl->AddRectFilled(p, q,
                      ImGui::GetColorU32((barHover || barHeld)
                                             ? ImGuiCol_FrameBgHovered
                                             : ImGuiCol_FrameBg),
                      sty.FrameRounding);
    dl->AddRectFilled(ImVec2(xs, p.y + 1.0f), ImVec2(xe, q.y - 1.0f),
                      clipGifSpanColour(0.55f * alpha));
    if (curMs >= mode->zoom.startMs && curMs <= mode->zoom.endMs) {
        const float xp = clipGifRangeX(curMs, mode->zoom, x0, x1);
        dl->AddRectFilled(ImVec2(xp - 1.0f, p.y), ImVec2(xp + 1.0f, q.y),
                          ImGui::GetColorU32(ImGuiCol_Text, alpha));
    }
    clipGifDrawSpanHandle(dl, xs, p.y, q.y, sHot || sHeld, alpha);
    clipGifDrawSpanHandle(dl, xe, p.y, q.y, eHot || eHeld, alpha);

    char a[16], b[16];
    const unsigned as = (unsigned)(mode->zoom.startMs / 1000u);
    const unsigned bs = (unsigned)(mode->zoom.endMs / 1000u);
    snprintf(a, sizeof(a), "%02u:%02u", as / 60u, as % 60u);
    snprintf(b, sizeof(b), "%02u:%02u", bs / 60u, bs % 60u);
    const float aw = ImGui::CalcTextSize(a).x;
    const float bw = ImGui::CalcTextSize(b).x;
    const float pad = sty.FramePadding.x;
    if (aw + bw + pad * 4.0f < width) {
        const float ty = p.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
        const ImU32 tc = ImGui::GetColorU32(ImGuiCol_TextDisabled, alpha);
        dl->AddText(ImVec2(p.x + pad, ty), tc, a);
        dl->AddText(ImVec2(q.x - pad - bw, ty), tc, b);
    }
    ImGui::PopID();
    return r;
}

/* Which part of the crop frame a press took hold of, kept until the button
 * is let go, so a drag that goes off the edge keeps moving the edge it
 * started on. */
typedef struct {
    int  grab;      /* CLIP_GIF_GRAB_* bits; 0 when not held */
    bool dragging;
} ClipGifCropDrag;

/* The crop frame of mode over a view drawn at imgMin, imgSize, that shows
 * viewPxW x viewPxH pixels of the render target: everything outside the
 * frame is shaded, and the frame is outlined. Until the player moves the
 * frame it is the default for those pixels (clipGifModeFitFrame), the same
 * frame the GIF takes. Its edges, corners and inside can be dragged to
 * change mode->frame, which is stored back as fractions of the view, and a
 * drag sets mode->frameSet. Returns true when the frame has the mouse this frame, so the
 * caller keeps its own drag (a pan) out of it.
 *
 * asItem true puts an invisible button over the frame, for a view that is an
 * ImGui item in a window (the game's replay): ImGui then gives the press to
 * the frame and not to the view's own button. Submit it after that button,
 * which must be marked allow-overlap. asItem false is for a view that is not
 * in a window (the Log Viewer's map, drawn on the background draw list): the
 * frame then takes a press only where no ImGui window is under the mouse.
 *
 * A pan that already holds the mouse (panHoldsMouse) keeps it: who has the
 * mouse is decided at the press, so a pan across the frame is not taken over
 * half way. While locked (a GIF is being made) the frame is dimmer and takes
 * no mouse. The wheel is never taken: a zoom works over the frame too. */
static inline bool clipGifCropFrame(const char *id, ImDrawList *dl,
                                    ImVec2 imgMin, ImVec2 imgSize,
                                    ClipGifMode *mode, int viewPxW,
                                    int viewPxH, ClipGifCropDrag *drag,
                                    bool locked, bool panHoldsMouse,
                                    bool asItem) {
    if (mode == NULL || imgSize.x < 1.0f || imgSize.y < 1.0f) {
        return false;
    }
    clipGifModeFitFrame(mode, viewPxW, viewPxH);
    ClipGifFrame *f = &mode->frame;
    const ClipGifBox image = { imgMin.x, imgMin.y, imgMin.x + imgSize.x,
                               imgMin.y + imgSize.y };
    ClipGifBox box = clipGifFrameToBox(f, &image);

    /* Shade all the GIF leaves out, so the part it keeps reads at a glance.
     * Dimmer while a GIF is being made: the frame shows what is taken and
     * does not answer a drag. */
    const ImU32 line  = locked ? IM_COL32(255, 255, 255, 110)
                               : IM_COL32(255, 255, 255, 230);
    const ImU32 shade = IM_COL32(0, 0, 0, 90);
    dl->AddRectFilled(ImVec2(image.x0, image.y0), ImVec2(image.x1, box.y0),
                      shade);
    dl->AddRectFilled(ImVec2(image.x0, box.y1), ImVec2(image.x1, image.y1),
                      shade);
    dl->AddRectFilled(ImVec2(image.x0, box.y0), ImVec2(box.x0, box.y1), shade);
    dl->AddRectFilled(ImVec2(box.x1, box.y0), ImVec2(image.x1, box.y1), shade);
    dl->AddRect(ImVec2(box.x0, box.y0), ImVec2(box.x1, box.y1),
                IM_COL32(0, 0, 0, 160), 0.0f, 0, 3.0f);
    dl->AddRect(ImVec2(box.x0, box.y0), ImVec2(box.x1, box.y1), line, 0.0f, 0,
                1.0f);

    if (locked) {
        drag->dragging = false;
        drag->grab = 0;
        return false;
    }
    if (panHoldsMouse && !drag->dragging) {
        return false;
    }

    const float g = CLIP_GIF_CROP_GRAB_PX;
    const ImGuiIO &io = ImGui::GetIO();
    /* Without an item, only where no ImGui window is under the mouse: the
     * panels over the map keep their own clicks. As an item, only where the
     * window it is in is the one under the mouse: a window over the view
     * (the GIF window) keeps its own clicks, and the frame's cursor does
     * not show through it. */
    const bool mouseFree =
        asItem ? ImGui::IsWindowHovered(
                     ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
               : !io.WantCaptureMouse;
    const int hot = mouseFree ? clipGifFrameHit(&box, io.MousePos.x,
                                                io.MousePos.y, g)
                              : 0;

    bool takesMouse = false;
    if (asItem) {
        if (hot != 0 || drag->dragging) {
            ImGui::SetCursorScreenPos(ImVec2(box.x0 - g, box.y0 - g));
            ImGui::InvisibleButton(id, ImVec2((box.x1 - box.x0) + g * 2.0f,
                                              (box.y1 - box.y0) + g * 2.0f));
            takesMouse = ImGui::IsItemHovered() || ImGui::IsItemActive() ||
                         drag->dragging;
            if (ImGui::IsItemActivated()) {
                drag->grab = hot;
                drag->dragging = true;
            }
        }
    } else {
        takesMouse = (hot != 0) || drag->dragging;
        if (!drag->dragging && hot != 0 &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            drag->grab = hot;
            drag->dragging = true;
        }
    }

    const int shape = drag->dragging ? drag->grab : hot;
    if (shape != 0) {
        ImGuiMouseCursor cur = ImGuiMouseCursor_ResizeAll;
        switch (shape & 15) {
            case CLIP_GIF_GRAB_LEFT:
            case CLIP_GIF_GRAB_RIGHT:
                cur = ImGuiMouseCursor_ResizeEW;
                break;
            case CLIP_GIF_GRAB_TOP:
            case CLIP_GIF_GRAB_BOTTOM:
                cur = ImGuiMouseCursor_ResizeNS;
                break;
            case CLIP_GIF_GRAB_LEFT | CLIP_GIF_GRAB_TOP:
            case CLIP_GIF_GRAB_RIGHT | CLIP_GIF_GRAB_BOTTOM:
                cur = ImGuiMouseCursor_ResizeNWSE;
                break;
            case CLIP_GIF_GRAB_RIGHT | CLIP_GIF_GRAB_TOP:
            case CLIP_GIF_GRAB_LEFT | CLIP_GIF_GRAB_BOTTOM:
                cur = ImGuiMouseCursor_ResizeNESW;
                break;
            default:
                break;  /* the inside: a move */
        }
        ImGui::SetMouseCursor(cur);
    }

    if (drag->dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) {
            clipGifFrameDrag(&box, drag->grab, io.MouseDelta.x,
                             io.MouseDelta.y, &image, CLIP_GIF_CROP_MIN_PX);
            clipGifFrameFromBox(f, &box, &image);
            mode->frameSet = true;
        }
    } else if (drag->dragging) {
        drag->dragging = false;
        drag->grab = 0;
    }
    return takesMouse;
}

/* What was pressed on the row of GIF controls this frame. The row handles
 * the length itself; the screen acts on these. */
typedef struct ClipGifToolsPressed {
    bool preview;  /* Preview clip: start or stop the preview */
    bool save;     /* Save clip: make the GIF */
    bool cancel;   /* Cancel: recording mode is off now (done in the row) */
    bool gallery;  /* open the gallery (only where it is built) */
} ClipGifToolsPressed;

/* The width of the row clipGifToolsRow draws, for a screen that centres
 * it. */
static inline float clipGifToolsRowWidth(bool havePicture) {
    const ImGuiStyle &sty = ImGui::GetStyle();
    const float lenW = ImGui::CalcTextSize("00.0").x + sty.FramePadding.x * 2.0f;
    float w = clipGifPreviewButtonWidth() + clipGifButtonWidth(havePicture) +
              clipGifPreviewButtonWidth() + lenW + sty.ItemSpacing.x * 3.0f;
#if CLIP_GIF_GALLERY_BUILT
    const float square = ImGui::GetTextLineHeight() + sty.FramePadding.x * 2.0f;
    w += square + sty.ItemSpacing.x;
#endif
    return w;
}

/* The row of GIF controls that recording mode shows, the same in the game's
 * replay and the Log Viewer: Preview clip, Save clip, Cancel, the length in
 * seconds, and the gallery button (where the gallery is built). The
 * first control goes where the cursor is; the caller puts a SameLine before
 * the call when the row goes on after other controls.
 *
 * mode is the screen's recording mode (the length is changed here, and
 * Cancel turns it off here: the GIF controls go, and no GIF is made), play
 * its preview, curMs and totalMs the replay's time now and its length.
 * Preview, Save and Cancel are disabled while capturing (the capture's own
 * window has its Cancel), and Preview and Save when canMake is false (no GIF
 * can be made now). The length does not change while a GIF is made or the
 * preview plays, as that GIF or that preview is of the length it started
 * with. */
static inline ClipGifToolsPressed clipGifToolsRow(const char *id,
                                                  ClipGifMode *mode,
                                                  const ClipGifPlay *play,
                                                  bool capturing, bool canMake,
                                                  ImTextureID picture,
                                                  uint32_t curMs,
                                                  uint32_t totalMs) {
    ClipGifToolsPressed r = { false, false, false, false };
    const bool previewing = (play != NULL && play->active);
    ImGui::PushID(id);

    ImGui::BeginDisabled(!canMake || capturing);
    r.preview = clipGifPreviewButton("##preview", previewing, false);
    ImGui::SameLine();
    r.save = clipGifButton("##save", picture, false);
    ImGui::EndDisabled();

    /* Out of recording mode with no GIF: the same as the movie camera
     * button, here by the Save clip button where it is easy to find. */
    ImGui::SameLine();
    ImGui::BeginDisabled(capturing);
    r.cancel = clipGifToggleButton("##cancel", false, CLIP_GIF_GLYPH_CLOSE,
                                   langGetText(STR_CLIPGIF_CANCEL));
    ImGui::EndDisabled();
    if (r.cancel) {
        clipGifModeToggle(mode, capturing);
    }

    /* How long the clip runs, from the span's start: the span's handles on
     * the zoomed timeline set the same length. */
    uint32_t spanStart = 0;
    clipGifSpanClip(mode, clipGifPlayBandStart(play, curMs), totalMs,
                    &spanStart, NULL);
    ImGui::SameLine();
    clipGifLenField("##len", spanStart, totalMs, &mode->lenMs,
                    capturing || previewing);

#if CLIP_GIF_GALLERY_BUILT
    /* The GIFs made so far. */
    ImGui::SameLine();
    r.gallery = clipGifGalleryButton("##gallery");
#endif

    ImGui::PopID();
    return r;
}

#endif /* CLIP_GIF_CONTROLS_H */
