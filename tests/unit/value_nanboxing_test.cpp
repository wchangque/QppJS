#include "qppjs/runtime/rc_object.h"
#include "qppjs/runtime/value.h"
#include "qppjs/runtime/js_object.h"

#include <bit>
#include <cmath>
#include <limits>

#include <gtest/gtest.h>

namespace qppjs {

// ============================================================
// sizeof(Value) == 8
// ============================================================

TEST(NanBoxingTest, ValueSizeIsEightBytes) {
    static_assert(sizeof(Value) == 8, "Value must be 8 bytes");
    EXPECT_EQ(sizeof(Value), 8u);
}

// ============================================================
// Double encoding: basic values
// ============================================================

TEST(NanBoxingTest, ZeroIsNumber) {
    Value v = Value::number(0.0);
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), 0.0);
}

TEST(NanBoxingTest, NegativeZeroIsNumber) {
    Value v = Value::number(-0.0);
    EXPECT_TRUE(v.is_number());
    // -0.0 and 0.0 are equal in IEEE 754
    EXPECT_EQ(v.as_number(), 0.0);
}

TEST(NanBoxingTest, PositiveInfinityIsNumber) {
    double inf = std::numeric_limits<double>::infinity();
    Value v = Value::number(inf);
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isinf(v.as_number()));
    EXPECT_GT(v.as_number(), 0.0);
}

TEST(NanBoxingTest, NegativeInfinityIsNumber) {
    double neg_inf = -std::numeric_limits<double>::infinity();
    Value v = Value::number(neg_inf);
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isinf(v.as_number()));
    EXPECT_LT(v.as_number(), 0.0);
}

TEST(NanBoxingTest, NaNIsNormalizedToCanonicalNaN) {
    // Any NaN input is canonicalized to quiet NaN.
    double nan_val = std::numeric_limits<double>::quiet_NaN();
    Value v = Value::number(nan_val);
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isnan(v.as_number()));
    // Verify canonical NaN bits
    uint64_t bits = std::bit_cast<uint64_t>(v.as_number());
    EXPECT_EQ(bits, 0x7FF8'0000'0000'0000ULL);
}

TEST(NanBoxingTest, SignalingNaNNormalized) {
    // Signaling NaN (non-zero mantissa, quiet bit=0) should also normalize.
    uint64_t snan_bits = 0x7FF0'0000'0000'0001ULL;  // signaling NaN
    double snan = std::bit_cast<double>(snan_bits);
    Value v = Value::number(snan);
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isnan(v.as_number()));
    uint64_t out_bits = std::bit_cast<uint64_t>(v.as_number());
    EXPECT_EQ(out_bits, 0x7FF8'0000'0000'0000ULL);
}

TEST(NanBoxingTest, ArbitraryDoubleRoundTrips) {
    double d = 3.141592653589793;
    Value v = Value::number(d);
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), d);
}

TEST(NanBoxingTest, LargeDoubleRoundTrips) {
    double d = 1.7976931348623157e+308;
    Value v = Value::number(d);
    EXPECT_TRUE(v.is_number());
    EXPECT_EQ(v.as_number(), d);
}

// ============================================================
// Tag encoding: non-double types
// ============================================================

TEST(NanBoxingTest, UndefinedKind) {
    Value v = Value::undefined();
    EXPECT_TRUE(v.is_undefined());
    EXPECT_FALSE(v.is_null());
    EXPECT_FALSE(v.is_bool());
    EXPECT_FALSE(v.is_number());
    EXPECT_FALSE(v.is_string());
    EXPECT_FALSE(v.is_object());
    EXPECT_EQ(v.kind(), ValueKind::Undefined);
}

TEST(NanBoxingTest, NullKind) {
    Value v = Value::null();
    EXPECT_TRUE(v.is_null());
    EXPECT_FALSE(v.is_undefined());
    EXPECT_EQ(v.kind(), ValueKind::Null);
}

