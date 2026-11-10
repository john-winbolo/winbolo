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
 *   6. bothost_child_relay_codec: the relay frames and the hello with its
 *      fullmap field go through and back; bad frames are refused.
 *   7. bothost_child_sandbox_files: a WinBoloBotHost child in its file
 *      sandbox reads a file in its read folder, cannot read or write a file
 *      outside, and writes in its cache folder.
 *   8. bothost_child_sandbox_net: the sandboxed child's own datagram never
 *      arrives; the same child with no sandbox (the control) sends it.
 *   9. bothost_child_relay: the sandboxed child reaches a server through
 *      the relay and back; its datagram to another port is refused.
 *  10. bothost_child_machine_id: the sandboxed child, given the game's
 *      machine id and salts, answers a salt with the game's own answer,
 *      does not answer a salt the game answered, and tells the game each
 *      salt it answers.
 *  11. bothost_child_no_unsafe: the child refuses --allow-unsafe-brains and
 *      -allow-unsafe-brains (two layers: the Lua sandbox inside the OS
 *      sandbox).
 *  12. bothost_child_copy_tree: the copy the game runs when the program's
 *      folder keeps its ACL copies new and changed files (a change of the
 *      same size too), removes what the source no longer has, and leaves
 *      no temporary file.
 *  16. bothost_child_seccomp (Linux): under the seccomp filter the child
 *      starts threads and a LuaJIT state; fork, a clone with no
 *      CLONE_THREAD and the pid calls on the game are refused; the same
 *      calls on itself work. With no Landlock it uses the filter alone
 *      (botHostChildTestSeccompOnly). The control, with no filter, shows
 *      the same calls are allowed there. The calls that change a file's
 *      mode, owner, times or extended attributes, truncate by path, System
 *      V IPC and prctl PR_SET_PDEATHSIG are refused; ftruncate and
 *      PR_GET_PDEATHSIG work.
 *  17. bothost_child_stdin_full: a child that does not read its stdin
 *      fills the pipe; the game's write then fails at once and does not
 *      wait, and the child still runs.
 * Tests 7 to 10 need the sandbox: where this computer has none (Linux with
 * no Landlock, other systems) they pass with a note. WINBOLO_TEST_TMP names
 * the folder for their files (default: TEMP, or /tmp).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "../../src/bothost_child/bothost_child.h"
