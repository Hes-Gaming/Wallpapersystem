#include "pause_watcher.h"

#include <dwmapi.h>

#include <cwchar>
#include <system_error>

namespace
{

PauseWatcher* g_watcher = nullptr;

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

void CALLBACK PauseWatcher::WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                          LONG idObject, LONG, DWORD, DWORD)
{
    if (g_watcher == nullptr)
        return;

    if (event == EVENT_SYSTEM_FOREGROUND ||
        (event == EVENT_OBJECT_LOCATIONCHANGE && idObject == OBJID_WINDOW &&
         hwnd == GetForegroundWindow()))
    {
        g_watcher->dirty_ = true;
    }
}

void PauseWatcher::ThreadMain()
{
    threadId_ = GetCurrentThreadId();
    g_watcher = this;

    hookForeground_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                      nullptr, &PauseWatcher::WinEventProc, 0, 0,
                                      WINEVENT_OUTOFCONTEXT);

    hookLocation_ = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
                                    nullptr, &PauseWatcher::WinEventProc, 0, 0,
                                    WINEVENT_OUTOFCONTEXT);

    Evaluate();
    ULONGLONG lastEval = GetTickCount64();

    bool quit = false;
    while (!stop_.load(std::memory_order_relaxed) && !quit)
    {
        // Wait only for the rest of the 500 ms poll interval; irrelevant events
        // (cursor moves) wake us but must not force an Evaluate.
        const ULONGLONG elapsed = GetTickCount64() - lastEval;
        MsgWaitForMultipleObjectsEx(0, nullptr,
                                    elapsed < 500 ? static_cast<DWORD>(500 - elapsed) : 0,
                                    QS_ALLINPUT, MWMO_INPUTAVAILABLE);

        MSG message = {};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
            {
                quit = true;
                break;
            }

            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        if (quit || stop_.load(std::memory_order_relaxed))
            break;

        if (dirty_ || GetTickCount64() - lastEval >= 500)
        {
            dirty_ = false;
            Evaluate();
            lastEval = GetTickCount64();
        }
    }

    if (hookForeground_ != nullptr)
    {
        UnhookWinEvent(hookForeground_);
        hookForeground_ = nullptr;
    }

    if (hookLocation_ != nullptr)
    {
        UnhookWinEvent(hookLocation_);
        hookLocation_ = nullptr;
    }

    g_watcher = nullptr;
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

            if (!ContainsRect(windowRect, monitorInfo.rcWork) &&
                !ContainsRect(windowRect, monitorInfo.rcMonitor))
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
