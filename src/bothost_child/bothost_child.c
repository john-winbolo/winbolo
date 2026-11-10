/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The built-in Bot Host as a child process: the parts with no process in
 * them (bothost_child.h). The child's arguments, the Windows command line
 * quoting, the status and hello lines, the ticket in hex, the machine id
 * lines, the relay frames and the restart counter. The unit tests run all
 * of it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#define childGetPid() ((unsigned)_getpid())
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#define childGetPid() ((unsigned)getpid())
#endif

#include "bothost_child.h"
#include "machine_id.h"

/* ---- Arguments ------------------------------------------------------- */

typedef struct {
    char        *buf;
    size_t       size;
    size_t       used;
    const char **argv;
    int          max;
    int          argc;
    bool         ok;
} ArgBuild;

static void argAdd(ArgBuild *b, const char *s) {
    size_t n;
    if (!b->ok) return;
    if (s == NULL) s = "";
    n = strlen(s) + 1;
    /* One place is kept for the NULL at the end. */
    if (b->argc + 1 >= b->max || b->used + n > b->size) {
        b->ok = false;
        return;
    }
    memcpy(b->buf + b->used, s, n);
    b->argv[b->argc++] = b->buf + b->used;
    b->used += n;
}

static void argAddInt(ArgBuild *b, const char *opt, long v) {
    char num[32];
    snprintf(num, sizeof(num), "%ld", v);
    argAdd(b, opt);
    argAdd(b, num);
}

int botHostChildBuildArgs(const BotHostChildParams *p, const char *exe,
                          char *buf, size_t bufSize,
                          const char **argv, int maxArgv) {
    ArgBuild b;
    if (p == NULL || exe == NULL || buf == NULL || argv == NULL ||
        maxArgv < 2) {
        return -1;
    }
    memset(&b, 0, sizeof(b));
    b.buf = buf;
    b.size = bufSize;
    b.argv = argv;
    b.max = maxArgv;
    b.ok = true;

    argAdd(&b, exe);
    argAdd(&b, "--embed-child");
    /* The child ends when the game's pipe closes, on every system. */
    argAdd(&b, "--exit-on-stdin-close");
    argAdd(&b, "--server");
    argAdd(&b, p->serverHost);
    argAddInt(&b, "--port", (long)p->serverPort);
    argAddInt(&b, "--player", (long)p->player);
    argAdd(&b, "--brain");
    argAdd(&b, p->brain);
    argAdd(&b, "--name");
    argAdd(&b, p->name);
    argAdd(&b, "--cachedir");
    argAdd(&b, p->cacheDir);
    argAddInt(&b, "--slots", (long)(p->slots > 0 ? p->slots : 0));
    if (p->officialOnly) argAdd(&b, "--official-only");
    if (p->record) argAdd(&b, "--record");
    if (p->recordMinFreeGb > 0.0) {
        char num[32];
        snprintf(num, sizeof(num), "%.3f", p->recordMinFreeGb);
        argAdd(&b, "--record-min-free-gb");
        argAdd(&b, num);
    }
    if (p->cpuSpeedFile != NULL && p->cpuSpeedFile[0] != '\0') {
        argAdd(&b, "--cpu-speed-file");
        argAdd(&b, p->cpuSpeedFile);
    }
    if (!b.ok) return -1;
    argv[b.argc] = NULL;
    return b.argc;
}

/* The rules of CommandLineToArgvW: backslashes are plain unless a quote
 * follows them; then each pair is one backslash, and an odd one makes the
 * quote plain. */
bool botHostChildQuoteWinArg(const char *arg, char *out, size_t outSize,
                             size_t *len) {
    size_t at = *len;
    const char *c;
    bool quote = arg[0] == '\0' || strpbrk(arg, " \t\n\v\"") != NULL;

#define PUT(ch)                                                          \
    do {                                                                 \
        if (at + 1 >= outSize) return false;                             \
        out[at++] = (ch);                                                \
    } while (0)

    if (!quote) {
        for (c = arg; *c != '\0'; c++) PUT(*c);
    } else {
        PUT('"');
        for (c = arg;; c++) {
            size_t slashes = 0, i;
            while (*c == '\\') {
                slashes++;
                c++;
            }
            if (*c == '\0') {
                /* Before the closing quote: every backslash doubled. */
                for (i = 0; i < slashes * 2; i++) PUT('\\');
                break;
            }
            if (*c == '"') {
                for (i = 0; i < slashes * 2 + 1; i++) PUT('\\');
                PUT('"');
            } else {
                for (i = 0; i < slashes; i++) PUT('\\');
                PUT(*c);
            }
        }
        PUT('"');
    }
#undef PUT
    out[at] = '\0';
    *len = at;
    return true;
}

