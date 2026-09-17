/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * The seven panel primitives, drawn with ImGui's 2D calls.
 *
 * One function per primitive, all of them working in panel units and
 * converting to pixels in one place — unitX/unitY below — so the mapping
 * from the square a script draws in to the square a player sees is written
 * once. A script never knows a pixel size, and nothing here knows what a
 * script meant.
 *
 * Everything is clipped to the square. scnPanelParse takes a coordinate,
 * width or height of any byte value on purpose: clipping is the drawer's
 * business, and a rect the script placed at x=120 with w=60 is trimmed at
 * the edge rather than painted across the game view.
 */

#include <cfloat>
#include <cmath>

#include "imgui.h"

#include "scenario_panel_draw.h"

#include "../tiles.h"       /* TILE_FILE_X/Y — the classic sheet's own size,
                             * and TILE_SIZE_X/Y, one tile in it */
#include "sprite_positions.h" /* mapViewPosX/Y, the tilenum -> sheet lookup */
#include "tilenum.h"        /* ROAD_HORZ — the one tile that lives at 0,0 */

/* ── The palette ────────────────────────────────────────────────────
 * The sixteen entries a script picks by index, as RGBA.
 *
 * Chosen to read against the game's own ground rather than to be pure
 * primaries: the map under the panel is grass green, sea blue and road
 * grey, so the reds, greens and blues here are lifted and desaturated a
 * little to stay legible over any of it.
 *
 * Index 0 draws nothing, and so do the four reserved entries until
 * something gives them a colour. A skin may take this table over later —
 * the whole reason a script sends an index and not an RGB is that the
 * frontend owns what the index means.
 *
 * Bytes rather than a packed word, because the two things that read the
 * table pack differently: the panel below wants IM_COL32 and the map marker
 * drawn with the SDL renderer wants four arguments.
 */
static const struct { uint8_t r, g, b, a; }
kScnPanelPalette[SCN_PANEL_COLOURS] = {
    {   0,   0,   0,   0 },   /* none — draws nothing */
    {  16,  16,  16, 255 },   /* black */
    { 255, 255, 255, 255 },   /* white */
    { 176, 176, 176, 255 },   /* grey */
    {  88,  88,  88, 255 },   /* grey_dark */
    { 232,  72,  64, 255 },   /* red */
    {  88, 216, 104, 255 },   /* green */
    {  96, 152, 248, 255 },   /* blue */
    { 248, 216,  80, 255 },   /* yellow */
    { 248, 152,  56, 255 },   /* orange */
    {  88, 216, 224, 255 },   /* cyan */
    { 232, 112, 208, 255 },   /* magenta */
    {   0,   0,   0,   0 },   /* reserved_12 */
    {   0,   0,   0,   0 },   /* reserved_13 */
    {   0,   0,   0,   0 },   /* reserved_14 */
    {   0,   0,   0,   0 }    /* reserved_15 */
};

/* The one way out of the table, for anything that is not the panel. */
bool scnPanelColourRGBA(uint8_t index, uint8_t *r, uint8_t *g, uint8_t *b,
                        uint8_t *a) {
    if (index >= SCN_PANEL_COLOURS) return false;
    if (kScnPanelPalette[index].a == 0) return false;
    if (r) *r = kScnPanelPalette[index].r;
    if (g) *g = kScnPanelPalette[index].g;
    if (b) *b = kScnPanelPalette[index].b;
    if (a) *a = kScnPanelPalette[index].a;
    return true;
}

/* The colour an index stands for as ImGui takes it, or 0 for one that draws
 * nothing — index 0, a reserved entry, and an index past the palette, which
 * the parser turns down but which costs nothing to hold here too. */
static ImU32 panelColour(uint8_t index) {
    uint8_t r = 0, g = 0, b = 0, a = 0;
    if (!scnPanelColourRGBA(index, &r, &g, &b, &a)) return 0;
    return IM_COL32(r, g, b, a);
}

