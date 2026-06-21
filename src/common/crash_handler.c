/*
 * crash_handler.c — see crash_handler.h.
 *
 * Windows implementation uses DbgHelp's StackWalk64 over the faulting thread's
 * CONTEXT, resolving each frame with SymFromAddr / SymGetLineFromAddr64. Other
 * platforms get a no-op so callers don't need their own #ifdefs.
 */
#include "crash_handler.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32

#include <windows.h>
#include <dbghelp.h>
#include <process.h>

/* Belt-and-suspenders auto-link under MSVC; CMake also links dbghelp. */
#if defined(_MSC_VER)
#pragma comment(lib, "dbghelp.lib")
#endif

static char  s_appName[64]   = "app";
static char  s_outDir[1024]  = "";
static LPTOP_LEVEL_EXCEPTION_FILTER s_prevFilter = NULL;

static const char *exc_name(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      return "EXCEPTION_ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW:        return "EXCEPTION_STACK_OVERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
        case EXCEPTION_PRIV_INSTRUCTION:      return "EXCEPTION_PRIV_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:         return "EXCEPTION_IN_PAGE_ERROR";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
        default:                              return "EXCEPTION_UNKNOWN";
    }
}

/* Write a line to stderr and (if open) the crash file. */
static void emit2(FILE *f, const char *s) {
    fputs(s, stderr);
    if (f) fputs(s, f);
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    HANDLE proc   = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();

    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(proc, NULL, TRUE);

    /* Open the crash file (best effort — stderr always gets the trace). */
    FILE *f = NULL;
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char path[1280];
        if (s_outDir[0]) {
            _snprintf(path, sizeof(path),
                      "%s/crash_%s_%04d%02d%02d_%02d%02d%02d_pid%lu.log",
                      s_outDir, s_appName, st.wYear, st.wMonth, st.wDay,
                      st.wHour, st.wMinute, st.wSecond,
                      (unsigned long)GetCurrentProcessId());
        } else {
            _snprintf(path, sizeof(path),
                      "crash_%s_%04d%02d%02d_%02d%02d%02d_pid%lu.log",
                      s_appName, st.wYear, st.wMonth, st.wDay,
                      st.wHour, st.wMinute, st.wSecond,
                      (unsigned long)GetCurrentProcessId());
        }
        path[sizeof(path) - 1] = '\0';
        f = fopen(path, "w");
        if (f) fprintf(stderr, "  (crash log: %s)\n", path);
    }

    char hdr[320];
    _snprintf(hdr, sizeof(hdr),
              "\n===== C CRASH (%s) =====\n"
              "  app=%s  pid=%lu  fault_addr=0x%p\n",
              exc_name(ep->ExceptionRecord->ExceptionCode),
              s_appName, (unsigned long)GetCurrentProcessId(),
              ep->ExceptionRecord->ExceptionAddress);
    hdr[sizeof(hdr) - 1] = '\0';
    emit2(f, hdr);

    /* Access-violation extra detail: read vs write + target address. */
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION
        && ep->ExceptionRecord->NumberParameters >= 2) {
        char av[160];
        ULONG_PTR rw  = ep->ExceptionRecord->ExceptionInformation[0];
        ULONG_PTR adr = ep->ExceptionRecord->ExceptionInformation[1];
        _snprintf(av, sizeof(av), "  access_violation: %s 0x%llx\n",
                  rw == 0 ? "READ from" : (rw == 1 ? "WRITE to" : "EXECUTE at"),
                  (unsigned long long)adr);
        av[sizeof(av) - 1] = '\0';
        emit2(f, av);
    }

    /* Walk the faulting thread's stack. */
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 frame;
    memset(&frame, 0, sizeof(frame));
    DWORD machine;
#if defined(_M_X64)
    machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset    = ctx.Rip; frame.AddrPC.Mode    = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp; frame.AddrStack.Mode = AddrModeFlat;
#elif defined(_M_IX86)
    machine = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset    = ctx.Eip; frame.AddrPC.Mode    = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Ebp; frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Esp; frame.AddrStack.Mode = AddrModeFlat;
