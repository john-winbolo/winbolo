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
 *                GIF button, the length field, the length band
 *                and its end marker on the timeline, the crop
 *                button, and the crop frame over the view.
 *
 *                The rules behind them, with no ImGui in them,
 *                are in clip_gif_core.h. Header only, so a
 *                target that builds one of the two needs no
 *                other file for it.
 *********************************************************/

#ifndef CLIP_GIF_CONTROLS_H
#define CLIP_GIF_CONTROLS_H

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
    CLIP_GIF_GLYPH_CROP,        /* a square frame with heavy corners: crop */
    CLIP_GIF_GLYPH_PLAY,        /* a triangle pointing right: preview */
    CLIP_GIF_GLYPH_STOP         /* a filled square: stop the preview */
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
    } else if (glyph == CLIP_GIF_GLYPH_PLAY) {
        dl->AddTriangleFilled(ImVec2(a.x + w * 0.18f, a.y + w * 0.08f),
                              ImVec2(a.x + w * 0.92f, a.y + w * 0.50f),
                              ImVec2(a.x + w * 0.18f, a.y + w * 0.92f), col);
    } else if (glyph == CLIP_GIF_GLYPH_STOP) {
        dl->AddRectFilled(ImVec2(a.x + w * 0.16f, a.y + w * 0.16f),
                          ImVec2(a.x + w * 0.84f, a.y + w * 0.84f), col,
                          w * 0.06f);
    } else {
        /* A square outline with heavier corners, so it reads as a crop frame
         * and not as an empty box. */
        ImVec2 b(a.x + w, a.y + w);
        dl->AddRect(a, b, col, 0.0f, 0, 1.0f);
        const float tick = w * 0.32f;
        dl->AddLine(ImVec2(a.x, a.y), ImVec2(a.x + tick, a.y), col, 2.0f);
        dl->AddLine(ImVec2(a.x, a.y), ImVec2(a.x, a.y + tick), col, 2.0f);
        dl->AddLine(ImVec2(b.x, b.y), ImVec2(b.x - tick, b.y), col, 2.0f);
        dl->AddLine(ImVec2(b.x, b.y), ImVec2(b.x, b.y - tick), col, 2.0f);
    }
}

/* While on is true, the pressed colour for the button's normal, hovered and
 * active colours, so a toggle that is on stays pressed in, under the mouse
 * too. Pair each call with clipGifPopLatched(on) after the button. */
static inline void clipGifPushLatched(bool on) {
    if (on) {
        const ImVec4 down = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
        ImGui::PushStyleColor(ImGuiCol_Button, down);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, down);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, down);
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

/* The Preview clip button: a play glyph, or a stop glyph that stays pressed
 * while the preview plays (previewing). compact is as for clipGifButton.
 * Returns true when pressed. */
