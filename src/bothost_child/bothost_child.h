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
 * The file sandbox (on by default; BotHostChildSandbox). Two layers: the
 * Lua sandbox inside the OS sandbox. The Lua sandbox is always on in the
 * child. A brain that got out of it can still read only the program's
 * folder (with the install's brains) and the system's libraries, and
 * write only the Bot Host's cache folder (the received brains, the
 * recordings and the print2 logs). It has no network at all: the game
 * sends the child's datagrams for it (the relay below), to the game's
 * server only.
 *   Windows: an AppContainer with no capabilities, inside the job object.
 *            The container's SID is granted read or write on the folders
 *            with ACLs. One container profile per install, used again.
 *   Linux:   Landlock (read on the needed folders, read and write on the
 *            cache folder), PR_SET_NO_NEW_PRIVS and a seccomp filter that
 *            refuses socket(), io_uring, ptrace and signals to other
 *            programs, all set between the fork and the exec.
 *   macOS:   sandbox_init_with_parameters with a profile of the same
 *            folders and no network, at the start of the child's main.
 * When the sandbox cannot be set up, the game runs no bots for that game:
 * no brain runs in a child without its sandbox. The same holds for every
 * other failure of a sandboxed child (no WinBoloBotHost, a failed start
 * at any step, a hello that does not match, no hello in time, the restart
 * limit): this computer then takes no bots, and the host never runs in
 * the game's own process in its place (botHostChildFailRule). Only the
 * user's switches (the config's "Run As Child Process" = No or "Child
 * File Sandbox" = No, --bothost-inproc, --bothost-no-sandbox) and the
 * builds with no child (Web, Android) run brains with no OS sandbox.
 *
 * The game and the child talk in lines of text:
 *   game to child (stdin):  "ticket <hex>", "withhold 0|1", "consent 0|1",
 *                           "quit"; in the sandbox also, before the
 *                           ticket, "machineid <hex>" and "salts <hex>..."
 *                           (the game's machine id and the salts it
 *                           answered), and "cpu <idle> <total>" once a
 *                           second
 *   child to game (stdout): "hello <protocol> official=0|1 fullmap=0|1",
 *                           then "status <running> <connected> <refused>
 *                           <slots> <bots> <sameServer> <consentAsking>"
 *                           on each change and once a second; in the
 *                           sandbox also "salt <hex>" for each salt the
 *                           child answered.
 * The child's stderr goes to a log file (bothost-child.log in the WinBolo
 * per-user folder, or WINBOLO_BOTHOST_CHILD_LOG; the three runs before it
 * are .1, .2 and .3).
 *
 * The relay (in the sandbox): two more pipes carry binary frames,
 *   [type u8][socket u8][body length u16, low byte first][body]
 * OPEN and CLOSE have no body; SEND (child to game) and RECV (game to
 * child) have the address (IPv4 4 bytes and port 2 bytes, both in network
 * order) and then the datagram. The game opens one real UDP socket for
 * each of the child's sockets, so the server sees one address for each,
 * as before. It sends only to the server it knows and passes on only what
 * comes from that server.
 *
 * Files:
 *   bothost_child.c         arguments, the line and relay codecs, the
 *                           restart counter
 *   bothost_child_win.c     the process, the job object and the
 *                           AppContainer (Windows)
 *   bothost_child_posix.c   the process, Landlock and seccomp (Linux and
 *                           macOS)
 *   bothost_child_relay.c   the game's side of the relay
 *   bothost_child_client.c  the game's side: start, poll, stop
 *   bothost_child_main.c    the child's side (in WinBoloBotHost), with its
 *                           side of the relay and the macOS sandbox
 *
 * Desktop only (Windows, Linux, macOS). Web and Android have no built-in
 * Bot Host (bothost_embed_stub.c) and compile none of this.
 */
#ifndef BOTHOST_CHILD_H
#define BOTHOST_CHILD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The line protocol's version, in the child's hello. 2: fullmap= in the
 * hello, the sandbox lines and the relay. */
