/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          android_netpath.c
 * Purpose:
 *  The game front's net path query over WinBoloNet.java.
 *  The Java side keeps the kind up to date from
 *  ConnectivityManager's default network callback; the
 *  query here is one static call that reads it, so it
 *  returns at once on the main thread as gamefront.h
 *  requires. See android_netpath.h.
 *********************************************************/

#include <jni.h>
#include <SDL3/SDL.h>

#include "../gui/gamefront.h"
#include "../common/wb_log.h"
#include "android_netpath.h"

#define NETPATH_JAVA_CLASS "com/winbolo/android/WinBoloNet"

/* The values WinBoloNet.kind() returns. */
#define NETPATH_KIND_WIFI     1
#define NETPATH_KIND_ETHERNET 2
#define NETPATH_KIND_CELLULAR 3

static jclass    s_netClass = NULL;   /* global reference */
static jmethodID s_midKind  = NULL;

static bool netPathClearException(JNIEnv *env, const char *what) {
  if ((*env)->ExceptionCheck(env)) {
    WB_LOG_WARN(WB_LOG_CAT_NET, "net path JNI: Java exception in %s", what);
    (*env)->ExceptionDescribe(env);
    (*env)->ExceptionClear(env);
    return true;
  }
  return false;
}

static GameFrontNetPath androidNetPathQuery(void) {
  JNIEnv *env;
  jint kind;
  if (s_netClass == NULL || s_midKind == NULL) return GAMEFRONT_NET_PATH_UNKNOWN;
  env = (JNIEnv *)SDL_GetAndroidJNIEnv();
  if (env == NULL) return GAMEFRONT_NET_PATH_UNKNOWN;
  kind = (*env)->CallStaticIntMethod(env, s_netClass, s_midKind);
  if (netPathClearException(env, "kind")) return GAMEFRONT_NET_PATH_UNKNOWN;
  switch (kind) {
  case NETPATH_KIND_WIFI:     return GAMEFRONT_NET_PATH_WIFI;
  case NETPATH_KIND_ETHERNET: return GAMEFRONT_NET_PATH_WIRED;
  case NETPATH_KIND_CELLULAR: return GAMEFRONT_NET_PATH_CELLULAR;
  default:                    return GAMEFRONT_NET_PATH_UNKNOWN; /* none, or not known */
  }
}

bool androidNetPathInstall(void) {
  JNIEnv *env;
  jclass local;
  jmethodID midStart;
  jboolean started;

  if (s_netClass != NULL) return true;
  env = (JNIEnv *)SDL_GetAndroidJNIEnv();
  if (env == NULL) {
    WB_LOG_ERROR(WB_LOG_CAT_NET, "net path JNI: no JNI environment: %s", SDL_GetError());
    return false;
  }
  if ((*env)->PushLocalFrame(env, 8) < 0) return false;

  /* FindClass sees the app's classes from this thread, which Java created
     (http_jni.c resolves its class the same way). */
  local = (*env)->FindClass(env, NETPATH_JAVA_CLASS);
  if (local == NULL) {
    (*env)->ExceptionClear(env);
    WB_LOG_ERROR(WB_LOG_CAT_NET, "net path JNI: class %s not found", NETPATH_JAVA_CLASS);
    (*env)->PopLocalFrame(env, NULL);
    return false;
  }
  midStart = (*env)->GetStaticMethodID(env, local, "start", "()Z");
  s_midKind = (*env)->GetStaticMethodID(env, local, "kind", "()I");
  if (midStart == NULL || s_midKind == NULL) {
    netPathClearException(env, "method lookup");
    s_midKind = NULL;
    (*env)->PopLocalFrame(env, NULL);
    return false;
  }
  started = (*env)->CallStaticBooleanMethod(env, local, midStart);
  if (netPathClearException(env, "start") || !started) {
    WB_LOG_WARN(WB_LOG_CAT_NET, "net path: the network watch did not start");
    s_midKind = NULL;
    (*env)->PopLocalFrame(env, NULL);
    return false;
  }
  s_netClass = (jclass)(*env)->NewGlobalRef(env, local);
  (*env)->PopLocalFrame(env, NULL);
  if (s_netClass == NULL) {
    s_midKind = NULL;
    return false;
  }
  gameFrontSetNetPathQuery(androidNetPathQuery);
  WB_LOG_INFO(WB_LOG_CAT_NET, "net path: watching the default network");
  return true;
}
