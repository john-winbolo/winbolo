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
 *     stdout and log file (and the relay pipes as fds 3 and 4 in the
 *     sandbox).
 * On every system the child also ends when its stdin closes
 * (--exit-on-stdin-close), which is what ends it on macOS when the game
 * ends without a word.
 *
 * The file sandbox on Linux, set in the child between the fork and the
 * exec, so it holds from the child's first instruction:
 *   - PR_SET_NO_NEW_PRIVS;
 *   - Landlock: read (and run) the program's folder, /usr, /lib*, a few
 *     files of /etc, /proc and /sys and /dev; read and write the cache
 *     folder; nothing else. No Landlock (a kernel before 5.13, or Landlock
 *     off) means no sandbox, and so no bots;
 *   - seccomp: socket(), io_uring_setup, pidfd_open, ptrace and tkill fail
 *     with EPERM; kill and tgkill only to the child itself; another
 *     architecture's system calls end the child.
 * A failure there is sent back over a status pipe before the child ends,
 * so the game knows the sandbox failed. On macOS the child sets its own
 * sandbox at the start of its main (bothost_child_main.c).
 *
 * Between the fork and the exec the child calls only async-signal-safe
 * functions and raw system calls (the game has other threads): every
 * path, number and buffer is made before the fork.
 */
#ifndef _WIN32

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
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
#include <stddef.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#endif

#include "bothost_child.h"

#if defined(__linux__) && !defined(__ANDROID__) &&                         \
    (defined(__x86_64__) || defined(__aarch64__))
#define CHILD_LINUX_SANDBOX 1
#else
#define CHILD_LINUX_SANDBOX 0
#endif

/* The relay pipes' size, where the system lets it change. */
#define RELAY_PIPE_BYTES (256 * 1024)

struct BotHostChildProc {
    pid_t pid;
    int   stdinW;   /* -1 when closed */
    int   stdoutR;  /* -1 when closed */
    int   relayW;   /* the game's end of the relay to the child; or -1 */
    int   relayR;   /* the game's end of the relay from the child */
    bool  reaped;
    int   exitCode;
};

static void setErr(char *err, size_t errSize, const char *what) {
    if (err != NULL && errSize > 0) {
        snprintf(err, errSize, "%s (%s)", what, strerror(errno));
    }
}

#ifndef __linux__
static void setCloexec(int fd) {
    int f = fcntl(fd, F_GETFD);
    if (f >= 0) (void)fcntl(fd, F_SETFD, f | FD_CLOEXEC);
}
#endif

/* A pipe whose two ends close at an exec. Linux makes it so at once
 * (pipe2); macOS has no pipe2, so another thread's fork and exec between
 * pipe and fcntl could take a copy. Note: that gap is open on macOS. */
static int pipeCloexec(int fds[2]) {
#ifdef __linux__
    /* The raw call: a header forced in before this file's _GNU_SOURCE
     * (luajit_compat.h) can hide the pipe2 declaration. */
    return (int)syscall(SYS_pipe2, fds, O_CLOEXEC);
#else
    if (pipe(fds) != 0) return -1;
    setCloexec(fds[0]);
    setCloexec(fds[1]);
    return 0;
#endif
}

/* The game's write ends: a child that ended gives EPIPE, not SIGPIPE. */
static void noSigpipe(int fd) {
#ifdef F_SETNOSIGPIPE
    /* macOS: for this fd only; the SIGPIPE ignore below covers Linux. */
    (void)fcntl(fd, F_SETNOSIGPIPE, 1);
#else
    (void)fd;
#endif
}

/* ---- Landlock and seccomp (Linux) ------------------------------------ */

#if CHILD_LINUX_SANDBOX

#ifndef O_PATH
#define O_PATH 010000000
#endif
#define LL_SYS_CREATE_RULESET 444
#define LL_SYS_ADD_RULE       445
#define LL_SYS_RESTRICT_SELF  446
#define LL_CREATE_RULESET_VERSION 1u
#define LL_RULE_PATH_BENEATH  1

