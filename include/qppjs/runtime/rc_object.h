#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace qppjs {

class GcHeap;

enum class ObjectKind { kOrdinary, kFunction, kArray, kEnvironment, kModule, kPromise };

// Base class for all heap-allocated JS objects with non-atomic reference counting.
// Subclasses must call RcObject(ObjectKind) in their constructor.
//
// Phase 9: GC support added. When an object is registered with a GcHeap, the heap
// manages its lifetime via mark-sweep. set_gc_sentinel() is called before GC-driven
// deletion to prevent double-free in the RC release() path.
class RcObject {
public:
    explicit RcObject(ObjectKind kind) : ref_count_(0), object_kind_(static_cast<int32_t>(kind)) {}

    virtual ~RcObject() {
        if (gc_heap_) {
            // RC归零路径：从 GcHeap 摘除，避免 GcHeap 持有悬空指针
            Unregister();
        }
    }

    // Pure virtual: subclasses must implement to trace all reachable RcObject* children.
    virtual void TraceRefs(GcHeap& heap) = 0;

    // Called by GcHeap::Sweep() before deleting an unreachable object.
    // Subclasses must clear all RcPtr members (without calling release) to prevent
    // use-after-free when sibling objects in the same cycle are also being swept.
    virtual void ClearRefs() = 0;

    void add_ref() {
        if (ref_count_ == kGcSentinel) return;
        ++ref_count_;
    }

    void release() {
        if (ref_count_ == kGcSentinel) return;
        if (--ref_count_ == 0) {
            delete this;
        }
    }

    // Called by GcHeap::Sweep() before deleting an unreachable object.
    // Prevents RC release() from triggering a second delete.
    void set_gc_sentinel() { ref_count_ = kGcSentinel; }

    ObjectKind object_kind() const { return static_cast<ObjectKind>(object_kind_); }

    bool gc_mark_ = false;
    GcHeap* gc_heap_ = nullptr;

    static constexpr int32_t kGcSentinel = -0x40000000;

private:
    void Unregister();

    int32_t ref_count_;
    int32_t object_kind_;
};

// Non-owning typed smart pointer with non-atomic reference counting.
template <typename T>
class RcPtr {
public:
    RcPtr() : ptr_(nullptr) {}

    explicit RcPtr(T* ptr) : ptr_(ptr) {
        if (ptr_) {
            ptr_->add_ref();
        }
    }

    RcPtr(const RcPtr& other) : ptr_(other.ptr_) {
        if (ptr_) {
            ptr_->add_ref();
        }
    }

