/// src/tui/tui_dashboard_stub.cpp
// ╔═════════════════════════════════════════════════════════════════════════╗
// ║  TUI Dashboard stub — compiled when KAIROS_TUI=OFF                      ║
// ║  Prints an informative error message and returns exit code 1.           ║
// ║  Spec reference: §23 (optional TUI)                                     ║
// ╚═════════════════════════════════════════════════════════════════════════╝

#include "kairos/tui/tui_dashboard.hpp"

#include <iostream>

namespace kairos::tui {

int run_dashboard(const DashboardConfig&) {
    std::cerr << "Error: TUI dashboard not compiled.\n"
              << "Rebuild with -DKAIROS_TUI=ON to enable "
              << "the `kairos dashboard` command.\n"
              << "\n"
              << "  cmake -B build -DKAIROS_TUI=ON\n"
              << "  cmake --build build\n";
    return 1;
}

}  // namespace kairos::tui
