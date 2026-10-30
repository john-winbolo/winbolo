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

/*
 * The rules behind the clip GIFs that do not need ImGui, for the game's
 * replay and the Log Viewer both: the length the GIF button records and
 * where that length sits on the seek bar, recording mode's state, the crop
 * frame and the pixels it takes, the Log Viewer's visible map, and the small
 * metadata file written next to each GIF in the gallery folder, with the
 * scan that lists them newest first.
 *
 * Plain C over SDL and cJSON, split out of lobby_clip_gif.cpp and
 * lobby_reel.cpp so test_clip_gif_core.c drives the same code the UI runs.
 * The ImGui controls that use these rules are in clip_gif_controls.h.
 */

#ifndef CLIP_GIF_CORE_H
#define CLIP_GIF_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A rectangle in the pixels of the viewer's render target. */
typedef struct {
    int x;
    int y;
    int w;
    int h;
} ClipGifRect;

/* ── Length ──────────────────────────────────────────────────────────
 * The export records one frame every 100 ms (10 fps), so a length is held in
 * whole frames. The shortest and longest lengths are the export's own frame
 * floor and cap (10 and 300 frames); the default is the 5 s the transport's
 * button recorded before the length could be changed. */
#define CLIP_GIF_LEN_STEP_MS    100u
#define CLIP_GIF_LEN_MIN_MS     1000u
#define CLIP_GIF_LEN_MAX_MS     30000u
#define CLIP_GIF_LEN_DEFAULT_MS 5000u

/* Rounds lenMs to the nearest whole frame and holds it inside
 * [CLIP_GIF_LEN_MIN_MS, CLIP_GIF_LEN_MAX_MS]. Then, when totalMs is known
 * (not 0), holds it to the replay time left after curMs, so a clip does not
 * run past the end of the round. The minimum still applies near the end: a
 * clip is never shorter than CLIP_GIF_LEN_MIN_MS. */
uint32_t clipGifLenClampMs(uint32_t lenMs, uint32_t curMs, uint32_t totalMs);

/* Seconds typed into the length field, to a length in ms, clamped as above.
 * A negative or not-a-number value gives the minimum. */
uint32_t clipGifLenFromSeconds(float secs, uint32_t curMs, uint32_t totalMs);

/* A length in ms, as the seconds the length field shows. */
float clipGifLenToSeconds(uint32_t lenMs);

/* ── Capture ─────────────────────────────────────────────────────────
 * The rules the game's replay and the Log Viewer share when they make a GIF:
 * one frame every CLIP_GIF_FRAME_MS (10 fps), at least CLIP_GIF_MIN_FRAMES
 * and at most CLIP_GIF_MAX_FRAMES frames, and a view wider than
 * CLIP_GIF_MAX_WIDTH pixels cut down to its middle part. */
#define CLIP_GIF_FRAME_MS   100u
#define CLIP_GIF_MIN_FRAMES 10u
#define CLIP_GIF_MAX_FRAMES 300u
#define CLIP_GIF_MAX_WIDTH  480

/* How many frames a GIF of durationMs has: durationMs / CLIP_GIF_FRAME_MS,
 * held inside [CLIP_GIF_MIN_FRAMES, CLIP_GIF_MAX_FRAMES]. From the length in
 * ms, not in ticks: the viewer steps a log clock and a highlight is measured
 * in sim ticks, and the two do not have the same rate. */
uint32_t clipGifFrameCount(uint32_t durationMs);


/* The part of the visible slice (srcX, srcY, srcW, srcH) a GIF takes when
 * the player has not drawn a frame: all of it when it is maxW pixels wide or
 * less, else the middle maxW pixels, with the height cut by the same ratio
 * so the GIF keeps the shape of the view. False, with *out cleared, when the
 * slice is empty. */
bool clipGifCentreCrop(int srcX, int srcY, int srcW, int srcH, int maxW,
                       ClipGifRect *out);

/* The ticks of the replay's log clock in one GIF frame (the log clock is
 * 20 ms a tick, so five ticks are CLIP_GIF_FRAME_MS), and how many frames a
 * capture makes in each frame of the screen, so the longest GIF takes about
 * a second or two and the screen still updates. */
#define CLIP_GIF_TICKS_PER_FRAME 5
#define CLIP_GIF_FRAMES_PER_PASS 4

/* ── File names ──────────────────────────────────────────────────────
 * A GIF is called <map>_<mmss>_<YYYYMMDD-HHMMSS>.gif in the gallery folder:
 * the map, the time in the replay it starts at, and when it was made. */

