/*
 * The key tokens' parser against text no one checked. A popup, a status
 * line and an announcement come from whatever server the player joined, so
 * the client splits and fills in bytes it cannot trust.
 *
 *   malformed — a brace that starts no token ("{", "{ACC", "{accel}",
 *               "{FOO}", "{{ACCEL}}") is copied as plain text, and the
 *               tokens around it are still found. No token is the start of
 *               another, so the first match is the only one.
 *   split     — tutorialTokensSplit cuts a text into plain runs and tokens
 *               that cover it in order, and at its piece limit says where
 *               it stopped.
 *   utf8 cut  — tutorialTokensExpand, cutting a text too long for its
 *               buffer, never ends in half a character.
 *   random    — a fixed seed's worth of random text, made of token parts,
 *               braces, letters, UTF-8 characters and stray high bytes:
 *               the expand never writes past its buffer, always ends with a
 *               NUL, answers its length, keeps valid UTF-8 valid and copies
 *               a text whole when nothing is named; the split's pieces
 *               join back into the text and each holds what it says.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../src/gui/sdl3/tutorial_tokens.h"
#include "test_harness.h"

/* Every token named "#", so an output shows where tokens were found. */
static const char *hashName(TutorialTokenId id, void *ctx) {
    (void)id;
    (void)ctx;
    return "#";
}

/* Every token named by a long word, so a cut falls inside text often. */
static const char *longName(TutorialTokenId id, void *ctx) {
    (void)ctx;
    return (id % 2 == 0) ? "the forward button" : "Bouton avancé";
}

/* Whether s[0..len) is well-formed UTF-8: no stray continuation byte, no
   lead byte without all its continuation bytes, nothing past U+10FFFF. */
static bool utf8Valid(const char *s, size_t len) {
    size_t i = 0;
    while (i < len) {
        const unsigned char c = (unsigned char)s[i];
        size_t n, k;
        if (c < 0x80) { i++; continue; }
        if (c >= 0xC2 && c <= 0xDF) n = 2;
        else if (c >= 0xE0 && c <= 0xEF) n = 3;
        else if (c >= 0xF0 && c <= 0xF4) n = 4;
        else return false;
        if (i + n > len) return false;
        for (k = 1; k < n; k++) {
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
        }
        i += n;
    }
    return true;
}

/* ── malformed ─────────────────────────────────────────────────────── */

int run_tutorial_tokens_malformed(void) {
    static const struct { const char *in; const char *want; } kCases[] = {
        { "",                    ""                },
        { "{",                   "{"               },
        { "}",                   "}"               },
        { "{}",                  "{}"              },
        { "{ACC",                "{ACC"            },
        { "{ACCEL",              "{ACCEL"          },
        { "ACCEL}",              "ACCEL}"          },
        { "{accel}",             "{accel}"         },
        { "{FOO}",               "{FOO}"           },
        { "{ ACCEL}",            "{ ACCEL}"        },
        { "{ACCEL }",            "{ACCEL }"        },
        { "{ACCEL\n}",           "{ACCEL\n}"       },
        { "{{ACCEL}}",           "{#}"             },
        { "{ACCEL}}",            "#}"              },
        { "{ACCEL}{",            "#{"              },
        { "{FIRE}{FIRE}",        "##"              },
        { "x{QUICK_TREE}y",      "x#y"             },
        { "{ACCEL}{ACC{FIRE}",   "#{ACC#"          },
        { "{SCROLL_UP}",         "#"               },
        { "{SCROLL_UPX}",        "{SCROLL_UPX}"    },
        { "{SCROLL_UP",          "{SCROLL_UP"      },
        { "{SCROLL_}",           "{SCROLL_}"       },
        { "{%s}{%n}%d",          "{%s}{%n}%d"      },
        { "\xC3{FIRE}\xA9",      "\xC3#\xA9"       },
    };
    char   out[128];
    size_t i, j;

    for (i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        size_t n = tutorialTokensExpand(kCases[i].in, out, sizeof(out),
                                        hashName, NULL);
        UT_ASSERT_MSG(strcmp(out, kCases[i].want) == 0,
                      "case %u: '%s' gave '%s', want '%s'", (unsigned)i,
                      kCases[i].in, out, kCases[i].want);
        UT_ASSERT(n == strlen(out));
    }

    /* Every token ends at its only '}', so none is the start of another
       and the matcher's first hit is the only one. A token added later
       that broke this would match the wrong row. */
    for (i = 0; i < (size_t)TT_COUNT; i++) {
        const char *a = tutorialTokenText((TutorialTokenId)i);
        size_t      alen;
        UT_ASSERT(a != NULL);
        alen = strlen(a);
        UT_ASSERT_MSG(alen >= 3 && a[0] == '{' && a[alen - 1] == '}' &&
                      strchr(a, '}') == a + alen - 1 &&
                      strchr(a + 1, '{') == NULL,
                      "token %u '%s' is not one {NAME}", (unsigned)i, a);
        for (j = 0; j < (size_t)TT_COUNT; j++) {
            const char *b = tutorialTokenText((TutorialTokenId)j);
            if (i == j) continue;
            UT_ASSERT_MSG(strncmp(a, b, alen) != 0,
                          "'%s' is the start of '%s'", a, b);
        }
    }
    UT_ASSERT(tutorialTokenText(TT_COUNT) == NULL);
    UT_ASSERT(tutorialTokenText((TutorialTokenId)-1) == NULL);
    UT_ASSERT(tutorialTokenMatch(NULL, NULL) == TT_COUNT);
    UT_ASSERT(tutorialTokenMatch("", NULL) == TT_COUNT);
    UT_ASSERT(tutorialTokenMatch("x{FIRE}", NULL) == TT_COUNT);

    /* A long run of braces, cut to the buffer: every byte a brace, and
       the NUL where it belongs. */
    {
        char   braces[600];
        size_t n;
        memset(braces, '{', sizeof(braces) - 1);
        braces[sizeof(braces) - 1] = '\0';
        n = tutorialTokensExpand(braces, out, 64, hashName, NULL);
        UT_ASSERT(n == 63 && out[63] == '\0');
        for (i = 0; i < n; i++) UT_ASSERT(out[i] == '{');
    }
    return 0;
}

