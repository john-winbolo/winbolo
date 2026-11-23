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
 *                  [--ds-child] [--lockstep] [--threads N]
 *                  [--brain-lua-seed N] [--brain-no-budget-kill]
 *                  [--brain-tier N]
 *                  [--sandbox [--sandbox-read DIR]... [--sandbox-file F]...
 *                   [--sandbox-write DIR]... --relay IN OUT]
 *
 * It runs the game's own host code (bothost_embed.c, the embed copy of
 * bothost_main.c) on its thread, as the game would, and talks with the
 * game in lines on stdin and stdout. The ticket comes on stdin, never as
 * an argument. The game never starts it with a console, so what it writes
 * on stderr goes to the log file the game gives it.
 *
 * Two layers: the Lua sandbox inside the OS sandbox. The Lua sandbox is
 * always on here: --allow-unsafe-brains (or -allow-unsafe-brains) is
 * refused, and the host makes every brain with
 * luaBrainInstanceCreateSandboxed.
 *
 * --sandbox: the game put the child in its file sandbox (Windows: the
 * AppContainer; Linux: Landlock and seccomp) and the child checks that it
 * is in it, or (macOS) enters it itself; it ends with
 * BOT_HOST_CHILD_EXIT_SANDBOX when it is not. Its sockets then go through
 * the relay pipes (--relay: the handles on Windows, fds 3 and 4
 * elsewhere), and it takes the machine id and the CPU counters from the
 * game.
 *
 * --ds-child: the dedicated server's own child (ds_bot_child.h). It joins
 * with the server's secret in place of a player's ticket, takes the
 * server's brain code with no question, and has no player's limits on
 * init tokens or the full map. Its brains still run in both sandboxes.
 * --lockstep: the brains think once for each new snapshot, not on a
 * 20 ms timer. --threads, --brain-lua-seed, --brain-no-budget-kill and
 * --brain-tier are the server's own options, passed on.
 *
 * Test modes for the unit tests, with no host at all:
 *   --test-alloc-mb N   take N MiB of memory in pieces, writing each into
 *                       use; "alloc ok" (exit 0) or "alloc failed at M"
 *                       (exit BOT_HOST_CHILD_EXIT_NO_MEM)
 *   --test-sleep-ms N   wait N ms, then exit 0
 *   --test-read PATH    read a file: "read ok" (0) or "read denied"
 *   --test-write PATH   write a file: "write ok" (0) or "write denied"
 *   --test-net IP PORT  send a datagram with a socket of its own: "net
 *                       sent" (0) or "net denied"
 *   --test-relay IP PORT
 *                       send "ping" through the relay and wait for "pong"
 *                       (and send "stray" to PORT + 1, which the relay
 *                       must refuse): "relay ok" (0) or "relay none"
 *   --test-salt         read the machine id lines, then "ask <salt>"
 *                       lines until "end": "answer <hash>" or "answer
 *                       none" for each
 *   --test-seccomp      Linux: start threads and a LuaJIT state, then try
 *                       fork, a clone with no CLONE_THREAD, the pid
 *                       calls on the game (its parent) and on itself,
 *                       setpgid into the game's group, pidfd_send_signal,
 *                       and the fcntl and ioctl calls that set an fd's
 *                       owner (and two that do not), the calls that
 *                       change a file's mode, owner, times or extended
 *                       attributes, truncate by path (and ftruncate,
 *                       which stays allowed), the System V IPC calls and
 *                       prctl PR_SET_PDEATHSIG (and PR_GET_PDEATHSIG,
 *                       which stays allowed). One
 *                       line for each ("threads ok 4", "lua ok", "fork
 *                       denied" or "fork allowed", ...), then "seccomp
 *                       done" (exit 0). Elsewhere "seccomp none" (exit 0)
 * (each refusal exits BOT_HOST_CHILD_EXIT_DENIED).
 */
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <process.h>
#define getpid _getpid
#else
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#ifdef __linux__
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#endif
#endif

#include <stdarg.h>
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
#include "machine_cpu_free.h"
#include "machine_id.h"
#include "platform_net.h"
#include "udp_socket_hooks.h"
#include "luabrainshandler.h" /* luaBrainsSetAllowUnsafe */
#include "server_sim.h"      /* serverSimBotPoolInit */
#include "cpu_speed_capacity.h" /* cpuSpeedApplyStoredToBrains */
#include "../gui/lang.h"
#include "../gui/clientmutex.h"
#include "../common/wb_log.h"
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

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
static bool   coUnsafeAsked;
static bool   coSandbox;
static const char *coSbRead[BOT_HOST_SANDBOX_MAX_PATHS];
static int    coSbReadCount;
static const char *coSbFile[BOT_HOST_SANDBOX_MAX_PATHS];
static int    coSbFileCount;
static const char *coSbWrite[BOT_HOST_SANDBOX_MAX_PATHS];
static int    coSbWriteCount;
static const char *coRelayIn;
static const char *coRelayOut;
static const char *coTestRead;
static const char *coTestWrite;
static const char *coTestNetIp;
static int    coTestNetPort;
static const char *coTestRelayIp;
static int    coTestRelayPort;
static bool   coTestSalt;
static bool   coDsChild;
static bool   coLockstep;
static int    coThreads;
static bool   coHaveLuaSeed;
static long   coLuaSeed;
static bool   coNoBudgetKill;
static int    coBrainTier;
static bool   coTestSeccomp;

static const char *const childTestModes[] = {
    "--test-alloc-mb", "--test-sleep-ms", "--test-read", "--test-write",
    "--test-net", "--test-relay", "--test-salt", "--test-seccomp",
};

bool botHostChildWanted(int argc, char **argv) {
    int i;
    size_t k;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--embed-child") == 0) return true;
        for (k = 0; k < sizeof(childTestModes) / sizeof(childTestModes[0]);
             k++) {
            if (strcmp(argv[i], childTestModes[k]) == 0) return true;
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
        if (strcmp(a, "--sandbox") == 0) {
            coSandbox = true;
            continue;
        }
        if (strcmp(a, "--test-salt") == 0) {
            coTestSalt = true;
            continue;
        }
        if (strcmp(a, "--ds-child") == 0) {
            coDsChild = true;
            continue;
        }
        if (strcmp(a, "--lockstep") == 0) {
            coLockstep = true;
            continue;
        }
        if (strcmp(a, "--brain-no-budget-kill") == 0) {
            coNoBudgetKill = true;
            continue;
        }
        if (strcmp(a, "--test-seccomp") == 0) {
            coTestSeccomp = true;
            continue;
        }
        if (strcmp(a, "--allow-unsafe-brains") == 0 ||
            strcmp(a, "-allow-unsafe-brains") == 0) {
            /* Refused in main, with its own line. */
            coUnsafeAsked = true;
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
        } else if (strcmp(a, "--threads") == 0) {
            coThreads = atoi(v);
        } else if (strcmp(a, "--brain-lua-seed") == 0) {
            coHaveLuaSeed = true;
            coLuaSeed = strtol(v, NULL, 0);
        } else if (strcmp(a, "--brain-tier") == 0) {
            coBrainTier = atoi(v);
        } else if (strcmp(a, "--test-alloc-mb") == 0) {
            coTestAllocMb = atol(v);
        } else if (strcmp(a, "--test-sleep-ms") == 0) {
            coTestSleepMs = atol(v);
        } else if (strcmp(a, "--sandbox-read") == 0) {
            (void)botHostChildSandboxAdd(coSbRead, &coSbReadCount, v);
        } else if (strcmp(a, "--sandbox-file") == 0) {
            (void)botHostChildSandboxAdd(coSbFile, &coSbFileCount, v);
        } else if (strcmp(a, "--sandbox-write") == 0) {
            (void)botHostChildSandboxAdd(coSbWrite, &coSbWriteCount, v);
        } else if (strcmp(a, "--test-read") == 0) {
            coTestRead = v;
        } else if (strcmp(a, "--test-write") == 0) {
            coTestWrite = v;
        } else if (strcmp(a, "--relay") == 0 || strcmp(a, "--test-net") == 0 ||
                   strcmp(a, "--test-relay") == 0) {
            /* Two values. */
            if (i + 2 >= argc) {
                fprintf(stderr, "[bothost] %s needs two values\n", a);
                return false;
            }
            if (strcmp(a, "--relay") == 0) {
                coRelayIn = v;
                coRelayOut = argv[i + 2];
            } else if (strcmp(a, "--test-net") == 0) {
                coTestNetIp = v;
                coTestNetPort = atoi(argv[i + 2]);
            } else {
                coTestRelayIp = v;
                coTestRelayPort = atoi(argv[i + 2]);
            }
            i++;
        } else {
            fprintf(stderr, "[bothost] unknown child option %s\n", a);
            return false;
        }
        i++;
    }
    return true;
}

/* ---- stdout ---------------------------------------------------------- */

/* The main thread and the host's thread (a salt line) both write. */
static SDL_Mutex *outLock;

static void childOut(const char *fmt, ...) {
    va_list ap;
    if (outLock != NULL) SDL_LockMutex(outLock);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
    if (outLock != NULL) SDL_UnlockMutex(outLock);
}

/* ---- The sandbox ----------------------------------------------------- */

#if defined(__APPLE__)
/* libsystem_sandbox: no public header for these two. */
extern int sandbox_init_with_parameters(const char *profile, uint64_t flags,
                                        const char *const parameters[],
                                        char **errorbuf);
extern void sandbox_free_error(char *errorbuf);

/* Untested: no Mac to run it on yet. Deny by default; read the system's
 * libraries and the given folders; read and write the cache; no network;
 * signals to itself only; the few mach services the C library, the
 * notify system and the system log need at start (a guess until it runs
 * on a Mac: a child that fails here exits with code 5, and the player
 * gets no bots). */
static bool enterMacSandbox(void) {
    char profile[4096];
    static char real[3 * BOT_HOST_SANDBOX_MAX_PATHS][PATH_MAX];
    const char *params[2 * 3 * BOT_HOST_SANDBOX_MAX_PATHS + 1];
    char names[3 * BOT_HOST_SANDBOX_MAX_PATHS][8];
    size_t len;
    int np = 0, nr = 0, i;
    char *err = NULL;

    len = (size_t)snprintf(profile, sizeof(profile),
        "(version 1)\n"
        "(deny default)\n"
        "(allow file-read-metadata)\n"
        "(allow sysctl-read)\n"
        "(allow signal (target self))\n"
        "(allow mach-lookup\n"
        "  (global-name \"com.apple.system.notification_center\")\n"
        "  (global-name \"com.apple.system.logger\")\n"
        "  (global-name \"com.apple.system.opendirectoryd.libinfo\")\n"
        "  (global-name \"com.apple.logd\")\n"
        "  (global-name \"com.apple.diagnosticd\"))\n"
        "(allow ipc-posix-shm-read-data\n"
        "  (ipc-posix-name \"apple.shm.notification_center\"))\n"
        "(allow file-read* (subpath \"/usr/lib\") (subpath \"/usr/share\")\n"
        "  (subpath \"/System\") (subpath \"/Library/Apple\")\n"
        "  (subpath \"/private/var/db/timezone\")\n"
        "  (literal \"/dev/null\") (literal \"/dev/urandom\")\n"
        "  (literal \"/dev/random\"))\n"
        "(allow file-write-data (literal \"/dev/null\"))\n");
#define ADD_PATHS(list_, count_, rule_, kind_)                               \
    for (i = 0; i < (count_) && len < sizeof(profile); i++) {              \
        if (realpath((list_)[i], real[nr]) == NULL) continue;              \
        snprintf(names[nr], sizeof(names[nr]), "P%d", nr);                 \
        params[np++] = names[nr];                                          \
        params[np++] = real[nr];                                           \
        len += (size_t)snprintf(profile + len, sizeof(profile) - len,      \
                                "(allow " rule_ " (" kind_                 \
                                " (param \"%s\")))\n", names[nr]);          \
        nr++;                                                              \
    }
    ADD_PATHS(coSbRead, coSbReadCount, "file-read*", "subpath")
    ADD_PATHS(coSbFile, coSbFileCount, "file-read*", "literal")
    ADD_PATHS(coSbWrite, coSbWriteCount, "file-read* file-write*", "subpath")
#undef ADD_PATHS
    params[np] = NULL;
    if (len >= sizeof(profile)) return false;
    if (sandbox_init_with_parameters(profile, 0, params, &err) != 0) {
        fprintf(stderr, "[bothost] the macOS sandbox failed: %s\n",
                err != NULL ? err : "?");
        if (err != NULL) sandbox_free_error(err);
        return false;
    }
    return true;
}
#endif

#ifdef _WIN32
/* A crash ends the child. Its log gets one line with the exception code and
 * the place, as a module and an offset, before the process ends. It starts
 * no second program (the job allows none). */
static LONG WINAPI childCrashFilter(EXCEPTION_POINTERS *ep) {
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    HMODULE mod = NULL;
    char path[MAX_PATH] = "?";
    const char *name = path, *c;
    uintptr_t at = (uintptr_t)er->ExceptionAddress, base = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)er->ExceptionAddress, &mod) &&
        GetModuleFileNameA(mod, path, sizeof(path)) > 0) {
        base = (uintptr_t)mod;
        for (c = path; *c != '\0'; c++) {
            if (*c == '\\' || *c == '/') name = c + 1;
        }
    }
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        er->NumberParameters >= 2) {
        fprintf(stderr, "[bothost] the Bot Host child crashed: exception "
                        "0x%08lX at %s+0x%llx (%s 0x%llx)\n",
                (unsigned long)er->ExceptionCode, name,
                (unsigned long long)(at - base),
                er->ExceptionInformation[0] == 0 ? "read" :
                er->ExceptionInformation[0] == 1 ? "write" : "execute",
                (unsigned long long)er->ExceptionInformation[1]);
    } else {
        fprintf(stderr, "[bothost] the Bot Host child crashed: exception "
                        "0x%08lX at %s+0x%llx\n",
                (unsigned long)er->ExceptionCode, name,
                (unsigned long long)(at - base));
    }
    {
        /* Then the frames, as modules and offsets. A frame in no module
         * (m stays NULL) prints "?" and its absolute address. */
        void *fr[48];
        USHORT k, nf = RtlCaptureStackBackTrace(0, 48, fr, NULL);
        for (k = 0; k < nf; k++) {
            HMODULE m = NULL;
            char mp[MAX_PATH] = "?";
            const char *mn = mp, *q;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)fr[k], &m))
                GetModuleFileNameA(m, mp, sizeof(mp));
            for (q = mp; *q; q++) if (*q == '\\' || *q == '/') mn = q + 1;
            fprintf(stderr, "  #%u %s+0x%llx\n", (unsigned)k, mn,
                    (unsigned long long)((uintptr_t)fr[k] - (uintptr_t)m));
        }
    }
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

