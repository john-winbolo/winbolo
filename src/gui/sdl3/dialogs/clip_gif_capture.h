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
 * Name:          clip_gif_capture.h
 * Purpose:       Making a clip GIF and previewing it, as the
 *                game's replay (lobby_clip_gif.cpp) and the Log
 *                Viewer (imgui_clip_gif.cpp) both do it: the
 *                Preview clip playback, the capture that steps
 *                the replay a GIF frame at a time and reads each
 *                frame back into the encoder, the progress window
 *                with Cancel, and where the finished GIF goes
 *                (the gallery on the desktop, Documents on iOS,
 *                the share sheet on Android).
 *
 *                The screen gives the replay it shows as a
 *                ClipGifReplay: its time, play and pause, seek,
 *                one GIF frame's step and the render target the
 *                frame is read from. The rules with no ImGui and
 *                no renderer in them are in clip_gif_core.h.
 *
 *                C++ only: the progress window is drawn with
 *                ImGui.
 *********************************************************/

#ifndef CLIP_GIF_CAPTURE_H
#define CLIP_GIF_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

#include <SDL3/SDL.h>

#include "clip_gif_core.h"   /* ClipGifPlay, ClipGifRect, ClipGifMeta */
#include "clip_gif_frame.h"  /* MsfGifState, clipGifEncodeFrame */

/* The replay a screen shows, as the capture and the preview use it. Times
 * are in ms from the start of the part of the log the screen shows (the
 * times its seek bar shows). user is given back to each call. */
typedef struct ClipGifReplay {
    void *user;
    /* The time now and the length. */
    void (*progress)(void *user, uint32_t *curMs, uint32_t *totalMs);
    bool (*isPlaying)(void *user);
    void (*play)(void *user);
    void (*pause)(void *user);
    void (*seek)(void *user, uint32_t ms);
    /* on true as a capture starts: the tank names go into the render target
     * (which is what is read back), and anything else the screen does only
     * while a GIF is made. on false as it ends: those go back. */
    void (*captureMode)(void *user, bool on);
    /* Moves the replay on by one GIF frame (CLIP_GIF_TICKS_PER_FRAME log
     * ticks) and draws it into the render target. Sets *ended when the
     * replay has no more to play. False when the frame cannot be made. */
    bool (*advance)(void *user, bool *ended);
    /* The renderer and the render target a frame is read from. */
    bool (*target)(void *user, SDL_Renderer **renderer, SDL_Texture **texture);
} ClipGifReplay;

/* A press or a drag on the zoomed timeline (clipGifZoomRow) that moves the
 * playhead: the replay is paused while the mouse is held, a seek forward is
 * made each frame and a seek back at most each CLIP_GIF_SCRUB_BACK_SECS (it
 * decodes the round again from a snapshot), the seek lands on the exact time
 * when the mouse is let go, and a replay that played goes on playing. */
#define CLIP_GIF_SCRUB_BACK_SECS 0.090

typedef struct ClipGifScrub {
    bool     on;
    bool     wasPlaying;
    uint32_t appliedMs;  /* where the last seek put the replay */
    double   backAt;     /* ImGui time of the last seek back */
} ClipGifScrub;

/* What one screen holds for its clips: the preview, the scrub of the zoomed
 * timeline, and the capture from the moment it starts to the moment its
 * bytes are handed on. */
typedef struct ClipGifSession {
    ClipGifPlay play;
    ClipGifScrub scrub;
    bool        active;
    bool        failed;
    bool        done;      /* all frames made: the next render ends it */
    MsfGifState enc;
    int         frame;
    int         total;
    SDL_Rect    crop;      /* in the render target, fixed for the capture */
    uint32_t    restoreMs; /* where the replay was before the capture */
    bool        restorePlaying;
    char        name[96];  /* <map>_<mmss>, the file's base name */
    ClipGifMeta meta;
} ClipGifSession;

/* Stops a capture (its bytes are dropped) and the preview, and puts the
 * session back to its start values. restoreTime false leaves the replay
 * where it is, for a screen that is about to open another log. */
void clipGifSessionReset(ClipGifSession *s, const ClipGifReplay *r,
                         bool restoreTime);

/* True while a GIF is being made. */
bool clipGifCaptureActive(const ClipGifSession *s);

/* ── Preview clip ────────────────────────────────────────────────── */

/* True while the preview plays. */
bool clipGifPreviewActive(const ClipGifSession *s);

/* Plays the clip of lenMs from the time the replay is at now, at the normal
 * speed. A preview already on starts again from now. Nothing while a GIF is
 * being made. False when it did not start. */
bool clipGifPreviewStart(ClipGifSession *s, const ClipGifReplay *r,
                         uint32_t lenMs);

/* Preview clip in recording mode: plays the span of mode (clipGifSpanRange)
 * from its start, wherever the playhead is. Before the span has a place
 * (spanSet false), from the playhead for mode->lenMs. */
bool clipGifPreviewSpan(ClipGifSession *s, const ClipGifReplay *r,
                        const ClipGifMode *mode);

/* Stops the preview, and the replay goes back to the start of the clip. */
void clipGifPreviewStop(ClipGifSession *s, const ClipGifReplay *r);

/* Once a frame. At the end of the clip the replay stops and goes back to
 * the start of the clip. A pause or a seek by the player ends the preview
 * where the replay is. keep false (recording mode is off) ends it there
 * too. */
