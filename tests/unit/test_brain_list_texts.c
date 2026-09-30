/*
 * Coverage for brainListLoadTexts / brainListLoadTextsForPath — the two
 * lobby texts a brain ships beside about.txt.
 *
 *   announce.txt   the message the lobby drops into team chat when a bot
 *                  running this brain joins your team
 *   commands.txt   the long docs that message opens
 *
 * Unlike about.txt these go over the wire, so what the reader does with a
 * missing file, an over-long file and Windows line endings is what every
 * client ends up seeing. The test builds a throwaway brain directory and
 * reads it both ways: by directory, and by the "<dir>/init.lua" path the
 * catalogue actually stores.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "brain_list.h"
#include "test_harness.h"

#if defined(_WIN32)
#  define SEP '\\'
#else
#  define SEP '/'
#endif

#define TEXTS_ROOT "wbtest_braintexts"
#define BRAIN_DIR  "TalkativeBrain"
#define QUIET_DIR  "SilentBrain"

static void rmPath(const char *p) { SDL_RemovePath(p); }

static void cleanup(void) {
    char p[512];
    SDL_snprintf(p, sizeof(p), "%s%c%s%cannounce.txt", TEXTS_ROOT, SEP,
                 BRAIN_DIR, SEP);
    rmPath(p);
    SDL_snprintf(p, sizeof(p), "%s%c%s%ccommands.txt", TEXTS_ROOT, SEP,
                 BRAIN_DIR, SEP);
    rmPath(p);
    SDL_snprintf(p, sizeof(p), "%s%c%s", TEXTS_ROOT, SEP, BRAIN_DIR);
    rmPath(p);
    SDL_snprintf(p, sizeof(p), "%s%c%s", TEXTS_ROOT, SEP, QUIET_DIR);
    rmPath(p);
    rmPath(TEXTS_ROOT);
}

static int writeFile(const char *path, const char *bytes, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    if (n > 0 && fwrite(bytes, 1, n, f) != n) { fclose(f); return 1; }
    fclose(f);
    return 0;
}

int run_brain_list_texts_read(void) {
    /* CRLF on purpose: a brain edited on Windows must not put a stray
     * carriage return in front of every newline the lobby draws. */
    static const char kAnnounce[] =
        "GoalHunter loaded. Click this message to read more.\r\n"
        "\r\n"
        "Repositioning is off with human allies.\r\n";
    static const char kAnnounceWant[] =
        "GoalHunter loaded. Click this message to read more.\n"
        "\n"
        "Repositioning is off with human allies.\n";

    char dir[512], path[512], initPath[512];
    char *announce = NULL, *docs = NULL, *big = NULL;
    bool truncated = true;
    int rc = 1;

    announce = (char *)malloc(BRAIN_ANNOUNCE_MAX + 1);
    docs     = (char *)malloc(BRAIN_DOCS_MAX + 1);
    UT_ASSERT(announce != NULL && docs != NULL);

    cleanup();
    UT_ASSERT(SDL_CreateDirectory(TEXTS_ROOT));
    SDL_snprintf(dir, sizeof(dir), "%s%c%s", TEXTS_ROOT, SEP, BRAIN_DIR);
    UT_ASSERT(SDL_CreateDirectory(dir));
    SDL_snprintf(path, sizeof(path), "%s%c%s", TEXTS_ROOT, SEP, QUIET_DIR);
    UT_ASSERT(SDL_CreateDirectory(path));

    /* ── A brain that ships neither file says nothing ──────────────── */
    SDL_snprintf(path, sizeof(path), "%s%c%s", TEXTS_ROOT, SEP, QUIET_DIR);
    UT_ASSERT_MSG(brainListLoadTexts(path,
                                     announce, (size_t)BRAIN_ANNOUNCE_MAX + 1,
                                     docs, (size_t)BRAIN_DOCS_MAX + 1,
                                     &truncated) == false,
                  "a brain with no text files must read as false");
    UT_ASSERT(announce[0] == '\0');
    UT_ASSERT(docs[0] == '\0');
    UT_ASSERT(truncated == false);

    /* ── announce.txt alone ────────────────────────────────────────── */
    SDL_snprintf(path, sizeof(path), "%s%cannounce.txt", dir, SEP);
    UT_ASSERT_MSG(writeFile(path, kAnnounce, sizeof(kAnnounce) - 1) == 0,
                  "could not write %s", path);
    truncated = true;
    UT_ASSERT(brainListLoadTexts(dir,
                                 announce, (size_t)BRAIN_ANNOUNCE_MAX + 1,
                                 docs, (size_t)BRAIN_DOCS_MAX + 1,
                                 &truncated) == true);
    UT_ASSERT_MSG(strcmp(announce, kAnnounceWant) == 0,
                  "CR stripping failed: got '%s'", announce);
    UT_ASSERT_MSG(docs[0] == '\0', "docs must stay empty, got '%s'", docs);
    UT_ASSERT(truncated == false);

    /* ── commands.txt too, and the blank lines inside it survive ───── */
    SDL_snprintf(path, sizeof(path), "%s%ccommands.txt", dir, SEP);
    UT_ASSERT(writeFile(path, "HEADING\n\nA line.\n", 17) == 0);
    UT_ASSERT(brainListLoadTexts(dir,
                                 announce, (size_t)BRAIN_ANNOUNCE_MAX + 1,
                                 docs, (size_t)BRAIN_DOCS_MAX + 1,
                                 NULL) == true);
    UT_ASSERT_MSG(strcmp(docs, "HEADING\n\nA line.\n") == 0,
                  "docs round-trip: got '%s'", docs);

    /* ── The ForPath form: the catalogue stores "<dir>/init.lua" ───── */
    SDL_snprintf(initPath, sizeof(initPath), "%s%cinit.lua", dir, SEP);
    announce[0] = '\0';
    docs[0] = '\0';
    UT_ASSERT_MSG(brainListLoadTextsForPath(initPath,
                                            announce,
                                            (size_t)BRAIN_ANNOUNCE_MAX + 1,
                                            docs, (size_t)BRAIN_DOCS_MAX + 1,
                                            NULL) == true,
                  "ForPath must find the texts beside init.lua");
    UT_ASSERT(strcmp(announce, kAnnounceWant) == 0);
    UT_ASSERT(strcmp(docs, "HEADING\n\nA line.\n") == 0);

    /* A path with no directory part has nowhere to look. */
    UT_ASSERT(brainListLoadTextsForPath("init.lua", announce,
                                        (size_t)BRAIN_ANNOUNCE_MAX + 1,
                                        docs, (size_t)BRAIN_DOCS_MAX + 1,
                                        NULL) == false);

    /* ── A file longer than the cap is cut, and SAYS it was cut ────── */
    big = (char *)malloc(BRAIN_DOCS_MAX + 64);
    UT_ASSERT(big != NULL);
    memset(big, 'x', BRAIN_DOCS_MAX + 64);
    SDL_snprintf(path, sizeof(path), "%s%ccommands.txt", dir, SEP);
    UT_ASSERT(writeFile(path, big, (size_t)BRAIN_DOCS_MAX + 64) == 0);
    truncated = false;
    UT_ASSERT(brainListLoadTexts(dir,
                                 announce, (size_t)BRAIN_ANNOUNCE_MAX + 1,
                                 docs, (size_t)BRAIN_DOCS_MAX + 1,
                                 &truncated) == true);
    UT_ASSERT_MSG(strlen(docs) == (size_t)BRAIN_DOCS_MAX,
                  "docs cut to %u bytes, want %d",
                  (unsigned)strlen(docs), BRAIN_DOCS_MAX);
    UT_ASSERT_MSG(truncated == true,
                  "an over-long commands.txt must report the truncation");

    /* ── The brain this branch actually ships, if it is in the tree ── */
    {
        char shipped[512];
        SDL_snprintf(shipped, sizeof(shipped), "brains%cGoalHunter_1.7", SEP);
        if (brainListLoadTexts(shipped,
                               announce, (size_t)BRAIN_ANNOUNCE_MAX + 1,
                               docs, (size_t)BRAIN_DOCS_MAX + 1,
                               &truncated)) {
            UT_ASSERT_MSG(truncated == false,
                          "GoalHunter's own texts no longer fit the wire caps "
                          "(%d / %d bytes) — raise the cap or trim the file",
                          BRAIN_ANNOUNCE_MAX, BRAIN_DOCS_MAX);
            UT_ASSERT_MSG(announce[0] != '\0',
                          "GoalHunter ships no announce.txt");
            UT_ASSERT_MSG(docs[0] != '\0',
                          "GoalHunter ships no commands.txt");
        }
        /* Not found is fine: the unit tests do not always run from the
         * project root, and the wire path is what this test really pins. */
    }

    rc = 0;
    free(announce);
    free(docs);
    free(big);
    cleanup();
    return rc;
}

