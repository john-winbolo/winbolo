/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The built-in Bot Host's child process on Windows (bothost_child.h).
 *
 * The child is made suspended, put in a job object of its own, and only
 * then let run, so it never runs a moment outside the job. The job:
 *   - KILL_ON_JOB_CLOSE: the child ends when the game ends, even when the
 *     game crashes (the game holds the job's only handle);
 *   - ACTIVE_PROCESS = 1: the child cannot start other programs;
 *   - PROCESS_MEMORY: the child cannot commit more than the limit;
 *   - DIE_ON_UNHANDLED_EXCEPTION: a crash ends the child with no
 *     Windows error dialog that would keep it alive;
 *   - a CPU rate hard cap, as a part of the whole machine.
 * The child starts at BELOW_NORMAL priority with no console window. Only
 * its stdin, stdout and log file handles are given to it (and the two
 * relay pipes in the sandbox).
 *
 * The file sandbox: the child runs in an AppContainer with no
 * capabilities, so it has no network (not even loopback) and reaches only
 * what its container SID was granted, the system's own files (which allow
 * ALL APPLICATION PACKAGES) and the handles it was given. The profile is
 * one per install ("WinBolo.BotHost." and a hash of the program's
 * folder), made once and used again. botHostChildSandboxPrepare grants
 * the SID read on the program's folder and full use of the cache folder
 * with inherited ACL entries, once: a folder that already has the entry
 * is not changed again.
 */
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <userenv.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bothost_child.h"

#ifdef _MSC_VER
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "advapi32.lib")
#endif

/* The relay pipes' size: a few hundred datagrams. */
#define RELAY_PIPE_BYTES (256u * 1024u)
/* The child's stdin: room for the lines the game writes before the child
 * starts to read (the machine id, the ticket), and for the CPU lines of a
 * child that is slow for a while. */
#define STDIN_PIPE_BYTES (64u * 1024u)

/* Read: list, read and run. Write: list, read, change and delete, but not
 * run (no program written there can start), and not the ACL or the owner.
 *
 * Note: the grants stay after the game ends, and so does the container's
 * profile; the next run uses them again. The container's SID can do
 * nothing outside the granted folders, so a grant left behind gives no
 * other program any right. A later switch could remove them
 * (DeleteAppContainerProfile and the ACEs). */
#define GRANT_READ  (FILE_GENERIC_READ | FILE_GENERIC_EXECUTE)
#define GRANT_WRITE (FILE_GENERIC_READ | FILE_GENERIC_WRITE | DELETE |      \
                     FILE_DELETE_CHILD)

struct BotHostChildProc {
    HANDLE process;
    HANDLE job;      /* NULL when the job could not be made */
    HANDLE stdinW;   /* the game's end of the child's stdin */
    HANDLE stdoutR;  /* the game's end of the child's stdout */
    HANDLE relayW;   /* the game's end of the relay to the child; or NULL */
    HANDLE relayR;   /* the game's end of the relay from the child */
    DWORD  pid;
};

static void setErr(char *err, size_t errSize, const char *what) {
    if (err != NULL && errSize > 0) {
        snprintf(err, errSize, "%s (error %lu)", what,
                 (unsigned long)GetLastError());
    }
}

static wchar_t *utf8ToWide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w;
    if (n <= 0) return NULL;
    w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (w == NULL) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
        free(w);
        return NULL;
    }
    return w;
}

/* ---- The AppContainer ------------------------------------------------ */

void botHostChildContainerName(char *out, size_t outSize) {
    wchar_t path[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, path, (DWORD)(sizeof(path) /
                                                     sizeof(path[0])));
    uint64_t h = 1469598103934665603ull; /* FNV-1a */
    DWORD i, cut = 0;
    if (n == 0 || n >= sizeof(path) / sizeof(path[0])) n = 0;
    for (i = 0; i < n; i++) {
        if (path[i] == L'\\' || path[i] == L'/') cut = i;
    }
    for (i = 0; i < cut; i++) {
        wchar_t c = path[i];
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
        if (c == L'/') c = L'\\';
        h ^= (uint64_t)(c & 0xFF);
        h *= 1099511628211ull;
        h ^= (uint64_t)((c >> 8) & 0xFF);
        h *= 1099511628211ull;
    }
    snprintf(out, outSize, "WinBolo.BotHost.%016llx", (unsigned long long)h);
}

