#pragma once

#include <unordered_set>
#include <vector>

namespace qppjs {

class RcObject;

class GcHeap {
public:
    void Register(RcObject* obj);
    void Unregister(RcObject* obj);
    void MarkPending(RcObject* obj);
    void Collect(const std::vector<RcObject*>& roots);

private:
    void DrainWorklist();
    void Sweep();

    std::unordered_set<RcObject*> objects_;
    std::vector<RcObject*> worklist_;
};

}  // namespace qppjs
