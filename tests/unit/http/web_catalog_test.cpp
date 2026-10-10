/// tests/unit/http/web_catalog_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  WebCatalog tests — asset resolution: embedded fallback, external         ║
// ║  override, path safety, content types (Roadmap §3.1)                      ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#ifdef KAIROS_HTTP_ENABLED

#include "kairos/http/web_catalog.hpp"

#include "test_helpers.hpp"

#include <gtest/gtest.h>

#include <fstream>

namespace kairos::http::web {
namespace {

TEST(WebCatalogTest, EmbeddedAssetLoads) {
    WebCatalog catalog;  // No external root → embedded only.
    auto asset = catalog.load("templates/base.html");
    ASSERT_TRUE(asset.has_value());
    EXPECT_EQ(asset->source, AssetSource::Embedded);
    // The base layout must contain the two-pass interpolation anchor.
    EXPECT_NE(asset->content.find("{{ content }}"), std::string::npos);
}

TEST(WebCatalogTest, EmbeddedStaticCssLoads) {
    WebCatalog catalog;
    auto asset = catalog.load("static/css/kairos.css");
    ASSERT_TRUE(asset.has_value());
    // All three themes are present (§3.2).
    EXPECT_NE(asset->content.find("[data-theme=\"blue\"]"),
              std::string::npos);
    EXPECT_NE(asset->content.find("[data-theme=\"dark\"]"),
              std::string::npos);
    EXPECT_NE(asset->content.find("[data-theme=\"light\"]"),
              std::string::npos);
}

TEST(WebCatalogTest, EveryPageTemplateLoads) {
    WebCatalog catalog;
    for (const char* tmpl : {
        "templates/base.html",
        "templates/dashboard.html",
        "templates/error.html",
        "templates/workflows/list.html",
        "templates/workflows/detail.html",
        "templates/jobs/list.html",
        "templates/jobs/detail.html",
        "templates/runs/list.html",
        "templates/runs/detail.html",
        "templates/events/list.html",
        "templates/events/detail.html",
        "templates/watches/list.html",
        "templates/watches/detail.html",
        "templates/triggers/list.html",
    }) {
        auto asset = catalog.load(tmpl);
        ASSERT_TRUE(asset.has_value()) << "missing embedded template " << tmpl;
        EXPECT_FALSE(asset->content.empty()) << tmpl;
    }
}

TEST(WebCatalogTest, MissingAssetReturnsNullopt) {
    WebCatalog catalog;
    EXPECT_FALSE(catalog.load("templates/nope.html").has_value());
    EXPECT_FALSE(catalog.load("static/nope.js").has_value());
}

TEST(WebCatalogTest, RejectsTraversalPaths) {
    WebCatalog catalog;
    EXPECT_FALSE(catalog.load("../CMakeLists.txt").has_value());
    EXPECT_FALSE(catalog.load("templates/../../src/cli/cli_app.cpp")
                 .has_value());
    EXPECT_FALSE(catalog.load("").has_value());
    EXPECT_FALSE(catalog.load("/etc/passwd").has_value());
    EXPECT_FALSE(catalog.load(R"(templates\..\..\secret)").has_value());
}

TEST(WebCatalogTest, ExternalOverrideWins) {
    kairos::testing::TempDir dir;
    dir.create_file("templates/base.html",
                    "EXTERNAL BASE {{ content }}");
    WebCatalog catalog(dir.path());

    auto asset = catalog.load("templates/base.html");
    ASSERT_TRUE(asset.has_value());
    EXPECT_EQ(asset->source, AssetSource::External);
    EXPECT_EQ(asset->content, "EXTERNAL BASE {{ content }}");
    EXPECT_TRUE(catalog.has_external());
}

TEST(WebCatalogTest, ExternalMissingFallsBackToEmbedded) {
    kairos::testing::TempDir dir;
    dir.create_file("templates/unrelated.html", "x");
    WebCatalog catalog(dir.path());

    // Present externally → external; absent externally → embedded.
    auto base = catalog.load("templates/base.html");
    ASSERT_TRUE(base.has_value());
    EXPECT_EQ(base->source, AssetSource::Embedded);
}

TEST(WebCatalogTest, NonexistentRootUsesEmbeddedOnly) {
    WebCatalog catalog(std::filesystem::path("/nonexistent/kairos/web"));
    EXPECT_FALSE(catalog.has_external());
    auto asset = catalog.load("templates/base.html");
    ASSERT_TRUE(asset.has_value());
    EXPECT_EQ(asset->source, AssetSource::Embedded);
}

TEST(WebCatalogTest, ContentTypes) {
    EXPECT_EQ(WebCatalog::content_type("x.html"), "text/html; charset=utf-8");
    EXPECT_EQ(WebCatalog::content_type("x.css"), "text/css; charset=utf-8");
    EXPECT_EQ(WebCatalog::content_type("x.js"),
              "application/javascript; charset=utf-8");
    EXPECT_EQ(WebCatalog::content_type("x.svg"), "image/svg+xml");
    EXPECT_EQ(WebCatalog::content_type("x.json"),
              "application/json; charset=utf-8");
    EXPECT_EQ(WebCatalog::content_type("x.unknown"),
              "application/octet-stream");
    EXPECT_EQ(WebCatalog::content_type("noext"),
              "application/octet-stream");
}

}  // namespace
}  // namespace kairos::http::web

#endif  // KAIROS_HTTP_ENABLED
