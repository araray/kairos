/// include/kairos/http/web_catalog.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/http/web_catalog.hpp — web asset resolution (external → embedded) ║
// ║                                                                          ║
// ║  Resolves virtual asset paths ("templates/base.html",                    ║
// ║  "static/css/kairos.css") to file content:                               ║
// ║                                                                          ║
// ║    1. External override — <web_root>/<path> when a web root is set       ║
// ║       (kairos.http.web_root, or <data_dir>/web). Enables hot-editing     ║
// ║       of the UI without recompiling. Read per-request, no caching.       ║
// ║    2. Embedded fallback — assets compiled into the binary from web/      ║
// ║       (see web_resources.hpp), keeping the single-binary property.       ║
// ║                                                                          ║
// ║  Thread safety: const methods; external reads use stdio streams only.    ║
// ║                                                                          ║
// ║  Spec reference: §26.5 (Roadmap §3.1)                                   ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#ifdef KAIROS_HTTP_ENABLED

#include <filesystem>
#include <optional>
#include <string>

namespace kairos::http::web {

/// Which layer served an asset (for diagnostics and the X-Kairos-Source
/// debug header).
enum class AssetSource {
    Embedded,   ///< Compiled-in default.
    External,   ///< Overridden via web root directory.
};

/// Result of a catalog lookup.
struct Asset {
    std::string content;
    AssetSource source;
};

/// Resolver for web assets.
class WebCatalog {
public:
    /// @param external_root  Directory that overrides embedded assets.
    ///                       Empty path = embedded assets only.
    explicit WebCatalog(std::filesystem::path external_root = {});

    /// Load an asset by virtual path (e.g. "static/css/kairos.css").
    /// @return nullopt when neither layer has the asset, or when the path
    ///         is malformed (traversal, absolute, empty).
    [[nodiscard]] std::optional<Asset> load(const std::string& virtual_path) const;

    /// The configured external root (empty when embedding only).
    [[nodiscard]] const std::filesystem::path& external_root() const {
        return external_root_;
    }

    /// True when an external root is configured and exists.
    [[nodiscard]] bool has_external() const { return external_ok_; }

    /// MIME type for a virtual path (by extension).
    /// Defaults to "application/octet-stream".
    [[nodiscard]] static std::string content_type(
        const std::string& virtual_path);

private:
    std::filesystem::path external_root_;
    bool external_ok_ = false;
};

}  // namespace kairos::http::web

#endif  // KAIROS_HTTP_ENABLED