/* The container's SID, the profile made when it is not there yet. Free it
 * with FreeSid. */
static PSID containerSid(char *why, size_t whySize) {
    char name[64];
    wchar_t wname[64];
    PSID sid = NULL;
    HRESULT hr;
    botHostChildContainerName(name, sizeof(name));
    if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 64) <= 0) {
        return NULL;
    }
    hr = CreateAppContainerProfile(wname, L"WinBolo Bot Host",
                                   L"The WinBolo Bot Host child's sandbox",
                                   NULL, 0, &sid);
    if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
        sid = NULL;
        hr = DeriveAppContainerSidFromAppContainerName(wname, &sid);
    }
    if (FAILED(hr) || sid == NULL) {
        if (why != NULL && whySize > 0) {
            snprintf(why, whySize, "the AppContainer profile %s failed "
                                   "(0x%08lx)", name, (unsigned long)hr);
        }
        return NULL;
    }
    return sid;
}

/* True when acl has the one entry this grant makes: sid, exactly rights
 * (no more, so a wider grant from an older version is replaced), set on
 * this object (not inherited) and, for a folder, inherited by all in it. */
static bool aclHasGrant(PACL acl, PSID sid, DWORD rights, bool dir) {
    const BYTE want = dir ? (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE) : 0;
    const BYTE flags = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE |
                       INHERIT_ONLY_ACE | INHERITED_ACE;
    DWORD i;
    if (acl == NULL) return false;
    for (i = 0; i < acl->AceCount; i++) {
        ACCESS_ALLOWED_ACE *ace = NULL;
        if (!GetAce(acl, i, (LPVOID *)&ace) || ace == NULL) continue;
        if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
        if (!EqualSid((PSID)&ace->SidStart, sid)) continue;
        if (ace->Mask == rights && (ace->Header.AceFlags & flags) == want) {
            return true;
        }
    }
    return false;
}

/* Grants sid rights on path (inherited by all in it for a folder).
 * ERROR_SUCCESS when it has them now. */
static DWORD grantPath(const char *path, PSID sid, DWORD rights, bool dir) {
    wchar_t *wpath = utf8ToWide(path);
    PACL oldAcl = NULL, newAcl = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    EXPLICIT_ACCESS_W ea;
    DWORD rc;
    if (wpath == NULL) return ERROR_INVALID_PARAMETER;
    rc = GetNamedSecurityInfoW(wpath, SE_FILE_OBJECT,
                               DACL_SECURITY_INFORMATION, NULL, NULL,
                               &oldAcl, NULL, &sd);
    if (rc == ERROR_SUCCESS && !aclHasGrant(oldAcl, sid, rights, dir)) {
        memset(&ea, 0, sizeof(ea));
        ea.grfAccessPermissions = rights;
        /* SET_ACCESS: this one entry replaces any other entry this SID
         * has on the object, so the grant is only what is asked. */
        ea.grfAccessMode = SET_ACCESS;
        ea.grfInheritance = dir ? SUB_CONTAINERS_AND_OBJECTS_INHERIT
                                : NO_INHERITANCE;
        ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        ea.Trustee.ptstrName = (LPWSTR)sid;
        rc = SetEntriesInAclW(1, &ea, oldAcl, &newAcl);
        if (rc == ERROR_SUCCESS) {
            rc = SetNamedSecurityInfoW(wpath, SE_FILE_OBJECT,
                                       DACL_SECURITY_INFORMATION, NULL, NULL,
                                       newAcl, NULL);
        }
    }
    if (newAcl != NULL) LocalFree(newAcl);
    if (sd != NULL) LocalFree(sd);
    free(wpath);
    return rc;
}

bool botHostChildSandboxAvailable(char *why, size_t whySize) {
    PSID sid = containerSid(why, whySize);
    if (sid == NULL) return false;
    FreeSid(sid);
    return true;
}

int botHostChildLandlockAbi(void) {
    return -1;
}