/* ---- The line codec -------------------------------------------------- */

void botHostChildFormatStatus(const BotHostChildStatus *st, char *out,
                              size_t outSize) {
    snprintf(out, outSize, "status %d %d %d %d %d %d %d",
             st->running ? 1 : 0, st->connected ? 1 : 0,
             st->refused ? 1 : 0, st->slots, st->bots,
             st->sameServer ? 1 : 0, st->consentAsking ? 1 : 0);
}

bool botHostChildParseStatus(const char *line, BotHostChildStatus *out) {
    int v[7];
    if (line == NULL || out == NULL) return false;
    if (sscanf(line, "status %d %d %d %d %d %d %d", &v[0], &v[1], &v[2],
               &v[3], &v[4], &v[5], &v[6]) != 7) {
        return false;
    }
    if (v[3] < 0 || v[4] < 0) return false;
    out->running = v[0] != 0;
    out->connected = v[1] != 0;
    out->refused = v[2] != 0;
    out->slots = v[3];
    out->bots = v[4];
    out->sameServer = v[5] != 0;
    out->consentAsking = v[6] != 0;
    return true;
}

void botHostChildFormatHello(int protocol, bool officialOnly, bool fullMap,
                             bool shippedTable, char *out, size_t outSize) {
    snprintf(out, outSize, "hello %d official=%d fullmap=%d table=%d",
             protocol, officialOnly ? 1 : 0, fullMap ? 1 : 0,
             shippedTable ? 1 : 0);
}

bool botHostChildParseHello(const char *line, int *protocol,
                            bool *officialOnly, bool *fullMap,
                            bool *shippedTable) {
    int proto, off, full = 0, table = 0, got;
    if (line == NULL) return false;
    got = sscanf(line, "hello %d official=%d fullmap=%d table=%d", &proto,
                 &off, &full, &table);
    if (got < 2) return false;
    /* Protocol 1 had no fullmap=; the caller refuses that protocol. */
    if (got < 3) full = -1;
    /* No table=: a child with no table. */
    if (got < 4) table = 0;
    if (protocol != NULL) *protocol = proto;
    if (officialOnly != NULL) *officialOnly = off != 0;
    if (fullMap != NULL) *fullMap = full == 1;
    if (shippedTable != NULL) *shippedTable = table == 1;
    return true;
}

bool botHostChildHelloMatches(int protocol, bool officialOnly, bool fullMap,
                              bool shippedTable, bool wantOfficial,
                              bool wantTable) {
    if (protocol != BOT_HOST_CHILD_PROTOCOL) return false;
    if (officialOnly != wantOfficial) return false;
    if (fullMap != botHostChildBuildFullMap()) return false;
    if (wantOfficial && wantTable && !shippedTable) return false;
    return true;
}

bool botHostChildBuildFullMap(void) {
#if defined(WINBOLO_BOTHOST_ALLOW_FULL_MAP)
    return true;
#else
    return false;
#endif
}

void botHostChildTicketHex(const uint8_t *ticket, size_t len, char *out,
                           size_t outSize) {
    static const char digits[] = "0123456789abcdef";
    size_t i;
    if (outSize == 0) return;
    for (i = 0; i < len && 2 * i + 2 < outSize; i++) {
        out[2 * i] = digits[ticket[i] >> 4];
        out[2 * i + 1] = digits[ticket[i] & 15];
    }
    out[2 * i] = '\0';
}

