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
 *
 * In the file sandbox (the default) it also gives the child the machine
 * id, the salts and the CPU counters (which the child may not read), keeps
 * the salts the child answers, and runs the relay for its datagrams. When
 * the sandbox cannot be set up, or a sandboxed child fails in any other
 * way, no bots run for that game: the host does not run in this program
 * in its place, because that would run the brains with no OS sandbox at
 * all (botHostChildFailRule). The sandbox's folders (the grants and, on
 * Windows, the copy of the program) are made ready on a thread, so the
 * game's frame does not wait for them.
 */
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "bothost_child.h"
#include "machine_cpu_free.h"
#include "machine_id.h"
#include "../common/wb_log.h"

#define CHILD_EVENT(...)                                                   \
    do {                                                                   \
        fprintf(stderr, __VA_ARGS__);                                      \
        fputc('\n', stderr);                                               \
        WB_LOG_INFO(WB_LOG_CAT_NET, __VA_ARGS__);                          \
    } while (0)

/* How long a stop waits for the child to end by itself. The main thread
 * waits for it (the game's frame stops that long, at most): the child
 * tells the server it leaves, and a kill would make the server wait for
 * its time-out instead. */
#define CHILD_STOP_WAIT_MS 2000u
/* Logs kept: the log, then .1 to .3 (the older runs). */
#define CHILD_LOGS_KEPT 3
/* The CPU counters go to a sandboxed child this often. */
#define CHILD_CPU_EVERY_MS 1000u

static BotHostChildProc  *childProc;
static BotHostChildStatus childStatus;
static bool               childHello;
static bool               childEnded;
static int                childExitCode;
static BotHostChildEnd    childEnd;
static bool               childOfficialWanted;
static bool               childTableWanted;
static bool               childWithhold;
static char               childLine[512];
static size_t             childLineLen;
static bool               childSandboxed;
static BotHostRelay      *childRelay;
static uint32_t           childCpuSentMs;
static uint32_t           childStartMs;
static int                childSaltLines;
static bool               childLastSandboxed;

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
    const char *env = SDL_getenv("WINBOLO_BOTHOST_CHILD_LOG");
    char *pref;
    if (env != NULL && env[0] != '\0') {
        SDL_strlcpy(out, env, outSize);
        return;
    }
    pref = SDL_GetPrefPath("WinBolo", "WinBolo");
    if (pref != NULL) {
        SDL_snprintf(out, outSize, "%sbothost-child.log", pref);
        SDL_free(pref);
    } else {
        SDL_strlcpy(out, "bothost-child.log", outSize);
    }
}

/* The log, then .1 to .3: each start moves them one on. */
static void childRotateLogs(const char *log) {
    char from[1100], to[1100];
    int i;
    SDL_snprintf(to, sizeof(to), "%s.%d", log, CHILD_LOGS_KEPT);
    (void)SDL_RemovePath(to);
    for (i = CHILD_LOGS_KEPT - 1; i >= 0; i--) {
        if (i == 0) {
            SDL_strlcpy(from, log, sizeof(from));
        } else {
            SDL_snprintf(from, sizeof(from), "%s.%d", log, i);
        }
        SDL_snprintf(to, sizeof(to), "%s.%d", log, i + 1);
        (void)SDL_RenamePath(from, to);
    }
}

/* The folder of a file's path, with no separator at the end. */
static void childDirOf(const char *path, char *out, size_t outSize) {
    size_t n;
    SDL_strlcpy(out, path, outSize);
    n = strlen(out);
    while (n > 0 && out[n - 1] != '/' && out[n - 1] != '\\') n--;
    while (n > 1 && (out[n - 1] == '/' || out[n - 1] == '\\')) n--;
    out[n] = '\0';
}

#ifdef _WIN32
/* Where the copy of the program goes when its own folder cannot be
 * granted (Program Files, Steam): one folder per install, in the user's
 * LocalAppData. WINBOLO_BOTHOST_RUN_DIR names another (the tests). */
