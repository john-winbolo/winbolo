/*
 * Coverage for the Bolo pascal-string reader — a length byte followed by
 * that many characters — in both places it is implemented.
 *
 * The length byte is read out of a `char *`, which is signed on x86-64
 * macOS and Linux and unsigned on ARM. Read without a cast, any length
 * from 128 up came back negative, the copy loop never ran, and the caller
 * got an empty string on exactly the platforms where plain char is
 * signed: a server message over 127 characters showed up blank in the log
 * viewer, and a 128-character chat message (PACKET_MAX_CHAT_MESSAGE)
 * reached a brain as no message at all.
 *
 * src/bolo/util.c and src/logviewer/util.c hold the same function twice —
 * the log viewer builds without the game's util.c — so both are tested
 * here, and against each other.
 */

#include <string.h>

#include "global.h"
#include "util.h"
#include "test_harness.h"

/* Declared rather than included: lv_util.h and util.h share the UTILS_H
 * include guard, so one hides the other. */
void lv_utilPtoCString(char *src, char *dest);

static const int kLengths[] = { 0, 1, 32, 126, 127, 128, 129, 200, 254, 255 };
#define NUM_LENGTHS ((int)(sizeof(kLengths) / sizeof(kLengths[0])))

/* Fill `pstr` with a length byte and that many identifiable characters. */
static void makePascal(char *pstr, int len) {
    int i;
    pstr[0] = (char)len;
    for (i = 0; i < len; i++) {
        pstr[i + 1] = (char)('A' + (i % 26));
    }
}

int run_pascal_string_lengths(void) {
    char pstr[257];
    char out[256];
    int i;

    for (i = 0; i < NUM_LENGTHS; i++) {
        int len = kLengths[i];
        int j;

        makePascal(pstr, len);
        memset(out, '#', sizeof(out));
        utilPtoCString(pstr, out);

        UT_ASSERT_MSG((int)strlen(out) == len,
                      "a %d-character pascal string should read back %d "
                      "characters, got %d", len, len, (int)strlen(out));
        for (j = 0; j < len; j++) {
            UT_ASSERT_MSG(out[j] == (char)('A' + (j % 26)),
                          "byte %d of the %d-character string is wrong", j, len);
        }
    }
    return 0;
}

int run_pascal_string_viewer_copy_agrees(void) {
    char pstr[257];
    char game[256];
    char viewer[256];
    int i;

    for (i = 0; i < NUM_LENGTHS; i++) {
        int len = kLengths[i];

        makePascal(pstr, len);
        memset(game, '#', sizeof(game));
        memset(viewer, '#', sizeof(viewer));
        utilPtoCString(pstr, game);
        lv_utilPtoCString(pstr, viewer);

        UT_ASSERT_MSG(strcmp(game, viewer) == 0,
                      "the game and log-viewer readers disagree at length %d: "
                      "'%s' vs '%s'", len, game, viewer);
    }
    return 0;
}

int run_pascal_string_roundtrip(void) {
    char text[256];
    char pstr[257];
    char out[256];
    int i;

    for (i = 0; i < 200; i++) {
        text[i] = (char)('a' + (i % 26));
    }
    text[200] = '\0';

    utilCtoPString(text, pstr);
    UT_ASSERT_MSG((unsigned char)pstr[0] == 200,
                  "the written length byte should be 200, got %d",
                  (unsigned char)pstr[0]);

    utilPtoCString(pstr, out);
    UT_ASSERT_MSG(strcmp(out, text) == 0,
                  "a 200-character string should survive the round trip");
    return 0;
}