/* ── split ─────────────────────────────────────────────────────────── */

int run_tutorial_tokens_split(void) {
    TutorialPiece p[8];
    const char   *rest = NULL;
    const char   *src;
    int           n;

    /* Plain, token, plain, token: four pieces that cover the text. */
    src = "Press {ACCEL} then {FIRE}";
    n = tutorialTokensSplit(src, p, 8, &rest);
    UT_ASSERT_MSG(n == 4, "got %d pieces", n);
    UT_ASSERT(p[0].id == TT_COUNT && p[0].start == src && p[0].len == 6);
    UT_ASSERT(p[1].id == TT_ACCEL && p[1].start == src + 6 && p[1].len == 7);
    UT_ASSERT(p[2].id == TT_COUNT && p[2].len == 6);
    UT_ASSERT(p[3].id == TT_FIRE && p[3].len == 6);
    UT_ASSERT(rest == src + strlen(src) && *rest == '\0');

    /* A brace that starts no token stays inside its plain run. */
    src = "{FOO} {ACC {";
    n = tutorialTokensSplit(src, p, 8, &rest);
    UT_ASSERT(n == 1 && p[0].id == TT_COUNT && p[0].len == strlen(src));
    UT_ASSERT(*rest == '\0');

    /* Tokens side by side, and no plain run of length 0 between them. */
    src = "{FIRE}{FIRE}{MINE}";
    n = tutorialTokensSplit(src, p, 8, &rest);
    UT_ASSERT(n == 3 && p[0].id == TT_FIRE && p[1].id == TT_FIRE &&
              p[2].id == TT_MINE);

    /* At the piece limit: rest is the first piece left out, a plain run
       or a token. */
    src = "a{FIRE}b{MINE}c";
    n = tutorialTokensSplit(src, p, 2, &rest);
    UT_ASSERT(n == 2 && rest == src + 7 && *rest == 'b');
    n = tutorialTokensSplit(src, p, 3, &rest);
    UT_ASSERT(n == 3 && rest == src + 8 && strncmp(rest, "{MINE}", 6) == 0);
    n = tutorialTokensSplit(src, p, 1, &rest);
    UT_ASSERT(n == 1 && p[0].len == 1 && rest == src + 1);

    /* Nothing to split, or nowhere to put it. */
    n = tutorialTokensSplit("", p, 8, &rest);
    UT_ASSERT(n == 0 && *rest == '\0');
    src = "abc";
    UT_ASSERT(tutorialTokensSplit(src, p, 0, &rest) == 0 && rest == src);
    UT_ASSERT(tutorialTokensSplit(src, NULL, 8, &rest) == 0 && rest == src);
    UT_ASSERT(tutorialTokensSplit(NULL, p, 8, &rest) == 0 && rest == NULL);
    UT_ASSERT(tutorialTokensSplit(src, p, 8, NULL) == 1);
    return 0;
}

