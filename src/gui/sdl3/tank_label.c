/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Tank labels
 *Filename:      tank_label.c
 *Purpose:
 *  See tank_label.h. Extracted from sdl3draw_status.c's
 *  sdl3DrawTankLabel so the classic view and the two
 *  overview hosts render labels through one body; the
 *  position maths stayed with each host, everything the
 *  label *is* moved here.
 *********************************************************/

#include <ctype.h>
#include <string.h>

#include "tank_label.h"
#include "flags.h"           /* flagsGetSurface */
#include "sdl3imgui.h"       /* sdl3ImguiPlayerIsBot, sdl3ImguiGetBrainIconSurface */
#include "sdl3draw_status.h" /* sdl3DrawOnRenderThread, the mic-icon setting */
#if defined(WINBOLO_VOICE)
#include "../voice.h"        /* talking map for the mic icon */
#endif

#if defined(WINBOLO_VOICE)
/* Textured per renderer beside the cached name and icon. Unlike those it is
 * not part of the per-label rebuild: who is talking changes from frame to
 * frame, while the cache is keyed on the label string. */
static SDL_Texture *micTexFor(TankLabelCache *c, SDL_Renderer *r) {
    if (c->micTex) return c->micTex;
    SDL_Surface *surf = sdl3ImguiGetMicIconSurface();
    if (!surf) return NULL;
    c->micTex = SDL_CreateTextureFromSurface(r, surf);
    if (c->micTex) {
        SDL_SetTextureBlendMode(c->micTex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureAlphaMod(c->micTex, 170);
    }
    return c->micTex;
}
#endif

/* Rebuild the cached textures for one player when the label string changed.
 * Also decides, once per change, whether an icon rides beside the name. */
static void tankLabelRebuild(TankLabelCache *c, SDL_Renderer *r,
                             TTF_Font *font, const char *label,
                             BYTE playerNum) {
    if (c->nameTex[playerNum]) {
        SDL_DestroyTexture(c->nameTex[playerNum]);
        c->nameTex[playerNum] = NULL;
    }
    if (c->iconTex[playerNum]) {
        SDL_DestroyTexture(c->iconTex[playerNum]);
        c->iconTex[playerNum] = NULL;
    }
    strncpy(c->str[playerNum], label, TANK_LABEL_NAME_LEN - 1);
    c->str[playerNum][TANK_LABEL_NAME_LEN - 1] = '\0';

    /* Split the label into the name and its trailing location. The label
     * builder appends "@" + location only in long-label mode, and player
     * names cannot contain '@', so the last '@' is the separator. A bot
     * gets the brain icon (its location, if any, is dropped); a human with
     * a 2-letter country code gets the flag. Anything else (short labels,
     * or our own "@This Computer") is left as plain text with no icon. */
    char nameOnly[TANK_LABEL_NAME_LEN];
    strncpy(nameOnly, label, TANK_LABEL_NAME_LEN - 1);
    nameOnly[TANK_LABEL_NAME_LEN - 1] = '\0';

    SDL_Surface *iconSurf = NULL;
    char *at = strrchr(nameOnly, '@');
    if (at) {
        const char *loc = at + 1;
        if (sdl3ImguiPlayerIsBot(playerNum) &&
            (iconSurf = sdl3ImguiGetBrainIconSurface()) != NULL) {
            *at = '\0';
        } else if (isalpha((unsigned char)loc[0]) &&
                   isalpha((unsigned char)loc[1]) && loc[2] == '\0' &&
                   (iconSurf = flagsGetSurface(loc)) != NULL) {
            *at = '\0';
        }
        /* No icon available (flags not loaded, unknown country, …): leave
         * nameOnly as the full "name@loc" string so the label is unchanged. */
    }

    SDL_Color fg = {200, 200, 200, 255};
    SDL_Surface *sFg = TTF_RenderText_Blended(font, nameOnly, 0, fg);
    if (sFg) {
        c->nameTex[playerNum] = SDL_CreateTextureFromSurface(r, sFg);
        SDL_DestroySurface(sFg);
        if (c->nameTex[playerNum]) {
            SDL_SetTextureBlendMode(c->nameTex[playerNum], SDL_BLENDMODE_BLEND);
        }
    }
    if (iconSurf) {
        c->iconTex[playerNum] = SDL_CreateTextureFromSurface(r, iconSurf);
        if (c->iconTex[playerNum]) {
            /* The icon sits unobtrusively beside the name: partial alpha,
             * set once — the texture is the cache's own, not the shared
             * chat/lobby render the old pass borrowed. */
            SDL_SetTextureBlendMode(c->iconTex[playerNum], SDL_BLENDMODE_BLEND);
            SDL_SetTextureAlphaMod(c->iconTex[playerNum], 170);
        }
    }
}

bool tankLabelDraw(TankLabelCache *c, SDL_Renderer *r, TTF_Font *font,
                   const char *label, BYTE playerNum,
                   float x, float y, float scale) {
    SDL_assert(sdl3DrawOnRenderThread());
    if (!r || !label || label[0] == '\0') return false;
    if (playerNum >= MAX_TANKS) return false;

    /* A zoom change reopens the fonts; a pop-out close/reopen changes the
     * renderer. Either way every cached texture is stale at once.
     *
     * The pixel size is in the key as well as the pointer. A zoom change
     * closes the face and reopens it at the new size, and the reopened face
     * can be allocated at the address the closed one had. The pointer then
     * compares equal, nothing is flushed, and because the cached label
     * strings survive too, every texture goes on drawing glyphs rendered at
     * the old size until the player's label text happens to change. */
    float fontSize = font ? TTF_GetFontSize(font) : 0.0f;
    if (r != c->renderer || font != c->font || fontSize != c->fontSize) {
        tankLabelCacheFlush(c);
        c->renderer = r;
        c->font     = font;
        c->fontSize = fontSize;
    }
    if (!font) return false;

    if (strncmp(c->str[playerNum], label, TANK_LABEL_NAME_LEN - 1) != 0) {
        tankLabelRebuild(c, r, font, label, playerNum);
    }

    SDL_Texture *tex = c->nameTex[playerNum];
    if (!tex) return false;

    float texW = 0.0f, texH = 0.0f;
    SDL_GetTextureSize(tex, &texW, &texH);
    float w = texW * scale;
    float h = texH * scale;

    /* Terrain runs from black sea to pale road under the same label, so the
     * name is drawn over its own shadow rather than trusting one colour to
     * read against all of it. The shadow is the glyph texture colour-modded
     * black, offset about a glyph pixel of the 13 px face. */
    float shOff = h * (1.0f / 13.0f);
    if (shOff < 1.0f) shOff = 1.0f;
    SDL_FRect sh = { x + shOff, y + shOff, w, h };
    SDL_SetTextureColorMod(tex, 0, 0, 0);
    SDL_RenderTexture(r, tex, NULL, &sh);
    SDL_FRect d = { x, y, w, h };
    SDL_SetTextureColorMod(tex, 255, 255, 255);
    SDL_RenderTexture(r, tex, NULL, &d);

    /* Country flag (humans) or brain icon (bots) just after the name, at
     * 75% of the text height, centred on the line. */
    float gap   = h * (2.0f / 13.0f);
    float iconX = x + w + gap;   /* advances past each icon drawn */
    SDL_Texture *icon = c->iconTex[playerNum];
    if (icon) {
        float iw = 0.0f, ih = 0.0f;
        SDL_GetTextureSize(icon, &iw, &ih);
        if (ih > 0.0f) {
            float iconH = h * 0.75f;
            float iconW = iconH * iw / ih;
            SDL_FRect id = { iconX, y + (h - iconH) * 0.5f, iconW, iconH };
            SDL_RenderTexture(r, icon, NULL, &id);
            iconX += iconW + gap;
        }
    }

#if defined(WINBOLO_VOICE)
    /* Microphone while this player's voice is being heard here, after the
     * flag/brain icon or straight after the name when there was none. The
     * talking map is local — a player muted here never appears in it — so
     * there is no muted state to draw. */
    if (sdl3DrawStatusGetShowMicIcons() &&
        (voiceGetTalkingMap() & ((PlayerBitMap)1u << playerNum)) != 0) {
        SDL_Texture *mic = micTexFor(c, r);
        if (mic) {
            float micH = h * 0.75f;  /* the mic icon is square */
            SDL_FRect md = { iconX, y + (h - micH) * 0.5f, micH, micH };
            SDL_RenderTexture(r, mic, NULL, &md);
        }
    }
#endif
    return true;
}

void tankLabelCacheFlush(TankLabelCache *c) {
    for (int i = 0; i < MAX_TANKS; i++) {
        if (c->nameTex[i]) {
            SDL_DestroyTexture(c->nameTex[i]);
            c->nameTex[i] = NULL;
        }
        if (c->iconTex[i]) {
            SDL_DestroyTexture(c->iconTex[i]);
            c->iconTex[i] = NULL;
        }
        c->str[i][0] = '\0';
    }
#if defined(WINBOLO_VOICE)
    if (c->micTex) {
        SDL_DestroyTexture(c->micTex);
        c->micTex = NULL;
    }
#endif
    c->renderer = NULL;
    c->font     = NULL;
    c->fontSize = 0.0f;
}
