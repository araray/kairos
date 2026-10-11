/// include/kairos/security/native_secret_file.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/security/native_secret_file.hpp — Kairos-native encrypted store ║
// ║                                                                          ║
// ║  Roadmap §12.2.1: a simple AES-256-GCM + PBKDF2 secrets file at          ║
// ║  <data_dir>/secrets.enc — lighter than Ansible Vault, managed by the    ║
// ║  `kairos secrets` CLI and readable by the daemon (backend "file").      ║
// ║                                                                          ║
// ║  File format (text, hex-encoded fields, one per line):                  ║
// ║      KAIROS_SECRET_STORE v1                                              ║
// ║      <salt_hex: 16 bytes>                                               ║
// ║      <iv_hex: 12 bytes>                                                 ║
// ║      <tag_hex: 16 bytes>                                                ║
// ║      <ciphertext_hex: AES-256-GCM(JSON {"key": "value", …})>            ║
// ║                                                                          ║
// ║  Key derivation: PBKDF2-HMAC-SHA256(password, salt, 100_000) → 32 B.   ║
// ║  GCM verifies integrity — wrong password or tampering fails the load.  ║
// ║                                                                          ║
// ║  Requires OpenSSL (KAIROS_VAULT=ON); a stub throws otherwise.           ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>

namespace kairos::security {

/// Static helpers for the native encrypted secrets file.
class NativeSecretFile {
public:
    /// Default file name under the data dir.
    static constexpr const char* kDefaultName = "secrets.enc";

    /// Whether the file exists.
    [[nodiscard]] static bool exists(
        const std::filesystem::path& path);

    /// Load, decrypt, and parse. Returns the key→value map.
    /// @throws std::runtime_error with a precise reason on missing
    ///         file, bad header, wrong password, or tampering.
    [[nodiscard]] static std::unordered_map<std::string, std::string>
    load(const std::filesystem::path& path,
         const std::string& password);

    /// Encrypt and save atomically (temp file + rename).
    /// @throws std::runtime_error on crypto or I/O failure.
    static void save(const std::filesystem::path& path,
                     const std::string& password,
                     const std::unordered_map<std::string, std::string>&
                         secrets);
};

}  // namespace kairos::security