/* The highest number clipGifFileName puts on the end of a name, for the rare
 * case that two GIFs of the same map and time are made in the same second. */
#define CLIP_GIF_FILE_NAME_MAX_N 99

/* <map>_<mmss> into out: the map's name without a ".map" on the end, with
 * every character other than 0-9, A-Z, a-z, '-' and '_' changed to '_', as a
 * map name is free text. "clip" when there is no map name. */
void clipGifBaseName(char *out, size_t outLen, const char *mapName,
                     unsigned mins, unsigned secs);

/* The gallery's file name for a GIF: <baseName>_<stamp>.gif for n 1, and
 * <baseName>_<stamp>_<n>.gif for n 2 to CLIP_GIF_FILE_NAME_MAX_N. "clip" for
 * an empty or NULL baseName. False when n is out of that range or the name
 * does not fit in outLen. */
bool clipGifFileName(char *out, size_t outLen, const char *baseName,
                     const char *stamp, int n);

/* ── Recording mode ──────────────────────────────────────────────────
 * The GIF controls (the GIF button, the length field and marker, the
 * gallery button, the crop button and frame, and in the game's replay the
 * GIF button on each highlight row) show only in recording mode. The movie
 * camera button turns it on and off: on the replay's transport row, and on
 * the Log Viewer's Controls panel (also Action > Recording Mode, Ctrl+G).
 *
 * True when the GIF controls show: in recording mode, and also while a
 * capture is running, so the controls of a GIF that is being made do not go
 * away until it ends. */
bool clipGifToolsShown(bool recordMode, bool captureActive);

/* ── Timeline position ───────────────────────────────────────────────
 * The seek bar maps 0..totalMs onto the screen range [x0, x1] (the range
 * the slider's grab centre moves over). These convert between a time and a
 * screen x on that range, and between the clip's end handle and a length. */

/* Screen x of timeMs on the bar. Times past totalMs sit at x1. When totalMs
 * is 0 or the range is empty, the result is x0. */
float clipGifTimelineX(uint32_t timeMs, uint32_t totalMs, float x0, float x1);

/* The length that puts the clip's end at screen x, for a clip that starts at
 * curMs. Clamped with clipGifLenClampMs, so dragging the handle before the
 * playhead gives the minimum and dragging it past the end of the round gives
 * the time left. */
uint32_t clipGifLenFromTimelineX(float x, uint32_t curMs, uint32_t totalMs,
                                 float x0, float x1);

/* ── Crop frame ──────────────────────────────────────────────────────
 * The frame the player can put over the view in recording mode. With it on,
 * a GIF takes the part of the view inside the frame, in place of the middle
 * of the view. It is held as fractions of the visible view (0 is the left or
 * top edge, 1 the right or bottom edge), so a zoom or a new window size keeps
 * the same framing. */
typedef struct {
    float x0;
    float y0;
    float x1;
    float y1;
} ClipGifFrame;

/* The frame a recording mode starts with: the middle half of the view. */
#define CLIP_GIF_FRAME_INIT { 0.25f, 0.25f, 0.75f, 0.75f }

/* A box on the screen, in the units the view is drawn in: x0,y0 is the top
 * left corner and x1,y1 the bottom right corner. */
typedef struct {
    float x0;
    float y0;
    float x1;
    float y1;
} ClipGifBox;

/* The parts of the frame a press can take hold of: an edge, two edges at a
 * corner, or the inside, which moves the whole frame. */
#define CLIP_GIF_GRAB_LEFT   1
#define CLIP_GIF_GRAB_RIGHT  2
#define CLIP_GIF_GRAB_TOP    4
#define CLIP_GIF_GRAB_BOTTOM 8
#define CLIP_GIF_GRAB_MOVE   16

/* The frame f on a view drawn in the box image. */
ClipGifBox clipGifFrameToBox(const ClipGifFrame *f, const ClipGifBox *image);

/* The frame that box is on a view drawn in the box image. An image with no
 * width or no height leaves *f as it was. */
void clipGifFrameFromBox(ClipGifFrame *f, const ClipGifBox *box,
                         const ClipGifBox *image);

/* The part of the frame box that the point (mx, my) is on: the edges within
 * grab of the point (CLIP_GIF_GRAB_LEFT and the others, two at a corner),
 * CLIP_GIF_GRAB_MOVE inside the frame away from the edges, and 0 off it. */
int clipGifFrameHit(const ClipGifBox *box, float mx, float my, float grab);

