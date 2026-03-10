/// src/security/secret_store.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  SecretStore — Ansible Vault AES-256-CTR decryption via OpenSSL           ║
// ║                                                                           ║
// ║  This file is compiled ONLY when KAIROS_VAULT=ON.                        ║
// ║  When OFF, secret_store_stub.cpp is compiled instead.                    ║
// ║                                                                           ║
// ║  Algorithm (matches Ansible Vault 1.1):                                  ║
// ║    1. Read vault file, verify header "$ANSIBLE_VAULT;1.1;AES256"         ║
// ║    2. Hex-decode payload → binary                                        ║
// ║    3. Split: salt(32) | hmac(32) | ciphertext                            ║
// ║    4. PBKDF2-HMAC-SHA256(password, salt, 10000) → 80 bytes               ║
// ║       aes_key[0:32], hmac_key[32:64], iv[64:80]                          ║
// ║    5. HMAC-SHA256(hmac_key, ciphertext) == stored hmac                   ║
// ║    6. AES-256-CTR decrypt → plaintext                                    ║
// ║    7. PKCS7 unpad                                                        ║
// ║    8. YAML parse → key/value map                                          ║
// ║                                                                           ║
// ║  Spec reference: §17.2                                                   ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/security/secret_store.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/err.h>
#include <yaml-cpp/yaml.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace kairos::security {

// ── Internal helpers ─────────────────────────────────────────────────────

namespace {

/// Secure-clear a vector of bytes using volatile writes.
void secure_clear_bytes(std::vector<uint8_t>& buf) {
    if (buf.empty()) return;
    volatile uint8_t* p = buf.data();
    for (std::size_t i = 0; i < buf.size(); ++i) {
        p[i] = 0;
    }
    buf.clear();
}

/// Secure-clear a std::string using volatile writes.
void secure_clear_string(std::string& s) {
    if (s.empty()) return;
    volatile char* p = s.data();
    for (std::size_t i = 0; i < s.size(); ++i) {
        p[i] = '\0';
    }
    s.clear();
}

/// Decode a hex string to binary bytes.
/// Ansible Vault hex-encodes each of the three segments (salt, hmac,
/// ciphertext) on separate lines.
std::vector<uint8_t> hex_decode(std::string_view hex) {
    if (hex.size() % 2 != 0) {
        throw std::runtime_error("Vault: invalid hex string (odd length)");
    }
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        unsigned int byte = 0;
        auto [ptr, ec] = std::from_chars(
            hex.data() + i, hex.data() + i + 2, byte, 16);
        if (ec != std::errc{}) {
            throw std::runtime_error("Vault: invalid hex character");
        }
        out.push_back(static_cast<uint8_t>(byte));
    }
    return out;
}

/// PBKDF2-HMAC-SHA256 key derivation.
/// Returns 80 bytes: key(32) + hmac_key(32) + iv(16).
std::vector<uint8_t> derive_key(
    const std::string& password,
    const std::vector<uint8_t>& salt,
    int iterations = 10000)
{
    constexpr int DERIVED_KEY_LEN = 80;
    std::vector<uint8_t> derived(DERIVED_KEY_LEN);

    int rc = PKCS5_PBKDF2_HMAC(
        password.c_str(),
        static_cast<int>(password.size()),
        salt.data(),
        static_cast<int>(salt.size()),
        iterations,
        EVP_sha256(),
        DERIVED_KEY_LEN,
        derived.data());

    if (rc != 1) {
        throw std::runtime_error("Vault: PBKDF2 key derivation failed");
    }
    return derived;
}

/// HMAC-SHA256 verification.
bool verify_hmac(
    const uint8_t* hmac_key, int hmac_key_len,
    const uint8_t* data, int data_len,
    const uint8_t* expected_hmac, int expected_hmac_len)
{
    unsigned char computed[EVP_MAX_MD_SIZE];
    unsigned int computed_len = 0;

    HMAC(EVP_sha256(),
         hmac_key, hmac_key_len,
         data, data_len,
         computed, &computed_len);

    if (static_cast<int>(computed_len) != expected_hmac_len) {
        return false;
    }

    // Constant-time comparison to prevent timing attacks.
    return CRYPTO_memcmp(computed, expected_hmac, computed_len) == 0;
}

/// AES-256-CTR decryption.
std::vector<uint8_t> aes256_ctr_decrypt(
    const uint8_t* key, const uint8_t* iv,
    const uint8_t* ciphertext, int ciphertext_len)
{
    std::vector<uint8_t> plaintext(ciphertext_len + EVP_MAX_BLOCK_LENGTH);
    int out_len1 = 0;
    int out_len2 = 0;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        throw std::runtime_error("Vault: failed to create cipher context");
    }

    // RAII cleanup for the cipher context.
    struct CtxGuard {
        EVP_CIPHER_CTX* c;
        ~CtxGuard() { EVP_CIPHER_CTX_free(c); }
    } guard{ctx};

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_ctr(),
                            nullptr, key, iv) != 1) {
        throw std::runtime_error("Vault: cipher init failed");
    }

    if (EVP_DecryptUpdate(ctx, plaintext.data(), &out_len1,
                           ciphertext, ciphertext_len) != 1) {
        throw std::runtime_error("Vault: decryption failed");
    }

    if (EVP_DecryptFinal_ex(ctx, plaintext.data() + out_len1,
                             &out_len2) != 1) {
        throw std::runtime_error("Vault: decryption finalization failed");
    }

    plaintext.resize(out_len1 + out_len2);
    return plaintext;
}

