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

struct PrimaryMonitorSearch
{
    MONITORINFO info;
    bool found;
};

BOOL CALLBACK EnumPrimaryMonitorProc(HMONITOR monitor, HDC, LPRECT, LPARAM param)
{
    MONITORINFO info = { sizeof(info) };
    if (GetMonitorInfoW(monitor, &info) && (info.dwFlags & MONITORINFOF_PRIMARY) != 0)
    {
        PrimaryMonitorSearch* search = reinterpret_cast<PrimaryMonitorSearch*>(param);
        search->info = info;
        search->found = true;
        return FALSE;
    }
    return TRUE;
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
        g_watcher->Evaluate();
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

    bool quit = false;
    while (!stop_.load(std::memory_order_relaxed) && !quit)
    {
        MsgWaitForMultipleObjectsEx(0, nullptr, 500, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

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

        Evaluate();
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
    bool nowPaused = false;

    PrimaryMonitorSearch primary = {};
    primary.info.cbSize = static_cast<DWORD>(sizeof(primary.info));

    EnumDisplayMonitors(nullptr, nullptr, EnumPrimaryMonitorProc,
                        reinterpret_cast<LPARAM>(&primary));

    if (primary.found)
    {
        const MONITORINFO& monitorInfo = primary.info;
        const DWORD ownProcess = GetCurrentProcessId();

        for (HWND window = GetTopWindow(nullptr);
             window != nullptr;
             window = GetWindow(window, GW_HWNDNEXT))
        {
            if (window == hostWindow_ || window == GetShellWindow())
                continue;

            DWORD processId = 0;
            GetWindowThreadProcessId(window, &processId);
            if (processId == ownProcess)
                continue;

            if (IsWindowVisible(window) == FALSE || IsIconic(window) != FALSE)
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

            RECT windowRect = {};
            if (GetWindowRect(window, &windowRect) == FALSE)
                continue;
            if (windowRect.right <= windowRect.left || windowRect.bottom <= windowRect.top)
                continue;

            if (ContainsRect(windowRect, monitorInfo.rcWork) ||
                ContainsRect(windowRect, monitorInfo.rcMonitor))
            {
                nowPaused = true;
                break;
            }
        }
    }

    if (nowPaused != paused_.load(std::memory_order_relaxed))
    {
        paused_.store(nowPaused, std::memory_order_relaxed);
        if (wakeEvent_ != nullptr)
            SetEvent(wakeEvent_);
    }
}