#include "machine_id.h"
#include "platform_net.h"
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
    char buf[4096], tooSmall[64], hex[64];
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
    UT_ASSERT(botHostChildBuildArgs(&p, "WinBoloBotHost", tooSmall,
                                    sizeof(tooSmall), argv,
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
    bool official = false, fullMap = true, table = true;

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

    UT_ASSERT(botHostChildParseHello("hello 1 official=1", &proto, &official,
                                     &fullMap, &table));
    UT_ASSERT(proto == 1 && official && !fullMap && !table);
    UT_ASSERT(botHostChildParseHello("hello 2 official=0 fullmap=1", &proto,
                                     &official, &fullMap, &table));
    UT_ASSERT(proto == 2 && !official && fullMap && !table);
    UT_ASSERT(botHostChildParseHello("hello 2 official=1 fullmap=0 table=1",
                                     &proto, &official, &fullMap, &table));
    UT_ASSERT(proto == 2 && official && !fullMap && table);
    UT_ASSERT(!botHostChildParseHello("status 1 1 0 4 0 0 0", &proto,
                                      &official, &fullMap, &table));

    /* The match rule. fm is this program's fullmap. */
    {
        const bool fm = botHostChildBuildFullMap();
        const int pr = BOT_HOST_CHILD_PROTOCOL;
        UT_ASSERT(botHostChildHelloMatches(pr, false, fm, false, false, false));
        UT_ASSERT(botHostChildHelloMatches(pr, true, fm, true, true, true));
        /* An official-only game with the table: a child with no table is
         * refused, like a protocol mismatch. */
        UT_ASSERT(!botHostChildHelloMatches(pr, true, fm, false, true, true));
        /* An official-only game with no table of its own (a run-time
         * --official-only on a build with no table) asks for none. */
        UT_ASSERT(botHostChildHelloMatches(pr, true, fm, false, true, false));
        /* A game that is not official-only does not need the table. */
        UT_ASSERT(botHostChildHelloMatches(pr, false, fm, false, false, true));
        UT_ASSERT(!botHostChildHelloMatches(pr, false, fm, true, true, true));
        UT_ASSERT(!botHostChildHelloMatches(pr + 1, true, fm, true, true,
                                            true));
        UT_ASSERT(!botHostChildHelloMatches(pr, true, !fm, true, true, true));
    }
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
    p = botHostChildSpawn(exe, argv, &lim, NULL, NULL, err, sizeof(err),
                          NULL);
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
    p = botHostChildSpawn(exe, argv, &lim, NULL, NULL, err, sizeof(err),
                          NULL);
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
    p = botHostChildSpawn(exe, argv, &lim, NULL, NULL, err, sizeof(err),
                          NULL);
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

/* ---- The relay codec -------------------------------------------------- */

int run_bothost_child_relay_codec(void) {
    uint8_t frame[BOT_HOST_RELAY_MAX_FRAME], big[BOT_HOST_RELAY_MAX_DATAGRAM + 1];
    uint8_t type = 0, sock = 0;
    uint16_t len = 0, port = 0;
    uint32_t ip = 0;
    const uint8_t *data = NULL;
    size_t n, dlen = 0;
    char line[BOT_HOST_CHILD_LINE_MAX];
    int proto = 0;
    bool official = true, fullMap = false, table = false;

    /* SEND with data. */
    n = botHostRelayEncode(BOT_HOST_RELAY_SEND, 5, 0x0100007Fu, 0x3412u,
                           (const uint8_t *)"hello", 5, frame, sizeof(frame));
    UT_ASSERT(n == BOT_HOST_RELAY_HDR + BOT_HOST_RELAY_ADDR + 5);
    UT_ASSERT(botHostRelayParseHeader(frame, &type, &sock, &len));
    UT_ASSERT(type == BOT_HOST_RELAY_SEND && sock == 5 &&
              len == BOT_HOST_RELAY_ADDR + 5);
    UT_ASSERT(botHostRelayParseBody(frame + BOT_HOST_RELAY_HDR, len, &ip,
                                    &port, &data, &dlen));
    UT_ASSERT(ip == 0x0100007Fu && port == 0x3412u && dlen == 5 &&
              memcmp(data, "hello", 5) == 0);
    /* OPEN and CLOSE have no body. */
    n = botHostRelayEncode(BOT_HOST_RELAY_OPEN, 63, 0, 0, NULL, 0, frame,
                           sizeof(frame));
    UT_ASSERT(n == BOT_HOST_RELAY_HDR);
    UT_ASSERT(botHostRelayParseHeader(frame, &type, &sock, &len));
    UT_ASSERT(type == BOT_HOST_RELAY_OPEN && sock == 63 && len == 0);
    /* An empty datagram is valid. */
    n = botHostRelayEncode(BOT_HOST_RELAY_RECV, 0, 1, 2, NULL, 0, frame,
                           sizeof(frame));
    UT_ASSERT(n == BOT_HOST_RELAY_HDR + BOT_HOST_RELAY_ADDR);
    /* Refused: a socket past the limit, a datagram too long, a buffer too
     * small, an unknown type. */
    UT_ASSERT(botHostRelayEncode(BOT_HOST_RELAY_OPEN, BOT_HOST_RELAY_MAX_SOCKS,
                                 0, 0, NULL, 0, frame, sizeof(frame)) == 0);
    memset(big, 0, sizeof(big));
    UT_ASSERT(botHostRelayEncode(BOT_HOST_RELAY_SEND, 1, 0, 0, big,
                                 sizeof(big), frame, sizeof(frame)) == 0);
    UT_ASSERT(botHostRelayEncode(BOT_HOST_RELAY_SEND, 1, 0, 0, big, 8, frame,
                                 8) == 0);
    UT_ASSERT(botHostRelayEncode(9, 1, 0, 0, NULL, 0, frame,
                                 sizeof(frame)) == 0);
    /* Bad headers. */
    {
        uint8_t h[4] = { BOT_HOST_RELAY_OPEN, 1, 1, 0 }; /* OPEN with a body */
        UT_ASSERT(!botHostRelayParseHeader(h, &type, &sock, &len));
        h[0] = BOT_HOST_RELAY_SEND;
        h[2] = 5; /* shorter than the address */
        UT_ASSERT(!botHostRelayParseHeader(h, &type, &sock, &len));
        h[2] = 0xFF;
        h[3] = 0xFF; /* longer than a datagram */
        UT_ASSERT(!botHostRelayParseHeader(h, &type, &sock, &len));
        h[2] = 6;
        h[3] = 0;
        h[1] = BOT_HOST_RELAY_MAX_SOCKS;
        UT_ASSERT(!botHostRelayParseHeader(h, &type, &sock, &len));
        h[1] = 0;
        h[0] = 0;
        UT_ASSERT(!botHostRelayParseHeader(h, &type, &sock, &len));
    }
    UT_ASSERT(!botHostRelayParseBody(frame, 5, &ip, &port, &data, &dlen));

    /* The hello carries fullmap and the table flag. */
    botHostChildFormatHello(BOT_HOST_CHILD_PROTOCOL, false, true, false, line,
                            sizeof(line));
    UT_ASSERT(botHostChildParseHello(line, &proto, &official, &fullMap,
                                     &table));
    UT_ASSERT(proto == BOT_HOST_CHILD_PROTOCOL && !official && fullMap &&
              !table);
    botHostChildFormatHello(BOT_HOST_CHILD_PROTOCOL, true, false, true, line,
                            sizeof(line));
    UT_ASSERT(botHostChildParseHello(line, &proto, &official, &fullMap,
                                     &table));
    UT_ASSERT(official && !fullMap && table);
    return 0;
}

/* ---- Sandbox helpers ---------------------------------------------------- */

static void setEnv(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value != NULL ? value : "");
#else
    if (value != NULL) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
#endif
}

static bool childExePath(char *out, size_t outSize) {
    const char *base = SDL_GetBasePath();
    SDL_PathInfo pi;
    if (base == NULL) return false;
#ifdef _WIN32
    SDL_snprintf(out, outSize, "%sWinBoloBotHost.exe", base);
#else
    SDL_snprintf(out, outSize, "%sWinBoloBotHost", base);
#endif
    return SDL_GetPathInfo(out, &pi) && pi.type == SDL_PATHTYPE_FILE;
}

/* The program's folder, with no separator at the end. */
static void exeDirOf(const char *exe, char *out, size_t outSize) {
    size_t n;
    SDL_strlcpy(out, exe, outSize);
    n = strlen(out);
    while (n > 0 && out[n - 1] != '/' && out[n - 1] != '\\') n--;
    if (n > 1) n--;
    out[n] = '\0';
}

/* A new folder for one test: <tmp>/wbsb_<name>_<ticks>. */
static bool makeTestDir(const char *name, char *out, size_t outSize) {
    const char *tmp = SDL_getenv("WINBOLO_TEST_TMP");
#ifdef _WIN32
    if (tmp == NULL || tmp[0] == '\0') tmp = SDL_getenv("TEMP");
    if (tmp == NULL || tmp[0] == '\0') tmp = SDL_getenv("TMP");
#endif
    if (tmp == NULL || tmp[0] == '\0') tmp = "/tmp";
    SDL_snprintf(out, outSize, "%s/wbsb_%s_%llu", tmp, name,
                 (unsigned long long)SDL_GetTicksNS());
    return SDL_CreateDirectory(out);
}

static bool writeFile(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) return false;
    fputs(text, f);
    return fclose(f) == 0;
}

/* Removes a test folder and what is in it (one level of folders). */
static SDL_EnumerationResult SDLCALL rmOne(void *ud, const char *dir,
                                          const char *name) {
    char path[1200];
    SDL_PathInfo pi;
    (void)ud;
    SDL_snprintf(path, sizeof(path), "%s%s", dir, name);
    if (SDL_GetPathInfo(path, &pi) && pi.type == SDL_PATHTYPE_DIRECTORY) {
        (void)SDL_EnumerateDirectory(path, rmOne, NULL);
    }
    (void)SDL_RemovePath(path);
    return SDL_ENUM_CONTINUE;
}

static void removeTestDir(const char *dir) {
    (void)SDL_EnumerateDirectory(dir, rmOne, NULL);
    (void)SDL_RemovePath(dir);
}

/* False (with a note) when this computer has no sandbox: the test passes. */
static bool sandboxHere(void) {
    char why[256];
    why[0] = '\0';
    if (!botHostChildSandboxAvailable(why, sizeof(why))) {
        printf("    (no sandbox here: %s; Landlock ABI %d)\n", why,
               botHostChildLandlockAbi());
        return false;
    }
#if defined(__APPLE__)
    printf("    (the macOS sandbox is not tested here)\n");
    return false;
#else
    printf("    (Landlock ABI %d; -1: not Linux)\n", botHostChildLandlockAbi());
    return true;
#endif
}

