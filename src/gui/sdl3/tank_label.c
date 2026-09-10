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
#include "sdl3imgui.h"       /* sdl3ImguiPlayerIsBot, sdl3ImguiGetBrainIconSurface,
                                the voice-state accessors and MicIconGlyph */
#include "sdl3draw_status.h" /* sdl3DrawOnRenderThread, the mic-icon setting */
#if defined(WINBOLO_VOICE)
#include "../voice.h"        /* who is talking, and who this client has muted */
#include "player_flags.h"    /* PLAYER_FLAG_HAS_MIC, PLAYER_FLAG_VOICE_MUTED */
#endif

#if defined(WINBOLO_VOICE)
/* Alpha the voice glyphs are drawn at. They are drawn over their own shadow,
 * like the name, so they are set at full strength rather than the flag's 170:
 * the shadow is what makes them read over pale terrain, and dimming on top of
 * it only takes the contrast back off. Self-muted sits a little below talking
 * so the two are told apart by weight as well as by shape. */
#define TANK_LABEL_VOICE_ALPHA      255
#define TANK_LABEL_VOICE_ALPHA_DIM  200

/* What a player's voice state puts beside their tank, or nothing. */
typedef enum {
    TANK_VOICE_NONE,
    TANK_VOICE_TALKING,      /* plain speaker */
    TANK_VOICE_SELF_MUTED    /* barred microphone, dimmed */
} TankVoiceIcon;

/* Resolved in renderPlayerMicCell's precedence order, so the map and the
 * players panel never disagree about which state a player is in. Three of the
 * panel's five states draw nothing here: an icon beside every player all the
 * time is clutter mid-fight, and talking and self-muted are the two worth
 * interrupting the map for.
 *
 * A limit this inherits rather than introduces: in a running game the server
 * strips the microphone bits for players this client is not allied with
 * (server_sim_snapshot.c), mirroring where voice is actually routed. An
 * enemy's no-microphone and self-muted states are therefore not known here
 * and read as blank. */
static TankVoiceIcon tankVoiceIconFor(BYTE playerNum) {
    uint8_t flags = sdl3ImguiPlayerFlags(playerNum);

    /* A player this client has muted draws nothing, in every state. What is
     * worth marking beside a tank is someone saying something now, and for a
     * muted player that cannot be known here: their audio never reaches this
     * client, and the server's talking set is published in the lobby and the
     * countdown only — deliberately, because broadcasting it in a running game
     * would tell a player that an enemy is speaking. Their row in the players
     * panel is where being muted is shown, and where the click that undoes it
     * lives. Checked first, so a player who is muted here and self-muted at
     * their end draws nothing either. */
    if (voiceIsPlayerMuted((int)playerNum))         return TANK_VOICE_NONE;
    if ((flags & PLAYER_FLAG_HAS_MIC) == 0)         return TANK_VOICE_NONE;
    if ((voiceGetTalkingMap() & ((PlayerBitMap)1u << playerNum)) != 0)
                                                    return TANK_VOICE_TALKING;
    if ((flags & PLAYER_FLAG_VOICE_MUTED) != 0)     return TANK_VOICE_SELF_MUTED;
    return TANK_VOICE_NONE;
}

/* Textured per renderer beside the cached name and icon, and rasterized at
 * the height it is drawn at rather than scaled to it: the barred microphone
 * cuts its slash as thin negative space, and a draw-time downscale averages it
 * into grey until the icon stops reading as barred. Unlike the name and the
 * flag these are not part of the per-label rebuild — a player's voice state
 * changes from frame to frame, while the cache is keyed on the label string —
 * so the size is the key instead: one that holds re-rasterizes nothing, and a
 * font or zoom change re-rasterizes once.
 *
 * The tint is set once here rather than per draw, because each texture serves
 * exactly one state. */
