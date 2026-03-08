/// tests/unit/watch/sample_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Sample+diff unit tests                                                   ║
// ║  Tests compute_diff(), classify_changes(), and event type classification. ║
// ║  Spec reference: §12.6, §12.8                                            ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/watch/sample.hpp"
#include "kairos/watch/event_types.hpp"

#include <gtest/gtest.h>

#include <chrono>

using namespace kairos::watch;
using namespace std::chrono_literals;

// ── Helpers ─────────────────────────────────────────────────────────────

namespace {

auto T0 = std::chrono::system_clock::time_point{
    std::chrono::hours(24 * 365 * 56)};  // ~2026
auto T1 = T0 + 60s;  // 1 minute later.
auto T2 = T0 + 300s; // 5 minutes later.

FileMetrics make_file(const std::string& path, int64_t size,
                      std::chrono::system_clock::time_point mtime,
                      const std::string& perms = "0644") {
    FileMetrics m;
    m.path = path;
    m.entry_type = "file";
    m.size = size;
    m.last_modified = mtime;
    m.permissions = perms;
    return m;
}

FileMetrics make_dir(const std::string& path, int files_count,
                     int subdirs_count,
                     std::chrono::system_clock::time_point mtime) {
    FileMetrics m;
    m.path = path;
    m.entry_type = "directory";
    m.last_modified = mtime;
    m.files_count = files_count;
    m.subdirs_count = subdirs_count;
    return m;
}

}  // namespace

// ── compute_diff tests ──────────────────────────────────────────────────

TEST(SampleDiffTest, EmptySamplesProduceEmptyDiff) {
    Sample current, previous;
    auto diff = compute_diff(current, previous);
    EXPECT_TRUE(diff.empty());
    EXPECT_EQ(diff.change_count(), 0u);
}

TEST(SampleDiffTest, NewFilesDetectedAsCreated) {
    Sample previous;
    Sample current;
    current.entries["/a.txt"] = make_file("/a.txt", 100, T0);
    current.entries["/b.txt"] = make_file("/b.txt", 200, T0);

    auto diff = compute_diff(current, previous);

    EXPECT_EQ(diff.created.size(), 2u);
    EXPECT_TRUE(diff.deleted.empty());
    EXPECT_TRUE(diff.modified.empty());

    // Sorted for determinism.
    EXPECT_EQ(diff.created[0], "/a.txt");
    EXPECT_EQ(diff.created[1], "/b.txt");
}

TEST(SampleDiffTest, RemovedFilesDetectedAsDeleted) {
    Sample previous;
    previous.entries["/a.txt"] = make_file("/a.txt", 100, T0);
    previous.entries["/b.txt"] = make_file("/b.txt", 200, T0);

    Sample current;
    current.entries["/a.txt"] = make_file("/a.txt", 100, T0);
    // /b.txt is gone.

    auto diff = compute_diff(current, previous);

    EXPECT_TRUE(diff.created.empty());
    ASSERT_EQ(diff.deleted.size(), 1u);
    EXPECT_EQ(diff.deleted[0], "/b.txt");
    EXPECT_TRUE(diff.modified.empty());
}

TEST(SampleDiffTest, SizeChangeDetected) {
    Sample previous;
    previous.entries["/f.txt"] = make_file("/f.txt", 100, T0);

    Sample current;
    current.entries["/f.txt"] = make_file("/f.txt", 200, T0);

    auto diff = compute_diff(current, previous);

    EXPECT_TRUE(diff.created.empty());
    EXPECT_TRUE(diff.deleted.empty());
    ASSERT_EQ(diff.modified.size(), 1u);
    ASSERT_TRUE(diff.modified.count("/f.txt"));

    const auto& changes = diff.modified.at("/f.txt");
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].field, "size");
    EXPECT_EQ(changes[0].old_value, "100");
    EXPECT_EQ(changes[0].new_value, "200");
}

TEST(SampleDiffTest, MtimeChangeDetected) {
    Sample previous;
    previous.entries["/f.txt"] = make_file("/f.txt", 100, T0);

    Sample current;
    current.entries["/f.txt"] = make_file("/f.txt", 100, T1);

    auto diff = compute_diff(current, previous);

    ASSERT_EQ(diff.modified.size(), 1u);
    const auto& changes = diff.modified.at("/f.txt");
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].field, "mtime");
}

TEST(SampleDiffTest, PermissionChangeDetected) {
    Sample previous;
    previous.entries["/f.txt"] = make_file("/f.txt", 100, T0, "0644");

    Sample current;
    current.entries["/f.txt"] = make_file("/f.txt", 100, T0, "0755");

    auto diff = compute_diff(current, previous);

    ASSERT_EQ(diff.modified.size(), 1u);
    const auto& changes = diff.modified.at("/f.txt");
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].field, "permissions");
    EXPECT_EQ(changes[0].old_value, "0644");
    EXPECT_EQ(changes[0].new_value, "0755");
}

TEST(SampleDiffTest, HashChangeDetected) {
    Sample previous;
    auto prev_file = make_file("/f.txt", 100, T0);
    prev_file.sha256 = "aaa111";
    previous.entries["/f.txt"] = prev_file;

    Sample current;
    auto cur_file = make_file("/f.txt", 100, T0);
    cur_file.sha256 = "bbb222";
    current.entries["/f.txt"] = cur_file;

    auto diff = compute_diff(current, previous);

    ASSERT_EQ(diff.modified.size(), 1u);
    const auto& changes = diff.modified.at("/f.txt");
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].field, "sha256");
}

