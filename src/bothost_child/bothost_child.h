/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The built-in Bot Host as a child process.
 *
 * The game's own Bot Host (bothost_embed.h) can run in the game's process,
 * on a thread, or in a child process: WinBoloBotHost started with
 * --embed-child. The child runs the same code the thread runs (the ticket
 * join, official-only brains, the consent question, the one-game opt-out,
 * the capacity answer, the recording rules) and connects to the server the
 * normal way. A brain crash or a stuck brain then stops only the child, and
 * the operating system limits the child's memory and CPU:
 *
 *   Windows: a job object with KILL_ON_JOB_CLOSE, one process only (no
 *            grandchildren), a memory limit and a CPU rate hard cap; the
 *            child starts at BELOW_NORMAL priority with no console window.
 *   Linux:   RLIMIT_AS, nice +10, PR_SET_PDEATHSIG = SIGKILL.
 *   macOS:   RLIMIT_AS (macOS does not apply it well), nice +10.
 *   All:     the child ends when its stdin pipe from the game closes
 *            (--exit-on-stdin-close), so it never runs on without the game.
 *
 * It is not a file-access sandbox.
 *
 * The game and the child talk in lines of text:
 *   game to child (stdin):  "ticket <hex>", "withhold 0|1", "consent 0|1",
 *                           "quit"
 *   child to game (stdout): "hello <protocol> official=0|1", then
 *                           "status <running> <connected> <refused> <slots>
 *                            <bots> <sameServer> <consentAsking>"
 *                           on each change and once a second.
 * The child's stderr goes to a log file (bothost-child.log in the
 * WinBolo per-user folder).
 *
 * Files:
 *   bothost_child.c         arguments, the line codec, the restart counter
 *   bothost_child_win.c     the process and job object (Windows)
 *   bothost_child_posix.c   the process (Linux and macOS)
 *   bothost_child_client.c  the game's side: start, poll, stop
 *   bothost_child_main.c    the child's side (in WinBoloBotHost)
 */
#ifndef BOTHOST_CHILD_H
#define BOTHOST_CHILD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The line protocol's version, in the child's hello. */
#define BOT_HOST_CHILD_PROTOCOL 1

/* The limits' defaults. 0 turns a limit off. Values to confirm. */
#define BOT_HOST_CHILD_DEFAULT_MEM_MB   4096
#define BOT_HOST_CHILD_DEFAULT_CPU_PCT  75   /* Windows only */

/* A child that ends by itself during a game is started again at most this
 * many times for that game, each after this wait. */
#define BOT_HOST_CHILD_MAX_RESTARTS     3
#define BOT_HOST_CHILD_RESTART_WAIT_MS  2000u

/* The child's exit codes. */
#define BOT_HOST_CHILD_EXIT_OK       0  /* it was told to stop */
#define BOT_HOST_CHILD_EXIT_ARGS     1  /* bad arguments */
#define BOT_HOST_CHILD_EXIT_START    2  /* the host did not start */
#define BOT_HOST_CHILD_EXIT_REFUSED  3  /* the server refused the host */
#define BOT_HOST_CHILD_EXIT_NO_MEM   4  /* --test-alloc-mb: memory refused */

#define BOT_HOST_CHILD_MAX_ARGS 48

/* ---- Arguments ------------------------------------------------------- */

/* What the game's host would use in-process; the child gets the same. */
typedef struct {
    const char *serverHost;
    uint16_t    serverPort;
    uint8_t     player;
    const char *brain;
    const char *name;
    const char *cacheDir;
    int         slots;            /* > 0: a fixed slot count; 0: capacity */
    bool        record;           /* -bothost-record */
    double      recordMinFreeGb;  /* 0: the default */
    bool        officialOnly;     /* the game's WINBOLO_BOTHOST_OFFICIAL_ONLY */
    const char *cpuSpeedFile;     /* the stored CPU speed result; NULL: none */
} BotHostChildParams;

/* Builds the child's argv (argv[0] = exe, NULL at the end) with its strings
 * in buf. Returns argc, or -1 when buf or argv is too small. The ticket is
 * not an argument: it goes on stdin, so other programs do not see it. */
int botHostChildBuildArgs(const BotHostChildParams *p, const char *exe,
                          char *buf, size_t bufSize,
                          const char **argv, int maxArgv);

/* One argument quoted for a Windows command line, the way
 * CommandLineToArgvW and the C runtime read it back. Appended to out at
 * *len. False when out is too small. */
bool botHostChildQuoteWinArg(const char *arg, char *out, size_t outSize,
                             size_t *len);

/* ---- The line codec -------------------------------------------------- */

typedef struct {
    bool running;
    bool connected;
    bool refused;
    int  slots;
    int  bots;
    bool sameServer;
    bool consentAsking;
} BotHostChildStatus;

/* "status ..." without the newline. */
void botHostChildFormatStatus(const BotHostChildStatus *st, char *out,
                              size_t outSize);
bool botHostChildParseStatus(const char *line, BotHostChildStatus *out);
bool botHostChildParseHello(const char *line, int *protocol,
                            bool *officialOnly);

/* 2 * BOT_HOST_TICKET_LEN hex digits. */
void botHostChildTicketHex(const uint8_t *ticket, size_t len, char *out,
                           size_t outSize);
