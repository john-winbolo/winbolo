#include "brain_overlay.h"
#include <stdlib.h>
#include <string.h>

#define INITIAL_CAPACITY 64

void overlayCmdBufferInit(OverlayCmdBuffer *buf) {
    buf->cmds = NULL;
    buf->count = 0;
    buf->capacity = 0;
}

void overlayCmdBufferDestroy(OverlayCmdBuffer *buf) {
    free(buf->cmds);
    buf->cmds = NULL;
    buf->count = 0;
    buf->capacity = 0;
}

void overlayCmdBufferClear(OverlayCmdBuffer *buf) {
    buf->count = 0;
}

static OverlayCmd *pushCmd(OverlayCmdBuffer *buf) {
    if (buf->count >= buf->capacity) {
        int newCap = buf->capacity == 0 ? INITIAL_CAPACITY : buf->capacity * 2;
        buf->cmds = (OverlayCmd *)realloc(buf->cmds, newCap * sizeof(OverlayCmd));
        buf->capacity = newCap;
    }
    OverlayCmd *cmd = &buf->cmds[buf->count++];
    memset(cmd, 0, sizeof(*cmd));
    /* Default to "no viz_id" — Lua callers via viz.X overwrite this
     * by calling overlayCmdSetLastVizIdx after the push. */
    cmd->viz_idx = OVERLAY_VIZ_IDX_NONE;
    return cmd;
}

void overlayCmdSetLastVizIdx(OverlayCmdBuffer *buf, uint8_t viz_idx) {
    if (!buf || buf->count <= 0) return;
    buf->cmds[buf->count - 1].viz_idx = viz_idx;
}

void overlayCmdLine(OverlayCmdBuffer *buf, float x1, float y1, float x2, float y2,
                    uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    OverlayCmd *cmd = pushCmd(buf);
    cmd->type = OVERLAY_CMD_LINE;
    cmd->x1 = x1; cmd->y1 = y1;
    cmd->x2 = x2; cmd->y2 = y2;
    cmd->r = r; cmd->g = g; cmd->b = b; cmd->a = a;
}

void overlayCmdRect(OverlayCmdBuffer *buf, float x1, float y1, float x2, float y2,
                    uint8_t r, uint8_t g, uint8_t b, uint8_t a, int filled) {
    OverlayCmd *cmd = pushCmd(buf);
    cmd->type = filled ? OVERLAY_CMD_RECT_FILL : OVERLAY_CMD_RECT;
    cmd->x1 = x1; cmd->y1 = y1;
    cmd->x2 = x2; cmd->y2 = y2;
    cmd->r = r; cmd->g = g; cmd->b = b; cmd->a = a;
}

void overlayCmdCircle(OverlayCmdBuffer *buf, float cx, float cy, float radius,
                      uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    OverlayCmd *cmd = pushCmd(buf);
    cmd->type = OVERLAY_CMD_CIRCLE;
    cmd->x1 = cx; cmd->y1 = cy;
    cmd->radius = radius;
    cmd->r = r; cmd->g = g; cmd->b = b; cmd->a = a;
}

void overlayCmdText(OverlayCmdBuffer *buf, float x, float y, const char *text,
                    uint8_t anchor, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    OverlayCmd *cmd = pushCmd(buf);
    cmd->type = OVERLAY_CMD_TEXT;
    cmd->x1 = x; cmd->y1 = y;
    cmd->anchor = anchor;
    cmd->r = r; cmd->g = g; cmd->b = b; cmd->a = a;
    strncpy(cmd->text, text, OVERLAY_TEXT_MAX - 1);
    cmd->text[OVERLAY_TEXT_MAX - 1] = '\0';
}

void overlayCmdHudText(OverlayCmdBuffer *buf, float x, float y, const char *text,
                       uint8_t anchor, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    OverlayCmd *cmd = pushCmd(buf);
    cmd->type = OVERLAY_CMD_HUD_TEXT;
    cmd->x1 = x; cmd->y1 = y;
    cmd->anchor = anchor;
    cmd->r = r; cmd->g = g; cmd->b = b; cmd->a = a;
    strncpy(cmd->text, text, OVERLAY_TEXT_MAX - 1);
    cmd->text[OVERLAY_TEXT_MAX - 1] = '\0';
}
