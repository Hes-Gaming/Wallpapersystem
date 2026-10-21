#pragma once

#include <windows.h>
#include <atomic>
#include <thread>

// Pauses wallpaper playback while another application is maximized or
// fullscreen on the primary monitor. Runs its own message-pumping thread.
class PauseWatcher
{
public:
    PauseWatcher() = default;
    ~PauseWatcher();

    PauseWatcher(const PauseWatcher&) = delete;
    PauseWatcher& operator=(const PauseWatcher&) = delete;

    bool Start(HWND hostWindow);
    void Stop();

    bool IsPaused() const { return paused_.load(std::memory_order_relaxed); }

    // Auto-reset event signaled whenever the paused state changes.
    HANDLE WakeEvent() const { return wakeEvent_; }

private:
    static void CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                      LONG idObject, LONG idChild,
                                      DWORD eventThread, DWORD eventTime);
    void ThreadMain();
    void Evaluate();

    HWND hostWindow_ = nullptr;
    HANDLE wakeEvent_ = nullptr;
    std::atomic<bool> paused_{false};
    std::atomic<bool> stop_{false};
    std::thread thread_;
    DWORD threadId_ = 0;
    HWINEVENTHOOK hookForeground_ = nullptr;
    HWINEVENTHOOK hookLocation_ = nullptr;
};