/* True when the child is in the sandbox the game asked for (and, on
 * macOS, enters it now). */
static bool checkSandbox(void) {
#if defined(_WIN32)
    HANDLE tok = NULL;
    DWORD isContainer = 0, got = 0;
    bool ok = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        ok = GetTokenInformation(tok, TokenIsAppContainer, &isContainer,
                                 sizeof(isContainer), &got) &&
             isContainer != 0;
        CloseHandle(tok);
    }
    return ok;
#elif defined(__linux__) && !defined(__ANDROID__)
    /* The game set them before the exec; seccomp mode 2 is a filter. */
    return prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1 &&
           prctl(PR_GET_SECCOMP, 0, 0, 0, 0) == 2;
#elif defined(__APPLE__)
    return enterMacSandbox();
#else
    return false;
#endif
}

/* ---- The relay: the child's side ------------------------------------- */

/* A virtual socket's value; no real socket of this process has it (it
 * cannot make any). */
#define VSOCK_BASE 0x5000
#define VSOCK_QUEUE_MAX 512

typedef struct RelayPkt {
    struct RelayPkt   *next;
    struct sockaddr_in from;
    int                len;
    uint8_t            data[1];
} RelayPkt;

typedef struct {
    bool      used;
    RelayPkt *head, *tail;
    int       count;
} VSock;

