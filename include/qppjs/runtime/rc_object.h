#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace qppjs {

enum class ObjectKind { kOrdinary, kFunction, kArray };

// Base class for all heap-allocated JS objects with non-atomic reference counting.
// Subclasses must call RcObject(ObjectKind) in their constructor.
//
// Known limitation: circular references (e.g., proto chains, closure envs) will leak.
// Phase 9 GC will resolve this.
class RcObject {
public:
    explicit RcObject(ObjectKind kind) : ref_count_(0), object_kind_(static_cast<int32_t>(kind)) {}

    virtual ~RcObject() = default;

    void add_ref() { ++ref_count_; }

    void release() {
        if (--ref_count_ == 0) {
            delete this;
        }
    }

    ObjectKind object_kind() const { return static_cast<ObjectKind>(object_kind_); }

private:
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
        if (this != &other) {
            if (ptr_) {
                ptr_->release();
            }
            ptr_ = other.ptr_;
            if (ptr_) {
                ptr_->add_ref();
            }
        }
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