static int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool botHostChildTicketParse(const char *hex, uint8_t *ticket, size_t len) {
    size_t i;
    if (hex == NULL || strlen(hex) < 2 * len) return false;
    for (i = 0; i < len; i++) {
        int hi = hexValue(hex[2 * i]);
        int lo = hexValue(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        ticket[i] = (uint8_t)((hi << 4) | lo);
    }
    /* Nothing but line ends after the digits. */
    for (i = 2 * len; hex[i] != '\0'; i++) {
        if (hex[i] != '\r' && hex[i] != '\n') return false;
    }
    return true;
}

int botHostChildMachineLines(char lines[][BOT_HOST_CHILD_LINE_MAX],
                             int maxLines) {
    uint8_t id[MACHINE_ID_LEN];
    static MachineSaltMemory salts; /* large: not on the stack */
    char hex[2 * MACHINE_ID_LEN + 1];
    int n = 0, i;
    if (lines == NULL || maxLines < 1 || !machineIdGet(id)) return 0;
    botHostChildTicketHex(id, sizeof(id), hex, sizeof(hex));
    snprintf(lines[n++], BOT_HOST_CHILD_LINE_MAX, "machineid %s", hex);
    memset(id, 0, sizeof(id));
    machineIdSaltSnapshot(&salts);
    for (i = 0; i < salts.count && n < maxLines;) {
        size_t len = (size_t)snprintf(lines[n], BOT_HOST_CHILD_LINE_MAX,
                                      "salts");
        int k;
        for (k = 0; k < BOT_HOST_CHILD_SALTS_PER_LINE && i < salts.count;
             k++, i++) {
            char sh[2 * MACHINE_SALT_LEN + 1];
            botHostChildTicketHex(salts.salt[i], MACHINE_SALT_LEN, sh,
                                  sizeof(sh));
            len += (size_t)snprintf(lines[n] + len,
                                    BOT_HOST_CHILD_LINE_MAX - len, " %s",
                                    sh);
        }
        n++;
    }
    return n;
}

/* ---- The relay codec ------------------------------------------------- */

size_t botHostRelayEncode(uint8_t type, uint8_t sock, uint32_t ipNet,
                          uint16_t portNet, const uint8_t *data, size_t len,
                          uint8_t *out, size_t outSize) {
    size_t body = 0;
    if (out == NULL || sock >= BOT_HOST_RELAY_MAX_SOCKS) return 0;
    if (type == BOT_HOST_RELAY_SEND || type == BOT_HOST_RELAY_RECV) {
        if (len > BOT_HOST_RELAY_MAX_DATAGRAM || (len > 0 && data == NULL)) {
            return 0;
        }
        body = BOT_HOST_RELAY_ADDR + len;
    } else if (type != BOT_HOST_RELAY_OPEN && type != BOT_HOST_RELAY_CLOSE) {
        return 0;
    }
    if (outSize < BOT_HOST_RELAY_HDR + body) return 0;
    out[0] = type;
    out[1] = sock;
    out[2] = (uint8_t)(body & 0xFF);
    out[3] = (uint8_t)(body >> 8);
    if (body > 0) {
        /* Both already in network order: copied as they are in memory. */
        memcpy(out + BOT_HOST_RELAY_HDR, &ipNet, 4);
        memcpy(out + BOT_HOST_RELAY_HDR + 4, &portNet, 2);
        if (len > 0) {
            memcpy(out + BOT_HOST_RELAY_HDR + BOT_HOST_RELAY_ADDR, data, len);
        }
    }
    return BOT_HOST_RELAY_HDR + body;
}

bool botHostRelayParseHeader(const uint8_t *hdr, uint8_t *type,
                             uint8_t *sock, uint16_t *bodyLen) {
    uint16_t len;
    if (hdr == NULL) return false;
    len = (uint16_t)(hdr[2] | (hdr[3] << 8));
    if (hdr[1] >= BOT_HOST_RELAY_MAX_SOCKS) return false;
    switch (hdr[0]) {
    case BOT_HOST_RELAY_OPEN:
    case BOT_HOST_RELAY_CLOSE:
        if (len != 0) return false;
        break;
    case BOT_HOST_RELAY_SEND:
    case BOT_HOST_RELAY_RECV:
        if (len < BOT_HOST_RELAY_ADDR ||
            len > BOT_HOST_RELAY_ADDR + BOT_HOST_RELAY_MAX_DATAGRAM) {
            return false;
        }
        break;
    default:
        return false;
    }
    if (type != NULL) *type = hdr[0];
    if (sock != NULL) *sock = hdr[1];
    if (bodyLen != NULL) *bodyLen = len;
    return true;
}

bool botHostRelayParseBody(const uint8_t *body, uint16_t bodyLen,
                           uint32_t *ipNet, uint16_t *portNet,
                           const uint8_t **data, size_t *len) {
    if (body == NULL || bodyLen < BOT_HOST_RELAY_ADDR) return false;
    if (ipNet != NULL) memcpy(ipNet, body, 4);
    if (portNet != NULL) memcpy(portNet, body + 4, 2);
    if (data != NULL) *data = body + BOT_HOST_RELAY_ADDR;
    if (len != NULL) *len = (size_t)bodyLen - BOT_HOST_RELAY_ADDR;
    return true;
}

/* ---- The sandbox's paths --------------------------------------------- */

bool botHostChildSandboxAdd(const char **list, int *count, const char *path) {
    if (list == NULL || count == NULL || path == NULL || path[0] == '\0') {
        return false;
    }
    if (*count >= BOT_HOST_SANDBOX_MAX_PATHS) return false;
    list[(*count)++] = path;
    return true;
}

/* ---- The restart counter --------------------------------------------- */

void botHostChildRestartInit(BotHostChildRestart *r) {
    if (r == NULL) return;
    memset(r, 0, sizeof(*r));
}

bool botHostChildRestartAfterExit(BotHostChildRestart *r, uint32_t nowMs) {
    if (r == NULL) return false;
    if (r->restarts >= BOT_HOST_CHILD_MAX_RESTARTS) {
        r->waiting = false;
        return false;
    }
    r->restarts++;
    r->waiting = true;
    r->waitUntilMs = nowMs + BOT_HOST_CHILD_RESTART_WAIT_MS;
    return true;
}

bool botHostChildRestartDue(BotHostChildRestart *r, uint32_t nowMs) {
    if (r == NULL || !r->waiting) return false;
    if ((int32_t)(nowMs - r->waitUntilMs) < 0) return false;
    r->waiting = false;
    return true;
}

/* ---- A copy of the install's files ----------------------------------- */

typedef struct {
    const char *src;
    const char *dst;
    bool        recurse;
    const char *exeName;  /* not NULL: only it and the DLLs */
    bool        ok;
} CopyTree;

bool botHostChildCopyWanted(const char *name, const char *exeName) {
    size_t n;
    if (name == NULL) return false;
    if (exeName != NULL && SDL_strcasecmp(name, exeName) == 0) return true;
    n = strlen(name);
    return n > 4 && SDL_strcasecmp(name + n - 4, ".dll") == 0;
}

/* Sets a file's modify time (SDL_Time, ns since 1970). */
static bool setModifyTime(const char *path, SDL_Time t) {
#ifdef _WIN32
    wchar_t wpath[1100];
    HANDLE h;
    ULARGE_INTEGER u;
    FILETIME ft;
    BOOL ok;
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath,
                            (int)(sizeof(wpath) / sizeof(wpath[0]))) == 0) {
        return false;
    }
    h = CreateFileW(wpath, FILE_WRITE_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    /* FILETIME: 100 ns since 1601. */
    u.QuadPart = (ULONGLONG)(t / 100) + 116444736000000000ull;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    ok = SetFileTime(h, NULL, NULL, &ft);
    CloseHandle(h);
    return ok != 0;
#else
    struct timespec ts[2];
    ts[0].tv_sec = 0;
    ts[0].tv_nsec = UTIME_OMIT;
    ts[1].tv_sec = (time_t)(t / 1000000000);
    ts[1].tv_nsec = (long)(t % 1000000000);
    return utimensat(AT_FDCWD, path, ts, 0) == 0;
#endif
}