#ifdef _WIN32
static HANDLE relayInH, relayOutH;
#else
static int relayInFd = -1, relayOutFd = -1;
#endif
static SDL_Mutex *relayLock;    /* the sockets and their queues */
static SDL_Mutex *relaySendLock;
static VSock      vsocks[BOT_HOST_RELAY_MAX_SOCKS];
static int        vsockNext;

static bool relayPipeRead(void *buf, size_t n) {
    uint8_t *b = (uint8_t *)buf;
    while (n > 0) {
#ifdef _WIN32
        DWORD got = 0;
        if (!ReadFile(relayInH, b, (DWORD)n, &got, NULL) || got == 0) {
            return false;
        }
#else
        ssize_t got = read(relayInFd, b, n);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
#endif
        b += got;
        n -= (size_t)got;
    }
    return true;
}

static bool relayPipeWrite(const void *buf, size_t n) {
    const uint8_t *b = (const uint8_t *)buf;
    bool ok = true;
    SDL_LockMutex(relaySendLock);
    while (n > 0) {
#ifdef _WIN32
        DWORD wrote = 0;
        if (!WriteFile(relayOutH, b, (DWORD)n, &wrote, NULL) || wrote == 0) {
            ok = false;
            break;
        }
#else
        ssize_t wrote = write(relayOutFd, b, n);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) {
            ok = false;
            break;
        }
#endif
        b += wrote;
        n -= (size_t)wrote;
    }
    SDL_UnlockMutex(relaySendLock);
    return ok;
}

static void relaySendFrame(uint8_t type, int id, const struct sockaddr_in *to,
                           const uint8_t *data, size_t len) {
    static uint8_t frame[BOT_HOST_RELAY_MAX_FRAME];
    size_t n;
    /* The frame buffer is shared: one sender at a time. relayPipeWrite
     * takes the same lock again; SDL mutexes are recursive, so this is
     * valid, and the frame goes out whole. */
    SDL_LockMutex(relaySendLock);
    n = botHostRelayEncode(type, (uint8_t)id,
                           to != NULL ? to->sin_addr.s_addr : 0,
                           to != NULL ? to->sin_port : 0, data, len, frame,
                           sizeof(frame));
    if (n > 0) (void)relayPipeWrite(frame, n);
    SDL_UnlockMutex(relaySendLock);
}

static void vsockDrop(VSock *v) {
    while (v->head != NULL) {
        RelayPkt *n = v->head->next;
        free(v->head);
        v->head = n;
    }
    v->tail = NULL;
    v->count = 0;
}

static int vsockId(bolo_socket_t s) {
    long id = (long)s - VSOCK_BASE;
    return (id >= 0 && id < BOT_HOST_RELAY_MAX_SOCKS) ? (int)id : -1;
}

static bolo_socket_t hookCreate(void) {
    int i, id = -1;
    SDL_LockMutex(relayLock);
    /* Round robin, so a socket id is not used again at once. */
    for (i = 0; i < BOT_HOST_RELAY_MAX_SOCKS; i++) {
        int k = (vsockNext + i) % BOT_HOST_RELAY_MAX_SOCKS;
        if (!vsocks[k].used) {
            id = k;
            break;
        }
    }
    if (id >= 0) {
        vsocks[id].used = true;
        vsockDrop(&vsocks[id]);
        vsockNext = (id + 1) % BOT_HOST_RELAY_MAX_SOCKS;
    }
    SDL_UnlockMutex(relayLock);
    if (id < 0) return BOLO_INVALID_SOCKET;
    relaySendFrame(BOT_HOST_RELAY_OPEN, id, NULL, NULL, 0);
    return (bolo_socket_t)(VSOCK_BASE + id);
}

