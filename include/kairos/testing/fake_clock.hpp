/// include/kairos/testing/fake_clock.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/testing/fake_clock.hpp — Clock abstraction for deterministic      ║
// ║  testing of time-dependent components.                                    ║
// ║                                                                           ║
// ║  Production code uses SystemClockSource (delegates to std::chrono).       ║
// ║  Test code uses FakeClock (advances only when explicitly told to).        ║
// ║                                                                           ║
// ║  All time-dependent components (Scheduler, Pipeline, WatchEngine,         ║
// ║  DBWriter) accept a ClockSource* at construction.                         ║
// ║                                                                           ║
// ║  Spec reference: §30.3                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace kairos {

/// Abstract clock interface.
///
/// Thread safety: all methods must be safe to call from any thread.
/// The contract is:
///   - now() / steady_now(): read current time (lockless in SystemClockSource).
///   - sleep_until(): block until the target time is reached or wake() is called.
///   - wake(): unblock all threads sleeping in sleep_until().
class ClockSource {
public:
    using time_point   = std::chrono::system_clock::time_point;
    using steady_point = std::chrono::steady_clock::time_point;
    using duration     = std::chrono::milliseconds;

    virtual ~ClockSource() = default;

    /// Current wall-clock time (for cron/date triggers, timestamps).
    [[nodiscard]] virtual time_point now() const = 0;

    /// Current monotonic time (for interval timers, timeouts).
    [[nodiscard]] virtual steady_point steady_now() const = 0;

    /// Sleep until the given steady time point or until woken.
    /// @return true if the target time was reached; false if woken early.
    ///
    /// For FakeClock, this blocks until advance() pushes steady_time_
    /// past `target`, or wake() is called.
    virtual bool sleep_until(steady_point target) = 0;

    /// Sleep for a given duration (convenience wrapper).
    /// @return true if the full duration elapsed; false if woken early.
    virtual bool sleep_for(duration d) {
        return sleep_until(steady_now() + d);
    }

    /// Wake all threads sleeping in sleep_until().
    /// Used for shutdown signals and FakeClock::advance().
    virtual void wake() = 0;
};

/// Production clock — delegates to std::chrono.
///
/// sleep_until() uses a condition variable so it can be interrupted
/// by wake() (needed for clean daemon shutdown).
class SystemClockSource final : public ClockSource {
public:
    [[nodiscard]] time_point now() const override {
        return std::chrono::system_clock::now();
    }

    [[nodiscard]] steady_point steady_now() const override {
        return std::chrono::steady_clock::now();
    }

    bool sleep_until(steady_point target) override {
        std::unique_lock lock(mu_);
        return !cv_.wait_until(lock, target, [this] { return woken_; });
    }

    void wake() override {
        {
            std::lock_guard lock(mu_);
            woken_ = true;
        }
        cv_.notify_all();
    }

    /// Reset the woken flag (call after handling a wake).
    void reset_wake() {
        std::lock_guard lock(mu_);
        woken_ = false;
    }

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool woken_ = false;
};

namespace testing {

/// Test clock — time advances only via explicit calls to advance().
///
/// Usage:
///   FakeClock clock;
///   clock.set_now(parse_iso("2026-03-02T14:00:00Z"));
///   // ... create scheduler with &clock ...
///   clock.advance(std::chrono::minutes(5));
///   // scheduler fires any timers due at or before 14:05:00.
///
/// Thread safety: FakeClock is thread-safe. Multiple component threads
/// can call now()/steady_now()/sleep_until() concurrently. Only the
/// test thread calls advance() and set_now().
class FakeClock final : public ClockSource {
public:
    /// Construct with epoch as initial time.
    FakeClock()
        : wall_time_(std::chrono::system_clock::time_point{})
        , steady_time_(std::chrono::steady_clock::time_point{}) {}

    /// Set absolute wall-clock time.
    void set_now(time_point tp) {
        std::lock_guard lock(mu_);
        wall_time_ = tp;
    }

    /// Set absolute steady time.
    void set_steady(steady_point tp) {
        std::lock_guard lock(mu_);
        steady_time_ = tp;
    }

    /// Advance both clocks by the given duration.
    /// After advancing, wakes all threads sleeping in sleep_until()
    /// whose target time has been reached.
    void advance(duration d) {
        {
            std::lock_guard lock(mu_);
            wall_time_ += d;
            steady_time_ += d;
        }
        // Wake all sleepers — they will re-check their targets.
        cv_.notify_all();
    }

    /// Advance both clocks by an arbitrary chrono duration.
    template <typename Rep, typename Period>
    void advance(std::chrono::duration<Rep, Period> d) {
        advance(std::chrono::duration_cast<duration>(d));
    }

    [[nodiscard]] time_point now() const override {
        std::lock_guard lock(mu_);
        return wall_time_;
    }

    [[nodiscard]] steady_point steady_now() const override {
        std::lock_guard lock(mu_);
        return steady_time_;
    }

    bool sleep_until(steady_point target) override {
        std::unique_lock lock(mu_);
        cv_.wait(lock, [&] {
            return steady_time_ >= target || woken_;
        });
        bool reached = steady_time_ >= target;
        woken_ = false;
        return reached;
    }

    void wake() override {
        {
            std::lock_guard lock(mu_);
            woken_ = true;
        }
        cv_.notify_all();
    }

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    time_point wall_time_;
    steady_point steady_time_;
    bool woken_ = false;
};

}  // namespace testing
}  // namespace kairos
