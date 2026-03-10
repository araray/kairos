/// include/kairos/daemon/win32_service.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/daemon/win32_service.hpp — Windows Service integration           ║
// ║                                                                         ║
// ║  Bridges the Windows Service Control Manager (SCM) callback model       ║
// ║  with Kairos's stop_source-based cooperative shutdown.                  ║
// ║                                                                         ║
// ║  Spec reference: §27.6                                                  ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#ifdef _WIN32

#include "kairos/config/config_store.hpp"

#include <filesystem>
#include <memory>

namespace kairos::daemon {

/// Run Kairos as a Windows Service.
///
/// This function:
///   1. Registers with the SCM via StartServiceCtrlDispatcher.
///   2. Reports SERVICE_RUNNING after daemon init.
///   3. Translates SERVICE_CONTROL_STOP → stop_source::request_stop().
///   4. Reports SERVICE_STOPPED on clean exit.
///
/// Must be called from main() when --service flag is present.
///
/// @param config  Loaded and validated configuration.
/// @return        The service exit code (0 on success).
int run_as_windows_service(
    std::shared_ptr<const kairos::config::ConfigState> config);

/// Install Kairos as a Windows Service.
///
/// Creates the service entry:
///   HKLM\SYSTEM\CurrentControlSet\Services\Kairos
///
/// Start type: SERVICE_AUTO_START (starts at boot).
/// Display name: "Kairos Orchestration Daemon"
///
/// @param exe_path     Path to the kairos.exe binary.
/// @param config_path  Path to the configuration file.
/// @return             true if installation succeeded.
bool install_service(const std::filesystem::path& exe_path,
                     const std::filesystem::path& config_path);

/// Uninstall the Kairos Windows Service.
///
/// Stops the service if running, then removes the service entry.
///
/// @return  true if uninstallation succeeded.
bool uninstall_service();

}  // namespace kairos::daemon

#endif // _WIN32