#define BOT_HOST_CHILD_PROTOCOL 2

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
#define BOT_HOST_CHILD_EXIT_SANDBOX  5  /* it is not in its sandbox */
#define BOT_HOST_CHILD_EXIT_DENIED   6  /* a --test-* access was refused */

#define BOT_HOST_CHILD_MAX_ARGS 64

/* A child that has not said hello this long after its start is ended
 * (in the sandbox: no bots for this game). */
#define BOT_HOST_CHILD_HELLO_WAIT_MS 15000u
/* The "salt" lines one child may send; more are not kept (a child
 * answers one salt for each round of the server's same-computer check). */
#define BOT_HOST_CHILD_MAX_SALT_LINES 16

/* The longest line either way, with its newline. */
#define BOT_HOST_CHILD_LINE_MAX 200

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
/* "hello <protocol> official=0|1 fullmap=0|1". */
void botHostChildFormatHello(int protocol, bool officialOnly, bool fullMap,
                             char *out, size_t outSize);
bool botHostChildParseHello(const char *line, int *protocol,
                            bool *officialOnly, bool *fullMap);
/* This program's WINBOLO_BOTHOST_ALLOW_FULL_MAP. */
bool botHostChildBuildFullMap(void);

/* 2 * len hex digits. */
void botHostChildTicketHex(const uint8_t *ticket, size_t len, char *out,
                           size_t outSize);
bool botHostChildTicketParse(const char *hex, uint8_t *ticket, size_t len);

/* The lines that give a sandboxed child this computer's machine id and
 * the salts it answered: "machineid <hex>", then "salts <hex>..." with up
 * to BOT_HOST_CHILD_SALTS_PER_LINE salts each. Returns the line count, or
 * 0 when this computer has no machine id. */
#define BOT_HOST_CHILD_SALTS_PER_LINE 4
#define BOT_HOST_CHILD_MACHINE_LINES  20
int botHostChildMachineLines(char lines[][BOT_HOST_CHILD_LINE_MAX],
                             int maxLines);

/* ---- The relay codec ------------------------------------------------- */

#define BOT_HOST_RELAY_OPEN   1
#define BOT_HOST_RELAY_CLOSE  2
#define BOT_HOST_RELAY_SEND   3   /* child to game */
#define BOT_HOST_RELAY_RECV   4   /* game to child */
#define BOT_HOST_RELAY_HDR    4
#define BOT_HOST_RELAY_ADDR   6
/* The child's sockets: one control socket and one for each bot. */
#define BOT_HOST_RELAY_MAX_SOCKS 64
#define BOT_HOST_RELAY_MAX_DATAGRAM 8192
#define BOT_HOST_RELAY_MAX_FRAME \
    (BOT_HOST_RELAY_HDR + BOT_HOST_RELAY_ADDR + BOT_HOST_RELAY_MAX_DATAGRAM)

/* One frame into out. ip and port are in network order. Returns its size,
 * or 0 when the values are not valid or out is too small. */
size_t botHostRelayEncode(uint8_t type, uint8_t sock, uint32_t ipNet,
                          uint16_t portNet, const uint8_t *data, size_t len,
                          uint8_t *out, size_t outSize);
/* A frame's header: false when the type, the socket or the length is not
 * valid for that type. */
bool botHostRelayParseHeader(const uint8_t *hdr, uint8_t *type,
                             uint8_t *sock, uint16_t *bodyLen);
/* A SEND or RECV body: the address and the datagram in it. */
bool botHostRelayParseBody(const uint8_t *body, uint16_t bodyLen,
                           uint32_t *ipNet, uint16_t *portNet,
                           const uint8_t **data, size_t *len);

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

#define BOT_HOST_SANDBOX_MAX_PATHS 8