static void hookSend(bolo_socket_t s, const uint8_t *buf, int len,
                     const struct sockaddr_in *addr) {
    int id = vsockId(s);
    if (id < 0 || addr == NULL || len < 0 ||
        len > BOT_HOST_RELAY_MAX_DATAGRAM) {
        return;
    }
    relaySendFrame(BOT_HOST_RELAY_SEND, id, addr, buf, (size_t)len);
}

static int hookRecv(bolo_socket_t s, uint8_t *buf, int maxLen,
                    struct sockaddr_in *from) {
    int id = vsockId(s), len = -1;
    RelayPkt *pk = NULL;
    if (id < 0) return -1;
    SDL_LockMutex(relayLock);
    if (vsocks[id].used && vsocks[id].head != NULL) {
        pk = vsocks[id].head;
        vsocks[id].head = pk->next;
        if (vsocks[id].head == NULL) vsocks[id].tail = NULL;
        vsocks[id].count--;
    }
    SDL_UnlockMutex(relayLock);
    if (pk == NULL) return -1;
    /* A datagram longer than the buffer is cut, as recvfrom would. */
    len = pk->len < maxLen ? pk->len : maxLen;
    memcpy(buf, pk->data, (size_t)len);
    if (from != NULL) *from = pk->from;
    free(pk);
    return len;
}

static void hookClose(bolo_socket_t s) {
    int id = vsockId(s);
    if (id < 0) return;
    SDL_LockMutex(relayLock);
    vsocks[id].used = false;
    vsockDrop(&vsocks[id]);
    SDL_UnlockMutex(relayLock);
    relaySendFrame(BOT_HOST_RELAY_CLOSE, id, NULL, NULL, 0);
}

static const UdpSocketHooks relayHooks = {
    hookCreate, hookSend, hookRecv, hookClose
};

static int SDLCALL relayReaderFn(void *arg) {
    uint8_t hdr[BOT_HOST_RELAY_HDR];
    static uint8_t body[BOT_HOST_RELAY_ADDR + BOT_HOST_RELAY_MAX_DATAGRAM];
    (void)arg;
    for (;;) {
        uint8_t type, id;
        uint16_t blen;
        uint32_t ip;
        uint16_t port;
        const uint8_t *data;
        size_t n;
        RelayPkt *pk;
        if (!relayPipeRead(hdr, sizeof(hdr))) break;
        if (!botHostRelayParseHeader(hdr, &type, &id, &blen)) break;
        if (blen > 0 && !relayPipeRead(body, blen)) break;
        if (type != BOT_HOST_RELAY_RECV ||
            !botHostRelayParseBody(body, blen, &ip, &port, &data, &n)) {
            continue;
        }
        pk = (RelayPkt *)malloc(sizeof(RelayPkt) + n);
        if (pk == NULL) continue;
        pk->next = NULL;
        memset(&pk->from, 0, sizeof(pk->from));
        pk->from.sin_family = AF_INET;
        pk->from.sin_addr.s_addr = ip;
        pk->from.sin_port = port;
        pk->len = (int)n;
        memcpy(pk->data, data, n);
        SDL_LockMutex(relayLock);
        if (!vsocks[id].used || vsocks[id].count >= VSOCK_QUEUE_MAX) {
            /* A closed socket, or a full queue: dropped, as the system
             * would drop it. */
            free(pk);
        } else {
            if (vsocks[id].tail != NULL) {
                vsocks[id].tail->next = pk;
            } else {
                vsocks[id].head = pk;
            }
            vsocks[id].tail = pk;
            vsocks[id].count++;
        }
        SDL_UnlockMutex(relayLock);
    }
    fprintf(stderr, "[bothost] the relay from the game ended\n");
    return 0;
}

/* Opens the relay pipes and sets the socket hooks. */
static bool startRelay(void) {
    SDL_Thread *t;
    if (coRelayIn == NULL || coRelayOut == NULL) return false;
#ifdef _WIN32
    relayInH = (HANDLE)(uintptr_t)SDL_strtoull(coRelayIn, NULL, 10);
    relayOutH = (HANDLE)(uintptr_t)SDL_strtoull(coRelayOut, NULL, 10);
    if (relayInH == NULL || relayOutH == NULL) return false;
#else
    relayInFd = atoi(coRelayIn);
    relayOutFd = atoi(coRelayOut);
    if (relayInFd < 0 || relayOutFd < 0) return false;
#endif
    relayLock = SDL_CreateMutex();
    relaySendLock = SDL_CreateMutex();
    if (relayLock == NULL || relaySendLock == NULL) return false;
    t = SDL_CreateThread(relayReaderFn, "bothost-relay", NULL);
    if (t == NULL) return false;
    /* It waits in its read until the game closes the pipe. */
    SDL_DetachThread(t);
    udpSetSocketHooks(&relayHooks);
    return true;
}

/* ---- The game's machine id and CPU counters -------------------------- */

static uint8_t           mlId[MACHINE_ID_LEN];
static bool              mlHaveId;
static MachineSaltMemory mlSalts;
static SDL_Mutex        *cpuLock;
static uint64_t          cpuIdle, cpuTotal;
static bool              cpuHave;

static void saltObserver(const uint8_t salt[MACHINE_SALT_LEN]) {
    char hex[2 * MACHINE_SALT_LEN + 1];
    botHostChildTicketHex(salt, MACHINE_SALT_LEN, hex, sizeof(hex));
    childOut("salt %s", hex);
}

static bool cpuFromGame(uint64_t *idle, uint64_t *total) {
    bool have;
    if (cpuLock == NULL) return false;
    SDL_LockMutex(cpuLock);
    have = cpuHave;
    *idle = cpuIdle;
    *total = cpuTotal;
    SDL_UnlockMutex(cpuLock);
    return have;
}