TEST(NanBoxingTest, BoolTrueKind) {
    Value v = Value::boolean(true);
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
    EXPECT_EQ(v.kind(), ValueKind::Bool);
}

TEST(NanBoxingTest, BoolFalseKind) {
    Value v = Value::boolean(false);
    EXPECT_TRUE(v.is_bool());
    EXPECT_FALSE(v.as_bool());
}

TEST(NanBoxingTest, StringKind) {
    Value v = Value::string("hello");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hello");
    EXPECT_EQ(v.kind(), ValueKind::String);
}

TEST(NanBoxingTest, EmptyStringKind) {
    Value v = Value::string("");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "");
}

TEST(NanBoxingTest, ObjectKind) {
    auto obj = RcPtr<JSObject>::make();
    Value v = Value::object(ObjectPtr(obj));
    EXPECT_TRUE(v.is_object());
    EXPECT_EQ(v.kind(), ValueKind::Object);
    EXPECT_NE(v.as_object_raw(), nullptr);
}

// ============================================================
// Tag values do not conflict with valid doubles
// ============================================================

TEST(NanBoxingTest, TagsDoNotConflictWithDoubles) {
    // Tags -1 to -5 occupy high 16 bits 0xFFFF to 0xFFFB.
    // These map to NaN-space (exponent all-ones, sign=1).
    // A number stored in the Value must not be misidentified as a tag.
    // Verify that a double with high 16 bits in 0xFFFB..0xFFFF range
    // is a NaN and gets canonicalized, so it cannot conflict.

    // 0xFFFF_xxxx doubles are all NaN (sign=1, exp=all-ones, mantissa non-zero).
    uint64_t candidate = 0xFFFF'0000'0000'0001ULL;
    double d = std::bit_cast<double>(candidate);
    EXPECT_TRUE(std::isnan(d));
    Value v = Value::number(d);
    // After canonicalization it's still a number (not misidentified as Object tag).
    EXPECT_TRUE(v.is_number());
    EXPECT_TRUE(std::isnan(v.as_number()));
}

// ============================================================
// Default constructor
// ============================================================

TEST(NanBoxingTest, DefaultConstructIsUndefined) {
    Value v;
    EXPECT_TRUE(v.is_undefined());
}

// ============================================================
// Copy semantics
// ============================================================

TEST(NanBoxingTest, CopyConstructorNumber) {
    Value a = Value::number(42.0);
    Value b = a;
    EXPECT_TRUE(b.is_number());
    EXPECT_EQ(b.as_number(), 42.0);
}

TEST(NanBoxingTest, CopyConstructorString) {
    Value a = Value::string("world");
    Value b = a;
    EXPECT_EQ(b.as_string(), "world");
    EXPECT_EQ(a.as_string(), "world");  // original still valid
}

TEST(NanBoxingTest, CopyAssignmentString) {
    Value a = Value::string("foo");
    Value b = Value::undefined();
    b = a;
    EXPECT_EQ(b.as_string(), "foo");
    EXPECT_EQ(a.as_string(), "foo");
}

// ============================================================
// Move semantics
// ============================================================

TEST(NanBoxingTest, MoveConstructorString) {
    Value a = Value::string("move_me");
    Value b = std::move(a);
    EXPECT_EQ(b.as_string(), "move_me");
    EXPECT_TRUE(a.is_undefined());  // moved-from is undefined
}

TEST(NanBoxingTest, MoveAssignmentString) {
    Value a = Value::string("assign_me");
    Value b = Value::undefined();
    b = std::move(a);
    EXPECT_EQ(b.as_string(), "assign_me");
    EXPECT_TRUE(a.is_undefined());
}

TEST(NanBoxingTest, MoveConstructorObject) {
    auto obj = RcPtr<JSObject>::make();
    RcObject* raw = obj.get();
    Value a = Value::object(ObjectPtr(obj));
    Value b = std::move(a);
    EXPECT_TRUE(b.is_object());
    EXPECT_EQ(b.as_object_raw(), raw);
    EXPECT_TRUE(a.is_undefined());
}