/// Remove PKCS7 padding from decrypted plaintext.
/// The last byte indicates the padding length; all padding bytes
/// must have the same value.
void pkcs7_unpad(std::vector<uint8_t>& data) {
    if (data.empty()) {
        throw std::runtime_error("Vault: empty plaintext after decryption");
    }

    uint8_t pad_len = data.back();
    if (pad_len == 0 || pad_len > 16 ||
        static_cast<std::size_t>(pad_len) > data.size()) {
        throw std::runtime_error("Vault: invalid PKCS7 padding");
    }

    // Verify all padding bytes.
    for (std::size_t i = data.size() - pad_len; i < data.size(); ++i) {
        if (data[i] != pad_len) {
            throw std::runtime_error(
                "Vault: corrupted PKCS7 padding (bad password?)");
        }
    }

    data.resize(data.size() - pad_len);
}

/// Parse the vault file: extract header and hex-encoded payload lines.
///
/// Ansible Vault format:
///   Line 1: $ANSIBLE_VAULT;1.1;AES256
///   Line 2: hex(salt)
///   Line 3: hex(hmac)
///   Line 4: hex(ciphertext)
///
/// Note: The hex payload may be wrapped across multiple lines
/// (continuation lines with no delimiter). Ansible wraps at 80 chars.
struct VaultPayload {
    std::vector<uint8_t> salt;
    std::vector<uint8_t> hmac;
    std::vector<uint8_t> ciphertext;
};

VaultPayload parse_vault_file(const std::string& file_path) {
    std::ifstream file(file_path);
    if (!file.is_open()) {
        throw std::runtime_error(
            "Vault: cannot open file: " + file_path);
    }

    // Read header line.
    std::string header;
    std::getline(file, header);

    // Trim whitespace from header.
    while (!header.empty() &&
           (header.back() == '\r' || header.back() == '\n' ||
            header.back() == ' ')) {
        header.pop_back();
    }

    if (header != "$ANSIBLE_VAULT;1.1;AES256") {
        throw std::runtime_error(
            "Vault: unsupported format (expected "
            "'$ANSIBLE_VAULT;1.1;AES256', got '" + header + "')");
    }

    // Read remaining lines — the payload is hex, possibly wrapped.
    // Ansible Vault wraps the hex output at 80 characters per line.
    // The payload is three hex strings separated by newlines:
    //   1. salt (64 hex chars = 32 bytes)
    //   2. hmac (64 hex chars = 32 bytes)
    //   3. ciphertext (variable length)
    //
    // But each of these may be wrapped across multiple lines.
    // The standard Python implementation joins all lines and splits
    // by '\n' after hex-decoding. However, the more common format
    // is that the three segments are on individual lines and ONLY
    // the ciphertext may wrap.
    //
    // We handle both by collecting hex content until we have all
    // three segments.
    std::string hex_content;
    std::string line;
    while (std::getline(file, line)) {
        // Strip carriage returns and trailing whitespace.
        while (!line.empty() &&
               (line.back() == '\r' || line.back() == '\n' ||
                line.back() == ' ')) {
            line.pop_back();
        }
        if (!line.empty()) {
            hex_content += line;
            hex_content += '\n';
        }
    }

    // The Ansible Vault format has hex payload split across exactly
    // three newline-separated segments: salt, hmac, ciphertext.
    // Each segment is a hex string (possibly wrapped at 80 chars
    // by ansible-vault encrypt). We need to split on the segment
    // boundaries.
    //
    // Ansible's vault lib joins wrapped lines and then splits on
    // newline to get exactly 3 fields. Let's do the same: split
    // the hex_content by newline into segments.
    std::vector<std::string> segments;
    {
        std::istringstream ss(hex_content);
        std::string seg;
        while (std::getline(ss, seg)) {
            if (!seg.empty()) segments.push_back(std::move(seg));
        }
    }

    if (segments.size() < 3) {
        throw std::runtime_error(
            "Vault: expected at least 3 hex segments, got " +
            std::to_string(segments.size()));
    }

    // If there are more than 3 segments, the ciphertext was wrapped.
    // Join segments 2..N into the ciphertext.
    std::string ct_hex = segments[2];
    for (std::size_t i = 3; i < segments.size(); ++i) {
        ct_hex += segments[i];
    }

    VaultPayload payload;
    payload.salt = hex_decode(segments[0]);
    payload.hmac = hex_decode(segments[1]);
    payload.ciphertext = hex_decode(ct_hex);

    if (payload.salt.size() != 32) {
        throw std::runtime_error(
            "Vault: expected 32-byte salt, got " +
            std::to_string(payload.salt.size()));
    }
    if (payload.hmac.size() != 32) {
        throw std::runtime_error(
            "Vault: expected 32-byte HMAC, got " +
            std::to_string(payload.hmac.size()));
    }

    return payload;
}

