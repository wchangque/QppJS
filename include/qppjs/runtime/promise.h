#pragma once

#include "qppjs/runtime/job_queue.h"
#include "qppjs/runtime/rc_object.h"
#include "qppjs/runtime/value.h"

#include <vector>

namespace qppjs {

enum class PromiseState { kPending, kFulfilled, kRejected };

struct PromiseReaction {
    Value handler;    // JS function or undefined
    Value capability; // JSPromise wrapped as Value::object(), may be undefined
    bool is_fulfill;
};

class JSPromise : public RcObject {
public:
    JSPromise() : RcObject(ObjectKind::kPromise) {
        fulfill_reactions_.reserve(1);
        reject_reactions_.reserve(1);
    }

    void TraceRefs(GcHeap& heap) override;
    void ClearRefs() override;

    PromiseState state() const { return state_; }
    const Value& result() const { return result_; }

    // 状态转换（幂等，settled 后无效）
    void Fulfill(Value value, JobQueue& jq);
    void Reject(Value reason, JobQueue& jq);

    // PerformPromiseThen：追加 reaction 或立即入队
    // 返回 result_promise（链式调用，未注册到 GC，调用方负责注册）
    static RcPtr<JSPromise> PerformThen(
        RcPtr<JSPromise> promise,
        Value on_fulfilled,
        Value on_rejected,
        JobQueue& jq);

private:
    void EnqueueReactions(std::vector<PromiseReaction>& reactions,
                          const Value& arg, JobQueue& jq);

    PromiseState state_ = PromiseState::kPending;
    Value result_;
    std::vector<PromiseReaction> fulfill_reactions_;
    std::vector<PromiseReaction> reject_reactions_;
};

}  // namespace qppjs
