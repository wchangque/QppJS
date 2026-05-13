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

// ============================================================
// JSString SSO layout
// ============================================================

TEST(JSStringSSOTest, SizeIs48Bytes) {
    static_assert(sizeof(JSString) == 48, "JSString must be 48 bytes with SSO layout");
    EXPECT_EQ(sizeof(JSString), 48u);
}

TEST(JSStringSSOTest, InlineShortString) {
    JSString s("hello");
    EXPECT_EQ(s.sv(), "hello");
}

TEST(JSStringSSOTest, InlineMaxCapacity) {
    // 32-byte inline capacity: a 32-char string should be inline
    std::string str32(32, 'x');
    JSString s(str32);
    EXPECT_EQ(s.sv(), str32);
}

TEST(JSStringSSOTest, HeapLongString) {
    // 33-byte string exceeds inline capacity → heap allocation
    std::string str33(33, 'y');
    JSString s(str33);
    EXPECT_EQ(s.sv(), str33);
}

TEST(JSStringSSOTest, EmptyString) {
    JSString s("");
    EXPECT_EQ(s.sv(), "");
    EXPECT_EQ(s.sv().size(), 0u);
}

TEST(JSStringSSOTest, SvReturnsStringView) {
    JSString s("test_sv");
    std::string_view sv = s.sv();
    EXPECT_EQ(sv, "test_sv");
    EXPECT_EQ(sv.size(), 7u);
}

TEST(JSStringSSOTest, RefCountStartsZero) {
    JSString s("rc");
    EXPECT_EQ(s.ref_count, 0);
}

TEST(JSStringSSOTest, CpCountStartsMinusOne) {
    JSString s("cached");
    EXPECT_EQ(s.cp_count_, -1);
}

TEST(JSStringSSOTest, ValueSvAccessor) {
    Value v = Value::string("hello_sv");
    EXPECT_EQ(v.sv(), "hello_sv");
}

TEST(JSStringSSOTest, ValueJsStringRaw) {
    Value v = Value::string("raw_ptr");
    JSString* raw = v.js_string_raw();
    EXPECT_NE(raw, nullptr);
    EXPECT_EQ(raw->sv(), "raw_ptr");
}

TEST(JSStringSSOTest, AsStringReturnsStdString) {
    Value v = Value::string("as_str");
    std::string s = v.as_string();
    EXPECT_EQ(s, "as_str");
}

TEST(JSStringSSOTest, LongStringHeapRoundTrip) {
    std::string long_str(100, 'z');
    Value v = Value::string(long_str);
    EXPECT_EQ(v.sv(), long_str);
    EXPECT_EQ(v.as_string(), long_str);
}

// ============================================================
// SSO-P1: is_inline() 标志正确性
// ============================================================

// 长度 <= 32 字节时 is_inline() 返回 true
TEST(JSStringSSOTest, IsInlineTrueForShortString) {
    JSString s("short");
    EXPECT_TRUE(s.is_inline());
}

// 恰好 32 字节时 is_inline() 返回 true（边界值）
TEST(JSStringSSOTest, IsInlineTrueAtExactCapacity) {
    std::string str32(32, 'a');
    JSString s(str32);
    EXPECT_TRUE(s.is_inline());
}

// 33 字节时 is_inline() 返回 false（超出边界一字节）
TEST(JSStringSSOTest, IsInlineFalseJustOverCapacity) {
    std::string str33(33, 'b');
    JSString s(str33);
    EXPECT_FALSE(s.is_inline());
}

// 空字符串 is_inline() 返回 true
TEST(JSStringSSOTest, IsInlineTrueForEmpty) {
    JSString s("");
    EXPECT_TRUE(s.is_inline());
}

// ============================================================
// SSO-P2: ref_count 生命周期正确性
// ============================================================

// Value::string() 工厂调用后 ref_count 应为 1
TEST(JSStringSSOTest, RefCountIsOneAfterValueFactory) {
    Value v = Value::string("factory");
    JSString* raw = v.js_string_raw();
    EXPECT_EQ(raw->ref_count, 1);
}

// 拷贝 Value 后 ref_count 增加到 2
TEST(JSStringSSOTest, RefCountIncreasesOnValueCopy) {
    Value a = Value::string("copy_me");
    Value b = a;
    JSString* raw = a.js_string_raw();
    EXPECT_EQ(raw->ref_count, 2);
}

// 拷贝 Value 析构后 ref_count 回到 1
TEST(JSStringSSOTest, RefCountDecreasesOnValueDestroy) {
    Value a = Value::string("copy_destroy");
    JSString* raw = a.js_string_raw();
    {
        Value b = a;
        EXPECT_EQ(raw->ref_count, 2);
    }
    // b 析构后
    EXPECT_EQ(raw->ref_count, 1);
}