void botHostChildTestSeccompOnly(bool on) { (void)on; }

bool botHostChildSandboxPrepare(const BotHostChildSandbox *sb,
                                unsigned *readFailed, char *why,
                                size_t whySize) {
    PSID sid;
    int i;
    DWORD rc;
    if (readFailed != NULL) *readFailed = 0;
    if (sb == NULL || !sb->on) return true;
    sid = containerSid(why, whySize);
    if (sid == NULL) return false;
    for (i = 0; i < sb->readDirCount; i++) {
        rc = grantPath(sb->readDirs[i], sid, GRANT_READ, true);
        if (rc != ERROR_SUCCESS && readFailed != NULL) {
            *readFailed |= 1u << i;
        }
    }
    for (i = 0; i < sb->readFileCount; i++) {
        /* A file that cannot be granted is only not read (the CPU speed
         * result: the child measures again). */
        (void)grantPath(sb->readFiles[i], sid, GRANT_READ, false);
    }
    for (i = 0; i < sb->writeDirCount; i++) {
        rc = grantPath(sb->writeDirs[i], sid, GRANT_WRITE, true);
        if (rc != ERROR_SUCCESS) {
            if (why != NULL && whySize > 0) {
                snprintf(why, whySize, "the sandbox cannot write in %s "
                                       "(error %lu)", sb->writeDirs[i],
                         (unsigned long)rc);
            }
            FreeSid(sid);
            return false;
        }
    }
    FreeSid(sid);
    return true;
}

/* ---- The job --------------------------------------------------------- */

static HANDLE makeJob(const BotHostChildLimits *lim, char *err,
                      size_t errSize) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION ext;
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (job == NULL) {
        setErr(err, errSize, "CreateJobObject failed");
        return NULL;
    }
    memset(&ext, 0, sizeof(ext));
    ext.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
        JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
        JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    ext.BasicLimitInformation.ActiveProcessLimit = 1;
    if (lim != NULL && lim->memMb > 0) {
        ext.BasicLimitInformation.LimitFlags |=
            JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        ext.ProcessMemoryLimit = (SIZE_T)lim->memMb * (SIZE_T)1024 * 1024;
    }
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                 &ext, sizeof(ext))) {
        setErr(err, errSize, "the job's limits were refused");
        CloseHandle(job);
        return NULL;
    }
    if (lim != NULL && lim->cpuPct > 0 && lim->cpuPct < 100) {
        JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu;
        memset(&cpu, 0, sizeof(cpu));
        cpu.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE |
                           JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
        /* In 1/100 of a percent of all the machine's processors. */
        cpu.CpuRate = (DWORD)lim->cpuPct * 100u;
        if (!SetInformationJobObject(job, JobObjectCpuRateControlInformation,
                                     &cpu, sizeof(cpu))) {
            /* The memory limit and the rest still hold. Note: Windows
             * refuses a CPU rate in some nested jobs (a game started in a
             * job of its own, as some launchers do); the child then has
             * only its lower priority, as on Linux and macOS. */
            fprintf(stderr, "[bothost] the child's CPU cap was refused "
                            "(error %lu); it runs without it\n",
                    (unsigned long)GetLastError());
        }
    }
    return job;
}

/* The sandbox's arguments after argv, with the relay's handles. */
static bool addSandboxArgs(char *cmd, size_t cmdSize, size_t *cmdLen,
                           const BotHostChildSandbox *sb, HANDLE relayIn,
                           HANDLE relayOut) {
    char num[2][32];
    const char *extra[3 + 2 * 3 * BOT_HOST_SANDBOX_MAX_PATHS + 3];
    int n = 0, i;
    extra[n++] = "--sandbox";
    for (i = 0; i < sb->readDirCount; i++) {
        extra[n++] = "--sandbox-read";
        extra[n++] = sb->readDirs[i];
    }
    for (i = 0; i < sb->readFileCount; i++) {
        extra[n++] = "--sandbox-file";
        extra[n++] = sb->readFiles[i];
    }
    for (i = 0; i < sb->writeDirCount; i++) {
        extra[n++] = "--sandbox-write";
        extra[n++] = sb->writeDirs[i];
    }
    snprintf(num[0], sizeof(num[0]), "%llu",
             (unsigned long long)(uintptr_t)relayIn);
    snprintf(num[1], sizeof(num[1]), "%llu",
             (unsigned long long)(uintptr_t)relayOut);
    extra[n++] = "--relay";
    extra[n++] = num[0];
    extra[n++] = num[1];
    for (i = 0; i < n; i++) {
        if (*cmdLen + 2 >= cmdSize) return false;
        cmd[(*cmdLen)++] = ' ';
        cmd[*cmdLen] = '\0';
        if (!botHostChildQuoteWinArg(extra[i], cmd, cmdSize, cmdLen)) {
            return false;
        }
    }
    return true;
}

