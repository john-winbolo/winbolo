/*
 * Copyright (c) 1998-2008 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*********************************************************
*Name:          posix_stubs
*Filename:      posix_stubs.c
*Purpose:
*  Non-Win32 stubs for the dedicated server that replace
*  Win32-only APIs used by winbolonet/http.c:
*
*  - preferencesGetPreferenceFile() — returns the config file path
*  - GetPrivateProfileString()      — reads a key from a .ini file
*  - WritePrivateProfileString()    — writes a key to a .ini file
*
*  These use a simple hand-rolled INI parser so there is no
*  dependency on platform_config (which belongs to the SDL3 client).
*********************************************************/

#ifndef _WIN32

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pwd.h>
#include <unistd.h>
#include "global.h"

/* ------------------------------------------------------------------ */
/* preferencesGetPreferenceFile                                        */
/* ------------------------------------------------------------------ */

static char preferenceFileOverride[FILENAME_MAX] = "";

void preferencesSetPreferenceFileOverride(const char *path) {
  if (!path || !path[0]) {
    preferenceFileOverride[0] = '\0';
    return;
  }
  strncpy(preferenceFileOverride, path, FILENAME_MAX - 1);
  preferenceFileOverride[FILENAME_MAX - 1] = '\0';
}

void preferencesGetPreferenceFile(char *dest) {
  if (preferenceFileOverride[0]) {
    strncpy(dest, preferenceFileOverride, FILENAME_MAX - 1);
    dest[FILENAME_MAX - 1] = '\0';
    return;
  }

  const char *home = getenv("HOME");
  if (!home) {
    struct passwd *pw = getpwuid(getuid());
    if (pw) home = pw->pw_dir;
  }
  if (!home) home = ".";

  /* ~/.config/winbolo/ directory */
  char dir[FILENAME_MAX];
  snprintf(dir, sizeof(dir), "%s/.config/winbolo", home);
  dir[sizeof(dir) - 1] = '\0';
  mkdir(dir, 0755);

  size_t dlen = strlen(dir);
  if (dlen + sizeof("/WinBolo.ini") <= FILENAME_MAX) {
    memcpy(dest, dir, dlen);
    memcpy(dest + dlen, "/WinBolo.ini", sizeof("/WinBolo.ini"));
  } else {
    snprintf(dest, FILENAME_MAX, "%s", ".config/winbolo/WinBolo.ini");
  }
}

/* ------------------------------------------------------------------ */
/* Minimal INI reader/writer                                           */
/* ------------------------------------------------------------------ */

#define MAX_INI_LINE 512

/* Read a value from a plain INI file.
 * Returns number of characters written (excluding NUL), like the Win32 API. */
DWORD GetPrivateProfileString(const char *section, const char *key,
                               const char *defaultVal,
                               char *out, DWORD outSize,
                               const char *filePath) {
  FILE *fp;
  char line[MAX_INI_LINE];
  char curSection[128] = "";
  size_t keyLen;

  if (outSize == 0) return 0;

  fp = fopen(filePath, "r");
  if (!fp) {
    strncpy(out, defaultVal ? defaultVal : "", outSize - 1);
    out[outSize - 1] = '\0';
    return (DWORD)strlen(out);
  }

  keyLen = strlen(key);

  while (fgets(line, sizeof(line), fp)) {
    /* Strip trailing newline */
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
      line[--len] = '\0';

    if (line[0] == '[') {
      char *end = strchr(line + 1, ']');
      if (end) {
        /* Bounded copy + explicit NUL: strncpy(... sizeof-1) leaves
         * curSection un-terminated when the section name reaches its
         * cap, which would make the next strcmp read past the buffer. */
        size_t slen = (size_t)(end - (line + 1));
        if (slen >= sizeof(curSection)) slen = sizeof(curSection) - 1;
        memcpy(curSection, line + 1, slen);
        curSection[slen] = '\0';
      }
      continue;
    }

    if (strcmp(curSection, section) != 0) continue;

    /* key=value */
    if (strncmp(line, key, keyLen) == 0 && line[keyLen] == '=') {
      const char *val = line + keyLen + 1;
      strncpy(out, val, outSize - 1);
      out[outSize - 1] = '\0';
      fclose(fp);
      return (DWORD)strlen(out);
    }
  }

  fclose(fp);
  strncpy(out, defaultVal ? defaultVal : "", outSize - 1);
  out[outSize - 1] = '\0';
  return (DWORD)strlen(out);
}