static inline bool clipGifPreviewButton(const char *id, bool previewing,
                                        bool compact) {
    if (compact) {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(ImGui::GetStyle().FramePadding.x, 0.0f));
    }
    bool clicked = clipGifToggleButton(
        id, previewing, previewing ? CLIP_GIF_GLYPH_STOP : CLIP_GIF_GLYPH_PLAY,
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

/* The GIF length field: the length in seconds, the same length the marker
 * on the timeline sets. It shows the length the GIF will have, clamped to
 * the time left after curMs; what is typed is kept as asked for, inside the
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

/* The GIF on the timeline: a bright band from startMs to where the GIF would
 * end, and a marker at its end that can be dragged to change *lenMs.
 * barMin/barMax are the frame of the seek slider, which maps 0..totalMs on
 * to it. Call it after the slider is submitted and its own state is read,
 * with the slider marked allow-overlap, so the marker is the newer item and
 * wins the hover where the two meet. The marker is added with ItemAdd on a
 * box of its own, so it moves no cursor and takes no room. While locked (a
 * GIF is being made) it is dimmer and does not answer a drag. */
static inline void clipGifLenMarker(const char *id, ImVec2 barMin,
                                    ImVec2 barMax, uint32_t startMs,
                                    uint32_t totalMs, uint32_t *lenMs,
                                    bool locked) {
    if (totalMs == 0 || barMax.x <= barMin.x) {
        return;
    }
    const ImGuiStyle &sty = ImGui::GetStyle();
    /* The x range the slider's grab centre moves over, as ImGui lays out a
     * float slider: 2 px in from each end of the frame and then half the
     * grab, so a time sits at the same x as the grab would. */
    const float grabPad = 2.0f;
    float grabW = sty.GrabMinSize;
    const float slideW = (barMax.x - barMin.x) - grabPad * 2.0f;
    if (grabW > slideW) grabW = slideW;
    const float x0 = barMin.x + grabPad + grabW * 0.5f;
    const float x1 = barMax.x - grabPad - grabW * 0.5f;

    const uint32_t len = clipGifLenClampMs(*lenMs, startMs, totalMs);
    const float xs = clipGifTimelineX(startMs, totalMs, x0, x1);
    const float xe = clipGifTimelineX(startMs + len, totalMs, x0, x1);

    /* The marker's hit box is wider than the line, so it is easy to take
     * hold of. */
    const float hitW = ImGui::GetTextLineHeight() * 0.6f;
    ImRect hit(ImVec2(xe - hitW * 0.5f, barMin.y),
               ImVec2(xe + hitW * 0.5f, barMax.y));
    bool hovered = false, held = false;
    if (!locked) {
        /* Mouse only: out of keyboard and controller navigation, where it
         * would read a mouse position that is not there. The length field is
         * how those set the length. */
        ImGuiID mid = ImGui::GetID(id);
        if (ImGui::ItemAdd(hit, mid, nullptr, ImGuiItemFlags_NoNav)) {
            ImGui::ButtonBehavior(hit, mid, &hovered, &held,
                                  ImGuiButtonFlags_NoNavFocus);
            if (held && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                *lenMs = clipGifLenFromTimelineX(ImGui::GetIO().MousePos.x,
                                                 startMs, totalMs, x0, x1);
            }
            if (hovered || held) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
            if (!held) {
                imguiHelpTooltip(langGetText(STR_CLIPGIF_LENGTH_TIP));
            }
        }
    }

    /* Over the slider's frame: the band lets the time label under it show
     * through. */
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float alpha = locked ? 0.45f : 1.0f;
    ImU32 band = ImGui::GetColorU32(ImGuiCol_SliderGrabActive, 0.30f * alpha);
    ImU32 mark = ImGui::GetColorU32((hovered || held) ? ImGuiCol_Text
                                                      : ImGuiCol_SliderGrabActive,
                                    alpha);
    dl->AddRectFilled(ImVec2(xs, barMin.y + 1.0f), ImVec2(xe, barMax.y - 1.0f),
                      band);
    const float lineW = (hovered || held) ? 3.0f : 2.0f;
    dl->AddRectFilled(ImVec2(xe - lineW * 0.5f, barMin.y),
                      ImVec2(xe + lineW * 0.5f, barMax.y), mark);
    /* A small notch on top, so the marker reads as a handle and not as a
     * line in the label. */
    const float tri = ImGui::GetTextLineHeight() * 0.25f;
    dl->AddTriangleFilled(ImVec2(xe - tri, barMin.y),
                          ImVec2(xe + tri, barMin.y),
                          ImVec2(xe, barMin.y + tri), mark);
}

/* Which part of the crop frame a press took hold of, kept until the button
 * is let go, so a drag that goes off the edge keeps moving the edge it
 * started on. */
typedef struct {
    int  grab;      /* CLIP_GIF_GRAB_* bits; 0 when not held */
    bool dragging;
} ClipGifCropDrag;

/* The crop frame over a view drawn at imgMin, imgSize: everything outside
 * the frame is shaded, and the frame is outlined. Its edges, corners and
 * inside can be dragged to change *f, which is stored back as fractions of
 * the view. Returns true when the frame has the mouse this frame, so the
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
                                    ClipGifFrame *f, ClipGifCropDrag *drag,
                                    bool locked, bool panHoldsMouse,
                                    bool asItem) {
    if (imgSize.x < 1.0f || imgSize.y < 1.0f) {
        return false;
    }
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
     * panels over the map keep their own clicks. */
    const bool mouseFree = asItem || !io.WantCaptureMouse;
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
        clipGifFrameDrag(&box, drag->grab, io.MouseDelta.x, io.MouseDelta.y,
                         &image, CLIP_GIF_CROP_MIN_PX);
        clipGifFrameFromBox(f, &box, &image);
    } else if (drag->dragging) {
        drag->dragging = false;
        drag->grab = 0;
    }
    return takesMouse;
}

