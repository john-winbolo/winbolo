/*
 * android_dialog_stubs.c - Stub implementations for dialog functions
 * not yet implemented on Android.
 *
 * The ImGui dialog backend (dialog_backend.c) is used with Android-specific
 * implementations of imguiWelcomeShow and imguiGameSetupShow provided by
 * android_welcome.cpp and android_gamesetup.cpp respectively.
 */

/* Tracker setup stub — not applicable on Android */
int imguiTrackerSetupShow(void) {
  return 0;
}
