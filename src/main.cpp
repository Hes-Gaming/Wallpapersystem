#include <windows.h>
#include <mfapi.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <intrin.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <share.h>
#include <string>
#include <thread>
#include <vector>

#include "clear_taskbar.h"
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

// Writes an HKCU DWORD; returns true only when the value actually changed.
bool SetUserDword(const wchar_t* key, const wchar_t* name, DWORD wanted)
{
    DWORD current = 0;
    DWORD size = sizeof(current);
    if (RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, nullptr, &current,
                     &size) == ERROR_SUCCESS && current == wanted)
        return false;

    const LSTATUS status =
        RegSetKeyValueW(HKEY_CURRENT_USER, key, name, REG_DWORD, &wanted, sizeof(wanted));
    if (status != ERROR_SUCCESS)
    {
        Log(L"warning: failed to set %ls (%ld)", name, static_cast<long>(status));
        return false;
    }
    return true;
}

// Mirrors config.md's Transparency into Windows' own "Transparency effects"
// setting (Start, taskbar, title bars, borders). When on, the accent color tint
// on Start/taskbar and title bars is turned off too, since it hides the wallpaper.
// With a clear taskbar the Start/taskbar tint only colors Start, so it is left to
// the user's Windows setting. Broadcasts only on change.
void SyncTransparency(bool enable, bool clearTaskbar)
{
    const wchar_t* const personalize =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
    bool changed = SetUserDword(personalize, L"EnableTransparency", enable ? 1 : 0);
    if (enable)
    {
        if (!clearTaskbar)
            changed |= SetUserDword(personalize, L"ColorPrevalence", 0);
        changed |= SetUserDword(L"Software\\Microsoft\\Windows\\DWM", L"ColorPrevalence", 0);
    }
    if (!changed)
        return;
    // Explorer and DWM re-read the theme on this broadcast, same as the Settings toggle.
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                        reinterpret_cast<LPARAM>(L"ImmersiveColorSet"), SMTO_ABORTIFHUNG, 1000,
                        nullptr);
    Log(L"transparency effects %ls", enable ? L"enabled" : L"disabled");
}

constexpr wchar_t kClearTaskbarDll[] = L"ClearTaskbar.dll";

DWORD TaskbarProcessId()
{
    DWORD pid = 0;
    const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    return taskbar && GetWindowThreadProcessId(taskbar, &pid) ? pid : 0;
}

bool ModuleLoaded(DWORD pid, const wchar_t* name)
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;
    MODULEENTRY32W entry = { sizeof(entry) };
    bool found = false;
    for (BOOL ok = Module32FirstW(snapshot, &entry); ok && !found;
         ok = Module32NextW(snapshot, &entry))
        found = _wcsicmp(entry.szModule, name) == 0;
    CloseHandle(snapshot);
    return found;
}

// Has XAML diagnostics load ClearTaskbar.dll into Explorer (TranslucentTB's method).
HRESULT InjectClearTaskbarOnThisThread(DWORD pid)
{
    using InitializeXamlDiagnosticsExFn =
        HRESULT(WINAPI*)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, CLSID, LPCWSTR);
    static const HMODULE xaml =
        LoadLibraryExW(L"Windows.UI.Xaml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto initialize = xaml ? reinterpret_cast<InitializeXamlDiagnosticsExFn>(
                                       GetProcAddress(xaml, "InitializeXamlDiagnosticsEx"))
                                 : nullptr;
    if (!initialize)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    const std::wstring dll = GetExeDirectory() + L"\\" + kClearTaskbarDll;
    HRESULT hr = E_FAIL;
    // Endpoint names can be taken by other diagnostics clients; try the next one.
    for (int i = 1; i <= 60; ++i)
    {
        const std::wstring endpoint = L"VisualDiagConnection" + std::to_wstring(i);
        hr = initialize(endpoint.c_str(), pid, L"", dll.c_str(), __uuidof(ClearTaskbarTap),
                        nullptr);
        if (hr != HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
            break;
    }
    return hr;
}

