#include "util.h"

// Set window size, while accounting for:
// - how DPI affects the size of the non-client area
// - whether the window has a menu
// - any other styles that affect the non-client area
// - different rendering of the non-client area in specific Windows versions
// Returns a nonzero value on success, zero (FALSE) on failure
// Note: This function requires Windows 10, version 1607 or later
// for GetDpiForWindow and AdjustWindowRectExForDpi to be available.
// Parameters:
//   hWnd     - Handle to the window
//   clientW  - Desired client area width
//   clientH  - Desired client area height
//   flags    - SetWindowPos flags
BOOL SizeWindowForClient(HWND hWnd, int clientW, int clientH, UINT flags) {
  RECT rc = { 0, 0, clientW, clientH };
  DWORD style = (DWORD)GetWindowLongPtr(hWnd, GWL_STYLE);
  DWORD ex = (DWORD)GetWindowLongPtr(hWnd, GWL_EXSTYLE);
  BOOL  hasMenu = (GetMenu(hWnd) != NULL);

  UINT dpi = GetDpiForWindow(hWnd);
  if (!AdjustWindowRectExForDpi(&rc, style, hasMenu, ex, dpi)) {
    // failed; fall back to the old, hardcoded logic.
    return SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, clientW + 4, clientH + 42, flags);
  }

  return SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, rc.right - rc.left, rc.bottom - rc.top, flags);
}