/* ── UTF-8 cut ─────────────────────────────────────────────────────── */

int run_tutorial_tokens_utf8_cut(void) {
    char   out[32];
    size_t n;

    /* "abé": the é (2 bytes) does not fit in 4 with the NUL. */
    memset(out, 'X', sizeof(out));
    n = tutorialTokensExpand("ab\xC3\xA9", out, 4, NULL, NULL);
    UT_ASSERT_MSG(n == 2 && strcmp(out, "ab") == 0, "got '%s' (%u)", out,
                  (unsigned)n);
    UT_ASSERT(out[4] == 'X');

    /* "あい" (3 bytes each) in 6: the first whole, the second left out. */
    n = tutorialTokensExpand("\xE3\x81\x82\xE3\x81\x84", out, 6, NULL, NULL);
    UT_ASSERT_MSG(n == 3 && strcmp(out, "\xE3\x81\x82") == 0, "got %u bytes",
                  (unsigned)n);

    /* A 4-byte character after a name, cut at each of its bytes. */
    {
        static const char src[] = "{FIRE}\xF0\x9F\x98\x80";
        size_t cap;
        for (cap = 2; cap <= 5; cap++) {
            n = tutorialTokensExpand(src, out, cap, hashName, NULL);
            UT_ASSERT_MSG(n == 1 && strcmp(out, "#") == 0,
                          "cap %u: got %u bytes", (unsigned)cap, (unsigned)n);
        }
        n = tutorialTokensExpand(src, out, 6, hashName, NULL);
        UT_ASSERT(n == 5 && strcmp(out, "#\xF0\x9F\x98\x80") == 0);
    }

    /* Broken bytes after a name, at a cut: the name stays whole. */
    n = tutorialTokensExpand("{MINE}\x80\x80", out, 15, longName, NULL);
    UT_ASSERT_MSG(n == 14 && strcmp(out, "Bouton avanc\xC3\xA9") == 0,
                  "got '%s' (%u)", out, (unsigned)n);

    /* A cut that falls between two whole characters keeps them both. */
    n = tutorialTokensExpand("\xC3\xA9\xC3\xA9", out, 3, NULL, NULL);
    UT_ASSERT(n == 2 && strcmp(out, "\xC3\xA9") == 0);

    /* Bytes that are no character at all (stray continuation bytes) are
       taken off together at a cut; nothing is read before the buffer. */
    n = tutorialTokensExpand("\x80\x80\x80\x80", out, 3, NULL, NULL);
    UT_ASSERT(n == 0 && out[0] == '\0');
    n = tutorialTokensExpand("a\x80\x80\x80", out, 3, NULL, NULL);
    UT_ASSERT(n == 1 && strcmp(out, "a") == 0);

    /* Text that fits is never touched, broken UTF-8 included. */
    n = tutorialTokensExpand("a\x80" "b\xC3", out, sizeof(out), NULL, NULL);
    UT_ASSERT(n == 4 && memcmp(out, "a\x80" "b\xC3", 5) == 0);
    return 0;
}

/* ── random ────────────────────────────────────────────────────────── */