#define LL_EXECUTE     (1ull << 0)
#define LL_WRITE_FILE  (1ull << 1)
#define LL_READ_FILE   (1ull << 2)
#define LL_READ_DIR    (1ull << 3)
#define LL_REFER       (1ull << 13)
#define LL_TRUNCATE    (1ull << 14)
#define LL_IOCTL_DEV   (1ull << 15)
#define LL_FILE_RIGHTS (LL_EXECUTE | LL_WRITE_FILE | LL_READ_FILE |         \
                        LL_TRUNCATE | LL_IOCTL_DEV)

struct llRulesetAttr {
    uint64_t handledAccessFs;
};

struct llPathBeneath {
    uint64_t allowedAccess;
    int32_t  parentFd;
} __attribute__((packed));

#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup 425
#endif
#ifndef __NR_pidfd_open
#define __NR_pidfd_open 434
#endif
#ifndef SECCOMP_RET_KILL_PROCESS
#define SECCOMP_RET_KILL_PROCESS SECCOMP_RET_KILL
#endif
#if defined(__x86_64__)
#define CHILD_AUDIT_ARCH AUDIT_ARCH_X86_64
#else
#define CHILD_AUDIT_ARCH AUDIT_ARCH_AARCH64
#endif

int botHostChildLandlockAbi(void) {
    long abi = syscall(LL_SYS_CREATE_RULESET, NULL, (size_t)0,
                       LL_CREATE_RULESET_VERSION);
    return abi > 0 ? (int)abi : 0;
}

/* The system paths every child reads, with their rights. */
typedef struct {
    const char *path;
    bool        dir;
    bool        write;  /* a file read and written (/dev/null) */
} SysPath;

static const SysPath sysPaths[] = {
    { "/usr", true, false },
    { "/lib", true, false },
    { "/lib64", true, false },
    { "/lib32", true, false },
    { "/etc/ld.so.cache", false, false },
    { "/etc/ld.so.preload", false, false },
    { "/etc/localtime", false, false },
    { "/proc/self", true, false },     /* opened in the child: its own */
    { "/proc/stat", false, false },    /* the CPU counters */
    { "/proc/cpuinfo", false, false },
    { "/proc/meminfo", false, false },
    { "/sys/devices/system/cpu", true, false },
    { "/dev/null", false, true },
    { "/dev/urandom", false, false },
};

/* Everything the child needs, made before the fork. */
typedef struct {
    bool                on;
    int                 abi;
    const char         *readDirs[BOT_HOST_SANDBOX_MAX_PATHS];
    int                 readDirCount;
    const char         *readFiles[BOT_HOST_SANDBOX_MAX_PATHS];
    int                 readFileCount;
    const char         *writeDirs[BOT_HOST_SANDBOX_MAX_PATHS];
    int                 writeDirCount;
    struct sock_filter  filter[80];
    unsigned short      filterLen;
} ChildSandbox;

/* The seccomp filter: a deny-list. An allow-list would be shorter to
 * read, but LuaJIT, SDL and the C library need a long list of calls that
 * changes with their versions, and no computer here has a kernel with
 * Landlock to test one on (the filter goes on only with Landlock). So the
 * filter refuses the calls a brain that got out of the Lua sandbox could
 * use against the game or the kernel, and allows the rest:
 *   - EPERM: socket; ptrace and the calls that read or write another
 *     program's memory (process_vm_readv/writev, pidfd_getfd, pidfd_open,
 *     process_madvise); tkill; new namespaces (unshare, setns); the
 *     mount family; bpf, perf_event_open, userfaultfd; the key calls
 *     (keyctl, add_key, request_key); io_uring; the module, kexec, swap,
 *     reboot, chroot and pivot_root calls;
 *   - ENOSYS: clone3, whose flags are in memory the filter cannot read;
 *     the C library then uses clone;
 *   - clone with a CLONE_NEW* flag: EPERM;
 *   - kill, tgkill, rt_sigqueueinfo and rt_tgsigqueueinfo: only to the
 *     child itself or to 0 (its own process group: the child is the
 *     leader of a group of its own, setpgid in childExec); EPERM else.
 * pidSlot is the index of the instruction that holds the child's pid,
 * which only the child knows. */
