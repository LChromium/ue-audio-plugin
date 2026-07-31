from __future__ import annotations

import argparse
import ctypes
import subprocess
import sys
import time
from ctypes import wintypes
from pathlib import Path

from PIL import ImageGrab, ImageStat


PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
SW_RESTORE = 9
HWND_TOPMOST = -1
HWND_NOTOPMOST = -2
SWP_NOSIZE = 0x0001
SWP_NOMOVE = 0x0002
SWP_SHOWWINDOW = 0x0040

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)


class Point(ctypes.Structure):
    _fields_ = [("x", wintypes.LONG), ("y", wintypes.LONG)]


class Rect(ctypes.Structure):
    _fields_ = [
        ("left", wintypes.LONG),
        ("top", wintypes.LONG),
        ("right", wintypes.LONG),
        ("bottom", wintypes.LONG),
    ]


user32.EnumWindows.argtypes = [
    ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM),
    wintypes.LPARAM,
]
user32.EnumWindows.restype = wintypes.BOOL
user32.IsWindowVisible.argtypes = [wintypes.HWND]
user32.IsWindowVisible.restype = wintypes.BOOL
user32.GetWindowTextLengthW.argtypes = [wintypes.HWND]
user32.GetWindowTextLengthW.restype = ctypes.c_int
user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.GetWindowTextW.restype = ctypes.c_int
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.GetWindowThreadProcessId.restype = wintypes.DWORD
user32.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(Rect)]
user32.GetClientRect.restype = wintypes.BOOL
user32.ClientToScreen.argtypes = [wintypes.HWND, ctypes.POINTER(Point)]
user32.ClientToScreen.restype = wintypes.BOOL
user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
user32.ShowWindow.restype = wintypes.BOOL
user32.SetForegroundWindow.argtypes = [wintypes.HWND]
user32.SetForegroundWindow.restype = wintypes.BOOL
user32.BringWindowToTop.argtypes = [wintypes.HWND]
user32.BringWindowToTop.restype = wintypes.BOOL
user32.GetForegroundWindow.argtypes = []
user32.GetForegroundWindow.restype = wintypes.HWND
user32.AttachThreadInput.argtypes = [wintypes.DWORD, wintypes.DWORD, wintypes.BOOL]
user32.AttachThreadInput.restype = wintypes.BOOL
user32.SetWindowPos.argtypes = [
    wintypes.HWND,
    wintypes.HWND,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_int,
    wintypes.UINT,
]
user32.SetWindowPos.restype = wintypes.BOOL
kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel32.OpenProcess.restype = wintypes.HANDLE
kernel32.QueryFullProcessImageNameW.argtypes = [
    wintypes.HANDLE,
    wintypes.DWORD,
    wintypes.LPWSTR,
    ctypes.POINTER(wintypes.DWORD),
]
kernel32.QueryFullProcessImageNameW.restype = wintypes.BOOL
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.CloseHandle.restype = wintypes.BOOL
kernel32.GetCurrentThreadId.argtypes = []
kernel32.GetCurrentThreadId.restype = wintypes.DWORD


def process_image_name(process_id: int) -> str:
    handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, process_id)
    if not handle:
        return ""
    try:
        capacity = wintypes.DWORD(32768)
        buffer = ctypes.create_unicode_buffer(capacity.value)
        if not kernel32.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(capacity)):
            return ""
        return Path(buffer.value).name.lower()
    finally:
        kernel32.CloseHandle(handle)


def visible_unreal_windows() -> list[tuple[int, int, str]]:
    windows: list[tuple[int, int, str]] = []
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    @callback_type
    def collect(window_handle: int, _parameter: int) -> bool:
        if not user32.IsWindowVisible(window_handle):
            return True
        title_length = user32.GetWindowTextLengthW(window_handle)
        if title_length <= 0:
            return True
        title_buffer = ctypes.create_unicode_buffer(title_length + 1)
        user32.GetWindowTextW(window_handle, title_buffer, len(title_buffer))
        process_id = wintypes.DWORD()
        user32.GetWindowThreadProcessId(window_handle, ctypes.byref(process_id))
        if process_image_name(process_id.value) == "unrealeditor.exe":
            windows.append((window_handle, process_id.value, title_buffer.value))
        return True

    if not user32.EnumWindows(collect, 0):
        raise ctypes.WinError(ctypes.get_last_error())
    return windows


def client_bounds(window_handle: int) -> tuple[int, int, int, int]:
    rect = Rect()
    origin = Point()
    if not user32.GetClientRect(window_handle, ctypes.byref(rect)):
        raise ctypes.WinError(ctypes.get_last_error())
    if not user32.ClientToScreen(window_handle, ctypes.byref(origin)):
        raise ctypes.WinError(ctypes.get_last_error())
    width = rect.right - rect.left
    height = rect.bottom - rect.top
    return origin.x, origin.y, width, height


def bring_to_foreground(window_handle: int) -> None:
    current_thread_id = kernel32.GetCurrentThreadId()
    target_thread_id = user32.GetWindowThreadProcessId(window_handle, None)
    foreground_window = user32.GetForegroundWindow()
    foreground_thread_id = (
        user32.GetWindowThreadProcessId(foreground_window, None) if foreground_window else 0
    )
    attached_threads: list[int] = []
    try:
        for thread_id in (foreground_thread_id, target_thread_id):
            if thread_id and thread_id != current_thread_id:
                if user32.AttachThreadInput(current_thread_id, thread_id, True):
                    attached_threads.append(thread_id)
        user32.ShowWindow(window_handle, SW_RESTORE)
        user32.BringWindowToTop(window_handle)
        user32.SetForegroundWindow(window_handle)
    finally:
        for thread_id in reversed(attached_threads):
            user32.AttachThreadInput(current_thread_id, thread_id, False)


