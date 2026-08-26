#pragma once

#include "table/table_types.h"

#include <filesystem>
#include <stdexcept>
#include <string>

// table.json loader (09-table-format.md; 04-milestones.md M5). The load
// phase may throw TableLoadError — startup/table-load code catches it,
// logs, and fails clean (03-process.md §1.6).
namespace tb::table {

class TableLoadError : public std::runtime_error {
public:
    TableLoadError(const std::string& what, std::string json_pointer, std::filesystem::path file)
        : std::runtime_error(what), json_pointer(std::move(json_pointer)), file(std::move(file)) {}

    std::string json_pointer; // e.g. "/elements/3/pivot"
    std::filesystem::path file;
};

// Loads <table_dir>/table.json, expands prefabs (§5), and validates
// (§2/§8 rules owned by the loader; tb_validate at M15 adds the rest).
TableDef load_table(const std::filesystem::path& table_dir);

// Expands one prefab instance into elements (pure, deterministic; §5).
// Exported for the expansion golden test.
std::vector<Element> expand_prefab(const TableDef& partial, const PrefabInstance& inst);

} // namespace tb::table
