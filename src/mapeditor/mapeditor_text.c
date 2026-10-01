/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_text.c
 * Purpose:
 *   Text rasterization for the map editor. Converts text
 *   into terrain tiles using a built-in 5x7 bitmap font.
 *********************************************************/

#include "mapeditor_text.h"
#include <string.h>
#include <stdint.h>

/* 5x7 base font — each glyph is 7 rows of 5-bit-wide bitmaps.
 * Lower 5 bits of each byte are pixels, MSB = leftmost.
 * Covers printable ASCII 32 (space) through 126 (tilde). */
#define GLYPH_W 5
#define GLYPH_H 7

static const uint8_t font5x7[95][GLYPH_H] = {
    /* ' ' (32) */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    /* '!' (33) */ { 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04 },
    /* '"' (34) */ { 0x0A, 0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00 },
    /* '#' (35) */ { 0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A },
    /* '$' (36) */ { 0x04, 0x0F, 0x14, 0x0E, 0x05, 0x1E, 0x04 },
    /* '%' (37) */ { 0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03 },
    /* '&' (38) */ { 0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D },
    /* ''' (39) */ { 0x0C, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00 },
    /* '(' (40) */ { 0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02 },
    /* ')' (41) */ { 0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08 },
    /* '*' (42) */ { 0x00, 0x04, 0x15, 0x0E, 0x15, 0x04, 0x00 },
    /* '+' (43) */ { 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00 },
    /* ',' (44) */ { 0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08 },
    /* '-' (45) */ { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 },
    /* '.' (46) */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C },
    /* '/' (47) */ { 0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00 },
    /* '0' (48) */ { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E },
    /* '1' (49) */ { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
    /* '2' (50) */ { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },
    /* '3' (51) */ { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },
    /* '4' (52) */ { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },
    /* '5' (53) */ { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
    /* '6' (54) */ { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E },
    /* '7' (55) */ { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
    /* '8' (56) */ { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },
    /* '9' (57) */ { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
    /* ':' (58) */ { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00 },
    /* ';' (59) */ { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x04, 0x08 },
    /* '<' (60) */ { 0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02 },
    /* '=' (61) */ { 0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00 },
    /* '>' (62) */ { 0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08 },
    /* '?' (63) */ { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04 },
    /* '@' (64) */ { 0x0E, 0x11, 0x17, 0x15, 0x17, 0x10, 0x0E },
    /* 'A' (65) */ { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
    /* 'B' (66) */ { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },
    /* 'C' (67) */ { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E },
    /* 'D' (68) */ { 0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C },
    /* 'E' (69) */ { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F },
    /* 'F' (70) */ { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },
    /* 'G' (71) */ { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F },
    /* 'H' (72) */ { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
    /* 'I' (73) */ { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E },
    /* 'J' (74) */ { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C },
    /* 'K' (75) */ { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 },
    /* 'L' (76) */ { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },
    /* 'M' (77) */ { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 },
    /* 'N' (78) */ { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 },
    /* 'O' (79) */ { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
    /* 'P' (80) */ { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 },
    /* 'Q' (81) */ { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D },
    /* 'R' (82) */ { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 },
    /* 'S' (83) */ { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E },
    /* 'T' (84) */ { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },
    /* 'U' (85) */ { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
    /* 'V' (86) */ { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 },
    /* 'W' (87) */ { 0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11 },
    /* 'X' (88) */ { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 },
    /* 'Y' (89) */ { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 },
    /* 'Z' (90) */ { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F },
    /* '[' (91) */ { 0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E },
    /* '\' (92) */ { 0x00, 0x10, 0x08, 0x04, 0x02, 0x01, 0x00 },
    /* ']' (93) */ { 0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E },
    /* '^' (94) */ { 0x04, 0x0A, 0x11, 0x00, 0x00, 0x00, 0x00 },
    /* '_' (95) */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F },
    /* '`' (96) */ { 0x08, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00 },
    /* 'a' (97) */ { 0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F },
    /* 'b' (98) */ { 0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x1E },
    /* 'c' (99) */ { 0x00, 0x00, 0x0E, 0x10, 0x10, 0x11, 0x0E },
    /* 'd' (100)*/ { 0x01, 0x01, 0x0D, 0x13, 0x11, 0x11, 0x0F },
    /* 'e' (101)*/ { 0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E },
    /* 'f' (102)*/ { 0x06, 0x09, 0x08, 0x1C, 0x08, 0x08, 0x08 },
    /* 'g' (103)*/ { 0x00, 0x0F, 0x11, 0x11, 0x0F, 0x01, 0x0E },
    /* 'h' (104)*/ { 0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x11 },
    /* 'i' (105)*/ { 0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E },
    /* 'j' (106)*/ { 0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0C },
    /* 'k' (107)*/ { 0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12 },
    /* 'l' (108)*/ { 0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E },
    /* 'm' (109)*/ { 0x00, 0x00, 0x1A, 0x15, 0x15, 0x11, 0x11 },
    /* 'n' (110)*/ { 0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11 },
    /* 'o' (111)*/ { 0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E },
    /* 'p' (112)*/ { 0x00, 0x00, 0x1E, 0x11, 0x1E, 0x10, 0x10 },
    /* 'q' (113)*/ { 0x00, 0x00, 0x0D, 0x13, 0x0F, 0x01, 0x01 },
    /* 'r' (114)*/ { 0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10 },
    /* 's' (115)*/ { 0x00, 0x00, 0x0E, 0x10, 0x0E, 0x01, 0x1E },
    /* 't' (116)*/ { 0x08, 0x08, 0x1C, 0x08, 0x08, 0x09, 0x06 },
    /* 'u' (117)*/ { 0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0D },
    /* 'v' (118)*/ { 0x00, 0x00, 0x11, 0x11, 0x11, 0x0A, 0x04 },
    /* 'w' (119)*/ { 0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0A },
    /* 'x' (120)*/ { 0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11 },
    /* 'y' (121)*/ { 0x00, 0x00, 0x11, 0x11, 0x0F, 0x01, 0x0E },
    /* 'z' (122)*/ { 0x00, 0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F },
    /* '{' (123)*/ { 0x02, 0x04, 0x04, 0x08, 0x04, 0x04, 0x02 },
    /* '|' (124)*/ { 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },
    /* '}' (125)*/ { 0x08, 0x04, 0x04, 0x02, 0x04, 0x04, 0x08 },
    /* '~' (126)*/ { 0x00, 0x00, 0x0D, 0x12, 0x00, 0x00, 0x00 },
};

TextConfig textDefaultConfig(void) {
    TextConfig cfg;
    cfg.text[0] = '\0';
    cfg.fontSize = TEXT_SIZE_MEDIUM;
    cfg.style = TEXT_STYLE_REGULAR;
    cfg.textTerrain = BUILDING;       /* 0 */
    cfg.edgeTerrain = HALFBUILDING;   /* 8 */
    cfg.bgTerrain = 0xFE;             /* ME_TRANSPARENT — none */
    return cfg;
}

/* Compute the dimensions of a single scaled+styled character. */
static void charDimensions(int scale, int style, int *cw, int *ch) {
    int w = GLYPH_W * scale;
    int h = GLYPH_H * scale;

    if (style == TEXT_STYLE_BOLD || style == TEXT_STYLE_BOLD_ITALIC) {
        w += 1;  /* bold adds 1 tile width */
    }
    if (style == TEXT_STYLE_ITALIC || style == TEXT_STYLE_BOLD_ITALIC) {
        w += h / 3;  /* italic shear widens the bounding box */
    }

    *cw = w;
    *ch = h;
}

/* Render a single glyph into a temporary buffer (charW x charH).
 * Returns true for each "on" pixel. Buffer must be zeroed by caller. */
static void renderGlyph(const uint8_t glyph[GLYPH_H], int scale, int style,
                        BYTE *buf, int charW, int charH) {
    int totalH = GLYPH_H * scale;

    for (int gy = 0; gy < GLYPH_H; gy++) {
        uint8_t row = glyph[gy];
        for (int gx = 0; gx < GLYPH_W; gx++) {
            if (!(row & (0x10 >> gx))) continue;

            /* Scale: each source pixel becomes scale x scale block */
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    int px = gx * scale + sx;
                    int py = gy * scale + sy;

                    /* Italic shear: shift right by (totalH - py) / 3 */
                    if (style == TEXT_STYLE_ITALIC || style == TEXT_STYLE_BOLD_ITALIC) {
                        px += (totalH - py) / 3;
                    }

                    if (px >= 0 && px < charW && py >= 0 && py < charH) {
                        buf[py * charW + px] = 1;
                    }

                    /* Bold: also set pixel to the right */
                    if (style == TEXT_STYLE_BOLD || style == TEXT_STYLE_BOLD_ITALIC) {
                        int bx = px + 1;
                        if (bx >= 0 && bx < charW && py >= 0 && py < charH) {
                            buf[py * charW + bx] = 1;
                        }
                    }
                }
            }
        }
    }
}

/* Shared edge pass: outline textTerrain tiles with edgeTerrain in 8 directions */
static void textApplyEdge(BYTE outTerrain[256][256], int w, int h,
                          BYTE textTerrain, BYTE edgeTerrain) {
    if (edgeTerrain == 0xFE) return;  /* ME_TRANSPARENT — no edge */

    /* Make a copy to avoid feedback (static to avoid 64KB stack usage) */
    static BYTE copy[256][256];
    for (int x = 0; x < w; x++) {
        memcpy(copy[x], outTerrain[x], (size_t)h);
    }

    static const int dx8[] = { -1, -1, -1, 0, 0, 1, 1, 1 };
    static const int dy8[] = { -1, 0, 1, -1, 1, -1, 0, 1 };

    for (int x = 0; x < w; x++) {
        for (int y = 0; y < h; y++) {
            if (copy[x][y] == textTerrain) {
                for (int d = 0; d < 8; d++) {
                    int nx = x + dx8[d];
                    int ny = y + dy8[d];
                    if (nx >= 0 && nx < w && ny >= 0 && ny < h) {
                        if (copy[nx][ny] != textTerrain) {
                            outTerrain[nx][ny] = edgeTerrain;
                        }
                    }
                }
            }
        }
    }
}

bool textRasterize(const TextConfig *cfg,
                   BYTE outTerrain[256][256],
                   int *outW, int *outH) {
    if (!cfg || cfg->text[0] == '\0') return false;

    int scale = cfg->fontSize + 1;  /* Small=1, Medium=2, Large=3, XL=4 */
    int charW, charH;
    charDimensions(scale, cfg->style, &charW, &charH);

    int charSpacing = 1;  /* 1-tile gap between characters */
    int lineSpacing = 1;  /* 1-tile gap between lines */

    /* First pass: compute total dimensions */
    const char *p = cfg->text;
    int totalW = 0;
    int totalH = 0;
    int lineW = 0;
    int numLines = 0;

    while (*p) {
        if (*p == '\n') {
            if (lineW > totalW) totalW = lineW;
            lineW = 0;
            numLines++;
            p++;
            continue;
        }
        if (*p >= 32 && *p <= 126) {
            if (lineW > 0) lineW += charSpacing;
            lineW += charW;
        }
        p++;
    }
    /* Account for last line (may not end with \n) */
    if (lineW > 0 || numLines == 0) {
        if (lineW > totalW) totalW = lineW;
        numLines++;
    }

    totalH = numLines * charH + (numLines - 1) * lineSpacing;

    if (totalW <= 0 || totalH <= 0) return false;

    /* Add 1-pixel padding on each side for edge outline expansion */
    int paddedW = totalW + 2;
    int paddedH = totalH + 2;
    if (paddedW > 256) paddedW = 256;
    if (paddedH > 256) paddedH = 256;

    /* Initialize output to background or transparent */
    BYTE bgFill = cfg->bgTerrain;  /* 0xFE (ME_TRANSPARENT) if none */
    for (int x = 0; x < paddedW; x++) {
        for (int y = 0; y < paddedH; y++) {
            outTerrain[x][y] = bgFill;
        }
    }

    /* Temporary glyph buffer */
    BYTE glyphBuf[64 * 64];  /* max charW*charH at XL+bold+italic */

    /* Second pass: render each character (offset by +1 for edge padding) */
    p = cfg->text;
    int curX = 1;
    int curY = 1;

    while (*p) {
        if (*p == '\n') {
            curX = 1;
            curY += charH + lineSpacing;
            p++;
            continue;
        }
        if (*p >= 32 && *p <= 126) {
            int glyphIdx = *p - 32;

            memset(glyphBuf, 0, (size_t)(charW * charH));
            renderGlyph(font5x7[glyphIdx], scale, cfg->style,
                        glyphBuf, charW, charH);

            /* Write glyph pixels to output */
            for (int gy = 0; gy < charH; gy++) {
                for (int gx = 0; gx < charW; gx++) {
                    if (glyphBuf[gy * charW + gx]) {
                        int ox = curX + gx;
                        int oy = curY + gy;
                        if (ox >= 0 && ox < paddedW && oy >= 0 && oy < paddedH) {
                            outTerrain[ox][oy] = cfg->textTerrain;
                        }
                    }
                }
            }

            curX += charW + charSpacing;
        }
        p++;
    }

    textApplyEdge(outTerrain, paddedW, paddedH, cfg->textTerrain, cfg->edgeTerrain);

    *outW = paddedW;
    *outH = paddedH;
    return true;
}

/* -------------------------------------------------------
 * TTF rasterization (desktop only)
 * ------------------------------------------------------- */

#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__) && !defined(__IPHONEOS__)

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

bool textRasterizeTTF(const char *fontPath, int pixelHeight,
                      const char *text,
                      BYTE textTerrain, BYTE edgeTerrain, BYTE bgTerrain,
                      BYTE outTerrain[256][256], int *outW, int *outH) {
    if (!text || !text[0]) return false;
    if (!fontPath || !fontPath[0]) return false;

    TTF_Font *font = TTF_OpenFont(fontPath, (float)pixelHeight);
    if (!font) return false;

    /* The font file already embodies the requested style (e.g. Arial-Bold.ttf),
     * so use NORMAL to avoid double-bold/italic from TTF_SetFontStyle. */
    TTF_SetFontStyle(font, TTF_STYLE_NORMAL);

    /* Render text to surface (white on transparent) */
    SDL_Color white = { 255, 255, 255, 255 };
    SDL_Surface *surf = TTF_RenderText_Blended_Wrapped(font, text, 0, white, 0);
    TTF_CloseFont(font);

    if (!surf) return false;

    int w = surf->w;
    int h = surf->h;
    /* Add 1-pixel padding on each side for edge outline expansion */
    int paddedW = w + 2;
    int paddedH = h + 2;
    if (paddedW > 256) paddedW = 256;
    if (paddedH > 256) paddedH = 256;
    if (w <= 0 || h <= 0 || w > 254 || h > 254) {
        SDL_DestroySurface(surf);
        return false;
    }

    /* Convert to RGBA32 for reliable pixel access */
    SDL_Surface *rgba = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(surf);
    if (!rgba) return false;

    /* Initialize output buffer */
    BYTE bgVal = bgTerrain;  /* 0xFE (ME_TRANSPARENT) if none */
    for (int x = 0; x < paddedW; x++) {
        for (int y = 0; y < paddedH; y++) {
            outTerrain[x][y] = bgVal;
        }
    }

    /* Threshold pixels: alpha > 128 = text terrain (offset by +1 for padding) */
    uint8_t *pixels = (uint8_t *)rgba->pixels;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t *p = pixels + y * rgba->pitch + x * 4;
            uint8_t alpha = p[3];
            if (alpha > 128) {
                outTerrain[x + 1][y + 1] = textTerrain;
            }
        }
    }
    SDL_DestroySurface(rgba);

    /* Edge pass — shared with bitmap version */
    textApplyEdge(outTerrain, paddedW, paddedH, textTerrain, edgeTerrain);

    *outW = paddedW;
    *outH = paddedH;
    return true;
}

#else /* Non-desktop stub */

bool textRasterizeTTF(const char *fontPath, int pixelHeight,
                      const char *text,
                      BYTE textTerrain, BYTE edgeTerrain, BYTE bgTerrain,
                      BYTE outTerrain[256][256], int *outW, int *outH) {
    (void)fontPath; (void)pixelHeight;
    (void)text; (void)textTerrain; (void)edgeTerrain; (void)bgTerrain;
    (void)outTerrain; (void)outW; (void)outH;
    return false;
}

#endif