/* A machine id or CPU line from the game: true when it was one. */
static bool takeMachineLine(const char *line) {
    if (strncmp(line, "machineid ", 10) == 0) {
        mlHaveId = botHostChildTicketParse(line + 10, mlId, sizeof(mlId));
        return true;
    }
    if (strncmp(line, "salts", 5) == 0) {
        const char *c = line + 5;
        while (*c == ' ') {
            uint8_t salt[MACHINE_SALT_LEN];
            char hex[2 * MACHINE_SALT_LEN + 1];
            c++;
            if (strlen(c) < 2 * MACHINE_SALT_LEN) break;
            memcpy(hex, c, 2 * MACHINE_SALT_LEN);
            hex[2 * MACHINE_SALT_LEN] = '\0';
            if (botHostChildTicketParse(hex, salt, sizeof(salt))) {
                machineSaltMemoryAdd(&mlSalts, salt);
            }
            c += 2 * MACHINE_SALT_LEN;
        }
        return true;
    }
    if (strncmp(line, "cpu ", 4) == 0) {
        unsigned long long idle = 0, total = 0;
        if (sscanf(line + 4, "%llu %llu", &idle, &total) == 2 &&
            cpuLock != NULL) {
            SDL_LockMutex(cpuLock);
            cpuIdle = idle;
            cpuTotal = total;
            cpuHave = true;
            SDL_UnlockMutex(cpuLock);
        }
        return true;
    }
    return false;
}

/* The game's id and salts in place of the files (which the sandbox may
 * not let the child read). */
static void useGameMachineId(void) {
    if (mlHaveId) {
        machineIdUseGiven(mlId, &mlSalts);
        memset(mlId, 0, sizeof(mlId));
    } else {
        fprintf(stderr, "[bothost] no machine id from the game: the child "
                        "reads its own\n");
        (void)machineIdInit();
    }
    /* The game keeps the salt file: it hears of each answer. */
    machineIdSetSaltObserver(saltObserver);
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

static int testRead(const char *path) {
    FILE *f = fopen(path, "rb");
    char c;
    bool ok = f != NULL && (fread(&c, 1, 1, f) == 1 || feof(f));
    if (f != NULL) fclose(f);
    childOut(ok ? "read ok" : "read denied");
    return ok ? BOT_HOST_CHILD_EXIT_OK : BOT_HOST_CHILD_EXIT_DENIED;
}

static int testWrite(const char *path) {
    FILE *f = fopen(path, "wb");
    bool ok = f != NULL && fputs("sandbox test\n", f) >= 0;
    if (f != NULL && fclose(f) != 0) ok = false;
    childOut(ok ? "write ok" : "write denied");
    return ok ? BOT_HOST_CHILD_EXIT_OK : BOT_HOST_CHILD_EXIT_DENIED;
}

static int testNet(const char *ip, int port) {
    struct sockaddr_in to;
    bolo_socket_t s;
    int r;
    (void)bolo_net_init();
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &to.sin_addr) != 1) {
        childOut("net bad address");
        return BOT_HOST_CHILD_EXIT_ARGS;
    }
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == BOLO_INVALID_SOCKET) {
        childOut("net denied (socket)");
        return BOT_HOST_CHILD_EXIT_DENIED;
    }
    r = (int)sendto(s, "sandbox", 7, 0, (const struct sockaddr *)&to,
                    sizeof(to));
    closesocket(s);
    if (r != 7) {
        childOut("net denied (send)");
        return BOT_HOST_CHILD_EXIT_DENIED;
    }
    childOut("net sent");
    return BOT_HOST_CHILD_EXIT_OK;
}

static int testRelay(const char *ip, int port) {
    struct sockaddr_in to, stray, from;
    bolo_socket_t s;
    uint8_t buf[64];
    uint32_t start;
    if (!startRelay()) {
        childOut("relay none (no pipes)");
        return BOT_HOST_CHILD_EXIT_DENIED;
    }
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &to.sin_addr) != 1) {
        childOut("relay bad address");
        return BOT_HOST_CHILD_EXIT_ARGS;
    }
    stray = to;
    stray.sin_port = htons((uint16_t)(port + 1));
    s = relayHooks.create();
    if (s == BOLO_INVALID_SOCKET) {
        childOut("relay none (no socket)");
        return BOT_HOST_CHILD_EXIT_DENIED;
    }
    relayHooks.send(s, (const uint8_t *)"stray", 5, &stray);
    start = (uint32_t)SDL_GetTicks();
    while ((uint32_t)SDL_GetTicks() - start < 5000u) {
        int n;
        /* Again each 200 ms: a datagram may be lost. */
        if (((uint32_t)SDL_GetTicks() - start) % 200u < 10u) {
            relayHooks.send(s, (const uint8_t *)"ping", 4, &to);
        }
        n = relayHooks.recv(s, buf, (int)sizeof(buf), &from);
        if (n == 4 && memcmp(buf, "pong", 4) == 0 &&
            from.sin_addr.s_addr == to.sin_addr.s_addr &&
            from.sin_port == to.sin_port) {
            relayHooks.close(s);
            childOut("relay ok");
            return BOT_HOST_CHILD_EXIT_OK;
        }
        SDL_Delay(10);
    }
    relayHooks.close(s);
    childOut("relay none");
    return BOT_HOST_CHILD_EXIT_DENIED;
}

static int testSalt(void) {
    char line[BOT_HOST_CHILD_LINE_MAX];
    bool set = false;
    int asked = 0;
    while (fgets(line, sizeof(line), stdin) != NULL) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        if (takeMachineLine(line)) continue;
        if (strcmp(line, "end") == 0) break;
        if (strncmp(line, "ask ", 4) == 0) {
            uint8_t salt[MACHINE_SALT_LEN], hash[MACHINE_HASH_LEN];
            char hex[2 * MACHINE_HASH_LEN + 1];
            if (!set) {
                useGameMachineId();
                set = true;
            }
            asked++;
            if (botHostChildTicketParse(line + 4, salt, sizeof(salt)) &&
                machineIdAnswerSalt(salt, hash)) {
                botHostChildTicketHex(hash, sizeof(hash), hex, sizeof(hex));
                childOut("answer %s", hex);
            } else {
                childOut("answer none");
            }
        }
    }
    return asked > 0 ? BOT_HOST_CHILD_EXIT_OK : BOT_HOST_CHILD_EXIT_ARGS;
}

static int SDLCALL seccompThread(void *arg) {
    volatile unsigned long sum = 0;
    unsigned long i;
    for (i = 0; i < 1000000ul; i++) sum += i;
    *(unsigned long *)arg = sum;
    return 0;
}

#ifdef __linux__
/* "<what> denied" when the call failed with EPERM, else "<what> allowed". */
static void seccompDenied(const char *what, long r) {
    childOut("%s %s", what, r != 0 && errno == EPERM ? "denied" : "allowed");
}