    RcPtr(RcPtr&& other) noexcept : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }

    // Implicit upcast: RcPtr<Derived> → RcPtr<Base>
    template <typename U, typename = std::enable_if_t<std::is_base_of_v<T, U>>>
    RcPtr(const RcPtr<U>& other) : ptr_(other.get()) {  // NOLINT(google-explicit-constructor)
        if (ptr_) {
            ptr_->add_ref();
        }
    }

    ~RcPtr() {
        if (ptr_) {
            ptr_->release();
        }
    }

    RcPtr& operator=(const RcPtr& other) {
        T* new_ptr = other.ptr_;
        if (new_ptr) new_ptr->add_ref();
        if (ptr_) ptr_->release();
        ptr_ = new_ptr;
        return *this;
    }

    RcPtr& operator=(RcPtr&& other) noexcept {
        if (this != &other) {
            if (ptr_) {
                ptr_->release();
            }
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    T* get() const { return ptr_; }

    T* operator->() const { return ptr_; }

    T& operator*() const { return *ptr_; }

    explicit operator bool() const { return ptr_ != nullptr; }

    bool operator==(const RcPtr& other) const { return ptr_ == other.ptr_; }
    bool operator!=(const RcPtr& other) const { return ptr_ != other.ptr_; }
    bool operator==(std::nullptr_t) const { return ptr_ == nullptr; }
    bool operator!=(std::nullptr_t) const { return ptr_ != nullptr; }

    template <typename... Args>
    static RcPtr make(Args&&... args) {
        T* raw = new T(std::forward<Args>(args)...);
        RcPtr p;
        p.ptr_ = raw;
        raw->add_ref();
        return p;
    }

    // Unsafe: construct from raw pointer without adding a ref (for internal use).
    // Caller must ensure the object already has a ref for this RcPtr.
    static RcPtr from_raw_no_addref(T* raw) {
        RcPtr p;
        p.ptr_ = raw;
        return p;
    }

    // Called by GcHeap::Sweep() via ClearRefs() to nullify this pointer without
    // decrementing the ref count. Only safe when the pointed-to object is being
    // simultaneously swept (its ref count is already set to kGcSentinel).
    void reset_no_release() noexcept { ptr_ = nullptr; }

private:
    T* ptr_;
};

// JSString: heap-allocated string with non-atomic reference counting and SSO.
// Strings up to kInlineCapacity bytes are stored inline (no second heap allocation).
// Longer strings use a separately malloc'd buffer.
struct JSString {
    int32_t ref_count = 0;
    // Cached UTF-16 code unit count. -1 means not yet computed.
    int32_t cp_count_ = -1;
    uint32_t size_ = 0;
    uint32_t flags_ = 0;  // bit 0: 1 = inline storage, 0 = heap storage

    static constexpr uint32_t kInlineCapacity = 32;
    static constexpr uint32_t kFlagInline = 1u;

    union {
        char inline_buf_[kInlineCapacity];
        char* heap_ptr_;
    };

    explicit JSString(std::string_view sv) : size_(static_cast<uint32_t>(sv.size())) {
        // Reject strings larger than UINT32_MAX to prevent silent truncation.
        if (sv.size() > UINT32_MAX) std::abort();
        if (size_ <= kInlineCapacity) {
            flags_ = kFlagInline;
            std::memcpy(inline_buf_, sv.data(), size_);
        } else {
            flags_ = 0;
            heap_ptr_ = static_cast<char*>(std::malloc(size_));
            if (!heap_ptr_) std::abort();
            std::memcpy(heap_ptr_, sv.data(), size_);
        }
    }

    // JSString owns heap_ptr_ in the non-inline case, so copying is not allowed.
    JSString(const JSString&) = delete;
    JSString& operator=(const JSString&) = delete;

    JSString(JSString&& other) noexcept
        : ref_count(other.ref_count), cp_count_(other.cp_count_), size_(other.size_), flags_(other.flags_) {
        if (flags_ & kFlagInline) {
            std::memcpy(inline_buf_, other.inline_buf_, size_);
        } else {
            heap_ptr_ = other.heap_ptr_;
            other.flags_ |= kFlagInline;
            other.size_ = 0;
        }
    }

    JSString& operator=(JSString&& other) noexcept {
        if (this != &other) {
            if (!(flags_ & kFlagInline)) std::free(heap_ptr_);
            ref_count = other.ref_count;
            cp_count_ = other.cp_count_;
            size_ = other.size_;
            flags_ = other.flags_;
            if (flags_ & kFlagInline) {
                std::memcpy(inline_buf_, other.inline_buf_, size_);
            } else {
                heap_ptr_ = other.heap_ptr_;
                other.flags_ |= kFlagInline;
                other.size_ = 0;
            }
        }
        return *this;
    }

    ~JSString() {
        if (!(flags_ & kFlagInline)) {
            std::free(heap_ptr_);
        }
    }

    [[nodiscard]] bool is_inline() const noexcept { return (flags_ & kFlagInline) != 0; }

    [[nodiscard]] std::string_view sv() const noexcept {
        return std::string_view(is_inline() ? inline_buf_ : heap_ptr_, size_);
    }

    void add_ref() { ++ref_count; }

    void release() {
        if (--ref_count == 0) {
            delete this;
        }
    }
};

static_assert(sizeof(JSString) == 48, "JSString must be 48 bytes with SSO layout");

}  // namespace qppjs
