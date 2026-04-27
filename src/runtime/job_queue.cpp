#include "qppjs/runtime/job_queue.h"

namespace qppjs {

void JobQueue::Enqueue(ReactionJob job) {
    queue_.push_back(std::move(job));
}

void JobQueue::CollectRoots(std::vector<Value>& out) const {
    for (const auto& job : queue_) {
        out.push_back(job.handler);
        out.push_back(job.capability);
        out.push_back(job.arg);
    }
}

}  // namespace qppjs
