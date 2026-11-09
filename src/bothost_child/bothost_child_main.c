/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The built-in Bot Host as a child process: the child's side, in
 * WinBoloBotHost (bothost_child.h).
 *
 *   WinBoloBotHost --embed-child --server ADDR --port N --player N
 *                  --brain NAME --name NAME --cachedir DIR --slots N
 *                  [--official-only] [--record] [--record-min-free-gb N]
 *                  [--cpu-speed-file PATH] [--exit-on-stdin-close]
 *
 * It runs the game's own host code (bothost_embed.c, the embed copy of
 * bothost_main.c) on its thread, as the game would, and talks with the
 * game in lines on stdin and stdout. The ticket comes on stdin, never as
 * an argument. The game never starts it with a console, so what it writes
 * on stderr goes to the log file the game gives it.
 *
 * Two test modes for the unit tests, with no host at all:
 *   --test-alloc-mb N   take N MiB of memory in pieces, writing each into
 *                       use; "alloc ok" (exit 0) or "alloc failed at M"
 *                       (exit BOT_HOST_CHILD_EXIT_NO_MEM)
 *   --test-sleep-ms N   wait N ms, then exit 0
 */
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL3/SDL.h>

#include "bothost_child.h"
#include "bothost_embed.h"
#include "bolo_rand.h"
#include "control_event.h"   /* BOT_HOST_TICKET_LEN */
#include "gui_message.h"
#include "machine_id.h"
#include "platform_net.h"
#include "server_sim.h"      /* serverSimBotPoolInit */
#include "cpu_speed_capacity.h" /* cpuSpeedApplyStoredToBrains */
#include "../gui/lang.h"
#include "../gui/clientmutex.h"
#include "../common/wb_log.h"

/* ---- Options --------------------------------------------------------- */

static bool   coEmbed;
static bool   coExitOnStdinClose;
static char   coServer[256];
static int    coPort;
static int    coPlayer = -1;
static char   coBrain[BOT_HOST_BRAIN_NAME_LEN];
static char   coName[64];
static char   coCacheDir[1024];
static int    coSlots;
static bool   coOfficialOnly;
static bool   coRecord;
static double coRecordMinFreeGb;
static char   coCpuSpeedFile[1024];
static long   coTestAllocMb = -1;
static long   coTestSleepMs = -1;

bool botHostChildWanted(int argc, char **argv) {
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--embed-child") == 0 ||
            strcmp(argv[i], "--test-alloc-mb") == 0 ||
            strcmp(argv[i], "--test-sleep-ms") == 0) {
            return true;
        }
    }
    return false;
}

static bool parseChildArgs(int argc, char **argv) {
    int i;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(a, "--embed-child") == 0) {
            coEmbed = true;
            continue;
        }
        if (strcmp(a, "--exit-on-stdin-close") == 0) {
            coExitOnStdinClose = true;
            continue;
        }
        if (strcmp(a, "--official-only") == 0) {
            coOfficialOnly = true;
            continue;
        }
        if (strcmp(a, "--record") == 0) {
            coRecord = true;
            continue;
        }
        if (v == NULL) {
            fprintf(stderr, "[bothost] %s needs a value\n", a);
            return false;
        }
        if (strcmp(a, "--server") == 0) {
            SDL_strlcpy(coServer, v, sizeof(coServer));
        } else if (strcmp(a, "--port") == 0) {
            coPort = atoi(v);
        } else if (strcmp(a, "--player") == 0) {
            coPlayer = atoi(v);
        } else if (strcmp(a, "--brain") == 0) {
            SDL_strlcpy(coBrain, v, sizeof(coBrain));
        } else if (strcmp(a, "--name") == 0) {
            SDL_strlcpy(coName, v, sizeof(coName));
        } else if (strcmp(a, "--cachedir") == 0) {
            SDL_strlcpy(coCacheDir, v, sizeof(coCacheDir));
        } else if (strcmp(a, "--slots") == 0) {
            coSlots = atoi(v);
        } else if (strcmp(a, "--record-min-free-gb") == 0) {
            coRecordMinFreeGb = atof(v);
        } else if (strcmp(a, "--cpu-speed-file") == 0) {
            SDL_strlcpy(coCpuSpeedFile, v, sizeof(coCpuSpeedFile));
        } else if (strcmp(a, "--test-alloc-mb") == 0) {
            coTestAllocMb = atol(v);
        } else if (strcmp(a, "--test-sleep-ms") == 0) {
            coTestSleepMs = atol(v);
        } else {
            fprintf(stderr, "[bothost] unknown child option %s\n", a);
            return false;
        }
        i++;
    }
    return true;
}

/* ---- Test modes ------------------------------------------------------ */

