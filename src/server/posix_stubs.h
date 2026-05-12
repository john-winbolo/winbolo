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
*Filename:      posix_stubs.h
*Purpose:
*  Non-Win32 declarations for Win32 API stubs used by
*  winbolonet/http.c and the dedicated server.
*  The implementations live in posix_stubs.c.
*********************************************************/

#ifndef POSIX_STUBS_H
#define POSIX_STUBS_H

#ifndef _WIN32

#include "global.h"  /* DWORD, FILENAME_MAX */

/* Returns the platform config file path. If preferencesSetPreferenceFileOverride
 * has been called, returns that path; otherwise returns the headless default
 * (~/.config/winbolo/WinBolo.ini). SDL-linked binaries should call the override
 * with their SDL_GetPrefPath result during init so all INI access goes to the
 * same file. */
void preferencesGetPreferenceFile(char *dest);

/* Sets the path that preferencesGetPreferenceFile will return. Pass NULL or
 * empty string to clear the override. The path is copied; caller may free. */
void preferencesSetPreferenceFileOverride(const char *path);

/* Minimal INI reader — same signature as Win32 GetPrivateProfileString */
DWORD GetPrivateProfileString(const char *section, const char *key,
                               const char *defaultVal,
                               char *out, DWORD outSize,
                               const char *filePath);

/* Minimal INI writer — same signature as Win32 WritePrivateProfileString */
int WritePrivateProfileString(const char *section, const char *key,
                               const char *value,
                               const char *filePath);

#endif /* !_WIN32 */

#endif /* POSIX_STUBS_H */