/* The sandbox the game would give: the program's folder read, the read
 * folder, the cache written. The strings stay in the caller's buffers. */
static bool prepareSandbox(BotHostChildSandbox *sb, const char *exeDir,
                           const char *readDir, const char *cacheDir) {
    unsigned readFailed = 0;
    char why[512];
    memset(sb, 0, sizeof(*sb));
    sb->on = true;
    (void)botHostChildSandboxAdd(sb->readDirs, &sb->readDirCount, exeDir);
    if (readDir != NULL) {
        (void)botHostChildSandboxAdd(sb->readDirs, &sb->readDirCount,
                                     readDir);
    }
    if (cacheDir != NULL) {
        (void)botHostChildSandboxAdd(sb->writeDirs, &sb->writeDirCount,
                                     cacheDir);
    }
    why[0] = '\0';
    if (!botHostChildSandboxPrepare(sb, &readFailed, why, sizeof(why))) {
        fprintf(stderr, "    prepare failed: %s\n", why);
        return false;
    }
    if (readFailed != 0) {
        fprintf(stderr, "    a read folder could not be granted (%#x)\n",
                readFailed);
        return false;
    }
    return true;
}

/* Runs the child at exe with one test mode; its output in out. */
static BotHostChildProc *spawnTest(const char *exe, const char *const *args,
                                   const BotHostChildSandbox *sb,
                                   const char *log) {
    const char *argv[16];
    BotHostChildLimits lim;
    char err[256];
    int i, n = 0;
    BotHostChildProc *p;
    bool sbFailed = false;
    argv[n++] = exe;
    for (i = 0; args[i] != NULL && n < 15; i++) argv[n++] = args[i];
    argv[n] = NULL;
    lim.memMb = 0;
    lim.cpuPct = 0;
    err[0] = '\0';
    p = botHostChildSpawn(exe, argv, &lim, sb, log, err, sizeof(err),
                          &sbFailed);
    if (p == NULL) {
        fprintf(stderr, "    spawn failed (%s, sandbox %d)\n", err,
                (int)sbFailed);
    }
    return p;
}

static bool runTest(const char *exe, const char *const *args,
                    const BotHostChildSandbox *sb, const char *log,
                    char *out, size_t outSize, int *code) {
    BotHostChildProc *p = spawnTest(exe, args, sb, log);
    bool ended;
    if (p == NULL) return false;
    ended = readUntilEnd(p, out, outSize, 30000, code);
    botHostChildFree(p);
    return ended;
}

int run_bothost_child_sandbox_files(void) {
    char exe[1024], exeDir[1024], root[1024], readDir[1100], cache[1100];
    char outside[1100], inFile[1200], outFile[1200], cacheFile[1200];
    char outWrite[1200], log[1200], out[512];
    BotHostChildSandbox sb;
    int code = 99;
    SDL_PathInfo pi;

    UT_ASSERT_MSG(childExePath(exe, sizeof(exe)),
                  "no WinBoloBotHost: build it first");
    if (!sandboxHere()) return 0;
    exeDirOf(exe, exeDir, sizeof(exeDir));
    UT_ASSERT(makeTestDir("files", root, sizeof(root)));
    SDL_snprintf(readDir, sizeof(readDir), "%s/read", root);
    SDL_snprintf(cache, sizeof(cache), "%s/cache", root);
    SDL_snprintf(outside, sizeof(outside), "%s/outside", root);
    UT_ASSERT(SDL_CreateDirectory(readDir) && SDL_CreateDirectory(cache) &&
              SDL_CreateDirectory(outside));
    SDL_snprintf(inFile, sizeof(inFile), "%s/allowed.txt", readDir);
    SDL_snprintf(outFile, sizeof(outFile), "%s/secret.txt", outside);
    SDL_snprintf(cacheFile, sizeof(cacheFile), "%s/written.txt", cache);
    SDL_snprintf(outWrite, sizeof(outWrite), "%s/planted.txt", outside);
    SDL_snprintf(log, sizeof(log), "%s/child.log", root);
    UT_ASSERT(writeFile(inFile, "allowed\n") &&
              writeFile(outFile, "secret\n"));
    UT_ASSERT(prepareSandbox(&sb, exeDir, readDir, cache));

    {
        const char *a[] = { "--test-read", inFile, NULL };
        UT_ASSERT(runTest(exe, a, &sb, log, out, sizeof(out), &code));
        printf("    read in the read folder: exit %d, %s", code, out);
        UT_ASSERT_MSG(code == 0 && strstr(out, "read ok") != NULL,
                      "exit %d [%s]", code, out);
    }
    {
        const char *a[] = { "--test-read", outFile, NULL };
        UT_ASSERT(runTest(exe, a, &sb, log, out, sizeof(out), &code));
        printf("    read outside: exit %d, %s", code, out);
        UT_ASSERT_MSG(code == BOT_HOST_CHILD_EXIT_DENIED &&
                          strstr(out, "read denied") != NULL,
                      "exit %d [%s]", code, out);
    }
    {
        const char *a[] = { "--test-write", outWrite, NULL };
        UT_ASSERT(runTest(exe, a, &sb, log, out, sizeof(out), &code));
        printf("    write outside: exit %d, %s", code, out);
        UT_ASSERT_MSG(code == BOT_HOST_CHILD_EXIT_DENIED &&
                          strstr(out, "write denied") != NULL,
                      "exit %d [%s]", code, out);
        UT_ASSERT_MSG(!SDL_GetPathInfo(outWrite, &pi),
                      "the child made %s", outWrite);
    }
    {
        const char *a[] = { "--test-write", inFile, NULL };
        UT_ASSERT(runTest(exe, a, &sb, log, out, sizeof(out), &code));
        printf("    write in the read folder: exit %d, %s", code, out);
        UT_ASSERT_MSG(code == BOT_HOST_CHILD_EXIT_DENIED,
                      "exit %d [%s]", code, out);
    }
    {
        const char *a[] = { "--test-write", cacheFile, NULL };
        UT_ASSERT(runTest(exe, a, &sb, log, out, sizeof(out), &code));
        printf("    write in the cache: exit %d, %s", code, out);
        UT_ASSERT_MSG(code == 0 && strstr(out, "write ok") != NULL,
                      "exit %d [%s]", code, out);
        UT_ASSERT(SDL_GetPathInfo(cacheFile, &pi) &&
                  pi.type == SDL_PATHTYPE_FILE);
    }
    {
        /* The control: with no sandbox the same read works. */
        const char *a[] = { "--test-read", outFile, NULL };
        UT_ASSERT(runTest(exe, a, NULL, log, out, sizeof(out), &code));
        UT_ASSERT_MSG(code == 0, "control: exit %d [%s]", code, out);
    }
    removeTestDir(root);
    return 0;
}

