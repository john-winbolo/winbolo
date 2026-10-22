/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Log Viewer HUD panel sums
 *Filename:      lv_hud.h
 *Purpose:
 *  The parts of the Log Viewer's HUD panel (imgui/imgui_hud.cpp)
 *  that are plain arithmetic: which icon a tank, pillbox or
 *  base slot shows, how the three grids are laid out in the
 *  room the window has, and how a status icon becomes a grey
 *  shape that takes a team colour.
 *
 *  The panel copies the game's three status grids. The game
 *  colours an icon by what the thing is to the player (own,
 *  allied, hostile). The viewer has no player, so it colours
 *  an owned thing by the team that owns it and keeps the
 *  game's own icons for neutral, dead and off the map.
 *
 *  When a tank is clicked on, the viewer does have a player:
 *  the perspective. The panel then draws the game's own icons,
 *  picked by the game's rules (lvHudGameTankIcon and the two
 *  beside it).
 *
 *  Header only, with no includes but the C library's, so the
 *  viewer (C and C++) and the unit tests use the same sums.
 *********************************************************/

#ifndef LV_HUD_H
#define LV_HUD_H

#include <stdbool.h>
#include <stddef.h>

/* Slots in one grid: the game's panels have room for 16 of each. */
#define LV_HUD_SLOTS 16

/* What a slot shows. */
typedef enum LvHudIcon {
    LV_HUD_ICON_EMPTY = 0,     /* nothing: no such item, or it is off the map */
    LV_HUD_ICON_DEAD,          /* the game's white cross */
    LV_HUD_ICON_NEUTRAL,       /* the game's neutral icon, uncoloured */
    LV_HUD_ICON_OWNED,         /* a solid shape in the owner's team colour */
    LV_HUD_ICON_OWNED_IN_TANK  /* pillbox only: the small "carried" shape */
} LvHudIcon;

/* The grey shapes the owned icons are cut from, in the order they sit in the
 * strip texture draw.c builds (lv_drawGetHudShapesTexture). Each cell holds
 * one 12x12 status icon with LV_HUD_SHAPE_PAD black pixels all round, so a
 * sampler that reads a little past the icon reads black and not its
 * neighbour. */
#define LV_HUD_SHAPE_DISC         0  /* tanks and bases */
#define LV_HUD_SHAPE_SQUARE       1  /* a pillbox on the map */
#define LV_HUD_SHAPE_SMALL_SQUARE 2  /* a pillbox carried in a tank */
#define LV_HUD_SHAPE_COUNT        3
#define LV_HUD_SHAPE_PAD          1

/* What an owned thing is to the perspective player. */
typedef enum LvHudRelation {
    LV_HUD_REL_NONE = 0,  /* no perspective, or the thing has no owner */
    LV_HUD_REL_OWN,       /* the player's own */
    LV_HUD_REL_ALLIED,    /* an ally's */
    LV_HUD_REL_HOSTILE    /* anybody else's */
} LvHudRelation;

/* The game's own status icons, as the perspective view draws them. The panel
 * turns each into its place in the tile atlas (the STATUS_* numbers of
 * gui/tiles.h). */
typedef enum LvHudGameIcon {
    LV_HUD_GAME_NONE = 0,          /* nothing drawn */
    LV_HUD_GAME_DEAD,              /* STATUS_ITEM_DEAD */
    LV_HUD_GAME_TANK_SELF,         /* STATUS_TANK_SELF */
    LV_HUD_GAME_TANK_ALLIED,       /* STATUS_TANK_GOOD */
    LV_HUD_GAME_TANK_HOSTILE,      /* STATUS_TANK_EVIL */
    LV_HUD_GAME_PILL_NEUTRAL,      /* STATUS_PILLBOX_NEUTRAL */
    LV_HUD_GAME_PILL_OWN,          /* STATUS_PILLBOX_GOOD */
    LV_HUD_GAME_PILL_ALLIED,       /* STATUS_PILLBOX_ALLIEGOOD */
    LV_HUD_GAME_PILL_HOSTILE,      /* STATUS_PILLBOX_EVIL */
    LV_HUD_GAME_PILL_TANK_OWN,     /* STATUS_PILLBOX_TANKGOOD */
    LV_HUD_GAME_PILL_TANK_ALLIED,  /* STATUS_PILLBOX_TANKALLIE */
    LV_HUD_GAME_PILL_TANK_HOSTILE, /* STATUS_PILLBOX_TANKEVIL */
    LV_HUD_GAME_BASE_NEUTRAL,      /* STATUS_BASE_NEUTRAL */
    LV_HUD_GAME_BASE_OWN,          /* STATUS_BASE_GOOD */
    LV_HUD_GAME_BASE_ALLIED,       /* STATUS_BASE_ALLIEGOOD */
    LV_HUD_GAME_BASE_HOSTILE       /* STATUS_BASE_EVIL */
} LvHudGameIcon;