static void closeIf(HANDLE *h) {
    if (*h != NULL && *h != INVALID_HANDLE_VALUE) CloseHandle(*h);
    *h = NULL;
}

/* Appends s (n wide chars) and a zero to the block at *len; false when it
 * is full. */
static bool envPut(wchar_t *out, size_t cap, size_t *len, const wchar_t *s,
                   size_t n) {
    if (*len + n + 2 > cap) return false;
    memcpy(out + *len, s, n * sizeof(wchar_t));
    *len += n;
    out[(*len)++] = L'\0';
    return true;
}

/* The user's folder names. CreateProcess for an AppContainer reads them
 * from the new block (it puts the container's own folders in their place)
 * and fails with error 203 when they are missing. They hold paths only. */
static bool childEnvKeepWin(const char *entry) {
    static const char *const names[] = {
        "USERPROFILE", "LOCALAPPDATA",
    };
    const char *eq = strchr(entry, '=');
    size_t n, i;
    if (eq == NULL || eq == entry) return false;
    n = (size_t)(eq - entry);
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strlen(names[i]) == n && _strnicmp(entry, names[i], n) == 0) {
            return true;
        }
    }
    return false;
}

/* The sandboxed child's environment block: the names botHostChildEnvKeep
 * keeps, PATH = the child's folder and System32, TEMP and TMP = the cache
 * folder. NULL on failure (the caller then fails the start). */
static wchar_t *childEnvBlock(const wchar_t *wdir, const char *cache) {
    const size_t cap = 32768;
    wchar_t *out = (wchar_t *)malloc(cap * sizeof(wchar_t));
    wchar_t *env, *e, *wcache = NULL;
    wchar_t sysRoot[MAX_PATH], line[2 * MAX_PATH + 64];
    size_t len = 0;
    bool ok = out != NULL;
    if (out == NULL) return NULL;
    env = GetEnvironmentStringsW();
    for (e = env; ok && e != NULL && *e != L'\0'; e += wcslen(e) + 1) {
        char u8[1024];
        int n = WideCharToMultiByte(CP_UTF8, 0, e, -1, u8, sizeof(u8), NULL,
                                    NULL);
        if (n <= 0 || (!botHostChildEnvKeep(u8) && !childEnvKeepWin(u8))) {
            continue;
        }
        ok = envPut(out, cap, &len, e, wcslen(e));
    }
    if (env != NULL) FreeEnvironmentStringsW(env);
    if (GetEnvironmentVariableW(L"SystemRoot", sysRoot, MAX_PATH) == 0) {
        wcscpy(sysRoot, L"C:\\Windows");
    }
    if (ok) {
        _snwprintf(line, sizeof(line) / sizeof(line[0]) - 1,
                   L"PATH=%ls;%ls\\System32", wdir != NULL ? wdir : L"",
                   sysRoot);
        line[sizeof(line) / sizeof(line[0]) - 1] = L'\0';
        ok = envPut(out, cap, &len, line, wcslen(line));
    }
    if (ok && cache != NULL && cache[0] != '\0' &&
        (wcache = utf8ToWide(cache)) != NULL) {
        size_t n = wcslen(wcache);
        wchar_t *t = (wchar_t *)malloc((n + 8) * sizeof(wchar_t));
        if (t != NULL) {
            wcscpy(t, L"TEMP=");
            wcscat(t, wcache);
            ok = envPut(out, cap, &len, t, wcslen(t));
            wcscpy(t, L"TMP=");
            wcscat(t, wcache);
            if (ok) ok = envPut(out, cap, &len, t, wcslen(t));
            free(t);
        }
        free(wcache);
    }
    if (!ok || len + 1 > cap) {
        free(out);
        return NULL;
    }
    out[len] = L'\0';  /* the block ends with an empty entry */
    return out;
}