/* Reject any byte that would let a value spill out of its own
 * key=value line on read-back. Win32's INI API has the same
 * constraint in practice. Used in WritePrivateProfileString so a
 * value sourced from the network (WBN auth token, winbolo:// URL
 * fields) can't inject synthetic keys or section headers. */
static int hasIniMetachars(const char *s, int rejectEquals, int rejectBrackets) {
  if (!s) return 0;
  for (; *s; s++) {
    if (*s == '\n' || *s == '\r') return 1;
    if (rejectEquals && *s == '=') return 1;
    if (rejectBrackets && (*s == '[' || *s == ']')) return 1;
  }
  return 0;
}

/* Free a dynamic line array. */
static void freeLineArray(char **lines, int count) {
  int j;
  for (j = 0; j < count; j++) free(lines[j]);
  free(lines);
}

/* Append a heap-duplicated line to a growing vector. Returns 0 on OOM. */
static int appendLine(char ***lines, int *count, int *cap, const char *s, size_t len) {
  if (*count == *cap) {
    int newCap = *cap ? *cap * 2 : 64;
    char **nb = (char **)realloc(*lines, (size_t)newCap * sizeof(char *));
    if (!nb) return 0;
    *lines = nb;
    *cap = newCap;
  }
  char *copy = (char *)malloc(len + 1);
  if (!copy) return 0;
  memcpy(copy, s, len);
  copy[len] = '\0';
  (*lines)[(*count)++] = copy;
  return 1;
}

/* Write a value to a plain INI file.
 *
 * Slurps the whole file into a growing heap vector (one line per entry,
 * CR/LF stripped at read time), modifies the matching key, and rewrites
 * the file. Dynamic allocation: there is no line cap — WinBolo's flush
 * path writes ~40 keys per save and that cascades fast when corruption
 * doubles blank lines, so any fixed cap will eventually be hit.
 *
 * Returns non-zero on success, 0 on failure (matches Win32 contract). */
