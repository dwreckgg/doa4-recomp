"""A stand-in for a game copy that was just relaunched (Task 12 fix round Q3): for its first FAKE_WINDOW_AFTER_S seconds
(default 3) it has no window, so a close request (taskkill without /F) cannot reach it; then it creates a hidden top-level
window and exits when that window gets WM_CLOSE. It writes "closed" or nothing into FAKE_RESULT (a file) as it ends, so a
test can tell a close from a /F. It lives at most 60 s.
  py -3 -I fake_late_window.py
"""
import ctypes
import os
import sys
import time
from ctypes import wintypes

WM_CLOSE, WM_DESTROY = 0x0010, 0x0002
user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
LRESULT = ctypes.c_ssize_t
WNDPROC = ctypes.WINFUNCTYPE(LRESULT, wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)
user32.DefWindowProcW.restype = LRESULT
user32.DefWindowProcW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user32.CreateWindowExW.restype = wintypes.HWND
user32.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD, ctypes.c_int,
                                   ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.HWND, wintypes.HMENU,
                                   wintypes.HINSTANCE, wintypes.LPVOID]


class WNDCLASSW(ctypes.Structure):
    _fields_ = [("style", wintypes.UINT), ("lpfnWndProc", WNDPROC), ("cbClsExtra", ctypes.c_int),
                ("cbWndExtra", ctypes.c_int), ("hInstance", wintypes.HINSTANCE), ("hIcon", wintypes.HICON),
                ("hCursor", wintypes.HANDLE), ("hbrBackground", wintypes.HBRUSH), ("lpszMenuName", wintypes.LPCWSTR),
                ("lpszClassName", wintypes.LPCWSTR)]


def result(text):
    path = os.environ.get("FAKE_RESULT")
    if path:
        with open(path, "w") as f:
            f.write(text)


@WNDPROC
def proc(hwnd, msg, wp, lp):
    if msg == WM_CLOSE:
        result("closed")
        user32.DestroyWindow(hwnd)
        return 0
    if msg == WM_DESTROY:
        user32.PostQuitMessage(0)
        return 0
    return user32.DefWindowProcW(hwnd, msg, wp, lp)


def main():
    t0 = time.time()
    time.sleep(float(os.environ.get("FAKE_WINDOW_AFTER_S", "3")))
    hinst = kernel32.GetModuleHandleW(None)
    wc = WNDCLASSW()
    wc.lpfnWndProc = proc
    wc.hInstance = hinst
    wc.lpszClassName = "np_fake_late_window"
    user32.RegisterClassW(ctypes.byref(wc))
    hwnd = user32.CreateWindowExW(0, wc.lpszClassName, "fake", 0, 0, 0, 10, 10, None, None, hinst, None)
    if not hwnd:
        result("no window")
        return 2
    msg = wintypes.MSG()
    user32.SetTimer(hwnd, 1, 200, None)
    while user32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
        if time.time() - t0 > 60:
            break
        user32.TranslateMessage(ctypes.byref(msg))
        user32.DispatchMessageW(ctypes.byref(msg))
    return 0


if __name__ == "__main__":
    sys.exit(main())
