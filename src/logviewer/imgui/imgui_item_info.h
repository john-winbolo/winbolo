/*
 * imgui_item_info.h - ImGui item information window for Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef IMGUI_ITEM_INFO_H
#define IMGUI_ITEM_INFO_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Render the item info window
 * Should be called each frame when window is visible */
void lv_imgui_item_info_window(void);

/* Initialize item info state */
void lv_imgui_item_info_init(void);

/* Update item info display
 * itemType: 0 = clear, 1 = base, 2 = pillbox
 * itemNumber: the item number
 * owner: the owner (player index or 0xFF for NEUTRAL)
 * x, y: location coordinates
 * armour, shells, mines: item stats
 * inTank: whether item is in a tank
 * Using C types directly to avoid C++/C type conflicts */
void lv_imgui_item_info_update(unsigned char itemType, unsigned char itemNumber, unsigned char owner, 
                            unsigned char x, unsigned char y, unsigned char armour, unsigned char shells, 
                            unsigned char mines, int inTank);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_ITEM_INFO_H */