/* One slot of one grid, copied out of the decoder. */
typedef struct LvHudSlot {
    unsigned char icon;    /* LvHudIcon */
    unsigned char owner;   /* player slot that owns it; the tank's own slot */
    unsigned char team;    /* the owner's team, NEUTRAL_TEAM when it has none */
    unsigned char hasTeam; /* 0 when the owner is in no team */
    unsigned char colour;  /* the palette entry the team is drawn in (tc[team]) */
    unsigned char relation; /* LvHudRelation; NONE when no perspective is on */
    unsigned char chip;    /* LvBotChip of the tank's player; NONE for a pillbox or a base */
} LvHudSlot;

/* The longest player name the snapshot carries, with its NUL. A longer name
 * is cut. */
#define LV_HUD_NAME_LEN 64

/* Everything the panel draws in one frame. */
typedef struct LvHudSnapshot {
    LvHudSlot tanks[LV_HUD_SLOTS];
    LvHudSlot pills[LV_HUD_SLOTS];
    LvHudSlot bases[LV_HUD_SLOTS];
    unsigned char perspective;         /* 1 while one player's view is shown */
    unsigned char viewer;              /* that player's slot */
    char viewerName[LV_HUD_NAME_LEN];  /* and name */
    unsigned char viewerChip;          /* and LvBotChip */
    unsigned char hasSelected;         /* 1 while a tank is selected */
    unsigned char selected;            /* its slot */
} LvHudSnapshot;

/* The icon for a base slot. The order is the game's (lv_basesGetAlliancePos):
 * a base with no armour is dead whoever owns it. */
static inline LvHudIcon lvHudBaseIcon(bool active, bool neutral, int armour) {
    if (!active) return LV_HUD_ICON_EMPTY;
    if (armour == 0) return LV_HUD_ICON_DEAD;
    if (neutral) return LV_HUD_ICON_NEUTRAL;
    return LV_HUD_ICON_OWNED;
}

/* The icon for a pillbox slot. The order is the game's
 * (lv_pillsGetAllianceNum): a pillbox with no armour is dead only while it is
 * on the map, because one carried in a tank has no armour to speak of. */
static inline LvHudIcon lvHudPillIcon(bool active, bool neutral, int armour,
                                      bool inTank) {
    if (!active) return LV_HUD_ICON_EMPTY;
    if (armour == 0 && !inTank) return LV_HUD_ICON_DEAD;
    if (neutral) return LV_HUD_ICON_NEUTRAL;
    return inTank ? LV_HUD_ICON_OWNED_IN_TANK : LV_HUD_ICON_OWNED;
}

/* The icon for a tank slot. The game's panel knows only whether a player is
 * in the slot; it does not mark a tank that is waiting to come back. */
static inline LvHudIcon lvHudTankIcon(bool inUse) {
    return inUse ? LV_HUD_ICON_OWNED : LV_HUD_ICON_EMPTY;
}

/* What a thing owned by a player is to the perspective player. The order is
 * the game's, the same for tanks, pillboxes and bases: the player's own
 * first, then an ally's, and everybody else's is hostile. A thing with no
 * owner has no relation. */
static inline LvHudRelation lvHudRelation(bool hasOwner, bool ownerIsViewer,
                                          bool ownerIsAlly) {
    if (!hasOwner) return LV_HUD_REL_NONE;
    if (ownerIsViewer) return LV_HUD_REL_OWN;
    if (ownerIsAlly) return LV_HUD_REL_ALLIED;
    return LV_HUD_REL_HOSTILE;
}

/* The game's icon for a tank slot (playersScreenAllience, drawn by
 * sdl3DrawStatusTank): the player's own tank, an ally's, a hostile one, and
 * nothing for a slot with no player. */
static inline LvHudGameIcon lvHudGameTankIcon(LvHudIcon icon, LvHudRelation rel) {
    if (icon != LV_HUD_ICON_OWNED) return LV_HUD_GAME_NONE;
    if (rel == LV_HUD_REL_OWN) return LV_HUD_GAME_TANK_SELF;
    if (rel == LV_HUD_REL_ALLIED) return LV_HUD_GAME_TANK_ALLIED;
    return LV_HUD_GAME_TANK_HOSTILE;
}

/* The game's icon for a pillbox slot (pillsGetAllianceNum, drawn by
 * sdl3DrawStatusPillbox). Dead and neutral look the same to every player. An
 * owned pillbox has one icon on the map and another when a tank carries it,
 * each in own, allied and hostile. */
static inline LvHudGameIcon lvHudGamePillIcon(LvHudIcon icon, LvHudRelation rel) {
    bool inTank = (icon == LV_HUD_ICON_OWNED_IN_TANK);
    if (icon == LV_HUD_ICON_EMPTY) return LV_HUD_GAME_NONE;
    if (icon == LV_HUD_ICON_DEAD) return LV_HUD_GAME_DEAD;
    if (icon == LV_HUD_ICON_NEUTRAL || rel == LV_HUD_REL_NONE) return LV_HUD_GAME_PILL_NEUTRAL;
    if (rel == LV_HUD_REL_OWN) return inTank ? LV_HUD_GAME_PILL_TANK_OWN : LV_HUD_GAME_PILL_OWN;
    if (rel == LV_HUD_REL_ALLIED) return inTank ? LV_HUD_GAME_PILL_TANK_ALLIED : LV_HUD_GAME_PILL_ALLIED;
    return inTank ? LV_HUD_GAME_PILL_TANK_HOSTILE : LV_HUD_GAME_PILL_HOSTILE;
}

