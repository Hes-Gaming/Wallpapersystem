#include <windows.h>
#include <mfapi.h>
#include <dxgi1_4.h>
#include <psapi.h>

#include <intrin.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <share.h>
#include <string>
#include <vector>

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

// --bench <seconds>: play (ignoring the pause watcher) for a 2 second warmup
// plus <seconds>, log one "bench:" summary line, then exit.
struct BenchStats
{
    bool measuring = false;
    LONGLONG start = 0;
    LONGLONG lastPresent = 0;
    LONGLONG decodeTicks = 0;
    LONGLONG renderTicks = 0;
    ULONG64 cyclesStart = 0;
    unsigned long long tscStart = 0;
    long long evalCountStart = 0;
    long long evalTicksStart = 0;
    std::vector<LONGLONG> intervals;
};

// GetProcessTimes is tick sampled and reads ~0 for short bursty work; cycle
// time is exact and counts in TSC units, so cycles / TSC delta = cores busy.
ULONG64 ProcessCycles()
{
    ULONG64 cycles = 0;
    QueryProcessCycleTime(GetCurrentProcess(), &cycles);
    return cycles;
}

void LogBench(const BenchStats& bench, LONGLONG now, LONGLONG frequency,
              const PauseWatcher& watcher, ID3D11Device* device, double startupMs)
{
    const double seconds = static_cast<double>(now - bench.start) / frequency;
    const double toMs = 1000.0 / frequency;
    std::vector<LONGLONG> sorted = bench.intervals;
    std::sort(sorted.begin(), sorted.end());
    const size_t frames = sorted.size();
    const LONGLONG median = frames ? sorted[frames / 2] : 0;
    const LONGLONG p99 = frames ? sorted[(frames * 99) / 100] : 0;
    const LONGLONG maximum = frames ? sorted.back() : 0;
    const long long late = std::count_if(sorted.begin(), sorted.end(),
                                         [&](LONGLONG v) { return v * 2 > median * 3; });

    const double cpuPct = static_cast<double>(ProcessCycles() - bench.cyclesStart) /
                          static_cast<double>(__rdtsc() - bench.tscStart) * 100.0;
    const long long evals = watcher.evaluations() - bench.evalCountStart;
    const long long evalTicks = watcher.evaluationTicks() - bench.evalTicksStart;

    PROCESS_MEMORY_COUNTERS_EX memory = {};
    GetProcessMemoryInfo(GetCurrentProcess(),
                         reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));

    DXGI_QUERY_VIDEO_MEMORY_INFO local = {}, nonLocal = {};
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIAdapter3> adapter3;
    if (device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) &&
        SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && SUCCEEDED(adapter.As(&adapter3)))
    {
        adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
        adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal);
        DXGI_ADAPTER_DESC desc = {};
        adapter->GetDesc(&desc);
        Log(L"bench adapter: %ls luid=0x%08lX", desc.Description, desc.AdapterLuid.LowPart);
    }

    const double mb = 1024.0 * 1024.0;
    Log(L"bench: seconds=%.2f frames=%llu fps=%.2f median_ms=%.3f p99_ms=%.3f max_ms=%.3f "
        L"late=%lld cpu_pct=%.2f decode_us=%.1f render_us=%.1f evals_per_s=%.1f eval_us=%.2f "
        L"watcher_cpu_pct=%.3f ws_mb=%.1f private_mb=%.1f peak_ws_mb=%.1f "
        L"gpu_local_mb=%.1f gpu_nonlocal_mb=%.1f startup_ms=%.1f",
        seconds, static_cast<unsigned long long>(frames), frames / seconds,
        median * toMs, p99 * toMs, maximum * toMs, late, cpuPct,
        frames ? bench.decodeTicks * toMs * 1000.0 / frames : 0.0,
        frames ? bench.renderTicks * toMs * 1000.0 / frames : 0.0,
        evals / seconds, evals ? evalTicks * toMs * 1000.0 / evals : 0.0,
        evalTicks * toMs / (seconds * 10.0),
        memory.WorkingSetSize / mb, memory.PrivateUsage / mb, memory.PeakWorkingSetSize / mb,
        local.CurrentUsage / mb, nonLocal.CurrentUsage / mb, startupMs);
}

