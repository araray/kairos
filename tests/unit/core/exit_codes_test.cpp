/// tests/unit/core/exit_codes_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  exit_codes_test.cpp — Exit code description tests                        ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/core/exit_codes.hpp"

#include <gtest/gtest.h>

namespace kairos {

TEST(ExitCodes, SuccessDescription) {
    EXPECT_EQ(exit_code_description(0), "Success");
}

TEST(ExitCodes, GenericErrorDescription) {
    EXPECT_EQ(exit_code_description(1), "Generic error");
}

TEST(ExitCodes, ConfigErrorDescription) {
    EXPECT_EQ(exit_code_description(2), "Configuration error");
}

TEST(ExitCodes, CommandNotFound) {
    EXPECT_EQ(exit_code_description(127), "Command not found");
}

TEST(ExitCodes, SignalKill) {
    // 128 + 9 = 137 (SIGKILL)
    EXPECT_EQ(exit_code_description(137), "Killed by signal");
}

TEST(ExitCodes, KairosTimeoutSoft) {
    EXPECT_EQ(exit_code_description(200), "Timeout exceeded (soft kill)");
}

TEST(ExitCodes, KairosTimeoutHard) {
    EXPECT_EQ(exit_code_description(201), "Timeout exceeded (hard kill)");
}

TEST(ExitCodes, KairosDependencyFailure) {
    EXPECT_EQ(exit_code_description(203), "Dependency failure (upstream job failed)");
}

TEST(ExitCodes, KairosConditionError) {
    EXPECT_EQ(exit_code_description(204), "Condition evaluation error");
}

TEST(ExitCodes, UnknownCode) {
    EXPECT_EQ(exit_code_description(42), "Unknown exit code");
}

TEST(ExitCodes, EnumValues) {
    EXPECT_EQ(static_cast<int>(ExitCode::kSuccess), 0);
    EXPECT_EQ(static_cast<int>(ExitCode::kConfigError), 2);
    EXPECT_EQ(static_cast<int>(ExitCode::kTimeoutSoft), 200);
    EXPECT_EQ(static_cast<int>(ExitCode::kTimeoutHard), 201);
    EXPECT_EQ(static_cast<int>(ExitCode::kRunnerConfigError), 202);
    EXPECT_EQ(static_cast<int>(ExitCode::kDependencyFailure), 203);
    EXPECT_EQ(static_cast<int>(ExitCode::kConditionError), 204);
}

}  // namespace kairos