/* COMMANDS.TXT GOES OVER THE WIRE COMPRESSED, AND COMES BACK WHOLE OR NOT AT
 * ALL.
 *
 * The server keeps each brain's docs as brainDocsCompress output and sends
 * those bytes on CHANNEL_BULK; the client hands them to brainDocsDecompress
 * with the length the announce said. What the client must never do is show
 * part of a text, or a text of another length, as though it were the docs,
 * so each way the bytes can be wrong is refused here: a stream cut short, a
 * length that does not match, a length past the cap, and a stream that is not
 * zlib at all. */
int run_brain_docs_compress_roundtrip(void) {
    char    *docs = NULL, *back = NULL;
    uint8_t *z    = NULL;
    size_t   zLen, i;
    uint32_t seed = 12345;

    docs = (char *)malloc(BRAIN_DOCS_MAX + 1);
    back = (char *)malloc(BRAIN_DOCS_MAX + 1);
    z    = (uint8_t *)malloc(BRAIN_DOCS_Z_MAX);
    UT_ASSERT(docs != NULL && back != NULL && z != NULL);

    /* The worst case: the whole cap of bytes that barely compress. The
     * result must still fit BRAIN_DOCS_Z_MAX, which is what the wire and the
     * client's receive buffer are sized from. */
    for (i = 0; i < BRAIN_DOCS_MAX; i++) {
        seed = seed * 1103515245u + 12345u;
        docs[i] = (char)(' ' + (int)((seed >> 16) % 95u));
    }
    docs[BRAIN_DOCS_MAX] = '\0';
    zLen = brainDocsCompress(docs, BRAIN_DOCS_MAX, z, BRAIN_DOCS_Z_MAX);
    UT_ASSERT_MSG(zLen > 0 && zLen <= BRAIN_DOCS_Z_MAX,
                  "a full-size random text compressed to %zu bytes, cap %d",
                  zLen, BRAIN_DOCS_Z_MAX);
    UT_ASSERT(brainDocsDecompress(z, zLen, BRAIN_DOCS_MAX, back));
    UT_ASSERT(memcmp(back, docs, BRAIN_DOCS_MAX) == 0 &&
              back[BRAIN_DOCS_MAX] == '\0');

    /* Ordinary text shrinks, which is the point of sending it this way. */
    for (i = 0; i < 20000; i++) docs[i] = "move north, hold, fire\n"[i % 23];
    zLen = brainDocsCompress(docs, 20000, z, BRAIN_DOCS_Z_MAX);
    UT_ASSERT_MSG(zLen > 0 && zLen < 2000,
                  "20000 bytes of repeated text compressed to %zu", zLen);
    UT_ASSERT(brainDocsDecompress(z, zLen, 20000, back));
    UT_ASSERT(memcmp(back, docs, 20000) == 0 && back[20000] == '\0');

    /* A length other than the one the stream inflates to is refused, in
     * either direction, and leaves an empty string rather than a part. */
    UT_ASSERT(!brainDocsDecompress(z, zLen, 19999, back));
    UT_ASSERT(back[0] == '\0');
    UT_ASSERT(!brainDocsDecompress(z, zLen, 20001, back));
    UT_ASSERT(back[0] == '\0');
    /* A stream cut short. */
    UT_ASSERT(!brainDocsDecompress(z, zLen / 2, 20000, back));
    UT_ASSERT(back[0] == '\0');
    /* Bytes that are not a zlib stream. */
    memset(z, 0xA5, 64);
    UT_ASSERT(!brainDocsDecompress(z, 64, 100, back));
    /* Lengths the wire cannot carry. */
    UT_ASSERT(!brainDocsDecompress(z, 64, BRAIN_DOCS_MAX + 1, back));
    UT_ASSERT(!brainDocsDecompress(z, 0, 10, back));
    UT_ASSERT(!brainDocsDecompress(z, 64, 0, back));

    /* And the compressor refuses what it may not send. */
    UT_ASSERT(brainDocsCompress(docs, 0, z, BRAIN_DOCS_Z_MAX) == 0);
    UT_ASSERT(brainDocsCompress(docs, BRAIN_DOCS_MAX + 1, z,
                                BRAIN_DOCS_Z_MAX) == 0);
    UT_ASSERT_MSG(brainDocsCompress(docs, 20000, z, 16) == 0,
                  "a result that does not fit the buffer must be refused");

    free(docs);
    free(back);
    free(z);
    return 0;
}