/* True when the copy at to is the source file si describes: the same
 * size and the same modify time (the copy takes the source's time). */
static bool copyIsCurrent(const char *to, const SDL_PathInfo *si) {
    SDL_PathInfo di;
    return SDL_GetPathInfo(to, &di) && di.type == SDL_PATHTYPE_FILE &&
           di.size == si->size && di.modify_time == si->modify_time;
}

/* Copies from to to through a temporary file of this process, then a
 * rename, so no child of another game starts a half-written file. The
 * copy takes the source's modify time. */
static bool copyFile(const char *from, const char *to,
                     const SDL_PathInfo *si) {
    SDL_IOStream *in = SDL_IOFromFile(from, "rb");
    SDL_IOStream *out;
    char buf[65536], tmp[1100];
    bool ok = true;
    if (in == NULL) return false;
    SDL_snprintf(tmp, sizeof(tmp), "%s.%u.tmp", to, childGetPid());
    out = SDL_IOFromFile(tmp, "wb");
    if (out == NULL) {
        SDL_CloseIO(in);
        return false;
    }
    for (;;) {
        size_t n = SDL_ReadIO(in, buf, sizeof(buf));
        if (n == 0) break;
        if (SDL_WriteIO(out, buf, n) != n) {
            ok = false;
            break;
        }
    }
    SDL_CloseIO(in);
    if (!SDL_CloseIO(out)) ok = false;
    if (ok && !setModifyTime(tmp, si->modify_time)) ok = false;
    if (ok && !SDL_RenamePath(tmp, to)) {
        /* Another game may have put the same file there first (or runs
         * the old one): that copy counts when it is current. */
        ok = copyIsCurrent(to, si);
    }
    (void)SDL_RemovePath(tmp);
    return ok;
}