#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup 425
#endif
#ifndef __NR_io_uring_enter
#define __NR_io_uring_enter 426
#endif
#ifndef __NR_io_uring_register
#define __NR_io_uring_register 427
#endif
#ifndef __NR_open_tree
#define __NR_open_tree 428
#endif
#ifndef __NR_move_mount
#define __NR_move_mount 429
#endif
#ifndef __NR_fsopen
#define __NR_fsopen 430
#endif
#ifndef __NR_fsconfig
#define __NR_fsconfig 431
#endif
#ifndef __NR_fsmount
#define __NR_fsmount 432
#endif
#ifndef __NR_fspick
#define __NR_fspick 433
#endif
#ifndef __NR_pidfd_open
#define __NR_pidfd_open 434
#endif
#ifndef __NR_clone3
#define __NR_clone3 435
#endif
#ifndef __NR_pidfd_getfd
#define __NR_pidfd_getfd 438
#endif
#ifndef __NR_process_madvise
#define __NR_process_madvise 440
#endif
#ifndef __NR_mount_setattr
#define __NR_mount_setattr 442
#endif
/* All the CLONE_NEW* flags of clone's first argument (CLONE_NEWTIME is
 * only for clone3, which is refused). */
#define CHILD_CLONE_NS_FLAGS                                               \
    (0x00020000u /* NEWNS */ | 0x02000000u /* NEWCGROUP */ |               \
     0x04000000u /* NEWUTS */ | 0x08000000u /* NEWIPC */ |                 \
     0x10000000u /* NEWUSER */ | 0x20000000u /* NEWPID */ |                \
     0x40000000u /* NEWNET */)

static void buildFilter(ChildSandbox *cs, int *pidSlot) {
    static const unsigned blocked[] = {
        __NR_socket, __NR_ptrace, __NR_tkill,
        __NR_process_vm_readv, __NR_process_vm_writev, __NR_pidfd_getfd,
        __NR_pidfd_open, __NR_process_madvise,
        __NR_unshare, __NR_setns,
        __NR_mount, __NR_umount2, __NR_open_tree, __NR_move_mount,
        __NR_fsopen, __NR_fsconfig, __NR_fsmount, __NR_fspick,
        __NR_mount_setattr, __NR_pivot_root, __NR_chroot,
        __NR_bpf, __NR_perf_event_open, __NR_userfaultfd,
        __NR_keyctl, __NR_add_key, __NR_request_key,
        __NR_io_uring_setup, __NR_io_uring_enter, __NR_io_uring_register,
        __NR_init_module, __NR_finit_module, __NR_delete_module,
        __NR_kexec_load, __NR_swapon, __NR_swapoff, __NR_reboot,
    };
    static const unsigned pidChecked[] = {
        __NR_kill, __NR_tgkill, __NR_rt_sigqueueinfo, __NR_rt_tgsigqueueinfo,
    };
    const int nb = (int)(sizeof(blocked) / sizeof(blocked[0]));
    const int np = (int)(sizeof(pidChecked) / sizeof(pidChecked[0]));
    struct sock_filter *f = cs->filter;
    int n = 0, i, chk, cloneChk, err, nosys, allowEnd;
#if defined(__x86_64__)
    const int x32 = 1;
#else
    const int x32 = 0;
#endif
    /* The layout: the arch check (3), the nr load (1), x32 (0 or 1), the
     * blocked list, clone3, clone, the pid list, allow; then chk (4),
     * cloneChk (3), err, nosys, allowEnd. */
    chk = 4 + x32 + nb + 2 + np + 1;
    cloneChk = chk + 4;
    err = cloneChk + 3;
    nosys = err + 1;
    allowEnd = nosys + 1;

#define AT(i_) ((unsigned char)((i_) - n - 1))
    f[n] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                        offsetof(struct seccomp_data, arch));
    n++;
    f[n] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                                        CHILD_AUDIT_ARCH, 1, 0);
    n++;
    f[n] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K,
                                        SECCOMP_RET_KILL_PROCESS);
    n++;
    f[n] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                        offsetof(struct seccomp_data, nr));
    n++;
#if defined(__x86_64__)
    /* x32 system calls: refused. */
    f[n] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K,
                                        0x40000000u, AT(err), 0);
    n++;