TEST(SampleDiffTest, MultipleChangesOnSameFile) {
    Sample previous;
    previous.entries["/f.txt"] = make_file("/f.txt", 100, T0, "0644");

    Sample current;
    current.entries["/f.txt"] = make_file("/f.txt", 200, T1, "0755");

    auto diff = compute_diff(current, previous);

    ASSERT_EQ(diff.modified.size(), 1u);
    const auto& changes = diff.modified.at("/f.txt");
    EXPECT_EQ(changes.size(), 3u);  // size + mtime + permissions
}

TEST(SampleDiffTest, DirectoryChangesDetected) {
    Sample previous;
    previous.entries["/dir"] = make_dir("/dir", 5, 2, T0);

    Sample current;
    current.entries["/dir"] = make_dir("/dir", 7, 2, T0);

    auto diff = compute_diff(current, previous);

    ASSERT_EQ(diff.modified.size(), 1u);
    const auto& changes = diff.modified.at("/dir");
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].field, "files_count");
}

TEST(SampleDiffTest, UnchangedFilesNotReported) {
    Sample previous;
    previous.entries["/a.txt"] = make_file("/a.txt", 100, T0);
    previous.entries["/b.txt"] = make_file("/b.txt", 200, T0);

    Sample current;
    current.entries["/a.txt"] = make_file("/a.txt", 100, T0);
    current.entries["/b.txt"] = make_file("/b.txt", 200, T0);

    auto diff = compute_diff(current, previous);
    EXPECT_TRUE(diff.empty());
}

TEST(SampleDiffTest, PatternFoundChangeDetected) {
    Sample previous;
    auto pf = make_file("/log.txt", 100, T0);
    pf.pattern_found = false;
    previous.entries["/log.txt"] = pf;

    Sample current;
    auto cf = make_file("/log.txt", 100, T0);
    cf.pattern_found = true;
    current.entries["/log.txt"] = cf;

    auto diff = compute_diff(current, previous);

    ASSERT_EQ(diff.modified.size(), 1u);
    const auto& changes = diff.modified.at("/log.txt");
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].field, "pattern_found");
    EXPECT_EQ(changes[0].new_value, "true");
}

// ── classify_changes tests ──────────────────────────────────────────────

TEST(ClassifyChangesTest, SizeChangeClassifiedCorrectly) {
    std::vector<SampleDiff::FieldChange> changes = {
        {.field = "size", .old_value = "100", .new_value = "200"}};

    auto flags = classify_changes(changes, "file");
    EXPECT_TRUE(has_flag(flags, WatchEventType::SizeChanged));
    EXPECT_FALSE(has_flag(flags, WatchEventType::DirSizeChanged));
}

TEST(ClassifyChangesTest, DirectorySizeChangeClassified) {
    std::vector<SampleDiff::FieldChange> changes = {
        {.field = "size", .old_value = "100", .new_value = "200"}};

    auto flags = classify_changes(changes, "directory");
    EXPECT_TRUE(has_flag(flags, WatchEventType::DirSizeChanged));
    EXPECT_FALSE(has_flag(flags, WatchEventType::SizeChanged));
}

TEST(ClassifyChangesTest, MultipleChangesProduceMultipleFlags) {
    std::vector<SampleDiff::FieldChange> changes = {
        {.field = "size", .old_value = "100", .new_value = "200"},
        {.field = "mtime", .old_value = "1000", .new_value = "2000"},
        {.field = "permissions", .old_value = "0644", .new_value = "0755"},
    };

    auto flags = classify_changes(changes, "file");
    EXPECT_TRUE(has_flag(flags, WatchEventType::SizeChanged));
    EXPECT_TRUE(has_flag(flags, WatchEventType::ContentModified));
    EXPECT_TRUE(has_flag(flags, WatchEventType::PermissionsChanged));
}

TEST(ClassifyChangesTest, PatternFoundClassified) {
    std::vector<SampleDiff::FieldChange> changes = {
        {.field = "pattern_found", .old_value = "false", .new_value = "true"}};

    auto flags = classify_changes(changes, "file");
    EXPECT_TRUE(has_flag(flags, WatchEventType::PatternFound));
}

TEST(ClassifyChangesTest, PatternRemovedClassified) {
    std::vector<SampleDiff::FieldChange> changes = {
        {.field = "pattern_found", .old_value = "true", .new_value = "false"}};

    auto flags = classify_changes(changes, "file");
    EXPECT_TRUE(has_flag(flags, WatchEventType::PatternRemoved));
}

TEST(ClassifyChangesTest, HashChangeClassified) {
    std::vector<SampleDiff::FieldChange> changes = {
        {.field = "sha256", .old_value = "aaa", .new_value = "bbb"}};

    auto flags = classify_changes(changes, "file");
    EXPECT_TRUE(has_flag(flags, WatchEventType::ContentChanged));
}

TEST(ClassifyChangesTest, StringOutputMatchesLegacyFormat) {
    std::vector<SampleDiff::FieldChange> changes = {
        {.field = "size", .old_value = "100", .new_value = "200"},
        {.field = "sha256", .old_value = "aaa", .new_value = "bbb"},
    };

    auto str = classify_changes_string(changes, "file");
    // Should contain both event types, comma-separated.
    EXPECT_NE(str.find("size_changed"), std::string::npos);
    EXPECT_NE(str.find("content_changed"), std::string::npos);
}