/* Removes path and, for a folder, all in it. */
static bool removeTree(const char *path);

typedef struct {
    const char *base;
    bool        ok;
} RemoveTree;

static SDL_EnumerationResult SDLCALL removeOne(void *ud, const char *dir,
                                               const char *name) {
    RemoveTree *rt = (RemoveTree *)ud;
    char p[1100];
    (void)dir;
    if (SDL_strcmp(name, ".") == 0 || SDL_strcmp(name, "..") == 0) {
        return SDL_ENUM_CONTINUE;
    }
    SDL_snprintf(p, sizeof(p), "%s/%s", rt->base, name);
    if (!removeTree(p)) rt->ok = false;
    return SDL_ENUM_CONTINUE;
}

static bool removeTree(const char *path) {
    SDL_PathInfo pi;
    RemoveTree rt;
    if (!SDL_GetPathInfo(path, &pi)) return true;
    rt.base = path;
    rt.ok = true;
    if (pi.type == SDL_PATHTYPE_DIRECTORY) {
        (void)SDL_EnumerateDirectory(path, removeOne, &rt);
    }
    if (!SDL_RemovePath(path)) rt.ok = false;
    return rt.ok;
}

typedef struct {
    const char *src;
    const char *dst;
    bool        ok;
} PruneTree;

/* An entry of the copy with no source entry of the same name and kind is
 * removed: a brain file taken out of the install leaves the copy too. */
static SDL_EnumerationResult SDLCALL pruneOne(void *ud, const char *dir,
                                              const char *name) {
    PruneTree *pt = (PruneTree *)ud;
    char from[1100], to[1100];
    SDL_PathInfo si, di;
    size_t n;
    (void)dir;
    if (SDL_strcmp(name, ".") == 0 || SDL_strcmp(name, "..") == 0) {
        return SDL_ENUM_CONTINUE;
    }
    /* Another game's temporary file: not this copy's to remove. */
    n = strlen(name);
    if (n > 4 && SDL_strcmp(name + n - 4, ".tmp") == 0) {
        return SDL_ENUM_CONTINUE;
    }
    SDL_snprintf(from, sizeof(from), "%s/%s", pt->src, name);
    SDL_snprintf(to, sizeof(to), "%s/%s", pt->dst, name);
    if (!SDL_GetPathInfo(to, &di)) return SDL_ENUM_CONTINUE;
    if (SDL_GetPathInfo(from, &si) && si.type == di.type) {
        return SDL_ENUM_CONTINUE;
    }
    if (!removeTree(to)) pt->ok = false;
    return SDL_ENUM_CONTINUE;
}

