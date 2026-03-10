/// tests/unit/security/secret_store_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  SecretStore unit tests                                                   ║
// ║                                                                           ║
// ║  Tests SecureString, SecretStore (load_from_map), make_resolver(),       ║
// ║  EnvBuilder integration, and OutputMultiplexer masking.                  ║
// ║                                                                           ║
// ║  Vault decryption tests (AES-256-CTR) require KAIROS_VAULT=ON and       ║
// ║  are guarded by #ifdef KAIROS_VAULT.                                    ║
// ║                                                                           ║
// ║  Spec reference: §17.1–§17.3                                            ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/security/secret_store.hpp"
#include "kairos/exec/env_builder.hpp"
#include "kairos/exec/output_sink.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace kairos::security {
namespace {

// ═══════════════════════════════════════════════════════════════════════
// SecureString tests
// ═══════════════════════════════════════════════════════════════════════

TEST(SecureStringTest, DefaultConstruction) {
    SecureString ss;
    EXPECT_TRUE(ss.empty());
    EXPECT_EQ(ss.size(), 0u);
    EXPECT_EQ(ss.view(), "");
}

TEST(SecureStringTest, ConstructFromString) {
    SecureString ss(std::string("my-secret-value"));
    EXPECT_FALSE(ss.empty());
    EXPECT_EQ(ss.size(), 15u);
    EXPECT_EQ(ss.view(), "my-secret-value");
}

TEST(SecureStringTest, MoveConstruction) {
    SecureString original(std::string("password123"));
    SecureString moved(std::move(original));

    EXPECT_EQ(moved.view(), "password123");
    // Original is moved-from — implementation-defined but should be empty
    // after std::vector move.
    EXPECT_TRUE(original.empty());
}

TEST(SecureStringTest, MoveAssignment) {
    SecureString a(std::string("alpha"));
    SecureString b(std::string("beta"));

    b = std::move(a);
    EXPECT_EQ(b.view(), "alpha");
}

TEST(SecureStringTest, SelfMoveAssignment) {
    SecureString ss(std::string("test"));
    auto* ptr = &ss;
    *ptr = std::move(ss);
    // Should not crash; value may or may not be preserved.
}

TEST(SecureStringTest, ToString) {
    SecureString ss(std::string("convert-me"));
    std::string result = ss.to_string();
    EXPECT_EQ(result, "convert-me");
}

// ═══════════════════════════════════════════════════════════════════════
// SecretStore — load_from_map tests
// ═══════════════════════════════════════════════════════════════════════

TEST(SecretStoreTest, EmptyByDefault) {
    SecretStore store;
    EXPECT_FALSE(store.is_loaded());
    EXPECT_EQ(store.size(), 0u);
    EXPECT_EQ(store.get("anything"), std::nullopt);
    EXPECT_TRUE(store.keys().empty());
    EXPECT_TRUE(store.values().empty());
}

TEST(SecretStoreTest, LoadFromMap) {
    SecretStore store;
    store.load_from_map({
        {"db_password", "s3cret!"},
        {"api_key",     "sk-abc123"},
    });

    EXPECT_TRUE(store.is_loaded());
    EXPECT_EQ(store.size(), 2u);

    auto db_pw = store.get("db_password");
    ASSERT_TRUE(db_pw.has_value());
    EXPECT_EQ(*db_pw, "s3cret!");

    auto api = store.get("api_key");
    ASSERT_TRUE(api.has_value());
    EXPECT_EQ(*api, "sk-abc123");

    // Non-existent key.
    EXPECT_EQ(store.get("nonexistent"), std::nullopt);
}

TEST(SecretStoreTest, Keys) {
    SecretStore store;
    store.load_from_map({{"a", "1"}, {"b", "2"}, {"c", "3"}});

    auto keys = store.keys();
    EXPECT_EQ(keys.size(), 3u);
    std::sort(keys.begin(), keys.end());
    EXPECT_EQ(keys[0], "a");
    EXPECT_EQ(keys[1], "b");
    EXPECT_EQ(keys[2], "c");
}

TEST(SecretStoreTest, Values) {
    SecretStore store;
    store.load_from_map({{"x", "val1"}, {"y", "val2"}});

    auto values = store.values();
    EXPECT_EQ(values.size(), 2u);
    std::sort(values.begin(), values.end());
    EXPECT_EQ(values[0], "val1");
    EXPECT_EQ(values[1], "val2");
}

TEST(SecretStoreTest, ReloadOverwrites) {
    SecretStore store;
    store.load_from_map({{"old", "value"}});
    EXPECT_TRUE(store.get("old").has_value());

    // Reload with new data.
    store.load_from_map({{"new", "value2"}});
    EXPECT_EQ(store.get("old"), std::nullopt);
    EXPECT_EQ(*store.get("new"), "value2");
}

// ═══════════════════════════════════════════════════════════════════════
// SecretStore — make_resolver tests
// ═══════════════════════════════════════════════════════════════════════

TEST(SecretStoreTest, MakeResolver) {
    SecretStore store;
    store.load_from_map({{"token", "abc-def-ghi"}});

    auto resolver = store.make_resolver();

    auto result = resolver("token");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "abc-def-ghi");

    EXPECT_EQ(resolver("nonexistent"), std::nullopt);
}