static bool childRunDir(char *out, size_t outSize) {
    const char *env = SDL_getenv("WINBOLO_BOTHOST_RUN_DIR");
    const char *local = SDL_getenv("LOCALAPPDATA");
    char name[96];
    if (env != NULL && env[0] != '\0') {
        SDL_strlcpy(out, env, outSize);
        return true;
    }
    if (local == NULL || local[0] == '\0') return false;
    botHostChildContainerName(name, sizeof(name));
    /* "WinBolo.BotHost.<hash>": the hash names the install. */
    SDL_snprintf(out, outSize, "%s\\WinBolo\\BotHostRun-%s", local,
                 strrchr(name, '.') != NULL ? strrchr(name, '.') + 1 : name);
    return true;
}

/* Copies the program, its DLLs and its brains to runDir; the new exe's
 * path in exeOut. */
static bool childCopyProgram(const char *exeDir, const char *exe,
                             const char *runDir, char *exeOut,
                             size_t exeOutSize) {
    static const char *const brainDirs[] = { "Brains", "brains" };
    char src[1100], dst[1100];
    const char *name = exe + strlen(exeDir);
    size_t i;
    while (*name == '/' || *name == '\\') name++;
    if (!SDL_CreateDirectory(runDir)) return false;
    /* Only what the child needs from the top folder: its exe and the
     * DLLs; then the brains. Not the game's data (maps, sounds, skins). */
    if (!botHostChildCopyProgramFiles(exeDir, runDir, name)) return false;
    for (i = 0; i < sizeof(brainDirs) / sizeof(brainDirs[0]); i++) {
        SDL_PathInfo pi;
        SDL_snprintf(src, sizeof(src), "%s\\%s", exeDir, brainDirs[i]);
        if (!SDL_GetPathInfo(src, &pi) || pi.type != SDL_PATHTYPE_DIRECTORY) {
            continue;
        }
        SDL_snprintf(dst, sizeof(dst), "%s\\%s", runDir, brainDirs[i]);
        if (!botHostChildCopyTree(src, dst, true)) return false;
        break;
    }
    SDL_snprintf(exeOut, exeOutSize, "%s\\%s", runDir, name);
    return true;
}
#endif

static void childReset(void) {
    memset(&childStatus, 0, sizeof(childStatus));
    childHello = false;
    childEnded = false;
    childExitCode = 0;
    childEnd = BOT_HOST_CHILD_RUNS;
    childLineLen = 0;
    childSandboxed = false;
    childCpuSentMs = 0;
    childStartMs = 0;
    childSaltLines = 0;
}

/* The machine id and the salts, before the ticket. */
static void childSendMachine(void) {
    static char lines[BOT_HOST_CHILD_MACHINE_LINES][BOT_HOST_CHILD_LINE_MAX];
    int n = botHostChildMachineLines(lines, BOT_HOST_CHILD_MACHINE_LINES);
    int i;
    if (n == 0) {
        CHILD_EVENT("[bothost] no machine id to give the Bot Host child");
    }
    for (i = 0; i < n; i++) {
        (void)botHostChildWriteLine(childProc, lines[i]);
    }
    SDL_memset(lines, 0, sizeof(lines));
}

static void childSendCpu(void) {
    uint64_t idle, total;
    char line[80];
    if (!machineCpuFreeCounters(&idle, &total)) return;
    SDL_snprintf(line, sizeof(line), "cpu %llu %llu",
                 (unsigned long long)idle, (unsigned long long)total);
    (void)botHostChildWriteLine(childProc, line);
}

/* The sandbox's paths for the child at exe: its folder and the env's
 * install folder read, the CPU speed file read, the cache read and
 * write. The strings live in store. */
typedef struct {
    char exeDir[1100];
    char install[1100];
    char cache[1100];
    char speed[1100];
} ChildSandboxPaths;