#elif defined(_M_ARM64)
    machine = IMAGE_FILE_MACHINE_ARM64;
    frame.AddrPC.Offset    = ctx.Pc;  frame.AddrPC.Mode    = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Fp;  frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Sp;  frame.AddrStack.Mode = AddrModeFlat;
#else
    emit2(f, "  (unsupported architecture — no stack walk)\n");
    machine = 0;
#endif

    if (machine != 0) {
        char symbuf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *sym = (SYMBOL_INFO *)symbuf;
        int i;
        for (i = 0; i < 64; i++) {
            if (!StackWalk64(machine, proc, thread, &frame, &ctx, NULL,
                             SymFunctionTableAccess64, SymGetModuleBase64, NULL))
                break;
            if (frame.AddrPC.Offset == 0) break;

            DWORD64 addr = frame.AddrPC.Offset;
            char line[640];

            memset(symbuf, 0, sizeof(symbuf));
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen   = 255;
            DWORD64 symDisp   = 0;
            const char *fname = NULL;
            if (SymFromAddr(proc, addr, &symDisp, sym)) fname = sym->Name;

            IMAGEHLP_LINE64 ln;
            memset(&ln, 0, sizeof(ln));
            ln.SizeOfStruct = sizeof(ln);
            DWORD lnDisp = 0;
            if (SymGetLineFromAddr64(proc, addr, &lnDisp, &ln)) {
                _snprintf(line, sizeof(line),
                          "  #%-2d %s  %s:%lu  [0x%llx]\n",
                          i, fname ? fname : "?",
                          ln.FileName, (unsigned long)ln.LineNumber,
                          (unsigned long long)addr);
            } else {
                /* No line info — fall back to module base + offset. */
                char modname[MAX_PATH] = "?";
                DWORD64 modbase = SymGetModuleBase64(proc, addr);
                if (modbase) {
                    HMODULE hm = (HMODULE)(ULONG_PTR)modbase;
                    char full[MAX_PATH];
                    if (GetModuleFileNameA(hm, full, sizeof(full))) {
                        const char *bn = full, *p;
                        for (p = full; *p; p++)
                            if (*p == '\\' || *p == '/') bn = p + 1;
                        strncpy(modname, bn, sizeof(modname) - 1);
                        modname[sizeof(modname) - 1] = '\0';
                    }
                }
                if (fname) {
                    _snprintf(line, sizeof(line),
                              "  #%-2d %s+0x%llx  (%s)  [0x%llx]\n",
                              i, fname, (unsigned long long)symDisp,
                              modname, (unsigned long long)addr);
                } else {
                    _snprintf(line, sizeof(line),
                              "  #%-2d %s+0x%llx  [0x%llx]\n",
                              i, modname,
                              (unsigned long long)(addr - modbase),
                              (unsigned long long)addr);
                }
            }
            line[sizeof(line) - 1] = '\0';
            emit2(f, line);
        }
        if (i == 0) emit2(f, "  (stack walk produced no frames)\n");
    }

    emit2(f, "===== END C CRASH =====\n");
    fflush(stderr);
    if (f) { fflush(f); fclose(f); }

    SymCleanup(proc);

    /* Chain to whatever was installed before us (e.g. Sentry) so it can still
     * report; if nothing, terminate the process. */
    if (s_prevFilter) return s_prevFilter(ep);
    return EXCEPTION_EXECUTE_HANDLER;
}

void crashHandlerInstall(const char *appName) {
    if (appName && appName[0]) {
        strncpy(s_appName, appName, sizeof(s_appName) - 1);
        s_appName[sizeof(s_appName) - 1] = '\0';
    }
    /* Capture the prior filter so we can chain to it. */
    s_prevFilter = SetUnhandledExceptionFilter(crash_filter);
}

void crashHandlerSetOutputDir(const char *dir) {
    if (dir && dir[0]) {
        strncpy(s_outDir, dir, sizeof(s_outDir) - 1);
        s_outDir[sizeof(s_outDir) - 1] = '\0';
    } else {
        s_outDir[0] = '\0';
    }
}

#else /* !_WIN32 — no-op */

void crashHandlerInstall(const char *appName) { (void)appName; }
void crashHandlerSetOutputDir(const char *dir) { (void)dir; }

#endif /* _WIN32 */
