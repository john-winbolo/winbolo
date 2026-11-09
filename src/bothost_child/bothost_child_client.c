/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The built-in Bot Host as a child process: the game's side
 * (bothost_child.h). One child at a time, on the main thread.
 *
 * It starts WinBoloBotHost --embed-child with the settings the in-process
 * host would use, sends it the ticket and the player's answers, reads its
 * status lines, and stops it: "quit" and a closed stdin first, the job (or
 * SIGKILL to this child only) last.
 */
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "bothost_child.h"
#include "../common/wb_log.h"

#define CHILD_EVENT(...)                                                   \
    do {                                                                   \
        fprintf(stderr, __VA_ARGS__);                                      \
        fputc('\n', stderr);                                               \
        WB_LOG_INFO(WB_LOG_CAT_NET, __VA_ARGS__);                          \
    } while (0)

/* How long a stop waits for the child to end by itself. */
#define CHILD_STOP_WAIT_MS 2000u

static BotHostChildProc  *childProc;
static BotHostChildStatus childStatus;
static bool               childHello;
static bool               childEnded;
static int                childExitCode;
static BotHostChildEnd    childEnd;
static bool               childOfficialWanted;
static bool               childWithhold;
static char               childLine[512];
static size_t             childLineLen;

bool botHostChildFindExe(char *out, size_t outSize) {
#ifdef _WIN32
    static const char name[] = "WinBoloBotHost.exe";
#else
    static const char name[] = "WinBoloBotHost";
#endif
    static const char *const subdirs[] = {
        "",
#ifdef __APPLE__
        /* In an app bundle SDL_GetBasePath is Contents/Resources. */
        "../MacOS/",
#endif
    };
    const char *base = SDL_GetBasePath();
    size_t i;
    if (base == NULL || out == NULL || outSize == 0) return false;
    for (i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); i++) {
        SDL_PathInfo pi;
        SDL_snprintf(out, outSize, "%s%s%s", base, subdirs[i], name);
        if (SDL_GetPathInfo(out, &pi) && pi.type == SDL_PATHTYPE_FILE) {
            return true;
        }
    }
    out[0] = '\0';
    return false;
}

void botHostChildLogPath(char *out, size_t outSize) {
    char *pref = SDL_GetPrefPath("WinBolo", "WinBolo");
    if (pref != NULL) {
        SDL_snprintf(out, outSize, "%sbothost-child.log", pref);
        SDL_free(pref);
    } else {
        SDL_strlcpy(out, "bothost-child.log", outSize);
    }
}

static void childReset(void) {
    memset(&childStatus, 0, sizeof(childStatus));
    childHello = false;
    childEnded = false;
    childExitCode = 0;
    childEnd = BOT_HOST_CHILD_RUNS;
    childLineLen = 0;
}

bool botHostChildClientStart(const BotHostChildParams *p,
                             const uint8_t *ticket, size_t ticketLen,
                             const BotHostChildLimits *lim, bool withhold) {
    char exe[1024], log[1024], oldLog[1100], err[256];
    char argBuf[4096], hex[128], line[160];
    const char *argv[BOT_HOST_CHILD_MAX_ARGS];

    if (childProc != NULL) botHostChildClientStop(true);
    childReset();
    if (!botHostChildFindExe(exe, sizeof(exe))) {
        CHILD_EVENT("[bothost] no WinBoloBotHost next to this program: the "
                    "Bot Host runs in this program");
        return false;
    }
    if (botHostChildBuildArgs(p, exe, argBuf, sizeof(argBuf), argv,
                              BOT_HOST_CHILD_MAX_ARGS) < 0) {
        CHILD_EVENT("[bothost] the Bot Host child's arguments are too long: "
                    "the Bot Host runs in this program");
        return false;
    }
    botHostChildLogPath(log, sizeof(log));
    /* Keep the last run's log once. */
    SDL_snprintf(oldLog, sizeof(oldLog), "%s.1", log);
    (void)SDL_RemovePath(oldLog);
    (void)SDL_RenamePath(log, oldLog);

    childProc = botHostChildSpawn(exe, argv, lim, log, err, sizeof(err));
    if (childProc == NULL) {
        CHILD_EVENT("[bothost] the Bot Host child did not start (%s): the "
                    "Bot Host runs in this program", err);
        return false;
    }
    childOfficialWanted = p->officialOnly;
    childWithhold = withhold;
    /* Until its first line: it runs, with no slots yet. */
    childStatus.running = true;
    botHostChildTicketHex(ticket, ticketLen, hex, sizeof(hex));
    SDL_snprintf(line, sizeof(line), "ticket %s", hex);
    (void)botHostChildWriteLine(childProc, line);
    (void)botHostChildWriteLine(childProc, withhold ? "withhold 1"
                                                    : "withhold 0");
#ifdef _WIN32
    const char *cpuNote = "";
#else
    const char *cpuNote = " (not applied here: nice +10 only)";
#endif
    CHILD_EVENT("[bothost] Bot Host child started (pid %lu): memory limit "
                "%d MiB, CPU cap %d%%%s, log %s",
                botHostChildPid(childProc), lim != NULL ? lim->memMb : 0,
                lim != NULL ? lim->cpuPct : 0, cpuNote, log);
    return true;
}