/* Moves the part grab of the frame box by (dx, dy). The frame stays inside
 * image. A move stops at the edge of image and keeps the frame's size; an
 * edge stops minSize short of the edge across from it, so the frame never
 * turns inside out. */
void clipGifFrameDrag(ClipGifBox *box, int grab, float dx, float dy,
                      const ClipGifBox *image, float minSize);

/* The pixels of the render target that frame f takes, on a visible view
 * that starts at (viewX, viewY) and is viewW x viewH pixels: each fraction
 * times the view's size, to the nearest pixel, inside the view, and at
 * least 1 x 1. No width cap: the frame says what to take. False, with *out
 * cleared, when the view is empty. */
bool clipGifCropFromFrame(const ClipGifFrame *f, int viewX, int viewY,
                          int viewW, int viewH, ClipGifRect *out);

/* The pixels a GIF takes from the visible view (viewX, viewY, viewW,
 * viewH), as the game's replay and the Log Viewer both choose them: the part
 * in the frame f when useFrame is true (clipGifCropFromFrame, no width cap),
 * else the middle of the view, at most CLIP_GIF_MAX_WIDTH wide
 * (clipGifCentreCrop). False, with *out cleared, when the view is empty or
 * useFrame is true with no frame. */
bool clipGifChooseCrop(bool useFrame, const ClipGifFrame *f, int viewX,
                       int viewY, int viewW, int viewH, ClipGifRect *out);

/* ── The Log Viewer's map ────────────────────────────────────────────
 * The Log Viewer draws its map with one blit of its render target: src (in
 * render-target pixels) goes to dst (in pixels of the window's frame
 * buffer). area is the part of the window, in window points, that is map:
 * what the menu bar and the solid sidebars leave. pxScale is the frame
 * buffer pixels to each window point.
 *
 * Gives the whole render-target pixels that show inside area (*slice), and
 * the box in window points that those pixels are drawn in (*view). This is
 * the view the crop frame is drawn on and is a fraction of. False, with
 * both cleared, when no whole pixel shows. */
bool clipGifBlitView(const ClipGifBox *src, const ClipGifBox *dst,
                     const ClipGifBox *area, float pxScale,
                     ClipGifRect *slice, ClipGifBox *view);

/* ── Recording mode state ────────────────────────────────────────────
 * What a recording mode holds: on or off, the crop frame on or off, where
 * the frame is, and the length the GIF button records. The game's replay
 * holds one for each replay it opens, and the Log Viewer one for each log
 * it opens; both start from CLIP_GIF_MODE_INIT. */
typedef struct {
    bool         on;
    bool         cropOn;
    ClipGifFrame frame;
    uint32_t     lenMs;
} ClipGifMode;

#define CLIP_GIF_MODE_INIT \
    { false, false, CLIP_GIF_FRAME_INIT, CLIP_GIF_LEN_DEFAULT_MS }

/* Back to CLIP_GIF_MODE_INIT. */
void clipGifModeReset(ClipGifMode *m);

/* Turns recording mode on or off, and returns what it is now. While a
 * capture runs nothing changes: the controls of a GIF being made stay. */
bool clipGifModeToggle(ClipGifMode *m, bool captureActive);

/* Turns the crop frame on or off, and returns what it is now. While a
 * capture runs nothing changes: the GIF is being read from the frame. */
bool clipGifModeToggleCrop(ClipGifMode *m, bool captureActive);

/* ── Preview ─────────────────────────────────────────────────────────
 * The Preview clip button plays the clip in the view, at the normal speed,
 * from where it starts to where it ends, and makes no file. At the end the
 * replay stops and goes back to the start of the clip, so the Save clip
 * button then makes the GIF of the same clip. This is what the preview
 * holds: on or off, and the clip's start and end, in the replay's ms. */
typedef struct {
    bool     active;
    uint32_t startMs;
    uint32_t endMs;
} ClipGifPlay;

#define CLIP_GIF_PLAY_INIT { false, 0u, 0u }

/* How far past the clip's end the replay can be on the frame the preview
 * sees it there and still count as the end. A time later than that is a
 * seek by the player. */
#define CLIP_GIF_PLAY_OVERRUN_MS 1000u

/* What clipGifPlayUpdate tells the screen to do. */
#define CLIP_GIF_PLAY_NONE   0  /* nothing: the preview plays on, or is off */
#define CLIP_GIF_PLAY_END    1  /* the clip ended: stop the replay and go back
                                 * to startMs */
#define CLIP_GIF_PLAY_LET_GO 2  /* the player paused or moved the replay: the
                                 * preview ends and the replay stays where the
                                 * player put it */