/* Whether a colour is worth drawing at all: a fully transparent entry is
 * how this table spells "nothing here". */
static bool panelColourDraws(ImU32 col) {
    return (col >> IM_COL32_A_SHIFT) != 0u;
}

/* One panel unit, in pixels, and the two axes it is applied on. Every
 * coordinate in this file goes through these. */
static float unitX(const ScnPanelDrawEnv *env, float originX, float u) {
    return originX + u * env->scale;
}

static float unitY(const ScnPanelDrawEnv *env, float originY, float u) {
    return originY + u * env->scale;
}

/* A one-unit stroke, never thinner than the pixel that would make it
 * invisible at zoom 1. */
static float strokeWidth(const ScnPanelDrawEnv *env) {
    return (env->scale > 1.0f) ? env->scale : 1.0f;
}

/* The pixel height of the font a size byte asks for. */
static float textHeight(const ScnPanelDrawEnv *env, uint8_t size) {
    float units = (size == (uint8_t)SCN_PANEL_SIZE_SMALL)
                      ? SCN_PANEL_TEXT_SMALL_UNITS
                      : SCN_PANEL_TEXT_NORMAL_UNITS;
    float px = units * env->scale;
    /* ImGui rasterises nothing below a pixel or two, and a line the player
       cannot see is worse than one that is slightly too big. */
    return (px < 4.0f) ? 4.0f : px;
}

/* One string, in the window's own font at the asked-for height, sitting
 * left of, centred on, or right of its x. Used by text, name and timer,
 * which differ only in where the bytes come from. */
static void drawString(ImDrawList *dl, const ScnPanelDrawEnv *env,
                       float originX, float originY, uint8_t x, uint8_t y,
                       uint8_t colour, uint8_t size, uint8_t align,
                       const char *text) {
    ImU32   col;
    ImFont *font;
    float   height, px, py;
    ImVec2  measured;

    if (text == NULL || text[0] == '\0') return;
    col = panelColour(colour);
    if (!panelColourDraws(col)) return;

    font = ImGui::GetFont();
    if (font == NULL) return;

    height   = textHeight(env, size);
    measured = font->CalcTextSizeA(height, FLT_MAX, 0.0f, text);

    px = unitX(env, originX, (float)x);
    py = unitY(env, originY, (float)y);
    if (align == (uint8_t)SCN_PANEL_ALIGN_CENTRE) {
        px -= measured.x * 0.5f;
    } else if (align == (uint8_t)SCN_PANEL_ALIGN_RIGHT) {
        px -= measured.x;
    }

    dl->AddText(font, height, ImVec2(px, py), col, text);
}

/* ── The primitives ─────────────────────────────────────────────────── */

static void drawRect(ImDrawList *dl, const ScnPanelDrawEnv *env,
                     float originX, float originY, const ScnPanelItem *it) {
    ImU32 col = panelColour(it->u.rect.colour);
    float x0, y0, x1, y1;

    if (!panelColourDraws(col)) return;

    x0 = unitX(env, originX, (float)it->u.rect.x);
    y0 = unitY(env, originY, (float)it->u.rect.y);
    x1 = unitX(env, originX, (float)it->u.rect.x + (float)it->u.rect.w);
    y1 = unitY(env, originY, (float)it->u.rect.y + (float)it->u.rect.h);

    if (it->u.rect.fill != 0) {
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col);
        return;
    }
    /* Outlined. ImGui strokes centred on the path, so the rect is pulled in
       by half the stroke and the outline covers the same box a filled one
       would — a script drawing an outline and a fill at the same operands
       gets two shapes the same size. */
    {
        float t = strokeWidth(env);
        float h = t * 0.5f;
        if (x1 - x0 <= t || y1 - y0 <= t) {
            /* Too thin to inset without turning inside out: a rect this
               small is a line, and is drawn as the solid it looks like. */
            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col);
            return;
        }
        /* rounding then thickness: ImGui swapped thickness and flags in
           1.92.8, and the old order still compiles against the shim while
           meaning something else. */
        dl->AddRect(ImVec2(x0 + h, y0 + h), ImVec2(x1 - h, y1 - h), col,
                    0.0f, t);
    }
}