/* A UDP socket on 127.0.0.1 at a port the system picks. */
static bolo_socket_t openLoopback(uint16_t *port) {
    struct sockaddr_in a;
    socklen_t al = sizeof(a);
    bolo_socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == BOLO_INVALID_SOCKET) return s;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        getsockname(s, (struct sockaddr *)&a, &al) != 0) {
        closesocket(s);
        return BOLO_INVALID_SOCKET;
    }
    *port = ntohs(a.sin_port);
    return s;
}

/* One datagram within waitMs, or -1. */
static int recvWait(bolo_socket_t s, char *buf, int size, uint32_t waitMs,
                    struct sockaddr_in *from) {
    fd_set rd;
    struct timeval tv;
    socklen_t fl = sizeof(*from);
    FD_ZERO(&rd);
    FD_SET(s, &rd);
    tv.tv_sec = (long)(waitMs / 1000u);
    tv.tv_usec = (long)((waitMs % 1000u) * 1000u);
    if (select((int)s + 1, &rd, NULL, NULL, &tv) <= 0) return -1;
    return (int)recvfrom(s, buf, size, 0, (struct sockaddr *)from, &fl);
}

int run_bothost_child_sandbox_net(void) {
    char exe[1024], exeDir[1024], root[1024], log[1200], out[512];
    char portText[16], buf[64];
    BotHostChildSandbox sb;
    struct sockaddr_in from;
    bolo_socket_t s;
    uint16_t port = 0;
    int code = 99, got;

    UT_ASSERT_MSG(childExePath(exe, sizeof(exe)),
                  "no WinBoloBotHost: build it first");
    if (!sandboxHere()) return 0;
    UT_ASSERT(bolo_net_init() == 0);
    exeDirOf(exe, exeDir, sizeof(exeDir));
    UT_ASSERT(makeTestDir("net", root, sizeof(root)));
    SDL_snprintf(log, sizeof(log), "%s/child.log", root);
    UT_ASSERT(prepareSandbox(&sb, exeDir, NULL, NULL));
    s = openLoopback(&port);
    UT_ASSERT(s != BOLO_INVALID_SOCKET);
    SDL_snprintf(portText, sizeof(portText), "%u", (unsigned)port);
    {
        const char *a[] = { "--test-net", "127.0.0.1", portText, NULL };
        UT_ASSERT(runTest(exe, a, &sb, log, out, sizeof(out), &code));
        got = recvWait(s, buf, sizeof(buf), 1000, &from);
        printf("    sandboxed: exit %d, %s    datagram arrived: %s\n", code,
               out, got > 0 ? "yes" : "no");
        /* The child's line may be "net sent" where the system drops the
         * datagram later (an AppContainer with no network capability):
         * what counts is that nothing arrives. */
        UT_ASSERT_MSG(got <= 0, "the sandboxed child's datagram arrived");
    }
    {
        const char *a[] = { "--test-net", "127.0.0.1", portText, NULL };
        UT_ASSERT(runTest(exe, a, NULL, log, out, sizeof(out), &code));
        got = recvWait(s, buf, sizeof(buf), 3000, &from);
        printf("    control (no sandbox): exit %d, %s    datagram arrived: "
               "%s\n", code, out, got > 0 ? "yes" : "no");
        UT_ASSERT_MSG(code == 0 && got == 7 && memcmp(buf, "sandbox", 7) == 0,
                      "control: exit %d, got %d", code, got);
    }
    closesocket(s);
    removeTestDir(root);
    return 0;
}

int run_bothost_child_relay(void) {
    char exe[1024], exeDir[1024], root[1024], log[1200], out[512];
    char portText[16], buf[64];
    BotHostChildSandbox sb;
    BotHostChildProc *p;
    BotHostRelay *r;
    bolo_socket_t server, other;
    uint16_t port = 0, otherPort = 0;
    unsigned long toServer = 0, fromServer = 0, refused = 0;
    int code = 99, pings = 0, strays = 0;
    uint32_t start;
    size_t used = 0;

    UT_ASSERT_MSG(childExePath(exe, sizeof(exe)),
                  "no WinBoloBotHost: build it first");
    if (!sandboxHere()) return 0;
    UT_ASSERT(bolo_net_init() == 0);
    exeDirOf(exe, exeDir, sizeof(exeDir));
    UT_ASSERT(makeTestDir("relay", root, sizeof(root)));
    SDL_snprintf(log, sizeof(log), "%s/child.log", root);
    UT_ASSERT(prepareSandbox(&sb, exeDir, NULL, NULL));
    /* The server and the port after it (the child's stray datagram goes
     * there, and the relay must refuse it). */
    server = BOLO_INVALID_SOCKET;
    other = BOLO_INVALID_SOCKET;
    for (int tries = 0; tries < 20 && other == BOLO_INVALID_SOCKET; tries++) {
        struct sockaddr_in a;
        if (server != BOLO_INVALID_SOCKET) closesocket(server);
        server = openLoopback(&port);
        UT_ASSERT(server != BOLO_INVALID_SOCKET);
        other = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons((uint16_t)(port + 1));
        if (bind(other, (struct sockaddr *)&a, sizeof(a)) != 0) {
            closesocket(other);
            other = BOLO_INVALID_SOCKET;
        } else {
            otherPort = (uint16_t)(port + 1);
        }
    }
    UT_ASSERT(other != BOLO_INVALID_SOCKET && otherPort == port + 1);
    SDL_snprintf(portText, sizeof(portText), "%u", (unsigned)port);
    {
        const char *a[] = { "--test-relay", "127.0.0.1", portText, NULL };
        p = spawnTest(exe, a, &sb, log);
        UT_ASSERT(p != NULL);
    }
    r = botHostRelayStart(p, "127.0.0.1", port);
    UT_ASSERT(r != NULL);
    out[0] = '\0';
    start = (uint32_t)SDL_GetTicks();
    while ((uint32_t)SDL_GetTicks() - start < 10000u) {
        struct sockaddr_in from;
        int n = recvWait(server, buf, sizeof(buf), 20, &from);
        if (n == 4 && memcmp(buf, "ping", 4) == 0) {
            pings++;
            (void)sendto(server, "pong", 4, 0, (struct sockaddr *)&from,
                         sizeof(from));
        }
        if (recvWait(other, buf, sizeof(buf), 0, &from) > 0) strays++;
        if (used + 1 < sizeof(out)) {
            int m = botHostChildRead(p, out + used, sizeof(out) - used - 1);
            if (m > 0) {
                used += (size_t)m;
                out[used] = '\0';
            }
        }
        if (!botHostChildAlive(p, &code)) break;
    }
    UT_ASSERT(readUntilEnd(p, out + used, sizeof(out) - used, 5000, &code));
    botHostChildKill(p);
    botHostRelayCounts(r, &toServer, &fromServer, &refused);
    botHostRelayStop(r);
    botHostChildFree(p);
    printf("    child: exit %d, %s    pings %d, to the server %lu, from it "
           "%lu, refused %lu, stray datagrams %d\n", code, out, pings,
           toServer, fromServer, refused, strays);
    UT_ASSERT_MSG(code == 0 && strstr(out, "relay ok") != NULL,
                  "exit %d [%s]", code, out);
    UT_ASSERT(pings >= 1 && toServer >= 1 && fromServer >= 1);
    UT_ASSERT_MSG(refused >= 1, "the stray datagram was not refused");
    UT_ASSERT_MSG(strays == 0, "a stray datagram arrived");
    closesocket(server);
    closesocket(other);
    removeTestDir(root);
    return 0;
}