/* The file sandbox (see the top). Full paths. */
typedef struct {
    bool        on;
    const char *readDirs[BOT_HOST_SANDBOX_MAX_PATHS];
    int         readDirCount;
    const char *readFiles[BOT_HOST_SANDBOX_MAX_PATHS];
    int         readFileCount;
    const char *writeDirs[BOT_HOST_SANDBOX_MAX_PATHS];
    int         writeDirCount;
} BotHostChildSandbox;

/* Adds a path to a list of sb; false when the list is full. */
bool botHostChildSandboxAdd(const char **list, int *count, const char *path);

/* True when this computer can run the child in its sandbox; else why not
 * in why. */
bool botHostChildSandboxAvailable(char *why, size_t whySize);
/* The Landlock ABI of this kernel (Linux): 0 when there is none, -1 on
 * other systems. */
int botHostChildLandlockAbi(void);

/* Windows: makes the container profile and grants its SID read on each
 * readDir and readFile and full access on each writeDir. A readDir whose
 * ACL cannot change is reported in *readFailed (bit i for readDirs[i]);
 * the caller can copy what it needs elsewhere and grant that. Elsewhere
 * it does nothing and returns true. False (with why) when the container
 * or a write folder failed. */
bool botHostChildSandboxPrepare(const BotHostChildSandbox *sb,
                                unsigned *readFailed, char *why,
                                size_t whySize);
/* Windows: the container profile's name for this install. */
void botHostChildContainerName(char *out, size_t outSize);
/* Copies the files of src into dst (only files that are new, of another
 * size or newer), into folders too when recurse. False when a file could
 * not be copied. */
bool botHostChildCopyTree(const char *src, const char *dst, bool recurse);
/* The same for the top folder of the program only: exeName and the DLLs
 * (botHostChildCopyWanted), no other file and no folder. */
bool botHostChildCopyProgramFiles(const char *src, const char *dst,
                                  const char *exeName);
/* True for a file of the program's folder that the child needs: exeName
 * itself or a .dll (any case). */
bool botHostChildCopyWanted(const char *name, const char *exeName);

/* True for an environment entry ("NAME=value") that a sandboxed child
 * keeps: the system's own names (SystemRoot, windir, the processor's),
 * the locale (LANG, LC_*, TZ) and WINBOLO_*. Every other name is left
 * out, so no token in the game's environment reaches the child. PATH,
 * TEMP, TMP and TMPDIR are not kept: the spawn sets its own. */
bool botHostChildEnvKeep(const char *entry);

/* Starts exe with argv in the limits, stdin and stdout as pipes to this
 * program, stderr into logPath (NULL: discarded). With sb->on also in the
 * file sandbox and with the relay pipes; the child gets --sandbox, the
 * paths and --relay as more arguments. NULL on failure, with a reason in
 * err; *sandboxFailed (may be NULL) is true when the sandbox was the
 * reason. */
BotHostChildProc *botHostChildSpawn(const char *exe, const char *const *argv,
                                    const BotHostChildLimits *lim,
                                    const BotHostChildSandbox *sb,
                                    const char *logPath, char *err,
                                    size_t errSize, bool *sandboxFailed);
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
/* The relay pipes (the game's ends), for bothost_child_relay.c. Both
 * wait; false at the end of the pipe or on an error. */
bool botHostChildHasRelay(const BotHostChildProc *p);
bool botHostChildRelayRead(BotHostChildProc *p, void *buf, size_t n);
bool botHostChildRelayWrite(BotHostChildProc *p, const void *buf, size_t n);

/* For the tests: close the job only, as the game's exit would (Windows;
 * elsewhere it does nothing and returns false). */
bool botHostChildCloseJobForTest(BotHostChildProc *p);
/* For the tests: the job's limits as Windows reports them. False when
 * there is no job (or not Windows). */
bool botHostChildQueryJobForTest(BotHostChildProc *p, int *memMb,
                                 int *cpuPct, int *activeProcesses,
                                 bool *killOnClose);

/* ---- The game's side of the relay (bothost_child_relay.c) ------------ */