static void drawLine(ImDrawList *dl, const ScnPanelDrawEnv *env,
                     float originX, float originY, const ScnPanelItem *it) {
    ImU32 col = panelColour(it->u.line.colour);

    if (!panelColourDraws(col)) return;

    /* Half a unit onto each endpoint so a one-unit line sits inside the
       units it names rather than straddling their edges. */
    dl->AddLine(ImVec2(unitX(env, originX, (float)it->u.line.x0 + 0.5f),
                       unitY(env, originY, (float)it->u.line.y0 + 0.5f)),
                ImVec2(unitX(env, originX, (float)it->u.line.x1 + 0.5f),
                       unitY(env, originY, (float)it->u.line.y1 + 0.5f)),
                col, strokeWidth(env));
}

static void drawText(ImDrawList *dl, const ScnPanelDrawEnv *env,
                     float originX, float originY, const ScnPanelItem *it) {
    /* The script's own bytes, as they arrived. Not run through the language
       table: a scenario writes what its author wrote. */
    drawString(dl, env, originX, originY, it->u.text.x, it->u.text.y,
               it->u.text.colour, it->u.text.size, it->u.text.align,
               it->u.text.text);
}

static void drawName(ImDrawList *dl, const ScnPanelDrawEnv *env,
                     float originX, float originY, const ScnPanelItem *it) {
    const char *name;

    /* No roster to ask, or a seat nobody is in: nothing is drawn. That is
       what lets a script name a slot without knowing whether it is filled. */
    if (env->playerName == NULL) return;
    name = env->playerName(env->ctx, it->u.name.slot);
    if (name == NULL) return;

    drawString(dl, env, originX, originY, it->u.name.x, it->u.name.y,
               it->u.name.colour, it->u.name.size, it->u.name.align, name);
}

static void drawSprite(ImDrawList *dl, const ScnPanelDrawEnv *env,
                       float originX, float originY, const ScnPanelItem *it) {
    int   tile = (int)it->u.sprite.tile;
    int   sx, sy;
    float x0, y0, x1, y1;

    if (env->tiles == NULL) return;

    /* mapViewPosX/Y is cleared before it is filled, so a tile id the sheet
       has no slot for reads 0,0 — which is a real address, the one
       ROAD_HORZ lives at. Everything else reading 0,0 is an id the skin
       does not define, and draws nothing rather than a stray piece of road. */
    sx = mapViewPosX[tile];
    sy = mapViewPosY[tile];
    if (sx == 0 && sy == 0 && tile != ROAD_HORZ) return;

    x0 = unitX(env, originX, (float)it->u.sprite.x);
    y0 = unitY(env, originY, (float)it->u.sprite.y);
    x1 = unitX(env, originX, (float)it->u.sprite.x + (float)TILE_SIZE_X);
    y1 = unitY(env, originY, (float)it->u.sprite.y + (float)TILE_SIZE_Y);

    /* Texture coordinates in the classic 1x layout. The loaded sheet is that
       layout scaled whole, so its own density cancels and this is right at
       every skin density. */
    {
        ImVec2 uv0((float)sx / (float)TILE_FILE_X,
                   (float)sy / (float)TILE_FILE_Y);
        ImVec2 uv1((float)(sx + TILE_SIZE_X) / (float)TILE_FILE_X,
                   (float)(sy + TILE_SIZE_Y) / (float)TILE_FILE_Y);
        /* Point sampling while the tile is on the list, linear again after:
           magnified tile art turns to mush under the bilinear filter the
           SDL renderer backend resets to at the top of every pass, and the
           glyphs drawn after it want that filter back. */
        ImDrawCallback nearest = ImGui::GetPlatformIO().DrawCallback_SetSamplerNearest;
        ImDrawCallback linear  = ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear;
        if (nearest) dl->AddCallback(nearest, NULL);
        dl->AddImage((ImTextureID)(size_t)env->tiles, ImVec2(x0, y0), ImVec2(x1, y1),
                     uv0, uv1, IM_COL32_WHITE);
        if (linear) dl->AddCallback(linear, NULL);
    }
}