int run_bothost_child_machine_id(void) {
    char exe[1024], exeDir[1024], root[1024], idDir[1100], log[1200];
    char out[2048], line[BOT_HOST_CHILD_LINE_MAX], hex[80], want[80];
    static char lines[BOT_HOST_CHILD_MACHINE_LINES][BOT_HOST_CHILD_LINE_MAX];
    uint8_t id[MACHINE_ID_LEN], s1[MACHINE_SALT_LEN], s2[MACHINE_SALT_LEN];
    uint8_t hash[MACHINE_HASH_LEN];
    char s1hex[40], s2hex[40];
    BotHostChildSandbox sb;
    BotHostChildProc *p;
    int code = 99, n, i;

    UT_ASSERT_MSG(childExePath(exe, sizeof(exe)),
                  "no WinBoloBotHost: build it first");
    if (!sandboxHere()) return 0;
    exeDirOf(exe, exeDir, sizeof(exeDir));
    UT_ASSERT(makeTestDir("machine", root, sizeof(root)));
    SDL_snprintf(idDir, sizeof(idDir), "%s/id", root);
    SDL_snprintf(log, sizeof(log), "%s/child.log", root);
    UT_ASSERT(SDL_CreateDirectory(idDir));
    /* The child inherits this too: it must not need it (it cannot read
     * it in the sandbox). */
    setEnv("WINBOLO_MACHINE_ID_DIR", idDir);
    UT_ASSERT(machineIdInit());
    UT_ASSERT(machineIdGet(id));
    UT_ASSERT(machineRandomBytes(s1, sizeof(s1)) &&
              machineRandomBytes(s2, sizeof(s2)));
    /* The game answered s2 before: the child must not answer it again. */
    UT_ASSERT(machineIdAnswerSalt(s2, hash));
    botHostChildTicketHex(s1, sizeof(s1), s1hex, sizeof(s1hex));
    botHostChildTicketHex(s2, sizeof(s2), s2hex, sizeof(s2hex));
    machineIdHashSalt(id, s1, hash);
    botHostChildTicketHex(hash, sizeof(hash), want, sizeof(want));

    UT_ASSERT(prepareSandbox(&sb, exeDir, NULL, NULL));
    {
        const char *a[] = { "--test-salt", NULL };
        p = spawnTest(exe, a, &sb, log);
        UT_ASSERT(p != NULL);
    }
    n = botHostChildMachineLines(lines, BOT_HOST_CHILD_MACHINE_LINES);
    UT_ASSERT_MSG(n >= 2, "%d machine lines", n);
    for (i = 0; i < n; i++) UT_ASSERT(botHostChildWriteLine(p, lines[i]));
    SDL_snprintf(line, sizeof(line), "ask %s", s1hex);
    UT_ASSERT(botHostChildWriteLine(p, line));
    SDL_snprintf(line, sizeof(line), "ask %s", s2hex);
    UT_ASSERT(botHostChildWriteLine(p, line));
    UT_ASSERT(botHostChildWriteLine(p, "end"));
    UT_ASSERT(readUntilEnd(p, out, sizeof(out), 30000, &code));
    botHostChildFree(p);
    printf("    child: exit %d\n%s", code, out);
    UT_ASSERT_MSG(code == 0, "exit %d", code);
    SDL_snprintf(hex, sizeof(hex), "answer %s", want);
    /* The same answer as the game: the server sees one computer. */
    UT_ASSERT_MSG(strstr(out, hex) != NULL, "want [%s]", hex);
    UT_ASSERT(strstr(out, "answer none") != NULL);
    SDL_snprintf(line, sizeof(line), "salt %s", s1hex);
    UT_ASSERT_MSG(strstr(out, line) != NULL, "no [%s]", line);
    SDL_snprintf(line, sizeof(line), "salt %s", s2hex);
    UT_ASSERT(strstr(out, line) == NULL);
    /* The game keeps the salt the child answered. */
    machineIdNoteSalt(s1);
    UT_ASSERT(!machineIdAnswerSalt(s1, hash));
    setEnv("WINBOLO_MACHINE_ID_DIR", NULL);
    removeTestDir(root);
    return 0;
}

