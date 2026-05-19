/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          WinBolo.net country-code cache
 * Filename:      wbn_country_cache.c
 * Purpose:
 *   In-memory storage and INI passthrough for the client's
 *   resolved ISO 3166-1 alpha-2 country code. Written once
 *   per program run by the news fetcher; read by SP / LAN /
 *   tutorial game-setup paths that need a country code on
 *   the local player record.
 *
 *   Lives in its own libcurl-free TU so the accessor pair
 *   can compile into WinBoloUnitTests without dragging the
 *   rest of winbolonet_core / http / curl into the test
 *   binary — same pattern as wbn_bearer.c.
 *
 *   On Windows the INI path is read from
 *   winbolonetCorePrefsPath() (set at startup by every
 *   binary). On POSIX it comes from preferencesGetPreferenceFile
 *   in posix_stubs.c.
 *
 *   The s_inMemoryOnly flag is flipped by the internal
 *   winbolonetCountryCacheResetForTesting() hook (declared
 *   in wbn_country_cache_internal.h, visible only to the
 *   unit-test binary). When set, Get() skips the lazy INI
 *   read and Set() skips the INI write-through, so test
 *   runs can exercise the state machine without touching
 *   the user's WinBolo.ini. Production callers never invoke
 *   the reset hook and therefore never enter this mode.
 *********************************************************/

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "winbolonet_core.h"
#include "wbn_country_cache_internal.h"
#include "wbn_prefs_path.h"

#ifndef _WIN32
/* Provided by posix_stubs.c. Forward-declared here (rather than including
 * posix_stubs.h) so winbolonet_core's include path doesn't need src/server.
 * Matches the same pattern in http.c. */
void preferencesGetPreferenceFile(char *dest);
unsigned int GetPrivateProfileString(const char *section, const char *key,
                                     const char *def, char *out,
                                     unsigned int outSize,
                                     const char *filePath);
int WritePrivateProfileString(const char *section, const char *key,
                              const char *value, const char *filePath);
#else
/* Win32 GetPrivateProfileString / WritePrivateProfileString come from
 * <winsock2.h> -> <windows.h> brought in transitively by global.h. */
#endif

static char s_country[3] = {'\0', '\0', '\0'};
static bool s_initialised = false;
static bool s_inMemoryOnly = false;

static void resolvePrefsPath(char *out, size_t outSize) {
  if (out == NULL || outSize == 0) return;
#ifdef _WIN32
  const char *p = winbolonetCorePrefsPath();
  if (p == NULL) p = "";
  strncpy(out, p, outSize - 1);
  out[outSize - 1] = '\0';
#else
  preferencesGetPreferenceFile(out);
#endif
}

static bool isValidTwoAlpha(const char *cc) {
  if (cc == NULL) return false;
  if (cc[0] == '\0' || cc[1] == '\0' || cc[2] != '\0') return false;
  if (!isalpha((unsigned char)cc[0])) return false;
  if (!isalpha((unsigned char)cc[1])) return false;
  return true;
}

static void writeNormalised(const char *cc) {
  s_country[0] = (char)toupper((unsigned char)cc[0]);
  s_country[1] = (char)toupper((unsigned char)cc[1]);
  s_country[2] = '\0';
  s_initialised = true;
}

const char *winbolonetGetCountryCode(void) {
  if (!s_initialised) {
    if (s_inMemoryOnly) {
      s_country[0] = 'X';
      s_country[1] = 'X';
      s_country[2] = '\0';
      s_initialised = true;
      return s_country;
    }
    char prefs[FILENAME_MAX];
    char iniValue[8] = {0};
    resolvePrefsPath(prefs, sizeof(prefs));
    GetPrivateProfileString("WINBOLO.NET", "CountryCode", "XX",
                            iniValue, (unsigned int)sizeof(iniValue), prefs);
    if (isValidTwoAlpha(iniValue)) {
      writeNormalised(iniValue);
    } else {
      s_country[0] = 'X';
      s_country[1] = 'X';
      s_country[2] = '\0';
      s_initialised = true;
    }
  }
  return s_country;
}

void winbolonetSetCountryCode(const char *cc) {
  if (!isValidTwoAlpha(cc)) {
    return;
  }
  writeNormalised(cc);

  if (s_inMemoryOnly) {
    return;
  }

  char prefs[FILENAME_MAX];
  resolvePrefsPath(prefs, sizeof(prefs));
  WritePrivateProfileString("WINBOLO.NET", "CountryCode", s_country, prefs);
}

void winbolonetCountryCacheResetForTesting(void) {
  s_country[0] = '\0';
  s_country[1] = '\0';
  s_country[2] = '\0';
  s_initialised = false;
  s_inMemoryOnly = true;
}
