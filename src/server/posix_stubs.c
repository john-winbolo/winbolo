/*
 * Copyright (c) 1998-2008 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*********************************************************
*Name:          posix_stubs
*Filename:      posix_stubs.c
*Purpose:
*  Non-Win32 stubs for the dedicated server that replace
*  Win32-only APIs used by winbolonet/http.c:
*
*  - preferencesGetPreferenceFile() — returns the config file path
*  - GetPrivateProfileString()      — reads a key from a .ini file
*  - WritePrivateProfileString()    — writes a key to a .ini file
*
*  These use a simple hand-rolled INI parser so there is no
*  dependency on platform_config (which belongs to the SDL3 client).
*********************************************************/

#ifndef _WIN32

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <pwd.h>
#include <unistd.h>
#include "global.h"

/* ------------------------------------------------------------------ */
/* preferencesGetPreferenceFile                                        */
/* ------------------------------------------------------------------ */

static char preferenceFileOverride[FILENAME_MAX] = "";

void preferencesSetPreferenceFileOverride(const char *path) {
  if (!path || !path[0]) {
    preferenceFileOverride[0] = '\0';
    return;
  }
  strncpy(preferenceFileOverride, path, FILENAME_MAX - 1);
  preferenceFileOverride[FILENAME_MAX - 1] = '\0';
}

void preferencesGetPreferenceFile(char *dest) {
  if (preferenceFileOverride[0]) {
    strncpy(dest, preferenceFileOverride, FILENAME_MAX - 1);
    dest[FILENAME_MAX - 1] = '\0';
    return;
  }

  const char *home = getenv("HOME");
  if (!home) {
    struct passwd *pw = getpwuid(getuid());
    if (pw) home = pw->pw_dir;
  }
  if (!home) home = ".";

  /* ~/.config/winbolo/ directory */
  char dir[FILENAME_MAX];
  snprintf(dir, sizeof(dir), "%s/.config/winbolo", home);
  dir[sizeof(dir) - 1] = '\0';
  mkdir(dir, 0755);

  size_t dlen = strlen(dir);
  if (dlen + sizeof("/WinBolo.ini") <= FILENAME_MAX) {
    memcpy(dest, dir, dlen);
    memcpy(dest + dlen, "/WinBolo.ini", sizeof("/WinBolo.ini"));
  } else {
    snprintf(dest, FILENAME_MAX, "%s", ".config/winbolo/WinBolo.ini");
  }
}

/* ------------------------------------------------------------------ */
/* Minimal INI reader/writer                                           */
/* ------------------------------------------------------------------ */

#define MAX_INI_LINE 512

/* Read a value from a plain INI file.
 * Returns number of characters written (excluding NUL), like the Win32 API. */
DWORD GetPrivateProfileString(const char *section, const char *key,
                               const char *defaultVal,
                               char *out, DWORD outSize,
                               const char *filePath) {
  FILE *fp;
  char line[MAX_INI_LINE];
  char curSection[128] = "";
  size_t keyLen;

  if (outSize == 0) return 0;

  fp = fopen(filePath, "r");
  if (!fp) {
    strncpy(out, defaultVal ? defaultVal : "", outSize - 1);
    out[outSize - 1] = '\0';
    return (DWORD)strlen(out);
  }

  keyLen = strlen(key);

  while (fgets(line, sizeof(line), fp)) {
    /* Strip trailing newline */
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
      line[--len] = '\0';

    if (line[0] == '[') {
      char *end = strchr(line + 1, ']');
      if (end) {
        *end = '\0';
        strncpy(curSection, line + 1, sizeof(curSection) - 1);
      }
      continue;
    }

    if (strcmp(curSection, section) != 0) continue;

    /* key=value */
    if (strncmp(line, key, keyLen) == 0 && line[keyLen] == '=') {
      const char *val = line + keyLen + 1;
      strncpy(out, val, outSize - 1);
      out[outSize - 1] = '\0';
      fclose(fp);
      return (DWORD)strlen(out);
    }
  }

  fclose(fp);
  strncpy(out, defaultVal ? defaultVal : "", outSize - 1);
  out[outSize - 1] = '\0';
  return (DWORD)strlen(out);
}

/* Write a value to a plain INI file.
 * Returns non-zero on success, like the Win32 API. */
int WritePrivateProfileString(const char *section, const char *key,
                                const char *value,
                                const char *filePath) {
  /* Read existing file into memory */
  FILE *fp;
  char lines[512][MAX_INI_LINE];
  int lineCount = 0;
  char curSection[128] = "";
  int sectionFound = 0;
  int keyFound = 0;
  int i;

  fp = fopen(filePath, "r");
  if (fp) {
    while (lineCount < 512 && fgets(lines[lineCount], MAX_INI_LINE, fp)) {
      lineCount++;
    }
    fclose(fp);
  }

  /* Find or append section + key */
  for (i = 0; i < lineCount; i++) {
    char *ln = lines[i];
    size_t len = strlen(ln);
    while (len > 0 && (ln[len-1] == '\n' || ln[len-1] == '\r')) len--;
    ln[len] = '\0';

    if (ln[0] == '[') {
      char *end = strchr(ln + 1, ']');
      if (end) {
        *end = '\0';
        strncpy(curSection, ln + 1, sizeof(curSection) - 1);
        *end = ']';  /* restore */
      }
    } else if (strcmp(curSection, section) == 0) {
      size_t klen = strlen(key);
      if (strncmp(ln, key, klen) == 0 && ln[klen] == '=') {
        snprintf(lines[i], MAX_INI_LINE, "%s=%s", key, value);
        keyFound = 1;
        sectionFound = 1;
        break;
      }
    }
    if (strcmp(curSection, section) == 0) sectionFound = 1;
  }

  if (!keyFound) {
    /* Append key under section (or add new section+key) */
    if (!sectionFound && lineCount < 512) {
      snprintf(lines[lineCount++], MAX_INI_LINE, "[%s]", section);
    }
    if (lineCount < 512) {
      snprintf(lines[lineCount++], MAX_INI_LINE, "%s=%s", key, value);
    }
  }

  /* Write back */
  fp = fopen(filePath, "w");
  if (!fp) return 0;
  for (i = 0; i < lineCount; i++) {
    fprintf(fp, "%s\n", lines[i]);
  }
  fclose(fp);
  return 1;
}

#endif /* !_WIN32 */
