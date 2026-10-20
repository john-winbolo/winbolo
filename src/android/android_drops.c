/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
*Name:          Android Drops
*Filename:      android_drops.c
*Purpose:
*  The kept recording and game link; see android_drops.h.
*********************************************************/

#include <jni.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "../gui/gamefront.h"
#include "../common/wb_log.h"
#include "android_drops.h"

/* The latest of each kind wins: a second file opened before the welcome
 * screen ran is the one the player asked for last. Written by the natives
 * below on the Java main thread (and by main for a cold launch) and read by
 * the welcome screen on SDL's main thread, so the lock is needed. */
static SDL_Mutex *s_dropLock = NULL;
static char s_dropLog[FILENAME_MAX];
static char s_dropLink[512];

static void androidDropsKeepLink(const char *link) {
  SDL_LockMutex(s_dropLock);
  SDL_strlcpy(s_dropLink, link, sizeof(s_dropLink));
  SDL_UnlockMutex(s_dropLock);
  WB_LOG_INFO(WB_LOG_CAT_GUI, "[URL] Kept winbolo:// link for the menu: %s", link);
}

void androidDropsKeepRecording(const char *path) {
  SDL_LockMutex(s_dropLock);
  SDL_strlcpy(s_dropLog, path, sizeof(s_dropLog));
  SDL_UnlockMutex(s_dropLock);
  WB_LOG_INFO(WB_LOG_CAT_GUI, "Kept recording to play: %s", path);
}

/* The two natives WinBoloActivity.onNewIntent calls. Named for the JVM's
 * own lookup, so no registration call has to have run first: the library
 * is loaded before the activity can receive an intent. */
JNIEXPORT void JNICALL Java_com_winbolo_android_WinBoloActivity_nativeOpenLink(
    JNIEnv *env, jclass cls, jstring url) {
  const char *s;
  (void)cls;
  if (url == NULL) return;
  s = (*env)->GetStringUTFChars(env, url, NULL);
  if (s == NULL) return;
  androidDropsKeepLink(s);
  (*env)->ReleaseStringUTFChars(env, url, s);
}

JNIEXPORT void JNICALL Java_com_winbolo_android_WinBoloActivity_nativeOpenRecording(
    JNIEnv *env, jclass cls, jstring path) {
  const char *s;
  (void)cls;
  if (path == NULL) return;
  s = (*env)->GetStringUTFChars(env, path, NULL);
  if (s == NULL) return;
  androidDropsKeepRecording(s);
  (*env)->ReleaseStringUTFChars(env, path, s);
}

void androidDropsInstall(void) {
  if (s_dropLock == NULL) {
    s_dropLock = SDL_CreateMutex();
  }
}

void androidDropsForgetLink(const char *link) {
  if (link == NULL) return;
  SDL_LockMutex(s_dropLock);
  if (strcmp(s_dropLink, link) == 0) {
    s_dropLink[0] = '\0';
  }
  SDL_UnlockMutex(s_dropLock);
}

int androidDropsWelcomeResult(void) {
  char link[sizeof(s_dropLink)];
  bool haveLog;

  SDL_LockMutex(s_dropLock);
  SDL_strlcpy(link, s_dropLink, sizeof(link));
  s_dropLink[0] = '\0';
  haveLog = (s_dropLog[0] != '\0');
  SDL_UnlockMutex(s_dropLock);

  if (link[0] != '\0') {
    WB_LOG_INFO(WB_LOG_CAT_GUI, "[URL] Welcome screen taking the kept link: %s", link);
    gameFrontHandleUrlOpen(link);
    return (int)openInternetManual;
  }
  if (haveLog) {
    WB_LOG_INFO(WB_LOG_CAT_GUI, "Welcome screen opening the kept recording");
    return (int)openLogViewer;
  }
  return -1;
}

bool androidDropsTakeLog(char *out, size_t size) {
  bool have;
  SDL_LockMutex(s_dropLock);
  have = (s_dropLog[0] != '\0');
  if (have) {
    SDL_strlcpy(out, s_dropLog, size);
    s_dropLog[0] = '\0';
  }
  SDL_UnlockMutex(s_dropLock);
  return have;
}