/* The game's icon for a base slot (basesGetStatusNum, drawn by
 * sdl3DrawStatusBase). */
static inline LvHudGameIcon lvHudGameBaseIcon(LvHudIcon icon, LvHudRelation rel) {
    if (icon == LV_HUD_ICON_EMPTY) return LV_HUD_GAME_NONE;
    if (icon == LV_HUD_ICON_DEAD) return LV_HUD_GAME_DEAD;
    if (icon == LV_HUD_ICON_NEUTRAL || rel == LV_HUD_REL_NONE) return LV_HUD_GAME_BASE_NEUTRAL;
    if (rel == LV_HUD_REL_OWN) return LV_HUD_GAME_BASE_OWN;
    if (rel == LV_HUD_REL_ALLIED) return LV_HUD_GAME_BASE_ALLIED;
    return LV_HUD_GAME_BASE_HOSTILE;
}

/* One pixel of a status icon as a grey level. The icons are pure green or
 * pure red on black, and a colour multiplied into green gives black for a
 * blue team, so the shape is kept as its brightest channel and stretched so
 * the brightest pixel of the icon (cellMax) is white. The team colour
 * multiplied into that comes out as the team colour itself. */
static inline unsigned char lvHudGreyValue(int r, int g, int b, int cellMax) {
    int v = r;
    if (g > v) v = g;
    if (b > v) v = b;
    if (cellMax <= 0 || v <= 0) return 0;
    if (v >= cellMax) return 255;
    return (unsigned char)((v * 255 + cellMax / 2) / cellMax);
}

/* The grids are drawn on black, where a dark team colour (the palette's blue
 * is 0,0,189) is hard to see. A colour darker than LV_HUD_MIN_LUMA is mixed
 * with white until it is that bright; its hue stays. Channels are 0..1. */
#define LV_HUD_MIN_LUMA 0.35f
static inline void lvHudReadableColour(float *r, float *g, float *b) {
    float luma = 0.299f * *r + 0.587f * *g + 0.114f * *b;
    if (luma >= LV_HUD_MIN_LUMA || luma >= 1.0f) return;
    float t = (LV_HUD_MIN_LUMA - luma) / (1.0f - luma);
    *r += (1.0f - *r) * t;
    *g += (1.0f - *g) * t;
    *b += (1.0f - *b) * t;
}

/* The largest scale lvHudFit tries. */
#define LV_HUD_MAX_SCALE 8

/* How the three grids are laid out: all in one row (columns 3) or one above
 * the other (columns 1), each drawn at a whole-number scale. */
typedef struct LvHudFit {
    int columns;
    int scale;
} LvHudFit;

/* The room the three grids take, labels and gaps included. panelW and panelH
 * are one grid at scale 1, labelH the height of the name above each grid,
 * gap the space between two grids. */
static inline void lvHudContentSize(int columns, int scale, int panelW,
                                    int panelH, int labelH, int gap,
                                    int *outW, int *outH) {
    int rows = (columns == 3) ? 1 : 3;
    if (outW) *outW = columns * scale * panelW + (columns - 1) * gap;
    if (outH) *outH = rows * (labelH + scale * panelH) + (rows - 1) * gap;
}

/* The largest scale at which that layout fits, 0 when even scale 1 does not. */
static inline int lvHudMaxScale(int columns, int availW, int availH,
                                int panelW, int panelH, int labelH, int gap) {
    int best = 0;
    for (int s = 1; s <= LV_HUD_MAX_SCALE; s++) {
        int w, h;
        lvHudContentSize(columns, s, panelW, panelH, labelH, gap, &w, &h);
        if (w > availW || h > availH) break;
        best = s;
    }
    return best;
}

/* Pick the layout for the room there is. The one that draws the icons larger
 * wins; a tie goes to the row, which is the shorter of the two. When neither
 * fits at scale 1 the width alone decides, at scale 1: the row if the window
 * is wide enough for it, else the column. */
static inline LvHudFit lvHudFit(int availW, int availH, int panelW, int panelH,
                                int labelH, int gap) {
    LvHudFit fit;
    int row = lvHudMaxScale(3, availW, availH, panelW, panelH, labelH, gap);
    int col = lvHudMaxScale(1, availW, availH, panelW, panelH, labelH, gap);
    if (row == 0 && col == 0) {
        int rowW;
        lvHudContentSize(3, 1, panelW, panelH, labelH, gap, &rowW, NULL);
        fit.columns = (rowW <= availW) ? 3 : 1;
        fit.scale = 1;
    } else if (row >= col) {
        fit.columns = 3;
        fit.scale = row;
    } else {
        fit.columns = 1;
        fit.scale = col;
    }
    return fit;
}

#endif /* LV_HUD_H */
