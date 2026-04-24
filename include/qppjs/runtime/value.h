#pragma once

#include "qppjs/runtime/rc_object.h"

#include <bit>
#include <cstdint>
#include <string>

namespace qppjs {

using ObjectPtr = RcPtr<RcObject>;

// Legacy compatibility — kept for code that references these names.
// To be removed in a future cleanup pass.
struct Undefined final {};
struct Null final {};

enum class ValueKind { Undefined, Null, Bool, Number, String, Object };

// NaN-boxing Value (8 bytes).
//
// Layout (uint64_t raw_):
//   double:    std::bit_cast<uint64_t>(d), NaN canonicalized to kCanonicalNaN
//   non-double: high 16 bits = tag (signed int32 via arithmetic shift), low 48 bits = payload
//     tag = -1 (0xFFFF): Object,    payload = RcObject* (low 48 bits)
//     tag = -2 (0xFFFE): String,    payload = JSString* (low 48 bits)
//     tag = -3 (0xFFFD): Bool,      payload = 0 (false) or 1 (true)
//     tag = -4 (0xFFFC): Null,      payload = 0
//     tag = -5 (0xFFFB): Undefined, payload = 0
//
// A double is detected when tag is NOT in [-5, -1].
class Value {
public:
    // Default constructs as undefined.
    Value() noexcept;

    Value(const Value& other);
    Value(Value&& other) noexcept;
    Value& operator=(const Value& other);
    Value& operator=(Value&& other) noexcept;
    ~Value();

    // Factory functions
    static Value undefined();
    static Value null();
    static Value boolean(bool value);
    static Value number(double value);
    static Value string(std::string value);
    static Value object(ObjectPtr value);

    // Type predicates
    [[nodiscard]] ValueKind kind() const;
    [[nodiscard]] bool is_undefined() const;
    [[nodiscard]] bool is_null() const;
    [[nodiscard]] bool is_bool() const;
    [[nodiscard]] bool is_number() const;
    [[nodiscard]] bool is_string() const;
    [[nodiscard]] bool is_object() const;

    // Accessors
    [[nodiscard]] bool as_bool() const;
    [[nodiscard]] double as_number() const;
    [[nodiscard]] const std::string& as_string() const;
    // Returns a new RcPtr (increments ref count).
    [[nodiscard]] ObjectPtr as_object() const;
    // Returns raw pointer without incrementing ref count. Valid as long as Value is alive.
    [[nodiscard]] RcObject* as_object_raw() const;

private:
    static constexpr int32_t kTagObject    = -1;
    static constexpr int32_t kTagString    = -2;
    static constexpr int32_t kTagBool      = -3;
    static constexpr int32_t kTagNull      = -4;
    static constexpr int32_t kTagUndefined = -5;
    static constexpr uint64_t kPayloadMask = (1ULL << 48) - 1;
    // Canonical quiet NaN used to represent all NaN doubles.
    static constexpr uint64_t kCanonicalNaN = 0x7FF8'0000'0000'0000ULL;

    int32_t tag() const noexcept {
        return static_cast<int32_t>(static_cast<int64_t>(raw_) >> 48);
    }
    uint64_t payload() const noexcept { return raw_ & kPayloadMask; }

    void add_ref_if_needed() noexcept;
    void release_if_needed() noexcept;

    uint64_t raw_;
};

static_assert(sizeof(Value) == 8, "Value must be 8 bytes (NaN-boxing)");

}  // namespace qppjs
