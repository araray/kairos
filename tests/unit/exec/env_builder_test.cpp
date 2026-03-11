/// tests/unit/exec/env_builder_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Tests for EnvBuilder                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/exec/env_builder.hpp"

#include <gtest/gtest.h>

#include <cstdlib>

// ── Cross-platform setenv/unsetenv ───────────────────────────────────────
// MSVC doesn't provide POSIX setenv/unsetenv.  Use _putenv_s instead.
namespace {
void test_setenv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    ::setenv(name, value, 1);
#endif
}
void test_unsetenv(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    ::unsetenv(name);
#endif
}
}  // anonymous namespace

using namespace kairos::exec;

TEST(EnvBuilderTest, EmptyBuild) {
    EnvBuilder builder;
    auto env = builder.build();
    // PATH should always be present.
    EXPECT_TRUE(env.contains("PATH"));
}

TEST(EnvBuilderTest, InheritParent) {
    // Set a test variable in the current process.
    test_setenv("KAIROS_TEST_INHERIT", "hello");

    EnvBuilder builder;
    builder.inherit_parent();
    auto env = builder.build();

    EXPECT_EQ(env["KAIROS_TEST_INHERIT"], "hello");
    EXPECT_TRUE(env.contains("PATH"));

    test_unsetenv("KAIROS_TEST_INHERIT");
}

TEST(EnvBuilderTest, LayerPrecedence) {
    EnvBuilder builder;
    builder.add_global({{"KEY", "global"}})
           .add_workflow({{"KEY", "workflow"}})
           .add_job({{"KEY", "job"}})
           .add_step({{"KEY", "step"}});

    auto env = builder.build();
    // Step-level should override all previous layers.
    EXPECT_EQ(env["KEY"], "step");
}

TEST(EnvBuilderTest, LayerPrecedencePartial) {
    EnvBuilder builder;
    builder.add_global({{"A", "global_a"}, {"B", "global_b"}})
           .add_job({{"B", "job_b"}});

    auto env = builder.build();
    EXPECT_EQ(env["A"], "global_a");  // Not overridden.
    EXPECT_EQ(env["B"], "job_b");     // Overridden by job layer.
}

TEST(EnvBuilderTest, SecretResolution) {
    EnvBuilder builder;
    builder.add_step({
        {"DB_PASS", "${{ secrets.db_password }}"},
        {"API_KEY", "${{ secrets.api_key }}"},
        {"NORMAL",  "plain_value"},
    });

    builder.resolve_secrets([](const std::string& key)
        -> std::optional<std::string> {
        if (key == "db_password") return "s3cret!";
        if (key == "api_key") return "ak-12345";
        return std::nullopt;
    });

    auto env = builder.build();
    EXPECT_EQ(env["DB_PASS"], "s3cret!");
    EXPECT_EQ(env["API_KEY"], "ak-12345");
    EXPECT_EQ(env["NORMAL"], "plain_value");

    // Check that resolved keys are tracked.
    const auto& keys = builder.resolved_secret_keys();
    EXPECT_EQ(keys.size(), 2u);
}

TEST(EnvBuilderTest, UnresolvedSecretLeftAsIs) {
    EnvBuilder builder;
    builder.add_step({
        {"TOKEN", "${{ secrets.missing_key }}"},
    });

    builder.resolve_secrets([](const std::string&)
        -> std::optional<std::string> {
        return std::nullopt;
    });

    auto env = builder.build();
    EXPECT_EQ(env["TOKEN"], "${{ secrets.missing_key }}");
}

TEST(EnvBuilderTest, SecretWithWhitespace) {
    EnvBuilder builder;
    builder.add_step({
        {"PASS", "${{  secrets.pw  }}"},
    });

    builder.resolve_secrets([](const std::string& key)
        -> std::optional<std::string> {
        if (key == "pw") return "pass123";
        return std::nullopt;
    });

    auto env = builder.build();
    EXPECT_EQ(env["PASS"], "pass123");
}

