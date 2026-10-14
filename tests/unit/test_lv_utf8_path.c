/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * File paths as UTF-8 on every system (lv_utf8_path.h): what the log
 * viewer's file dialogs give back, and how the viewer opens it.
 *
 *   lv_utf8_path — a name with letters outside ASCII goes to UTF-16 and
 *       back unchanged (Windows); a buffer too small and text that is not
 *       UTF-8 are refused; lvFopenUtf8 writes and reads a file with such a
 *       name, and the file on the disk has that name.
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "lv_utf8_path.h"
#include "test_harness.h"

/* "2026-10-13 Oil Rig - Zoë, Łukasz vs 日本.wbv": letters of two bytes and
 * of three, from more than one code page. */
#define UP_NAME                                                                \
    "lv_utf8_path 2026-10-13 Oil Rig - Zo\xC3\xAB, \xC5\x81ukasz vs "          \
    "\xE6\x97\xA5\xE6\x9C\xAC.wbv"

int run_lv_utf8_path(void) {
    static const char body[] = "the bytes of a log";
    char              back[64];
    FILE             *fp;
    size_t            n;

#ifdef _WIN32
    {
        static const wchar_t want[] =
            L"lv_utf8_path 2026-10-13 Oil Rig - Zo\x00EB, \x0141ukasz vs \x65E5\x672C.wbv";
        wchar_t wide[128];
        char    utf8[128];
        wchar_t small[8];
        char    tiny[8];

        /* There and back. */
        UT_ASSERT(lvUtf8ToWide(UP_NAME, wide, sizeof(wide) / sizeof(wide[0])));
        UT_ASSERT_MSG(wcscmp(wide, want) == 0, "UTF-16 of the name has %u units",
                      (unsigned)wcslen(wide));
        UT_ASSERT(lvWideToUtf8(wide, utf8, sizeof(utf8)));
        UT_ASSERT_MSG(strcmp(utf8, UP_NAME) == 0, "back: \"%s\"", utf8);
        /* A buffer that is just big enough, and one unit too small. */
        UT_ASSERT(lvUtf8ToWide(UP_NAME, wide, wcslen(want) + 1));
        UT_ASSERT(!lvUtf8ToWide(UP_NAME, wide, wcslen(want)));
        UT_ASSERT_MSG(wide[0] == L'\0', "a refused string is empty");
        UT_ASSERT(lvWideToUtf8(want, utf8, strlen(UP_NAME) + 1));
        UT_ASSERT(!lvWideToUtf8(want, utf8, strlen(UP_NAME)));
        UT_ASSERT_MSG(utf8[0] == '\0', "a refused string is empty");
        UT_ASSERT(!lvUtf8ToWide(UP_NAME, small, sizeof(small) / sizeof(small[0])));
        UT_ASSERT(!lvWideToUtf8(want, tiny, sizeof(tiny)));
        /* Nothing to write to, nothing to read. */
        UT_ASSERT(!lvUtf8ToWide(UP_NAME, wide, 0));
        UT_ASSERT(!lvUtf8ToWide(UP_NAME, NULL, 16));
        UT_ASSERT(!lvUtf8ToWide(NULL, wide, sizeof(wide) / sizeof(wide[0])));
        UT_ASSERT(!lvWideToUtf8(want, utf8, 0));
        UT_ASSERT(!lvWideToUtf8(NULL, utf8, sizeof(utf8)));
        /* The empty string is a string. */
        UT_ASSERT(lvUtf8ToWide("", wide, 1) && wide[0] == L'\0');
        UT_ASSERT(lvWideToUtf8(L"", utf8, 1) && utf8[0] == '\0');
        /* Text in a code page of one byte is not UTF-8: "Zo" and a lone
           0xEB. Half of a UTF-16 pair is not UTF-16. */
        UT_ASSERT(!lvUtf8ToWide("Zo\xEB.wbv", wide, sizeof(wide) / sizeof(wide[0])));
        UT_ASSERT(!lvWideToUtf8(L"Zo\xD83D.wbv", utf8, sizeof(utf8)));
    }
#endif

    /* Nothing to open. */
    UT_ASSERT(lvFopenUtf8(NULL, "rb") == NULL);
    UT_ASSERT(lvFopenUtf8(UP_NAME, NULL) == NULL);

    /* A file with the name is written and read back. */
    fp = lvFopenUtf8(UP_NAME, "wb");
    UT_ASSERT_MSG(fp != NULL, "the file is made");
    n = fwrite(body, 1, sizeof(body), fp);
    UT_ASSERT(fclose(fp) == 0);
    UT_ASSERT_MSG(n == sizeof(body), "wrote %u bytes", (unsigned)n);

    fp = lvFopenUtf8(UP_NAME, "rb");
    UT_ASSERT_MSG(fp != NULL, "the file is found again");
    memset(back, 0, sizeof(back));
    n = fread(back, 1, sizeof(back), fp);
    fclose(fp);
    UT_ASSERT_MSG(n == sizeof(body) && memcmp(back, body, sizeof(body)) == 0,
                  "read %u bytes back", (unsigned)n);

#ifdef _WIN32
    {
        /* The file on the disk has the name, not the name's bytes read in
           the system's code page. */
        wchar_t wide[128];
        UT_ASSERT(lvUtf8ToWide(UP_NAME, wide, sizeof(wide) / sizeof(wide[0])));
        fp = _wfopen(wide, L"rb");
        UT_ASSERT_MSG(fp != NULL, "the file has its UTF-16 name");
        fclose(fp);
        UT_ASSERT(_wremove(wide) == 0);
        /* It is gone under every reading of the name. */
        UT_ASSERT(lvFopenUtf8(UP_NAME, "rb") == NULL);
    }
    /* A path in the system's code page, not UTF-8, still opens: the command
       line gives such paths. Plain fopen made the file, so the two agree on
       its name whatever the code page is. Skipped where the code page has
       no such letter. */
    fp = fopen("lv_utf8_path Zo\xEB.tmp", "wb");
    if (fp != NULL) {
        fclose(fp);
        fp = lvFopenUtf8("lv_utf8_path Zo\xEB.tmp", "rb");
        UT_ASSERT_MSG(fp != NULL, "a code page path is read");
        fclose(fp);
        remove("lv_utf8_path Zo\xEB.tmp");
    }
#else
    remove(UP_NAME);
#endif
    return 0;
}