/// Parse decrypted YAML into key→value pairs.
/// Only top-level string/number/bool values are extracted.
/// Nested maps are flattened with dot notation (one level deep).
std::unordered_map<std::string, std::string> parse_secrets_yaml(
    const std::string& yaml_text)
{
    std::unordered_map<std::string, std::string> secrets;
    YAML::Node root = YAML::Load(yaml_text);

    if (!root.IsMap()) {
        throw std::runtime_error(
            "Vault: decrypted content is not a YAML map");
    }

    for (auto it = root.begin(); it != root.end(); ++it) {
        std::string key = it->first.as<std::string>();
        const auto& val = it->second;

        if (val.IsScalar()) {
            secrets[key] = val.as<std::string>();
        } else if (val.IsMap()) {
            // One level of nesting: "parent.child"
            for (auto nit = val.begin(); nit != val.end(); ++nit) {
                if (nit->second.IsScalar()) {
                    std::string nested_key =
                        key + "." + nit->first.as<std::string>();
                    secrets[nested_key] = nit->second.as<std::string>();
                }
            }
        }
        // Skip sequences and deeper nesting (not supported for secrets).
    }

    return secrets;
}

}  // anonymous namespace

// ── SecretStore implementation ───────────────────────────────────────────

SecretStore::~SecretStore() {
    // SecureStrings self-clear on destruction via their destructors.
    // We just clear the map structure.
    secrets_.clear();
    loaded_ = false;
}

void SecretStore::load_vault(const std::string& vault_file_path,
                              const std::string& password)
{
    spdlog::info("SecretStore: loading vault from {}", vault_file_path);

    // Step 1: Parse the vault file (header + hex payload).
    auto payload = parse_vault_file(vault_file_path);

    // Step 2: Derive keys via PBKDF2-HMAC-SHA256.
    auto derived = derive_key(password, payload.salt, 10000);

    // Split derived key material.
    const uint8_t* aes_key  = derived.data();         // [0:32]
    const uint8_t* hmac_key = derived.data() + 32;    // [32:64]
    const uint8_t* iv       = derived.data() + 64;    // [64:80]

    // Step 3: Verify HMAC.
    if (!verify_hmac(hmac_key, 32,
                     payload.ciphertext.data(),
                     static_cast<int>(payload.ciphertext.size()),
                     payload.hmac.data(),
                     static_cast<int>(payload.hmac.size()))) {
        secure_clear_bytes(derived);
        throw std::runtime_error(
            "Vault: HMAC verification failed (bad password?)");
    }

    // Step 4: AES-256-CTR decrypt.
    auto plaintext = aes256_ctr_decrypt(
        aes_key, iv,
        payload.ciphertext.data(),
        static_cast<int>(payload.ciphertext.size()));

    // Clear derived key material immediately after use.
    secure_clear_bytes(derived);

    // Step 5: Remove PKCS7 padding.
    pkcs7_unpad(plaintext);

    // Step 6: Parse YAML.
    std::string yaml_text(
        reinterpret_cast<const char*>(plaintext.data()),
        plaintext.size());
    secure_clear_bytes(plaintext);

    auto parsed = parse_secrets_yaml(yaml_text);
    secure_clear_string(yaml_text);

    // Step 7: Store as SecureStrings.
    secrets_.clear();
    for (auto& [key, value] : parsed) {
        secrets_.emplace(key, SecureString(std::move(value)));
    }
    loaded_ = true;

    spdlog::info("SecretStore: loaded {} secrets from vault",
                 secrets_.size());
}

void SecretStore::load_from_map(
    std::unordered_map<std::string, std::string> secrets)
{
    secrets_.clear();
    for (auto& [key, value] : secrets) {
        secrets_.emplace(key, SecureString(std::move(value)));
    }
    loaded_ = true;
}

std::optional<std::string>
SecretStore::get(const std::string& key) const {
    auto it = secrets_.find(key);
    if (it == secrets_.end()) return std::nullopt;
    return it->second.to_string();
}

std::vector<std::string> SecretStore::keys() const {
    std::vector<std::string> result;
    result.reserve(secrets_.size());
    for (const auto& [key, _] : secrets_) {
        result.push_back(key);
    }
    return result;
}

std::vector<std::string> SecretStore::values() const {
    std::vector<std::string> result;
    result.reserve(secrets_.size());
    for (const auto& [_, value] : secrets_) {
        result.push_back(value.to_string());
    }
    return result;
}

std::function<std::optional<std::string>(const std::string&)>
SecretStore::make_resolver() const {
    return [this](const std::string& key) -> std::optional<std::string> {
        return this->get(key);
    };
}

}  // namespace kairos::security
