#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace qppjs {

class GcHeap;

enum class ObjectKind { kOrdinary, kFunction, kArray, kEnvironment, kModule };

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

// JSString: heap-allocated string with non-atomic reference counting.
// Known limitation: std::string causes a second heap allocation (tech debt).
struct JSString {
    int32_t ref_count = 0;
    std::string str;

    explicit JSString(std::string s) : str(std::move(s)) {}

    void add_ref() { ++ref_count; }

    void release() {
        if (--ref_count == 0) {
            delete this;
        }
    }
};

}  // namespace qppjs
