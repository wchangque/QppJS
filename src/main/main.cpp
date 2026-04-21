#include "qppjs/base/error.h"
#include "qppjs/debug/format.h"

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
    std::cout << "source: " << source << '\n';
    return 0;
}