static void childSandboxPaths(const BotHostChildParams *p, const char *exe,
                              ChildSandboxPaths *store,
                              BotHostChildSandbox *sb) {
    const char *inst = SDL_getenv("WINBOLO_BOTHOST_INSTALL_DIR");
    SDL_memset(sb, 0, sizeof(*sb));
    sb->on = true;
    childDirOf(exe, store->exeDir, sizeof(store->exeDir));
    (void)botHostChildSandboxAdd(sb->readDirs, &sb->readDirCount,
                                 store->exeDir);
#ifdef __APPLE__
    {
        /* The bundle's Contents (Frameworks, Resources) next to MacOS. */
        static char contents[1100];
        childDirOf(store->exeDir, contents, sizeof(contents));
        (void)botHostChildSandboxAdd(sb->readDirs, &sb->readDirCount,
                                     contents);
    }
#endif
    if (inst != NULL && inst[0] != '\0' && SDL_strcasecmp(inst, "none") != 0) {
        SDL_strlcpy(store->install, inst, sizeof(store->install));
        (void)botHostChildSandboxAdd(sb->readDirs, &sb->readDirCount,
                                     store->install);
    }
    SDL_strlcpy(store->cache, p->cacheDir, sizeof(store->cache));
    (void)botHostChildSandboxAdd(sb->writeDirs, &sb->writeDirCount,
                                 store->cache);
    if (p->cpuSpeedFile != NULL && p->cpuSpeedFile[0] != '\0') {
        SDL_strlcpy(store->speed, p->cpuSpeedFile, sizeof(store->speed));
        (void)botHostChildSandboxAdd(sb->readFiles, &sb->readFileCount,
                                     store->speed);
    }
}

/* After a failure: one line, why, and what the game does
 * (botHostChildFailRule). In the sandbox that is always no bots. */
static BotHostChildStart childFail(BotHostChildFail f, bool sandbox,
                                   const char *why) {
    BotHostChildStart how = botHostChildFailRule(f, sandbox);
    if (how == BOT_HOST_CHILD_START_NO_BOTS) {
        CHILD_EVENT("[bothost] the Bot Host child cannot run in its sandbox "
                    "(%s): no bots from this computer in this game", why);
    } else {
        CHILD_EVENT("[bothost] the Bot Host child cannot run (%s): the Bot "
                    "Host runs in this program (no sandbox)", why);
    }
    return how;
}

/* The sandbox's folders made ready on a thread: the grants (the first
 * grant on a folder goes through all of it) and, on Windows, the copy of
 * the program when its folder keeps its ACL. Done once for each set of
 * paths in a run; a later game with the same paths uses the result at
 * once. */
typedef struct {
    SDL_Thread       *thread;
    SDL_AtomicInt     done;
    bool              have;       /* a job ran or runs for these inputs */
    /* The inputs. */
    char              exe[1024];
    char              cacheDir[1100];
    char              speed[1100];
    char              install[1100];
    /* The results, valid once done. */
    bool              ok;
    char              why[512];
    char              note[1200]; /* a line for the log, or empty */
    char              runExe[1024];
    ChildSandboxPaths paths;
} ChildPrep;

static ChildPrep childPrep;

/* The sandbox of job: from its inputs (fromPaths false: the paths are
 * filled in), or from the paths the job left (the exe folder may be the
 * copy's). */
static void childPrepSandbox(const ChildPrep *job, BotHostChildSandbox *sb,
                             ChildSandboxPaths *paths, bool fromPaths) {
    BotHostChildParams q;
    if (!fromPaths) {
        memset(&q, 0, sizeof(q));
        q.cacheDir = job->cacheDir;
        q.cpuSpeedFile = job->speed[0] != '\0' ? job->speed : NULL;
        childSandboxPaths(&q, job->exe, paths, sb);
        return;
    }
    SDL_memset(sb, 0, sizeof(*sb));
    sb->on = true;
    (void)botHostChildSandboxAdd(sb->readDirs, &sb->readDirCount,
                                 paths->exeDir);
#ifdef __APPLE__
    {
        static char contents[1100];
        childDirOf(paths->exeDir, contents, sizeof(contents));
        (void)botHostChildSandboxAdd(sb->readDirs, &sb->readDirCount,
                                     contents);
    }
#endif
    if (paths->install[0] != '\0') {
        (void)botHostChildSandboxAdd(sb->readDirs, &sb->readDirCount,
                                     paths->install);
    }
    (void)botHostChildSandboxAdd(sb->writeDirs, &sb->writeDirCount,
                                 paths->cache);
    if (paths->speed[0] != '\0') {
        (void)botHostChildSandboxAdd(sb->readFiles, &sb->readFileCount,
                                     paths->speed);
    }
}

