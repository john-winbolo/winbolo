/*
 * The built-in Bot Host as a child process (src/bothost_child).
 *
 *   1. bothost_child_args: the game's settings reach the child's arguments
 *      (official-only, the slots, -bothost-record, the CPU speed file, the
 *      stdin-close rule); the ticket is never an argument; a buffer that is
 *      too small is refused; the ticket goes through hex and back.
 *   2. bothost_child_quote: arguments quoted for a Windows command line
 *      (spaces, quotes, backslashes, an empty argument).
 *   3. bothost_child_codec: the status and hello lines go through and back;
 *      bad lines are refused.
 *   4. bothost_child_restart: a game allows 3 restarts, each after the
 *      wait; a 4th end stops it; a new game starts the count again.
 *   5. bothost_child_job_limits (Windows; elsewhere it passes with a note):
 *      a WinBoloBotHost child in its job cannot take memory past the limit;
 *      the job carries the memory limit, the CPU cap, one process and
 *      KILL_ON_JOB_CLOSE; closing the job ends the child.
 */
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "../../src/bothost_child/bothost_child.h"
#include "test_harness.h"

static int argIndex(const char *const *argv, int argc, const char *a) {
    int i;
    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], a) == 0) return i;
    }
    return -1;
}

int run_bothost_child_args(void) {
    BotHostChildParams p;
    const char *argv[BOT_HOST_CHILD_MAX_ARGS];
    char buf[4096], small[64], hex[64];
    uint8_t ticket[16], back[16];
    int argc, i;

    memset(&p, 0, sizeof(p));
    p.serverHost = "192.168.0.10";
    p.serverPort = 27500;
    p.player = 3;
    p.brain = "GoalHunter";
    p.name = "Player Name";
    p.cacheDir = "C:/Users/x/bothost_cache";
    p.slots = 5;
    p.record = true;
    p.recordMinFreeGb = 20.0;
    p.officialOnly = true;
    p.cpuSpeedFile = "C:/Users/x/cpu_speed.json";
    argc = botHostChildBuildArgs(&p, "WinBoloBotHost.exe", buf, sizeof(buf),
                                 argv, BOT_HOST_CHILD_MAX_ARGS);
    UT_ASSERT_MSG(argc > 0, "argc %d", argc);
    UT_ASSERT(argv[argc] == NULL);
    UT_ASSERT(strcmp(argv[0], "WinBoloBotHost.exe") == 0);
    UT_ASSERT(argIndex(argv, argc, "--embed-child") > 0);
    UT_ASSERT(argIndex(argv, argc, "--exit-on-stdin-close") > 0);
    UT_ASSERT(argIndex(argv, argc, "--official-only") > 0);
    UT_ASSERT(argIndex(argv, argc, "--record") > 0);
    i = argIndex(argv, argc, "--slots");
    UT_ASSERT(i > 0 && strcmp(argv[i + 1], "5") == 0);
    i = argIndex(argv, argc, "--server");
    UT_ASSERT(i > 0 && strcmp(argv[i + 1], "192.168.0.10") == 0);
    i = argIndex(argv, argc, "--port");
    UT_ASSERT(i > 0 && strcmp(argv[i + 1], "27500") == 0);
    i = argIndex(argv, argc, "--player");
    UT_ASSERT(i > 0 && strcmp(argv[i + 1], "3") == 0);
    i = argIndex(argv, argc, "--name");
    UT_ASSERT(i > 0 && strcmp(argv[i + 1], "Player Name") == 0);
    i = argIndex(argv, argc, "--brain");
    UT_ASSERT(i > 0 && strcmp(argv[i + 1], "GoalHunter") == 0);
    i = argIndex(argv, argc, "--cachedir");
    UT_ASSERT(i > 0 && strcmp(argv[i + 1], p.cacheDir) == 0);
    i = argIndex(argv, argc, "--cpu-speed-file");
    UT_ASSERT(i > 0 && strcmp(argv[i + 1], p.cpuSpeedFile) == 0);
    i = argIndex(argv, argc, "--record-min-free-gb");
    UT_ASSERT(i > 0 && strncmp(argv[i + 1], "20", 2) == 0);
    UT_ASSERT(argIndex(argv, argc, "--ticket") < 0);

    /* The rules off: their flags are gone; slots 0 is the capacity. */
    p.record = false;
    p.officialOnly = false;
    p.slots = 0;
    p.recordMinFreeGb = 0.0;
    p.cpuSpeedFile = NULL;
    p.name = "";
    argc = botHostChildBuildArgs(&p, "WinBoloBotHost", buf, sizeof(buf),
                                 argv, BOT_HOST_CHILD_MAX_ARGS);
    UT_ASSERT(argc > 0);
    UT_ASSERT(argIndex(argv, argc, "--official-only") < 0);
    UT_ASSERT(argIndex(argv, argc, "--record") < 0);
    UT_ASSERT(argIndex(argv, argc, "--record-min-free-gb") < 0);
    UT_ASSERT(argIndex(argv, argc, "--cpu-speed-file") < 0);
    i = argIndex(argv, argc, "--slots");
    UT_ASSERT(i > 0 && strcmp(argv[i + 1], "0") == 0);
    i = argIndex(argv, argc, "--name");
    UT_ASSERT(i > 0 && argv[i + 1][0] == '\0');

    /* Too small: refused, not cut short. */
    UT_ASSERT(botHostChildBuildArgs(&p, "WinBoloBotHost", small,
                                    sizeof(small), argv,
                                    BOT_HOST_CHILD_MAX_ARGS) == -1);
    UT_ASSERT(botHostChildBuildArgs(&p, "WinBoloBotHost", buf, sizeof(buf),
                                    argv, 4) == -1);

    for (i = 0; i < 16; i++) ticket[i] = (uint8_t)(i * 17 + 3);
    botHostChildTicketHex(ticket, sizeof(ticket), hex, sizeof(hex));
    UT_ASSERT(strlen(hex) == 32);
    UT_ASSERT(botHostChildTicketParse(hex, back, sizeof(back)));
    UT_ASSERT(memcmp(ticket, back, sizeof(back)) == 0);
    UT_ASSERT(!botHostChildTicketParse("12zz", back, 2));
    UT_ASSERT(!botHostChildTicketParse("12", back, 2));
    UT_ASSERT(!botHostChildTicketParse("1234x", back, 2));
    return 0;
}