BotHostChildProc *botHostChildSpawn(const char *exe, const char *const *argv,
                                    const BotHostChildLimits *lim,
                                    const BotHostChildSandbox *sb,
                                    const char *logPath, char *err,
                                    size_t errSize, bool *sandboxFailed) {
    SECURITY_ATTRIBUTES sa;
    wchar_t *wenv = NULL;
    HANDLE inR = NULL, inW = NULL, outR = NULL, outW = NULL, logH = NULL;
    HANDLE rinR = NULL, rinW = NULL, routR = NULL, routW = NULL;
    HANDLE handles[5];
    DWORD handleCount = 3;
    STARTUPINFOEXW si;
    PROCESS_INFORMATION pi;
    SECURITY_CAPABILITIES caps;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    bool attrsInited = false;
    SIZE_T attrSize = 0;
    DWORD attrCount;
    char *cmd = NULL;
    size_t cmdSize = 32768, cmdLen = 0;
    wchar_t *wexe = NULL, *wcmd = NULL, *wdir = NULL;
    BotHostChildProc *p = NULL;
    HANDLE job = NULL;
    PSID sid = NULL;
    bool sandbox = sb != NULL && sb->on;
    int i;
    bool ok = false;

    if (err != NULL && errSize > 0) err[0] = '\0';
    if (sandboxFailed != NULL) *sandboxFailed = false;
    memset(&pi, 0, sizeof(pi));
    if (exe == NULL || argv == NULL || argv[0] == NULL) {
        setErr(err, errSize, "no program");
        return NULL;
    }

    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&inR, &inW, &sa, STDIN_PIPE_BYTES) ||
        !CreatePipe(&outR, &outW, &sa, 0)) {
        setErr(err, errSize, "CreatePipe failed");
        goto done;
    }
    {
        /* The game's end does not wait for the child: a write that does
         * not fit writes less, or nothing (botHostChildWriteLine). */
        DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
        if (!SetNamedPipeHandleState(inW, &mode, NULL, NULL)) {
            setErr(err, errSize, "SetNamedPipeHandleState failed (stdin)");
            goto done;
        }
    }
    /* The game's ends stay in the game. */
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    if (sandbox) {
        if (!CreatePipe(&rinR, &rinW, &sa, RELAY_PIPE_BYTES) ||
            !CreatePipe(&routR, &routW, &sa, RELAY_PIPE_BYTES)) {
            setErr(err, errSize, "CreatePipe failed (the relay)");
            goto done;
        }
        SetHandleInformation(rinW, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(routR, HANDLE_FLAG_INHERIT, 0);
    }

    /* The command line, quoted as the child's C runtime reads it. */
    cmd = (char *)malloc(cmdSize);
    if (cmd == NULL) goto done;
    cmd[0] = '\0';
    for (i = 0; argv[i] != NULL; i++) {
        if (i > 0) {
            if (cmdLen + 2 >= cmdSize) goto done;
            cmd[cmdLen++] = ' ';
            cmd[cmdLen] = '\0';
        }
        if (!botHostChildQuoteWinArg(argv[i], cmd, cmdSize, &cmdLen)) {
            setErr(err, errSize, "the command line is too long");
            goto done;
        }
    }
    if (sandbox && !addSandboxArgs(cmd, cmdSize, &cmdLen, sb, rinR, routW)) {
        setErr(err, errSize, "the command line is too long");
        goto done;
    }
    wexe = utf8ToWide(exe);
    wcmd = utf8ToWide(cmd);
    if (wexe == NULL || wcmd == NULL) {
        setErr(err, errSize, "bad program name");
        goto done;
    }
    {
        /* The child starts in its own folder: the game's folder may be
         * one the container cannot open. */
        size_t n = wcslen(wexe);
        wdir = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
        if (wdir != NULL) {
            memcpy(wdir, wexe, (n + 1) * sizeof(wchar_t));
            while (n > 0 && wdir[n - 1] != L'\\' && wdir[n - 1] != L'/') n--;
            if (n > 1) {
                wdir[n - 1] = L'\0';
            } else {
                free(wdir);
                wdir = NULL;
            }
        }
    }

    if (logPath != NULL && logPath[0] != '\0') {
        wchar_t *wlog = utf8ToWide(logPath);
        if (wlog != NULL) {
            logH = CreateFileW(wlog, GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE |
                                   FILE_SHARE_DELETE,
                               &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                               NULL);
            free(wlog);
        }
    }
    if (logH == NULL || logH == INVALID_HANDLE_VALUE) {
        logH = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa,
                           OPEN_EXISTING, 0, NULL);
        if (logH == INVALID_HANDLE_VALUE) {
            setErr(err, errSize, "no log file");
            logH = NULL;
            goto done;
        }
    }

    if (sandbox) {
        char why[200];
        why[0] = '\0';
        sid = containerSid(why, sizeof(why));
        if (sid == NULL) {
            if (err != NULL && errSize > 0) snprintf(err, errSize, "%s", why);
            if (sandboxFailed != NULL) *sandboxFailed = true;
            goto done;
        }
    }

    /* Only these handles go to the child. */
    handles[0] = inR;
    handles[1] = outW;
    handles[2] = logH;
    if (sandbox) {
        handles[3] = rinR;
        handles[4] = routW;
        handleCount = 5;
    }
    attrCount = sandbox ? 2 : 1;
    (void)InitializeProcThreadAttributeList(NULL, attrCount, 0, &attrSize);
    attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attrSize);
    if (attrs == NULL ||
        !InitializeProcThreadAttributeList(attrs, attrCount, 0, &attrSize)) {
        setErr(err, errSize, "the handle list failed");
        goto done;
    }
    attrsInited = true;
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   handles, handleCount * sizeof(HANDLE),
                                   NULL, NULL)) {
        setErr(err, errSize, "the handle list failed");
        goto done;
    }
    if (sandbox) {
        /* No capabilities: no network of any kind, no user folders. */
        memset(&caps, 0, sizeof(caps));
        caps.AppContainerSid = sid;
        if (!UpdateProcThreadAttribute(
                attrs, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &caps,
                sizeof(caps), NULL, NULL)) {
            setErr(err, errSize, "the AppContainer attribute failed");
            if (sandboxFailed != NULL) *sandboxFailed = true;
            goto done;
        }
    }

    memset(&si, 0, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = inR;
    si.StartupInfo.hStdOutput = outW;
    si.StartupInfo.hStdError = logH;
    si.lpAttributeList = attrs;

    if (sandbox) {
        /* A minimal environment: no token in the game's reaches the
         * child. */
        wenv = childEnvBlock(wdir, sb->writeDirCount > 0 ? sb->writeDirs[0]
                                                         : NULL);
        if (wenv == NULL) {
            setErr(err, errSize, "the child's environment failed");
            goto done;
        }
    }
    if (!CreateProcessW(wexe, wcmd, NULL, NULL, TRUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW |
                            BELOW_NORMAL_PRIORITY_CLASS |
                            EXTENDED_STARTUPINFO_PRESENT |
                            (wenv != NULL ? CREATE_UNICODE_ENVIRONMENT : 0),
                        wenv, sandbox ? wdir : NULL, &si.StartupInfo, &pi)) {
        setErr(err, errSize, sandbox ? "CreateProcess in the AppContainer "
                                       "failed"
                                     : "CreateProcess failed");
        /* In the sandbox the program's folder may not be readable by the
         * container: that is the sandbox's failure. */
        if (sandbox && sandboxFailed != NULL) *sandboxFailed = true;
        goto done;
    }

    job = makeJob(lim, err, errSize);
    if (job == NULL || !AssignProcessToJobObject(job, pi.hProcess)) {
        /* No child runs outside its limits. */
        if (job != NULL) {
            setErr(err, errSize, "AssignProcessToJobObject failed");
            CloseHandle(job);
            job = NULL;
        }
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        memset(&pi, 0, sizeof(pi));
        goto done;
    }
    if (ResumeThread(pi.hThread) == (DWORD)-1) {
        setErr(err, errSize, "ResumeThread failed");
        TerminateJobObject(job, 1);
        CloseHandle(job);
        job = NULL;
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        memset(&pi, 0, sizeof(pi));
        goto done;
    }
    CloseHandle(pi.hThread);

    p = (BotHostChildProc *)calloc(1, sizeof(*p));
    if (p == NULL) {
        TerminateJobObject(job, 1);
        CloseHandle(job);
        CloseHandle(pi.hProcess);
        goto done;
    }
    p->process = pi.hProcess;
    p->job = job;
    p->pid = pi.dwProcessId;
    p->stdinW = inW;
    p->stdoutR = outR;
    p->relayW = rinW;
    p->relayR = routR;
    inW = NULL;
    outR = NULL;
    rinW = NULL;
    routR = NULL;
    ok = true;

done:
    /* On every path: a list that was made is deleted before its free. */
    if (attrs != NULL) {
        if (attrsInited) DeleteProcThreadAttributeList(attrs);
        free(attrs);
    }
    if (sid != NULL) FreeSid(sid);
    /* The child's ends: the child has its own copies now. */
    closeIf(&inR);
    closeIf(&outW);
    closeIf(&logH);
    closeIf(&inW);
    closeIf(&outR);
    closeIf(&rinR);
    closeIf(&rinW);
    closeIf(&routR);
    closeIf(&routW);
    free(cmd);
    free(wexe);
    free(wcmd);
    free(wdir);
    free(wenv);
    return ok ? p : NULL;
}