static int SDLCALL childPrepThread(void *arg) {
    ChildPrep *job = (ChildPrep *)arg;
    BotHostChildSandbox sb;
    unsigned readFailed = 0;

    job->ok = false;
    job->why[0] = '\0';
    job->note[0] = '\0';
    SDL_strlcpy(job->runExe, job->exe, sizeof(job->runExe));
    SDL_memset(&job->paths, 0, sizeof(job->paths));
    /* The cache must be there before its grant. */
    (void)SDL_CreateDirectory(job->cacheDir);
    childPrepSandbox(job, &sb, &job->paths, false);
    if (!botHostChildSandboxPrepare(&sb, &readFailed, job->why,
                                    sizeof(job->why))) {
        SDL_SetAtomicInt(&job->done, 1);
        return 0;
    }
#ifdef _WIN32
    if (readFailed & 1u) {
        /* The program's folder keeps its ACL (Program Files, Steam): run a
         * copy that the sandbox may read. */
        char runDir[1024], copyExe[1024];
        unsigned again = 0;
        if (!childRunDir(runDir, sizeof(runDir)) ||
            !childCopyProgram(job->paths.exeDir, job->exe, runDir, copyExe,
                              sizeof(copyExe))) {
            SDL_strlcpy(job->why, "the program could not be copied to a "
                                  "folder the sandbox may read",
                        sizeof(job->why));
            SDL_SetAtomicInt(&job->done, 1);
            return 0;
        }
        SDL_snprintf(job->note, sizeof(job->note),
                     "[bothost] the sandbox cannot read %s: the Bot Host "
                     "child runs from a copy in %s (only files that changed "
                     "are copied again)", job->paths.exeDir, runDir);
        SDL_strlcpy(job->runExe, copyExe, sizeof(job->runExe));
        SDL_strlcpy(job->paths.exeDir, runDir, sizeof(job->paths.exeDir));
        childPrepSandbox(job, &sb, &job->paths, true);
        if (!botHostChildSandboxPrepare(&sb, &again, job->why,
                                        sizeof(job->why))) {
            SDL_SetAtomicInt(&job->done, 1);
            return 0;
        }
        readFailed = again;
    }
#endif
    if (readFailed != 0) {
        SDL_strlcpy(job->why, "a folder the child reads cannot be granted",
                    sizeof(job->why));
    } else {
        job->ok = true;
    }
    SDL_SetAtomicInt(&job->done, 1);
    return 0;
}

/* START_WAIT while the job runs; STARTED (here: ready) when it is done
 * and ok; NO_BOTS, with its line, when it failed. */
