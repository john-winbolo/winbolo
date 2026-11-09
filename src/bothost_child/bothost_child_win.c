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
 * its stdin, stdout and log file handles are given to it.
 */
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bothost_child.h"

struct BotHostChildProc {
    HANDLE process;
    HANDLE job;      /* NULL when the job could not be made */
    HANDLE stdinW;   /* the game's end of the child's stdin */
    HANDLE stdoutR;  /* the game's end of the child's stdout */
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
            /* The memory limit and the rest still hold. */
            fprintf(stderr, "[bothost] the child's CPU cap was refused "
                            "(error %lu); it runs without it\n",
                    (unsigned long)GetLastError());
        }
    }
    return job;
}

BotHostChildProc *botHostChildSpawn(const char *exe, const char *const *argv,
                                    const BotHostChildLimits *lim,
                                    const char *logPath, char *err,
                                    size_t errSize) {
    SECURITY_ATTRIBUTES sa;
    HANDLE inR = NULL, inW = NULL, outR = NULL, outW = NULL, logH = NULL;
    HANDLE handles[3];
    STARTUPINFOEXW si;
    PROCESS_INFORMATION pi;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    SIZE_T attrSize = 0;
    char *cmd = NULL;
    size_t cmdSize = 32768, cmdLen = 0;
    wchar_t *wexe = NULL, *wcmd = NULL;
    BotHostChildProc *p = NULL;
    HANDLE job = NULL;
    int i;
    bool ok = false;

    if (err != NULL && errSize > 0) err[0] = '\0';
    memset(&pi, 0, sizeof(pi));
    if (exe == NULL || argv == NULL || argv[0] == NULL) {
        setErr(err, errSize, "no program");
        return NULL;
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
    wexe = utf8ToWide(exe);
    wcmd = utf8ToWide(cmd);
    if (wexe == NULL || wcmd == NULL) {
        setErr(err, errSize, "bad program name");
        goto done;
    }

    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&inR, &inW, &sa, 0) || !CreatePipe(&outR, &outW, &sa, 0)) {
        setErr(err, errSize, "CreatePipe failed");
        goto done;
    }
    /* The game's ends stay in the game. */
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
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

    /* Only these three handles go to the child. */
    handles[0] = inR;
    handles[1] = outW;
    handles[2] = logH;
    (void)InitializeProcThreadAttributeList(NULL, 1, 0, &attrSize);
    attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attrSize);
    if (attrs == NULL ||
        !InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) ||
        !UpdateProcThreadAttribute(attrs, 0,
                                   PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   handles, sizeof(handles), NULL, NULL)) {
        setErr(err, errSize, "the handle list failed");
        if (attrs != NULL) {
            free(attrs);
            attrs = NULL;
        }
        goto done;
    }

    memset(&si, 0, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = inR;
    si.StartupInfo.hStdOutput = outW;
    si.StartupInfo.hStdError = logH;
    si.lpAttributeList = attrs;

    if (!CreateProcessW(wexe, wcmd, NULL, NULL, TRUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW |
                            BELOW_NORMAL_PRIORITY_CLASS |
                            EXTENDED_STARTUPINFO_PRESENT,
                        NULL, NULL, &si.StartupInfo, &pi)) {
        setErr(err, errSize, "CreateProcess failed");
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
    inW = NULL;
    outR = NULL;
    ok = true;

done:
    if (attrs != NULL) {
        DeleteProcThreadAttributeList(attrs);
        free(attrs);
    }
    /* The child's ends: the child has its own copies now. */
    if (inR != NULL) CloseHandle(inR);
    if (outW != NULL) CloseHandle(outW);
    if (logH != NULL) CloseHandle(logH);
    if (inW != NULL) CloseHandle(inW);
    if (outR != NULL) CloseHandle(outR);
    free(cmd);
    free(wexe);
    free(wcmd);
    return ok ? p : NULL;
}

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
