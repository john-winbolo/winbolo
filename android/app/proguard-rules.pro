# Classes native code reaches by name, through FindClass and
# GetStaticMethodID, so no reference from Java keeps them alive:
#   WinBoloHttp      the WinBolo.net transport (src/winbolonet/http_jni.c:
#                    request, and the natives it registers with RegisterNatives)
#   WinBoloActivity  smallestWidthDp, imeHeightPx, showKeyboardAgain,
#                    safeInsetsPx (src/android/android_device.c), shareFile
#                    (android_share.c), and the class loader http_jni.c
#                    falls back to on a worker thread
#   WinBoloNet       start and kind (src/android/android_netpath.c)
# Minification is off in build.gradle.kts; these rules are read all the
# same, so turning it on keeps every class listed here. Any other class
# native code is made to reach by name needs a rule of its own.
-keep class com.winbolo.android.WinBoloHttp { *; }
-keep class com.winbolo.android.WinBoloActivity { *; }
-keep class com.winbolo.android.WinBoloNet { *; }
# SDL's Java classes, which libSDL3 looks up with FindClass and
# GetStaticMethodID.
-keep class org.libsdl.app.** { *; }
