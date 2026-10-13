/*
 * imgui_hud.cpp - ImGui HUD window for Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

/*
 * The game's three status grids (tanks, pillboxes, bases) as a panel of the
 * viewer. The grids are the game's: the same 90x66 black panel, the same
 * slot places (sdl3StatusItemPos), the same tile in the middle and the same
 * 12x12 icons out of the tile atlas.
 *
 * The one difference is colour. The game colours an icon by what the thing
 * is to the player: green for own and allied, red for hostile. The viewer
 * watches every team at once, so an owned thing is drawn as the solid shape
 * of its kind in the colour of the team that owns it. Neutral, dead and
 * empty slots are the game's, untouched.
 *
 * A click on a tank picks that player as the perspective. The grids then are
 * the game's in colour too: every icon is the one that player's game draws,
 * straight out of the atlas (lv_hud.h has the rules). The map follows
 * (draw.c). A click on the same tank, or the X at the end of the line above
 * the grids, goes back.
 */

#include "imgui_hud.h"
#include "imgui_main_menu.h"
#include "imgui_dialogs.h"
#include "imgui_dock.h"
#include "imgui_context.h"
#include "imgui_lv_icons.h"
#include "imgui.h"
#include "../../gui/lang.h"
#include "../../gui/sdl3/dialogs/imgui_dialog_utils.h"
#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" {
    #include "../lv_hud.h"
    #include "../../gui/tiles.h"
    #include "../../gui/positions.h"
    #include "../../gui/sdl3/map_colours.h"  /* mapColourTeam */

    /* External functions from backend - using C types directly. The viewer's
     * own headers cannot be included here beside lang.h: its lv_global.h and
     * the game's global.h, which lang.h brings in, share an include guard. */
    void lv_clientMutexWaitFor(void);
    void lv_clientMutexRelease(void);
    void lv_screenHudSnapshot(LvHudSnapshot *out);
    void lv_screenSelectItem(unsigned char itemType, unsigned char itemNum);
    void lv_screenGetPlayerName(char *buffer, unsigned char player, size_t destSize);
    void lv_screenTogglePerspective(unsigned char playerNum);
    void lv_screenClearPerspective(void);
    void lv_screenDeselectTank(void);
    bool lv_screenCheckPerspective(void);
    void lv_drawDirtyScreen(void);
    SDL_Texture *lv_drawGetTilesTexture(void);
    SDL_Texture *lv_drawGetHudShapesTexture(void);
    /* sdl3draw_status.h: where slot n (1..16) sits in a status panel. */
    void sdl3StatusItemPos(int n, int type, int *outX, int *outY);
}

/* The three grids, in the order the game stacks them down its side column.
 * The numbers are also sdl3StatusItemPos's panel types. */
enum { HUD_GRID_TANKS = 2, HUD_GRID_PILLS = 1, HUD_GRID_BASES = 0 };
static const int k_grid_order[3] = { HUD_GRID_TANKS, HUD_GRID_PILLS, HUD_GRID_BASES };

/* One grid at scale 1. The three panels are the same size in the game. */
#define HUD_PANEL_W STATUS_TANKS_WIDTH
#define HUD_PANEL_H STATUS_TANKS_HEIGHT

/* The scale the floating window opens at: 2, one grid above the other, the
 * shape of the game's own column. */
#define HUD_DEFAULT_SCALE 2

/* The thin edge round each grid. The game's panels sit in the grey bevel of
 * its background picture, which is one picture for the whole window and
 * cannot be cut up a panel at a time; this stands in for it. */
#define HUD_EDGE_COLOUR IM_COL32(110, 110, 110, 255)

/* The room the name above a grid takes, and the space between two grids. */
static int hud_label_h(void) {
    return (int)ceilf(ImGui::GetTextLineHeight()) + 2;
}
static int hud_gap(void) {
    return (int)ImGui::GetStyle().ItemSpacing.x;
}
/* The room the line above the grids takes: the one that says whose view the
 * grids show. It is kept in both views, so the grids do not move, or change
 * size, when a tank is clicked on. */
static int hud_header_h(void) {
    return (int)ceilf(ImGui::GetTextLineHeightWithSpacing());
}