static SDL_EnumerationResult SDLCALL copyOne(void *ud, const char *dir,
                                             const char *name) {
    CopyTree *ct = (CopyTree *)ud;
    char from[1024], to[1024];
    SDL_PathInfo si;
    (void)dir;
    if (SDL_strcmp(name, ".") == 0 || SDL_strcmp(name, "..") == 0) {
        return SDL_ENUM_CONTINUE;
    }
    SDL_snprintf(from, sizeof(from), "%s/%s", ct->src, name);
    SDL_snprintf(to, sizeof(to), "%s/%s", ct->dst, name);
    if (!SDL_GetPathInfo(from, &si)) return SDL_ENUM_CONTINUE;
    if (si.type == SDL_PATHTYPE_DIRECTORY) {
        if (ct->exeName == NULL && ct->recurse &&
            !botHostChildCopyTree(from, to, true)) {
            ct->ok = false;
        }
        return SDL_ENUM_CONTINUE;
    }
    if (si.type != SDL_PATHTYPE_FILE) return SDL_ENUM_CONTINUE;
    if (ct->exeName != NULL && !botHostChildCopyWanted(name, ct->exeName)) {
        return SDL_ENUM_CONTINUE;
    }
    /* Reused only when it is the same file: the same size and the same
     * time (an installer may put back a file of the same size with an
     * older time). */
    if (copyIsCurrent(to, &si)) return SDL_ENUM_CONTINUE;
    if (!copyFile(from, to, &si)) ct->ok = false;
    return SDL_ENUM_CONTINUE;
}

bool botHostChildCopyTree(const char *src, const char *dst, bool recurse) {
    CopyTree ct;
    PruneTree pt;
    SDL_PathInfo si;
    if (src == NULL || dst == NULL) return false;
    if (!SDL_GetPathInfo(src, &si) || si.type != SDL_PATHTYPE_DIRECTORY) {
        return false;
    }
    if (!SDL_CreateDirectory(dst)) return false;
    /* First what the source no longer has, then the copy. */
    pt.src = src;
    pt.dst = dst;
    pt.ok = true;
    (void)SDL_EnumerateDirectory(dst, pruneOne, &pt);
    ct.src = src;
    ct.dst = dst;
    ct.recurse = recurse;
    ct.exeName = NULL;
    ct.ok = pt.ok;
    if (!SDL_EnumerateDirectory(src, copyOne, &ct)) return false;
    return ct.ok;
}

bool botHostChildCopyProgramFiles(const char *src, const char *dst,
                                  const char *exeName) {
    CopyTree ct;
    if (src == NULL || dst == NULL || exeName == NULL) return false;
    if (!SDL_CreateDirectory(dst)) return false;
    ct.src = src;
    ct.dst = dst;
    ct.recurse = false;
    ct.exeName = exeName;
    ct.ok = true;
    if (!SDL_EnumerateDirectory(src, copyOne, &ct)) return false;
    return ct.ok;
}

/* ---- The failure rule and the environment ---------------------------- */

BotHostChildStart botHostChildFailRule(BotHostChildFail f, bool sandbox) {
    /* No brain runs without the OS sandbox unless the user switched the
     * sandbox off: so in the sandbox, every failure means no bots. */
    if (sandbox) return BOT_HOST_CHILD_START_NO_BOTS;
    if (f == BOT_HOST_CHILD_FAIL_RESTART_LIMIT) {
        return BOT_HOST_CHILD_START_NO_BOTS;
    }
    return BOT_HOST_CHILD_START_INPROC;
}

bool botHostChildEnvKeep(const char *entry) {
    static const char *const names[] = {
        "SystemRoot", "windir", "SystemDrive", "NUMBER_OF_PROCESSORS",
        "PROCESSOR_ARCHITECTURE", "PROCESSOR_IDENTIFIER", "PROCESSOR_LEVEL",
        "PROCESSOR_REVISION", "LANG", "LANGUAGE", "TZ",
    };
    const char *eq;
    size_t n, i;
    if (entry == NULL) return false;
    eq = strchr(entry, '=');
    /* Windows keeps "=C:=C:\..." entries (the drives' folders): no name. */
    if (eq == NULL || eq == entry) return false;
    n = (size_t)(eq - entry);
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strlen(names[i]) == n && SDL_strncasecmp(entry, names[i], n) == 0) {
            return true;
        }
    }
    if (n > 3 && SDL_strncasecmp(entry, "LC_", 3) == 0) return true;
    if (n > 8 && SDL_strncasecmp(entry, "WINBOLO_", 8) == 0) return true;
    return false;
}