int WritePrivateProfileString(const char *section, const char *key,
                                const char *value,
                                const char *filePath) {
  FILE *fp;
  char **lines = NULL;
  int lineCount = 0;
  int lineCap = 0;
  char curSection[128] = "";
  int sectionFound = 0;
  int keyFound = 0;
  int i;

  if (!section || !key || !filePath) return 0;
  /* Win32 treats value=NULL as "delete this key". We do not implement
   * deletion (no caller needs it); treat NULL as the empty string so we
   * never strlen(NULL). */
  if (!value) value = "";

  /* Reject control bytes that would let untrusted values break out of
   * their own key=value line. Section/key are normally compile-time
   * literals; value carries network-sourced data (WBN token, URL fields). */
  if (hasIniMetachars(section, 0, 1)) return 0;
  if (hasIniMetachars(key, 1, 0))     return 0;
  if (hasIniMetachars(value, 0, 0))   return 0;

  /* Read whole file, stripping CR/LF from each line up-front. Stripping
   * here (not inside the find loop) is load-bearing: the previous
   * implementation stripped lazily in the find loop and broke out early
   * on a key hit, leaving later lines un-stripped — write-back then
   * appended an extra '\n' to each, doubling blank lines per call. */
  fp = fopen(filePath, "r");
  if (fp) {
    char buf[MAX_INI_LINE];
    while (fgets(buf, sizeof(buf), fp)) {
      size_t len = strlen(buf);
      while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) len--;
      if (!appendLine(&lines, &lineCount, &lineCap, buf, len)) {
        freeLineArray(lines, lineCount);
        fclose(fp);
        return 0;
      }
    }
    fclose(fp);
  }

  /* Find existing key; also remember the last line index belonging to
   * the target section, so a missing key can be inserted under its own
   * section instead of getting appended after whatever section happens
   * to be last in the file (which would cause every launch to re-append
   * a duplicate the next read won't find). */
  size_t klen = strlen(key);
  int sectionInsertIdx = -1;
  for (i = 0; i < lineCount; i++) {
    char *ln = lines[i];
    if (ln[0] == '[') {
      char *end = strchr(ln + 1, ']');
      if (end) {
        size_t slen = (size_t)(end - (ln + 1));
        if (slen >= sizeof(curSection)) slen = sizeof(curSection) - 1;
        memcpy(curSection, ln + 1, slen);
        curSection[slen] = '\0';
      }
      if (strcmp(curSection, section) == 0) {
        sectionFound = 1;
        sectionInsertIdx = i;  /* point at section header; advances below */
      }
      continue;
    }
    if (strcmp(curSection, section) != 0) continue;
    sectionInsertIdx = i;  /* track last in-section line */
    if (strncmp(ln, key, klen) == 0 && ln[klen] == '=') {
      size_t vlen = strlen(value);
      char *replacement = (char *)malloc(klen + 1 + vlen + 1);
      if (!replacement) {
        freeLineArray(lines, lineCount);
        return 0;
      }
      memcpy(replacement, key, klen);
      replacement[klen] = '=';
      memcpy(replacement + klen + 1, value, vlen + 1);
      free(lines[i]);
      lines[i] = replacement;
      keyFound = 1;
      break;
    }
  }

  if (!keyFound) {
    char tmp[MAX_INI_LINE];
    if (!sectionFound) {
      snprintf(tmp, sizeof(tmp), "[%s]", section);
      if (!appendLine(&lines, &lineCount, &lineCap, tmp, strlen(tmp))) {
        freeLineArray(lines, lineCount);
        return 0;
      }
      snprintf(tmp, sizeof(tmp), "%s=%s", key, value);
      if (!appendLine(&lines, &lineCount, &lineCap, tmp, strlen(tmp))) {
        freeLineArray(lines, lineCount);
        return 0;
      }
    } else {
      /* Insert right after the last in-section line so the key lands
       * under its own section, not at end-of-file. */
      int insertAt = sectionInsertIdx + 1;
      snprintf(tmp, sizeof(tmp), "%s=%s", key, value);
      size_t tlen = strlen(tmp);
      if (lineCount == lineCap) {
        int newCap = lineCap ? lineCap * 2 : 64;
        char **nb = (char **)realloc(lines, (size_t)newCap * sizeof(char *));
        if (!nb) { freeLineArray(lines, lineCount); return 0; }
        lines = nb;
        lineCap = newCap;
      }
      char *copy = (char *)malloc(tlen + 1);
      if (!copy) { freeLineArray(lines, lineCount); return 0; }
      memcpy(copy, tmp, tlen + 1);
      if (insertAt < lineCount) {
        memmove(&lines[insertAt + 1], &lines[insertAt],
                (size_t)(lineCount - insertAt) * sizeof(char *));
      }
      lines[insertAt] = copy;
      lineCount++;
    }
  }

  /* Write to a sibling temp file then rename(2) over the target.
   * Two reasons: atomic on POSIX within one filesystem, so a crash
   * mid-write can't leave a truncated prefs file; and we can set
   * mode 0600 on the temp before rename, so the auth token in this
   * file is never world-readable even on a shared host. */
  char tmpPath[FILENAME_MAX];
  int n = snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", filePath);
  if (n < 0 || n >= (int)sizeof(tmpPath)) {
    freeLineArray(lines, lineCount);
    return 0;
  }

  /* Open via open()+fdopen so we set 0600 at creation, not after.
   * O_TRUNC is fine because we own the temp name. */
  int fd = open(tmpPath, O_CREAT | O_WRONLY | O_TRUNC, 0600);
  if (fd < 0) {
    freeLineArray(lines, lineCount);
    return 0;
  }
  fp = fdopen(fd, "w");
  if (!fp) {
    close(fd);
    unlink(tmpPath);
    freeLineArray(lines, lineCount);
    return 0;
  }
  for (i = 0; i < lineCount; i++) {
    if (fprintf(fp, "%s\n", lines[i]) < 0) {
      fclose(fp);
      unlink(tmpPath);
      freeLineArray(lines, lineCount);
      return 0;
    }
  }
  if (fflush(fp) != 0 || fclose(fp) != 0) {
    unlink(tmpPath);
    freeLineArray(lines, lineCount);
    return 0;
  }
  /* Defensive: re-assert 0600 in case an old temp inode survived
   * with a wider mode (umask races on first creation, etc). */
  chmod(tmpPath, 0600);
  if (rename(tmpPath, filePath) != 0) {
    unlink(tmpPath);
    freeLineArray(lines, lineCount);
    return 0;
  }
  freeLineArray(lines, lineCount);
  return 1;
}

#endif /* !_WIN32 */
