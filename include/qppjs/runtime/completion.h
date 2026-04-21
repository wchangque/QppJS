#pragma once

#include "qppjs/base/error.h"
#include "qppjs/runtime/value.h"

#include <variant>

namespace qppjs {

// ---- Completion ----

enum class CompletionType { kNormal, kReturn };

struct Completion {
    CompletionType type;
    Value value;

    static Completion normal(Value v);
    static Completion return_(Value v);

    bool is_normal() const;
    bool is_return() const;
};

// ---- EvalResult ----

struct EvalResult {
    std::variant<Value, Error> data;

    static EvalResult ok(Value v);
    static EvalResult err(Error e);

    bool is_ok() const;

    Value& value();
    const Value& value() const;

    Error& error();
    const Error& error() const;
};

// ---- StmtResult ----

struct StmtResult {
    std::variant<Completion, Error> data;

    static StmtResult ok(Completion c);
    static StmtResult err(Error e);

    bool is_ok() const;

    Completion& completion();
    const Completion& completion() const;

    Error& error();
    const Error& error() const;
};

}  // namespace qppjs
