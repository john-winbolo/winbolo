/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * winbolods_resource.h
 *
 * Resource IDs consumed by winbolods.rc on Windows builds. The .rc
 * file is minimal — it embeds only the server icon. No string table
 * or dialog resources are produced; the dedicated server has no UI.
 *
 * IDI_ICON1 is preserved at its historical value so the embedded
 * icon resource keeps the same numeric ID across rebuilds.
 */

#ifndef WINBOLODS_RESOURCE_H
#define WINBOLODS_RESOURCE_H

#define IDI_ICON1 111

#endif
