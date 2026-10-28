/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          android_share.c
 * Purpose:
 *  The share sheet over WinBoloActivity.shareFile; see
 *  android_share.h. The class is resolved once from the
 *  main thread, as android_netpath.c resolves its class,
 *  and the call is one static method.
 *********************************************************/

#include <jni.h>
#include <SDL3/SDL.h>

#include "../common/wb_log.h"
#include "android_share.h"

#define SHARE_JAVA_CLASS "com/winbolo/android/WinBoloActivity"

static jclass    s_activityClass = NULL;   /* global reference */
static jmethodID s_midShareFile  = NULL;

static bool shareClearException(JNIEnv *env, const char *what) {
  if ((*env)->ExceptionCheck(env)) {
    WB_LOG_WARN(WB_LOG_CAT_PLATFORM, "share JNI: Java exception in %s", what);
    (*env)->ExceptionDescribe(env);
    (*env)->ExceptionClear(env);
    return true;
  }
  return false;
}

bool androidShareInstall(void) {
  JNIEnv *env;
  jclass local;

  if (s_activityClass != NULL) return true;
  env = (JNIEnv *)SDL_GetAndroidJNIEnv();
  if (env == NULL) {
    WB_LOG_ERROR(WB_LOG_CAT_PLATFORM, "share JNI: no JNI environment: %s", SDL_GetError());
    return false;
  }
  if ((*env)->PushLocalFrame(env, 8) < 0) return false;

  /* FindClass sees the app's classes from this thread, which Java created. */
  local = (*env)->FindClass(env, SHARE_JAVA_CLASS);
  if (local == NULL) {
    (*env)->ExceptionClear(env);
    WB_LOG_ERROR(WB_LOG_CAT_PLATFORM, "share JNI: class %s not found", SHARE_JAVA_CLASS);
    (*env)->PopLocalFrame(env, NULL);
    return false;
  }
  s_midShareFile = (*env)->GetStaticMethodID(env, local, "shareFile",
                                             "(Ljava/lang/String;Ljava/lang/String;)Z");
  if (s_midShareFile == NULL) {
    shareClearException(env, "method lookup");
    (*env)->PopLocalFrame(env, NULL);
    return false;
  }
  s_activityClass = (jclass)(*env)->NewGlobalRef(env, local);
  (*env)->PopLocalFrame(env, NULL);
  if (s_activityClass == NULL) {
    s_midShareFile = NULL;
    return false;
  }
  return true;
}

bool androidShareFile(const char *path, const char *mime) {
  JNIEnv *env;
  jstring jpath, jmime;
  jboolean started;

  if (path == NULL || mime == NULL) return false;
  if (s_activityClass == NULL || s_midShareFile == NULL) {
    WB_LOG_WARN(WB_LOG_CAT_PLATFORM, "share: not bound; cannot share %s", path);
    return false;
  }
  env = (JNIEnv *)SDL_GetAndroidJNIEnv();
  if (env == NULL) return false;
  if ((*env)->PushLocalFrame(env, 8) < 0) return false;
  jpath = (*env)->NewStringUTF(env, path);
  jmime = (*env)->NewStringUTF(env, mime);
  if (jpath == NULL || jmime == NULL) {
    (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env, NULL);
    return false;
  }
  started = (*env)->CallStaticBooleanMethod(env, s_activityClass, s_midShareFile,
                                            jpath, jmime);
  if (shareClearException(env, "shareFile")) started = JNI_FALSE;
  (*env)->PopLocalFrame(env, NULL);
  WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "share: %s %s (%s)",
              started ? "sheet shown for" : "could not share", path, mime);
  return started == JNI_TRUE;
}