float lv_imgui_hud_dock_height(float dockWidth) {
    const ImGuiStyle &style = ImGui::GetStyle();
    int availW = (int)(dockWidth - style.WindowPadding.x * 2.0f);
    int w, h;
    /* With no height to fit, lvHudFit picks by the width alone, at scale 1:
     * the question here is what height to give, not what fits in one. */
    LvHudFit fit = lvHudFit(availW, 0, HUD_PANEL_W, HUD_PANEL_H, hud_label_h(), hud_gap());
    lvHudContentSize(fit.columns, 1, HUD_PANEL_W, HUD_PANEL_H, hud_label_h(), hud_gap(), &w, &h);
    /* The content with the line above it, the padding above and below, and
     * the tab a docked window has for a title. */
    return (float)(h + hud_header_h()) + style.WindowPadding.y * 2.0f + ImGui::GetFrameHeight();
}

/* Draw one cell of a texture over the rectangle p0..p1. The source is in
 * texture pixels. */
static void hud_image(ImDrawList *dl, SDL_Texture *tex, float sx, float sy,
                      float sw, float sh, ImVec2 p0, ImVec2 p1, ImU32 tint) {
    float tw = 0.0f, th = 0.0f;
    if (tex == nullptr || !SDL_GetTextureSize(tex, &tw, &th) || tw <= 0.0f || th <= 0.0f) {
        return;
    }
    dl->AddImage((ImTextureID)(size_t)tex, p0, p1,
                 ImVec2(sx / tw, sy / th), ImVec2((sx + sw) / tw, (sy + sh) / th), tint);
}

/* The colour an owned slot is drawn in with all teams shown: its team's,
 * lifted where it would be too dark to see on the black panel. A red team is
 * drawn orange: red is what the game's HUD keeps for hostile. */
static ImU32 hud_team_colour(const LvHudSlot &slot) {
    SDL_FColor c = mapColourTeam(slot.colour);
    if (!lvHudRedToOrange(&c.r, &c.g, &c.b)) {
        lvHudReadableColour(&c.r, &c.g, &c.b);
    }
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, 1.0f));
}

/* Where one of the game's status icons is in the tile atlas. False for the
 * one that draws nothing. */
static bool hud_game_icon_pos(LvHudGameIcon icon, float *sx, float *sy) {
    switch (icon) {
    case LV_HUD_GAME_DEAD:              *sx = STATUS_ITEM_DEAD_X;          *sy = STATUS_ITEM_DEAD_Y;          return true;
    case LV_HUD_GAME_TANK_SELF:         *sx = STATUS_TANK_SELF_X;          *sy = STATUS_TANK_SELF_Y;          return true;
    case LV_HUD_GAME_TANK_ALLIED:       *sx = STATUS_TANK_GOOD_X;          *sy = STATUS_TANK_GOOD_Y;          return true;
    case LV_HUD_GAME_TANK_HOSTILE:      *sx = STATUS_TANK_EVIL_X;          *sy = STATUS_TANK_EVIL_Y;          return true;
    case LV_HUD_GAME_PILL_NEUTRAL:      *sx = STATUS_PILLBOX_NEUTRAL_X;    *sy = STATUS_PILLBOX_NEUTRAL_Y;    return true;
    case LV_HUD_GAME_PILL_OWN:          *sx = STATUS_PILLBOX_GOOD_X;       *sy = STATUS_PILLBOX_GOOD_Y;       return true;
    case LV_HUD_GAME_PILL_ALLIED:       *sx = STATUS_PILLBOX_ALLIEGOOD_X;  *sy = STATUS_PILLBOX_ALLIEGOOD_Y;  return true;
    case LV_HUD_GAME_PILL_HOSTILE:      *sx = STATUS_PILLBOX_EVIL_X;       *sy = STATUS_PILLBOX_EVIL_Y;       return true;
    case LV_HUD_GAME_PILL_TANK_OWN:     *sx = STATUS_PILLBOX_TANKGOOD_X;   *sy = STATUS_PILLBOX_TANKGOOD_Y;   return true;
    case LV_HUD_GAME_PILL_TANK_ALLIED:  *sx = STATUS_PILLBOX_TANKALLIE_X;  *sy = STATUS_PILLBOX_TANKALLIE_Y;  return true;
    case LV_HUD_GAME_PILL_TANK_HOSTILE: *sx = STATUS_PILLBOX_TANKEVIL_X;   *sy = STATUS_PILLBOX_TANKEVIL_Y;   return true;
    case LV_HUD_GAME_BASE_NEUTRAL:      *sx = STATUS_BASE_NEUTRAL_X;       *sy = STATUS_BASE_NEUTRAL_Y;       return true;
    case LV_HUD_GAME_BASE_OWN:          *sx = STATUS_BASE_GOOD_X;          *sy = STATUS_BASE_GOOD_Y;          return true;
    case LV_HUD_GAME_BASE_ALLIED:       *sx = STATUS_BASE_ALLIEGOOD_X;     *sy = STATUS_BASE_ALLIEGOOD_Y;     return true;
    case LV_HUD_GAME_BASE_HOSTILE:      *sx = STATUS_BASE_EVIL_X;          *sy = STATUS_BASE_EVIL_Y;          return true;
    default:
        return false;
    }
}

