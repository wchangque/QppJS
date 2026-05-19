#include "qppjs/runtime/symbol_table.h"

namespace qppjs {

SymbolTable::SymbolTable() {
    // Pre-allocate Well-Known Symbols (no description stored in descriptions_,
    // they use a fixed naming convention when toString() is called).
    well_known_iterator     = next_id_++;
    well_known_to_primitive = next_id_++;
    well_known_has_instance = next_id_++;
    well_known_to_string_tag = next_id_++;

    descriptions_[well_known_iterator]      = "Symbol.iterator";
    descriptions_[well_known_to_primitive]  = "Symbol.toPrimitive";
    descriptions_[well_known_has_instance]  = "Symbol.hasInstance";
    descriptions_[well_known_to_string_tag] = "Symbol.toStringTag";
}

uint64_t SymbolTable::NewSymbol(std::optional<std::string> description) {
    uint64_t id = next_id_++;
    if (description.has_value()) {
        descriptions_[id] = std::move(*description);
    }
    return id;
}

const std::string* SymbolTable::GetDescription(uint64_t id) const {
    auto it = descriptions_.find(id);
    if (it == descriptions_.end()) return nullptr;
    return &it->second;
}

uint64_t SymbolTable::ForKey(std::string_view key) {
    auto it = registry_key_to_id_.find(std::string(key));
    if (it != registry_key_to_id_.end()) {
        return it->second;
    }
    uint64_t id = next_id_++;
    std::string key_str(key);
    registry_key_to_id_[key_str] = id;
    registry_id_to_key_[id] = key_str;
    descriptions_[id] = key_str;
    return id;
}

std::optional<std::string> SymbolTable::KeyForId(uint64_t id) const {
    auto it = registry_id_to_key_.find(id);
    if (it == registry_id_to_key_.end()) return std::nullopt;
    return it->second;
}

}  // namespace qppjs
