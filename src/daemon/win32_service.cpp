/// src/daemon/win32_service.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  win32_service.cpp — Windows Service Control Manager integration         ║
// ║                                                                         ║
// ║  Bridges SCM's callback model with Kairos's cooperative shutdown.       ║
// ║  SERVICE_CONTROL_STOP → g_shutdown_requested = true, which the          ║
// ║  daemon's main loop polls.                                              ║
// ║                                                                         ║
// ║  Spec reference: §27.6                                                  ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#ifdef _WIN32

#include "kairos/daemon/win32_service.hpp"
#include "kairos/daemon/daemon.hpp"
#include "kairos/platform/signals.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <iostream>
#include <string>

namespace kairos::daemon {

// ── Service name constants ─────────────────────────────────────────────────

static constexpr const wchar_t* kServiceName = L"Kairos";
static constexpr const wchar_t* kDisplayName = L"Kairos Orchestration Daemon";
static constexpr const wchar_t* kDescription =
    L"Unified orchestration daemon — scheduling, workflows, and FS monitoring";

// ── Global state for SCM callbacks ─────────────────────────────────────────
// SCM callbacks are static C functions — they cannot capture context via
// closures.  We use file-scoped globals, protected by the fact that SCM
// calls service_main exactly once per process.

static SERVICE_STATUS_HANDLE g_status_handle = nullptr;
static SERVICE_STATUS g_status{};
static std::shared_ptr<const kairos::config::ConfigState> g_config;

// ── Helper: report service status to SCM ───────────────────────────────────

static void report_status(DWORD state, DWORD exit_code = 0,
                           DWORD wait_hint = 0) {
    static DWORD check_point = 0;
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = exit_code;
    g_status.dwWaitHint = wait_hint;

    if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) {
        g_status.dwCheckPoint = ++check_point;
    } else {
        g_status.dwCheckPoint = 0;
    }

    SetServiceStatus(g_status_handle, &g_status);
}

// ── Service control handler ────────────────────────────────────────────────

static DWORD WINAPI service_ctrl_handler(DWORD control, DWORD /*event_type*/,
                                          LPVOID /*event_data*/,
                                          LPVOID /*context*/) {
    switch (control) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            report_status(SERVICE_STOP_PENDING, 0, 30000);
            // Signal the daemon to stop via the global shutdown flag.
            // The daemon's main loop polls g_shutdown_requested.
            platform::g_shutdown_requested.store(true);
            return NO_ERROR;

        case SERVICE_CONTROL_INTERROGATE:
            // Report current status.
            SetServiceStatus(g_status_handle, &g_status);
            return NO_ERROR;

        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

// ── Service main function ──────────────────────────────────────────────────

static void WINAPI service_main(DWORD /*argc*/, LPWSTR* /*argv*/) {
    // Register the control handler.
    g_status_handle = RegisterServiceCtrlHandlerExW(
        kServiceName, service_ctrl_handler, nullptr);
    if (!g_status_handle) {
        return;  // Cannot continue without a status handle.
    }

    // Initialize status structure.
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwControlsAccepted =
        SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;

    report_status(SERVICE_START_PENDING, 0, 10000);

    // Run the actual daemon.
    report_status(SERVICE_RUNNING);

    int rc = run_daemon(g_config);

    // Report stopped.
    DWORD exit_code = (rc == 0) ? 0 : ERROR_SERVICE_SPECIFIC_ERROR;
    report_status(SERVICE_STOPPED, exit_code);
}

// ── Public API ─────────────────────────────────────────────────────────────

int run_as_windows_service(
    std::shared_ptr<const kairos::config::ConfigState> config) {

    g_config = std::move(config);

    SERVICE_TABLE_ENTRYW dispatch_table[] = {
        { const_cast<LPWSTR>(kServiceName), service_main },
        { nullptr, nullptr }
    };

    if (!StartServiceCtrlDispatcherW(dispatch_table)) {
        DWORD err = GetLastError();
        if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            // Not running as a service — the user probably ran
            // `kairos start --service` from a console.
            std::cerr << "Error: --service flag requires running via the "
                         "Windows Service Control Manager.\n"
                         "Use `kairos start` for foreground mode, or install "
                         "the service with `kairos service install`.\n";
            return 1;
        }
        std::cerr << "StartServiceCtrlDispatcher failed: "
                  << err << "\n";
        return 1;
    }

    return 0;
}

bool install_service(const std::filesystem::path& exe_path,
                     const std::filesystem::path& config_path) {
    // Open the SCM.
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr,
                                    SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        std::cerr << "Failed to open Service Control Manager. "
                     "Run as Administrator.\n";
        return false;
    }

    // Build the command line: kairos.exe start --service --config <path>
    std::wstring cmd_line = L"\"" + exe_path.wstring() + L"\" start --service";
    if (!config_path.empty()) {
        cmd_line += L" --config \"" + config_path.wstring() + L"\"";
    }

    SC_HANDLE svc = CreateServiceW(
        scm,
        kServiceName,
        kDisplayName,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        cmd_line.c_str(),
        nullptr,   // no load ordering group
        nullptr,   // no tag id
        nullptr,   // no dependencies
        nullptr,   // LocalSystem account
        nullptr    // no password
    );

    if (!svc) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_EXISTS) {
            std::cerr << "Service already exists. Uninstall first with "
                         "`kairos service uninstall`.\n";
        } else {
            std::cerr << "CreateService failed: " << err << "\n";
        }
        CloseServiceHandle(scm);
        return false;
    }

    // Set the service description.
    SERVICE_DESCRIPTIONW desc{};
    desc.lpDescription = const_cast<LPWSTR>(kDescription);
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

    std::cout << "Service '" << "Kairos" << "' installed successfully.\n"
              << "Start with: sc start Kairos\n"
              << "  or:       net start Kairos\n";

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return true;
}

bool uninstall_service() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr,
                                    SC_MANAGER_CONNECT);
    if (!scm) {
        std::cerr << "Failed to open Service Control Manager. "
                     "Run as Administrator.\n";
        return false;
    }

    SC_HANDLE svc = OpenServiceW(scm, kServiceName,
                                  SERVICE_STOP | DELETE |
                                  SERVICE_QUERY_STATUS);
    if (!svc) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
            std::cerr << "Service does not exist.\n";
        } else {
            std::cerr << "OpenService failed: " << err << "\n";
        }
        CloseServiceHandle(scm);
        return false;
    }

    // Stop the service if it is running.
    SERVICE_STATUS status{};
    ControlService(svc, SERVICE_CONTROL_STOP, &status);

    // Wait for the service to stop (up to 15 seconds).
    for (int i = 0; i < 30; ++i) {
        QueryServiceStatus(svc, &status);
        if (status.dwCurrentState == SERVICE_STOPPED) break;
        Sleep(500);
    }

    if (!DeleteService(svc)) {
        std::cerr << "DeleteService failed: " << GetLastError() << "\n";
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return false;
    }

    std::cout << "Service 'Kairos' uninstalled successfully.\n";

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return true;
}

}  // namespace kairos::daemon

#endif // _WIN32