static uint32_t rngNext(uint32_t *s) {
    /* xorshift32: fixed seed, the same text on every run. */
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

/* One random text into buf (cap bytes with its NUL), from the parts a
   hostile or broken text is made of. */
static size_t randomText(uint32_t *rng, char *buf, size_t cap) {
    static const char *const kParts[] = {
        "{", "}", "{{", "}}", "{ACC", "EL}", "{FOO}", "{accel}", " ", "\n",
        "a", "Press ", "%s", "%n", "\\", "\xC3\xA9", "\xE3\x81\x82",
        "\xF0\x9F\x98\x80", "\x80", "\xC3", "\xE3\x81", "\xFF", "\xF8",
        "{STEP_PILL", "_GREEN}", "{STEP_",
    };
    const size_t kPartCount = sizeof(kParts) / sizeof(kParts[0]);
    size_t len = 0;
    size_t want = rngNext(rng) % 300;
    while (len < want) {
        const char *part;
        size_t      plen;
        uint32_t    r = rngNext(rng) % 10;
        if (r < 3) {
            part = tutorialTokenText((TutorialTokenId)(rngNext(rng) % TT_COUNT));
        } else if (r < 4) {
            /* A byte of any value but NUL. */
            char b = (char)(1 + rngNext(rng) % 255);
            if (len + 1 >= cap) break;
            buf[len++] = b;
            continue;
        } else {
            part = kParts[rngNext(rng) % kPartCount];
        }
        plen = strlen(part);
        if (len + plen >= cap) break;
        memcpy(buf + len, part, plen);
        len += plen;
    }
    buf[len] = '\0';
    return len;
}

int run_tutorial_tokens_random_input(void) {
    enum { ROUNDS = 20000, GUARD = 16 };
    uint32_t rng = 0x5eed1234u;
    char     src[512];
    char     out[512 + GUARD];
    char     again[512 + GUARD];
    TutorialPiece pieces[80];
    int      round;

    for (round = 0; round < ROUNDS; round++) {
        size_t      len  = randomText(&rng, src, sizeof(src));
        size_t      cap  = 1 + rngNext(&rng) % 512;
        int         max  = 1 + (int)(rngNext(&rng) % 80);
        bool        valid = utf8Valid(src, len);
        TutorialTokenNameFn name =
            (round % 3 == 0) ? NULL : (round % 3 == 1) ? hashName : longName;
        const char *rest = NULL;
        size_t      n, k, covered;
        int         count, i;

        /* expand: inside its buffer, NUL-ended, its length answered. */
        memset(out, 'Z', sizeof(out));
        n = tutorialTokensExpand(src, out, cap, name, NULL);
        UT_ASSERT_MSG(n < cap && out[n] == '\0' && strlen(out) == n,
                      "round %d: %u bytes in a %u-byte buffer", round,
                      (unsigned)n, (unsigned)cap);
        for (k = cap; k < sizeof(out); k++) {
            UT_ASSERT_MSG(out[k] == 'Z', "round %d: byte %u past the "
                          "%u-byte buffer was written", round, (unsigned)k,
                          (unsigned)cap);
        }
        if (valid) {
            UT_ASSERT_MSG(utf8Valid(out, n), "round %d: valid UTF-8 in, "
                          "broken UTF-8 out (cap %u)", round, (unsigned)cap);
        }
        /* The same text and buffer give the same answer. */
        memset(again, 'Z', sizeof(again));
        UT_ASSERT(tutorialTokensExpand(src, again, cap, name, NULL) == n);
        UT_ASSERT(memcmp(out, again, n + 1) == 0);
        /* With nothing named and room enough, the text comes back whole. */
        if (name == NULL && len < cap) {
            UT_ASSERT_MSG(n == len && memcmp(out, src, len + 1) == 0,
                          "round %d: the text did not come back whole", round);
        }

        /* split: pieces in order, joined back into the text up to rest,
           each a token or a plain run with no token in it. */
        count = tutorialTokensSplit(src, pieces, max, &rest);
        UT_ASSERT(count >= 0 && count <= max);
        UT_ASSERT(rest >= src && rest <= src + len);
        covered = 0;
        for (i = 0; i < count; i++) {
            const TutorialPiece *p = &pieces[i];
            UT_ASSERT_MSG(p->start == src + covered && p->len > 0,
                          "round %d: piece %d is not where the last ended",
                          round, i);
            if (p->id == TT_COUNT) {
                UT_ASSERT_MSG(i == 0 || pieces[i - 1].id != TT_COUNT,
                              "round %d: two plain runs side by side", round);
                for (k = 0; k < p->len; k++) {
                    UT_ASSERT_MSG(tutorialTokenMatch(p->start + k, NULL) ==
                                  TT_COUNT, "round %d: a token inside a "
                                  "plain run", round);
                }
            } else {
                const char *t = tutorialTokenText(p->id);
                UT_ASSERT(t != NULL && strlen(t) == p->len &&
                          memcmp(t, p->start, p->len) == 0);
            }
            covered += p->len;
        }
        UT_ASSERT_MSG(rest == src + covered, "round %d: rest is not where "
                      "the pieces end", round);
        if (count < max) {
            UT_ASSERT_MSG(*rest == '\0', "round %d: room for more pieces "
                          "but the text was not all split", round);
        }
    }
    return 0;
}