/* --test-seccomp, part 2: the calls that could reach the game through its
 * process group or an fd's owner. Raw calls with the kernel's numbers, as
 * above (F_SETSIG and F_SETOWN_EX need _GNU_SOURCE). Each one names the
 * child itself, so with no filter (the control) nothing reaches the game. */
static void testSeccompOwners(pid_t parent) {
    struct {
        int type; /* F_OWNER_PID */
        int pid;
    } owner;
    int sv[2], me = (int)getpid(), avail = 0;
    long r, pg;

    /* setpgid into the game's group; with no filter the child goes back
     * to a group of its own at once. */
    pg = syscall(SYS_getpgid, parent);
    errno = 0;
    r = pg > 0 ? syscall(SYS_setpgid, 0, (pid_t)pg) : -1;
    seccompDenied("setpgid parent", r);
    if (r == 0) (void)syscall(SYS_setpgid, 0, 0);

    /* An fd that is not open: EPERM under the filter, EBADF with none. */
#ifdef SYS_pidfd_send_signal
    errno = 0;
    r = syscall(SYS_pidfd_send_signal, -1, 0, NULL, 0u);
#else
    errno = 0;
    r = syscall(424, -1, 0, NULL, 0u);
#endif
    seccompDenied("pidfd_send_signal", r);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        childOut("socketpair failed (errno %d)", errno);
        return;
    }
    errno = 0;
    seccompDenied("fcntl setown", syscall(SYS_fcntl, sv[0], 8 /* F_SETOWN */,
                                          me));
    errno = 0;
    seccompDenied("fcntl setsig", syscall(SYS_fcntl, sv[0], 10 /* F_SETSIG */,
                                          0));
    owner.type = 1;
    owner.pid = me;
    errno = 0;
    seccompDenied("fcntl setown_ex",
                  syscall(SYS_fcntl, sv[0], 15 /* F_SETOWN_EX */, &owner));
    errno = 0;
    seccompDenied("ioctl fiosetown",
                  syscall(SYS_ioctl, sv[0], 0x8901 /* FIOSETOWN */, &me));
    errno = 0;
    seccompDenied("ioctl siocspgrp",
                  syscall(SYS_ioctl, sv[0], 0x8902 /* SIOCSPGRP */, &me));
    /* The other commands still work. */
    r = syscall(SYS_fcntl, sv[0], 3 /* F_GETFL */);
    childOut(r >= 0 ? "fcntl getfl ok" : "fcntl getfl failed");
    r = syscall(SYS_ioctl, sv[0], 0x541B /* FIONREAD */, &avail);
    childOut(r == 0 ? "ioctl fionread ok" : "ioctl fionread failed");
    (void)close(sv[0]);
    (void)close(sv[1]);
}
#endif

/* --test-seccomp, part 3: the calls that change a file's mode, owner,
 * times or extended attributes, truncate by path, System V IPC and prctl
 * PR_SET_PDEATHSIG. Raw calls with a path that is not there, an fd that
 * is not open or an IPC key or id that does not exist: EPERM under the
 * filter, another error (ENOENT, EBADF, EINVAL, ENOSYS on an older
 * kernel) with none. So the control changes nothing. */
#ifdef __linux__
#define CHILD_TEST_NOWHERE "/nonexistent-winbolo-seccomp-test"
#define CHILD_TEST_AT_FDCWD (-100)
#define CHILD_TEST_IPC_KEY 0x57b01070L /* no IPC_CREAT: never made */
static void testSeccompFiles(void) {
    const char *nowhere = CHILD_TEST_NOWHERE;
    struct timespec ts[2];
    int sig = 0, fd;
    long r;

#define TRY(what_, call_)                                                  \
    do {                                                                   \
        errno = 0;                                                         \
        seccompDenied(what_, (call_));                                     \
    } while (0)
#ifdef SYS_chmod
    TRY("chmod", syscall(SYS_chmod, nowhere, 0644));
#endif
    TRY("fchmod", syscall(SYS_fchmod, -1, 0644));
    TRY("fchmodat", syscall(SYS_fchmodat, CHILD_TEST_AT_FDCWD, nowhere,
                            0644));
    TRY("fchmodat2", syscall(452, CHILD_TEST_AT_FDCWD, nowhere, 0644, 0));
#ifdef SYS_chown
    TRY("chown", syscall(SYS_chown, nowhere, -1, -1));
#endif
#ifdef SYS_lchown
    TRY("lchown", syscall(SYS_lchown, nowhere, -1, -1));
#endif
    TRY("fchown", syscall(SYS_fchown, -1, -1, -1));
    TRY("fchownat", syscall(SYS_fchownat, CHILD_TEST_AT_FDCWD, nowhere, -1,
                            -1, 0));
    memset(ts, 0, sizeof(ts));
    TRY("utimensat", syscall(SYS_utimensat, CHILD_TEST_AT_FDCWD, nowhere, ts,
                             0));
#ifdef SYS_utimes
    TRY("utimes", syscall(SYS_utimes, nowhere, NULL));
#endif
#ifdef SYS_utime
    TRY("utime", syscall(SYS_utime, nowhere, NULL));
#endif
#ifdef SYS_futimesat
    TRY("futimesat", syscall(SYS_futimesat, CHILD_TEST_AT_FDCWD, nowhere,
                             NULL));
#endif
    TRY("setxattr", syscall(SYS_setxattr, nowhere, "user.winbolo", "1", 1,
                            0));
    TRY("lsetxattr", syscall(SYS_lsetxattr, nowhere, "user.winbolo", "1", 1,
                             0));
    TRY("fsetxattr", syscall(SYS_fsetxattr, -1, "user.winbolo", "1", 1, 0));
    TRY("removexattr", syscall(SYS_removexattr, nowhere, "user.winbolo"));
    TRY("lremovexattr", syscall(SYS_lremovexattr, nowhere, "user.winbolo"));
    TRY("fremovexattr", syscall(SYS_fremovexattr, -1, "user.winbolo"));
    TRY("setxattrat", syscall(463, CHILD_TEST_AT_FDCWD, nowhere, 0, NULL,
                              0));
    TRY("removexattrat", syscall(466, CHILD_TEST_AT_FDCWD, nowhere, 0,
                                 "user.winbolo"));
    TRY("truncate", syscall(SYS_truncate, nowhere, 0));
    /* ftruncate stays allowed: /dev/null is open for writing in the
     * sandbox, and gives EINVAL. */
    fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        TRY("ftruncate", syscall(SYS_ftruncate, fd, 0));
        (void)close(fd);
    } else {
        childOut("ftruncate untested (no /dev/null)");
    }

    TRY("shmget", syscall(SYS_shmget, CHILD_TEST_IPC_KEY, 0, 0));
    TRY("shmat", syscall(SYS_shmat, -1, NULL, 0));
    TRY("shmctl", syscall(SYS_shmctl, -1, 2 /* IPC_STAT */, NULL));
    TRY("msgget", syscall(SYS_msgget, CHILD_TEST_IPC_KEY, 0));
    TRY("msgsnd", syscall(SYS_msgsnd, -1, NULL, 0, 0));
    TRY("msgrcv", syscall(SYS_msgrcv, -1, NULL, 0, 0, 0));
    TRY("msgctl", syscall(SYS_msgctl, -1, 2 /* IPC_STAT */, NULL));
    TRY("semget", syscall(SYS_semget, CHILD_TEST_IPC_KEY, 0, 0));
    TRY("semop", syscall(SYS_semop, -1, NULL, 0));
    TRY("semctl", syscall(SYS_semctl, -1, 0, 2 /* IPC_STAT */, NULL));
    TRY("semtimedop", syscall(SYS_semtimedop, -1, NULL, 0, NULL));