TEST(EnvBuilderTest, MultipleSecretsInOneValue) {
    EnvBuilder builder;
    builder.add_step({
        {"URL", "https://${{ secrets.user }}:${{ secrets.pass }}@host"},
    });

    builder.resolve_secrets([](const std::string& key)
        -> std::optional<std::string> {
        if (key == "user") return "admin";
        if (key == "pass") return "s3cr3t";
        return std::nullopt;
    });

    auto env = builder.build();
    EXPECT_EQ(env["URL"], "https://admin:s3cr3t@host");
}

TEST(EnvBuilderTest, KairosMetadataVars) {
    EnvBuilder builder;
    builder.add_kairos_vars(
        "run-abc123",
        "job-build",
        "stp-001",
        "wfl-deploy",
        "manual",
        "corr-xyz",
        "/data/kairos",
        "/data/kairos/kairos.db");

    auto env = builder.build();
    EXPECT_EQ(env["KAIROS_RUN_ID"], "run-abc123");
    EXPECT_EQ(env["KAIROS_JOB_ID"], "job-build");
    EXPECT_EQ(env["KAIROS_STEP_ID"], "stp-001");
    EXPECT_EQ(env["KAIROS_WORKFLOW_ID"], "wfl-deploy");
    EXPECT_EQ(env["KAIROS_TRIGGER_TYPE"], "manual");
    EXPECT_EQ(env["KAIROS_CORRELATION_ID"], "corr-xyz");
    EXPECT_EQ(env["KAIROS_DATA_DIR"], "/data/kairos");
    EXPECT_EQ(env["KAIROS_DB_PATH"], "/data/kairos/kairos.db");
}

TEST(EnvBuilderTest, PathAlwaysPresent) {
    EnvBuilder builder;
    // Don't inherit, don't set PATH explicitly.
    auto env = builder.build();
    EXPECT_TRUE(env.contains("PATH"));
    EXPECT_FALSE(env["PATH"].empty());
}

TEST(EnvBuilderTest, NullSecretResolver) {
    EnvBuilder builder;
    builder.add_step({{"KEY", "${{ secrets.x }}"}})
           .resolve_secrets(nullptr);

    auto env = builder.build();
    // Should leave as-is since resolver is null.
    EXPECT_EQ(env["KEY"], "${{ secrets.x }}");
}

TEST(EnvBuilderTest, CaptureCurrentEnv) {
    test_setenv("KAIROS_CAPTURE_TEST", "captured");

    auto env = capture_current_env();
    EXPECT_EQ(env["KAIROS_CAPTURE_TEST"], "captured");

    test_unsetenv("KAIROS_CAPTURE_TEST");
}

TEST(EnvBuilderTest, FullPipeline) {
    // Simulate a complete environment build pipeline.
    EnvBuilder builder;
    builder.inherit_parent()
           .add_global({{"GLOBAL_KEY", "g1"}})
           .add_workflow({{"WF_KEY", "w1"}, {"SHARED", "wf"}})
           .add_job({{"JOB_KEY", "j1"}, {"SHARED", "job"}})
           .add_step({{"STEP_KEY", "s1"}, {"SECRET_VAL", "${{ secrets.token }}"}})
           .resolve_secrets([](const std::string& key)
               -> std::optional<std::string> {
               if (key == "token") return "tok-abc";
               return std::nullopt;
           })
           .add_kairos_vars("r1", "j1", "s1", "w1", "manual",
                            "c1", "/d", "/d/k.db");

    auto env = builder.build();
    EXPECT_EQ(env["GLOBAL_KEY"], "g1");
    EXPECT_EQ(env["WF_KEY"], "w1");
    EXPECT_EQ(env["JOB_KEY"], "j1");
    EXPECT_EQ(env["STEP_KEY"], "s1");
    EXPECT_EQ(env["SHARED"], "job");  // Job overrides workflow.
    EXPECT_EQ(env["SECRET_VAL"], "tok-abc");
    EXPECT_EQ(env["KAIROS_RUN_ID"], "r1");
    EXPECT_TRUE(env.contains("PATH"));
}
