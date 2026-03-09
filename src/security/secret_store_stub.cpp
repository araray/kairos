/// src/security/secret_store_stub.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  SecretStore stub — compiled when KAIROS_VAULT is OFF                    ║
// ║                                                                           ║
// ║  load_vault() throws; all other methods work but return empty.           ║
// ║  load_from_map() still works (for testing and non-vault secret sources). ║
// ║                                                                           ║
// ║  Spec reference: §17.2 (KAIROS_VAULT build guard)                       ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/security/secret_store.hpp"

#include <stdexcept>

namespace kairos::security {

SecretStore::~SecretStore() {
    secrets_.clear();
    loaded_ = false;
}

void SecretStore::load_vault(const std::string& /*vault_file_path*/,
                              const std::string& /*password*/)
{
    throw std::runtime_error(
        "Vault support not compiled. Rebuild with -DKAIROS_VAULT=ON "
        "and ensure OpenSSL is available.");
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
