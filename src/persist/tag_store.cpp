/// src/persist/tag_store.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  tag_store.cpp — Tag persistence implementation                          ║
// ║  Spec reference: Roadmap §6 Tags System                                 ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/persist/tag_store.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <set>

namespace kairos::persist {

TagStore::TagStore(SQLite::Database& db)
    : db_(db) {}

// ═══════════════════════════════════════════════════════════════════════════
// Write operations
// ═══════════════════════════════════════════════════════════════════════════

void TagStore::sync_tags(TagEntityType entity_type,
                         const std::string& entity_id,
                         const std::vector<std::string>& tags) {
    const auto type_str = to_string(entity_type);

    SQLite::Transaction txn(db_);

    // Delete existing tags for this entity.
    SQLite::Statement del(db_,
        "DELETE FROM entity_tags WHERE entity_type = ? AND entity_id = ?");
    del.bind(1, type_str);
    del.bind(2, entity_id);
    del.exec();

    // Insert new tags.
    if (!tags.empty()) {
        SQLite::Statement ins(db_,
            "INSERT OR IGNORE INTO entity_tags "
            "(entity_type, entity_id, tag) VALUES (?, ?, ?)");
        for (const auto& tag : tags) {
            ins.bind(1, type_str);
            ins.bind(2, entity_id);
            ins.bind(3, tag);
            ins.exec();
            ins.reset();
        }
    }

    txn.commit();

    spdlog::debug("Synced {} tags for {} '{}'",
                  tags.size(), type_str, entity_id);
}

void TagStore::sync_all_tags(TagEntityType entity_type,
                             const std::vector<EntityTagSet>& entities) {
    const auto type_str = to_string(entity_type);

    SQLite::Transaction txn(db_);

    // Clear all tags for this entity type.
    SQLite::Statement del(db_,
        "DELETE FROM entity_tags WHERE entity_type = ?");
    del.bind(1, type_str);
    del.exec();

    // Bulk insert.
    if (!entities.empty()) {
        SQLite::Statement ins(db_,
            "INSERT OR IGNORE INTO entity_tags "
            "(entity_type, entity_id, tag) VALUES (?, ?, ?)");

        int count = 0;
        for (const auto& entity : entities) {
            for (const auto& tag : entity.tags) {
                ins.bind(1, type_str);
                ins.bind(2, entity.entity_id);
                ins.bind(3, tag);
                ins.exec();
                ins.reset();
                ++count;
            }
        }

        spdlog::debug("Bulk synced {} tag entries for entity type '{}'",
                      count, type_str);
    }

    txn.commit();
}

void TagStore::clear_all_tags(TagEntityType entity_type) {
    const auto type_str = to_string(entity_type);
    SQLite::Statement del(db_,
        "DELETE FROM entity_tags WHERE entity_type = ?");
    del.bind(1, type_str);
    del.exec();
}

// ═══════════════════════════════════════════════════════════════════════════
// Read operations
// ═══════════════════════════════════════════════════════════════════════════

std::vector<std::string> TagStore::get_tags(
    TagEntityType entity_type, const std::string& entity_id) const
{
    return get_tags(to_string(entity_type), entity_id);
}

std::vector<std::string> TagStore::get_tags(
    const std::string& entity_type, const std::string& entity_id) const
{
    std::vector<std::string> result;
    SQLite::Statement query(db_,
        "SELECT tag FROM entity_tags "
        "WHERE entity_type = ? AND entity_id = ? ORDER BY tag");
    query.bind(1, entity_type);
    query.bind(2, entity_id);

    while (query.executeStep()) {
        result.push_back(query.getColumn(0).getString());
    }
    return result;
}

std::vector<std::string> TagStore::find_entities_with_tags(
    TagEntityType entity_type,
    const std::vector<std::string>& required_tags) const
{
    if (required_tags.empty()) return {};

    const auto type_str = to_string(entity_type);

    // AND semantics via GROUP BY + HAVING COUNT(DISTINCT tag) = N.
    std::string placeholders;
    for (size_t i = 0; i < required_tags.size(); ++i) {
        if (i > 0) placeholders += ", ";
        placeholders += "?";
    }

    std::string sql =
        "SELECT entity_id FROM entity_tags "
        "WHERE entity_type = ? AND tag IN (" + placeholders + ") "
        "GROUP BY entity_id "
        "HAVING COUNT(DISTINCT tag) = ?";

    SQLite::Statement query(db_, sql);
    query.bind(1, type_str);
    for (size_t i = 0; i < required_tags.size(); ++i) {
        query.bind(static_cast<int>(i + 2), required_tags[i]);
    }
    query.bind(static_cast<int>(required_tags.size() + 2),
               static_cast<int>(required_tags.size()));

    std::vector<std::string> result;
    while (query.executeStep()) {
        result.push_back(query.getColumn(0).getString());
    }
    return result;
}

std::vector<std::string> TagStore::find_entities_with_any_tag(
    TagEntityType entity_type,
    const std::vector<std::string>& tags) const
{
    if (tags.empty()) return {};

    const auto type_str = to_string(entity_type);

    std::string placeholders;
    for (size_t i = 0; i < tags.size(); ++i) {
        if (i > 0) placeholders += ", ";
        placeholders += "?";
    }

    std::string sql =
        "SELECT DISTINCT entity_id FROM entity_tags "
        "WHERE entity_type = ? AND tag IN (" + placeholders + ")";

    SQLite::Statement query(db_, sql);
    query.bind(1, type_str);
    for (size_t i = 0; i < tags.size(); ++i) {
        query.bind(static_cast<int>(i + 2), tags[i]);
    }

    std::vector<std::string> result;
    while (query.executeStep()) {
        result.push_back(query.getColumn(0).getString());
    }
    return result;
}

bool TagStore::has_tag(TagEntityType entity_type,
                       const std::string& entity_id,
                       const std::string& tag) const {
    return has_tag(to_string(entity_type), entity_id, tag);
}

bool TagStore::has_tag(const std::string& entity_type,
                       const std::string& entity_id,
                       const std::string& tag) const {
    SQLite::Statement query(db_,
        "SELECT 1 FROM entity_tags "
        "WHERE entity_type = ? AND entity_id = ? AND tag = ? "
        "LIMIT 1");
    query.bind(1, entity_type);
    query.bind(2, entity_id);
    query.bind(3, tag);
    return query.executeStep();
}

std::vector<std::string> TagStore::all_tags(
    std::optional<TagEntityType> entity_type) const
{
    std::vector<std::string> result;

    if (entity_type) {
        SQLite::Statement query(db_,
            "SELECT DISTINCT tag FROM entity_tags "
            "WHERE entity_type = ? ORDER BY tag");
        query.bind(1, to_string(*entity_type));
        while (query.executeStep()) {
            result.push_back(query.getColumn(0).getString());
        }
    } else {
        SQLite::Statement query(db_,
            "SELECT DISTINCT tag FROM entity_tags ORDER BY tag");
        while (query.executeStep()) {
            result.push_back(query.getColumn(0).getString());
        }
    }
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// JSON helpers
// ═══════════════════════════════════════════════════════════════════════════

std::string TagStore::tags_to_json(const std::vector<std::string>& tags) {
    nlohmann::json j = tags;
    return j.dump();
}

std::vector<std::string> TagStore::tags_from_json(
    const std::string& json_str)
{
    if (json_str.empty()) return {};
    try {
        auto j = nlohmann::json::parse(json_str);
        if (!j.is_array()) return {};
        std::vector<std::string> result;
        result.reserve(j.size());
        for (const auto& item : j) {
            if (item.is_string()) {
                result.push_back(item.get<std::string>());
            }
        }
        return result;
    } catch (const nlohmann::json::exception&) {
        return {};
    }
}

std::vector<std::string> TagStore::merge_tags(
    const std::vector<std::string>& a,
    const std::vector<std::string>& b)
{
    std::set<std::string> merged(a.begin(), a.end());
    merged.insert(b.begin(), b.end());
    return {merged.begin(), merged.end()};
}

} // namespace kairos::persist