static BotHostChildStart childPrepReady(const BotHostChildParams *p,
                                        const char *exe) {
    const char *inst = SDL_getenv("WINBOLO_BOTHOST_INSTALL_DIR");
    char install[1100], speed[1100];
    install[0] = '\0';
    if (inst != NULL && inst[0] != '\0' && SDL_strcasecmp(inst, "none") != 0) {
        SDL_strlcpy(install, inst, sizeof(install));
    }
    speed[0] = '\0';
    if (p->cpuSpeedFile != NULL) {
        SDL_strlcpy(speed, p->cpuSpeedFile, sizeof(speed));
    }
    if (childPrep.have &&
        (strcmp(childPrep.exe, exe) != 0 ||
         strcmp(childPrep.cacheDir, p->cacheDir) != 0 ||
         strcmp(childPrep.speed, speed) != 0 ||
         strcmp(childPrep.install, install) != 0)) {
        /* Other paths: a new job, once the old one ended. */
        if (!SDL_GetAtomicInt(&childPrep.done)) {
            return BOT_HOST_CHILD_START_WAIT;
        }
        if (childPrep.thread != NULL) SDL_WaitThread(childPrep.thread, NULL);
        childPrep.thread = NULL;
        childPrep.have = false;
    }
    if (!childPrep.have) {
        SDL_strlcpy(childPrep.exe, exe, sizeof(childPrep.exe));
        SDL_strlcpy(childPrep.cacheDir, p->cacheDir,
                    sizeof(childPrep.cacheDir));
        SDL_strlcpy(childPrep.speed, speed, sizeof(childPrep.speed));
        SDL_strlcpy(childPrep.install, install, sizeof(childPrep.install));
        SDL_SetAtomicInt(&childPrep.done, 0);
        childPrep.have = true;
        childPrep.thread = SDL_CreateThread(childPrepThread,
                                            "bothost-prepare", &childPrep);
        if (childPrep.thread == NULL) {
            /* No thread: the work is done here, once. */
            (void)childPrepThread(&childPrep);
        }
    }
    if (!SDL_GetAtomicInt(&childPrep.done)) return BOT_HOST_CHILD_START_WAIT;
    if (childPrep.thread != NULL) {
        SDL_WaitThread(childPrep.thread, NULL);
        childPrep.thread = NULL;
    }
    if (childPrep.note[0] != '\0') {
        CHILD_EVENT("%s", childPrep.note);
        childPrep.note[0] = '\0';
    }
    if (!childPrep.ok) {
        BotHostChildStart how = childFail(BOT_HOST_CHILD_FAIL_SANDBOX, true,
                                          childPrep.why);
        /* The next game tries again. */
        childPrep.have = false;
        return how;
    }
    return BOT_HOST_CHILD_STARTED;
}

BotHostChildStart botHostChildClientStart(const BotHostChildParams *p,
                                          const uint8_t *ticket,
                                          size_t ticketLen,
                                          const BotHostChildLimits *lim,
                                          bool withhold, bool sandbox) {
    char exe[1024], log[1024], err[256], why[512];
    char argBuf[4096], hex[128], line[160];
    const char *argv[BOT_HOST_CHILD_MAX_ARGS];
    BotHostChildSandbox sb;

    if (childProc != NULL) botHostChildClientStop(true);
    childReset();
    /* While the prepare thread copies, the game asks again each frame:
     * no stat of the exe and no container profile call until it ends. */
    if (sandbox && childPrep.have && !SDL_GetAtomicInt(&childPrep.done)) {
        return BOT_HOST_CHILD_START_WAIT;
    }
    if (!botHostChildFindExe(exe, sizeof(exe))) {
        return childFail(BOT_HOST_CHILD_FAIL_NO_EXE, sandbox,
                         "no WinBoloBotHost next to this program");
    }
    SDL_memset(&sb, 0, sizeof(sb));
    if (sandbox) {
        BotHostChildStart ready;
        why[0] = '\0';
        if (!botHostChildSandboxAvailable(why, sizeof(why))) {
            return childFail(BOT_HOST_CHILD_FAIL_SANDBOX, true, why);
        }
        ready = childPrepReady(p, exe);
        if (ready != BOT_HOST_CHILD_STARTED) return ready;
        SDL_strlcpy(exe, childPrep.runExe, sizeof(exe));
        childPrepSandbox(&childPrep, &sb, &childPrep.paths, true);
    }
    if (botHostChildBuildArgs(p, exe, argBuf, sizeof(argBuf), argv,
                              BOT_HOST_CHILD_MAX_ARGS) < 0) {
        return childFail(BOT_HOST_CHILD_FAIL_ARGS, sandbox,
                         "the arguments are too long");
    }
    botHostChildLogPath(log, sizeof(log));
    childRotateLogs(log);

    /* The spawn's sandboxFailed is not needed: in the sandbox a failure
     * at any step means no bots. */
    err[0] = '\0';
    childProc = botHostChildSpawn(exe, argv, lim, sandbox ? &sb : NULL, log,
                                  err, sizeof(err), NULL);
    if (childProc == NULL) {
        return childFail(BOT_HOST_CHILD_FAIL_SPAWN, sandbox,
                         err[0] != '\0' ? err : "the start failed");
    }
    childSandboxed = sandbox;
    childLastSandboxed = sandbox;
    childStartMs = (uint32_t)SDL_GetTicks();
    if (sandbox) {
        childRelay = botHostRelayStart(childProc, p->serverHost,
                                       p->serverPort);
        if (childRelay == NULL) {
            botHostChildKill(childProc);
            botHostChildFree(childProc);
            childProc = NULL;
            childReset();
            return childFail(BOT_HOST_CHILD_FAIL_RELAY, true,
                             "the relay to the server did not start");
        }
        childSendMachine();
        childSendCpu();
        childCpuSentMs = (uint32_t)SDL_GetTicks();
    }
    childOfficialWanted = p->officialOnly;
    childTableWanted = p->shippedTable;
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
                "%d MiB, CPU cap %d%%%s, file sandbox %s, log %s",
                botHostChildPid(childProc), lim != NULL ? lim->memMb : 0,
                lim != NULL ? lim->cpuPct : 0, cpuNote,
                sandbox ? "on" : "off", log);
    return BOT_HOST_CHILD_STARTED;
}

