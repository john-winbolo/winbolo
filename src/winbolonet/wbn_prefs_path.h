/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          WinBolo.net preferences-file path
 * Filename:      wbn_prefs_path.h
 * Purpose:
 *   Tiny storage TU holding the INI prefs-file path used by
 *   the WinBolo.net subsystem on Windows. winbolonetCoreSetPreferencesPath
 *   is the public setter (declared on winbolonet_core.h) and is
 *   called once at startup by every binary.
 *
 *   Hoisted out of http.c so libcurl-free callers (wbn_country_cache.c
 *   and the unit-test binary) can read the path without dragging
 *   http.c's curl / cJSON / tweetnacl closure.
 *
 *   POSIX paths come from preferencesGetPreferenceFile() in
 *   posix_stubs.c and do not consult this storage.
 *********************************************************/

#ifndef __WBN_PREFS_PATH_H
#define __WBN_PREFS_PATH_H

/*********************************************************
 *NAME:          winbolonetCorePrefsPath
 *PURPOSE:
 * Returns the prefs-file path most recently passed to
 * winbolonetCoreSetPreferencesPath, or an empty string if
 * never set. The returned pointer is owned by the TU and
 * is valid until the next setter call.
 *********************************************************/
const char *winbolonetCorePrefsPath(void);

#endif /* __WBN_PREFS_PATH_H */