// XAML diagnostics initializes only once per calling thread (later calls return
// S_OK and do nothing), so each attempt runs on a fresh thread.
HRESULT InjectClearTaskbar(DWORD pid)
{
    HRESULT hr = E_FAIL;
    std::thread([&] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        hr = InjectClearTaskbarOnThisThread(pid);
        if (SUCCEEDED(com))
            CoUninitialize();
    }).join();
    return hr;
}

// Called every 2 s from the render loop, so it never runs while the wallpaper is
// paused. Injects once per Explorer instance; after that it is a single FindWindow.
void KeepTaskbarClear()
{
    static DWORD seenPid = 0;
    static int attempts = 0;  // -1 once done for this Explorer.
    const DWORD pid = TaskbarProcessId();
    if (!pid)
        return;
    if (pid != seenPid)
    {
        seenPid = pid;
        attempts = 0;
    }
    if (attempts < 0 || attempts >= 15)
        return;

    if (ModuleLoaded(pid, kClearTaskbarDll))
    {
        attempts = -1;
        return;
    }
    const HRESULT hr = InjectClearTaskbar(pid);
    if (SUCCEEDED(hr))
    {
        attempts = -1;
        Log(L"clear taskbar loaded into explorer %lu", pid);
    }
    else if (++attempts == 15)
    {
        Log(L"warning: clear taskbar failed (0x%08lX)", static_cast<unsigned long>(hr));
    }
}

// FullTransparency off: the DLL can only leave Explorer with Explorer itself.
void RemoveTaskbarClear()
{
    const DWORD pid = TaskbarProcessId();
    if (!pid || !ModuleLoaded(pid, kClearTaskbarDll))
        return;

    Log(L"restarting explorer to remove the clear taskbar");
    const HANDLE explorer = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!explorer)
        return;
    TerminateProcess(explorer, 1);
    WaitForSingleObject(explorer, 5000);
    CloseHandle(explorer);

    // Winlogon restarts the shell by itself; only start it if that did not happen.
    for (int i = 0; i < 40 && !TaskbarProcessId(); ++i)
        Sleep(250);
    if (TaskbarProcessId())
        return;
    wchar_t command[] = L"explorer.exe";
    STARTUPINFOW startup = { sizeof(startup) };
    PROCESS_INFORMATION process = {};
    // Explicit class, or Explorer would inherit our below-normal priority.
    if (CreateProcessW(nullptr, command, nullptr, nullptr, FALSE, NORMAL_PRIORITY_CLASS, nullptr,
                       nullptr, &startup, &process))
    {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

// Container-level frame size and duration: no decoder is loaded, so this is cheap.
bool ProbeVideo(const std::wstring& path, UINT32& width, UINT32& height, double& seconds)
{
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaType> type;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader)) ||
        FAILED(reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                                          0, &type)) ||
        FAILED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height)))
        return false;

    PROPVARIANT duration;
    PropVariantInit(&duration);
    seconds = 0.0;
    if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
                                                   MF_PD_DURATION, &duration)) &&
        duration.vt == VT_UI8)
        seconds = static_cast<double>(duration.uhVal.QuadPart) / 10000000.0;
    PropVariantClear(&duration);
    return true;
}