// Mirrors config.md's StartWithWindows into HKCU\...\Run (per-user, no admin).
void SyncStartup(bool enable)
{
    const wchar_t* const runKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    const wchar_t* const valueName = L"WallpaperSystem";
    if (!enable)
    {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, runKey, valueName);
        return;
    }

    wchar_t exe[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return;
    const std::wstring command = L"\"" + std::wstring(exe) + L"\"";
    const LSTATUS status = RegSetKeyValueW(HKEY_CURRENT_USER, runKey, valueName, REG_SZ,
                                           command.c_str(),
                                           static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    if (status != ERROR_SUCCESS)
        Log(L"warning: failed to register startup entry (%ld)", static_cast<long>(status));
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int)
{
    const LONGLONG startQpc = QueryQpc();
    const wchar_t* benchArg = commandLine ? std::wcsstr(commandLine, L"--bench") : nullptr;
    const int benchSeconds = benchArg ? (std::max)(1, _wtoi(benchArg + 7)) : 0;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // A wallpaper should yield the CPU to whatever the user is actually running.
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

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
    if (benchSeconds == 0)
        SyncStartup(config.startWithWindows);

    // At login the Run key can fire before Explorer has created the desktop.
    for (int i = 0; i < 60 && !FindWindowW(L"Progman", nullptr); ++i)
        Sleep(500);

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
    BenchStats bench;
    double startupMs = 0.0;
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
        {
            // A view on the back buffer would make ResizeBuffers fail.
            decoder.ReleaseOutputTarget();
            renderer.HandleDisplayChange();
        }

        if (watcher.IsPaused() && benchSeconds == 0)
        {
            if (!paused)
            {
                paused = true;
                Log(L"paused (maximized application detected)");
            }

            // The watcher signals on every state change and the display poll
            // runs on resume, so there is nothing to wake up for until then.
            HANDLE wake = watcher.WakeEvent();
            MsgWaitForMultipleObjectsEx(wake ? 1 : 0, wake ? &wake : nullptr,
                                        wake ? INFINITE : 250, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            continue;
        }

        if (paused)
        {
            paused = false;
            pacingValid = false;
            lastFrameTick = GetTickCount();  // A long pause must not trip the watchdog.
            Log(L"resumed");
        }

        const LONGLONG decodeStart = QueryQpc();
        DecodedFrame frame = {};
        const DecodeResult result = decoder.DecodeNext(frame);
        const LONGLONG decodeTicks = QueryQpc() - decodeStart;
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
            const LONGLONG renderStart = QueryQpc();
            HRESULT presentResult = frame.hardware
                ? decoder.BlitTo(renderer.backBuffer(), renderer.width(), renderer.height())
                : renderer.RenderFrame(frame.srv, frame.srvUV, decoder.width(), decoder.height());
            if (SUCCEEDED(presentResult))
                presentResult = renderer.Present();
            const LONGLONG presented = QueryQpc();
            if (SUCCEEDED(presentResult))
                lastFrameTick = GetTickCount();  // Persistent failures trip the 5 s watchdog.

            if (benchSeconds > 0 && firstFrame)
            {
                if (bench.measuring)
                {
                    bench.intervals.push_back(presented - bench.lastPresent);
                    bench.decodeTicks += decodeTicks;
                    bench.renderTicks += presented - renderStart;
                    if (presented - bench.start >= benchSeconds * qpcFrequency)
                    {
                        LogBench(bench, presented, qpcFrequency, watcher, renderer.device(),
                                 startupMs);
                        break;
                    }
                }
                else if (presented >= bench.start)
                {
                    bench.measuring = true;
                    bench.start = presented;
                    bench.cyclesStart = ProcessCycles();
                    bench.tscStart = __rdtsc();
                    bench.evalCountStart = watcher.evaluations();
                    bench.evalTicksStart = watcher.evaluationTicks();
                    bench.intervals.reserve(static_cast<size_t>(benchSeconds) * 240);
                }
                bench.lastPresent = presented;
            }

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
                startupMs = static_cast<double>(presented - startQpc) * 1000.0 / qpcFrequency;
                bench.start = presented + 2 * qpcFrequency;
                Log(L"first frame presented (%.1f ms after start)", startupMs);
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