/* Starts a preview of lenMs from startMs. The length is clamped as the
 * GIF's is (clipGifLenClampMs), and the end is never past totalMs. False,
 * with the preview off, when there is no replay time after startMs. */
bool clipGifPlayStart(ClipGifPlay *p, uint32_t startMs, uint32_t lenMs,
                      uint32_t totalMs);

/* Each frame while the preview is on: curMs is the replay's time now and
 * playing whether it plays. Returns CLIP_GIF_PLAY_END when curMs is at the
 * end (up to CLIP_GIF_PLAY_OVERRUN_MS past it), CLIP_GIF_PLAY_LET_GO when
 * the replay is paused or is before the start or well past the end, and
 * CLIP_GIF_PLAY_NONE else. The preview is off after END or LET_GO. */
int clipGifPlayUpdate(ClipGifPlay *p, uint32_t curMs, bool playing);

/* Turns the preview off. */
void clipGifPlayStop(ClipGifPlay *p);

/* Where the clip's band on the timeline starts: the preview's start while it
 * plays, so the band does not move with the playhead, else curMs. */
uint32_t clipGifPlayBandStart(const ClipGifPlay *p, uint32_t curMs);

/* ── Metadata ────────────────────────────────────────────────────────
 * One small JSON file next to each GIF, with the same base name and the
 * extension .json. It says when the GIF was made, on which map, with which
 * players and from which game. A GIF with no metadata file, or with one that
 * cannot be read, is still listed: its time then comes from the file itself
 * and the map and players are left empty. */
#define CLIP_GIF_META_VERSION     1
#define CLIP_GIF_META_MAX_PLAYERS 16
#define CLIP_GIF_META_NAME_LEN    64
#define CLIP_GIF_META_MAP_LEN     64
#define CLIP_GIF_META_FILE_LEN    160
#define CLIP_GIF_META_GAME_LEN    24

typedef struct {
    int64_t  createdUnix;    /* seconds since 1970-01-01 UTC */
    char     map[CLIP_GIF_META_MAP_LEN];
    int      playerCount;    /* 0..CLIP_GIF_META_MAX_PLAYERS */
    char     players[CLIP_GIF_META_MAX_PLAYERS][CLIP_GIF_META_NAME_LEN];
    uint32_t durationMs;     /* 0 when not known */
    int      width;          /* 0 when not known */
    int      height;         /* 0 when not known */
    /* The game the GIF was made from: clipGifGameIdFromBytes of the replay
     * that was open. Empty for a GIF written before the field was added,
     * which the gallery lists with the earlier games. */
    char     game[CLIP_GIF_META_GAME_LEN];
    /* The GIF's file name, with no folder. Filled by the scan from the file
     * it found, never trusted from the metadata text. */
    char     gifFile[CLIP_GIF_META_FILE_LEN];
    /* False when the metadata file was missing or could not be read, and the
     * fields above other than createdUnix and gifFile are empty. */
    bool     hasMeta;
} ClipGifMeta;

/* The metadata as JSON text. Returns NULL on failure; the caller releases the
 * text with clipGifMetaFreeJson. gifFile and hasMeta are not written. */
char *clipGifMetaToJson(const ClipGifMeta *m);
void  clipGifMetaFreeJson(char *json);

/* Reads metadata out of JSON text of len bytes (the text need not end in a
 * NUL). Returns false, with *out cleared, when the text is not a JSON object
 * or has no usable "created" time. Fields of the wrong type are left empty,
 * long strings are cut to fit, and players past CLIP_GIF_META_MAX_PLAYERS are
 * dropped. On success hasMeta is true; gifFile is left empty. */
bool clipGifMetaFromJson(const char *text, size_t len, ClipGifMeta *out);

/* Writes / reads one metadata file. Paths are UTF-8. */
bool clipGifMetaWriteFile(const char *path, const ClipGifMeta *m);
bool clipGifMetaReadFile(const char *path, ClipGifMeta *out);

/* Sorts newest first: by createdUnix, larger first, and for the same time by
 * gifFile, later name first, so the order does not depend on the scan. */
void clipGifMetaSortNewestFirst(ClipGifMeta *items, int count);

/* Lists every .gif file in dir with its metadata, newest first. Returns the
 * count and sets *out to an SDL_malloc'd array the caller frees with
 * SDL_free (NULL when the count is 0). A metadata file with no GIF beside it
 * is not listed. A folder that does not exist gives 0. */
int clipGifGalleryScan(const char *dir, ClipGifMeta **out);