int run_bothost_child_no_unsafe(void) {
    char exe[1024], root[1024], log[1200], out[512];
    char logText[2048];
    int code = 99;
    size_t n;
    FILE *f;
    static const char *const flags[] = { "--allow-unsafe-brains",
                                         "-allow-unsafe-brains" };
    size_t k;

    UT_ASSERT_MSG(childExePath(exe, sizeof(exe)),
                  "no WinBoloBotHost: build it first");
    UT_ASSERT(makeTestDir("unsafe", root, sizeof(root)));
    SDL_snprintf(log, sizeof(log), "%s/child.log", root);
    for (k = 0; k < sizeof(flags) / sizeof(flags[0]); k++) {
        /* As the game's child, and in a test mode: refused before both. */
        const char *a1[] = { "--embed-child", "--server", "127.0.0.1",
                             "--player", "1", "--cachedir", root, flags[k],
                             NULL };
        const char *a2[] = { "--test-sleep-ms", "10", flags[k], NULL };
        UT_ASSERT(runTest(exe, a1, NULL, log, out, sizeof(out), &code));
        UT_ASSERT_MSG(code == BOT_HOST_CHILD_EXIT_ARGS, "%s: exit %d",
                      flags[k], code);
        f = fopen(log, "rb");
        UT_ASSERT(f != NULL);
        n = fread(logText, 1, sizeof(logText) - 1, f);
        fclose(f);
        logText[n] = '\0';
        UT_ASSERT_MSG(strstr(logText, "refuses --allow-unsafe-brains") !=
                          NULL,
                      "log [%s]", logText);
        UT_ASSERT(runTest(exe, a2, NULL, log, out, sizeof(out), &code));
        UT_ASSERT_MSG(code == BOT_HOST_CHILD_EXIT_ARGS, "%s test mode: exit %d",
                      flags[k], code);
        printf("    %s: refused (exit %d)\n", flags[k], code);
    }
    {
        /* The control: the test mode alone runs. */
        const char *a[] = { "--test-sleep-ms", "10", NULL };
        UT_ASSERT(runTest(exe, a, NULL, log, out, sizeof(out), &code));
        UT_ASSERT_MSG(code == 0, "control: exit %d", code);
    }
    removeTestDir(root);
    return 0;
}

static bool fileIs(const char *path, const char *text) {
    char buf[64];
    size_t n;
    FILE *f = fopen(path, "rb");
    if (f == NULL) return false;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strcmp(buf, text) == 0;
}

static SDL_EnumerationResult SDLCALL countTmpOne(void *ud, const char *dir,
                                                 const char *name) {
    size_t n = strlen(name);
    (void)dir;
    if (n > 4 && strcmp(name + n - 4, ".tmp") == 0) (*(int *)ud)++;
    return SDL_ENUM_CONTINUE;
}

/* The .tmp files in a folder (not in its folders). */
static int countTmp(const char *dir) {
    int n = 0;
    (void)SDL_EnumerateDirectory(dir, countTmpOne, &n);
    return n;
}

int run_bothost_child_copy_tree(void) {
    char root[1024], src[1100], sub[1100], dst[1100], path[1200];
    SDL_PathInfo pi;

    UT_ASSERT(makeTestDir("copy", root, sizeof(root)));
    SDL_snprintf(src, sizeof(src), "%s/src", root);
    SDL_snprintf(sub, sizeof(sub), "%s/src/brains", root);
    SDL_snprintf(dst, sizeof(dst), "%s/dst", root);
    UT_ASSERT(SDL_CreateDirectory(sub));
    SDL_snprintf(path, sizeof(path), "%s/a.dll", src);
    UT_ASSERT(writeFile(path, "one"));
    SDL_snprintf(path, sizeof(path), "%s/init.lua", sub);
    UT_ASSERT(writeFile(path, "brain"));

    /* The top files only. */
    UT_ASSERT(botHostChildCopyTree(src, dst, false));
    SDL_snprintf(path, sizeof(path), "%s/a.dll", dst);
    UT_ASSERT(fileIs(path, "one"));
    SDL_snprintf(path, sizeof(path), "%s/brains/init.lua", dst);
    UT_ASSERT(!SDL_GetPathInfo(path, &pi));
    /* With the folders. */
    UT_ASSERT(botHostChildCopyTree(src, dst, true));
    UT_ASSERT(fileIs(path, "brain"));
    /* A changed file (another size) is copied again. */
    SDL_snprintf(path, sizeof(path), "%s/a.dll", src);
    UT_ASSERT(writeFile(path, "three"));
    UT_ASSERT(botHostChildCopyTree(src, dst, false));
    SDL_snprintf(path, sizeof(path), "%s/a.dll", dst);
    UT_ASSERT(fileIs(path, "three"));
    UT_ASSERT(!fileIs(path, "thr3e"));
    /* The same size, other bytes and another time: copied again. The
     * file systems here keep the time to 1 s or better. */
    SDL_Delay(1100);
    SDL_snprintf(path, sizeof(path), "%s/a.dll", src);
    UT_ASSERT(writeFile(path, "thr3e"));
    UT_ASSERT(botHostChildCopyTree(src, dst, true));
    SDL_snprintf(path, sizeof(path), "%s/a.dll", dst);
    UT_ASSERT(fileIs(path, "thr3e"));
    /* A file and a folder taken out of the source leave the copy. */
    SDL_snprintf(path, sizeof(path), "%s/init.lua", sub);
    UT_ASSERT(SDL_RemovePath(path));
    SDL_snprintf(path, sizeof(path), "%s/gone", sub);
    UT_ASSERT(SDL_CreateDirectory(path));
    UT_ASSERT(botHostChildCopyTree(src, dst, true));
    SDL_snprintf(path, sizeof(path), "%s/brains/gone", dst);
    UT_ASSERT(SDL_GetPathInfo(path, &pi));
    SDL_snprintf(path, sizeof(path), "%s/gone", sub);
    UT_ASSERT(SDL_RemovePath(path));
    UT_ASSERT(botHostChildCopyTree(src, dst, true));
    SDL_snprintf(path, sizeof(path), "%s/brains/init.lua", dst);
    UT_ASSERT(!SDL_GetPathInfo(path, &pi));
    SDL_snprintf(path, sizeof(path), "%s/brains/gone", dst);
    UT_ASSERT(!SDL_GetPathInfo(path, &pi));
    UT_ASSERT(countTmp(dst) == 0);
    /* A missing source is refused. */
    SDL_snprintf(path, sizeof(path), "%s/none", root);
    UT_ASSERT(!botHostChildCopyTree(path, dst, true));
    removeTestDir(root);
    return 0;
}

/* Each failure of the child, with the sandbox on, means no bots from this
 * computer; with the sandbox off (Web, Android, the knob) the host runs in
 * the game, except after the restart limit. */