// An empty "<copy>.failed" file stops a failing re-encode from retrying every
// startup; delete it to try again.
void MarkEncodeFailed(const std::wstring& cached)
{
    const HANDLE marker = CreateFileW((cached + L".failed").c_str(), GENERIC_WRITE, 0, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (marker != INVALID_HANDLE_VALUE)
        CloseHandle(marker);
}

// Once per wallpaper and screen size: when the video is larger than the screen or
// over 8 Mbps, ffmpeg (on PATH or next to the exe) re-encodes it into
// wallpapers\optimized. Later startups find the copy and skip all of this.
// ponytail: fixed 8 Mbps cap tuned for 1080p; scale it with screen pixels for 4K.
std::wstring PrepareWallpaper(const std::wstring& original)
{
    MONITORINFO monitor = { sizeof(monitor) };
    WIN32_FILE_ATTRIBUTE_DATA source = {};
    if (!GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &monitor) ||
        !GetFileAttributesExW(original.c_str(), GetFileExInfoStandard, &source))
        return original;
    const UINT32 screenWidth = static_cast<UINT32>(monitor.rcMonitor.right - monitor.rcMonitor.left);
    const UINT32 screenHeight = static_cast<UINT32>(monitor.rcMonitor.bottom - monitor.rcMonitor.top);
    const ULONGLONG bytes = (static_cast<ULONGLONG>(source.nFileSizeHigh) << 32) | source.nFileSizeLow;

    // The original's size in the name means a replaced video gets a new copy.
    const size_t slash = original.find_last_of(L"\\/");
    const size_t dot = original.find_last_of(L'.');
    const std::wstring folder = original.substr(0, slash) + L"\\optimized";
    const std::wstring stem = original.substr(slash + 1, (dot > slash ? dot : original.size()) - slash - 1);
    const std::wstring cached = folder + L"\\" + stem + L"_" + std::to_wstring(screenWidth) + L"x" +
                                std::to_wstring(screenHeight) + L"_" + std::to_wstring(bytes) + L".mp4";
    if (GetFileAttributesW(cached.c_str()) != INVALID_FILE_ATTRIBUTES)
        return cached;
    if (GetFileAttributesW((cached + L".failed").c_str()) != INVALID_FILE_ATTRIBUTES)
        return original;

    UINT32 width = 0, height = 0;
    double seconds = 0.0;
    if (!ProbeVideo(original, width, height, seconds))
        return original;
    const double mbps = seconds > 0.0 ? static_cast<double>(bytes) * 8.0 / seconds / 1e6 : 0.0;
    const bool larger = width > screenWidth || height > screenHeight;
    Log(L"wallpaper source: %ux%u %.1f Mbps (screen %ux%u)", width, height, mbps,
        screenWidth, screenHeight);
    if (!larger && mbps <= 8.0)
        return original;

    // Same center "cover" crop the renderer does, so the picture is unchanged.
    const std::wstring size = std::to_wstring(screenWidth) + L":" + std::to_wstring(screenHeight);
    const std::wstring partial = cached + L".part";
    std::wstring command = L"ffmpeg -hide_banner -loglevel error -nostdin -y -i \"" + original +
                           L"\" -map 0:v:0 ";
    if (larger)
        command += L"-vf \"scale=" + size + L":force_original_aspect_ratio=increase,crop=" + size + L"\" ";
    // Cap at the source bitrate too, so a crop-only re-encode never grows the file.
    const int capKbps = static_cast<int>((mbps > 0.0 ? (std::min)(mbps, 8.0) : 8.0) * 1000.0);
    command += L"-c:v libx264 -preset medium -crf 20 -maxrate " + std::to_wstring(capKbps) +
               L"k -bufsize " + std::to_wstring(capKbps * 2) + L"k -pix_fmt yuv420p "
               L"-movflags +faststart -f mp4 \"" + partial + L"\"";

    CreateDirectoryW(folder.c_str(), nullptr);
    Log(L"re-encoding wallpaper (one time): %ls", command.c_str());
    STARTUPINFOW startup = { sizeof(startup) };
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(nullptr, &command[0], nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr,
                        &startup, &process))
    {
        Log(L"ffmpeg not found (%lu), using the original video", GetLastError());
        return original;
    }

    DWORD exitCode = 1;
    if (WaitForSingleObject(process.hProcess, 10 * 60 * 1000) != WAIT_OBJECT_0)
        TerminateProcess(process.hProcess, 1);
    else
        GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    // Written under a temporary name, so a killed encode never leaves a broken copy.
    if (exitCode != 0 || !MoveFileExW(partial.c_str(), cached.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        Log(L"re-encode failed (exit %lu), using the original video", exitCode);
        DeleteFileW(partial.c_str());
        MarkEncodeFailed(cached);
        return original;
    }
    Log(L"re-encode done: %ls", cached.c_str());
    return cached;
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
    {
        SyncStartup(config.startWithWindows);
        if (config.transparency >= 0)
            SyncTransparency(config.transparency == 1, config.fullTransparency);
        if (!config.fullTransparency)
            RemoveTaskbarClear();
    }
    // Before the wallpaper window exists, so the normal desktop shows during a re-encode.
    const std::wstring videoPath = PrepareWallpaper(config.wallpaperPath);

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
    bool opened = decoder.Open(videoPath, renderer.device());
    if (!opened && videoPath != config.wallpaperPath)
    {
        // A bad optimized copy: drop it, mark it failed and play the original.
        Log(L"optimized copy failed to open, using the original video");
        DeleteFileW(videoPath.c_str());
        MarkEncodeFailed(videoPath);
        opened = decoder.Open(config.wallpaperPath, renderer.device());
    }
    if (!opened)
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
    bool relaunch = false;
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
                Log(L"paused (fullscreen application detected)");
                // Hand our RAM back to the game; pages fault back in on resume.
                SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1),
                                         static_cast<SIZE_T>(-1));
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

        // Decode/render timings only feed --bench.
        const bool benchmarking = benchSeconds > 0;
        bool frameOk = false;
        const LONGLONG decodeStart = benchmarking ? QueryQpc() : 0;
        DecodedFrame frame = {};
        const DecodeResult result = decoder.DecodeNext(frame);
        const LONGLONG decodeTicks = benchmarking ? QueryQpc() - decodeStart : 0;
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
            const LONGLONG renderStart = benchmarking ? QueryQpc() : 0;
            HRESULT presentResult = frame.hardware
                ? decoder.BlitTo(renderer.backBuffer(), renderer.width(), renderer.height())
                : renderer.RenderFrame(frame.srv, frame.srvUV, decoder.width(), decoder.height());
            if (SUCCEEDED(presentResult))
                presentResult = renderer.Present();
            const LONGLONG presented = (benchmarking || !firstFrame) ? QueryQpc() : 0;
            frameOk = SUCCEEDED(presentResult);  // Persistent failures trip the 5 s watchdog.

            if (benchmarking && firstFrame)
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
                // Startup-only memory (probing, decoder setup) goes back to the system.
                SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1),
                                         static_cast<SIZE_T>(-1));
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

        const DWORD tick = GetTickCount();
        if (frameOk)
            lastFrameTick = tick;
        if (tick - lastFrameTick > 5000)
        {
            Log(L"no frames decoded for 5 seconds, exiting");
            break;
        }

        if (tick - lastParentCheck > 2000)
        {
            // An Explorer restart destroys our window along with its desktop.
            if (!IsWindow(renderer.window()))
            {
                Log(L"wallpaper window destroyed (explorer restarted), relaunching");
                relaunch = benchSeconds == 0;
                break;
            }
            renderer.EnsureDesktopParent();
            if (config.fullTransparency && benchSeconds == 0)
                KeepTaskbarClear();
            lastParentCheck = tick;
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

    // After the mutex is gone; the new instance waits for Explorer's desktop itself.
    wchar_t exe[MAX_PATH] = {};
    if (relaunch && GetModuleFileNameW(nullptr, exe, MAX_PATH))
    {
        STARTUPINFOW startup = { sizeof(startup) };
        PROCESS_INFORMATION process = {};
        if (CreateProcessW(exe, nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                           &process))
        {
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
    }
    return 0;
}
