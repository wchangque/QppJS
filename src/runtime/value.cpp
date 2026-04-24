#include "qppjs/runtime/value.h"

#include "qppjs/runtime/rc_object.h"

#include <bit>
#include <cassert>
#include <cstdint>
#include <cstring>

namespace qppjs {

// ============================================================
// Internal helpers
// ============================================================

static uint64_t encode_tag_payload(int32_t tag, uint64_t payload) {
    return (static_cast<uint64_t>(static_cast<uint16_t>(static_cast<int16_t>(tag))) << 48) | payload;
}

// ============================================================
// Ref-count management
// ============================================================

void Value::add_ref_if_needed() noexcept {
    int32_t t = tag();
    if (t == kTagObject) {
        auto* p = reinterpret_cast<RcObject*>(static_cast<uintptr_t>(payload()));
        if (p) {
            p->add_ref();
        }
    } else if (t == kTagString) {
        auto* p = reinterpret_cast<JSString*>(static_cast<uintptr_t>(payload()));
        if (p) {
            p->add_ref();
        }
    }
}

void Value::release_if_needed() noexcept {
    int32_t t = tag();
    if (t == kTagObject) {
        auto* p = reinterpret_cast<RcObject*>(static_cast<uintptr_t>(payload()));
        if (p) {
            p->release();
        }
    } else if (t == kTagString) {
        auto* p = reinterpret_cast<JSString*>(static_cast<uintptr_t>(payload()));
        if (p) {
            p->release();
        }
    }
}

// ============================================================
// Constructors / assignment / destructor
// ============================================================

Value::Value() noexcept {
    raw_ = encode_tag_payload(kTagUndefined, 0);
}

Value::Value(const Value& other) : raw_(other.raw_) {
    add_ref_if_needed();
}

Value::Value(Value&& other) noexcept : raw_(other.raw_) {
    // Steal the ref; leave other as undefined so its destructor does nothing harmful.
    other.raw_ = encode_tag_payload(kTagUndefined, 0);
}

Value& Value::operator=(const Value& other) {
    if (this != &other) {
        release_if_needed();
        raw_ = other.raw_;
        add_ref_if_needed();
    }
    return *this;
}

Value& Value::operator=(Value&& other) noexcept {
    if (this != &other) {
        release_if_needed();
        raw_ = other.raw_;
        other.raw_ = encode_tag_payload(kTagUndefined, 0);
    }
    return *this;
}

Value::~Value() {
    release_if_needed();
}

// ============================================================
// Factory functions
// ============================================================

Value Value::undefined() {
    Value v;
    v.raw_ = encode_tag_payload(kTagUndefined, 0);
    return v;
}

Value Value::null() {
    Value v;
    v.raw_ = encode_tag_payload(kTagNull, 0);
    return v;
}

Value Value::boolean(bool value) {
    Value v;
    v.raw_ = encode_tag_payload(kTagBool, value ? 1ULL : 0ULL);
    return v;
}

Value Value::number(double d) {
    Value v;
    uint64_t bits = std::bit_cast<uint64_t>(d);
    // Detect NaN: exponent all-ones and mantissa non-zero.
    if ((bits & 0x7FF0'0000'0000'0000ULL) == 0x7FF0'0000'0000'0000ULL &&
        (bits & 0x000F'FFFF'FFFF'FFFFULL) != 0) {
        v.raw_ = kCanonicalNaN;
    } else {
        v.raw_ = bits;
    }
    return v;
}

Value Value::string(std::string s) {
    Value v;
    auto* js_str = new JSString(std::move(s));
    js_str->add_ref();
    auto ptr = reinterpret_cast<uintptr_t>(js_str);
    v.raw_ = encode_tag_payload(kTagString, static_cast<uint64_t>(ptr) & kPayloadMask);
    return v;
}

Value Value::object(ObjectPtr value) {
    Value v;
    RcObject* raw = value.get();
    if (raw) {
        raw->add_ref();
    }
    auto ptr = reinterpret_cast<uintptr_t>(raw);
    v.raw_ = encode_tag_payload(kTagObject, static_cast<uint64_t>(ptr) & kPayloadMask);
    // Release the incoming RcPtr's ref (we took our own above).
    // The RcPtr destructor will call release() on raw when value goes out of scope.
    // But we already called add_ref, so net effect is +1 for this Value's ref.
    // Actually: value holds +1 ref, we called add_ref() for +1 more, then value's destructor
    // releases -1 → net +1 ref owned by this Value. Correct.
    return v;
}

// ============================================================
// Type predicates
// ============================================================

ValueKind Value::kind() const {
    int32_t t = tag();
    switch (t) {
    case kTagUndefined: return ValueKind::Undefined;
    case kTagNull:      return ValueKind::Null;
    case kTagBool:      return ValueKind::Bool;
    case kTagString:    return ValueKind::String;
    case kTagObject:    return ValueKind::Object;
    default:            return ValueKind::Number;
    }
}

bool Value::is_undefined() const { return tag() == kTagUndefined; }
bool Value::is_null()      const { return tag() == kTagNull; }
bool Value::is_bool()      const { return tag() == kTagBool; }
bool Value::is_string()    const { return tag() == kTagString; }
bool Value::is_object()    const { return tag() == kTagObject; }

bool Value::is_number() const {
    int32_t t = tag();
    // A double has tag NOT in [kTagUndefined, kTagObject] = [-5, -1].
    return t < kTagUndefined || t > kTagObject;
}

// ============================================================
// Accessors
// ============================================================

bool Value::as_bool() const {
    assert(is_bool());
    return payload() != 0;
}

double Value::as_number() const {
    assert(is_number());
    return std::bit_cast<double>(raw_);
}

const std::string& Value::as_string() const {
    assert(is_string());
    auto* p = reinterpret_cast<JSString*>(static_cast<uintptr_t>(payload()));
    assert(p != nullptr);
    return p->str;
}

ObjectPtr Value::as_object() const {
    assert(is_object());
    auto* p = reinterpret_cast<RcObject*>(static_cast<uintptr_t>(payload()));
    return ObjectPtr(p);  // RcPtr(T*) calls add_ref
}

RcObject* Value::as_object_raw() const {
    assert(is_object());
    return reinterpret_cast<RcObject*>(static_cast<uintptr_t>(payload()));
}

}  // namespace qppjs
