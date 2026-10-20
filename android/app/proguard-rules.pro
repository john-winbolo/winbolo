# Classes native code reaches by name, through FindClass and
# GetStaticMethodID, so no reference from Java keeps them alive:
#   WinBoloHttp      the WinBolo.net transport (src/winbolonet/http_jni.c:
#                    request, and the natives it registers with RegisterNatives)
#   WinBoloActivity  smallestWidthDp, imeHeightPx, showKeyboardAgain
#                    (src/android/android_device.c), shareFile
#                    (android_share.c), and the class loader http_jni.c
#                    falls back to on a worker thread
#   WinBoloNet       start and kind (src/android/android_netpath.c)
# Minification is off in build.gradle.kts; these rules are read all the
# same, so turning it on cannot break the calls above.
-keep class com.winbolo.android.WinBoloHttp { *; }
-keep class com.winbolo.android.WinBoloActivity { *; }
-keep class com.winbolo.android.WinBoloNet { *; }