bool botHostChildTicketParse(const char *hex, uint8_t *ticket, size_t len);

/* ---- The restart counter --------------------------------------------- */

typedef struct {
    int      restarts;    /* restarts this game */
    bool     waiting;     /* a restart waits for waitUntilMs */
    uint32_t waitUntilMs;
} BotHostChildRestart;

/* A new game. */
void botHostChildRestartInit(BotHostChildRestart *r);
/* The child ended by itself at nowMs: true when it may start again (after
 * the wait), false when the game used all its restarts. */
bool botHostChildRestartAfterExit(BotHostChildRestart *r, uint32_t nowMs);
/* True once, when the wait is over: start the child again now. */
bool botHostChildRestartDue(BotHostChildRestart *r, uint32_t nowMs);

/* ---- The process (bothost_child_win.c / bothost_child_posix.c) -------- */

typedef struct {
    int memMb;   /* 0: no memory limit */
    int cpuPct;  /* 0: no CPU cap; Windows only */
} BotHostChildLimits;

typedef struct BotHostChildProc BotHostChildProc;

/* Starts exe with argv in the limits, stdin and stdout as pipes to this
 * program, stderr into logPath (NULL: discarded). NULL on failure, with a
 * reason in err. */
BotHostChildProc *botHostChildSpawn(const char *exe, const char *const *argv,
                                    const BotHostChildLimits *lim,
                                    const char *logPath, char *err,
                                    size_t errSize);
/* Writes one line (a newline is added). False when the pipe is closed. */
bool botHostChildWriteLine(BotHostChildProc *p, const char *line);
/* Closes the child's stdin: the child stops (--exit-on-stdin-close). */
void botHostChildCloseStdin(BotHostChildProc *p);
/* Reads what the child wrote without waiting. Returns the bytes read, 0
 * for none now, -1 at the end (the pipe closed). */
int botHostChildRead(BotHostChildProc *p, char *buf, size_t size);
/* True while the child runs; when it ended, *exitCode is its code (-1:
 * it was killed or crashed). */
bool botHostChildAlive(BotHostChildProc *p, int *exitCode);
/* Waits up to waitMs for the child to end. True when it ended. */
bool botHostChildWait(BotHostChildProc *p, uint32_t waitMs, int *exitCode);
/* Ends the child at once (the job, or SIGKILL to this child only). */
void botHostChildKill(BotHostChildProc *p);
/* Frees p; kills the child first if it still runs. */
void botHostChildFree(BotHostChildProc *p);
/* The child's process id. */
unsigned long botHostChildPid(const BotHostChildProc *p);

/* For the tests: close the job only, as the game's exit would (Windows;
 * elsewhere it does nothing and returns false). */
bool botHostChildCloseJobForTest(BotHostChildProc *p);
/* For the tests: the job's limits as Windows reports them. False when
 * there is no job (or not Windows). */
bool botHostChildQueryJobForTest(BotHostChildProc *p, int *memMb,
                                 int *cpuPct, int *activeProcesses,
                                 bool *killOnClose);

/* ---- The game's side (bothost_child_client.c) ------------------------ */

/* The WinBoloBotHost program next to this one. False when there is none. */
bool botHostChildFindExe(char *out, size_t outSize);
/* Where the child's stderr goes. */
void botHostChildLogPath(char *out, size_t outSize);

/* Starts the child for one join. False when it did not start (no program,
 * the spawn failed); the caller then runs the host in-process. */
bool botHostChildClientStart(const BotHostChildParams *p,
                             const uint8_t *ticket, size_t ticketLen,
                             const BotHostChildLimits *lim, bool withhold);
/* Once a frame: reads the child's lines, sees whether it still runs. */
void botHostChildClientPoll(void);
/* tellServer: "quit", stdin closed, a short wait, then the kill; false:
 * the kill at once (the server times the host out). */
void botHostChildClientStop(bool tellServer);
/* A child was started and not stopped (it may have ended by itself). */
bool botHostChildClientActive(void);
/* The last status the child sent; running is false once it ended. */
void botHostChildClientStatus(BotHostChildStatus *out);
void botHostChildClientSetWithhold(bool on);
void botHostChildClientConsent(bool yes);

/* How the child ended (valid when botHostChildClientActive and the status
 * says it does not run). */
typedef enum {
    BOT_HOST_CHILD_RUNS = 0,
    BOT_HOST_CHILD_ENDED_REFUSED,   /* the server refused it */
    BOT_HOST_CHILD_ENDED_EARLY,     /* it ended before its hello: a start
                                     * failure (old program, bad args) */
    BOT_HOST_CHILD_ENDED_MISMATCH,  /* its official-only rule differs */
    BOT_HOST_CHILD_ENDED_CRASH      /* it ended by itself after its hello */
} BotHostChildEnd;
BotHostChildEnd botHostChildClientEnd(void);
/* The running child said its hello. */
bool botHostChildClientHello(void);

/* ---- The child's side (bothost_child_main.c, in WinBoloBotHost) ------ */

/* True when argv asks for a child mode (--embed-child, or a --test-* mode). */
bool botHostChildWanted(int argc, char **argv);
int botHostChildMain(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* BOTHOST_CHILD_H */