// Move 后原 Value 变为 undefined，ref_count 不变（所有权转移）
TEST(JSStringSSOTest, RefCountUnchangedAfterMove) {
    Value a = Value::string("move_rc");
    JSString* raw = a.js_string_raw();
    Value b = std::move(a);
    EXPECT_TRUE(a.is_undefined());
    EXPECT_EQ(raw->ref_count, 1);
}

// 三个 Value 共享同一 JSString，ref_count == 3
TEST(JSStringSSOTest, RefCountThreeSharedValues) {
    Value a = Value::string("shared3");
    Value b = a;
    Value c = a;
    JSString* raw = a.js_string_raw();
    EXPECT_EQ(raw->ref_count, 3);
}

// ============================================================
// SSO-P3: cp_count_ 缓存在多次调用间保持
// ============================================================

// 两个不同 Value 指向同一 JSString，cp_count_ 缓存共享
TEST(JSStringSSOTest, CpCountCacheSharedAcrossCopies) {
    // 通过 js_string_raw() 直接操控 cp_count_
    Value a = Value::string("abc");
    JSString* raw = a.js_string_raw();
    EXPECT_EQ(raw->cp_count_, -1);  // 初始未缓存

    // 手动写入缓存值
    raw->cp_count_ = 3;

    // 拷贝 Value 后，指向同一 JSString，缓存可见
    Value b = a;
    EXPECT_EQ(b.js_string_raw()->cp_count_, 3);
}

// cp_count_ 一旦写入不会被 sv() 调用重置
TEST(JSStringSSOTest, CpCountNotResetBySvCall) {
    Value v = Value::string("hello");
    JSString* raw = v.js_string_raw();
    raw->cp_count_ = 5;
    // 调用 sv() 不应改变 cp_count_
    (void)v.sv();
    EXPECT_EQ(raw->cp_count_, 5);
}

// cp_count_ 一旦写入不会被 as_string() 调用重置
TEST(JSStringSSOTest, CpCountNotResetByAsStringCall) {
    Value v = Value::string("world");
    JSString* raw = v.js_string_raw();
    raw->cp_count_ = 5;
    (void)v.as_string();
    EXPECT_EQ(raw->cp_count_, 5);
}

// ============================================================
// SSO-P4: sv() 返回的 string_view 指向 JSString 内部数据（不悬空）
// ============================================================

// inline 字符串：sv() 指向 inline_buf_，Value 存活时不悬空
TEST(JSStringSSOTest, SvPointsIntoInlineBuffer) {
    Value v = Value::string("inline_ptr");
    std::string_view sv = v.sv();
    JSString* raw = v.js_string_raw();
    // sv 的 data() 指针应落在 JSString 对象内部
    const char* obj_begin = reinterpret_cast<const char*>(raw);
    const char* obj_end   = obj_begin + sizeof(JSString);
    EXPECT_GE(sv.data(), obj_begin);
    EXPECT_LT(sv.data(), obj_end);
}

// heap 字符串：sv() 指向堆分配的缓冲区（不在 JSString 对象内部）
TEST(JSStringSSOTest, SvPointsToHeapBufferForLongString) {
    std::string long_str(33, 'h');
    Value v = Value::string(long_str);
    std::string_view sv = v.sv();
    JSString* raw = v.js_string_raw();
    const char* obj_begin = reinterpret_cast<const char*>(raw);
    const char* obj_end   = obj_begin + sizeof(JSString);
    // 堆字符串的 data() 不在对象内部
    EXPECT_TRUE(sv.data() < obj_begin || sv.data() >= obj_end);
}

// ============================================================
// SSO-P5: 字符串内容相等性（非指针比较）
// ============================================================

// 两个独立 Value::string("same") 内容相等，但 JSString 指针不同
TEST(JSStringSSOTest, EqualContentDifferentPointers) {
    Value a = Value::string("same_content");
    Value b = Value::string("same_content");
    EXPECT_NE(a.js_string_raw(), b.js_string_raw());
    EXPECT_EQ(a.as_string(), b.as_string());
    EXPECT_EQ(a.sv(), b.sv());
}

// 不同内容的字符串 as_string() 不相等
TEST(JSStringSSOTest, DifferentContentNotEqual) {
    Value a = Value::string("aaa");
    Value b = Value::string("bbb");
    EXPECT_NE(a.as_string(), b.as_string());
}

// ============================================================
// SSO-P6: 长字符串析构无泄漏（LSan 验证）
// ============================================================

// 创建多个 heap 字符串并析构，LSan 不应报告泄漏
TEST(JSStringSSOTest, HeapStringDestructorFreesMemory) {
    for (int i = 0; i < 10; ++i) {
        std::string s(100 + i, static_cast<char>('a' + i));
        Value v = Value::string(s);
        EXPECT_EQ(v.sv(), s);
    }
    // 所有 Value 析构后 JSString 的 heap_ptr_ 应被 free
    SUCCEED();
}

