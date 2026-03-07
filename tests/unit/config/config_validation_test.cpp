// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  config_validation_test.cpp — Semantic validation tests                   ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/config/config_store.hpp"

#include <gtest/gtest.h>

namespace kairos::config {

class ValidationTest : public ::testing::Test {
protected:
    /// Build a Config with all defaults (should pass validation).
    confy::Config make_default_config() {
        confy::LoadOptions opts;
        opts.defaults = build_kairos_defaults();
        return confy::Config::load(opts);
    }

    /// Build a Config and override a specific key.
    confy::Config make_config_with(const std::string& key, const confy::Value& val) {
        auto cfg = make_default_config();
        cfg.set(key, val);
        return cfg;
    }
};

TEST_F(ValidationTest, DefaultConfigPassesValidation) {
    auto cfg = make_default_config();
    auto errors = validate_config(cfg);
    EXPECT_TRUE(errors.empty())
        << "Default config should pass validation; first error: "
        << (errors.empty() ? "none" : errors[0].key_path + ": " + errors[0].message);
}

TEST_F(ValidationTest, ZeroWorkerPoolSizeFails) {
    auto cfg = make_config_with("kairos.runners.worker_pool_size", 0);
    auto errors = validate_config(cfg);
    ASSERT_FALSE(errors.empty());
    bool found = false;
    for (const auto& e : errors) {
        if (e.key_path == "kairos.runners.worker_pool_size") found = true;
    }
    EXPECT_TRUE(found) << "Should have error for worker_pool_size = 0";
}

TEST_F(ValidationTest, NegativeTimeoutFails) {
    auto cfg = make_config_with("kairos.runners.default_timeout_s", -1);
    auto errors = validate_config(cfg);
    ASSERT_FALSE(errors.empty());
    bool found = false;
    for (const auto& e : errors) {
        if (e.key_path == "kairos.runners.default_timeout_s") found = true;
    }
    EXPECT_TRUE(found);
}

TEST_F(ValidationTest, InvalidLogLevelFails) {
    auto cfg = make_config_with("kairos.logging.level", "verbose");
    auto errors = validate_config(cfg);
    ASSERT_FALSE(errors.empty());
    bool found = false;
    for (const auto& e : errors) {
        if (e.key_path == "kairos.logging.level") found = true;
    }
    EXPECT_TRUE(found);
}

TEST_F(ValidationTest, ValidLogLevelsPass) {
    for (const auto& level : {"trace", "debug", "info", "warn", "error", "critical"}) {
        auto cfg = make_config_with("kairos.logging.level", level);
        auto errors = validate_config(cfg);
        for (const auto& e : errors) {
            EXPECT_NE(e.key_path, "kairos.logging.level")
                << "Level '" << level << "' should be valid";
        }
    }
}

TEST_F(ValidationTest, InvalidWatchModeFails) {
    auto cfg = make_config_with("kairos.watch.default_mode", "polling");
    auto errors = validate_config(cfg);
    bool found = false;
    for (const auto& e : errors) {
        if (e.key_path == "kairos.watch.default_mode") found = true;
    }
    EXPECT_TRUE(found);
}

TEST_F(ValidationTest, ValidWatchModesPass) {
    for (const auto& mode : {"native", "sample", "hybrid"}) {
        auto cfg = make_config_with("kairos.watch.default_mode", mode);
        auto errors = validate_config(cfg);
        for (const auto& e : errors) {
            EXPECT_NE(e.key_path, "kairos.watch.default_mode")
                << "Mode '" << mode << "' should be valid";
        }
    }
}

TEST_F(ValidationTest, InvalidPortFails) {
    auto cfg = make_config_with("kairos.http.listen_port", 99999);
    auto errors = validate_config(cfg);
    bool found = false;
    for (const auto& e : errors) {
        if (e.key_path == "kairos.http.listen_port") found = true;
    }
    EXPECT_TRUE(found);
}

TEST_F(ValidationTest, InvalidColorModeFails) {
    auto cfg = make_config_with("kairos.platform.color", "maybe");
    auto errors = validate_config(cfg);
    bool found = false;
    for (const auto& e : errors) {
        if (e.key_path == "kairos.platform.color") found = true;
    }
    EXPECT_TRUE(found);
}

}  // namespace kairos::config