typedef struct BotHostRelay BotHostRelay;
/* Starts the relay threads for the child p, to the server at
 * serverHost:serverPort (dotted IPv4) only. NULL when p has no relay or
 * the threads did not start. */
BotHostRelay *botHostRelayStart(BotHostChildProc *p, const char *serverHost,
                                uint16_t serverPort);
/* Once the child ended (or was killed): waits for the threads and closes
 * the sockets. Before botHostChildFree. */
void botHostRelayStop(BotHostRelay *r);
/* Datagrams passed each way, and the ones refused (to or from another
 * address). */
void botHostRelayCounts(BotHostRelay *r, unsigned long *toServer,
                        unsigned long *fromServer, unsigned long *refused);

/* ---- The game's side (bothost_child_client.c) ------------------------ */

/* The WinBoloBotHost program next to this one. False when there is none. */
bool botHostChildFindExe(char *out, size_t outSize);
/* Where the child's stderr goes. */
void botHostChildLogPath(char *out, size_t outSize);

typedef enum {
    BOT_HOST_CHILD_STARTED = 0,
    /* With no sandbox (the user's switch) only: no program, or the spawn
     * failed: the caller runs the host here. */
    BOT_HOST_CHILD_START_INPROC,
    /* In the sandbox, any failure: no bots on this computer for this
     * game. */
    BOT_HOST_CHILD_START_NO_BOTS,
    /* The sandbox's folders are still made ready (the grants, and on
     * Windows the copy of the program) on a thread: ask again later. */
    BOT_HOST_CHILD_START_WAIT
} BotHostChildStart;

/* The ways a child can fail. */
typedef enum {
    BOT_HOST_CHILD_FAIL_NO_EXE = 0,   /* no WinBoloBotHost next to the game */
    BOT_HOST_CHILD_FAIL_ARGS,         /* the arguments did not fit */
    BOT_HOST_CHILD_FAIL_SANDBOX,      /* the sandbox could not be set up */
    BOT_HOST_CHILD_FAIL_SPAWN,        /* the start failed, at any step */
    BOT_HOST_CHILD_FAIL_RELAY,        /* the relay did not start */
    BOT_HOST_CHILD_FAIL_NO_HELLO,     /* no hello in time, or it ended first */
    BOT_HOST_CHILD_FAIL_MISMATCH,     /* its hello does not match the game */
    BOT_HOST_CHILD_FAIL_RESTART_LIMIT /* it ended by itself too many times */
} BotHostChildFail;

/* What the game does after a failure of its child, in the sandbox or not:
 * in the sandbox always BOT_HOST_CHILD_START_NO_BOTS; with no sandbox
 * BOT_HOST_CHILD_START_INPROC, but NO_BOTS after the restart limit. */
BotHostChildStart botHostChildFailRule(BotHostChildFail f, bool sandbox);

/* Starts the child for one join; sandbox: in the file sandbox. */
BotHostChildStart botHostChildClientStart(const BotHostChildParams *p,
                                          const uint8_t *ticket,
                                          size_t ticketLen,
                                          const BotHostChildLimits *lim,
                                          bool withhold, bool sandbox);
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
    BOT_HOST_CHILD_ENDED_CRASH,     /* it ended by itself after its hello */
    BOT_HOST_CHILD_ENDED_SANDBOX    /* a sandboxed child ended before its
                                     * hello, or could not enter its
                                     * sandbox: no bots for this game */
} BotHostChildEnd;
BotHostChildEnd botHostChildClientEnd(void);
/* The running child said its hello. */
bool botHostChildClientHello(void);
/* The last child started was in the file sandbox. */
bool botHostChildClientSandboxed(void);

/* ---- The child's side (bothost_child_main.c, in WinBoloBotHost) ------ */

/* True when argv asks for a child mode (--embed-child, or a --test-* mode). */
bool botHostChildWanted(int argc, char **argv);
int botHostChildMain(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* BOTHOST_CHILD_H */
