#pragma once

#include <windows.h>
#include <atomic>
#include <thread>

// Pauses wallpaper playback while another application is fullscreen on the
// primary monitor. Runs its own thread that polls ~20 times a second.
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

    // Benchmark counters: number of Evaluate() calls and QPC ticks spent in them.
    long long evaluations() const { return evalCount_.load(std::memory_order_relaxed); }
    long long evaluationTicks() const { return evalTicks_.load(std::memory_order_relaxed); }

private:
    void ThreadMain();
    void Evaluate();

    HWND hostWindow_ = nullptr;
    HANDLE wakeEvent_ = nullptr;
    std::atomic<bool> paused_{false};
    std::atomic<bool> stop_{false};
    std::atomic<long long> evalCount_{0};
    std::atomic<long long> evalTicks_{0};
    std::thread thread_;
};