void clipGifPreviewPoll(ClipGifSession *s, const ClipGifReplay *r, bool keep);

/* Where the clip's band on the timeline starts (clipGifPlayBandStart). */
uint32_t clipGifPreviewBandStart(const ClipGifSession *s, uint32_t curMs);

/* ── Save clip ───────────────────────────────────────────────────── */

/* The first half of a start. A preview that plays is stopped and the
 * replay goes back to the start of its clip, so the GIF is of that clip.
 * Then it keeps where the replay is and whether it plays, pauses it, and
 * turns on captureMode. The screen then moves the replay to the moment the
 * GIF starts at and works out the crop, and calls clipGifCaptureOpen. False,
 * with nothing changed, while a GIF is being made already. */
bool clipGifCaptureArm(ClipGifSession *s, const ClipGifReplay *r);

/* The second half: opens the encoder at the crop's size, for lenMs from
 * startMs, with the file name made from mapName and startMs and the
 * metadata meta (its length and size are filled here). When crop is NULL
 * or the encoder cannot open, the replay goes back to where the arm found
 * it and the result is false. */
bool clipGifCaptureOpen(ClipGifSession *s, const ClipGifReplay *r,
                        const ClipGifRect *crop, uint32_t startMs,
                        uint32_t lenMs, const char *mapName,
                        const ClipGifMeta *meta);

/* Makes this frame's share of the GIF: up to CLIP_GIF_FRAMES_PER_PASS
 * frames. A frame that cannot be made marks the capture failed, and the
 * next render ends it with no GIF. */
void clipGifCaptureStep(ClipGifSession *s, const ClipGifReplay *r);

/* The progress window with Cancel (and Esc), while a GIF is made, drawn
 * inside the screen's ImGui frame. When the last frame is made the replay
 * goes back to where it was, and the GIF is saved (clipGifSaveGif). s is
 * the screen's UI scale. */
void clipGifCaptureRender(ClipGifSession *s, const ClipGifReplay *r,
                          float uiScale);

/* ── The zoomed timeline ─────────────────────────────────────────── */

/* The zoomed timeline (clipGifZoomBar) on a line of its own, width wide (0
 * or less: the rest of the line), with the presses on it that move the
 * playhead made on r (ClipGifScrub). The span's handles are held still while
 * a GIF is made or the preview plays; while a GIF is made the whole timeline
 * is. The cursor is left at the start of the next line. */
void clipGifZoomRow(const char *id, ClipGifSession *s, const ClipGifReplay *r,
                    ClipGifMode *mode, float width);

/* ── The GIF window ──────────────────────────────────────────────── */

/* Where the GIF window is. Until the player drags it, it goes over the
 * replay's transport row (clipGifWindowDefaultPos) and follows it. A drag
 * that ends at a new place keeps that place: saved is set, x and y are the
 * window's top-left from the main window's top-left, in whole pixels (0 or
 * more, as the window is kept on the main window), and changed is set
 * for the screen to write it to its preferences (it clears changed). The
 * screen reads a saved place back into saved, x and y at the start. */
typedef struct ClipGifWindowPlace {
    bool  saved;
    float x, y;
    bool  changed;
    /* A drag of the window is on, and where the window was when it began. */
    bool  moving;
    float fromX, fromY;
    /* The window's height the last frame, for its place over the row. */
    float lastH;
} ClipGifWindowPlace;

#define CLIP_GIF_WINDOW_PLACE_INIT { false, 0.0f, 0.0f, false, false, 0.0f, 0.0f, 0.0f }

/* What the player pressed in the GIF window this frame, for the screen to
 * do: start or stop the preview, make the GIF, open the gallery. Cancel and
 * the window's X are done in the window: recording mode is off. */
typedef struct ClipGifWindowPressed {
    bool preview;
    bool save;
    bool gallery;
} ClipGifWindowPressed;

/* The GIF window of recording mode: Preview clip, Save clip, Cancel, the
 * length, the gallery button (clipGifToolsRow) and under them the zoomed
 * timeline (clipGifZoomRow). The screen draws it while the GIF controls
 * show (clipGifToolsShown), inside its ImGui frame. The window's X does what
 * Cancel does; it has no X while a GIF is made (the progress window has
 * Cancel then).
 *
 * ax0..ay1 is the anchor: the box of the replay's own transport row and
 * timeline, in screen coordinates. The window is about as wide as it, and
 * goes over it until the player drags it (place). canMake and picture are
 * as clipGifToolsRow has them; picture is the screen's picture texture
 * (an SDL_Texture), or NULL for the written GIF button. A press on the
 * window is the window's: ImGui gives the mouse to the window on top, so
 * the view and the crop frame under it do not take it. */
ClipGifWindowPressed clipGifToolsWindow(const char *id, ClipGifSession *s,
                                        const ClipGifReplay *r,
                                        ClipGifMode *mode, bool canMake,
                                        const void *picture, float ax0,
                                        float ay0, float ax1, float ay1,
                                        ClipGifWindowPlace *place);

/* Where a finished GIF goes: on the desktop into the gallery folder, with
 * its metadata, and the gallery opens on it; on iOS into Documents, with a
 * message box that names it; on Android into the app's files, then to the
 * share sheet. Writes the bytes and keeps no pointer to them. */
void clipGifSaveGif(const void *data, size_t len, const ClipGifMeta *meta,
                    const char *baseName);

#endif /* CLIP_GIF_CAPTURE_H */
