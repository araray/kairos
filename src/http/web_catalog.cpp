/// src/http/web_catalog.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Web asset resolution — see include/kairos/http/web_catalog.hpp          ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#ifdef KAIROS_HTTP_ENABLED

#include "kairos/http/web_catalog.hpp"
#include "kairos/http/web_resources.hpp"

#include <fstream>
#include <spdlog/spdlog.h>

namespace kairos::http::web {

namespace {

/// Reject malformed virtual paths: empty, absolute, containing "..",
/// or containing backslashes (which std::filesystem may treat as
/// separators on Windows).
bool is_safe_virtual_path(const std::string& path) {
    if (path.empty() || path.front() == '/') return false;
    if (path.find("..") != std::string::npos) return false;
    if (path.find('\\') != std::string::npos) return false;
    return true;
}

/// Read a whole file as text. Returns nullopt on any failure.
std::optional<std::string> read_file(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec) || ec) return std::nullopt;

    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;

    std::string content;
    in.seekg(0, std::ios::end);
    if (in.tellg() > static_cast<std::streamoff>(64 * 1024 * 1024)) {
        return std::nullopt;  // 64 MiB sanity cap.
    }
    const auto size = static_cast<std::size_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    content.resize(size);
    if (size > 0) in.read(content.data(), static_cast<std::streamsize>(size));
    if (!in && !in.eof()) return std::nullopt;
    return content;
}

/// Look up an embedded asset by virtual path.
std::optional<std::string_view> find_embedded(
    const std::string& virtual_path) {
    for (const auto& r : web_resources::embedded_resources()) {
        if (r.path == virtual_path) return r.content;
    }
    return std::nullopt;
}

}  // namespace

WebCatalog::WebCatalog(std::filesystem::path external_root)
    : external_root_(std::move(external_root))
{
    if (!external_root_.empty()) {
        std::error_code ec;
        external_ok_ = std::filesystem::is_directory(external_root_, ec) && !ec;
        if (external_ok_) {
            spdlog::info("Web UI: external asset root {} (embedded fallback)",
                         external_root_.string());
        } else {
            spdlog::info("Web UI: asset root {} missing — embedded assets only",
                         external_root_.string());
        }
    }
}

std::optional<Asset> WebCatalog::load(const std::string& virtual_path) const {
    if (!is_safe_virtual_path(virtual_path)) return std::nullopt;

    // 1. External override.
    if (external_ok_) {
        auto abs = external_root_ / virtual_path;
        if (auto content = read_file(abs)) {
            return Asset{std::move(*content), AssetSource::External};
        }
    }

    // 2. Embedded fallback.
    if (auto content = find_embedded(virtual_path)) {
        return Asset{std::string(*content), AssetSource::Embedded};
    }

    return std::nullopt;
}

std::string WebCatalog::content_type(const std::string& virtual_path) {
    auto dot = virtual_path.find_last_of('.');
    if (dot == std::string::npos) return "application/octet-stream";
    const auto ext = virtual_path.substr(dot + 1);

    if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
    if (ext == "css")  return "text/css; charset=utf-8";
    if (ext == "js")   return "application/javascript; charset=utf-8";
    if (ext == "json") return "application/json; charset=utf-8";
    if (ext == "svg")  return "image/svg+xml";
    if (ext == "png")  return "image/png";
    if (ext == "ico")  return "image/x-icon";
    if (ext == "txt")  return "text/plain; charset=utf-8";
    if (ext == "woff2") return "font/woff2";
    return "application/octet-stream";
}

}  // namespace kairos::http::web

#endif  // KAIROS_HTTP_ENABLED