/* The icon of one slot, over p0..p1. With a perspective on it is the game's
 * own icon for that player, as it is in the atlas. */
static void hud_slot_icon(ImDrawList *dl, int grid, const LvHudSlot &slot,
                          bool perspective, ImVec2 p0, ImVec2 p1) {
    const float iw = (float)STATUS_ITEM_SIZE_X;
    const float ih = (float)STATUS_ITEM_SIZE_Y;
    SDL_Texture *atlas = lv_drawGetTilesTexture();

    if (perspective) {
        LvHudIcon icon = (LvHudIcon)slot.icon;
        LvHudRelation rel = (LvHudRelation)slot.relation;
        LvHudGameIcon game = (grid == HUD_GRID_TANKS) ? lvHudGameTankIcon(icon, rel) :
                             (grid == HUD_GRID_PILLS) ? lvHudGamePillIcon(icon, rel) :
                                                        lvHudGameBaseIcon(icon, rel);
        float sx, sy;
        if (hud_game_icon_pos(game, &sx, &sy)) {
            hud_image(dl, atlas, sx, sy, iw, ih, p0, p1, IM_COL32_WHITE);
        }
        return;
    }

    switch (slot.icon) {
    case LV_HUD_ICON_DEAD:
        hud_image(dl, atlas, STATUS_ITEM_DEAD_X, STATUS_ITEM_DEAD_Y, iw, ih, p0, p1,
                  IM_COL32_WHITE);
        break;
    case LV_HUD_ICON_NEUTRAL:
        if (grid == HUD_GRID_PILLS) {
            hud_image(dl, atlas, STATUS_PILLBOX_NEUTRAL_X, STATUS_PILLBOX_NEUTRAL_Y,
                      iw, ih, p0, p1, IM_COL32_WHITE);
        } else {
            hud_image(dl, atlas, STATUS_BASE_NEUTRAL_X, STATUS_BASE_NEUTRAL_Y,
                      iw, ih, p0, p1, IM_COL32_WHITE);
        }
        break;
    case LV_HUD_ICON_OWNED:
    case LV_HUD_ICON_OWNED_IN_TANK: {
        int shape = LV_HUD_SHAPE_DISC;
        if (grid == HUD_GRID_PILLS) {
            shape = (slot.icon == LV_HUD_ICON_OWNED_IN_TANK) ? LV_HUD_SHAPE_SMALL_SQUARE
                                                             : LV_HUD_SHAPE_SQUARE;
        }
        ImU32 colour = hud_team_colour(slot);
        SDL_Texture *shapes = lv_drawGetHudShapesTexture();
        if (shapes != nullptr) {
            float cell = iw + 2.0f * LV_HUD_SHAPE_PAD;
            hud_image(dl, shapes, shape * cell + LV_HUD_SHAPE_PAD, (float)LV_HUD_SHAPE_PAD,
                      iw, ih, p0, p1, colour);
        } else if (shape == LV_HUD_SHAPE_DISC) {
            /* No strip (it could not be built): a plain shape still says
             * whose the thing is. */
            dl->AddCircleFilled(ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f),
                                (p1.x - p0.x) * 0.5f, colour);
        } else {
            float inset = (shape == LV_HUD_SHAPE_SMALL_SQUARE) ? (p1.x - p0.x) * 0.25f : 0.0f;
            dl->AddRectFilled(ImVec2(p0.x + inset, p0.y + inset),
                              ImVec2(p1.x - inset, p1.y - inset), colour);
        }
        break;
    }
    default:
        break;
    }
}

