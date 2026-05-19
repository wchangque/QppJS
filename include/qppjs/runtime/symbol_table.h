#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace qppjs {

class SymbolTable {
public:
    SymbolTable();

    // Allocate a fresh symbol. If description is provided, store it.
    uint64_t NewSymbol(std::optional<std::string> description);

    // Returns pointer to description string, or nullptr if no description.
    const std::string* GetDescription(uint64_t id) const;

    // Symbol.for: returns existing id for key, or creates new one.
    uint64_t ForKey(std::string_view key);

    // Symbol.keyFor: returns the registered key for a Symbol.for symbol, or nullopt.
    std::optional<std::string> KeyForId(uint64_t id) const;

    // Well-Known Symbol IDs (assigned in constructor).
    uint64_t well_known_iterator;
    uint64_t well_known_to_primitive;
    uint64_t well_known_has_instance;
    uint64_t well_known_to_string_tag;

private:
    uint64_t next_id_ = 1;
    std::unordered_map<uint64_t, std::string> descriptions_;
    std::unordered_map<std::string, uint64_t> registry_key_to_id_;
    std::unordered_map<uint64_t, std::string> registry_id_to_key_;
};

}  // namespace qppjs
