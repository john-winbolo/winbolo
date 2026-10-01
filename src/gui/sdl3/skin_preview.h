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

/*********************************************************
 * Name:          skin_preview.h
 * Purpose:
 *   Renders a sample of a skin's art to a PNG, at the size
 *   a Steam Workshop item's preview image wants.
 *********************************************************/

#ifndef SKIN_PREVIEW_H
#define SKIN_PREVIEW_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct SkinSource;

/*********************************************************
 * NAME:          skinWritePreviewPng
 * PURPOSE:
 *   Writes a PNG showing a sample of this skin's art,
 *   sized for a Steam Workshop item's preview image. A
 *   NULL skin renders the built-in assets. False on any
 *   failure; the file is not left half-written.
 *********************************************************/
bool skinWritePreviewPng(struct SkinSource *skin, const char *outPath);

#ifdef __cplusplus
}
#endif

#endif /* SKIN_PREVIEW_H */