/* "Alice (Blue)" for the player in a slot: the name, and the colour of the
 * team when the player is in one. */
static void hud_owner_text(const LvHudSlot &slot, char *out, size_t outSize) {
    char name[PLAYER_NAME_LEN] = "";

    lv_clientMutexWaitFor();
    lv_screenGetPlayerName(name, slot.owner, sizeof(name));
    lv_clientMutexRelease();
    if (name[0] == '\0') {
        snprintf(name, sizeof(name), "%d", (int)slot.owner);
    }

    const char *team = slot.hasTeam ? lv_imgui_team_colour_name(slot.colour) : "";
    if (team[0] == '\0') {
        snprintf(out, outSize, "%s", name);
        return;
    }
    MessageArgs args = {};
    snprintf(args.string1, sizeof(args.string1), "%s", name);
    snprintf(args.string2, sizeof(args.string2), "%s", team);
    snprintf(out, outSize, "%s", langGetTextFmt(STR_LV_HUD_NAME_TEAM, &args));
}

/* The tooltip of one slot: which item it is and whose. The numbers count
 * from 0, as the Item Information panel and the owner numbers do. */
static void hud_slot_tooltip(int grid, int index, const LvHudSlot &slot) {
    char item[64];
    char tip[LANG_MSGARG_STRING_LEN];
    char who[LANG_MSGARG_STRING_LEN];
    MessageArgs args = {};

    args.number = index;
    snprintf(item, sizeof(item), "%s",
             langGetTextFmt(grid == HUD_GRID_TANKS ? STR_TABLET_TANK_FMT :
                            grid == HUD_GRID_PILLS ? STR_MAPEDIT_INSP_PILL_HASH :
                                                     STR_MAPEDIT_INSP_BASE_HASH, &args));

    langid fmt = STR_LV_HUD_TIP_STATE;
    if (slot.icon == LV_HUD_ICON_DEAD) {
        snprintf(who, sizeof(who), "%s", langGetText(STR_LV_HUD_DEAD));
    } else if (slot.icon == LV_HUD_ICON_NEUTRAL) {
        snprintf(who, sizeof(who), "%s", langGetText(STR_MAPEDIT_OWNER_NEUTRAL));
    } else {
        hud_owner_text(slot, who, sizeof(who));
        /* A tank is its player; a pillbox or a base has one for an owner. */
        if (grid != HUD_GRID_TANKS) fmt = STR_LV_HUD_TIP_OWNER;
    }

    args = {};
    snprintf(args.string1, sizeof(args.string1), "%s", item);
    snprintf(args.string2, sizeof(args.string2), "%s", who);
    snprintf(tip, sizeof(tip), "%s", langGetTextFmt(fmt, &args));

    /* With a perspective on, what the thing is to that player. */
    if (slot.relation != LV_HUD_REL_NONE && slot.icon != LV_HUD_ICON_DEAD &&
        slot.icon != LV_HUD_ICON_NEUTRAL) {
        args = {};
        snprintf(args.string1, sizeof(args.string1), "%s", tip);
        snprintf(args.string2, sizeof(args.string2), "%s",
                 langGetText(slot.relation == LV_HUD_REL_OWN    ? STR_LV_HUD_REL_OWN :
                             slot.relation == LV_HUD_REL_ALLIED ? STR_LV_HUD_REL_ALLIED :
                                                                  STR_LV_HUD_REL_HOSTILE));
        snprintf(tip, sizeof(tip), "%s", langGetTextFmt(STR_LV_HUD_TIP_REL, &args));
    }
    /* A tank that a bot drives has the game's chip in front of the line. */
    if (grid == HUD_GRID_TANKS && lv_bot_chip_width(slot.chip) > 0.0f) {
        if (ImGui::BeginTooltip()) {
            lv_bot_chip_text(slot.chip);
            ImGui::SameLine(0.0f, lv_bot_chip_gap());
            ImGui::TextUnformatted(tip);
            ImGui::EndTooltip();
        }
        return;
    }
    ImGui::SetTooltip("%s", tip);
}