#endif
    for (i = 0; i < nb; i++) {
        f[n] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                                            blocked[i], AT(err), 0);
        n++;
    }
    f[n] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                                        __NR_clone3, AT(nosys), 0);
    n++;
    f[n] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                                        __NR_clone, AT(cloneChk), 0);
    n++;
    for (i = 0; i < np; i++) {
        f[n] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                                            pidChecked[i], AT(chk), 0);
        n++;
    }
    f[n] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    n++;
    /* chk: the first argument (the low half, little-endian) is the child
     * itself, or 0 (its own process group). */
    f[n] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                        offsetof(struct seccomp_data, args));
    n++;
    *pidSlot = n;
    f[n] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0,
                                        AT(allowEnd), 0);
    n++;
    f[n] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0,
                                        AT(allowEnd), 0);
    n++;
    f[n] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K,
                                        SECCOMP_RET_ERRNO | (EPERM & 0xFFFF));
    n++;
    /* cloneChk: clone's flags (the first argument, its low half). */
    f[n] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                        offsetof(struct seccomp_data, args));
    n++;
    f[n] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K,
                                        CHILD_CLONE_NS_FLAGS, AT(err), 0);
    n++;
    f[n] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    n++;
    /* err */
    f[n] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K,
                                        SECCOMP_RET_ERRNO | (EPERM & 0xFFFF));
    n++;
    /* nosys */
    f[n] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K,
                                        SECCOMP_RET_ERRNO | (ENOSYS & 0xFFFF));
    n++;
    /* allowEnd */
    f[n] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    n++;
#undef AT
    cs->filterLen = (unsigned short)n;
}

static int filterPidSlot;

static bool prepareChildSandbox(ChildSandbox *cs,
                                const BotHostChildSandbox *sb, char *err,
                                size_t errSize) {
    memset(cs, 0, sizeof(*cs));
    if (sb == NULL || !sb->on) return true;
    cs->abi = botHostChildLandlockAbi();
    if (cs->abi < 1) {
        if (err != NULL && errSize > 0) {
            snprintf(err, errSize, "this kernel has no Landlock");
        }
        return false;
    }
    cs->on = true;
    memcpy(cs->readDirs, sb->readDirs, sizeof(cs->readDirs));
    cs->readDirCount = sb->readDirCount;
    memcpy(cs->readFiles, sb->readFiles, sizeof(cs->readFiles));
    cs->readFileCount = sb->readFileCount;
    memcpy(cs->writeDirs, sb->writeDirs, sizeof(cs->writeDirs));
    cs->writeDirCount = sb->writeDirCount;
    buildFilter(cs, &filterPidSlot);
    return true;
}

/* In the child: one rule. A path that is not there is left out. */
static void llAdd(int rs, const char *path, uint64_t rights) {
    struct llPathBeneath pb;
    int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0) return;
    pb.allowedAccess = rights;
    pb.parentFd = fd;
    (void)syscall(LL_SYS_ADD_RULE, rs, LL_RULE_PATH_BENEATH, &pb, 0);
    (void)close(fd);
}