// ============================================================
// RcPtr reference counting
// ============================================================

TEST(RcPtrTest, MakeCreatesWithRefCountOne) {
    auto p = RcPtr<JSObject>::make();
    EXPECT_NE(p.get(), nullptr);
    // We can't directly read ref_count_, but we can verify the object is alive.
    EXPECT_TRUE(static_cast<bool>(p));
}

TEST(RcPtrTest, CopyIncreasesRefCount) {
    auto p1 = RcPtr<JSObject>::make();
    JSObject* raw = p1.get();
    {
        auto p2 = p1;  // copy
        EXPECT_EQ(p2.get(), raw);
        // Both p1 and p2 hold a ref; object must still be alive.
        EXPECT_TRUE(static_cast<bool>(p1));
        EXPECT_TRUE(static_cast<bool>(p2));
    }
    // p2 destroyed; p1 still holds the object.
    EXPECT_TRUE(static_cast<bool>(p1));
    EXPECT_EQ(p1.get(), raw);
}

TEST(RcPtrTest, MoveDoesNotLeakOrDoubleRelease) {
    auto p1 = RcPtr<JSObject>::make();
    JSObject* raw = p1.get();
    auto p2 = std::move(p1);
    EXPECT_EQ(p2.get(), raw);
    EXPECT_EQ(p1.get(), nullptr);  // moved-from is null
}

TEST(RcPtrTest, DestructorReleasesRef) {
    // Verify that after all RcPtrs are destroyed, the object is deleted.
    // We can't directly observe deletion, but ASAN will catch double-free or leaks.
    {
        auto p = RcPtr<JSObject>::make();
        (void)p;
    }
    // If we reach here without ASAN error, the destructor ran correctly.
    SUCCEED();
}

TEST(RcPtrTest, UpcastFromDerivedToBase) {
    auto derived = RcPtr<JSObject>::make();
    ObjectPtr base = derived;  // implicit upcast via template constructor
    EXPECT_EQ(base.get(), derived.get());
}

// ============================================================
// String ref-count via Value copy/move/destroy
// ============================================================

TEST(NanBoxingTest, StringRefCountCopyDestroy) {
    // Create two copies of a string Value; both should access the same string.
    Value a = Value::string("shared");
    Value b = a;
    EXPECT_EQ(a.as_string(), "shared");
    EXPECT_EQ(b.as_string(), "shared");
    // Destroy b; a should still be valid.
    b = Value::undefined();
    EXPECT_EQ(a.as_string(), "shared");
}

TEST(NanBoxingTest, ObjectRefCountCopyDestroy) {
    auto obj = RcPtr<JSObject>::make();
    obj->set_property("x", Value::number(1.0));
    RcObject* raw = obj.get();

    Value a = Value::object(ObjectPtr(obj));
    Value b = a;

    EXPECT_EQ(a.as_object_raw(), raw);
    EXPECT_EQ(b.as_object_raw(), raw);

    // Destroy b; a and obj still hold refs.
    b = Value::undefined();
    EXPECT_EQ(a.as_object_raw(), raw);
    EXPECT_EQ(static_cast<JSObject*>(raw)->get_property("x").as_number(), 1.0);
}

// ============================================================
// Self-assignment safety
// ============================================================

TEST(NanBoxingTest, SelfAssignmentString) {
    Value a = Value::string("self");
    // Use volatile pointer to defeat the self-assignment warning.
    Value* volatile pa = &a;
    *pa = a;
    EXPECT_EQ(a.as_string(), "self");
}

TEST(NanBoxingTest, SelfAssignmentObject) {
    auto obj = RcPtr<JSObject>::make();
    Value a = Value::object(ObjectPtr(obj));
    Value* volatile pa = &a;
    *pa = a;
    EXPECT_TRUE(a.is_object());
}

}  // namespace qppjs
