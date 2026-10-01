/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * winbolo_resource.h
 *
 * Resource IDs consumed by winbolo.rc on Windows builds. The .rc file
 * is minimal — it embeds the app manifest and the icon, nothing else.
 * The Win32-era string table and dialog resources are gone; runtime
 * strings live in src/gui/sdl3/lang.c (langTable[]) and the UI is
 * built with SDL3+ImGui, so no STRINGTABLE / IDD_* / IDC_* IDs are
 * needed here.
 *
 * IDI_ICON1 is preserved at its historical value so the embedded
 * icon resource keeps the same numeric ID across rebuilds.
 */

#ifndef WINBOLO_RESOURCE_H
#define WINBOLO_RESOURCE_H

#define IDI_ICON1 111

#endif