/* In the child, after the fork. 0, or the step that failed. */
static int enterChildSandbox(ChildSandbox *cs) {
    struct llRulesetAttr ra;
    struct sock_fprog prog;
    uint64_t handled, readRights, fileRead;
    size_t i;
    int rs, k;

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return 1;

    handled = (1ull << 13) - 1;              /* ABI 1 */
    if (cs->abi >= 2) handled |= LL_REFER;
    if (cs->abi >= 3) handled |= LL_TRUNCATE;
    if (cs->abi >= 5) handled |= LL_IOCTL_DEV;
    readRights = (LL_EXECUTE | LL_READ_FILE | LL_READ_DIR) & handled;
    fileRead = (LL_EXECUTE | LL_READ_FILE) & handled;
    ra.handledAccessFs = handled;
    rs = (int)syscall(LL_SYS_CREATE_RULESET, &ra, sizeof(ra), 0);
    if (rs < 0) return 2;
    for (i = 0; i < sizeof(sysPaths) / sizeof(sysPaths[0]); i++) {
        uint64_t r = sysPaths[i].dir ? readRights : fileRead;
        if (sysPaths[i].write) {
            r |= (LL_WRITE_FILE | LL_TRUNCATE) & handled;
        }
        llAdd(rs, sysPaths[i].path, r);
    }
    for (k = 0; k < cs->readDirCount; k++) {
        llAdd(rs, cs->readDirs[k], readRights);
    }
    for (k = 0; k < cs->readFileCount; k++) {
        llAdd(rs, cs->readFiles[k], fileRead & LL_FILE_RIGHTS);
    }
    for (k = 0; k < cs->writeDirCount; k++) {
        /* All but execute: no program written there can start. */
        llAdd(rs, cs->writeDirs[k], handled & ~LL_EXECUTE);
    }
    if (syscall(LL_SYS_RESTRICT_SELF, rs, 0) != 0) {
        (void)close(rs);
        return 3;
    }
    (void)close(rs);

    cs->filter[filterPidSlot].k = (uint32_t)getpid();
    prog.len = cs->filterLen;
    prog.filter = cs->filter;
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog, 0, 0) != 0) {
        return 4;
    }
    return 0;
}

bool botHostChildSandboxAvailable(char *why, size_t whySize) {
    int abi = botHostChildLandlockAbi();
    if (abi < 1) {
        if (why != NULL && whySize > 0) {
            snprintf(why, whySize, "this kernel has no Landlock (it needs "
                                   "Linux 5.13 or later with Landlock on)");
        }
        return false;
    }
    if (prctl(PR_GET_SECCOMP, 0, 0, 0, 0) < 0) {
        if (why != NULL && whySize > 0) {
            snprintf(why, whySize, "this kernel has no seccomp");
        }
        return false;
    }
    return true;
}

#else /* !CHILD_LINUX_SANDBOX */

int botHostChildLandlockAbi(void) {
#if defined(__linux__)
    return 0;
#else
    return -1;
#endif
}

bool botHostChildSandboxAvailable(char *why, size_t whySize) {
#if defined(__APPLE__)
    /* The child sets it itself (sandbox_init_with_parameters). */
    (void)why;
    (void)whySize;
    return true;
#else
    if (why != NULL && whySize > 0) {
        snprintf(why, whySize, "there is no child sandbox on this system");
    }
    return false;
#endif
}

#endif /* CHILD_LINUX_SANDBOX */

bool botHostChildSandboxPrepare(const BotHostChildSandbox *sb,
                                unsigned *readFailed, char *why,
                                size_t whySize) {
    (void)sb;
    (void)why;
    (void)whySize;
    if (readFailed != NULL) *readFailed = 0;
    return true;
}

void botHostChildContainerName(char *out, size_t outSize) {
    if (out != NULL && outSize > 0) out[0] = '\0';
}

/* ---- The process ----------------------------------------------------- */

typedef struct {
    int    inR, outW, logFd;
    int    relayIn, relayOut;   /* the child's ends, or -1 */
    int    statusW;             /* the sandbox's status pipe, or -1 */
    rlim_t memBytes;
    pid_t  parent;
    long   maxFd;
    char *const *envp;          /* the child's environment */
#if CHILD_LINUX_SANDBOX
    ChildSandbox *sandbox;
#endif
} ChildSetup;

/* fd onto want; fd may already be want. */
static int moveFd(int fd, int want) {
    if (fd == want) {
        int f = fcntl(fd, F_GETFD);
        return f >= 0 ? fcntl(fd, F_SETFD, f & ~FD_CLOEXEC) : -1;
    }
    return dup2(fd, want) < 0 ? -1 : 0;
}

static void childFail(const ChildSetup *c, unsigned char step) {
    if (c->statusW >= 0) {
        ssize_t w;
        do {
            w = write(c->statusW, &step, 1);
        } while (w < 0 && errno == EINTR);
    }
    _exit(126);
}

/* The child, after the fork. Async-signal-safe calls and raw system
 * calls only. */
static void childExec(const char *exe, char *const *argv, ChildSetup *c) {
    struct sigaction dfl;
    int fds[6], i;
    long fd;

#ifdef __linux__
    /* Dies with the game; the game may already be gone. */
    (void)prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() != c->parent) _exit(127);