#ifdef SYS_ipc
    TRY("ipc", syscall(SYS_ipc, 0, 0, 0, 0, NULL, 0));
#endif

    /* The game set SIGKILL before the exec; the same value again, so the
     * control changes nothing. */
    TRY("prctl pdeathsig", syscall(SYS_prctl, PR_SET_PDEATHSIG, SIGKILL, 0,
                                   0, 0));
    r = syscall(SYS_prctl, PR_GET_PDEATHSIG, &sig, 0, 0, 0);
    childOut(r == 0 ? "prctl get ok" : "prctl get failed");
#undef TRY
}
#endif

/* --test-seccomp: what the child may and may not do under its filter.
 * The unit test reads the lines; with no filter (its control) the denied
 * lines read "allowed". */
static int testSeccomp(void) {
#ifdef __linux__
    SDL_Thread *t[4];
    unsigned long out[4];
    int i, started = 0, r;
    lua_State *L;
    pid_t parent = getppid(), f;
    /* Raw calls: the C library declares prlimit and the CPU_* sets only
     * with _GNU_SOURCE, which the forced include comes before. */
    unsigned long long rl[2];
    unsigned long cpus[16];
    long cpuBytes;

    for (i = 0; i < 4; i++) {
        t[i] = SDL_CreateThread(seccompThread, "seccomp-test", &out[i]);
        if (t[i] != NULL) started++;
    }
    for (i = 0; i < 4; i++) {
        if (t[i] != NULL) SDL_WaitThread(t[i], NULL);
    }
    childOut("threads ok %d", started);

    L = luaL_newstate();
    if (L != NULL) {
        luaL_openlibs(L);
        /* A loop long enough for LuaJIT to compile a trace. */
        r = luaL_dostring(L, "local s = 0 for i = 1, 2000000 do "
                             "s = s + i % 7 end return s");
        childOut(r == 0 ? "lua ok" : "lua failed (%s)",
                 r == 0 ? "" : lua_tostring(L, -1));
        lua_close(L);
    } else {
        childOut("lua failed (no state)");
    }

    f = fork();
    if (f == 0) _exit(0);
    if (f > 0) (void)waitpid(f, NULL, 0);
    childOut(f < 0 ? "fork denied (errno %d)" : "fork allowed", errno);

    /* clone with SIGCHLD only: a new process, as fork makes. */
    f = (pid_t)syscall(SYS_clone, (unsigned long)SIGCHLD, 0ul, 0ul, 0ul,
                       0ul);
    if (f == 0) _exit(0);
    if (f > 0) (void)waitpid(f, NULL, 0);
    childOut(f < 0 ? "clone denied (errno %d)" : "clone allowed", errno);

    errno = 0;
    r = kill(parent, 0);
    childOut(r != 0 && errno == EPERM ? "kill parent denied"
                                      : "kill parent allowed");
    errno = 0;
    r = (int)syscall(SYS_prlimit64, parent, RLIMIT_NOFILE, NULL, rl);
    childOut(r != 0 && errno == EPERM ? "prlimit parent denied"
                                      : "prlimit parent allowed");
    r = (int)syscall(SYS_prlimit64, 0, RLIMIT_NOFILE, NULL, rl);
    childOut(r == 0 ? "prlimit self ok" : "prlimit self failed");
    errno = 0;
    /* The parent's own value, so no run changes the game's priority. */
    errno = 0;
    r = getpriority(PRIO_PROCESS, (id_t)parent);
    errno = 0;
    r = setpriority(PRIO_PROCESS, (id_t)parent, r);
    childOut(r != 0 && errno == EPERM ? "setpriority parent denied"
                                      : "setpriority parent allowed");
    errno = 0;
    r = setpriority(PRIO_PGRP, 0, 10);
    childOut(r != 0 && errno == EPERM ? "setpriority group denied"
                                      : "setpriority group allowed");
    r = setpriority(PRIO_PROCESS, 0, 10);
    childOut(r == 0 ? "setpriority self ok" : "setpriority self failed");
    memset(cpus, 0, sizeof(cpus));
    /* The raw call gives the bytes it wrote. */
    cpuBytes = syscall(SYS_sched_getaffinity, 0, sizeof(cpus), cpus);
    if (cpuBytes > 0) {
        errno = 0;
        r = (int)syscall(SYS_sched_setaffinity, parent, (size_t)cpuBytes,
                         cpus);
        childOut(r != 0 && errno == EPERM ? "affinity parent denied"
                                          : "affinity parent allowed");
        r = (int)syscall(SYS_sched_setaffinity, 0, (size_t)cpuBytes, cpus);
        childOut(r == 0 ? "affinity self ok" : "affinity self failed");
    }
    testSeccompOwners(parent);
    testSeccompFiles();
    childOut("seccomp done");
#else
    childOut("seccomp none");
#endif
    return BOT_HOST_CHILD_EXIT_OK;
}

/* The test modes; -1 when none was asked for. */
static int runTestMode(void) {
    if (coTestAllocMb >= 0) return testAlloc(coTestAllocMb);
    if (coTestSleepMs >= 0) {
        SDL_Delay((uint32_t)coTestSleepMs);
        return BOT_HOST_CHILD_EXIT_OK;
    }
    if (coTestRead != NULL) return testRead(coTestRead);
    if (coTestWrite != NULL) return testWrite(coTestWrite);
    if (coTestNetIp != NULL) return testNet(coTestNetIp, coTestNetPort);
    if (coTestRelayIp != NULL) {
        return testRelay(coTestRelayIp, coTestRelayPort);
    }
    if (coTestSalt) return testSalt();
    if (coTestSeccomp) return testSeccomp();
    return -1;
}

