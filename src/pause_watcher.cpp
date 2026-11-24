#include "pause_watcher.h"

#include <dwmapi.h>

#include <cwchar>
#include <system_error>

namespace
{

bool IsShellClassName(const wchar_t* className)
{
    return std::wcscmp(className, L"Progman") == 0 ||
           std::wcscmp(className, L"WorkerW") == 0 ||
           std::wcscmp(className, L"Shell_TrayWnd") == 0 ||
           std::wcscmp(className, L"Shell_SecondaryTrayWnd") == 0;
}

bool ContainsRect(const RECT& outer, const RECT& inner)
{
    return outer.left <= inner.left && outer.top <= inner.top &&
           outer.right >= inner.right && outer.bottom >= inner.bottom;
}

} // namespace

PauseWatcher::~PauseWatcher()
{
    Stop();
}

bool PauseWatcher::Start(HWND hostWindow)
{
    Stop();

    hostWindow_ = hostWindow;
    stop_.store(false, std::memory_order_relaxed);
    paused_.store(false, std::memory_order_relaxed);

    wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (wakeEvent_ == nullptr)
        return false;

    try
    {
        thread_ = std::thread(&PauseWatcher::ThreadMain, this);
    }
    catch (const std::system_error&)
    {
        CloseHandle(wakeEvent_);
        wakeEvent_ = nullptr;
        return false;
    }
    catch (...)
    {
        CloseHandle(wakeEvent_);
        wakeEvent_ = nullptr;
        return false;
    }

    return true;
}

void PauseWatcher::Stop()
{
    if (thread_.joinable())
    {
        stop_.store(true, std::memory_order_relaxed);
        thread_.join();
    }

    if (wakeEvent_ != nullptr)
    {
        CloseHandle(wakeEvent_);
        wakeEvent_ = nullptr;
    }
}

void PauseWatcher::ThreadMain()
{
    // Plain polling: no WinEvent hooks, so mouse movement in games never wakes
    // this thread. ponytail: Sleep rounds to the 15.6 ms timer tick (~16 Hz), fine here.
    while (!stop_.load(std::memory_order_relaxed))
    {
        Evaluate();
        Sleep(50);
    }
}

void PauseWatcher::Evaluate()
{
    LARGE_INTEGER evalStart = {};
    QueryPerformanceCounter(&evalStart);
    bool nowPaused = false;

    MONITORINFO monitorInfo = { sizeof(monitorInfo) };

    if (GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY),
                        &monitorInfo))
    {
        const DWORD ownProcess = GetCurrentProcessId();
        const HWND shellWindow = GetShellWindow();

        for (HWND window = GetTopWindow(nullptr);
             window != nullptr;
             window = GetWindow(window, GW_HWNDNEXT))
        {
            if (window == hostWindow_ || window == shellWindow)
                continue;

            if (IsWindowVisible(window) == FALSE || IsIconic(window) != FALSE)
                continue;

            RECT windowRect = {};
            if (GetWindowRect(window, &windowRect) == FALSE)
                continue;
            if (windowRect.right <= windowRect.left || windowRect.bottom <= windowRect.top)
                continue;

            // Fullscreen only: a maximized window stops at the taskbar.
            if (!ContainsRect(windowRect, monitorInfo.rcMonitor))
                continue;

            // Click-through overlays (Game Bar, Discord, GPU overlays) cover the
            // screen without hiding it.
            if (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TRANSPARENT)
                continue;

            // A captioned maximized window also covers the monitor when the taskbar
            // auto-hides; that is maximized, not fullscreen.
            if (IsZoomed(window) &&
                (GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION) == WS_CAPTION)
                continue;

            // Expensive checks only for windows that actually cover the monitor.
            DWORD processId = 0;
            GetWindowThreadProcessId(window, &processId);
            if (processId == ownProcess)
                continue;

            wchar_t className[256] = {};
            const int classLength = GetClassNameW(
                window, className,
                static_cast<int>(sizeof(className) / sizeof(className[0])));
            if (classLength > 0 && IsShellClassName(className))
                continue;

            BOOL cloaked = FALSE;
            const HRESULT dwmResult =
                DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked,
                                      static_cast<DWORD>(sizeof(cloaked)));
            if (SUCCEEDED(dwmResult) && cloaked != FALSE)
                continue;

            nowPaused = true;
            break;
        }
    }

    if (nowPaused != paused_.load(std::memory_order_relaxed))
    {
        paused_.store(nowPaused, std::memory_order_relaxed);
        if (wakeEvent_ != nullptr)
            SetEvent(wakeEvent_);
    }

    LARGE_INTEGER evalEnd = {};
    QueryPerformanceCounter(&evalEnd);
    evalCount_.fetch_add(1, std::memory_order_relaxed);
    evalTicks_.fetch_add(evalEnd.QuadPart - evalStart.QuadPart, std::memory_order_relaxed);
}