// 拷贝链：a→b→c，逐一析构，最后一个析构时 free heap
TEST(JSStringSSOTest, HeapStringRefCountChainDestruct) {
    std::string long_str(50, 'q');
    Value a = Value::string(long_str);
    JSString* raw = a.js_string_raw();
    EXPECT_FALSE(raw->is_inline());
    {
        Value b = a;
        {
            Value c = b;
            EXPECT_EQ(raw->ref_count, 3);
        }
        EXPECT_EQ(raw->ref_count, 2);
    }
    EXPECT_EQ(raw->ref_count, 1);
    // a 析构后 raw 已 delete，不能再访问，但 LSan 不应报泄漏
}

// ============================================================
// SSO-P7: Value 赋值语义正确性
// ============================================================

// 将 string Value 赋值给已持有另一 string Value 的变量：旧字符串 ref_count 减少
TEST(JSStringSSOTest, AssignStringToStringDecrefsOld) {
    Value a = Value::string("old_str");
    JSString* old_raw = a.js_string_raw();
    Value b = Value::string("new_str");
    JSString* new_raw = b.js_string_raw();
    EXPECT_EQ(old_raw->ref_count, 1);
    EXPECT_EQ(new_raw->ref_count, 1);

    a = b;
    // a 现在持有 new_str，old_str ref_count 应降为 0（已 delete，但 ref_count 字段已不可信）
    // 只验证 a 的内容正确，以及 new_raw ref_count == 2
    EXPECT_EQ(a.as_string(), "new_str");
    EXPECT_EQ(new_raw->ref_count, 2);
}

// Move 赋值：被移入的 Value 获得所有权，原 Value 变 undefined
TEST(JSStringSSOTest, MoveAssignTransfersOwnership) {
    Value a = Value::string("move_assign");
    JSString* raw = a.js_string_raw();
    Value b = Value::undefined();
    b = std::move(a);
    EXPECT_TRUE(a.is_undefined());
    EXPECT_EQ(b.js_string_raw(), raw);
    EXPECT_EQ(raw->ref_count, 1);
}

// ============================================================
// SSO-P8: 非 BMP 字符的 JSString 内部存储（UTF-8 字节正确）
// ============================================================

// U+1F600 (😀) 的 UTF-8 编码为 4 字节，size_ 应为 4，is_inline() 为 true
TEST(JSStringSSOTest, SmpCharStoredAsUtf8Bytes) {
    // UTF-8 for U+1F600: \xF0\x9F\x98\x80
    const char smp[] = "\xF0\x9F\x98\x80";
    JSString s(std::string_view(smp, 4));
    EXPECT_EQ(s.sv().size(), 4u);   // 4 UTF-8 bytes
    EXPECT_TRUE(s.is_inline());     // 4 bytes fits inline
    EXPECT_EQ(s.cp_count_, -1);     // not yet computed
}

// 多个 SMP 字符：size_ 为 UTF-8 字节数，is_inline() 根据字节数判断
TEST(JSStringSSOTest, MultipleSmpCharsInlineIfFit) {
    // 7 SMP chars × 4 bytes = 28 bytes → inline
    std::string seven_smp;
    for (int i = 0; i < 7; ++i) seven_smp += "\xF0\x9F\x98\x80";
    JSString s(seven_smp);
    EXPECT_EQ(s.sv().size(), 28u);
    EXPECT_TRUE(s.is_inline());
}

// 9 SMP chars × 4 bytes = 36 bytes → heap
TEST(JSStringSSOTest, MultipleSmpCharsHeapIfExceedCapacity) {
    std::string nine_smp;
    for (int i = 0; i < 9; ++i) nine_smp += "\xF0\x9F\x98\x80";
    JSString s(nine_smp);
    EXPECT_EQ(s.sv().size(), 36u);
    EXPECT_FALSE(s.is_inline());
}

// ============================================================
// SSO-P9: 边界 size_ 字段正确性
// ============================================================

TEST(JSStringSSOTest, SizeFieldCorrectForEmpty) {
    JSString s("");
    EXPECT_EQ(s.sv().size(), 0u);
}

TEST(JSStringSSOTest, SizeFieldCorrectForInline) {
    JSString s("hello");
    EXPECT_EQ(s.sv().size(), 5u);
}

TEST(JSStringSSOTest, SizeFieldCorrectForHeap) {
    std::string str(100, 'k');
    JSString s(str);
    EXPECT_EQ(s.sv().size(), 100u);
}

// ============================================================
// SSO-P10: null 字节（含 \0 的字符串）存储正确
// ============================================================

// JSString 按 size_ 存储，不依赖 null terminator，可存储含 \0 的数据
TEST(JSStringSSOTest, NullByteInInlineString) {
    std::string_view sv_with_null("ab\0cd", 5);
    JSString s(sv_with_null);
    EXPECT_EQ(s.sv().size(), 5u);
    EXPECT_EQ(s.sv()[2], '\0');
    EXPECT_TRUE(s.is_inline());
}

}  // namespace qppjs
