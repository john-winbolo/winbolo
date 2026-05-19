/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          WinBolo.net preferences-file path
 * Filename:      wbn_prefs_path.c
 * Purpose:
 *   Storage + accessors for the Windows INI prefs-file
 *   path. Lives in its own TU (no curl / cJSON / SDL3) so
 *   libcurl-free callers — wbn_country_cache.c and the
 *   unit-test binary — can link without dragging the rest
 *   of winbolonet_core into the test binary.
 *********************************************************/

#include <stdio.h>
#include <string.h>

#include "global.h"
#include "winbolonet_core.h"
#include "wbn_prefs_path.h"

static char wbnPrefsPath[FILENAME_MAX];

void winbolonetCoreSetPreferencesPath(const char *path) {
  if (path == NULL) {
    wbnPrefsPath[0] = '\0';
    return;
  }
  strncpy(wbnPrefsPath, path, sizeof(wbnPrefsPath) - 1);
  wbnPrefsPath[sizeof(wbnPrefsPath) - 1] = '\0';
}

const char *winbolonetCorePrefsPath(void) {
  return wbnPrefsPath;
}