int run_bothost_child_fail_rule(void) {
    static const BotHostChildFail all[] = {
        BOT_HOST_CHILD_FAIL_NO_EXE,   BOT_HOST_CHILD_FAIL_ARGS,
        BOT_HOST_CHILD_FAIL_SANDBOX,  BOT_HOST_CHILD_FAIL_SPAWN,
        BOT_HOST_CHILD_FAIL_RELAY,    BOT_HOST_CHILD_FAIL_NO_HELLO,
        BOT_HOST_CHILD_FAIL_MISMATCH, BOT_HOST_CHILD_FAIL_RESTART_LIMIT,
    };
    size_t i;
    for (i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        UT_ASSERT(botHostChildFailRule(all[i], true) ==
                  BOT_HOST_CHILD_START_NO_BOTS);
        if (all[i] == BOT_HOST_CHILD_FAIL_RESTART_LIMIT) {
            UT_ASSERT(botHostChildFailRule(all[i], false) ==
                      BOT_HOST_CHILD_START_NO_BOTS);
        } else {
            UT_ASSERT(botHostChildFailRule(all[i], false) ==
                      BOT_HOST_CHILD_START_INPROC);
        }
    }
    return 0;
}

/* The copy of the program takes the exe and the DLLs only. */
int run_bothost_child_copy_wanted(void) {
    char root[1024], src[1100], sub[1100], dst[1100], path[1200];
    SDL_PathInfo pi;

    UT_ASSERT(botHostChildCopyWanted("WinBoloBotHost.exe",
                                     "WinBoloBotHost.exe"));
    UT_ASSERT(botHostChildCopyWanted("winbolobothost.EXE",
                                     "WinBoloBotHost.exe"));
    UT_ASSERT(botHostChildCopyWanted("SDL3.dll", "WinBoloBotHost.exe"));
    UT_ASSERT(botHostChildCopyWanted("lua51.DLL", "WinBoloBotHost.exe"));
    UT_ASSERT(!botHostChildCopyWanted("WinBolo.exe", "WinBoloBotHost.exe"));
    UT_ASSERT(!botHostChildCopyWanted("save.wbv", "WinBoloBotHost.exe"));
    UT_ASSERT(!botHostChildCopyWanted("a.dll.txt", "WinBoloBotHost.exe"));
    UT_ASSERT(!botHostChildCopyWanted("dll", "WinBoloBotHost.exe"));

    UT_ASSERT(makeTestDir("copywant", root, sizeof(root)));
    SDL_snprintf(src, sizeof(src), "%s/src", root);
    SDL_snprintf(sub, sizeof(sub), "%s/src/maps", root);
    SDL_snprintf(dst, sizeof(dst), "%s/dst", root);
    UT_ASSERT(SDL_CreateDirectory(sub));
    SDL_snprintf(path, sizeof(path), "%s/host.exe", src);
    UT_ASSERT(writeFile(path, "exe"));
    SDL_snprintf(path, sizeof(path), "%s/a.dll", src);
    UT_ASSERT(writeFile(path, "dll"));
    SDL_snprintf(path, sizeof(path), "%s/other.exe", src);
    UT_ASSERT(writeFile(path, "no"));
    SDL_snprintf(path, sizeof(path), "%s/notes.txt", src);
    UT_ASSERT(writeFile(path, "no"));
    SDL_snprintf(path, sizeof(path), "%s/x.map", sub);
    UT_ASSERT(writeFile(path, "no"));

    UT_ASSERT(botHostChildCopyProgramFiles(src, dst, "host.exe"));
    SDL_snprintf(path, sizeof(path), "%s/host.exe", dst);
    UT_ASSERT(fileIs(path, "exe"));
    SDL_snprintf(path, sizeof(path), "%s/a.dll", dst);
    UT_ASSERT(fileIs(path, "dll"));
    SDL_snprintf(path, sizeof(path), "%s/other.exe", dst);
    UT_ASSERT(!SDL_GetPathInfo(path, &pi));
    SDL_snprintf(path, sizeof(path), "%s/notes.txt", dst);
    UT_ASSERT(!SDL_GetPathInfo(path, &pi));
    SDL_snprintf(path, sizeof(path), "%s/maps", dst);
    UT_ASSERT(!SDL_GetPathInfo(path, &pi));
    removeTestDir(root);
    return 0;
}

static bool hasLine(const char *out, const char *line) {
    return strstr(out, line) != NULL;
}

int run_bothost_child_seccomp(void) {
#ifndef __linux__
    printf("    (the seccomp filter is Linux only: nothing to test here)\n");
    return 0;
#else
    char exe[1024], exeDir[1024], root[1024], log[1200], out[8192];
    const char *a[] = { "--test-seccomp", NULL };
    static const char *const denied[] = {
        "fork denied", "clone denied", "kill parent denied",
        "prlimit parent denied", "setpriority parent denied",
        "setpriority group denied", "affinity parent denied",
        "setpgid parent denied", "pidfd_send_signal denied",
        "fcntl setown denied", "fcntl setsig denied",
        "fcntl setown_ex denied", "ioctl fiosetown denied",
        "ioctl siocspgrp denied",
        "fchmod denied", "fchmodat denied", "fchmodat2 denied",
        "fchown denied", "fchownat denied", "utimensat denied",
        "setxattr denied", "lsetxattr denied", "fsetxattr denied",
        "setxattrat denied", "removexattr denied", "lremovexattr denied",
        "fremovexattr denied", "removexattrat denied", "truncate denied",
        "shmget denied", "shmat denied", "shmctl denied", "msgget denied",
        "msgsnd denied", "msgrcv denied", "msgctl denied", "semget denied",
        "semop denied", "semctl denied", "semtimedop denied",
        "prctl pdeathsig denied",
#if defined(__x86_64__)
        "chmod denied", "chown denied", "lchown denied", "utimes denied",
        "utime denied", "futimesat denied",
#endif
    };
    /* With no filter these must work; prlimit, setpriority, affinity and
     * pidfd_send_signal may still fail for other reasons, so they are not
     * checked there. */
    static const char *const allowed[] = {
        "fork allowed", "clone allowed", "kill parent allowed",
        "setpgid parent allowed", "fcntl setown allowed",
        "fcntl setsig allowed", "fcntl setown_ex allowed",
        "ioctl fiosetown allowed", "ioctl siocspgrp allowed",
        "fchmodat allowed", "fchownat allowed", "utimensat allowed",
        "setxattr allowed", "truncate allowed", "shmget allowed",
        "msgget allowed", "semget allowed", "prctl pdeathsig allowed",
    };
    static const char *const works[] = {
        "threads ok 4", "lua ok", "prlimit self ok", "setpriority self ok",
        "affinity self ok", "fcntl getfl ok", "ioctl fionread ok",
        "ftruncate allowed", "prctl get ok", "seccomp done",
    };
    BotHostChildSandbox sb;
    int code = 99;
    size_t i;
    bool filterOnly = botHostChildLandlockAbi() < 1;

    UT_ASSERT_MSG(childExePath(exe, sizeof(exe)),
                  "no WinBoloBotHost: build it first");
    exeDirOf(exe, exeDir, sizeof(exeDir));
    UT_ASSERT(makeTestDir("seccomp", root, sizeof(root)));
    SDL_snprintf(log, sizeof(log), "%s/child.log", root);
    memset(&sb, 0, sizeof(sb));
    sb.on = true;
    (void)botHostChildSandboxAdd(sb.readDirs, &sb.readDirCount, exeDir);
    (void)botHostChildSandboxAdd(sb.writeDirs, &sb.writeDirCount, root);
    if (filterOnly) {
        printf("    (no Landlock here: the seccomp filter alone)\n");
        botHostChildTestSeccompOnly(true);
    }
    {
        bool ran = runTest(exe, a, &sb, log, out, sizeof(out), &code);
        botHostChildTestSeccompOnly(false);
        UT_ASSERT_MSG(ran, "the child did not end");
    }
    printf("    filter on: exit %d\n%s", code, out);
    UT_ASSERT_MSG(code == 0, "exit %d [%s]", code, out);
    for (i = 0; i < sizeof(works) / sizeof(works[0]); i++) {
        UT_ASSERT_MSG(hasLine(out, works[i]), "no \"%s\" [%s]", works[i],
                      out);
    }
    for (i = 0; i < sizeof(denied) / sizeof(denied[0]); i++) {
        UT_ASSERT_MSG(hasLine(out, denied[i]), "no \"%s\" [%s]",
                      denied[i], out);
    }

    /* The control: no filter, the same calls work. */
    UT_ASSERT(runTest(exe, a, NULL, log, out, sizeof(out), &code));
    printf("    control (no filter): exit %d\n%s", code, out);
    UT_ASSERT_MSG(code == 0, "control: exit %d [%s]", code, out);
    for (i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        UT_ASSERT_MSG(hasLine(out, allowed[i]), "control: no \"%s\" [%s]",
                      allowed[i], out);
    }
    for (i = 0; i < sizeof(works) / sizeof(works[0]); i++) {
        UT_ASSERT_MSG(hasLine(out, works[i]), "control: no \"%s\" [%s]",
                      works[i], out);
    }
    removeTestDir(root);
    return 0;
#endif
}