static void childTakeLine(const char *line) {
    BotHostChildStatus st;
    int proto;
    bool official, fullMap, table;
    if (strncmp(line, "salt ", 5) == 0) {
        /* The child answered a salt: this program keeps the salt file. */
        uint8_t salt[MACHINE_SALT_LEN];
        /* At most BOT_HOST_CHILD_MAX_SALT_LINES for one child: a child
         * that sends more cannot fill the salt file. */
        if (childSandboxed &&
            childSaltLines < BOT_HOST_CHILD_MAX_SALT_LINES &&
            botHostChildTicketParse(line + 5, salt, sizeof(salt))) {
            childSaltLines++;
            machineIdNoteSalt(salt);
        }
        return;
    }
    if (botHostChildParseHello(line, &proto, &official, &fullMap, &table)) {
        childHello = true;
        if (!botHostChildHelloMatches(proto, official, fullMap, table,
                                      childOfficialWanted,
                                      childTableWanted)) {
            CHILD_EVENT("[bothost] the Bot Host child does not match this "
                        "program (protocol %d, official-only %d, full map "
                        "%d, brain table %d): it stops", proto,
                        (int)official, (int)fullMap, (int)table);
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
    if (childSandboxed &&
        (uint32_t)SDL_GetTicks() - childCpuSentMs >= CHILD_CPU_EVERY_MS) {
        childCpuSentMs = (uint32_t)SDL_GetTicks();
        childSendCpu();
    }
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
    if (!childHello && childEnd == BOT_HOST_CHILD_RUNS &&
        (uint32_t)SDL_GetTicks() - childStartMs >=
            BOT_HOST_CHILD_HELLO_WAIT_MS) {
        /* No hello in time: it is ended, and counts as a start failure
         * (in the sandbox: no bots for this game). */
        CHILD_EVENT("[bothost] the Bot Host child said no hello in %u ms: "
                    "it is ended", (unsigned)BOT_HOST_CHILD_HELLO_WAIT_MS);
        childEnd = childSandboxed ? BOT_HOST_CHILD_ENDED_SANDBOX
                                  : BOT_HOST_CHILD_ENDED_EARLY;
        botHostChildKill(childProc);
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
            } else if (childSandboxed &&
                       (childExitCode == BOT_HOST_CHILD_EXIT_SANDBOX ||
                        !childHello)) {
                /* No in-process host in its place: that would have no
                 * OS sandbox. */
                childEnd = BOT_HOST_CHILD_ENDED_SANDBOX;
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
                    : childEnd == BOT_HOST_CHILD_ENDED_SANDBOX
                        ? "in its sandbox, before it said hello: no bots "
                          "from this computer in this game"
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
    if (childRelay != NULL) {
        unsigned long toServer, fromServer, refused;
        /* The child is gone, so the relay's reader ends. */
        botHostChildKill(childProc);
        botHostRelayCounts(childRelay, &toServer, &fromServer, &refused);
        botHostRelayStop(childRelay);
        childRelay = NULL;
        CHILD_EVENT("[bothost] the relay passed %lu datagrams to the server "
                    "and %lu from it, and refused %lu", toServer, fromServer,
                    refused);
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

bool botHostChildClientSandboxed(void) {
    return childLastSandboxed;
}
