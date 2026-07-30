/*
 * Guards the generated symbolic-name lookup table in
 * src/gui/sdl3/lang_names.inc, which resolveName() searches with
 * bsearch() while loading a translation file.
 *
 * The crash this pins: lang_names.inc once shipped with
 * K_LANG_NAME_TABLE_SIZE one larger than the real kLangNameTable[]
 * length (a hand-edit that bumped the size without adding the row).
 * bsearch was handed the inflated count, read one element past the
 * end of the table, and dereferenced the garbage slot's name pointer
 * in strcmp — a startup SIGSEGV for any non-English user (English
 * never calls resolveName). See lang.c resolveName().
 *
 * This TU includes the generated header directly (RC_INVOKED trims
 * lang.h to just its string-id #defines; langid is typedef'd locally
 * to match src/bolo/public/lang_message.h) so the test compiles the
 * exact table the game ships, with no game include graph pulled in.
 */
#include <stdlib.h>
#include <string.h>

#define RC_INVOKED          /* pull only the STR_*/LGM_*/... #defines */
#include "lang.h"
#undef RC_INVOKED

typedef unsigned int langid;  /* mirrors src/bolo/public/lang_message.h */

#include "lang_names.inc"     /* LangNameEntry, kLangNameTable, K_LANG_NAME_TABLE_SIZE */

#include "test_harness.h"

/* Same comparator resolveName() hands to bsearch: key is a plain
 * string, member is a LangNameEntry. */
static int nameCmp(const void *a, const void *b) {
    const char          *key   = (const char *)a;
    const LangNameEntry *entry = (const LangNameEntry *)b;
    return strcmp(key, entry->name);
}

int run_lang_name_table(void) {
    const size_t actual = sizeof(kLangNameTable) / sizeof(kLangNameTable[0]);

    /* 1. The size macro must equal the real element count. This is the
     *    exact off-by-one that walked bsearch off the end. */
    UT_ASSERT_MSG(K_LANG_NAME_TABLE_SIZE == (int)actual,
                  "K_LANG_NAME_TABLE_SIZE=%d but kLangNameTable[] has %zu "
                  "entries — re-run tools/dump_lang_en.py",
                  K_LANG_NAME_TABLE_SIZE, actual);

    /* 2. bsearch's precondition: strictly ascending strcmp order (also
     *    rules out duplicate names). A mis-sorted table wouldn't crash
     *    but would silently mis-resolve translation keys. */
    for (size_t i = 1; i < actual; i++) {
        UT_ASSERT_MSG(strcmp(kLangNameTable[i - 1].name,
                             kLangNameTable[i].name) < 0,
                      "kLangNameTable not strictly sorted at index %zu: "
                      "'%s' !< '%s'",
                      i, kLangNameTable[i - 1].name, kLangNameTable[i].name);
    }

    /* 3. End-to-end: every name resolves to its own id through the same
     *    bsearch call resolveName() makes, and a name that isn't in the
     *    table returns no hit (the path that crashed on the OOB slot). */
    for (size_t i = 0; i < actual; i++) {
        const LangNameEntry *hit = (const LangNameEntry *)bsearch(
            kLangNameTable[i].name, kLangNameTable, actual,
            sizeof(kLangNameTable[0]), nameCmp);
        UT_ASSERT_MSG(hit != NULL, "name '%s' did not resolve",
                      kLangNameTable[i].name);
        UT_ASSERT_MSG(hit->id == kLangNameTable[i].id,
                      "name '%s' resolved to id %u, expected %u",
                      kLangNameTable[i].name, hit->id, kLangNameTable[i].id);
    }

    const LangNameEntry *miss = (const LangNameEntry *)bsearch(
        "ZZZ_NO_SUCH_STRING_ID", kLangNameTable, actual,
        sizeof(kLangNameTable[0]), nameCmp);
    UT_ASSERT_MSG(miss == NULL, "unknown name unexpectedly resolved");

    return 0;
}
