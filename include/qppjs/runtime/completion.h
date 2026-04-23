#pragma once

#include "qppjs/base/error.h"
#include "qppjs/runtime/value.h"

#include <optional>
#include <string>
#include <variant>

namespace qppjs {

// ---- Completion ----

enum class CompletionType { kNormal, kReturn, kThrow, kBreak, kContinue };

struct Completion {
    CompletionType type;
    Value value;
    std::optional<std::string> target;  // label for kBreak / kContinue

    static Completion normal(Value v);
    static Completion return_(Value v);
    static Completion throw_(Value v);
    static Completion break_(std::optional<std::string> label = std::nullopt);
    static Completion continue_(std::optional<std::string> label = std::nullopt);

    bool is_normal() const;
    bool is_return() const;
    bool is_throw() const;
    bool is_break() const;
    bool is_continue() const;
    bool is_abrupt() const;
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