static int quoteIs(const char *arg, const char *want) {
    char out[256];
    size_t len = 0;
    if (!botHostChildQuoteWinArg(arg, out, sizeof(out), &len)) return 0;
    if (strcmp(out, want) != 0) {
        fprintf(stderr, "quote [%s] gave [%s], want [%s]\n", arg, out, want);
        return 0;
    }
    return len == strlen(want);
}

int run_bothost_child_quote(void) {
    char tiny[4];
    size_t len = 0;
    UT_ASSERT(quoteIs("plain", "plain"));
    UT_ASSERT(quoteIs("", "\"\""));
    UT_ASSERT(quoteIs("two words", "\"two words\""));
    UT_ASSERT(quoteIs("say \"hi\"", "\"say \\\"hi\\\"\""));
    UT_ASSERT(quoteIs("C:\\path\\", "C:\\path\\"));
    UT_ASSERT(quoteIs("C:\\my path\\", "\"C:\\my path\\\\\""));
    UT_ASSERT(quoteIs("a\\\"b", "\"a\\\\\\\"b\""));
    UT_ASSERT(quoteIs("C:\\a b\\c", "\"C:\\a b\\c\""));
    /* Appends at len. */
    {
        char out[64];
        len = 0;
        UT_ASSERT(botHostChildQuoteWinArg("a", out, sizeof(out), &len));
        out[len++] = ' ';
        UT_ASSERT(botHostChildQuoteWinArg("b c", out, sizeof(out), &len));
        UT_ASSERT(strcmp(out, "a \"b c\"") == 0);
    }
    len = 0;
    UT_ASSERT(!botHostChildQuoteWinArg("too long", tiny, sizeof(tiny), &len));
    return 0;
}

