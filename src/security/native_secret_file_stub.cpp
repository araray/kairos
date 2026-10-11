/// src/security/native_secret_file_stub.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Native secrets file — stub for builds without OpenSSL (KAIROS_VAULT=OFF)║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/security/native_secret_file.hpp"

#include <stdexcept>

namespace kairos::security {

bool NativeSecretFile::exists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) && !ec;
}

std::unordered_map<std::string, std::string> NativeSecretFile::load(
    const std::filesystem::path&, const std::string&) {
    throw std::runtime_error(
        "native secrets file requires a build with KAIROS_VAULT=ON "
        "(OpenSSL)");
}

void NativeSecretFile::save(
    const std::filesystem::path&, const std::string&,
    const std::unordered_map<std::string, std::string>&) {
    throw std::runtime_error(
        "native secrets file requires a build with KAIROS_VAULT=ON "
        "(OpenSSL)");
}

}  // namespace kairos::security