/* What was pressed on the row of GIF controls this frame. The row handles
 * the length and the crop button itself; the screen acts on these. */
typedef struct ClipGifToolsPressed {
    bool preview;  /* Preview clip: start or stop the preview */
    bool save;     /* Save clip: make the GIF */
    bool gallery;  /* open the gallery (only where it is built) */
} ClipGifToolsPressed;

/* The width of the row clipGifToolsRow draws, for a screen that centres
 * it. */
static inline float clipGifToolsRowWidth(bool havePicture) {
    const ImGuiStyle &sty = ImGui::GetStyle();
    const float square = ImGui::GetTextLineHeight() + sty.FramePadding.x * 2.0f;
    const float lenW = ImGui::CalcTextSize("00.0").x + sty.FramePadding.x * 2.0f;
    float w = clipGifPreviewButtonWidth() + clipGifButtonWidth(havePicture) +
              lenW + square + sty.ItemSpacing.x * 3.0f;
#if CLIP_GIF_GALLERY_BUILT
    w += square + sty.ItemSpacing.x;
#endif
    return w;
}

/* The row of GIF controls that recording mode shows, the same in the game's
 * replay and the Log Viewer: Preview clip, Save clip, the length in seconds,
 * the gallery button (where the gallery is built) and the crop button. The
 * first control goes where the cursor is; the caller puts a SameLine before
 * the call when the row goes on after other controls.
 *
 * mode is the screen's recording mode (the length and the crop are changed
 * here), play its preview, curMs and totalMs the replay's time now and its
 * length. Preview and Save are disabled while capturing, and when canMake is
 * false (no GIF can be made now). The length does not change while a GIF is
 * made or the preview plays, as that GIF or that preview is of the length it
 * started with. */
static inline ClipGifToolsPressed clipGifToolsRow(const char *id,
                                                  ClipGifMode *mode,
                                                  const ClipGifPlay *play,
                                                  bool capturing, bool canMake,
                                                  ImTextureID picture,
                                                  uint32_t curMs,
                                                  uint32_t totalMs) {
    ClipGifToolsPressed r = { false, false, false };
    const bool previewing = (play != NULL && play->active);
    ImGui::PushID(id);

    ImGui::BeginDisabled(!canMake || capturing);
    r.preview = clipGifPreviewButton("##preview", previewing, false);
    ImGui::SameLine();
    r.save = clipGifButton("##save", picture, false);
    ImGui::EndDisabled();

    /* How long the clip runs, from where it starts: the marker on the
     * timeline sets the same length. */
    ImGui::SameLine();
    clipGifLenField("##len", clipGifPlayBandStart(play, curMs), totalMs,
                    &mode->lenMs, capturing || previewing);

#if CLIP_GIF_GALLERY_BUILT
    /* The GIFs made so far. */
    ImGui::SameLine();
    r.gallery = clipGifGalleryButton("##gallery");
#endif

    /* The crop frame: with it on, the GIF takes the part of the view in the
     * frame. */
    ImGui::SameLine();
    if (clipGifToggleButton("##crop", mode->cropOn, CLIP_GIF_GLYPH_CROP,
                            langGetText(STR_CLIPGIF_CROP))) {
        clipGifModeToggleCrop(mode, capturing);
    }

    ImGui::PopID();
    return r;
}

#endif /* CLIP_GIF_CONTROLS_H */