static void drawBar(ImDrawList *dl, const ScnPanelDrawEnv *env,
                    float originX, float originY, const ScnPanelItem *it) {
    ImU32 col = panelColour(it->u.bar.colour);
    float x0, y0, x1, y1, t, h;

    if (!panelColourDraws(col)) return;

    x0 = unitX(env, originX, (float)it->u.bar.x);
    y0 = unitY(env, originY, (float)it->u.bar.y);
    x1 = unitX(env, originX, (float)it->u.bar.x + (float)it->u.bar.w);
    y1 = unitY(env, originY, (float)it->u.bar.y + (float)it->u.bar.h);

    t = strokeWidth(env);
    h = t * 0.5f;
    if (x1 - x0 <= t || y1 - y0 <= t) {
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col);
        return;
    }

    /* The outline first, then the filled part inside it. A max of 0 leaves
       the outline empty rather than dividing by it — a bar whose total
       nothing has set yet is a bar with nothing in it. */
    dl->AddRect(ImVec2(x0 + h, y0 + h), ImVec2(x1 - h, y1 - h), col,
                0.0f, t);
    if (it->u.bar.max == 0) return;
    {
        uint16_t value = it->u.bar.value;
        float    inner = (x1 - t) - (x0 + t);
        float    filled;
        if (value > it->u.bar.max) value = it->u.bar.max;
        filled = inner * ((float)value / (float)it->u.bar.max);
        if (filled <= 0.0f) return;
        dl->AddRectFilled(ImVec2(x0 + t, y0 + t),
                          ImVec2(x0 + t + filled, y1 - t), col);
    }
}

static void drawTimer(ImDrawList *dl, const ScnPanelDrawEnv *env,
                      float originX, float originY, const ScnPanelItem *it) {
    char text[SCN_PANEL_TIMER_TEXT_MAX];

    scnPanelTimerText(env->tick, it->u.timer.tick, it->u.timer.mode, text,
                      sizeof(text));
    drawString(dl, env, originX, originY, it->u.timer.x, it->u.timer.y,
               it->u.timer.colour, it->u.timer.size, it->u.timer.align, text);
}

/* ── The list ───────────────────────────────────────────────────────── */

void scnPanelDraw(const ScnPanelList *list, float originX, float originY,
                  const ScnPanelDrawEnv *env) {
    ImDrawList *dl;
    float       side;
    uint8_t     i;

    if (list == NULL || list->count == 0 || env == NULL) return;
    if (env->scale <= 0.0f) return;

    dl = ImGui::GetWindowDrawList();
    if (dl == NULL) return;

    side = (float)SCN_PANEL_UNITS * env->scale;
    dl->PushClipRect(ImVec2(originX, originY),
                     ImVec2(originX + side, originY + side), true);

    for (i = 0; i < list->count; i++) {
        const ScnPanelItem *it = &list->items[i];
        switch (it->op) {
            case SCN_PANEL_OP_RECT:   drawRect  (dl, env, originX, originY, it); break;
            case SCN_PANEL_OP_LINE:   drawLine  (dl, env, originX, originY, it); break;
            case SCN_PANEL_OP_TEXT:   drawText  (dl, env, originX, originY, it); break;
            case SCN_PANEL_OP_NAME:   drawName  (dl, env, originX, originY, it); break;
            case SCN_PANEL_OP_SPRITE: drawSprite(dl, env, originX, originY, it); break;
            case SCN_PANEL_OP_BAR:    drawBar   (dl, env, originX, originY, it); break;
            case SCN_PANEL_OP_TIMER:  drawTimer (dl, env, originX, originY, it); break;
            default: break;   /* the parser refuses these; nothing to draw */
        }
    }

    dl->PopClipRect();
}
