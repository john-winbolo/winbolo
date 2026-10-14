/*
 * lv_utf8_path.h - file paths as UTF-8 on every system
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

/*
 * The log viewer's file dialogs give a path as UTF-8 on every system. On
 * Windows the C library's fopen reads a path in the system's own code page,
 * not as UTF-8, so a path with a letter outside ASCII has to be opened by
 * its UTF-16 form. The pieces for that are here, header only and with no
 * <windows.h>, so any source of the viewer and the unit tests can use them
 * (tests/unit/test_lv_utf8_path.c).
 *
 *   lvUtf8ToWide, lvWideToUtf8 - Windows only: one string from the one form
 *       to the other, into the caller's buffer, or false.
 *   lvFopenUtf8 - fopen for a path that is UTF-8. On other systems it is
 *       fopen itself.
 */

#ifndef LV_UTF8_PATH_H
#define LV_UTF8_PATH_H

#include <stddef.h>
#include <stdio.h>

#ifdef _WIN32
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif
/* The two functions of kernel32, as <windows.h> has them. They are written
 * out here because <windows.h> and the viewer's own lv_global.h do not go
 * in one source file without care (TRUE, FALSE, bool). */
__declspec(dllimport) int __stdcall MultiByteToWideChar(unsigned int CodePage,
                                                        unsigned long dwFlags,
                                                        const char *lpMultiByteStr,
                                                        int cbMultiByte, wchar_t *lpWideCharStr,
                                                        int cchWideChar);
__declspec(dllimport) int __stdcall WideCharToMultiByte(unsigned int CodePage,
                                                        unsigned long dwFlags,
                                                        const wchar_t *lpWideCharStr,
                                                        int cchWideChar, char *lpMultiByteStr,
                                                        int cbMultiByte,
                                                        const char *lpDefaultChar,
                                                        int *lpUsedDefaultChar);
#ifdef __cplusplus
}
#endif

#define LV_UTF8_PATH_CP_UTF8 65001u            /* CP_UTF8 */
#define LV_UTF8_PATH_STRICT_IN 0x00000008ul    /* MB_ERR_INVALID_CHARS */
#define LV_UTF8_PATH_STRICT_OUT 0x00000080ul   /* WC_ERR_INVALID_CHARS */

/* The most UTF-16 units of a path lvFopenUtf8 opens: well over MAX_PATH
 * (260), for a system with long paths turned on. */
#define LV_UTF8_PATH_WIDE_MAX 4096

/* utf8, with its end, to UTF-16 in out, which holds outCount units. False,
 * with out empty, when utf8 is no good UTF-8 or does not go in. */
static inline int lvUtf8ToWide(const char *utf8, wchar_t *out, size_t outCount) {
    int n;
    if (out == NULL || outCount == 0) return 0;
    out[0] = L'\0';
    if (utf8 == NULL || outCount > 0x7FFFFFFFu) return 0;
    n = MultiByteToWideChar(LV_UTF8_PATH_CP_UTF8, LV_UTF8_PATH_STRICT_IN, utf8, -1, out,
                            (int)outCount);
    if (n <= 0) {
        out[0] = L'\0';
        return 0;
    }
    return 1;
}

/* wide, with its end, to UTF-8 in out, which holds outSize bytes. False,
 * with out empty, when wide is no good UTF-16 or does not go in. */
static inline int lvWideToUtf8(const wchar_t *wide, char *out, size_t outSize) {
    int n;
    if (out == NULL || outSize == 0) return 0;
    out[0] = '\0';
    if (wide == NULL || outSize > 0x7FFFFFFFu) return 0;
    n = WideCharToMultiByte(LV_UTF8_PATH_CP_UTF8, LV_UTF8_PATH_STRICT_OUT, wide, -1, out,
                            (int)outSize, NULL, NULL);
    if (n <= 0) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}
#endif /* _WIN32 */

/* fopen for a path that is UTF-8.
 *
 * On Windows a path can still come in the system's code page: from the
 * command line, or from the preferences. Such a path with a letter outside
 * ASCII is as good as never also good UTF-8, so a path that is not good
 * UTF-8 is given to fopen as it is, and so is one that is to be read and
 * that the UTF-16 open does not find. A file that is to be written is never
 * made under the other reading of a good UTF-8 path: that would be a file
 * with a wrong name. A path of ASCII only is the same in both. */
static inline FILE *lvFopenUtf8(const char *path, const char *mode) {
#ifdef _WIN32
    wchar_t wpath[LV_UTF8_PATH_WIDE_MAX];
    wchar_t wmode[16];
    if (path == NULL || mode == NULL) return NULL;
    if (lvUtf8ToWide(path, wpath, LV_UTF8_PATH_WIDE_MAX) &&
        lvUtf8ToWide(mode, wmode, sizeof(wmode) / sizeof(wmode[0]))) {
        FILE *fp = _wfopen(wpath, wmode);
        if (fp != NULL || mode[0] != 'r' || mode[1] == '+' ||
            (mode[1] != '\0' && mode[2] == '+')) {
            return fp;
        }
    }
    return fopen(path, mode);
#else
    if (path == NULL || mode == NULL) return NULL;
    return fopen(path, mode);
#endif
}

#endif /* LV_UTF8_PATH_H */
