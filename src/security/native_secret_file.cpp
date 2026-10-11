/// src/security/native_secret_file.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Native encrypted secrets file — see the header for the format           ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/security/native_secret_file.hpp"

#include <openssl/evp.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <fstream>
#include <random>
#include <sstream>
#include <vector>

namespace kairos::security {
namespace {

constexpr const char* kMagic = "KAIROS_SECRET_STORE v1";
constexpr int kPbkdf2Iterations = 100'000;
constexpr std::size_t kSaltLen = 16;
constexpr std::size_t kIvLen = 12;
constexpr std::size_t kTagLen = 16;

std::string to_hex(const unsigned char* data, std::size_t len) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        out += digits[data[i] >> 4];
        out += digits[data[i] & 0x0F];
    }
    return out;
}

std::vector<unsigned char> from_hex(const std::string& hex) {
    if (hex.size() % 2 != 0) {
        throw std::runtime_error("secrets file: odd hex length");
    }
    std::vector<unsigned char> out;
    out.reserve(hex.size() / 2);
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        int hi = nib(hex[i]), lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            throw std::runtime_error("secrets file: bad hex");
        }
        out.push_back(static_cast<unsigned char>((hi << 4) | lo));
    }
    return out;
}

/// PBKDF2-HMAC-SHA256 → 32-byte key.
std::vector<unsigned char> derive_key(const std::string& password,
                                      const unsigned char* salt,
                                      std::size_t salt_len) {
    std::vector<unsigned char> key(32);
    int rc = PKCS5_PBKDF2_HMAC(
        password.data(), static_cast<int>(password.size()),
        salt, static_cast<int>(salt_len),
        kPbkdf2Iterations, EVP_sha256(),
        static_cast<int>(key.size()), key.data());
    if (rc != 1) {
        throw std::runtime_error("secrets file: key derivation failed");
    }
    return key;
}

std::vector<unsigned char> random_bytes(std::size_t n) {
    std::vector<unsigned char> out(n);
    std::random_device rd;
    std::uniform_int_distribution<unsigned int> dist(0, 255);
    for (auto& b : out) b = static_cast<unsigned char>(dist(rd));
    return out;
}

}  // namespace

bool NativeSecretFile::exists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) && !ec;
}

std::unordered_map<std::string, std::string> NativeSecretFile::load(
    const std::filesystem::path& path, const std::string& password) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error(
            "secrets file not found: " + path.string());
    }

    std::string magic, salt_hex, iv_hex, tag_hex, ct_hex;
    if (!std::getline(in, magic) || magic != kMagic) {
        throw std::runtime_error(
            "secrets file: bad header (not a Kairos secret store?)");
    }
    if (!std::getline(in, salt_hex) || !std::getline(in, iv_hex) ||
        !std::getline(in, tag_hex) || !std::getline(in, ct_hex)) {
        throw std::runtime_error("secrets file: truncated");
    }

    auto salt = from_hex(salt_hex);
    auto iv = from_hex(iv_hex);
    auto tag = from_hex(tag_hex);
    auto ct = from_hex(ct_hex);
    if (salt.size() != kSaltLen || iv.size() != kIvLen ||
        tag.size() != kTagLen) {
        throw std::runtime_error("secrets file: corrupt field length");
    }

    auto key = derive_key(password, salt.data(), salt.size());

    std::vector<unsigned char> pt(ct.size());
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("secrets file: OOM");
    int len = 0, pt_len = 0;
    bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr,
                                 key.data(), iv.data()) == 1;
    if (ok) ok = EVP_CIPHER_CTX_ctrl(
                   ctx, EVP_CTRL_GCM_SET_TAG, kTagLen, tag.data()) == 1;
    if (ok && !ct.empty()) {
        ok = EVP_DecryptUpdate(ctx, pt.data(), &len,
                               ct.data(), static_cast<int>(ct.size())) == 1;
        pt_len = len;
    }
    if (ok) {
        ok = EVP_DecryptFinal_ex(ctx, pt.data() + pt_len, &len) == 1;
    }
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        throw std::runtime_error(
            "secrets file: wrong password or corrupted file");
    }

    auto parsed = nlohmann::json::parse(
        std::string(pt.begin(), pt.begin() + pt_len));
    std::unordered_map<std::string, std::string> out;
    if (parsed.is_object()) {
        for (auto it = parsed.begin(); it != parsed.end(); ++it) {
            if (it.value().is_string()) {
                out[it.key()] = it.value().get<std::string>();
            }
        }
    }
    return out;
}

void NativeSecretFile::save(
    const std::filesystem::path& path, const std::string& password,
    const std::unordered_map<std::string, std::string>& secrets) {
    nlohmann::json j = nlohmann::json::object();
    for (const auto& [k, v] : secrets) j[k] = v;
    auto plaintext = j.dump();

    auto salt = random_bytes(kSaltLen);
    auto iv = random_bytes(kIvLen);
    auto key = derive_key(password, salt.data(), salt.size());

    std::vector<unsigned char> ct(plaintext.size() + kTagLen);
    std::vector<unsigned char> tag(kTagLen);
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("secrets file: OOM");
    int len = 0;
    bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr,
                                 key.data(), iv.data()) == 1;
    if (ok && !plaintext.empty()) {
        ok = EVP_EncryptUpdate(
                 ctx, ct.data(), &len,
                 reinterpret_cast<const unsigned char*>(plaintext.data()),
                 static_cast<int>(plaintext.size())) == 1;
    }
    int ct_len = len;
    if (ok) {
        ok = EVP_EncryptFinal_ex(ctx, ct.data() + ct_len, &len) == 1;
        ct_len += len;
    }
    if (ok) {
        ok = EVP_CIPHER_CTX_ctrl(
                 ctx, EVP_CTRL_GCM_GET_TAG, kTagLen, tag.data()) == 1;
    }
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        throw std::runtime_error("secrets file: encryption failed");
    }

    // Atomic write: temp + rename.
    std::ostringstream ss;
    ss << kMagic << "\n"
       << to_hex(salt.data(), salt.size()) << "\n"
       << to_hex(iv.data(), iv.size()) << "\n"
       << to_hex(tag.data(), tag.size()) << "\n"
       << to_hex(ct.data(), static_cast<std::size_t>(ct_len)) << "\n";

    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error(
                "secrets file: cannot write " + tmp.string());
        }
        out << ss.str();
        if (!out) {
            throw std::runtime_error(
                "secrets file: write failed " + tmp.string());
        }
    }
    std::error_code ec;
#ifdef _WIN32
    std::filesystem::remove(path, ec);
#endif
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        throw std::runtime_error(
            "secrets file: rename failed: " + ec.message());
    }
}

}  // namespace kairos::security
