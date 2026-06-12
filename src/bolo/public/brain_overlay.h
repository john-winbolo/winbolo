#ifndef BRAIN_OVERLAY_H
#define BRAIN_OVERLAY_H

#include <stdint.h>

typedef enum {
    OVERLAY_CMD_LINE,
    OVERLAY_CMD_RECT,
    OVERLAY_CMD_RECT_FILL,
    OVERLAY_CMD_RECT_FILL_SUBPIXEL,  /* same as RECT_FILL but skips the
                                       game-pixel floor in mapToScreen,
                                       so sub-wu fractions render. */
    OVERLAY_CMD_CIRCLE,
    OVERLAY_CMD_CIRCLE_SUBPIXEL,     /* same as CIRCLE but bypasses the
                                       game-pixel floor — for markers
                                       that need to land on a sub-game-
                                       pixel position (e.g. shell hit
                                       dot at WU precision). */
    OVERLAY_CMD_CIRCLE_FILL,         /* filled disc (triangle fan) instead
                                       of an outline ring. */
    OVERLAY_CMD_TEXT,
    OVERLAY_CMD_HUD_TEXT,
    OVERLAY_CMD_HUD_RECT,        /* HUD-space rect outline (x1/y1=offset, x2/y2=w/h) */
    OVERLAY_CMD_HUD_RECT_FILL,   /* HUD-space filled rect (solid background) */
    OVERLAY_CMD_CLEAR
} OverlayCmdType;

/* Text anchor points */
#define OVERLAY_ANCHOR_TOPLEFT     0
#define OVERLAY_ANCHOR_TOPRIGHT    1
#define OVERLAY_ANCHOR_BOTTOMLEFT  2
#define OVERLAY_ANCHOR_BOTTOMRIGHT 3
#define OVERLAY_ANCHOR_CENTER      4

#define OVERLAY_TEXT_MAX 128
#define OVERLAY_MAP_PIXELS (256 * 16)  /* 4096 — full map resolution */
#define OVERLAY_BUF_SIZE (OVERLAY_MAP_PIXELS * OVERLAY_MAP_PIXELS * 4)

typedef struct {
    OverlayCmdType type;
    float x1, y1, x2, y2;
    float radius;
    uint8_t r, g, b, a;
    uint8_t anchor;
    /* Index into BrainTest's VIZ_TOGGLES array (0..N-1). 0xFF means
     * "no viz_id" (legacy callers, native draws). When non-0xFF, the
     * BrainTest renderer can filter the command at draw time based on
     * the toggle state — required so toggling V checkboxes during
     * playback updates the visible overlays for that frame's recorded
     * commands. Lua-side wrappers (viz.X) populate this from a
     * _BT_VIZ_IDS lookup table pushed by BrainTest at startup. */
    uint8_t viz_idx;
    char text[OVERLAY_TEXT_MAX];
} OverlayCmd;
#define OVERLAY_VIZ_IDX_NONE 0xFF

#ifndef OVERLAYCMDBUFFER_TYPEDEF
#define OVERLAYCMDBUFFER_TYPEDEF
typedef struct OverlayCmdBuffer OverlayCmdBuffer;
#endif
struct OverlayCmdBuffer {
    OverlayCmd *cmds;
    int count;
    int capacity;

    /* Raw RGBA pixel buffer (4096x4096, map-pixel resolution) */
    uint8_t *pixels;    /* NULL until first use (lazy alloc, 64MB) */
    int pixelsDirty;    /* Non-zero if pixels were written this tick */
};

void overlayCmdBufferInit(OverlayCmdBuffer *buf);
void overlayCmdBufferDestroy(OverlayCmdBuffer *buf);
void overlayCmdBufferClear(OverlayCmdBuffer *buf);

void overlayCmdLine(OverlayCmdBuffer *buf, float x1, float y1, float x2, float y2,
                    uint8_t r, uint8_t g, uint8_t b, uint8_t a);

void overlayCmdRect(OverlayCmdBuffer *buf, float x1, float y1, float x2, float y2,
                    uint8_t r, uint8_t g, uint8_t b, uint8_t a, int filled);

void overlayCmdCircle(OverlayCmdBuffer *buf, float cx, float cy, float radius,
                      uint8_t r, uint8_t g, uint8_t b, uint8_t a);

void overlayCmdText(OverlayCmdBuffer *buf, float x, float y, const char *text,
                    uint8_t anchor, uint8_t r, uint8_t g, uint8_t b, uint8_t a);

/* HUD text: x/y are pixel offsets from the chosen corner.
 * anchor determines which corner of the screen to pin to. */
void overlayCmdHudText(OverlayCmdBuffer *buf, float x, float y, const char *text,
                       uint8_t anchor, uint8_t r, uint8_t g, uint8_t b, uint8_t a);

/* HUD rect: x/y are pixel offsets from the chosen corner (same anchor scheme
 * as HUD text), w/h the size in pixels. filled != 0 draws a solid fill,
 * otherwise a 1px outline. */
void overlayCmdHudRect(OverlayCmdBuffer *buf, float x, float y, float w, float h,
                       uint8_t anchor, uint8_t r, uint8_t g, uint8_t b, uint8_t a, int filled);

/* Tag the most-recently-pushed command with a viz_idx (a stable
 * uint8 index into BrainTest's VIZ_TOGGLES array). The Lua-side
 * overlay_X bindings call this right after pushing so the renderer
 * can filter recorded commands during playback when V checkboxes
 * change. No-op if buf is empty. */
void overlayCmdSetLastVizIdx(OverlayCmdBuffer *buf, uint8_t viz_idx);

#endif
