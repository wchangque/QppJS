#pragma once

#include "qppjs/runtime/value.h"

#include <deque>
#include <vector>

namespace qppjs {

// A reaction job: execute handler(arg), then fulfill/reject capability.
// handler may be undefined (identity/thrower behavior).
// capability is a JSPromise* (non-owning, kept alive by GC).
// is_fulfill: true = fulfill reaction, false = reject reaction.
struct ReactionJob {
    Value handler;       // JS function or undefined
    Value capability;    // JSPromise wrapped as Value::object()
    Value arg;           // settled value
    bool is_fulfill;     // true = fulfill reaction, false = reject reaction
};

class JobQueue {
public:
    void Enqueue(ReactionJob job);

    // Collect all Value roots (for GC tracing).
    void CollectRoots(std::vector<Value>& out) const;

    bool empty() const { return queue_.empty(); }

    // Execute all queued jobs. executor(job) is called for each entry.
    // New jobs enqueued during execution are also drained.
    template <typename Executor>
    void DrainAll(Executor&& executor) {
        while (!queue_.empty()) {
            ReactionJob job = std::move(queue_.front());
            queue_.pop_front();
            executor(std::move(job));
        }
    }

private:
    std::deque<ReactionJob> queue_;
};

}  // namespace qppjs