int run_bothost_child_codec(void) {
    BotHostChildStatus a, b;
    char line[160];
    int proto = 0;
    bool official = false;

    memset(&a, 0, sizeof(a));
    a.running = true;
    a.connected = true;
    a.refused = false;
    a.slots = 6;
    a.bots = 2;
    a.sameServer = false;
    a.consentAsking = true;
    botHostChildFormatStatus(&a, line, sizeof(line));
    memset(&b, 0, sizeof(b));
    UT_ASSERT(botHostChildParseStatus(line, &b));
    UT_ASSERT(b.running && b.connected && !b.refused && b.slots == 6 &&
              b.bots == 2 && !b.sameServer && b.consentAsking);
    a.running = false;
    a.refused = true;
    a.connected = false;
    a.consentAsking = false;
    a.sameServer = true;
    botHostChildFormatStatus(&a, line, sizeof(line));
    UT_ASSERT(botHostChildParseStatus(line, &b));
    UT_ASSERT(!b.running && b.refused && !b.connected && b.sameServer &&
              !b.consentAsking);
    UT_ASSERT(!botHostChildParseStatus("status 1 1 0", &b));
    UT_ASSERT(!botHostChildParseStatus("hello 1 official=1", &b));
    UT_ASSERT(!botHostChildParseStatus("status 1 1 0 -1 0 0 0", &b));
    UT_ASSERT(!botHostChildParseStatus(NULL, &b));

    UT_ASSERT(botHostChildParseHello("hello 1 official=1", &proto, &official));
    UT_ASSERT(proto == 1 && official);
    UT_ASSERT(botHostChildParseHello("hello 1 official=0", &proto, &official));
    UT_ASSERT(!official);
    UT_ASSERT(!botHostChildParseHello("status 1 1 0 4 0 0 0", &proto,
                                      &official));
    return 0;
}

int run_bothost_child_restart(void) {
    BotHostChildRestart r;
    uint32_t t = 0xFFFFF000u; /* near the wrap of the ms counter */
    int i;

    botHostChildRestartInit(&r);
    UT_ASSERT(!botHostChildRestartDue(&r, t));
    for (i = 0; i < BOT_HOST_CHILD_MAX_RESTARTS; i++) {
        UT_ASSERT_MSG(botHostChildRestartAfterExit(&r, t), "restart %d", i);
        UT_ASSERT(r.restarts == i + 1);
        /* Not before the wait. */
        UT_ASSERT(!botHostChildRestartDue(&r, t));
        UT_ASSERT(!botHostChildRestartDue(&r,
                                          t + BOT_HOST_CHILD_RESTART_WAIT_MS -
                                              1));
        UT_ASSERT(botHostChildRestartDue(&r,
                                         t + BOT_HOST_CHILD_RESTART_WAIT_MS));
        /* Once only. */
        UT_ASSERT(!botHostChildRestartDue(&r,
                                          t + BOT_HOST_CHILD_RESTART_WAIT_MS));
        t += BOT_HOST_CHILD_RESTART_WAIT_MS + 5000u;
    }
    /* The 4th end: no more for this game. */
    UT_ASSERT(!botHostChildRestartAfterExit(&r, t));
    UT_ASSERT(!r.waiting);
    UT_ASSERT(!botHostChildRestartDue(&r, t + 100000u));
    UT_ASSERT(BOT_HOST_CHILD_MAX_RESTARTS == 3);
    /* A new game. */
    botHostChildRestartInit(&r);
    UT_ASSERT(botHostChildRestartAfterExit(&r, t));
    UT_ASSERT(r.restarts == 1);
    UT_ASSERT(!botHostChildRestartAfterExit(NULL, t));
    return 0;
}

#ifdef _WIN32
/* Reads the child's stdout until it ends or waitMs passes. */
static bool readUntilEnd(BotHostChildProc *p, char *out, size_t outSize,
                         uint32_t waitMs, int *code) {
    size_t used = 0;
    uint32_t start = (uint32_t)SDL_GetTicks();
    out[0] = '\0';
    for (;;) {
        int n;
        if (used + 1 < outSize) {
            n = botHostChildRead(p, out + used, outSize - used - 1);
            if (n > 0) {
                used += (size_t)n;
                out[used] = '\0';
                continue;
            }
        }
        if (!botHostChildAlive(p, code)) {
            /* What is left in the pipe. */
            if (used + 1 < outSize) {
                n = botHostChildRead(p, out + used, outSize - used - 1);
                if (n > 0) used += (size_t)n;
                out[used] = '\0';
            }
            return true;
        }
        if ((uint32_t)SDL_GetTicks() - start > waitMs) return false;
        SDL_Delay(10);
    }
}
#endif

