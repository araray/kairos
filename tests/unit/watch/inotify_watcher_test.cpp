/// tests/unit/watch/inotify_watcher_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  InotifyWatcher component tests                                           ║
// ║                                                                           ║
// ║  Tests the inotify backend on Linux only. Creates real temporary          ║
// ║  directories, writes files, and verifies native event delivery.           ║
// ║                                                                           ║
// ║  Spec reference: §12.3                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#ifdef __linux__

#include "kairos/watch/inotify_watcher.hpp"
#include "kairos/watch/file_watcher.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace kairos::watch {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

/// Helper: create a temp directory and clean up on destruction.
class TempDir {
public:
    TempDir() {
        char tpl[] = "/tmp/kairos_inotify_test_XXXXXX";
        const char* dir = mkdtemp(tpl);
        EXPECT_NE(dir, nullptr);
        path_ = dir;
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

/// Helper: collect events into a thread-safe vector.
class EventCollector {
public:
    NativeEventCallback callback() {
        return [this](const NativeEvent& e) {
            std::lock_guard lock(mu_);
            events_.push_back(e);
            cv_.notify_all();
        };
    }

    /// Wait until at least n events are collected (with timeout).
    bool wait_for(size_t n, std::chrono::milliseconds timeout = 3000ms) {
        std::unique_lock lock(mu_);
        return cv_.wait_for(lock, timeout, [&] {
            return events_.size() >= n;
        });
    }

    std::vector<NativeEvent> events() {
        std::lock_guard lock(mu_);
        return events_;
    }

    size_t count() {
        std::lock_guard lock(mu_);
        return events_.size();
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<NativeEvent> events_;
};

// ── Tests ───────────────────────────────────────────────────────────────

class InotifyWatcherTest : public ::testing::Test {
protected:
    TempDir tmp_;
};

TEST_F(InotifyWatcherTest, ConstructDestruct) {
    // Verify inotify instance can be created and destroyed.
    InotifyWatcher watcher;
    EXPECT_EQ(watcher.watch_count(), 0u);
    EXPECT_EQ(watcher.platform_name(), "inotify");
}

TEST_F(InotifyWatcherTest, AddWatch) {
    InotifyWatcher watcher;
    EXPECT_TRUE(watcher.add_watch(tmp_.path(), false));
    EXPECT_EQ(watcher.watch_count(), 1u);
}

TEST_F(InotifyWatcherTest, AddWatchRecursive) {
    // Create a subdirectory structure.
    auto sub1 = tmp_.path() / "sub1";
    auto sub2 = tmp_.path() / "sub1" / "sub2";
    fs::create_directories(sub2);

    InotifyWatcher watcher;
    EXPECT_TRUE(watcher.add_watch(tmp_.path(), true));
    // Root + sub1 + sub2 = 3 watches.
    EXPECT_GE(watcher.watch_count(), 3u);
}

TEST_F(InotifyWatcherTest, AddWatchNonexistentPath) {
    InotifyWatcher watcher;
    EXPECT_FALSE(watcher.add_watch("/nonexistent/path/12345", false));
}

TEST_F(InotifyWatcherTest, RemoveWatch) {
    InotifyWatcher watcher;
    EXPECT_TRUE(watcher.add_watch(tmp_.path(), false));
    EXPECT_EQ(watcher.watch_count(), 1u);

    watcher.remove_watch(tmp_.path());
    EXPECT_EQ(watcher.watch_count(), 0u);
}

TEST_F(InotifyWatcherTest, DiagnosticInfo) {
    InotifyWatcher watcher;
    watcher.add_watch(tmp_.path(), false);
    std::string diag = watcher.diagnostic_info();
    EXPECT_NE(diag.find("inotify"), std::string::npos);
    EXPECT_NE(diag.find("watches:"), std::string::npos);
}

TEST_F(InotifyWatcherTest, DetectsFileCreation) {
    InotifyWatcher watcher;
    ASSERT_TRUE(watcher.add_watch(tmp_.path(), false));

    EventCollector collector;
    std::stop_source stop;

    // Run the watcher in a background thread.
    std::jthread watcher_thread([&](std::stop_token st) {
        watcher.run(st, collector.callback());
    });

    // Give the watcher a moment to start polling.
    std::this_thread::sleep_for(100ms);

    // Create a file.
    {
        std::ofstream f(tmp_.path() / "test.txt");
        f << "hello";
    }

    // Wait for at least 1 event (create and/or close_write).
    ASSERT_TRUE(collector.wait_for(1, 2000ms));

    auto events = collector.events();
    bool found_created = false;
    bool found_modified = false;
    for (const auto& e : events) {
        if (e.type == NativeEventType::Created) found_created = true;
        if (e.type == NativeEventType::Modified) found_modified = true;
    }
    // We should see at least a Created or Modified event for the new file.
    EXPECT_TRUE(found_created || found_modified)
        << "Expected Created or Modified event for new file";

    stop.request_stop();
    watcher_thread.request_stop();
}

TEST_F(InotifyWatcherTest, DetectsFileDeletion) {
    // Pre-create a file.
    auto file_path = tmp_.path() / "delete_me.txt";
    { std::ofstream f(file_path); f << "content"; }

    InotifyWatcher watcher;
    ASSERT_TRUE(watcher.add_watch(tmp_.path(), false));

    EventCollector collector;

    std::jthread watcher_thread([&](std::stop_token st) {
        watcher.run(st, collector.callback());
    });

    std::this_thread::sleep_for(100ms);

    // Delete the file.
    fs::remove(file_path);

    ASSERT_TRUE(collector.wait_for(1, 2000ms));

    auto events = collector.events();
    bool found_deleted = false;
    for (const auto& e : events) {
        if (e.type == NativeEventType::Deleted) {
            found_deleted = true;
            EXPECT_EQ(e.path.filename(), "delete_me.txt");
        }
    }
    EXPECT_TRUE(found_deleted);

    watcher_thread.request_stop();
}

TEST_F(InotifyWatcherTest, DetectsSubdirectoryCreation) {
    InotifyWatcher watcher;
    ASSERT_TRUE(watcher.add_watch(tmp_.path(), true));

    EventCollector collector;

    std::jthread watcher_thread([&](std::stop_token st) {
        watcher.run(st, collector.callback());
    });

    std::this_thread::sleep_for(100ms);

    // Create a new subdirectory.
    fs::create_directory(tmp_.path() / "newdir");

    ASSERT_TRUE(collector.wait_for(1, 2000ms));

    auto events = collector.events();
    bool found_dir_created = false;
    for (const auto& e : events) {
        if (e.type == NativeEventType::Created && e.is_directory) {
            found_dir_created = true;
            EXPECT_EQ(e.path.filename(), "newdir");
        }
    }
    EXPECT_TRUE(found_dir_created);

    // Auto-add: creating a file inside the new subdir should also be detected.
    std::this_thread::sleep_for(50ms);
    { std::ofstream f(tmp_.path() / "newdir" / "inner.txt"); f << "data"; }

    ASSERT_TRUE(collector.wait_for(events.size() + 1, 2000ms));

    watcher_thread.request_stop();
}

TEST_F(InotifyWatcherTest, StopRequestExitsRunLoop) {
    InotifyWatcher watcher;
    ASSERT_TRUE(watcher.add_watch(tmp_.path(), false));

    EventCollector collector;
    std::stop_source stop;

    auto start = std::chrono::steady_clock::now();

    std::jthread watcher_thread([&](std::stop_token) {
        watcher.run(stop.get_token(), collector.callback());
    });

    // Request stop immediately.
    std::this_thread::sleep_for(50ms);
    stop.request_stop();
    watcher_thread.join();

    auto elapsed = std::chrono::steady_clock::now() - start;
    // Should exit within ~1 second (poll timeout + processing).
    EXPECT_LT(elapsed, 3s);
}

TEST_F(InotifyWatcherTest, FactoryCreatesInotifyWatcher) {
    auto watcher = create_native_watcher();
    ASSERT_NE(watcher, nullptr);
    EXPECT_EQ(watcher->platform_name(), "inotify");
    EXPECT_TRUE(has_native_watcher());
}

TEST_F(InotifyWatcherTest, DuplicateAddWatchIsIdempotent) {
    InotifyWatcher watcher;
    EXPECT_TRUE(watcher.add_watch(tmp_.path(), false));
    auto count_first = watcher.watch_count();

    // Adding the same path again should be idempotent.
    EXPECT_TRUE(watcher.add_watch(tmp_.path(), false));
    EXPECT_EQ(watcher.watch_count(), count_first);
}

TEST_F(InotifyWatcherTest, DetectsFileModification) {
    auto file_path = tmp_.path() / "modify_me.txt";
    { std::ofstream f(file_path); f << "initial"; }

    InotifyWatcher watcher;
    ASSERT_TRUE(watcher.add_watch(tmp_.path(), false));

    EventCollector collector;

    std::jthread watcher_thread([&](std::stop_token st) {
        watcher.run(st, collector.callback());
    });

    std::this_thread::sleep_for(100ms);

    // Modify the file.
    { std::ofstream f(file_path); f << "modified content"; }

    ASSERT_TRUE(collector.wait_for(1, 2000ms));

    auto events = collector.events();
    bool found_modify = false;
    for (const auto& e : events) {
        if (e.type == NativeEventType::Modified) {
            found_modify = true;
        }
    }
    EXPECT_TRUE(found_modify);

    watcher_thread.request_stop();
}

}  // namespace
}  // namespace kairos::watch

#endif  // __linux__

// On non-Linux platforms, this file compiles to nothing.
#ifndef __linux__
#include <gtest/gtest.h>
TEST(InotifyWatcherSkipped, NotAvailableOnThisPlatform) {
    GTEST_SKIP() << "inotify tests only run on Linux";
}
#endif