static SDL_Texture *voiceTexFor(TankLabelCache *c, SDL_Renderer *r,
                                TankVoiceIcon which, int px) {
    SDL_Texture **slot;
    MicIconGlyph  glyph;
    SDL_Surface  *surf;

    if (c->voiceTexPx != px) {
        if (c->speakerTex)  { SDL_DestroyTexture(c->speakerTex);  c->speakerTex = NULL; }
        if (c->micMutedTex) { SDL_DestroyTexture(c->micMutedTex); c->micMutedTex = NULL; }
        c->voiceTexPx = px;
    }

    switch (which) {
        case TANK_VOICE_SELF_MUTED:
            slot = &c->micMutedTex; glyph = MIC_GLYPH_MIC_MUTED; break;
        default:
            slot = &c->speakerTex;  glyph = MIC_GLYPH_SPEAKER;   break;
    }
    if (*slot) return *slot;

    surf = sdl3ImguiCreateMicIconSurface(glyph, px);
    if (!surf) return NULL;
    *slot = SDL_CreateTextureFromSurface(r, surf);
    /* Ours to free — sdl3ImguiCreateMicIconSurface hands the surface over
     * rather than keeping it. */
    SDL_DestroySurface(surf);
    if (!*slot) return NULL;

    SDL_SetTextureBlendMode(*slot, SDL_BLENDMODE_BLEND);
    /* The label's own alphas, not the players panel's tints: the panel's were
     * picked against its dark background, and over the map the glyph has to
     * carry itself. Talking is drawn at full strength, self-muted a step below
     * it. */
    switch (which) {
        case TANK_VOICE_SELF_MUTED:
            SDL_SetTextureAlphaMod(*slot, TANK_LABEL_VOICE_ALPHA_DIM);
            break;
        default:
            SDL_SetTextureAlphaMod(*slot, TANK_LABEL_VOICE_ALPHA);
            break;
    }
    return *slot;
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

/* The cached name texture for one slot, rebuilding it first if anything it
 * was made from has changed under the cache. NULL when there is nothing to
 * draw. Shared by the two drawers below so they cannot drift apart on when a
 * texture is stale. */
static SDL_Texture *tankLabelNameTex(TankLabelCache *c, SDL_Renderer *r,
                                     TTF_Font *font, const char *label,
                                     BYTE playerNum) {
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
    if (!font) return NULL;

    if (strncmp(c->str[playerNum], label, TANK_LABEL_NAME_LEN - 1) != 0) {
        tankLabelRebuild(c, r, font, label, playerNum);
    }
    return c->nameTex[playerNum];
}

bool tankLabelDraw(TankLabelCache *c, SDL_Renderer *r, TTF_Font *font,
                   const char *label, BYTE playerNum,
                   float x, float y, float scale) {
    SDL_assert(sdl3DrawOnRenderThread());
    if (!r || !label || label[0] == '\0') return false;
    if (playerNum >= MAX_TANKS) return false;

    SDL_Texture *tex = tankLabelNameTex(c, r, font, label, playerNum);
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
    /* The player's voice state, after the flag/brain icon or straight after
     * the name when there was none. Never over our own tank: the game view's
     * own-microphone indicator carries that in game and the players list
     * carries it in the lobby, so a third copy here would say nothing new. */
    if (sdl3DrawStatusGetShowMicIcons() && !sdl3ImguiPlayerIsSelf(playerNum)) {
        TankVoiceIcon which = tankVoiceIconFor(playerNum);
        if (which != TANK_VOICE_NONE) {
            /* The glyphs are square, at the flag's height. Drawn at the
             * texture's own size rather than the size asked for — the same
             * number unless the rasterizer clamped it, and either way nothing
             * is resampled. */
            float voiceH = h * 0.75f;
            SDL_Texture *voiceTex =
                voiceTexFor(c, r, which, (int)(voiceH + 0.5f));
            if (voiceTex) {
                float vw = 0.0f, vh = 0.0f;
                SDL_GetTextureSize(voiceTex, &vw, &vh);
                /* Over the same black shadow the name uses, at the same
                 * offset, so the glyph reads over sea and over road alike.
                 * The colour mod goes back to white afterwards: the texture
                 * is cached across frames and players, and a black one left
                 * behind would draw the glyph black from the next label on. */
                SDL_FRect sd  = { iconX, y + (h - vh) * 0.5f, vw, vh };
                SDL_FRect vsh = { sd.x + shOff, sd.y + shOff, vw, vh };
                SDL_SetTextureColorMod(voiceTex, 0, 0, 0);
                SDL_RenderTexture(r, voiceTex, NULL, &vsh);
                SDL_SetTextureColorMod(voiceTex, 255, 255, 255);
                SDL_RenderTexture(r, voiceTex, NULL, &sd);
            }
        }
    }
#endif
    return true;
}

bool tankLabelDrawNameCentred(TankLabelCache *c, SDL_Renderer *r,
                              TTF_Font *font, const char *name, BYTE playerNum,
                              float cx, float y, float scale, float alpha) {
    SDL_assert(sdl3DrawOnRenderThread());
    if (!r || !name || name[0] == '\0') return false;
    if (playerNum >= MAX_TANKS) return false;
    if (alpha <= 0.0f) return false;
    if (alpha > 1.0f) alpha = 1.0f;

    SDL_Texture *tex = tankLabelNameTex(c, r, font, name, playerNum);
    if (!tex) return false;

    float texW = 0.0f, texH = 0.0f;
    SDL_GetTextureSize(tex, &texW, &texH);
    float w = texW * scale;
    float h = texH * scale;
    float x = cx - w * 0.5f;

    /* The same shadow tankLabelDraw draws, for the same reason: terrain runs
     * from black sea to pale road under it. Faded with the name rather than
     * held solid, so a marker on its way out goes quietly. */
    Uint8 a = (Uint8)(alpha * 255.0f + 0.5f);
    float shOff = h * (1.0f / 13.0f);
    if (shOff < 1.0f) shOff = 1.0f;
    SDL_FRect sh = { x + shOff, y + shOff, w, h };
    SDL_SetTextureAlphaMod(tex, a);
    SDL_SetTextureColorMod(tex, 0, 0, 0);
    SDL_RenderTexture(r, tex, NULL, &sh);
    SDL_FRect d = { x, y, w, h };
    SDL_SetTextureColorMod(tex, 255, 255, 255);
    SDL_RenderTexture(r, tex, NULL, &d);
    /* Back to opaque: the texture is cached across frames, and the other
     * drawer does not set an alpha of its own. */
    SDL_SetTextureAlphaMod(tex, 255);
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
    if (c->speakerTex) {
        SDL_DestroyTexture(c->speakerTex);
        c->speakerTex = NULL;
    }
    if (c->micMutedTex) {
        SDL_DestroyTexture(c->micMutedTex);
        c->micMutedTex = NULL;
    }
    c->voiceTexPx = 0;
#endif
    c->renderer = NULL;
    c->font     = NULL;
    c->fontSize = 0.0f;
}