/* The sandboxed child's environment: the system's names, the locale and
 * WINBOLO_*; nothing else. */
int run_bothost_child_env_keep(void) {
    UT_ASSERT(botHostChildEnvKeep("SystemRoot=C:/Windows"));
    UT_ASSERT(botHostChildEnvKeep("SYSTEMROOT=C:/Windows"));
    UT_ASSERT(botHostChildEnvKeep("windir=C:/Windows"));
    UT_ASSERT(botHostChildEnvKeep("NUMBER_OF_PROCESSORS=12"));
    UT_ASSERT(botHostChildEnvKeep("PROCESSOR_ARCHITECTURE=AMD64"));
    UT_ASSERT(botHostChildEnvKeep("LANG=en_CA.UTF-8"));
    UT_ASSERT(botHostChildEnvKeep("LC_ALL=C"));
    UT_ASSERT(botHostChildEnvKeep("TZ=UTC"));
    UT_ASSERT(botHostChildEnvKeep("WINBOLO_BRAINDBG_LABEL=x"));
    UT_ASSERT(!botHostChildEnvKeep("PATH=C:/Windows"));
    UT_ASSERT(!botHostChildEnvKeep("TEMP=C:/Temp"));
    UT_ASSERT(!botHostChildEnvKeep("TMPDIR=/tmp"));
    UT_ASSERT(!botHostChildEnvKeep("HOME=/home/a"));
    UT_ASSERT(!botHostChildEnvKeep("USERPROFILE=C:/Users/a"));
    UT_ASSERT(!botHostChildEnvKeep("GH_TOKEN=secret"));
    UT_ASSERT(!botHostChildEnvKeep("=C:=C:/"));
    UT_ASSERT(!botHostChildEnvKeep("LANG"));
    UT_ASSERT(!botHostChildEnvKeep("WINBOLO"));
    UT_ASSERT(!botHostChildEnvKeep(""));
    return 0;
}

/* A child that does not read its stdin (--test-sleep-ms) fills the pipe.
 * The game's write must then fail at once, not wait for the child: the
 * client ends such a child as a crash (childWrite). */
int run_bothost_child_stdin_full(void) {
    char exe[1024], root[1024], log[1200], line[560];
    const char *a[] = { "--test-sleep-ms", "30000", NULL };
    BotHostChildProc *p;
    uint32_t start, took;
    int code = 99, lines = 0;
    bool full = false;

    UT_ASSERT_MSG(childExePath(exe, sizeof(exe)),
                  "no WinBoloBotHost: build it first");
    UT_ASSERT(makeTestDir("stdinfull", root, sizeof(root)));
    SDL_snprintf(log, sizeof(log), "%s/child.log", root);
    memset(line, 'x', sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    p = spawnTest(exe, a, NULL, log);
    UT_ASSERT(p != NULL);
    start = (uint32_t)SDL_GetTicks();
    /* 64 KiB of pipe is about 115 lines; 2000 lines is 1.1 MiB. */
    while (lines < 2000) {
        if (!botHostChildWriteLine(p, line)) {
            full = true;
            break;
        }
        lines++;
    }
    took = (uint32_t)SDL_GetTicks() - start;
    printf("    %d lines fit, then the write failed after %u ms\n", lines,
           (unsigned)took);
    UT_ASSERT_MSG(full, "2000 lines went into a pipe no one reads");
    UT_ASSERT_MSG(lines > 0, "the first line did not fit");
    UT_ASSERT_MSG(took < 5000, "the writes took %u ms", (unsigned)took);
    /* Short lines may still fill the last of the pipe's space; then they
     * fail at once too. */
    start = (uint32_t)SDL_GetTicks();
    full = false;
    for (lines = 0; lines < 1000; lines++) {
        if (!botHostChildWriteLine(p, "cpu 1 2")) {
            full = true;
            break;
        }
    }
    UT_ASSERT_MSG(full, "1000 short lines went into a full pipe");
    UT_ASSERT((uint32_t)SDL_GetTicks() - start < 1000);
    UT_ASSERT_MSG(botHostChildAlive(p, &code), "the child ended (%d)", code);
    botHostChildKill(p);
    botHostChildFree(p);
    removeTestDir(root);
    return 0;
}