/* The metadata file's path for a GIF's path: the same path with its .gif
 * extension (any case) changed to .json, or .json added when it has none.
 * False when it does not fit in outLen. */
bool clipGifMetaPathFor(const char *gifPath, char *out, size_t outLen);

/* Removes a gallery GIF (a UTF-8 path) and the metadata file beside it.
 * True when the GIF is gone afterwards, also when it was already gone. A
 * metadata file that cannot be removed does not make it false. */
bool clipGifDeleteFiles(const char *gifPath);

/* The row to select after row removed of a list of count rows is taken out:
 * the row that moves up into its place, or the row above when it was the
 * last. -1 when no row is left. */
int clipGifSelectAfterRemove(int count, int removed);

/* Sets the metadata's map from a map name: the name without a ".map" on the
 * end (any case), cut on a whole UTF-8 character to fit. "" for a NULL or
 * empty name. */
void clipGifMetaSetMap(ClipGifMeta *m, const char *mapName);

/* Clears m and fills what every GIF's metadata starts with: when it was
 * made, the id of the game (cut to fit; "" for NULL) and the map
 * (clipGifMetaSetMap). The screen then adds the players. */
void clipGifMetaBegin(ClipGifMeta *m, int64_t createdUnix, const char *gameId,
                      const char *mapName);

/* Adds a player's name to m, cut on a whole UTF-8 character to fit. An
 * empty or NULL name is not added. False when it is not added, also when m
 * has CLIP_GIF_META_MAX_PLAYERS names already. */
bool clipGifMetaAddPlayer(ClipGifMeta *m, const char *name);

/* ── Games ───────────────────────────────────────────────────────────
 * The gallery puts the GIFs from the game whose replay is open above the
 * GIFs from earlier games. A game is named by its replay's bytes: the same
 * round's replay gives the same id each time it is opened, and two rounds
 * do not give the same id. */

/* The id of the replay bytes data (len bytes), as 16 lower-case hex digits
 * (a 64-bit FNV-1a hash), into out (outLen bytes, always ended with a NUL).
 * No bytes give "". */
void clipGifGameIdFromBytes(const void *data, size_t len, char *out,
                            size_t outLen);

/* True when m was made from the game currentGame names. False when either id
 * is empty, so a GIF without the field, or a gallery with no replay open,
 * never counts as the current game. */
bool clipGifMetaIsCurrentGame(const ClipGifMeta *m, const char *currentGame);

/* Moves the items from the current game to the front and keeps the order
 * inside each group, so a list sorted newest first stays newest first in
 * both groups. Returns how many items are from the current game. */
int clipGifMetaCurrentGameFirst(ClipGifMeta *items, int count,
                                const char *currentGame);

/* createdUnix as local "YYYY-MM-DD HH:MM", for the gallery's list. */
void clipGifFormatTime(int64_t createdUnix, char *out, size_t outLen);

/* createdUnix as local "YYYYMMDD-HHMMSS", for a file name. */
void clipGifFormatFileStamp(int64_t createdUnix, char *out, size_t outLen);

/* ── Preview ─────────────────────────────────────────────────────────
 * The most memory the gallery's preview gives the decoded frames of one GIF
 * (RGBA, 4 bytes a pixel). A 30 s export of a 480 x 270 view is about
 * 156 MB; a GIF that needs more, such as a long crop of a large view or a
 * 30 s export more than about 460 px high at the 480 px width, is not
 * decoded and the window says it is too large to show. The limit also counts
 * the decoder's own working buffers (STBI_GIF_WORK_FRAMES frames), so the
 * frames and those buffers together stay within it. The gallery also does
 * not read a .gif file larger than this. */
#define CLIP_GIF_PREVIEW_MAX_BYTES ((size_t)256u * 1024u * 1024u)

/* ── Text ────────────────────────────────────────────────────────────
 * The gallery shows lines longer than the lang module's {string1} slot
 * (63 bytes), such as the list of players or a file name, so it puts them
 * into the translated text itself. */

/* Ends s at byte len, and when that cuts a UTF-8 sequence in two, drops the
 * part of the sequence that is left so the text ends on a whole character.
 * s must have room for len + 1 bytes. */
void clipGifUtf8CutEnd(char *s, size_t len);

/* fmt with every "{string1}" in it changed to value, into out (outLen bytes,
 * always ended with a NUL). Text that does not fit is cut on a whole UTF-8
 * character. A NULL value is read as "". */
void clipGifFillString1(const char *fmt, const char *value, char *out,
                        size_t outLen);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* CLIP_GIF_CORE_H */