int run_bothost_child_job_limits(void) {
#ifndef _WIN32
    printf("    (the job object is Windows only: nothing to test here)\n");
    return 0;
#else
    char exe[1024], out[512];
    const char *base = SDL_GetBasePath();
    const char *argv[4];
    BotHostChildLimits lim;
    BotHostChildProc *p;
    SDL_PathInfo pi;
    char err[256];
    int code = 99, memMb = 0, cpuPct = 0, active = 0;
    bool killOnClose = false;
    uint32_t start;

    UT_ASSERT(base != NULL);
    SDL_snprintf(exe, sizeof(exe), "%sWinBoloBotHost.exe", base);
    UT_ASSERT_MSG(SDL_GetPathInfo(exe, &pi) && pi.type == SDL_PATHTYPE_FILE,
                  "no %s: build WinBoloBotHost first", exe);

    /* 1. Past the memory limit: the child's allocation is refused. */
    lim.memMb = 256;
    lim.cpuPct = 0;
    argv[0] = exe;
    argv[1] = "--test-alloc-mb";
    argv[2] = "1024";
    argv[3] = NULL;
    p = botHostChildSpawn(exe, argv, &lim, NULL, err, sizeof(err));
    UT_ASSERT_MSG(p != NULL, "spawn: %s", err);
    UT_ASSERT(readUntilEnd(p, out, sizeof(out), 30000, &code));
    botHostChildFree(p);
    UT_ASSERT_MSG(code == BOT_HOST_CHILD_EXIT_NO_MEM, "exit %d, out [%s]",
                  code, out);
    UT_ASSERT_MSG(strstr(out, "alloc failed at") != NULL, "out [%s]", out);
    {
        long at = -1;
        const char *s = strstr(out, "alloc failed at");
        if (s != NULL) sscanf(s, "alloc failed at %ld", &at);
        UT_ASSERT_MSG(at >= 0 && at < 256, "failed at %ld MiB", at);
    }

    /* 2. Under the limit: the same child is fine. */
    argv[2] = "64";
    code = 99;
    p = botHostChildSpawn(exe, argv, &lim, NULL, err, sizeof(err));
    UT_ASSERT_MSG(p != NULL, "spawn: %s", err);
    UT_ASSERT(readUntilEnd(p, out, sizeof(out), 30000, &code));
    botHostChildFree(p);
    UT_ASSERT_MSG(code == 0 && strstr(out, "alloc ok") != NULL,
                  "exit %d, out [%s]", code, out);

    /* 3. The default limits are on the job; closing the job ends the
     *    child (as the game's exit or crash would). */
    lim.memMb = BOT_HOST_CHILD_DEFAULT_MEM_MB;
    lim.cpuPct = BOT_HOST_CHILD_DEFAULT_CPU_PCT;
    argv[1] = "--test-sleep-ms";
    argv[2] = "60000";
    p = botHostChildSpawn(exe, argv, &lim, NULL, err, sizeof(err));
    UT_ASSERT_MSG(p != NULL, "spawn: %s", err);
    UT_ASSERT(botHostChildAlive(p, NULL));
    UT_ASSERT(botHostChildQueryJobForTest(p, &memMb, &cpuPct, &active,
                                          &killOnClose));
    UT_ASSERT_MSG(memMb == 4096, "memory limit %d", memMb);
    UT_ASSERT_MSG(cpuPct == 75, "CPU cap %d", cpuPct);
    UT_ASSERT_MSG(active == 1, "active process limit %d", active);
    UT_ASSERT(killOnClose);
    start = (uint32_t)SDL_GetTicks();
    UT_ASSERT(botHostChildCloseJobForTest(p));
    UT_ASSERT_MSG(botHostChildWait(p, 5000, &code),
                  "the child still runs after its job closed");
    UT_ASSERT((uint32_t)SDL_GetTicks() - start < 5000u);
    botHostChildFree(p);
    return 0;
#endif
}