bool botHostChildHasRelay(const BotHostChildProc *p) {
    return p != NULL && p->relayR != NULL && p->relayW != NULL;
}

bool botHostChildRelayRead(BotHostChildProc *p, void *buf, size_t n) {
    uint8_t *b = (uint8_t *)buf;
    if (p == NULL || p->relayR == NULL) return false;
    while (n > 0) {
        DWORD got = 0;
        if (!ReadFile(p->relayR, b, (DWORD)n, &got, NULL) || got == 0) {
            return false;
        }
        b += got;
        n -= got;
    }
    return true;
}

bool botHostChildRelayWrite(BotHostChildProc *p, const void *buf, size_t n) {
    const uint8_t *b = (const uint8_t *)buf;
    if (p == NULL || p->relayW == NULL) return false;
    while (n > 0) {
        DWORD wrote = 0;
        if (!WriteFile(p->relayW, b, (DWORD)n, &wrote, NULL) || wrote == 0) {
            return false;
        }
        b += wrote;
        n -= wrote;
    }
    return true;
}

/* The pipe does not wait (PIPE_NOWAIT): a full pipe gives a short write
 * or none, and so false at once. */
bool botHostChildWriteLine(BotHostChildProc *p, const char *line) {
    char buf[600];
    DWORD wrote = 0;
    int n;
    if (p == NULL || p->stdinW == NULL || line == NULL) return false;
    n = snprintf(buf, sizeof(buf), "%s\n", line);
    if (n <= 0 || (size_t)n >= sizeof(buf)) return false;
    return WriteFile(p->stdinW, buf, (DWORD)n, &wrote, NULL) &&
           wrote == (DWORD)n;
}

