#include "qppjs/base/error.h"
#include "qppjs/debug/format.h"
#include "qppjs/frontend/parser.h"
#include "qppjs/runtime/interpreter.h"
#include "qppjs/vm/compiler.h"
#include "qppjs/vm/vm.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace {

void print_help() {
    std::cout << "usage: qppjs [options] [file]\n"
              << "\n"
              << "Options:\n"
              << "  -h, --help        显示帮助信息\n"
              << "  -e, --eval EXPR   执行内联表达式\n"
              << "  -m, --module      作为 ES 模块执行\n"
              << "  --vm              使用字节码 VM（默认使用 AST 解释器）\n";
}

int fail_with_error(const std::string& message) {
    const qppjs::Error error(qppjs::ErrorKind::Cli, message);
    std::cerr << qppjs::format_error(error) << '\n';
    return 1;
}

// 返回读取的内容；若文件无法打开则返回 nullopt
std::optional<std::string> read_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string read_stdin() {
    std::cin >> std::noskipws;
    std::string content{std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>()};
    return content;
}

}  // namespace

int main(int argc, char** argv) {
    bool use_vm = false;
    bool is_module = false;
    std::string source;
    int arg_idx = 1;

    // 解析命令行参数
    while (arg_idx < argc) {
        std::string_view arg(argv[arg_idx]);

        if (arg == "-h" || arg == "--help") {
            print_help();
            return 0;
        }

        if (arg == "--vm") {
            use_vm = true;
            arg_idx++;
            continue;
        }

        if (arg == "-m" || arg == "--module") {
            is_module = true;
            arg_idx++;
            continue;
        }

        if (arg == "-e" || arg == "--eval") {
            if (arg_idx + 1 >= argc) {
                return fail_with_error("-e/--eval 需要表达式参数");
            }
            source = argv[arg_idx + 1];
            arg_idx += 2;
            continue;
        }

        // 位置参数：文件路径
        if (arg[0] != '-') {
            auto content = read_file(std::string(arg));
            if (!content.has_value()) {
                return fail_with_error("无法打开文件: " + std::string(arg));
            }
            source = std::move(content.value());
            arg_idx++;
            continue;
        }

        return fail_with_error("未知选项: " + std::string(arg));
    }

    // 无参数：从 stdin 读取
    if (source.empty() && !is_module) {
        source = read_stdin();
    }

    if (source.empty() && is_module) {
        return fail_with_error("模块模式需要指定文件路径");
    }

    auto parse_result = qppjs::parse_program(source, is_module);
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