static int testAlloc(long mb) {
    const size_t piece = (size_t)16 * 1024 * 1024;
    long done = 0;
    /* Kept, never freed: the point is to hold the memory. */
    while (done < mb) {
        char *p = (char *)malloc(piece);
        if (p == NULL) {
            printf("alloc failed at %ld\n", done);
            fflush(stdout);
            return BOT_HOST_CHILD_EXIT_NO_MEM;
        }
        memset(p, 1, piece);
        done += 16;
    }
    printf("alloc ok %ld\n", done);
    fflush(stdout);
    return BOT_HOST_CHILD_EXIT_OK;
}

/* ---- stdin ----------------------------------------------------------- */

/* The game's lines, read on a thread of their own: a read of a pipe waits,
 * and the host's loop must not. */
#define CHILD_QUEUE 32
static SDL_Mutex     *inLock;
static char           inLines[CHILD_QUEUE][160];
static int            inHead, inCount;
static SDL_AtomicInt  inClosed;

static int SDLCALL stdinThreadFn(void *arg) {
    char line[160];
    (void)arg;
    while (fgets(line, sizeof(line), stdin) != NULL) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        SDL_LockMutex(inLock);
        if (inCount < CHILD_QUEUE) {
            SDL_strlcpy(inLines[(inHead + inCount) % CHILD_QUEUE], line,
                        sizeof(inLines[0]));
            inCount++;
        }
        SDL_UnlockMutex(inLock);
    }
    SDL_SetAtomicInt(&inClosed, 1);
    return 0;
}

static bool takeLine(char *out, size_t outSize) {
    bool got = false;
    SDL_LockMutex(inLock);
    if (inCount > 0) {
        SDL_strlcpy(out, inLines[inHead], outSize);
        inHead = (inHead + 1) % CHILD_QUEUE;
        inCount--;
        got = true;
    }
    SDL_UnlockMutex(inLock);
    return got;
}

/* ---- stdout ---------------------------------------------------------- */

static void sendStatus(const BotHostEmbedStatus *e) {
    BotHostChildStatus st;
    char line[160];
    memset(&st, 0, sizeof(st));
    st.running = e->running;
    st.connected = e->connected;
    st.refused = e->refused;
    st.slots = e->slots;
    st.bots = e->bots;
    st.sameServer = e->sameServer;
    st.consentAsking = e->consentAsking;
    botHostChildFormatStatus(&st, line, sizeof(line));
    printf("%s\n", line);
    fflush(stdout);
}

static bool statusSame(const BotHostEmbedStatus *a,
                       const BotHostEmbedStatus *b) {
    return a->running == b->running && a->connected == b->connected &&
           a->refused == b->refused && a->slots == b->slots &&
           a->bots == b->bots && a->sameServer == b->sameServer &&
           a->consentAsking == b->consentAsking;
}

static void childMessageHandler(const char *message, const char *title) {
    fprintf(stderr, "[bothost] [%s] %s\n", title ? title : "WinBolo",
            message ? message : "");
}

/* ---- Main ------------------------------------------------------------ */

