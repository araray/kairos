/// include/kairos/security/secret_store.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/security/secret_store.hpp — Ansible Vault integration            ║
// ║                                                                          ║
// ║  SecureString: move-only buffer with secure zeroing on destruction.      ║
// ║  SecretStore:  decrypts Ansible Vault YAML, provides key→value lookup.  ║
// ║                                                                          ║
// ║  Thread safety: read-only after construction. Multiple threads may       ║
// ║  call get()/keys()/values() concurrently.                               ║
// ║                                                                          ║
// ║  Build guard: full implementation requires KAIROS_VAULT=ON and OpenSSL. ║
// ║  When KAIROS_VAULT is OFF, load_vault() throws std::runtime_error.      ║
// ║                                                                          ║
// ║  Spec reference: §17.1–§17.3                                            ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#ifndef _WIN32
#include <unistd.h>
#else
extern "C" char** _environ;
#endif
#include <unordered_map>
#include <vector>

namespace kairos::security {

// ── SecureString ──────────────────────────────────────────────────────────

/// A string-like buffer that overwrites its contents with zeros on
/// destruction. Move-only to prevent accidental duplication of
/// sensitive data.
///
/// Uses volatile writes in secure_clear() to prevent the compiler
/// from optimizing away the zeroing (C++20 does not guarantee
/// std::memset_explicit availability on all toolchains, so we use
/// the volatile idiom as a portable fallback).
class SecureString {
public:
    SecureString() = default;

    explicit SecureString(std::string value)
        : data_(value.begin(), value.end())
    {
        // Zero the source string's internal buffer.
        volatile char* p = value.data();
        for (std::size_t i = 0; i < value.size(); ++i) {
            p[i] = '\0';
        }
    }

    ~SecureString() { secure_clear(); }

    // Move-only (no copies — prevent accidental duplication).
    SecureString(const SecureString&) = delete;
    SecureString& operator=(const SecureString&) = delete;

    SecureString(SecureString&& other) noexcept
        : data_(std::move(other.data_)) {}

    SecureString& operator=(SecureString&& other) noexcept {
        if (this != &other) {
            secure_clear();
            data_ = std::move(other.data_);
        }
        return *this;
    }

    [[nodiscard]] std::string_view view() const noexcept {
        return {data_.data(), data_.size()};
    }

    [[nodiscard]] bool empty() const noexcept {
        return data_.empty();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return data_.size();
    }

    /// Explicit conversion to std::string (for passing to APIs
    /// that require std::string). Use sparingly.
    [[nodiscard]] std::string to_string() const {
        return {data_.data(), data_.size()};
    }

private:
    std::vector<char> data_;

    /// Overwrite buffer contents with zeros.
    /// Uses volatile writes to prevent compiler optimization.
    void secure_clear() noexcept {
        if (data_.empty()) return;
        volatile char* p = data_.data();
        for (std::size_t i = 0; i < data_.size(); ++i) {
            p[i] = '\0';
        }
        data_.clear();
    }
};

// ── SecretStore ───────────────────────────────────────────────────────────

/// In-memory secret store. Holds decrypted vault contents.
///
/// Lifecycle:
///   1. Daemon creates SecretStore.
///   2. If vault enabled, calls load_vault() with file path + password.
///   3. Pipeline uses get() to resolve ${{ secrets.key }} references.
///   4. On daemon shutdown, destructor secure-clears all secrets.
///
/// Thread safety: read-only after load_vault(). Multiple threads
/// may call get()/keys()/values() concurrently without locking.
class SecretStore {
public:
    SecretStore() = default;
    ~SecretStore();

    /// Load secrets from an Ansible Vault-encrypted YAML file.
    ///
    /// The vault file format is standard Ansible Vault 1.1:
    ///   Header:  $ANSIBLE_VAULT;1.1;AES256
    ///   Body:    hex-encoded (salt + hmac + ciphertext)
    ///
    /// Decryption algorithm:
    ///   1. PBKDF2-HMAC-SHA256(password, salt, 10000 iters) → 80 bytes
    ///      key[0:32], hmac_key[32:64], iv[64:80]
    ///   2. Verify HMAC-SHA256(hmac_key, ciphertext) == stored hmac
    ///   3. AES-256-CTR decrypt(key, iv, ciphertext) → plaintext
    ///   4. Strip PKCS7 padding
    ///   5. Parse YAML plaintext into key→value pairs
    ///
    /// @throws std::runtime_error on decryption failure, bad password,
    ///         YAML parse error, or if KAIROS_VAULT is not compiled.
    void load_vault(const std::string& vault_file_path,
                    const std::string& password);

    /// Load secrets from a pre-decrypted map (for testing or
    /// non-vault secret sources).
    void load_from_map(std::unordered_map<std::string, std::string> secrets);

    /// §12.2.3: load secrets from environment variables.
    /// Every variable named <prefix><KEY> (prefix default
    /// "KAIROS_SECRET_") contributes secret KEY with its value —
    /// e.g. KAIROS_SECRET_DEPLOY_API_KEY → secrets.deploy_api_key.
    /// Case: the key keeps the variable's case after the prefix.
    /// @return Number of secrets loaded.
    std::size_t load_from_env(
        const std::string& prefix = "KAIROS_SECRET_") {
        std::unordered_map<std::string, std::string> found;
#ifdef _WIN32
        char** table = _environ;
#else
        char** table = environ;
#endif
        for (char** e = table; e && *e; ++e) {
            std::string_view entry(*e);
            if (entry.size() <= prefix.size()) continue;
            if (entry.substr(0, prefix.size()) != prefix) continue;
            auto eq = entry.find('=');
            if (eq == std::string_view::npos || eq < prefix.size()) {
                continue;
            }
            std::string key(
                entry.substr(prefix.size(), eq - prefix.size()));
            std::string value(entry.substr(eq + 1));
            if (!key.empty()) {
                found.emplace(std::move(key), std::move(value));
            }
        }
        auto count = found.size();
        load_from_map(std::move(found));
        return count;
    }

    /// Look up a secret by key.
    /// Returns std::nullopt if the key does not exist.
    [[nodiscard]] std::optional<std::string>
    get(const std::string& key) const;

    /// Return all secret keys (for diagnostics — values NOT exposed).
    [[nodiscard]] std::vector<std::string> keys() const;

    /// Return all secret values (for the OutputMultiplexer masker).
    /// WARNING: Handle with extreme care. These are decrypted secrets.
    [[nodiscard]] std::vector<std::string> values() const;

    /// Check if the vault is loaded and contains secrets.
    [[nodiscard]] bool is_loaded() const noexcept { return loaded_; }

    /// Number of secrets loaded.
    [[nodiscard]] std::size_t size() const noexcept {
        return secrets_.size();
    }

    /// Create a SecretResolver function suitable for EnvBuilder.
    /// The returned closure captures `this` — the SecretStore must
    /// outlive any EnvBuilder that uses it.
    [[nodiscard]] std::function<std::optional<std::string>(
        const std::string&)> make_resolver() const;

private:
    std::unordered_map<std::string, SecureString> secrets_;
    bool loaded_ = false;
};

}  // namespace kairos::security