/* ---- stdin ----------------------------------------------------------- */

/* The game's lines, read on a thread of their own: a read of a pipe waits,
 * and the host's loop must not. */
#define CHILD_QUEUE 64
static SDL_Mutex     *inLock;
static char           inLines[CHILD_QUEUE][BOT_HOST_CHILD_LINE_MAX];
static int            inHead, inCount;
static SDL_AtomicInt  inClosed;

static int SDLCALL stdinThreadFn(void *arg) {
    char line[BOT_HOST_CHILD_LINE_MAX];
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
    childOut("%s", line);
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
    char line[BOT_HOST_CHILD_LINE_MAX];
    int exitCode = BOT_HOST_CHILD_EXIT_OK, testCode;

#ifdef _WIN32
    /* A crash ends the child; no Windows dialog keeps it alive. */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(childCrashFilter);
#endif
    if (!parseChildArgs(argc, argv)) return BOT_HOST_CHILD_EXIT_ARGS;
    /* Two layers: the Lua sandbox inside the OS sandbox. The game never
     * asks for unsafe brains, so this is not the game. */
    luaBrainsSetAllowUnsafe(0);
    if (coUnsafeAsked) {
        fprintf(stderr, "[bothost] the Bot Host child refuses "
                        "--allow-unsafe-brains: its brains always run in "
                        "the Lua sandbox\n");
        return BOT_HOST_CHILD_EXIT_ARGS;
    }
    if (coSandbox && !checkSandbox()) {
        fprintf(stderr, "[bothost] the Bot Host child is not in its "
                        "sandbox: it stops\n");
        return BOT_HOST_CHILD_EXIT_SANDBOX;
    }
    outLock = SDL_CreateMutex();
    testCode = runTestMode();
    if (testCode >= 0) return testCode;
    if (!coEmbed || coServer[0] == '\0' || coPlayer < 0 || coPlayer > 255 ||
        coCacheDir[0] == '\0') {
        fprintf(stderr, "[bothost] the child needs --embed-child, --server, "
                        "--player and --cachedir\n");
        return BOT_HOST_CHILD_EXIT_ARGS;
    }
    if (coSandbox && !startRelay()) {
        fprintf(stderr, "[bothost] the Bot Host child has no relay to the "
                        "game: it stops\n");
        return BOT_HOST_CHILD_EXIT_SANDBOX;
    }

    /* As the game sets up before its host starts. No crash reporter: it
     * would start a second program, which the job does not allow. In the
     * sandbox the machine id comes from the game, before the ticket. */
    bolo_srand((uint64_t)time(NULL) ^ (uint64_t)getpid());
    if (!coSandbox) (void)machineIdInit();
    cpuLock = SDL_CreateMutex();
    if (coSandbox) {
        /* Used only when the child cannot read the counters itself. */
        machineCpuFreeSetFallback(cpuFromGame);
        if (!machineCpuFreeCanRead()) {
            fprintf(stderr, "[bothost] the child cannot read the CPU "
                            "counters: it uses the game's\n");
        }
    }
    if (!SDL_Init(0)) {
        fprintf(stderr, "[bothost] SDL_Init failed: %s\n", SDL_GetError());
        return BOT_HOST_CHILD_EXIT_START;
    }
    /* In the sandbox only stderr (the game's log file) is written. */
    if (coSandbox) {
        wb_log_init(NULL, NULL, NULL);
    } else {
        wb_log_init("WinBolo", "WinBolo", "winbolo-bothost-child.log");
    }
    if (coCpuSpeedFile[0] != '\0') cpuSpeedStoreSetPath(coCpuSpeedFile);
    (void)cpuSpeedApplyStoredToBrains(WINBOLO_VERSION);
    /* The server's own options, before the brain threads start. */
    botHostInprocSetBrainOptions(coThreads, coHaveLuaSeed, coLuaSeed,
                                 coNoBudgetKill, coBrainTier);
    if (!serverSimBotPoolInit(coThreads > 0 ? coThreads : 0)) {
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

    /* The game's first lines: the ticket, then Max slots. */
    waitStartMs = (uint32_t)SDL_GetTicks();
    while (!haveTicket) {
        while (takeLine(line, sizeof(line))) {
            if (takeMachineLine(line)) continue;
            if (strncmp(line, "ticket ", 7) == 0) {
                haveTicket = botHostChildTicketParse(line + 7, ticket,
                                                     sizeof(ticket));
            } else if (strncmp(line, "max ", 4) == 0) {
                botHostInprocSetMax(atoi(line + 4));
            } else if (strcmp(line, "quit") == 0) {
                return BOT_HOST_CHILD_EXIT_OK;
            }
            if (haveTicket) break;
        }
        if (haveTicket) break;
        if (SDL_GetAtomicInt(&inClosed) ||
            (uint32_t)SDL_GetTicks() - waitStartMs >
                BOT_HOST_CHILD_HELLO_WAIT_MS) {
            fprintf(stderr, "[bothost] no ticket from the game\n");
            return BOT_HOST_CHILD_EXIT_ARGS;
        }
        SDL_Delay(5);
    }

    if (coSandbox) useGameMachineId();
    botHostInprocSetOfficialOnly(coOfficialOnly);
    botHostInprocSetRecord(coRecord);
    botHostInprocSetDsChild(coDsChild);
    botHostInprocSetLockstep(coLockstep);
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
    botHostChildFormatHello(BOT_HOST_CHILD_PROTOCOL,
                            botHostEmbedOfficialOnly(),
                            botHostChildBuildFullMap(),
                            botHostEmbedShippedTable(), line, sizeof(line));
    childOut("%s", line);
    fprintf(stderr, "[bothost] Bot Host child for player %d at %s:%d "
                    "(official-only %d, record %d, sandbox %d)\n",
            coPlayer, coServer, coPort, (int)coOfficialOnly, (int)coRecord,
            (int)coSandbox);

    memset(&last, 0, sizeof(last));
    botHostInprocGetStatus(&last);
    sendStatus(&last);
    lastSentMs = (uint32_t)SDL_GetTicks();
    while (!quit) {
        while (takeLine(line, sizeof(line))) {
            if (takeMachineLine(line)) {
                /* The id and salts count only before the host starts. */
            } else if (strcmp(line, "quit") == 0) {
                quit = true;
            } else if (strncmp(line, "max ", 4) == 0) {
                botHostInprocSetMax(atoi(line + 4));
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