/* A click on a slot. A pillbox or a base is selected, as an information
 * click on it on the map selects it. A tank picks its player as the
 * perspective, in either mode, and the tank that has it already gives it up.
 * The view takes that tank's side, as a Select Team click on the tank on the
 * map does. */
static void hud_slot_click(int grid, int index) {
    if (grid == HUD_GRID_TANKS) {
        lv_clientMutexWaitFor();
        lv_screenTogglePerspective((unsigned char)index);
        lv_clientMutexRelease();
        lv_drawDirtyScreen();
        return;
    }
    lv_clientMutexWaitFor();
    lv_screenSelectItem((unsigned char)(grid == HUD_GRID_BASES ? 1 : 2), (unsigned char)index);
    lv_clientMutexRelease();
}

/* One grid: its name, the black panel, the tile in the middle and the 16
 * slots. origin is the top left of the name; scale is whole. */
static void hud_grid(int grid, const LvHudSlot *slots, bool perspective,
                     ImVec2 origin, int scale, int selected) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float s = (float)scale;
    const char *label = langGetText(grid == HUD_GRID_TANKS ? STR_TABLET_TANKS :
                                    grid == HUD_GRID_PILLS ? STR_TABLET_PILLBOXES :
                                                             STR_TABLET_BASES);
    dl->AddText(origin, ImGui::GetColorU32(ImGuiCol_Text), label);

    ImVec2 p0(origin.x, origin.y + (float)hud_label_h());
    ImVec2 p1(p0.x + s * HUD_PANEL_W, p0.y + s * HUD_PANEL_H);
    dl->AddRectFilled(p0, p1, IM_COL32_BLACK);
    dl->AddRect(ImVec2(p0.x - 1.0f, p0.y - 1.0f), ImVec2(p1.x + 1.0f, p1.y + 1.0f),
                HUD_EDGE_COLOUR);

    /* Pixel art drawn several times its size: point sampled, or it blurs. */
    imguiPushNearestSamplingOn(dl);

    /* The tile in the middle that says which grid this is, as in the game. */
    {
        float sx, sy, mx, my;
        if (grid == HUD_GRID_TANKS) {
            sx = TANK_SELF_0_X; sy = TANK_SELF_0_Y;
            mx = STATUS_TANKS_MIDDLE_ICON_X; my = STATUS_TANKS_MIDDLE_ICON_Y;
        } else if (grid == HUD_GRID_PILLS) {
            sx = PILL_GOOD15_X; sy = PILL_GOOD15_Y;
            mx = STATUS_PILLS_MIDDLE_ICON_X; my = STATUS_PILLS_MIDDLE_ICON_Y;
        } else {
            sx = BASE_GOOD_X; sy = BASE_GOOD_Y;
            mx = STATUS_BASES_MIDDLE_ICON_X; my = STATUS_BASES_MIDDLE_ICON_Y;
        }
        ImVec2 m0(p0.x + s * mx, p0.y + s * my);
        hud_image(dl, lv_drawGetTilesTexture(), sx, sy, TILE_SIZE_X, TILE_SIZE_Y,
                  m0, ImVec2(m0.x + s * TILE_SIZE_X, m0.y + s * TILE_SIZE_Y), IM_COL32_WHITE);
    }

    for (int n = 0; n < LV_HUD_SLOTS; n++) {
        const LvHudSlot &slot = slots[n];
        if (slot.icon == LV_HUD_ICON_EMPTY) continue;

        int x, y;
        sdl3StatusItemPos(n + 1, grid, &x, &y);
        ImVec2 q0(p0.x + s * x, p0.y + s * y);
        ImVec2 q1(q0.x + s * STATUS_ITEM_SIZE_X, q0.y + s * STATUS_ITEM_SIZE_Y);
        hud_slot_icon(dl, grid, slot, perspective, q0, q1);
        if (n == selected) {
            /* The selected tank: a thin white edge round its cell, as the
             * map has white corners round the tank. */
            dl->AddRect(ImVec2(q0.x - 1.0f, q0.y - 1.0f), ImVec2(q1.x + 1.0f, q1.y + 1.0f),
                        IM_COL32(255, 255, 255, 190));
        }

        /* The slot is an item of the window, so the hover and the click go
         * through ImGui: a popup or another window over the grid hides it. */
        ImGui::PushID(grid * LV_HUD_SLOTS + n);
        ImGui::SetCursorScreenPos(q0);
        if (ImGui::InvisibleButton("##slot", ImVec2(q1.x - q0.x, q1.y - q0.y))) {
            hud_slot_click(grid, n);
        }
        imguiHandOnHover();
        /* At once, with no wait and no need for the mouse to stand still:
         * the cells are small and close, and the name is what the mouse is
         * moved over them for. It goes from cell to cell with the mouse.
         * Only these cells; every other tooltip keeps ImGui's delay. */
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNone)) {
            hud_slot_tooltip(grid, n, slot);
        }
        ImGui::PopID();
    }

    imguiPopNearestSamplingOn(dl);
}

