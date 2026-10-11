/// src/platform/sd_notify_posix.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  sd_notify_posix.cpp — systemd notification via raw AF_UNIX socket       ║
// ║                                                                          ║
// ║  The sd_notify protocol is trivial: open a SOCK_DGRAM to the path in    ║
// ║  $NOTIFY_SOCKET, sendmsg() the state string.  No libsystemd needed.     ║
// ║                                                                          ║
// ║  If $NOTIFY_SOCKET is unset, all calls succeed silently (no-op).        ║
// ║  If the socket path starts with '@', it is treated as an abstract       ║
// ║  socket (Linux-specific).                                                ║
// ║                                                                          ║
// ║  Spec reference: §27.4                                                  ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/platform/sd_notify.hpp"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#ifdef __linux__
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace kairos::platform {

#ifdef __linux__

bool sd_notify_send(std::string_view state) {
    // Check for $NOTIFY_SOCKET — if absent, we're not under systemd Type=notify.
    const char* socket_path = std::getenv("NOTIFY_SOCKET");
    if (!socket_path || socket_path[0] == '\0') {
        return true;  // Not running under systemd notify — silent success.
    }

    // Validate the socket path: must be absolute or abstract.
    if (socket_path[0] != '/' && socket_path[0] != '@') {
        spdlog::warn("sd_notify: NOTIFY_SOCKET has invalid path: '{}'",
                      socket_path);
        return false;
    }

    // Open a SOCK_DGRAM | SOCK_CLOEXEC socket.
    int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        spdlog::warn("sd_notify: socket() failed: {} ({})",
                      std::strerror(errno), errno);
        return false;
    }

    // Build the sockaddr_un.
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;

    std::size_t path_len = std::strlen(socket_path);
    if (path_len >= sizeof(addr.sun_path)) {
        spdlog::warn("sd_notify: NOTIFY_SOCKET path too long ({} bytes)",
                      path_len);
        ::close(fd);
        return false;
    }

    std::memcpy(addr.sun_path, socket_path, path_len + 1);

    // Abstract sockets: replace leading '@' with '\0'.
    if (addr.sun_path[0] == '@') {
        addr.sun_path[0] = '\0';
    }

    // Compute the actual address length.  For abstract sockets the
    // length includes the NUL byte and the path after it.  For
    // filesystem sockets it's offsetof(sun_path) + strlen + 1.
    socklen_t addr_len;
    if (socket_path[0] == '@') {
        // Abstract: offsetof(sun_path) + 1 (NUL) + path_len - 1
        addr_len = static_cast<socklen_t>(
            offsetof(struct sockaddr_un, sun_path) + path_len);
    } else {
        addr_len = static_cast<socklen_t>(
            offsetof(struct sockaddr_un, sun_path) + path_len + 1);
    }

    // Build the message.
    struct iovec iov {};
    iov.iov_base = const_cast<char*>(state.data());
    iov.iov_len = state.size();

    struct msghdr msg {};
    msg.msg_name = &addr;
    msg.msg_namelen = addr_len;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    // Send.
    ssize_t sent = ::sendmsg(fd, &msg, MSG_NOSIGNAL);
    ::close(fd);

    if (sent < 0) {
        spdlog::warn("sd_notify: sendmsg() failed: {} ({})",
                      std::strerror(errno), errno);
        return false;
    }

    spdlog::trace("sd_notify: sent '{}' ({} bytes)",
                   state, sent);
    return true;
}

int sd_watchdog_interval_s() {
    const char* usec_str = std::getenv("WATCHDOG_USEC");
    if (!usec_str || usec_str[0] == '\0') return 0;

    char* end = nullptr;
    unsigned long long usec = std::strtoull(usec_str, &end, 10);
    if (end == usec_str || usec == 0) return 0;

    // Convert µs → seconds.  The daemon should ping at half this interval.
    return static_cast<int>(usec / 1'000'000ULL);
}

#else  // macOS, other POSIX — no systemd

bool sd_notify_send(std::string_view /*state*/) {
    return true;  // No-op on non-Linux.
}

int sd_watchdog_interval_s() {
    return 0;
}

#endif

}  // namespace kairos::platform