int botHostChildMain(int argc, char **argv) {
    uint8_t ticket[BOT_HOST_TICKET_LEN];
    bool haveTicket = false, quit = false;
    BotHostEmbedConfig cfg;
    BotHostEmbedStatus last, now;
    SDL_Thread *reader;
    uint32_t lastSentMs, waitStartMs;
    char line[160];
    int exitCode = BOT_HOST_CHILD_EXIT_OK;

#ifdef _WIN32
    /* A crash ends the child; no Windows dialog keeps it alive. */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    if (!parseChildArgs(argc, argv)) return BOT_HOST_CHILD_EXIT_ARGS;
    if (coTestAllocMb >= 0) return testAlloc(coTestAllocMb);
    if (coTestSleepMs >= 0) {
        SDL_Delay((uint32_t)coTestSleepMs);
        return BOT_HOST_CHILD_EXIT_OK;
    }
    if (!coEmbed || coServer[0] == '\0' || coPlayer < 0 || coPlayer > 255 ||
        coCacheDir[0] == '\0') {
        fprintf(stderr, "[bothost] the child needs --embed-child, --server, "
                        "--player and --cachedir\n");
        return BOT_HOST_CHILD_EXIT_ARGS;
    }

    /* As the game sets up before its host starts. No crash reporter: it
     * would start a second program, which the job does not allow. */
    bolo_srand((uint64_t)time(NULL) ^ (uint64_t)getpid());
    (void)machineIdInit();
    if (!SDL_Init(0)) {
        fprintf(stderr, "[bothost] SDL_Init failed: %s\n", SDL_GetError());
        return BOT_HOST_CHILD_EXIT_START;
    }
    wb_log_init("WinBolo", "WinBolo", "winbolo-bothost-child.log");
    if (coCpuSpeedFile[0] != '\0') cpuSpeedStoreSetPath(coCpuSpeedFile);
    (void)cpuSpeedApplyStoredToBrains(WINBOLO_VERSION);
    if (!serverSimBotPoolInit(0)) {
        fprintf(stderr, "[bothost] cannot start the brain threads\n");
        return BOT_HOST_CHILD_EXIT_START;
    }
    if (!clientMutexCreate()) {
        fprintf(stderr, "[bothost] cannot make the client mutex\n");
        return BOT_HOST_CHILD_EXIT_START;
    }
    guiMessageSetHandler(childMessageHandler);
    langSetup();
    if (bolo_net_init() != 0) {
        fprintf(stderr, "[bothost] network start failed\n");
        return BOT_HOST_CHILD_EXIT_START;
    }

    inLock = SDL_CreateMutex();
    SDL_SetAtomicInt(&inClosed, 0);
    reader = SDL_CreateThread(stdinThreadFn, "bothost-stdin", NULL);
    if (inLock == NULL || reader == NULL) {
        fprintf(stderr, "[bothost] cannot read stdin\n");
        return BOT_HOST_CHILD_EXIT_START;
    }
    /* It may wait in a read for ever; the process ends without it. */
    SDL_DetachThread(reader);

    /* The game's first lines: the ticket, then the opt-out. */
    waitStartMs = (uint32_t)SDL_GetTicks();
    while (!haveTicket) {
        while (takeLine(line, sizeof(line))) {
            if (strncmp(line, "ticket ", 7) == 0) {
                haveTicket = botHostChildTicketParse(line + 7, ticket,
                                                     sizeof(ticket));
            } else if (strcmp(line, "withhold 1") == 0) {
                botHostInprocSetWithhold(true);
            } else if (strcmp(line, "withhold 0") == 0) {
                botHostInprocSetWithhold(false);
            } else if (strcmp(line, "quit") == 0) {
                return BOT_HOST_CHILD_EXIT_OK;
            }
            if (haveTicket) break;
        }
        if (haveTicket) break;
        if (SDL_GetAtomicInt(&inClosed) ||
            (uint32_t)SDL_GetTicks() - waitStartMs > 10000u) {
            fprintf(stderr, "[bothost] no ticket from the game\n");
            return BOT_HOST_CHILD_EXIT_ARGS;
        }
        SDL_Delay(5);
    }

    botHostInprocSetOfficialOnly(coOfficialOnly);
    botHostInprocSetRecord(coRecord);
    memset(&cfg, 0, sizeof(cfg));
    cfg.serverHost = coServer;
    cfg.serverPort = (uint16_t)coPort;
    cfg.player = (uint8_t)coPlayer;
    cfg.ticket = ticket;
    cfg.brain = coBrain;
    cfg.name = coName;
    cfg.cacheDir = coCacheDir;
    cfg.slots = coSlots;
    cfg.recordMinFreeGb = coRecordMinFreeGb;
    if (!botHostInprocStart(&cfg)) {
        fprintf(stderr, "[bothost] the Bot Host did not start\n");
        return BOT_HOST_CHILD_EXIT_START;
    }
    printf("hello %d official=%d\n", BOT_HOST_CHILD_PROTOCOL,
           botHostEmbedOfficialOnly() ? 1 : 0);
    fflush(stdout);
    fprintf(stderr, "[bothost] Bot Host child for player %d at %s:%d "
                    "(official-only %d, record %d)\n",
            coPlayer, coServer, coPort, (int)coOfficialOnly, (int)coRecord);

    memset(&last, 0, sizeof(last));
    botHostInprocGetStatus(&last);
    sendStatus(&last);
    lastSentMs = (uint32_t)SDL_GetTicks();
    while (!quit) {
        while (takeLine(line, sizeof(line))) {
            if (strcmp(line, "quit") == 0) {
                quit = true;
            } else if (strcmp(line, "withhold 1") == 0) {
                botHostInprocSetWithhold(true);
            } else if (strcmp(line, "withhold 0") == 0) {
                botHostInprocSetWithhold(false);
            } else if (strcmp(line, "consent 1") == 0) {
                botHostInprocConsentAnswer(true);
            } else if (strcmp(line, "consent 0") == 0) {
                botHostInprocConsentAnswer(false);
            }
        }
        if (!quit && coExitOnStdinClose && SDL_GetAtomicInt(&inClosed)) {
            fprintf(stderr, "[bothost] the game closed the pipe: the child "
                            "stops\n");
            quit = true;
        }
        botHostInprocGetStatus(&now);
        if (!statusSame(&now, &last) ||
            (uint32_t)SDL_GetTicks() - lastSentMs >= 1000u) {
            sendStatus(&now);
            last = now;
            lastSentMs = (uint32_t)SDL_GetTicks();
        }
        if (!now.running) {
            /* The host's thread ends by itself only when the server
             * refused it. */
            exitCode = BOT_HOST_CHILD_EXIT_REFUSED;
            break;
        }
        SDL_Delay(10);
    }

    botHostInprocStop(true);
    fprintf(stderr, "[bothost] Bot Host child ends (exit code %d)\n",
            exitCode);
    fflush(stderr);
    clientMutexDestroy();
    langCleanup();
    serverSimBotPoolDestroy();
    wb_log_shutdown();
    /* The stdin thread may still wait in its read: end at once. */
    fflush(stdout);
    _Exit(exitCode);
}