void lv_imgui_hud_window(void) {
    if (!lv_g_show_hud_window) {
        return;
    }

    {
        /* A panel that sat in the dock when the dock was turned off goes
         * back to its default place, the same as a reset. */
        bool to_default = lv_g_reset_window_positions ||
                          lv_imgui_dock_take_float_reset("###hud");
        ImGuiCond cond = to_default ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
        const ImGuiStyle &style = ImGui::GetStyle();
        int w, h;
        /* Down the left edge, where the game has its column and none of the
         * other panels opens. */
        lvHudContentSize(1, HUD_DEFAULT_SCALE, HUD_PANEL_W, HUD_PANEL_H,
                         hud_label_h(), hud_gap(), &w, &h);
        ImGui::SetNextWindowSize(ImVec2((float)w + style.WindowPadding.x * 2.0f,
                                        (float)(h + hud_header_h()) +
                                            style.WindowPadding.y * 2.0f +
                                            ImGui::GetFrameHeight()), cond);
        ImGui::SetNextWindowPos(ImVec2(10, 30), cond);
    }

    char hud_title[128];
    snprintf(hud_title, sizeof(hud_title), "%s###hud", langGetText(STR_LV_WIN_HUD));
    /* No scroll bar: the grids are sized to the window, and a bar that came
     * and went with them would change the room they are sized to. */
    if (ImGui::Begin(hud_title, &lv_g_show_hud_window,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse)) {
        /* In the dock the dock places the window; moving it here would pull
         * it straight back out. */
        if (!ImGui::IsWindowDocked()) {
            /* Reposition window relative to right/bottom edge when viewport is resized */
            float dx, dy;
            if (lv_imgui_context_get_resize_delta(&dx, &dy)) {
                ImVec2 pos = ImGui::GetWindowPos();
                float old_vp_x = ImGui::GetMainViewport()->Size.x - dx;
                float old_vp_y = ImGui::GetMainViewport()->Size.y - dy;
                ImVec2 size = ImGui::GetWindowSize();
                if (pos.x + size.x * 0.5f > old_vp_x * 0.5f) ImGui::SetWindowPos(ImVec2(pos.x + dx, pos.y + dy));
                else if (pos.y + size.y * 0.5f > old_vp_y * 0.5f) ImGui::SetWindowPos(ImVec2(pos.x, pos.y + dy));
            }
            /* Always clamp to keep fully on screen */
            {
                ImVec2 pos = ImGui::GetWindowPos();
                ImVec2 size = ImGui::GetWindowSize();
                ImVec2 vp = ImGui::GetMainViewport()->Size;
                ImVec2 new_pos = pos;
                if (new_pos.x + size.x > vp.x) new_pos.x = vp.x - size.x;
                if (new_pos.y + size.y > vp.y) new_pos.y = vp.y - size.y;
                if (new_pos.x < 0) new_pos.x = 0;
                if (new_pos.y < 0) new_pos.y = 0;
                if (new_pos.x != pos.x || new_pos.y != pos.y) ImGui::SetWindowPos(new_pos);
            }
        }

        /* Playback writes the pillboxes, bases and players on the decode
         * timer, so what is drawn is copied out under its lock, every frame:
         * the copy is then right while playing, scrubbing and stepping. */
        LvHudSnapshot snap;
        bool lost;
        lv_clientMutexWaitFor();
        /* The perspective's player may have left since the last frame. */
        lost = lv_screenCheckPerspective();
        lv_screenHudSnapshot(&snap);
        lv_clientMutexRelease();
        if (lost) lv_drawDirtyScreen();

        /* The line above the grids: whose view they show. With a perspective
         * on, the button that goes back to all teams comes first, so a long
         * name cannot push it out of the window. */
        {
            ImVec2 top = ImGui::GetCursorScreenPos();
            if (snap.perspective) {
                /* The stripe of the player's team, an eye, the name, and at
                 * the right the X that goes back to all teams: the same
                 * line the Tank panel has for its tank. The eye stands for
                 * "the view of"; the words are the tooltip of the name. */
                MessageArgs args = {};
                snprintf(args.string1, sizeof(args.string1), "%s", snap.viewerName);
                char viewAs[LANG_MSGARG_STRING_LEN];
                snprintf(viewAs, sizeof(viewAs), "%s", langGetTextFmt(STR_LV_HUD_VIEW_AS, &args));
                const LvHudSlot &who = snap.tanks[snap.viewer < LV_HUD_SLOTS ? snap.viewer : 0];
                if (lv_name_header("allteams", who.hasTeam != 0, hud_team_colour(who), LV_ICON_EYE,
                                   snap.viewerChip, snap.viewerName, viewAs,
                                   langGetText(STR_LV_TIP_ALL_TEAMS))) {
                    /* Out of the one-player view, and no tank selected. */
                    lv_clientMutexWaitFor();
                    lv_screenClearPerspective();
                    lv_screenDeselectTank();
                    lv_clientMutexRelease();
                    lv_drawDirtyScreen();
                }
            } else {
                ImGui::TextDisabled("%s", langGetText(STR_LV_HUD_ALL_TEAMS));
            }
            ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + (float)hud_header_h()));
        }

        ImVec2 avail = ImGui::GetContentRegionAvail();
        /* On whole pixels: an icon that starts part way through a pixel is
         * drawn with its edges uneven. */
        ImVec2 origin = ImGui::GetCursorScreenPos();
        origin.x = floorf(origin.x);
        origin.y = floorf(origin.y);

        LvHudFit fit = lvHudFit((int)avail.x, (int)avail.y, HUD_PANEL_W, HUD_PANEL_H,
                                hud_label_h(), hud_gap());
        for (int i = 0; i < 3; i++) {
            int grid = k_grid_order[i];
            const LvHudSlot *slots = (grid == HUD_GRID_TANKS) ? snap.tanks :
                                     (grid == HUD_GRID_PILLS) ? snap.pills : snap.bases;
            ImVec2 at = origin;
            if (fit.columns == 3) {
                at.x += (float)(i * (fit.scale * HUD_PANEL_W + hud_gap()));
            } else {
                at.y += (float)(i * (hud_label_h() + fit.scale * HUD_PANEL_H + hud_gap()));
            }
            hud_grid(grid, slots, snap.perspective != 0, at, fit.scale,
                     (grid == HUD_GRID_TANKS && snap.hasSelected) ? (int)snap.selected : -1);
        }

        /* The slots moved the cursor about; leave it under the whole block. */
        {
            int w, h;
            lvHudContentSize(fit.columns, fit.scale, HUD_PANEL_W, HUD_PANEL_H,
                             hud_label_h(), hud_gap(), &w, &h);
            ImGui::SetCursorScreenPos(origin);
            ImGui::Dummy(ImVec2((float)w, (float)h));
        }
    }
    ImGui::End();
}