#endif
    (void)setpgid(0, 0);

    /* Every fd that goes on is first moved above 9, so no dup2 below
     * overwrites another one. */
    fds[0] = c->inR;
    fds[1] = c->outW;
    fds[2] = c->logFd;
    fds[3] = c->relayIn;
    fds[4] = c->relayOut;
    fds[5] = c->statusW;
    for (i = 0; i < 6; i++) {
        if (fds[i] < 0) continue;
        fds[i] = fcntl(fds[i], F_DUPFD_CLOEXEC, 10);
        if (fds[i] < 0) _exit(127);
    }
    c->statusW = fds[5];
    if (dup2(fds[0], 0) < 0 || dup2(fds[1], 1) < 0 || dup2(fds[2], 2) < 0) {
        childFail(c, 10);
    }
    if (fds[3] >= 0 && (moveFd(fds[3], 3) != 0 || moveFd(fds[4], 4) != 0)) {
        childFail(c, 11);
    }
    /* The rest closes, but the status pipe, which closes at the exec.
     * close_range (Linux 5.9) when there is one; else one by one. */
    {
        long low = fds[3] >= 0 ? 5 : 3;
        bool done = false;
#if defined(__linux__) && defined(SYS_close_range)
        if (c->statusW < 0) {
            done = syscall(SYS_close_range, (unsigned)low, ~0u, 0u) == 0;
        } else {
            done = syscall(SYS_close_range, (unsigned)low,
                           (unsigned)c->statusW - 1u, 0u) == 0 &&
                   syscall(SYS_close_range, (unsigned)c->statusW + 1u, ~0u,
                           0u) == 0;
        }
#endif
        for (fd = low; !done && fd < c->maxFd; fd++) {
            if (fd != c->statusW) (void)close((int)fd);
        }
    }

    /* The game may ignore SIGPIPE; the child starts with the default. */
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    (void)sigaction(SIGPIPE, &dfl, NULL);

    if (c->memBytes > 0) {
        /* Note: macOS does not apply RLIMIT_AS well. */
        struct rlimit rl;
        rl.rlim_cur = c->memBytes;
        rl.rlim_max = c->memBytes;
        (void)setrlimit(RLIMIT_AS, &rl);
    }
    (void)setpriority(PRIO_PROCESS, 0, 10);

#if CHILD_LINUX_SANDBOX
    if (c->sandbox != NULL && c->sandbox->on) {
        int step = enterChildSandbox(c->sandbox);
        if (step != 0) childFail(c, (unsigned char)step);
    }
#endif

    execve(exe, argv, c->envp);
    childFail(c, 20);
}

extern char **environ;

/* The sandboxed child's environment (botHostChildEnvKeep), PATH and
 * TMPDIR (the cache folder), built before the fork. One spawn at a time:
 * the main thread. */
#define CHILD_ENV_MAX 64
static char *childEnv[CHILD_ENV_MAX + 3];
static char  childEnvTmp[1200];

static char *const *childEnvBuild(const BotHostChildSandbox *sb) {
    static char envPath[] = "PATH=/usr/bin:/bin";
    char **e;
    int n = 0;
    for (e = environ; e != NULL && *e != NULL && n < CHILD_ENV_MAX; e++) {
        if (botHostChildEnvKeep(*e)) childEnv[n++] = *e;
    }
    childEnv[n++] = envPath;
    if (sb->writeDirCount > 0) {
        snprintf(childEnvTmp, sizeof(childEnvTmp), "TMPDIR=%s",
                 sb->writeDirs[0]);
        childEnv[n++] = childEnvTmp;
    }
    childEnv[n] = NULL;
    return childEnv;
}

