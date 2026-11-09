/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The built-in Bot Host's child process on Linux and macOS
 * (bothost_child.h).
 *
 * fork and exec, so the child can set its own limits before the exec:
 *   - RLIMIT_AS: the memory limit (address space). Note: macOS does not
 *     apply RLIMIT_AS well (it may not stop a large allocation), so on
 *     macOS the limit is a request, not a promise;
 *   - nice +10: the game and other programs come first for the CPU. There
 *     is no CPU rate cap here, only the lower priority;
 *   - Linux: PR_SET_PDEATHSIG = SIGKILL, so the child dies with the game;
 *   - its own process group, so a Ctrl+C in the game's terminal does not
 *     reach it (the game stops it itself);
 *   - every other file descriptor closed: the child gets only its stdin,
 *     stdout and log file.
 * On every system the child also ends when its stdin closes
 * (--exit-on-stdin-close), which is what ends it on macOS when the game
 * ends without a word.
 *
 * Between the fork and the exec the child calls only async-signal-safe
 * functions: the game has other threads.
 */
#ifndef _WIN32

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "bothost_child.h"

struct BotHostChildProc {
    pid_t pid;
    int   stdinW;   /* -1 when closed */
    int   stdoutR;  /* -1 when closed */
    bool  reaped;
    int   exitCode;
};

static void setErr(char *err, size_t errSize, const char *what) {
    if (err != NULL && errSize > 0) {
        snprintf(err, errSize, "%s (%s)", what, strerror(errno));
    }
}

static void setCloexec(int fd) {
    int f = fcntl(fd, F_GETFD);
    if (f >= 0) (void)fcntl(fd, F_SETFD, f | FD_CLOEXEC);
}

/* The child, after the fork. Async-signal-safe calls only. */
static void childExec(const char *exe, char *const *argv, int inR, int outW,
                      int logFd, rlim_t memBytes, pid_t parent) {
    long maxFd, fd;
    struct sigaction dfl;

#ifdef __linux__
    /* Dies with the game; the game may already be gone. */
    (void)prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() != parent) _exit(127);
#else
    (void)parent;
#endif
    (void)setpgid(0, 0);

    if (dup2(inR, 0) < 0 || dup2(outW, 1) < 0 || dup2(logFd, 2) < 0) {
        _exit(127);
    }
    maxFd = sysconf(_SC_OPEN_MAX);
    if (maxFd < 0 || maxFd > 65536) maxFd = 65536;
    for (fd = 3; fd < maxFd; fd++) (void)close((int)fd);

    /* The game may ignore SIGPIPE; the child starts with the default. */
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    (void)sigaction(SIGPIPE, &dfl, NULL);

    if (memBytes > 0) {
        /* Note: macOS does not apply RLIMIT_AS well. */
        struct rlimit rl;
        rl.rlim_cur = memBytes;
        rl.rlim_max = memBytes;
        (void)setrlimit(RLIMIT_AS, &rl);
    }
    (void)setpriority(PRIO_PROCESS, 0, 10);

    execv(exe, argv);
    _exit(127);
}

