#pragma once

#include "qppjs/frontend/ast.h"
#include "qppjs/runtime/environment.h"
#include "qppjs/runtime/js_object.h"
#include "qppjs/runtime/rc_object.h"
#include "qppjs/runtime/value.h"

#include <memory>
#include <optional>
#include <vector>

namespace qppjs {

class GcHeap;
struct CallFrame;

enum class GeneratorState {
    kSuspendedStart,
    kExecuting,
    kSuspendedYield,
    kCompleted,
};

enum class ResumeMode {
    kNext,
    kReturn,
    kThrow,
};

// Generator object: JS-visible object returned when calling a generator function.
// Inherits JSObject so it IS the iterable/iterator object (no wrapper needed).
class JSGeneratorObject : public JSObject {
public:
    JSGeneratorObject();
    ~JSGeneratorObject() override;  // defined in js_generator.cpp where CallFrame is complete

    void TraceRefs(GcHeap& heap) override;
    void ClearRefs() override;

    GeneratorState state_ = GeneratorState::kSuspendedStart;
    ResumeMode resume_mode_ = ResumeMode::kNext;
    std::optional<Value> resume_value_;

    // VM path: suspended call frame
    std::unique_ptr<CallFrame> suspended_frame_;

    // Interpreter path: saved state
    std::shared_ptr<std::vector<StmtNode>> gen_body_;
    RcPtr<Environment> gen_env_;       // saved fn_env (with hoisted vars + params)
    size_t suspended_stmt_index_ = 0;  // index in gen_body_ to resume from
    Value gen_this_val_;               // 'this' value for the generator body
    bool vars_hoisted_ = false;        // true after first hoist_vars call

    // Interpreter path: saved yield* delegate iterator (non-undefined while inside yield*)
    Value yield_delegate_iter_;
    Value yield_delegate_next_;
};

}  // namespace qppjs
