/* Fuzz target — the key tokens in a scenario's text.
 *
 * A popup, a status line and an announcement come from whatever server the
 * player joined, and every client splits them into plain runs and tokens
 * ({ACCEL}, {FIRE} ...) and writes the tokens out as key names. Both are
 * hand-written walks over a NUL-ended string into fixed buffers.
 *
 * Input: [u8 cap][u8 max][text]. cap (1-256) is the expand's buffer and max
 * (1-64) the split's piece limit; the text is copied onto an exact-size heap
 * buffer with its NUL, so ASan sees a read past the end. Each input runs the
 * split, then the expand with no names, with one-byte names and with long
 * names, and checks what the unit test tutorial_tokens_random_input checks:
 * the pieces join back into the text, the expand stays inside its buffer,
 * ends with a NUL and answers its length. A broken check aborts, so the
 * fuzzer keeps the input as a crash.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/gui/sdl3/tutorial_tokens.h"

#define GUARD 16

static const char *oneByte(TutorialTokenId id, void *ctx) {
    (void)id;
    (void)ctx;
    return "#";
}

static const char *longWords(TutorialTokenId id, void *ctx) {
    (void)ctx;
    return (id % 2 == 0) ? "the forward button" : "Bouton avanc\xC3\xA9";
}

static void check(int ok) {
    if (!ok) abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static const TutorialTokenNameFn kNames[] = { NULL, oneByte, longWords };
    TutorialPiece pieces[64];
    const char   *rest = NULL;
    char         *text;
    char         *out;
    size_t        cap, len, covered, k, n;
    int           max, count, i, j;

    if (size < 2) return 0;
    cap = 1 + (size_t)data[0];
    max = 1 + data[1] % 64;
    data += 2;
    size -= 2;

    /* The text stops at its first NUL, as it does off the wire. */
    len = 0;
    while (len < size && data[len] != 0) len++;
    text = (char *)malloc(len + 1);
    out  = (char *)malloc(cap + GUARD);
    if (text == NULL || out == NULL) {
        free(text);
        free(out);
        return 0;
    }
    memcpy(text, data, len);
    text[len] = '\0';

    count = tutorialTokensSplit(text, pieces, max, &rest);
    check(count >= 0 && count <= max);
    covered = 0;
    for (i = 0; i < count; i++) {
        check(pieces[i].start == text + covered && pieces[i].len > 0);
        if (pieces[i].id != TT_COUNT) {
            const char *t = tutorialTokenText(pieces[i].id);
            check(t != NULL && strlen(t) == pieces[i].len &&
                  memcmp(t, pieces[i].start, pieces[i].len) == 0);
        }
        covered += pieces[i].len;
    }
    check(rest == text + covered);
    check(count == max || *rest == '\0');

    for (j = 0; j < 3; j++) {
        memset(out, 'Z', cap + GUARD);
        n = tutorialTokensExpand(text, out, cap, kNames[j], NULL);
        check(n < cap && out[n] == '\0' && strlen(out) == n);
        for (k = cap; k < cap + GUARD; k++) check(out[k] == 'Z');
        if (kNames[j] == NULL && len < cap) {
            check(n == len && memcmp(out, text, len) == 0);
        }
    }

    free(text);
    free(out);
    return 0;
}
