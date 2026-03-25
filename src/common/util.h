#include <WinSock2.h>

// Set window size, while accounting for:
// - how DPI affects the size of the non-client area
// - whether the window has a menu
// - any other styles that affect the non-client area
// - different rendering of the non-client area in specific Windows versions
// Returns TRUE on success, FALSE on failure
// Note: This function requires Windows 10, version 1607 or later
// for GetDpiForWindow and AdjustWindowRectExForDpi to be available.
// Parameters:
//   hWnd     - Handle to the window
//   clientW  - Desired client area width
//   clientH  - Desired client area height
//   flags    - SetWindowPos flags
extern BOOL SizeWindowForClient(HWND hWnd, int clientW, int clientH, UINT flags);