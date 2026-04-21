#include "qppjs/base/error.h"
#include "qppjs/debug/format.h"
#include "qppjs/frontend/parser.h"
#include "qppjs/runtime/interpreter.h"

#include <iostream>
#include <string_view>

namespace {

int fail_with_usage() {
    const qppjs::Error error(qppjs::ErrorKind::Cli, "usage: qppjs <source>");
    std::cerr << qppjs::format_error(error) << '\n';
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        fail_with_usage();
        return 0;
    }

    const std::string_view source(argv[1]);

    auto parse_result = qppjs::parse_program(source);
    if (!parse_result.ok()) {
        std::cerr << qppjs::format_error(parse_result.error()) << '\n';
        return 1;
    }

    qppjs::Interpreter interp;
    auto exec_result = interp.exec(parse_result.value());
    if (!exec_result.is_ok()) {
        std::cerr << qppjs::format_error(exec_result.error()) << '\n';
        return 1;
    }

    std::cout << qppjs::format_value(exec_result.value()) << '\n';
    return 0;
}
