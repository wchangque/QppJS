#include "qppjs/base/error.h"
#include "qppjs/debug/format.h"
#include "qppjs/frontend/parser.h"
#include "qppjs/runtime/interpreter.h"
#include "qppjs/vm/compiler.h"
#include "qppjs/vm/vm.h"

#include <cstring>
#include <iostream>
#include <string_view>

namespace {

int fail_with_usage() {
    const qppjs::Error error(qppjs::ErrorKind::Cli, "usage: qppjs [--vm] <source>");
    std::cerr << qppjs::format_error(error) << '\n';
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    bool use_vm = false;
    int source_idx = 1;

    if (argc == 3 && std::strcmp(argv[1], "--vm") == 0) {
        use_vm = true;
        source_idx = 2;
    } else if (argc != 2) {
        return fail_with_usage();
    }

    const std::string_view source(argv[source_idx]);

    auto parse_result = qppjs::parse_program(source);
    if (!parse_result.ok()) {
        std::cerr << qppjs::format_error(parse_result.error()) << '\n';
        return 1;
    }

    qppjs::EvalResult exec_result = qppjs::EvalResult::err(
        qppjs::Error(qppjs::ErrorKind::Runtime, "unreachable"));

    if (use_vm) {
        qppjs::Compiler compiler;
        auto bytecode = compiler.compile(parse_result.value());
        qppjs::VM vm;
        exec_result = vm.exec(bytecode);
    } else {
        qppjs::Interpreter interp;
        exec_result = interp.exec(parse_result.value());
    }

    if (!exec_result.is_ok()) {
        std::cerr << qppjs::format_error(exec_result.error()) << '\n';
        return 1;
    }

    std::cout << qppjs::format_value(exec_result.value()) << '\n';
    return 0;
}