/* argv and the sandbox's arguments, in buf. NULL when it does not fit. */
static const char **sandboxArgv(const char *const *argv,
                                const BotHostChildSandbox *sb,
                                const char **out, int maxOut) {
    int n = 0, i;
#define PUSH(s_)                                                           \
    do {                                                                   \
        if (n + 1 >= maxOut) return NULL;                                  \
        out[n++] = (s_);                                                   \
    } while (0)
    for (i = 0; argv[i] != NULL; i++) PUSH(argv[i]);
    PUSH("--sandbox");
    for (i = 0; i < sb->readDirCount; i++) {
        PUSH("--sandbox-read");
        PUSH(sb->readDirs[i]);
    }
    for (i = 0; i < sb->readFileCount; i++) {
        PUSH("--sandbox-file");
        PUSH(sb->readFiles[i]);
    }
    for (i = 0; i < sb->writeDirCount; i++) {
        PUSH("--sandbox-write");
        PUSH(sb->writeDirs[i]);
    }
    PUSH("--relay");
    PUSH("3");
    PUSH("4");
#undef PUSH
    out[n] = NULL;
    return out;
}

BotHostChildProc *botHostChildSpawn(const char *exe, const char *const *argv,
                                    const BotHostChildLimits *lim,
                                    const BotHostChildSandbox *sb,
                                    const char *logPath, char *err,
                                    size_t errSize, bool *sandboxFailed) {
    int inPipe[2] = { -1, -1 }, outPipe[2] = { -1, -1 };
    int rin[2] = { -1, -1 }, rout[2] = { -1, -1 }, st[2] = { -1, -1 };
    int logFd = -1;
    pid_t pid;
    BotHostChildProc *p;
    struct sigaction old;
    ChildSetup c;
    const char *fullArgv[BOT_HOST_CHILD_MAX_ARGS + 4 +
                         6 * BOT_HOST_SANDBOX_MAX_PATHS];
    const char *const *useArgv = argv;
    bool sandbox = sb != NULL && sb->on;
#if CHILD_LINUX_SANDBOX
    static ChildSandbox childSb; /* one spawn at a time: the main thread */
#endif

    if (err != NULL && errSize > 0) err[0] = '\0';
    if (sandboxFailed != NULL) *sandboxFailed = false;
    if (exe == NULL || argv == NULL || argv[0] == NULL) return NULL;
    memset(&c, 0, sizeof(c));
    c.relayIn = c.relayOut = c.statusW = -1;
    c.parent = getpid();
    c.envp = sandbox ? childEnvBuild(sb) : environ;
    c.maxFd = sysconf(_SC_OPEN_MAX);
    if (c.maxFd < 0 || c.maxFd > 65536) c.maxFd = 65536;
    if (lim != NULL && lim->memMb > 0) {
        c.memBytes = (rlim_t)lim->memMb * (rlim_t)1024 * 1024;
    }
    if (sandbox) {
#if CHILD_LINUX_SANDBOX
        if (!prepareChildSandbox(&childSb, sb, err, errSize)) {
            if (sandboxFailed != NULL) *sandboxFailed = true;
            return NULL;
        }
        c.sandbox = &childSb;
#elif !defined(__APPLE__)
        if (err != NULL && errSize > 0) {
            snprintf(err, errSize, "there is no child sandbox on this "
                                   "system");
        }
        if (sandboxFailed != NULL) *sandboxFailed = true;
        return NULL;
#endif
        useArgv = sandboxArgv(argv, sb, fullArgv,
                              (int)(sizeof(fullArgv) / sizeof(fullArgv[0])));
        if (useArgv == NULL) {
            if (err != NULL && errSize > 0) {
                snprintf(err, errSize, "too many arguments");
            }
            return NULL;
        }
    }
    p = (BotHostChildProc *)calloc(1, sizeof(*p));
    if (p == NULL) return NULL;
    p->relayW = p->relayR = -1;

    /* A write to a child that ended must not end the game. */
    if (sigaction(SIGPIPE, NULL, &old) == 0 && old.sa_handler == SIG_DFL) {
        signal(SIGPIPE, SIG_IGN);
    }

    if (pipeCloexec(inPipe) != 0 || pipeCloexec(outPipe) != 0) {
        setErr(err, errSize, "pipe failed");
        goto fail;
    }
    (void)fcntl(outPipe[0], F_SETFL, fcntl(outPipe[0], F_GETFL) | O_NONBLOCK);
    noSigpipe(inPipe[1]);
    if (sandbox) {
        if (pipeCloexec(rin) != 0 || pipeCloexec(rout) != 0 ||
            pipeCloexec(st) != 0) {
            setErr(err, errSize, "pipe failed (the relay)");
            goto fail;
        }
#ifdef F_SETPIPE_SZ
        (void)fcntl(rin[1], F_SETPIPE_SZ, RELAY_PIPE_BYTES);
        (void)fcntl(rout[1], F_SETPIPE_SZ, RELAY_PIPE_BYTES);
#endif
        noSigpipe(rin[1]);
        c.relayIn = rin[0];
        c.relayOut = rout[1];
        c.statusW = st[1];
    }
    if (logPath != NULL && logPath[0] != '\0') {
        logFd = open(logPath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    }
    if (logFd < 0) logFd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (logFd < 0) {
        setErr(err, errSize, "no log file");
        goto fail;
    }
    c.inR = inPipe[0];
    c.outW = outPipe[1];
    c.logFd = logFd;

    pid = fork();
    if (pid < 0) {
        setErr(err, errSize, "fork failed");
        goto fail;
    }
    if (pid == 0) childExec(exe, (char *const *)useArgv, &c);
    close(inPipe[0]);
    inPipe[0] = -1;
    close(outPipe[1]);
    outPipe[1] = -1;
    close(logFd);
    logFd = -1;
    p->pid = pid;
    p->exitCode = -1;
    if (sandbox) {
        unsigned char step = 0;
        ssize_t r;
        close(rin[0]);
        rin[0] = -1;
        close(rout[1]);
        rout[1] = -1;
        close(st[1]);
        st[1] = -1;
        /* 0 bytes: the exec closed it, the child runs in its sandbox. */
        do {
            r = read(st[0], &step, 1);
        } while (r < 0 && errno == EINTR);
        close(st[0]);
        st[0] = -1;
        if (r != 0) {
            int status;
            if (err != NULL && errSize > 0) {
                snprintf(err, errSize, "the child could not enter its "
                                       "sandbox (step %u)", (unsigned)step);
            }
            if (sandboxFailed != NULL) *sandboxFailed = step < 10;
            (void)waitpid(pid, &status, 0);
            close(inPipe[1]);
            close(outPipe[0]);
            close(rin[1]);
            close(rout[0]);
            free(p);
            return NULL;
        }
        p->relayW = rin[1];
        p->relayR = rout[0];
    }
    p->stdinW = inPipe[1];
    p->stdoutR = outPipe[0];
    return p;

fail:
    if (inPipe[0] >= 0) close(inPipe[0]);
    if (inPipe[1] >= 0) close(inPipe[1]);
    if (outPipe[0] >= 0) close(outPipe[0]);
    if (outPipe[1] >= 0) close(outPipe[1]);
    if (rin[0] >= 0) close(rin[0]);
    if (rin[1] >= 0) close(rin[1]);
    if (rout[0] >= 0) close(rout[0]);
    if (rout[1] >= 0) close(rout[1]);
    if (st[0] >= 0) close(st[0]);
    if (st[1] >= 0) close(st[1]);
    if (logFd >= 0) close(logFd);
    free(p);
    return NULL;
}

bool botHostChildHasRelay(const BotHostChildProc *p) {
    return p != NULL && p->relayR >= 0 && p->relayW >= 0;
}

bool botHostChildRelayRead(BotHostChildProc *p, void *buf, size_t n) {
    uint8_t *b = (uint8_t *)buf;
    if (p == NULL || p->relayR < 0) return false;
    while (n > 0) {
        ssize_t r = read(p->relayR, b, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        b += r;
        n -= (size_t)r;
    }
    return true;
}

bool botHostChildRelayWrite(BotHostChildProc *p, const void *buf, size_t n) {
    const uint8_t *b = (const uint8_t *)buf;
    if (p == NULL || p->relayW < 0) return false;
    while (n > 0) {
        ssize_t w = write(p->relayW, b, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        b += w;
        n -= (size_t)w;
    }
    return true;
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
    if (p->relayW >= 0) close(p->relayW);
    if (p->relayR >= 0) close(p->relayR);
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