// ═══════════════════════════════════════════════════════════════════════
// EnvBuilder + SecretStore integration
// ═══════════════════════════════════════════════════════════════════════

TEST(SecretStoreEnvTest, ResolveSecretsInEnv) {
    SecretStore store;
    store.load_from_map({
        {"db_pass",  "p@ssw0rd"},
        {"api_key",  "sk-12345"},
    });

    exec::EnvBuilder builder;
    builder.add_step({
        {"DB_PASSWORD",  "${{ secrets.db_pass }}"},
        {"API_KEY",      "${{ secrets.api_key }}"},
        {"PLAIN_VALUE",  "no-secret-here"},
    });
    builder.resolve_secrets(store.make_resolver());

    auto env = builder.build();

    EXPECT_EQ(env["DB_PASSWORD"], "p@ssw0rd");
    EXPECT_EQ(env["API_KEY"], "sk-12345");
    EXPECT_EQ(env["PLAIN_VALUE"], "no-secret-here");

    // Verify resolved_secret_keys tracks what was resolved.
    auto resolved = builder.resolved_secret_keys();
    EXPECT_EQ(resolved.size(), 2u);
}

TEST(SecretStoreEnvTest, UnresolvedSecretLeftAsIs) {
    SecretStore store;
    store.load_from_map({});  // Empty vault.

    exec::EnvBuilder builder;
    builder.add_step({
        {"MISSING", "${{ secrets.nonexistent }}"},
    });
    builder.resolve_secrets(store.make_resolver());

    auto env = builder.build();
    // Unresolved references are left as-is (Phase 5 logs a warning).
    EXPECT_EQ(env["MISSING"], "${{ secrets.nonexistent }}");
}

TEST(SecretStoreEnvTest, MultipleReferencesInOneValue) {
    SecretStore store;
    store.load_from_map({
        {"user", "admin"},
        {"pass", "s3cret"},
    });

    exec::EnvBuilder builder;
    builder.add_step({
        {"DSN", "postgres://${{ secrets.user }}:${{ secrets.pass }}@localhost/db"},
    });
    builder.resolve_secrets(store.make_resolver());

    auto env = builder.build();
    EXPECT_EQ(env["DSN"], "postgres://admin:s3cret@localhost/db");
}

// ═══════════════════════════════════════════════════════════════════════
// OutputMultiplexer — secret masking tests (§17.3)
// ═══════════════════════════════════════════════════════════════════════

TEST(SecretMaskingTest, MaskSecretsInOutput) {
    exec::OutputMultiplexer mux;

    // Set secret values (sorted longest-first per §17.3).
    std::vector<std::string> secrets = {"password123", "password", "token"};
    std::sort(secrets.begin(), secrets.end(),
        [](const auto& a, const auto& b) { return a.size() > b.size(); });
    mux.set_secret_values(std::move(secrets));

    std::string received;
    mux.add_sink([&](std::string_view chunk, bool) {
        received += std::string(chunk);
    });

    mux.deliver("Connected with password123 using token xyz", false);

    // "password123" should be masked as "***", not partially as
    // "***123" (longest-first ordering prevents this).
    EXPECT_EQ(received, "Connected with *** using *** xyz");
}

TEST(SecretMaskingTest, NoSecretsPassthrough) {
    exec::OutputMultiplexer mux;
    // No secrets set.

    std::string received;
    mux.add_sink([&](std::string_view chunk, bool) {
        received += std::string(chunk);
    });

    mux.deliver("plain output with no secrets", false);
    EXPECT_EQ(received, "plain output with no secrets");
}

TEST(SecretMaskingTest, EmptySecretIgnored) {
    exec::OutputMultiplexer mux;
    mux.set_secret_values({"", "actual_secret"});

    std::string received;
    mux.add_sink([&](std::string_view chunk, bool) {
        received += std::string(chunk);
    });

    mux.deliver("revealing actual_secret in log", false);
    EXPECT_EQ(received, "revealing *** in log");
}

// ═══════════════════════════════════════════════════════════════════════
// Vault decryption tests (require KAIROS_VAULT=ON)
// ═══════════════════════════════════════════════════════════════════════

#ifdef KAIROS_VAULT

TEST(SecretStoreVaultTest, InvalidFileThrows) {
    SecretStore store;
    EXPECT_THROW(
        store.load_vault("/nonexistent/path.vault", "password"),
        std::runtime_error);
}

TEST(SecretStoreVaultTest, BadHeaderThrows) {
    // Create a temp file with invalid header.
    auto tmp = std::filesystem::temp_directory_path() / "bad_vault.yml";
    {
        std::ofstream f(tmp);
        f << "NOT_A_VAULT\n";
        f << "deadbeef\n";
    }

    SecretStore store;
    EXPECT_THROW(
        store.load_vault(tmp.string(), "password"),
        std::runtime_error);

    std::filesystem::remove(tmp);
}

#else  // !KAIROS_VAULT

TEST(SecretStoreVaultTest, LoadVaultThrowsWhenNotCompiled) {
    SecretStore store;
    EXPECT_THROW(
        store.load_vault("any_file.vault", "password"),
        std::runtime_error);
}

#endif  // KAIROS_VAULT

}  // anonymous namespace
}  // namespace kairos::security
