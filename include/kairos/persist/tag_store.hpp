/// include/kairos/persist/tag_store.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/persist/tag_store.hpp — Tag persistence layer                    ║
// ║                                                                          ║
// ║  CRUD operations for entity_tags table and tags_json propagation on      ║
// ║  runs. Used by YAML loader on config load/reload and by QueryReader      ║
// ║  for filtering.                                                          ║
// ║                                                                          ║
// ║  Spec reference: Roadmap §6 Tags System                                 ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace kairos::persist {

/// Entity types that support tags.
enum class TagEntityType {
    Workflow,
    Job,
    WatchGroup,
    Trigger
};

/// Convert enum to string for DB storage.
inline std::string to_string(TagEntityType t) {
    switch (t) {
        case TagEntityType::Workflow:   return "workflow";
        case TagEntityType::Job:        return "job";
        case TagEntityType::WatchGroup: return "watch_group";
        case TagEntityType::Trigger:    return "trigger";
    }
    return "unknown";
}

/// Parse string from DB to enum.
inline std::optional<TagEntityType> parse_tag_entity_type(
    const std::string& s)
{
    if (s == "workflow")    return TagEntityType::Workflow;
    if (s == "job")         return TagEntityType::Job;
    if (s == "watch_group") return TagEntityType::WatchGroup;
    if (s == "trigger")     return TagEntityType::Trigger;
    return std::nullopt;
}

/// A batch of tags to sync for a single entity.
struct EntityTagSet {
    TagEntityType entity_type;
    std::string   entity_id;
    std::vector<std::string> tags;
};

/// Tag store — operates on a SQLite database.
///
/// Thread safety: callers must synchronize writes. Read-only
/// concurrent access is fine with WAL mode.
class TagStore {
public:
    explicit TagStore(SQLite::Database& db);

    // ─── Write operations ─────────────────────────────────────────

    /// Replace all tags for a single entity (delete old, insert new).
    void sync_tags(TagEntityType entity_type,
                   const std::string& entity_id,
                   const std::vector<std::string>& tags);

    /// Bulk sync: clear all tags for entity_type, then insert new ones.
    /// More efficient than per-entity sync when reloading all configs.
    void sync_all_tags(TagEntityType entity_type,
                       const std::vector<EntityTagSet>& entities);

    /// Clear all tags for a given entity type.
    void clear_all_tags(TagEntityType entity_type);

    // ─── Read operations ──────────────────────────────────────────

    /// Get all tags for a specific entity.
    std::vector<std::string> get_tags(TagEntityType entity_type,
                                      const std::string& entity_id) const;

    /// Get all tags for a specific entity (string type overload).
    std::vector<std::string> get_tags(const std::string& entity_type,
                                      const std::string& entity_id) const;

    /// Get entity IDs that have ALL of the specified tags (AND semantics).
    std::vector<std::string> find_entities_with_tags(
        TagEntityType entity_type,
        const std::vector<std::string>& required_tags) const;

    /// Get entity IDs that have ANY of the specified tags (OR semantics).
    std::vector<std::string> find_entities_with_any_tag(
        TagEntityType entity_type,
        const std::vector<std::string>& tags) const;

    /// Check if an entity has a specific tag.
    bool has_tag(TagEntityType entity_type,
                 const std::string& entity_id,
                 const std::string& tag) const;

    /// Check if an entity has a specific tag (string type overload).
    bool has_tag(const std::string& entity_type,
                 const std::string& entity_id,
                 const std::string& tag) const;

    /// Get all distinct tags, optionally filtered by entity type.
    std::vector<std::string> all_tags(
        std::optional<TagEntityType> entity_type = std::nullopt) const;

    // ─── JSON helpers ─────────────────────────────────────────────

    /// Serialize a tag list to JSON array string for runs.tags_json.
    static std::string tags_to_json(const std::vector<std::string>& tags);

    /// Parse JSON array string back to tag list.
    static std::vector<std::string> tags_from_json(
        const std::string& json_str);

    /// Merge and deduplicate two tag lists (sorted output).
    static std::vector<std::string> merge_tags(
        const std::vector<std::string>& a,
        const std::vector<std::string>& b);

private:
    SQLite::Database& db_;
};

} // namespace kairos::persist
