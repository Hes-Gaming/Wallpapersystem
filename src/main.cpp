#include <windows.h>
#include <mfapi.h>

#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <string>

#include "config.h"
#include "video_decoder.h"
#include "renderer.h"
#include "pause_watcher.h"

namespace
{

FILE* g_log = nullptr;

void OpenLog()
{
    const std::wstring path = GetExeDirectory() + L"\\wallpaper.log";
    g_log = _wfsopen(path.c_str(), L"a, ccs=UTF-8", _SH_DENYNO);
}

void CloseLog()
{
    if (g_log)
    {
        std::fclose(g_log);
        g_log = nullptr;
    }
}

void Log(const wchar_t* format, ...)
{
    if (!g_log)
        return;

    SYSTEMTIME now = {};
    GetLocalTime(&now);
    std::fwprintf(g_log, L"[%02d:%02d:%02d.%03d] ",
                  now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);

    va_list args;
    va_start(args, format);
    std::vfwprintf(g_log, format, args);
    va_end(args);

    std::fwprintf(g_log, L"\n");
    std::fflush(g_log);
}

void Fatal(const std::wstring& message)
{
    Log(L"fatal: %ls", message.c_str());
    MessageBoxW(nullptr, message.c_str(), L"Wallpaper System", MB_OK | MB_ICONERROR);
}

LONGLONG QueryQpc()
{
    LARGE_INTEGER counter = {};
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

LONGLONG HundredNsToQpc(LONGLONG hundredNanoseconds, LONGLONG frequency)
{
    return static_cast<LONGLONG>(static_cast<double>(hundredNanoseconds) *
                                 static_cast<double>(frequency) / 10000000.0);
}

void WaitUntilQpc(LONGLONG target, HANDLE timer, LONGLONG frequency)
{
    for (;;)
    {
        const LONGLONG remaining = target - QueryQpc();
        if (remaining <= 0)
            return;

        if (timer)
        {
            LARGE_INTEGER due = {};
            due.QuadPart = -(remaining * 10000000LL / frequency);
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
            {
                WaitForSingleObject(timer, 100);
                continue;
            }
        }

        const DWORD milliseconds = static_cast<DWORD>(remaining * 1000LL / frequency);
        Sleep(milliseconds > 1 ? milliseconds - 1 : 0);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HANDLE instanceMutex = CreateMutexW(nullptr, FALSE, L"Local\\WallpaperSystemInstance");
    if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(instanceMutex);
        return 0;
    }

    OpenLog();
    Log(L"starting");

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)))
    {
        CloseLog();
        return 1;
    }

    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET)))
    {
        Fatal(L"Failed to initialize Media Foundation.");
        CoUninitialize();
        CloseLog();
        return 1;
    }

    const AppConfig config = LoadConfig();
    if (!config.valid)
    {
        Fatal(config.error);
        MFShutdown();
        CoUninitialize();
        CloseLog();
        return 1;
    }
    Log(L"wallpaper: %ls", config.wallpaperPath.c_str());
    Log(L"process id: %lu", static_cast<unsigned long>(GetCurrentProcessId()));

    Renderer renderer;
    if (!renderer.Create(instance))
    {
        Log(L"renderer failure: stage=%d code=0x%08lX",
            renderer.failureStage(), static_cast<unsigned long>(renderer.failureCode()));
        Fatal(L"Failed to create the wallpaper renderer.");
        renderer.Destroy();
        MFShutdown();
        CoUninitialize();
        CloseLog();
        return 1;
    }

    VideoDecoder decoder;
    if (!decoder.Open(config.wallpaperPath, renderer.device()))
    {
        Fatal(L"Failed to open the wallpaper video. Only H.264 .mp4 files are supported.");
        renderer.Destroy();
        MFShutdown();
        CoUninitialize();
        CloseLog();
        return 1;
    }
    Log(L"wallpaper window %p attached to WorkerW %p", renderer.window(), renderer.workerWindow());
    Log(L"decoder ready (%ls, %dx%d) videoSupport=%d",
        decoder.hardware() ? L"hardware" : L"software",
        decoder.width(), decoder.height(), renderer.hasVideoSupport() ? 1 : 0);
    if (!decoder.hardware())
        Log(L"hardware probe failed: stage=%d hr=0x%08lX",
            decoder.hardwareFailureStage(),
            static_cast<unsigned long>(decoder.hardwareFailureCode()));

    PauseWatcher watcher;
    if (!watcher.Start(renderer.window()))
        Log(L"warning: fullscreen watcher failed to start");

    LARGE_INTEGER counterFrequency = {};
    QueryPerformanceFrequency(&counterFrequency);
    const LONGLONG qpcFrequency = counterFrequency.QuadPart > 0 ? counterFrequency.QuadPart : 1;

    HANDLE frameTimer = CreateWaitableTimerExW(nullptr, nullptr,
                                               CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                               TIMER_ALL_ACCESS);
    if (!frameTimer)
        frameTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

    bool running = true;
    bool paused = false;
    bool firstFrame = false;
    bool pacingValid = false;
    bool presentErrorLogged = false;
    LONGLONG pacingAnchor = 0;
    DWORD lastFrameTick = GetTickCount();
    DWORD lastParentCheck = GetTickCount();

    while (running)
    {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
            {
                running = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!running)
            break;

        if (renderer.CheckDisplayChanged())
            renderer.HandleDisplayChange();

        if (watcher.IsPaused())
        {
            if (!paused)
            {
                paused = true;
                Log(L"paused (maximized application detected)");
            }

            HANDLE wake = watcher.WakeEvent();
            MsgWaitForMultipleObjectsEx(wake ? 1 : 0, wake ? &wake : nullptr,
                                        250, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            continue;
        }

        if (paused)
        {
            paused = false;
            pacingValid = false;
            Log(L"resumed");
        }

        DecodedFrame frame = {};
        const DecodeResult result = decoder.DecodeNext(frame);
        if (result == DecodeResult::Frame)
        {
            LONGLONG pts = decoder.timestamp();
            if (pts < 0)
                pts = 0;

            const LONGLONG now = QueryQpc();
            if (!pacingValid)
            {
                pacingAnchor = now - HundredNsToQpc(pts, qpcFrequency);
                pacingValid = true;
            }

            LONGLONG target = pacingAnchor + HundredNsToQpc(pts, qpcFrequency);
            if (now - target > qpcFrequency / 10)
            {
                pacingAnchor = now - HundredNsToQpc(pts, qpcFrequency);
                target = now;
            }

            if (target > now)
                WaitUntilQpc(target, frameTimer, qpcFrequency);

            renderer.WaitForFrameLatency();
            const HRESULT presentResult = renderer.RenderFrame(frame.srv, frame.srvUV,
                                                                decoder.width(), decoder.height(),
                                                                frame.planarUV);
            lastFrameTick = GetTickCount();

            if (FAILED(presentResult))
            {
                if (!presentErrorLogged)
                {
                    Log(L"present failed: hr=0x%08lX", static_cast<unsigned long>(presentResult));
                    presentErrorLogged = true;
                }
                if (presentResult == DXGI_ERROR_DEVICE_REMOVED ||
                    presentResult == DXGI_ERROR_DEVICE_RESET)
                {
                    Log(L"graphics device lost, exiting");
                    break;
                }
            }

            if (!firstFrame)
            {
                firstFrame = true;
                Log(L"first frame presented");
            }
        }
        else if (result == DecodeResult::EndOfStream)
        {
            if (!decoder.Rewind())
            {
                Log(L"warning: failed to loop the video");
                Sleep(200);
            }
            else
            {
                pacingValid = false;
            }
        }
        else
        {
            Sleep(1);
        }

        if (GetTickCount() - lastFrameTick > 5000)
        {
            Log(L"no frames decoded for 5 seconds, exiting");
            break;
        }

        const DWORD now = GetTickCount();
        if (now - lastParentCheck > 2000)
        {
            renderer.EnsureDesktopParent();
            lastParentCheck = now;
        }
    }

    if (frameTimer)
        CloseHandle(frameTimer);

    watcher.Stop();
    decoder.Close();
    renderer.Destroy();
    MFShutdown();
    CoUninitialize();
    Log(L"stopped");
    CloseLog();
    if (instanceMutex)
        CloseHandle(instanceMutex);
    return 0;
}
