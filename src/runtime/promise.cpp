#include "qppjs/runtime/promise.h"

#include "qppjs/runtime/gc_heap.h"

namespace qppjs {

// ============================================================
// GC support
// ============================================================

void JSPromise::TraceRefs(GcHeap& heap) {
    if (result_.is_object()) {
        auto* raw = result_.as_object_raw();
        if (raw) heap.MarkPending(raw);
    }
    for (const auto& r : fulfill_reactions_) {
        if (r.handler.is_object()) heap.MarkPending(r.handler.as_object_raw());
        if (r.capability.is_object()) heap.MarkPending(r.capability.as_object_raw());
    }
    for (const auto& r : reject_reactions_) {
        if (r.handler.is_object()) heap.MarkPending(r.handler.as_object_raw());
        if (r.capability.is_object()) heap.MarkPending(r.capability.as_object_raw());
    }
}

void JSPromise::ClearRefs() {
    result_ = Value::undefined();
    for (auto& r : fulfill_reactions_) {
        r.handler = Value::undefined();
        r.capability = Value::undefined();
    }
    fulfill_reactions_.clear();
    for (auto& r : reject_reactions_) {
        r.handler = Value::undefined();
        r.capability = Value::undefined();
    }
    reject_reactions_.clear();
}

// ============================================================
// State transitions
// ============================================================

void JSPromise::Fulfill(Value value, JobQueue& jq) {
    if (state_ != PromiseState::kPending) return;
    state_ = PromiseState::kFulfilled;
    result_ = value;
    EnqueueReactions(fulfill_reactions_, value, jq);
    fulfill_reactions_.clear();
    reject_reactions_.clear();
}

void JSPromise::Reject(Value reason, JobQueue& jq) {
    if (state_ != PromiseState::kPending) return;
    state_ = PromiseState::kRejected;
    result_ = reason;
    EnqueueReactions(reject_reactions_, reason, jq);
    fulfill_reactions_.clear();
    reject_reactions_.clear();
}

// ============================================================
// EnqueueReactions
// ============================================================

void JSPromise::EnqueueReactions(std::vector<PromiseReaction>& reactions,
                                  const Value& arg, JobQueue& jq) {
    for (auto& reaction : reactions) {
        jq.Enqueue(ReactionJob{reaction.handler, reaction.capability, arg, reaction.is_fulfill});
    }
}

// ============================================================
// PerformPromiseThen
// ============================================================

RcPtr<JSPromise> JSPromise::PerformThen(
    RcPtr<JSPromise> promise,
    Value on_fulfilled,
    Value on_rejected,
    JobQueue& jq) {
    auto result_promise = RcPtr<JSPromise>::make();
    Value cap_val = Value::object(ObjectPtr(result_promise));

    PromiseReaction fulfill_reaction{on_fulfilled, cap_val, true};
    PromiseReaction reject_reaction{on_rejected, cap_val, false};

    if (promise->state_ == PromiseState::kPending) {
        promise->fulfill_reactions_.push_back(std::move(fulfill_reaction));
        promise->reject_reactions_.push_back(std::move(reject_reaction));
    } else if (promise->state_ == PromiseState::kFulfilled) {
        std::vector<PromiseReaction> reactions;
        reactions.push_back(std::move(fulfill_reaction));
        promise->EnqueueReactions(reactions, promise->result_, jq);
    } else {
        std::vector<PromiseReaction> reactions;
        reactions.push_back(std::move(reject_reaction));
        promise->EnqueueReactions(reactions, promise->result_, jq);
    }

    return result_promise;
}

}  // namespace qppjs
