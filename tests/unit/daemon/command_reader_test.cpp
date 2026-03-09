/// tests/unit/daemon/command_reader_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  CommandReader tests — command file parsing, polling, cleanup            ║
// ║  Spec reference: §23.10                                                 ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/daemon/command_reader.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace kairos::daemon {
namespace {

namespace fs = std::filesystem;

/// Helper to create a temp directory for each test.
class CommandReaderTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() / "kairos_test_cmd_reader";
        commands_dir_ = test_dir_ / "commands";
        fs::create_directories(commands_dir_);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }

    /// Write a command file to the commands directory.
    void write_command(const std::string& filename,
                       const std::string& content = "") {
        auto path = commands_dir_ / filename;
        std::ofstream ofs(path);
        if (!content.empty()) ofs << content;
    }

    fs::path test_dir_;
    fs::path commands_dir_;
};

TEST_F(CommandReaderTest, EmptyDirectoryReturnsNoCommands) {
    CommandReader reader(commands_dir_);
    auto cmds = reader.poll();
    EXPECT_TRUE(cmds.empty());
}

TEST_F(CommandReaderTest, ParseCancelCommand) {
    write_command("cancel_run-abc123");

    CommandReader reader(commands_dir_);
    auto cmds = reader.poll();

    ASSERT_EQ(cmds.size(), 1u);
    EXPECT_EQ(cmds[0].type, Command::Type::Cancel);
    EXPECT_EQ(cmds[0].run_id, "run-abc123");
}

TEST_F(CommandReaderTest, CancelCommandContentOverridesFilename) {
    write_command("cancel_placeholder", "run-actual-id");

    CommandReader reader(commands_dir_);
    auto cmds = reader.poll();

    ASSERT_EQ(cmds.size(), 1u);
    EXPECT_EQ(cmds[0].type, Command::Type::Cancel);
    EXPECT_EQ(cmds[0].run_id, "run-actual-id");
}

TEST_F(CommandReaderTest, ParseReloadCommand) {
    write_command("reload_uuid123");

    CommandReader reader(commands_dir_);
    auto cmds = reader.poll();

    ASSERT_EQ(cmds.size(), 1u);
    EXPECT_EQ(cmds[0].type, Command::Type::Reload);
}

TEST_F(CommandReaderTest, UnknownCommandFileReturnsUnknown) {
    write_command("something_else");

    CommandReader reader(commands_dir_);
    auto cmds = reader.poll();

    // Unknown commands are filtered out (not returned to caller).
    EXPECT_TRUE(cmds.empty());
}

TEST_F(CommandReaderTest, CommandFilesDeletedAfterPoll) {
    write_command("cancel_run-1");
    write_command("reload_uuid1");

    CommandReader reader(commands_dir_);

    // First poll reads and deletes.
    auto cmds1 = reader.poll();
    EXPECT_GE(cmds1.size(), 1u);

    // Second poll returns nothing.
    auto cmds2 = reader.poll();
    EXPECT_TRUE(cmds2.empty());

    // Verify files are gone.
    EXPECT_FALSE(fs::exists(commands_dir_ / "cancel_run-1"));
    EXPECT_FALSE(fs::exists(commands_dir_ / "reload_uuid1"));
}

TEST_F(CommandReaderTest, WriteResponse) {
    CommandReader reader(commands_dir_);

    reader.write_response("uuid-abc", true, "Run cancelled");

    auto response_path = test_dir_ / "responses" / "uuid-abc";
    ASSERT_TRUE(fs::exists(response_path));

    std::ifstream ifs(response_path);
    std::string line1, line2;
    std::getline(ifs, line1);
    std::getline(ifs, line2);

    EXPECT_EQ(line1, "OK");
    EXPECT_EQ(line2, "Run cancelled");
}

TEST_F(CommandReaderTest, NonexistentDirectoryHandled) {
    auto missing = test_dir_ / "nonexistent" / "commands";
    CommandReader reader(missing);

    // Should not crash — directory is created by constructor.
    auto cmds = reader.poll();
    EXPECT_TRUE(cmds.empty());
    EXPECT_TRUE(fs::exists(missing));
}

}  // namespace
}  // namespace kairos::daemon