void botHostChildCloseStdin(BotHostChildProc *p) {
    if (p == NULL || p->stdinW == NULL) return;
    CloseHandle(p->stdinW);
    p->stdinW = NULL;
}

int botHostChildRead(BotHostChildProc *p, char *buf, size_t size) {
    DWORD avail = 0, got = 0;
    if (p == NULL || p->stdoutR == NULL || size == 0) return -1;
    if (!PeekNamedPipe(p->stdoutR, NULL, 0, NULL, &avail, NULL)) return -1;
    if (avail == 0) return 0;
    if (avail > size) avail = (DWORD)size;
    if (!ReadFile(p->stdoutR, buf, avail, &got, NULL)) return -1;
    return (int)got;
}

static bool exitCodeOf(BotHostChildProc *p, int *exitCode) {
    DWORD code = 0;
    if (!GetExitCodeProcess(p->process, &code)) code = (DWORD)-1;
    if (exitCode != NULL) {
        /* A crash code (0xC0000005 and others) reads as -1. */
        *exitCode = code < 256 ? (int)code : -1;
    }
    return true;
}

bool botHostChildAlive(BotHostChildProc *p, int *exitCode) {
    if (p == NULL || p->process == NULL) return false;
    if (WaitForSingleObject(p->process, 0) == WAIT_TIMEOUT) return true;
    exitCodeOf(p, exitCode);
    return false;
}

