// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  signals_posix.cpp — Signal handling via self-pipe (POSIX)                ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/platform/signals.hpp"

#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

namespace kairos::platform {

// Global atomic flags.
std::atomic<bool> g_shutdown_requested{false};
std::atomic<bool> g_reload_requested{false};

namespace {

// Self-pipe file descriptors.
int g_pipe_fds[2] = {-1, -1};

// Signal handler — writes signal number to pipe (async-signal-safe).
void signal_handler(int signum) {
    // Write is async-signal-safe.  If the pipe is full, we silently
    // drop — the reader will still process the first notification.
    char sig = static_cast<char>(signum);
    [[maybe_unused]] auto _ = ::write(g_pipe_fds[1], &sig, 1);
}

}  // anonymous namespace

void install_signal_handlers(SignalCallback callback) {
    // Create self-pipe.
    if (::pipe(g_pipe_fds) != 0) {
        return;  // Silently fail (logging may not be initialized yet).
    }

    // Make both ends non-blocking.
    for (int i = 0; i < 2; ++i) {
        int flags = ::fcntl(g_pipe_fds[i], F_GETFL, 0);
        ::fcntl(g_pipe_fds[i], F_SETFL, flags | O_NONBLOCK);
    }

    // Install signal handlers.
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);

    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGINT,  &sa, nullptr);
    ::sigaction(SIGHUP,  &sa, nullptr);

    // Reader thread: reads from the self-pipe and invokes the callback.
    std::thread reader([callback = std::move(callback)]() {
        char buf[16];
        while (true) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(g_pipe_fds[0], &rfds);

            // Block until a signal arrives (or pipe is closed).
            struct timeval tv;
            tv.tv_sec  = 1;
            tv.tv_usec = 0;

            int ret = ::select(g_pipe_fds[0] + 1, &rfds, nullptr, nullptr, &tv);
            if (ret <= 0) {
                if (g_shutdown_requested.load()) break;
                continue;
            }

            ssize_t n = ::read(g_pipe_fds[0], buf, sizeof(buf));
            for (ssize_t i = 0; i < n; ++i) {
                int signum = static_cast<int>(buf[i]);
                if (signum == SIGTERM || signum == SIGINT) {
                    g_shutdown_requested.store(true);
                    if (callback) callback(SignalType::kShutdown);
                } else if (signum == SIGHUP) {
                    g_reload_requested.store(true);
                    if (callback) callback(SignalType::kReload);
                }
            }

            if (g_shutdown_requested.load()) break;
        }
    });
    reader.detach();
}

}  // namespace kairos::platform
