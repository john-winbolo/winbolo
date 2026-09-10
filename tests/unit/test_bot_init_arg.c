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

#include <SDL3/SDL.h>

#include "bot_manager.h"      /* botInitArgAppend*, BRAIN_INIT_ARG_MAX */
#include "brain_list.h"       /* BrainModes, brainListLoadModes */
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

/* The pair of tokens a bot is actually handed:
 *   mode=<modekey>;difficulty=<levelkey>
 * with the keys taken from the brain's own modes.txt (brain_list.h) and the
 * stored indices clamped against it. This is what botManagerStageInitArg
 * appends to whatever the caller staged, so it is what the brain parses. */
int run_bot_init_arg_mode_tokens(void) {
    char arg[BRAIN_INIT_ARG_MAX];
    BrainModes modes;

    /* No manifest for this name -> the synthesized default mode, which is
     * exactly the pre-manifest behaviour: mode "default", hard. */
    brainListLoadModes("wbtest_no_such_brain_at_all", &modes);

    arg[0] = '\0';
    botInitArgAppendModeTokens(arg, sizeof(arg), &modes, 0,
                               BOT_DIFFICULTY_HARD, 0);
    UT_ASSERT_MSG(strcmp(arg, "mode=default;difficulty=hard") == 0,
                  "default pair produced '%s'", arg);

    /* Easy in the same mode, and a staged -bot-init suffix still in front. */
    SDL_strlcpy(arg, "preset=keel", sizeof(arg));
    botInitArgAppendModeTokens(arg, sizeof(arg), &modes, 0,
                               BOT_DIFFICULTY_EASY, 0);
    UT_ASSERT_MSG(strcmp(arg, "preset=keel;mode=default;difficulty=easy") == 0,
                  "append onto a staged arg produced '%s'", arg);

    /* An explicit token already staged WINS over the lobby config: the brain
     * applies tokens in order, last write wins, so appending the lobby's
     * "difficulty=hard" after a bench's "difficulty=medium" would silently
     * make every benched bot Hard. Each key is skipped on its own. */
    SDL_strlcpy(arg, "difficulty=medium", sizeof(arg));
    botInitArgAppendModeTokens(arg, sizeof(arg), &modes, 0,
                               BOT_DIFFICULTY_HARD, 0);
    UT_ASSERT_MSG(strcmp(arg, "difficulty=medium;mode=default") == 0,
                  "explicit difficulty was not kept: '%s'", arg);
    SDL_strlcpy(arg, "preset=keel;mode=survival;difficulty=easy", sizeof(arg));
    botInitArgAppendModeTokens(arg, sizeof(arg), &modes, 0,
                               BOT_DIFFICULTY_HARD, 0);
    UT_ASSERT_MSG(strcmp(arg, "preset=keel;mode=survival;difficulty=easy") == 0,
                  "explicit mode+difficulty were not kept: '%s'", arg);
    /* A cfg= write of the constant is NOT a difficulty token: the lobby's
     * token is still appended (and, applied first, is then overridden by
     * the later cfg= exactly as any cfg= override is). */
    SDL_strlcpy(arg, "cfg=DIFFICULTY=easy", sizeof(arg));
    botInitArgAppendModeTokens(arg, sizeof(arg), &modes, 0,
                               BOT_DIFFICULTY_HARD, 0);
    UT_ASSERT_MSG(strcmp(arg, "cfg=DIFFICULTY=easy;mode=default;difficulty=hard") == 0,
                  "cfg= was mistaken for a token: '%s'", arg);
    UT_ASSERT(botInitArgHasKey("a=1;difficulty=x", "difficulty="));
    UT_ASSERT(!botInitArgHasKey("cfg=DIFFICULTY=x", "difficulty="));
    UT_ASSERT(!botInitArgHasKey("", "difficulty="));
    UT_ASSERT(!botInitArgHasKey(NULL, "difficulty="));

    /* An index past the end of the list never names something the brain
     * did not declare: the mode falls back to 0, the level to that mode's
     * own default (hard here). */
    arg[0] = '\0';
    botInitArgAppendModeTokens(arg, sizeof(arg), &modes, 9, 9, 0);
    UT_ASSERT_MSG(strcmp(arg, "mode=default;difficulty=hard") == 0,
                  "out-of-range indices produced '%s'", arg);

    /* A second mode, as brains/GoalHunter_1.7/modes.txt declares one: the
     * mode key is the manifest's, not a hardcoded word. */
    {
        BrainModes two;
        memset(&two, 0, sizeof(two));
        two.modeCount = 2;
        SDL_strlcpy(two.modes[0].key, "default", sizeof(two.modes[0].key));
        two.modes[0].levelCount = 1;
        SDL_strlcpy(two.modes[0].levels[0].key, "hard",
                    sizeof(two.modes[0].levels[0].key));
        SDL_strlcpy(two.modes[1].key, "survival", sizeof(two.modes[1].key));
        two.modes[1].levelCount = 3;
        SDL_strlcpy(two.modes[1].levels[0].key, "easy",
                    sizeof(two.modes[1].levels[0].key));
        SDL_strlcpy(two.modes[1].levels[1].key, "medium",
                    sizeof(two.modes[1].levels[1].key));
        SDL_strlcpy(two.modes[1].levels[2].key, "hard",
                    sizeof(two.modes[1].levels[2].key));
        two.modes[1].defaultLevel = 2;

        arg[0] = '\0';
        botInitArgAppendModeTokens(arg, sizeof(arg), &two, 1, 0, 0);
        UT_ASSERT_MSG(strcmp(arg, "mode=survival;difficulty=easy") == 0,
                      "survival pair produced '%s'", arg);

        /* A level index the OTHER mode has but this one does not takes the
         * mode's own default rather than reading off the end. */
        arg[0] = '\0';
        botInitArgAppendModeTokens(arg, sizeof(arg), &two, 0, 2, 0);
        UT_ASSERT_MSG(strcmp(arg, "mode=default;difficulty=hard") == 0,
                      "clamped level produced '%s'", arg);
    }

    /* Too long: each token is dropped whole and independently. Fill the
     * buffer so the mode token fits exactly and the difficulty one cannot,
     * and the mode token must still be there. */
    {
        const char *modeTok = "mode=default";              /* 12 chars */
        const size_t fill = BRAIN_INIT_ARG_MAX - 1         /* 127 usable */
                          - 1                              /* the ';'    */
                          - strlen(modeTok);
        memset(arg, 'x', fill);
        arg[fill] = '\0';
        botInitArgAppendModeTokens(arg, sizeof(arg), &modes, 0,
                                   BOT_DIFFICULTY_HARD, 0);
        UT_ASSERT_MSG(strlen(arg) == BRAIN_INIT_ARG_MAX - 1,
                      "mode token left %zu chars, expected %d",
                      strlen(arg), BRAIN_INIT_ARG_MAX - 1);
        UT_ASSERT_MSG(strcmp(arg + fill, ";mode=default") == 0,
                      "mode token mangled: '%s'", arg + fill);
        UT_ASSERT_MSG(strstr(arg, "difficulty=") == NULL,
                      "difficulty token was truncated in instead of dropped");
    }

    /* Neither token fits: the buffer is left exactly as it was. */
    {
        char before[BRAIN_INIT_ARG_MAX];
        memset(arg, 'x', BRAIN_INIT_ARG_MAX - 1);
        arg[BRAIN_INIT_ARG_MAX - 1] = '\0';
        SDL_strlcpy(before, arg, sizeof(before));
        botInitArgAppendModeTokens(arg, sizeof(arg), &modes, 0,
                                   BOT_DIFFICULTY_HARD, 0);
        UT_ASSERT_MSG(strcmp(arg, before) == 0,
                      "a full buffer was still written to");
    }

    /* Degenerate inputs are refused, not crashed on. */
    {
        BrainModes empty;
        memset(&empty, 0, sizeof(empty));
        arg[0] = '\0';
        botInitArgAppendModeTokens(NULL, sizeof(arg), &modes, 0, 0, 0);
        botInitArgAppendModeTokens(arg, 0, &modes, 0, 0, 0);
        botInitArgAppendModeTokens(arg, sizeof(arg), NULL, 0, 0, 0);
        botInitArgAppendModeTokens(arg, sizeof(arg), &empty, 0, 0, 0);
        UT_ASSERT_MSG(arg[0] == '\0', "a refused append wrote '%s'", arg);
    }

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