bool botHostChildWait(BotHostChildProc *p, uint32_t waitMs, int *exitCode) {
    if (p == NULL || p->process == NULL) return true;
    if (WaitForSingleObject(p->process, waitMs) != WAIT_OBJECT_0) return false;
    exitCodeOf(p, exitCode);
    return true;
}

void botHostChildKill(BotHostChildProc *p) {
    if (p == NULL || p->process == NULL) return;
    if (WaitForSingleObject(p->process, 0) != WAIT_TIMEOUT) return;
    if (p->job != NULL) {
        TerminateJobObject(p->job, 1);
    } else {
        TerminateProcess(p->process, 1);
    }
    WaitForSingleObject(p->process, 5000);
}

void botHostChildFree(BotHostChildProc *p) {
    if (p == NULL) return;
    botHostChildKill(p);
    if (p->stdinW != NULL) CloseHandle(p->stdinW);
    if (p->stdoutR != NULL) CloseHandle(p->stdoutR);
    if (p->relayW != NULL) CloseHandle(p->relayW);
    if (p->relayR != NULL) CloseHandle(p->relayR);
    if (p->job != NULL) CloseHandle(p->job);
    if (p->process != NULL) CloseHandle(p->process);
    free(p);
}

unsigned long botHostChildPid(const BotHostChildProc *p) {
    return p != NULL ? (unsigned long)p->pid : 0ul;
}

bool botHostChildCloseJobForTest(BotHostChildProc *p) {
    if (p == NULL || p->job == NULL) return false;
    CloseHandle(p->job);
    p->job = NULL;
    return true;
}

bool botHostChildQueryJobForTest(BotHostChildProc *p, int *memMb,
                                 int *cpuPct, int *activeProcesses,
                                 bool *killOnClose) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION ext;
    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu;
    DWORD got = 0;
    if (p == NULL || p->job == NULL) return false;
    memset(&ext, 0, sizeof(ext));
    if (!QueryInformationJobObject(p->job, JobObjectExtendedLimitInformation,
                                   &ext, sizeof(ext), &got)) {
        return false;
    }
    if (memMb != NULL) {
        *memMb = (ext.BasicLimitInformation.LimitFlags &
                  JOB_OBJECT_LIMIT_PROCESS_MEMORY)
                     ? (int)(ext.ProcessMemoryLimit / (1024 * 1024))
                     : 0;
    }
    if (activeProcesses != NULL) {
        *activeProcesses = (ext.BasicLimitInformation.LimitFlags &
                            JOB_OBJECT_LIMIT_ACTIVE_PROCESS)
                               ? (int)ext.BasicLimitInformation
                                     .ActiveProcessLimit
                               : 0;
    }
    if (killOnClose != NULL) {
        *killOnClose = (ext.BasicLimitInformation.LimitFlags &
                        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) != 0;
    }
    if (cpuPct != NULL) {
        memset(&cpu, 0, sizeof(cpu));
        *cpuPct = 0;
        if (QueryInformationJobObject(p->job,
                                      JobObjectCpuRateControlInformation,
                                      &cpu, sizeof(cpu), &got) &&
            (cpu.ControlFlags & JOB_OBJECT_CPU_RATE_CONTROL_ENABLE) &&
            (cpu.ControlFlags & JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP)) {
            *cpuPct = (int)(cpu.CpuRate / 100u);
        }
    }
    return true;
}

#endif /* _WIN32 */
