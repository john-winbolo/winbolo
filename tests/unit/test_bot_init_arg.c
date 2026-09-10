/*
 * Bot difficulty plumbing: the BRAIN_INIT_ARG token assembly and the
 * difficulty <-> word mapping (src/bolo/bot_manager.c).
 *
 * A bot's difficulty reaches the brain as one "difficulty=<word>" token
 * appended to whatever init arg the caller already staged (a CLI -bot-init
 * "[preset=keel;...]" suffix, usually nothing). Two things have to hold or a
 * bench silently measures the wrong brain:
 *
 *   - the existing tokens survive, separated by ';' — the split the brain's
 *     init-arg parser uses;
 *   - a token that would not fit is dropped WHOLE. Truncating it would hand
 *     the brain "difficulty=ha", which its parser rejects as a bad token —
 *     the failure would be a warning in a log nobody reads, not a build
 *     error, and the bot would run at a difficulty nobody asked for.
 *
 * The buffer is BRAIN_INIT_ARG_MAX (128), i.e. 127 usable bytes.
 */
#include <string.h>

#include "bot_manager.h"      /* botInitArgAppendToken, BRAIN_INIT_ARG_MAX */
#include "server_sim.h"       /* BOT_DIFFICULTY_*, botDifficultyName/FromName */

#include "test_harness.h"

int run_bot_init_arg_difficulty_token(void) {
    char arg[BRAIN_INIT_ARG_MAX];

    /* 1. Empty buffer: no leading separator. */
    arg[0] = '\0';
    UT_ASSERT(botInitArgAppendToken(arg, sizeof(arg), "difficulty=hard"));
    UT_ASSERT_MSG(strcmp(arg, "difficulty=hard") == 0,
                  "empty buffer produced '%s'", arg);

    /* 2. Existing tokens are preserved and the new one is appended with ';'
     *    — never a ',' (the -bot-init spec parser eats commas). */
    SDL_strlcpy(arg, "preset=keel;cfg=SQUAD_MAX_SIZE=3", sizeof(arg));
    UT_ASSERT(botInitArgAppendToken(arg, sizeof(arg), "difficulty=easy"));
    UT_ASSERT_MSG(strcmp(arg,
                         "preset=keel;cfg=SQUAD_MAX_SIZE=3;difficulty=easy") == 0,
                  "append onto existing tokens produced '%s'", arg);

    /* 3. Exact fit: a buffer with room for the separator + token + NUL and
     *    not one byte more still takes it. 127 usable bytes, so an existing
     *    arg of 127 - 1 - strlen(token) is the tightest that works. */
    {
        const char *token = "difficulty=medium";           /* 17 chars */
        const size_t fill = BRAIN_INIT_ARG_MAX - 1         /* 127 usable */
                          - 1                              /* the ';'    */
                          - strlen(token);
        memset(arg, 'x', fill);
        arg[fill] = '\0';
        UT_ASSERT(botInitArgAppendToken(arg, sizeof(arg), token));
        UT_ASSERT_MSG(strlen(arg) == BRAIN_INIT_ARG_MAX - 1,
                      "exact fit left %zu chars, expected %d",
                      strlen(arg), BRAIN_INIT_ARG_MAX - 1);
        UT_ASSERT_MSG(strcmp(arg + fill, ";difficulty=medium") == 0,
                      "exact fit mangled the token: '%s'", arg + fill);
    }

    /* 4. One byte too long: refused, and the buffer is left EXACTLY as it
     *    was. This is the case the caller logs and carries on from. */
    {
        const char *token = "difficulty=medium";
        const size_t fill = BRAIN_INIT_ARG_MAX - 1 - strlen(token); /* no room for ';' */
        char before[BRAIN_INIT_ARG_MAX];
        memset(arg, 'x', fill);
        arg[fill] = '\0';
        SDL_strlcpy(before, arg, sizeof(before));
        UT_ASSERT_MSG(!botInitArgAppendToken(arg, sizeof(arg), token),
                      "token that does not fit was accepted");
        UT_ASSERT_MSG(strcmp(arg, before) == 0,
                      "refused append still modified the buffer: '%s'", arg);
    }

    /* 5. Degenerate inputs are refused, not crashed on. */
    arg[0] = '\0';
    UT_ASSERT(!botInitArgAppendToken(NULL, sizeof(arg), "difficulty=hard"));
    UT_ASSERT(!botInitArgAppendToken(arg, 0, "difficulty=hard"));
    UT_ASSERT(!botInitArgAppendToken(arg, sizeof(arg), NULL));
    UT_ASSERT(!botInitArgAppendToken(arg, sizeof(arg), ""));
    UT_ASSERT_MSG(arg[0] == '\0', "refused append wrote into the buffer");

    return 0;
}

int run_bot_difficulty_names(void) {
    uint8_t d;

    /* The three wire values map to the three words the brain parses. */
    UT_ASSERT(strcmp(botDifficultyName(BOT_DIFFICULTY_EASY), "easy") == 0);
    UT_ASSERT(strcmp(botDifficultyName(BOT_DIFFICULTY_MEDIUM), "medium") == 0);
    UT_ASSERT(strcmp(botDifficultyName(BOT_DIFFICULTY_HARD), "hard") == 0);

    /* Out of range reads as hard — the shipping default, and what every
     * difficulty behaves like today. Never NULL, because the result goes
     * straight into a printf. */
    UT_ASSERT(strcmp(botDifficultyName(3), "hard") == 0);
    UT_ASSERT(strcmp(botDifficultyName(255), "hard") == 0);

    /* Round trip, case-insensitively (the -difficulty CLI flag). */
    for (uint8_t i = 0; i <= BOT_DIFFICULTY_MAX; i++) {
        d = 0xFF;
        UT_ASSERT_MSG(botDifficultyFromName(botDifficultyName(i), &d),
                      "'%s' did not parse", botDifficultyName(i));
        UT_ASSERT_MSG(d == i, "'%s' parsed to %u, expected %u",
                      botDifficultyName(i), (unsigned)d, (unsigned)i);
    }
    d = 0xFF;
    UT_ASSERT(botDifficultyFromName("HARD", &d) && d == BOT_DIFFICULTY_HARD);

    /* "normal" was the old label for wire value 1. */
    d = 0xFF;
    UT_ASSERT(botDifficultyFromName("normal", &d) && d == BOT_DIFFICULTY_MEDIUM);

    /* Anything else is refused with *out untouched, so the caller can say
     * so instead of silently picking a difficulty. */
    d = 42;
    UT_ASSERT(!botDifficultyFromName("bogus", &d));
    UT_ASSERT_MSG(d == 42, "rejected name still wrote *out (%u)", (unsigned)d);
    UT_ASSERT(!botDifficultyFromName("", &d));
    UT_ASSERT(!botDifficultyFromName(NULL, &d));
    UT_ASSERT(!botDifficultyFromName("easy ", &d));   /* the parse is exact */

    return 0;
}