BotHostChildProc *botHostChildSpawn(const char *exe, const char *const *argv,
                                    const BotHostChildLimits *lim,
                                    const char *logPath, char *err,
                                    size_t errSize) {
    int inPipe[2] = { -1, -1 }, outPipe[2] = { -1, -1 };
    int logFd = -1;
    rlim_t memBytes = 0;
    pid_t pid, parent = getpid();
    BotHostChildProc *p;
    struct sigaction old;

    if (err != NULL && errSize > 0) err[0] = '\0';
    if (exe == NULL || argv == NULL || argv[0] == NULL) return NULL;
    if (lim != NULL && lim->memMb > 0) {
        memBytes = (rlim_t)lim->memMb * (rlim_t)1024 * 1024;
    }
    p = (BotHostChildProc *)calloc(1, sizeof(*p));
    if (p == NULL) return NULL;

    /* A write to a child that ended must not end the game. */
    if (sigaction(SIGPIPE, NULL, &old) == 0 && old.sa_handler == SIG_DFL) {
        signal(SIGPIPE, SIG_IGN);
    }

    if (pipe(inPipe) != 0 || pipe(outPipe) != 0) {
        setErr(err, errSize, "pipe failed");
        goto fail;
    }
    setCloexec(inPipe[1]);
    setCloexec(outPipe[0]);
    (void)fcntl(outPipe[0], F_SETFL, fcntl(outPipe[0], F_GETFL) | O_NONBLOCK);
    if (logPath != NULL && logPath[0] != '\0') {
        logFd = open(logPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    if (logFd < 0) logFd = open("/dev/null", O_WRONLY);
    if (logFd < 0) {
        setErr(err, errSize, "no log file");
        goto fail;
    }

    pid = fork();
    if (pid < 0) {
        setErr(err, errSize, "fork failed");
        goto fail;
    }
    if (pid == 0) {
        childExec(exe, (char *const *)argv, inPipe[0], outPipe[1], logFd,
                  memBytes, parent);
    }
    close(inPipe[0]);
    close(outPipe[1]);
    close(logFd);
    p->pid = pid;
    p->stdinW = inPipe[1];
    p->stdoutR = outPipe[0];
    p->exitCode = -1;
    return p;

fail:
    if (inPipe[0] >= 0) close(inPipe[0]);
    if (inPipe[1] >= 0) close(inPipe[1]);
    if (outPipe[0] >= 0) close(outPipe[0]);
    if (outPipe[1] >= 0) close(outPipe[1]);
    if (logFd >= 0) close(logFd);
    free(p);
    return NULL;
}

bool botHostChildWriteLine(BotHostChildProc *p, const char *line) {
    char buf[600];
    int n;
    ssize_t w;
    if (p == NULL || p->stdinW < 0 || line == NULL) return false;
    n = snprintf(buf, sizeof(buf), "%s\n", line);
    if (n <= 0 || (size_t)n >= sizeof(buf)) return false;
    do {
        w = write(p->stdinW, buf, (size_t)n);
    } while (w < 0 && errno == EINTR);
    return w == (ssize_t)n;
}

void botHostChildCloseStdin(BotHostChildProc *p) {
    if (p == NULL || p->stdinW < 0) return;
    close(p->stdinW);
    p->stdinW = -1;
}

int botHostChildRead(BotHostChildProc *p, char *buf, size_t size) {
    ssize_t n;
    if (p == NULL || p->stdoutR < 0 || size == 0) return -1;
    do {
        n = read(p->stdoutR, buf, size);
    } while (n < 0 && errno == EINTR);
    if (n > 0) return (int)n;
    if (n == 0) return -1;
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
}

static void takeStatus(BotHostChildProc *p, int status) {
    p->reaped = true;
    p->exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool botHostChildAlive(BotHostChildProc *p, int *exitCode) {
    int status;
    pid_t r;
    if (p == NULL) return false;
    if (!p->reaped) {
        r = waitpid(p->pid, &status, WNOHANG);
        if (r == 0) return true;
        if (r == p->pid) {
            takeStatus(p, status);
        } else {
            p->reaped = true;
            p->exitCode = -1;
        }
    }
    if (exitCode != NULL) *exitCode = p->exitCode;
    return false;
}

bool botHostChildWait(BotHostChildProc *p, uint32_t waitMs, int *exitCode) {
    uint32_t waited = 0;
    for (;;) {
        if (!botHostChildAlive(p, exitCode)) return true;
        if (waited >= waitMs) return false;
        {
            struct timespec ts = { 0, 10 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
        waited += 10;
    }
}

void botHostChildKill(BotHostChildProc *p) {
    int status;
    if (p == NULL || p->reaped) return;
    if (!botHostChildAlive(p, NULL)) return;
    /* Only the child this program started, by its own pid. */
    (void)kill(p->pid, SIGKILL);
    if (waitpid(p->pid, &status, 0) == p->pid) {
        takeStatus(p, status);
    } else {
        p->reaped = true;
    }
}

void botHostChildFree(BotHostChildProc *p) {
    if (p == NULL) return;
    botHostChildKill(p);
    if (p->stdinW >= 0) close(p->stdinW);
    if (p->stdoutR >= 0) close(p->stdoutR);
    free(p);
}

unsigned long botHostChildPid(const BotHostChildProc *p) {
    return p != NULL ? (unsigned long)p->pid : 0ul;
}

bool botHostChildCloseJobForTest(BotHostChildProc *p) {
    (void)p;
    return false;
}

bool botHostChildQueryJobForTest(BotHostChildProc *p, int *memMb,
                                 int *cpuPct, int *activeProcesses,
                                 bool *killOnClose) {
    (void)p; (void)memMb; (void)cpuPct; (void)activeProcesses;
    (void)killOnClose;
    return false;
}

#endif /* !_WIN32 */
