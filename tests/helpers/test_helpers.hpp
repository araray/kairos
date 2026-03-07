/// tests/helpers/test_helpers.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  test_helpers.hpp — Common test utilities                                 ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace kairos::testing {

/// RAII temporary directory that is removed on destruction.
class TempDir {
public:
    TempDir() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<uint64_t> dist;

        auto tmp = std::filesystem::temp_directory_path();
        path_ = tmp / ("kairos_test_" + std::to_string(dist(gen)));
        std::filesystem::create_directories(path_);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

    /// Create a file inside the temp dir with given content.
    std::filesystem::path create_file(const std::string& name,
                                       const std::string& content) {
        auto fp = path_ / name;
        if (fp.has_parent_path()) {
            std::filesystem::create_directories(fp.parent_path());
        }
        std::ofstream ofs(fp);
        ofs << content;
        return fp;
    }

    // Non-copyable.
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

private:
    std::filesystem::path path_;
};

}  // namespace kairos::testing