def capture_metrics(window_handle: int, screenshot_path: Path) -> tuple[int, int, float, float, float]:
    user32.ShowWindow(window_handle, SW_RESTORE)
    if not user32.SetWindowPos(
        window_handle,
        HWND_TOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW,
    ):
        raise ctypes.WinError(ctypes.get_last_error())
    foreground_deadline = time.monotonic() + 5.0
    while time.monotonic() < foreground_deadline:
        bring_to_foreground(window_handle)
        if user32.GetForegroundWindow() == window_handle:
            break
        time.sleep(0.25)
    time.sleep(2.0)
    if user32.GetForegroundWindow() != window_handle:
        # Foreground activation is governed by Windows focus-stealing policy and can
        # be denied even after the exact PID-owned window is topmost. The topmost
        # placement plus PID-scoped bounds are sufficient for an unoccluded capture.
        user32.SetWindowPos(
            window_handle,
            HWND_TOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW,
        )
        time.sleep(0.5)
    left, top, width, height = client_bounds(window_handle)
    if width < 320 or height < 200:
        raise RuntimeError(f"Unreal client area is unexpectedly small: {width}x{height}.")

    screenshot = ImageGrab.grab(
        bbox=(left, top, left + width, top + height),
        all_screens=True,
    ).convert("RGB")
    screenshot_path.parent.mkdir(parents=True, exist_ok=True)
    screenshot.save(screenshot_path)
    user32.SetWindowPos(
        window_handle,
        HWND_NOTOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW,
    )

    sample = screenshot.resize((max(width // 8, 1), max(height // 8, 1))).convert("L")
    histogram = sample.histogram()
    sample_count = sum(histogram)
    non_black_count = sum(histogram[13:])
    statistics = ImageStat.Stat(sample)
    return (
        width,
        height,
        non_black_count / max(sample_count, 1),
        statistics.mean[0],
        statistics.stddev[0],
    )


def stop_process(process_id: int) -> None:
    try:
        subprocess.run(
            ["taskkill.exe", "/PID", str(process_id), "/T", "/F"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
            timeout=15.0,
        )
    except subprocess.TimeoutExpired:
        # Cleanup must never hold the validator open after evidence is written.
        pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--window-timeout", type=float, default=60.0)
    parser.add_argument(
        "--screenshot",
        type=Path,
        default=Path("Saved/Validation/runtime-scene.png"),
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    repo_root = Path(__file__).resolve().parent.parent
    screenshot_path = args.screenshot
    if not screenshot_path.is_absolute():
        screenshot_path = repo_root / screenshot_path

    existing_process_ids = {process_id for _, process_id, _ in visible_unreal_windows()}
    uv_executable = Path(r"C:\Users\splay\AppData\Roaming\Python\Python313\Scripts\uv.exe")
    launch_log_path = screenshot_path.with_suffix(".launch.log")
    launch_log_path.parent.mkdir(parents=True, exist_ok=True)
    runtime_process_id: int | None = None

    with launch_log_path.open("w", encoding="utf-8") as launch_log:
        validation_process = subprocess.Popen(
            [str(uv_executable), "run", r"script\launch_runtime_validation.py"],
            cwd=repo_root,
            stdout=launch_log,
            stderr=subprocess.STDOUT,
            creationflags=subprocess.CREATE_NO_WINDOW,
        )
        try:
            deadline = time.monotonic() + args.window_timeout
            runtime_window: tuple[int, int, str] | None = None
            while time.monotonic() < deadline:
                candidates = [
                    window
                    for window in visible_unreal_windows()
                    if window[1] not in existing_process_ids
                ]
                if candidates:
                    runtime_window = candidates[0]
                    break
                if validation_process.poll() is not None:
                    break
                time.sleep(0.25)

            if runtime_window is None:
                print(
                    f"VISIBLE_SCENE_FAIL no Unreal runtime window appeared within "
                    f"{args.window_timeout:.1f} seconds; launch_log='{launch_log_path}'.",
                    file=sys.stderr,
                )
                return 1

            window_handle, runtime_process_id, title = runtime_window
            width, height, non_black_ratio, mean_luma, luma_stddev = capture_metrics(
                window_handle,
                screenshot_path,
            )
            print(
                "VISIBLE_SCENE_METRICS "
                f"pid={runtime_process_id} title={title!r} size={width}x{height} "
                f"non_black_ratio={non_black_ratio:.4f} mean_luma={mean_luma:.2f} "
                f"luma_stddev={luma_stddev:.2f} screenshot='{screenshot_path}'"
            )

            if non_black_ratio < 0.10 or luma_stddev < 8.0:
                print(
                    "VISIBLE_SCENE_FAIL runtime window is black or lacks a visibly rendered scene.",
                    file=sys.stderr,
                )
                return 1

            print("VISIBLE_SCENE_PASS runtime window contains a visibly rendered scene.")
            return 0
        finally:
            if validation_process.poll() is None:
                validation_process.terminate()
                try:
                    validation_process.wait(timeout=5.0)
                except subprocess.TimeoutExpired:
                    validation_process.kill()
            if runtime_process_id is not None:
                stop_process(runtime_process_id)
            for _, process_id, _ in visible_unreal_windows():
                if process_id not in existing_process_ids:
                    stop_process(process_id)


if __name__ == "__main__":
    raise SystemExit(main())
