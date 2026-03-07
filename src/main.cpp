// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  main.cpp — Kairos entry point                                            ║
// ║                                                                           ║
// ║  Minimal: delegates all work to the CLI application.                      ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/cli/cli_app.hpp"

int main(int argc, char** argv) {
    return kairos::cli::run(argc, argv);
}