static void childTakeLine(const char *line) {
    BotHostChildStatus st;
    int proto;
    bool official;
    if (botHostChildParseHello(line, &proto, &official)) {
        childHello = true;
        if (proto != BOT_HOST_CHILD_PROTOCOL ||
            official != childOfficialWanted) {
            CHILD_EVENT("[bothost] the Bot Host child does not match this "
                        "program (protocol %d, official-only %d): it stops",
                        proto, (int)official);
            childEnd = BOT_HOST_CHILD_ENDED_MISMATCH;
            botHostChildKill(childProc);
        }
        return;
    }
    if (botHostChildParseStatus(line, &st)) {
        childStatus = st;
        if (!st.running && st.refused) {
            childEnd = BOT_HOST_CHILD_ENDED_REFUSED;
        }
    }
}

void botHostChildClientPoll(void) {
    char buf[1024];
    int n, guard = 0;
    if (childProc == NULL || childEnded) return;
    while (guard++ < 16 &&
           (n = botHostChildRead(childProc, buf, sizeof(buf))) > 0) {
        int i;
        for (i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') {
                childLine[childLineLen] = '\0';
                if (childLineLen > 0 && childLine[childLineLen - 1] == '\r') {
                    childLine[childLineLen - 1] = '\0';
                }
                childTakeLine(childLine);
                childLineLen = 0;
            } else if (childLineLen + 1 < sizeof(childLine)) {
                childLine[childLineLen++] = c;
            }
        }
    }
    if (!botHostChildAlive(childProc, &childExitCode)) {
        childEnded = true;
        childStatus.running = false;
        childStatus.connected = false;
        childStatus.bots = 0;
        childStatus.consentAsking = false;
        if (childEnd == BOT_HOST_CHILD_RUNS) {
            if (childExitCode == BOT_HOST_CHILD_EXIT_REFUSED) {
                childEnd = BOT_HOST_CHILD_ENDED_REFUSED;
            } else if (!childHello) {
                childEnd = BOT_HOST_CHILD_ENDED_EARLY;
            } else {
                childEnd = BOT_HOST_CHILD_ENDED_CRASH;
            }
        }
        if (childEnd == BOT_HOST_CHILD_ENDED_REFUSED) {
            childStatus.refused = true;
        }
        CHILD_EVENT("[bothost] Bot Host child ended (exit code %d, %s)",
                    childExitCode,
                    childEnd == BOT_HOST_CHILD_ENDED_REFUSED ? "the server "
                                                               "refused it"
                    : childEnd == BOT_HOST_CHILD_ENDED_EARLY ? "before it "
                                                               "said hello"
                    : childEnd == BOT_HOST_CHILD_ENDED_MISMATCH
                        ? "it did not match"
                        : "by itself");
    }
}

void botHostChildClientStop(bool tellServer) {
    int code = 0;
    if (childProc == NULL) return;
    if (!childEnded) {
        if (tellServer) {
            (void)botHostChildWriteLine(childProc, "quit");
            botHostChildCloseStdin(childProc);
            if (botHostChildWait(childProc, CHILD_STOP_WAIT_MS, &code)) {
                CHILD_EVENT("[bothost] Bot Host child stopped (exit code %d)",
                            code);
            } else {
                CHILD_EVENT("[bothost] Bot Host child did not stop in %u ms: "
                            "it is ended", (unsigned)CHILD_STOP_WAIT_MS);
                botHostChildKill(childProc);
            }
        } else {
            botHostChildKill(childProc);
            CHILD_EVENT("[bothost] Bot Host child ended without a word");
        }
    }
    botHostChildFree(childProc);
    childProc = NULL;
    childReset();
}

bool botHostChildClientActive(void) {
    return childProc != NULL;
}

void botHostChildClientStatus(BotHostChildStatus *out) {
    if (out == NULL) return;
    *out = childStatus;
    if (childProc == NULL || childEnded) out->running = false;
}

void botHostChildClientSetWithhold(bool on) {
    if (childProc == NULL || childEnded || on == childWithhold) return;
    childWithhold = on;
    (void)botHostChildWriteLine(childProc, on ? "withhold 1" : "withhold 0");
}

void botHostChildClientConsent(bool yes) {
    if (childProc == NULL || childEnded) return;
    (void)botHostChildWriteLine(childProc, yes ? "consent 1" : "consent 0");
    /* The question is answered: no prompt until the child asks again. */
    childStatus.consentAsking = false;
}

BotHostChildEnd botHostChildClientEnd(void) {
    if (childProc == NULL || !childEnded) return BOT_HOST_CHILD_RUNS;
    return childEnd;
}

bool botHostChildClientHello(void) {
    return childProc != NULL && childHello;
}
