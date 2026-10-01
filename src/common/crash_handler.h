/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * crash_handler.h — last-gasp C stack-trace dumper.
 *
 * Installs a process-wide unhandled-exception filter (Windows / DbgHelp) that,
 * on a fatal native exception (access violation, illegal instruction, etc.),
 * prints a symbolized C stack trace to stderr AND to a crash log file, then
 * lets the process die (chaining to any previously-installed filter, e.g.
 * Sentry, so cloud reporting still fires).
 *
 * Symbol quality depends on the build: a normal RelWithDebInfo build (PDBs
 * present) yields "func  file.c:line"; a stripped build (DebugInformationFormat
 * =None / GenerateDebugInformation=false) degrades to "module+0xADDR".
 *
 * No-op on non-Windows platforms.
 */
#ifndef WINBOLO_CRASH_HANDLER_H
#define WINBOLO_CRASH_HANDLER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Install the unhandled-exception filter. appName tags the banner + crash
 * filename (e.g. "BrainTest"). Call once, early in main(). Safe to call on any
 * platform — does nothing where unsupported. */
void crashHandlerInstall(const char *appName);

/* Set the directory the crash log is written to (e.g. the debug-session dir).
 * If never set, or set to NULL/"", the crash file is written to the current
 * working directory. May be called after install once the dir is known. */
void crashHandlerSetOutputDir(const char *dir);

#ifdef __cplusplus
}
#endif

#endif /* WINBOLO_CRASH_HANDLER_H */
