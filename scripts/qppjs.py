#!/usr/bin/env python3

from __future__ import annotations

import argparse
import contextlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class ProjectPaths:
    root: Path
    build_root: Path

    @classmethod
    def discover(cls) -> "ProjectPaths":
        root = Path(__file__).resolve().parent.parent
        return cls(root=root, build_root=root / "build")


@dataclass(frozen=True)
class BuildConfig:
    name: str
    build_dir: Path
    build_type: str
    build_tests: bool
    enable_asan: bool = False
    enable_coverage: bool = False


class CommandError(RuntimeError):
    def __init__(self, returncode: int) -> None:
        super().__init__(f"command failed with exit code {returncode}")
        self.returncode = returncode


@contextlib.contextmanager
def split_log(
    success_path: Path,
    failure_path: Path,
    *,
    failure_filter: "((Path, Path) -> None) | None" = None,
):
    """Write accumulated log to success_path on clean exit, failure_path on CommandError.

    failure_filter(raw, failure_path) is called before renaming on failure;
    if omitted the raw log is renamed directly.
    """
    raw = success_path.with_suffix(".raw.log")
    raw.parent.mkdir(parents=True, exist_ok=True)
    success_path.unlink(missing_ok=True)
    failure_path.unlink(missing_ok=True)
    raw.unlink(missing_ok=True)
    try:
        yield raw
    except CommandError:
        if failure_filter is not None:
            failure_filter(raw, failure_path)
            raw.unlink(missing_ok=True)
        else:
            raw.replace(failure_path)
        raise
    else:
        raw.replace(success_path)


class Runner:
    def run(
        self,
        args: list[str],
        *,
        env: dict[str, str] | None = None,
        stdout: int | None = None,
        stderr: int | None = None,
    ) -> None:
        result = subprocess.run(args, env=env, stdout=stdout, stderr=stderr, check=False)
        if result.returncode != 0:
            raise CommandError(result.returncode)

    def run_capture(self, args: list[str], *, env: dict[str, str] | None = None, output_file: Path) -> None:
        output_file.parent.mkdir(parents=True, exist_ok=True)
        with output_file.open("w", encoding="utf-8", errors="replace") as f:
            result = subprocess.run(args, env=env, stdout=f, stderr=subprocess.STDOUT, check=False)
        if result.returncode != 0:
            raise CommandError(result.returncode)


class Toolchain:
    def __init__(self) -> None:
        self.env = os.environ.copy()
        self.extra_linker_flags = ""
        self._detect_macos_homebrew_llvm()

    def _detect_macos_homebrew_llvm(self) -> None:
        if self.env.get("CC") or self.env.get("CXX"):
            return
        if platform.system() != "Darwin":
            return
        if shutil.which("brew") is None:
            return
        brew = shutil.which("brew")
        assert brew is not None
        result = subprocess.run([brew, "--prefix", "llvm"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        llvm_prefix = result.stdout.strip() if result.returncode == 0 else ""
        if not llvm_prefix:
            return
        clang = Path(llvm_prefix) / "bin" / "clang"
        clangxx = Path(llvm_prefix) / "bin" / "clang++"
        if not clang.is_file() or not os.access(clang, os.X_OK):
            return
        self.env["CC"] = str(clang)
        self.env["CXX"] = str(clangxx)
        self.extra_linker_flags = f"-L{llvm_prefix}/lib/c++ -Wl,-rpath,{llvm_prefix}/lib/c++"


class Builder:
    def __init__(self, paths: ProjectPaths, runner: Runner) -> None:
        self.paths = paths
        self.runner = runner

    def config(self, name: str) -> BuildConfig:
        configs = {
            "debug": BuildConfig(
                name="debug",
                build_dir=self.paths.build_root / "debug",
                build_type="Debug",
                build_tests=True,
                enable_asan=True,
            ),
            "release": BuildConfig(
                name="release",
                build_dir=self.paths.build_root / "release",
                build_type="Release",
                build_tests=False,
            ),
            "test": BuildConfig(
                name="test",
                build_dir=self.paths.build_root / "test",
                build_type="Debug",
                build_tests=True,
                enable_coverage=True,
            ),
        }
        return configs[name]

    def build(self, name: str, *, quiet_log: Path | None = None) -> None:
        config = self.config(name)
        toolchain = Toolchain()
        cmake_args = [
            "cmake",
            "-S",
            str(self.paths.root),
            "-B",
            str(config.build_dir),
            "-G",
            "Ninja",
            "-D",
            f"CMAKE_BUILD_TYPE={config.build_type}",
            "-D",
            "CMAKE_EXPORT_COMPILE_COMMANDS=ON",
            "-D",
            f"QPPJS_BUILD_TESTS={bool_to_cmake(config.build_tests)}",
        ]
        if toolchain.extra_linker_flags:
            cmake_args.extend(["-D", f"CMAKE_EXE_LINKER_FLAGS={toolchain.extra_linker_flags}"])
            cmake_args.extend(["-D", f"CMAKE_SHARED_LINKER_FLAGS={toolchain.extra_linker_flags}"])
        if config.enable_asan:
            cmake_args.extend(["-D", "QPPJS_ENABLE_ASAN=ON"])
        if config.enable_coverage:
            cmake_args.extend(["-D", "QPPJS_ENABLE_COVERAGE=ON"])

        build_args = ["cmake", "--build", str(config.build_dir)]
        if quiet_log is None:
            self.runner.run(cmake_args, env=toolchain.env)
            self.runner.run(build_args, env=toolchain.env)
            return

        quiet_log.parent.mkdir(parents=True, exist_ok=True)
        with quiet_log.open("w", encoding="utf-8", errors="replace") as f:
            result = subprocess.run(cmake_args, env=toolchain.env, stdout=f, stderr=subprocess.STDOUT, check=False)
            if result.returncode == 0:
                result = subprocess.run(build_args, env=toolchain.env, stdout=f, stderr=subprocess.STDOUT, check=False)
        if result.returncode != 0:
            raise CommandError(result.returncode)


class TestRunner:
    def __init__(self, paths: ProjectPaths, runner: Runner, builder: Builder) -> None:
        self.paths = paths
        self.runner = runner
        self.builder = builder
        self.build_dir = paths.build_root / "debug"

    def run(self, *, clean_first: bool, quiet: bool) -> None:
        if clean_first:
            clean(self.paths)
        if quiet:
            with split_log(self.build_dir / "run_ut_build_success.log", self.build_dir / "run_ut_build_failure.log") as log:
                try:
                    self.builder.build("debug", quiet_log=log)
                except CommandError:
                    print(f"build failed. Failure log written to: {self.build_dir / 'run_ut_build_failure.log'}", file=sys.stderr)
                    raise
            self.run_quiet(self.ctest_args(), test_env(self.paths))
        else:
            self.builder.build("debug")
            self.runner.run(["ctest", *self.ctest_args()], env=test_env(self.paths))

    def ctest_args(self) -> list[str]:
        args = ["--test-dir", str(self.build_dir), "--output-on-failure", "-E", "^qppjs_cli_"]
        meta_file = self.build_dir / "qppjs-build-meta.json"
        if meta_file.is_file() and read_meta(meta_file).get("is_multi_config", False):
            args.extend(["-C", "Debug"])
        return args

    def run_quiet(self, ctest_args: list[str], env: dict[str, str]) -> None:
        failure_report = self.build_dir / "run_ut_failure.log"
        result = subprocess.run(["ctest", *ctest_args], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        with split_log(self.build_dir / "run_ut_success.log", failure_report, failure_filter=write_test_failure_report) as raw:
            raw.write_text(result.stdout, encoding="utf-8", errors="replace")
            if result.returncode != 0:
                print(f"UT failed. Failure and leak details written to: {failure_report}", file=sys.stderr)
                raise CommandError(result.returncode)


class CoverageRunner:
    def __init__(self, paths: ProjectPaths, runner: Runner, builder: Builder) -> None:
        self.paths = paths
        self.runner = runner
        self.builder = builder
        self.build_dir = paths.build_root / "test"
        self.raw_info = self.build_dir / "coverage_raw.info"
        self.filtered_info = self.build_dir / "coverage.info"
        self.report_dir = self.build_dir / "coverage"

    def run(self, *, clean_first: bool, open_report: bool, quiet: bool) -> None:
        if clean_first:
            clean(self.paths)
        if quiet:
            failure_report = self.build_dir / "coverage_failure.log"

            def coverage_failure_filter(raw: Path, dest: Path) -> None:
                write_test_failure_report(raw, dest, title="QppJS coverage failed tests / leak report")

            with split_log(self.build_dir / "coverage_success.log", failure_report, failure_filter=coverage_failure_filter) as log:
                try:
                    self._run(open_report=open_report, quiet_log=log)
                except CommandError:
                    print(f"coverage failed. Failure log written to: {failure_report}", file=sys.stderr)
                    raise
        else:
            self._run(open_report=open_report)

    def _run(self, *, open_report: bool, quiet_log: Path | None = None) -> None:
        self.builder.build("test", quiet_log=quiet_log)
        meta_file = self.build_dir / "qppjs-build-meta.json"
        if not meta_file.is_file():
            print(f"error: build metadata not found: {meta_file}", file=sys.stderr)
            print("hint: build_test.sh must finish successfully first", file=sys.stderr)
            raise CommandError(1)
        meta = read_meta(meta_file)
        if meta.get("coverage_enabled") is not True:
            print(f"error: coverage is not enabled for build directory: {self.build_dir}", file=sys.stderr)
            raise CommandError(1)
        require_tools(["lcov", "genhtml"])

        ctest_args = ["--test-dir", str(self.build_dir), "--output-on-failure"]
        if meta.get("is_multi_config", False):
            ctest_args.extend(["-C", "Debug"])

        backend = str(meta.get("coverage_backend", ""))
        gcov_tool = ""
        if backend == "gcov":
            gcov_tool = self.run_gcov(ctest_args, quiet_log)
        elif backend == "llvm-cov":
            self.run_llvm_cov(ctest_args, meta, quiet_log)
        else:
            print(
                f"error: unsupported coverage backend '{backend}' on platform '{meta.get('platform', '')}'",
                file=sys.stderr,
            )
            raise CommandError(1)

        self.filter_coverage(gcov_tool, quiet_log)
        self.generate_html(quiet_log)
        if not quiet_log:
            print("")
        print(f"report: {self.report_dir / 'index.html'}")
        if open_report:
            self.open_report()

    def run_gcov(self, ctest_args: list[str], quiet_log: Path | None) -> str:
        self.run_step(["ctest", *ctest_args], quiet_log)
        if not first_match(self.build_dir, "*.gcno"):
            print(f"error: no .gcno files found in {self.build_dir}", file=sys.stderr)
            raise CommandError(1)
        if not first_match(self.build_dir, "*.gcda"):
            print(f"error: no .gcda files found in {self.build_dir}", file=sys.stderr)
            raise CommandError(1)
        gcov_tool = shutil.which("gcov")
        if gcov_tool is None:
            print("error: 'gcov' not found", file=sys.stderr)
            raise CommandError(1)
        if quiet_log is None:
            print("collecting coverage data...")
        self.run_step(
            [
                "lcov",
                "--gcov-tool",
                gcov_tool,
                "--capture",
                "--directory",
                str(self.build_dir),
                "--output-file",
                str(self.raw_info),
                "--ignore-errors",
                "mismatch,empty,inconsistent,unsupported,format,gcov",
                "--rc",
                "branch_coverage=1",
                "--rc",
                "derive_function_end_line=0",
                "--rc",
                "geninfo_unexecuted_blocks=1",
            ],
            quiet_log,
        )
        return gcov_tool

    def run_llvm_cov(self, ctest_args: list[str], meta: dict[str, object], quiet_log: Path | None) -> None:
        for path in self.build_dir.glob("default-*.profraw"):
            path.unlink()
        env = os.environ.copy()
        env["LLVM_PROFILE_FILE"] = str(self.build_dir / "default-%p.profraw")
        self.run_step(["ctest", *ctest_args], quiet_log, env=env)
        profraw_files = sorted(self.build_dir.glob("default-*.profraw"))
        if not profraw_files:
            print(f"error: no llvm profile data found matching: {self.build_dir / 'default-*.profraw'}", file=sys.stderr)
            raise CommandError(1)
        test_binary = Path(str(meta.get("test_binary", "")))
        if not test_binary.is_file() or not os.access(test_binary, os.X_OK):
            print(f"error: test binary not found: {test_binary}", file=sys.stderr)
            raise CommandError(1)
        llvm_cov = shutil.which("llvm-cov")
        llvm_profdata = shutil.which("llvm-profdata")
        if llvm_cov is None or llvm_profdata is None:
            print("error: llvm-cov/llvm-profdata not found", file=sys.stderr)
            raise CommandError(1)
        if quiet_log is None:
            print(f"merging {len(profraw_files)} profraw file(s)...")
        profdata = self.build_dir / "default.profdata"
        self.run_step([llvm_profdata, "merge", "-sparse", *map(str, profraw_files), "-o", str(profdata)], quiet_log)
        stdout = quiet_log.open("a", encoding="utf-8", errors="replace") if quiet_log is not None else self.raw_info.open(
            "w", encoding="utf-8", errors="replace"
        )
        with stdout as f:
            result = subprocess.run(
                [llvm_cov, "export", str(test_binary), f"-instr-profile={profdata}", "-format=lcov"],
                stdout=f,
                stderr=subprocess.STDOUT if quiet_log is not None else None,
                check=False,
            )
        if result.returncode != 0:
            raise CommandError(result.returncode)
        if quiet_log is not None:
            with self.raw_info.open("w", encoding="utf-8", errors="replace") as f:
                result = subprocess.run(
                    [llvm_cov, "export", str(test_binary), f"-instr-profile={profdata}", "-format=lcov"],
                    stdout=f,
                    stderr=subprocess.DEVNULL,
                    check=False,
                )
            if result.returncode != 0:
                raise CommandError(result.returncode)

    def filter_coverage(self, gcov_tool: str, quiet_log: Path | None) -> None:
        if quiet_log is None:
            print("filtering coverage data...")
        args = ["lcov"]
        if gcov_tool:
            args.extend(["--gcov-tool", gcov_tool])
        args.extend(
            [
                "--remove",
                str(self.raw_info),
                "*/tests/*",
                "*/_deps/*",
                "*/usr/*",
                "*/opt/homebrew/*",
                "--output-file",
                str(self.filtered_info),
                "--ignore-errors",
                "unused,unused,unused,inconsistent,unsupported,format",
                "--rc",
                "branch_coverage=1",
                "--rc",
                "derive_function_end_line=0",
                "--rc",
                "geninfo_unexecuted_blocks=1",
            ]
        )
        self.run_step(args, quiet_log)

    def generate_html(self, quiet_log: Path | None) -> None:
        if quiet_log is None:
            print("generating HTML report...")
        self.run_step(
            [
                "genhtml",
                str(self.filtered_info),
                "--output-directory",
                str(self.report_dir),
                "--branch-coverage",
                "--title",
                "QppJS Coverage",
                "--ignore-errors",
                "inconsistent,unsupported,corrupt,category",
                "--rc",
                "branch_coverage=1",
                "--rc",
                "derive_function_end_line=0",
            ],
            quiet_log,
        )

    def run_step(self, args: list[str], quiet_log: Path | None, *, env: dict[str, str] | None = None) -> None:
        if quiet_log is None:
            self.runner.run(args, env=env)
            return
        with quiet_log.open("a", encoding="utf-8", errors="replace") as f:
            result = subprocess.run(args, env=env, stdout=f, stderr=subprocess.STDOUT, check=False)
        if result.returncode != 0:
            raise CommandError(result.returncode)

    def open_report(self) -> None:
        report = self.report_dir / "index.html"
        system = platform.system()
        if system == "Darwin":
            self.runner.run(["open", str(report)])
        elif system == "Linux":
            self.runner.run(["xdg-open", str(report)])


def bool_to_cmake(value: bool) -> str:
    return "ON" if value else "OFF"


def read_meta(path: Path) -> dict[str, object]:
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def clean(paths: ProjectPaths) -> None:
    shutil.rmtree(paths.build_root, ignore_errors=True)


def require_tools(names: list[str]) -> None:
    for name in names:
        if shutil.which(name) is None:
            print(f"error: '{name}' not found", file=sys.stderr)
            if name in {"lcov", "genhtml"}:
                print("hint: install lcov", file=sys.stderr)
            raise CommandError(1)


def first_match(root: Path, pattern: str) -> Path | None:
    return next(root.rglob(pattern), None)


def test_env(paths: ProjectPaths) -> dict[str, str]:
    env = os.environ.copy()
    if platform.system() == "Darwin":
        env["ASAN_OPTIONS"] = f"{env.get('ASAN_OPTIONS', '')}detect_leaks=1"
        suppressions = paths.root / "lsan_suppressions.txt"
        if suppressions.is_file():
            env["LSAN_OPTIONS"] = f"{env.get('LSAN_OPTIONS', '')}suppressions={suppressions}"
    return env


def write_test_failure_report(raw_log: Path, report_file: Path, *, title: str = "QppJS run_ut failed tests / leak report") -> None:
    text = raw_log.read_text(encoding="utf-8", errors="replace")
    blocks: list[str] = []
    current: list[str] = []
    current_failed = False
    start_re = re.compile(r"^\s*Start\s+\d+:\s+(.+)$")
    end_re = re.compile(r"^\d+/\d+\s+Test\s+#\d+:\s+.+\*\*\*Failed")

    for line in text.splitlines():
        if start_re.match(line):
            if current and current_failed:
                blocks.append("\n".join(current))
            current = [line]
            current_failed = False
            continue
        if current:
            current.append(line)
            if end_re.match(line) or "LeakSanitizer: detected memory leaks" in line:
                current_failed = True
    if current and current_failed:
        blocks.append("\n".join(current))

    failed_summary: list[str] = []
    in_summary = False
    for line in text.splitlines():
        if re.match(r"^\d+% tests passed, \d+ tests failed out of \d+", line):
            in_summary = True
        if in_summary:
            failed_summary.append(line)

    with report_file.open("w", encoding="utf-8") as f:
        f.write(f"{title}\n")
        f.write("========================================\n\n")
        if blocks:
            f.write("\n\n".join(blocks))
            f.write("\n")
        else:
            f.write("No per-test failure block found. Full ctest failure summary follows.\n")
        if failed_summary:
            f.write("\nSummary\n-------\n")
            f.write("\n".join(failed_summary))
            f.write("\n")


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="QppJS 项目构建、测试和覆盖率统一入口。",
        epilog=(
            "常用示例:\n"
            "  python3 scripts/qppjs.py build debug     配置并构建 Debug + ASAN + 单元测试\n"
            "  python3 scripts/qppjs.py build release   配置并构建 Release，不构建单元测试\n"
            "  python3 scripts/qppjs.py build test      配置并构建覆盖率专用 Debug 测试目录\n"
            "  python3 scripts/qppjs.py test --quiet    静默构建并运行 UT；成功日志写入 build/debug/run_ut_success.log，失败详情写入 build/debug/run_ut_failure.log，构建日志写入同目录下的 run_ut_build_success.log / run_ut_build_failure.log\n"
            "  python3 scripts/qppjs.py coverage --quiet 静默生成覆盖率报告；成功日志写入 build/coverage_success.log，失败时仅将失败 UT 摘要写入 build/coverage_failure.log\n"
            "  python3 scripts/qppjs.py coverage --open  生成覆盖率 HTML 报告并打开\n"
            "  python3 scripts/qppjs.py clean                 删除整个 build/ 目录\n"
            "  python3 scripts/qppjs.py clean build release   先删除 build/，再构建 Release\n"
            "  python3 scripts/qppjs.py clean test --quiet    先删除 build/，再静默运行 UT\n\n"
            "兼容入口仍可使用: ./scripts/run_ut.sh --quiet、./scripts/coverage.sh --open 等。"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    subparsers = parser.add_subparsers(title="子命令", dest="command", required=True)

    subparsers.add_parser(
        "clean",
        help="删除 build/ 目录；也可作为前置动作，例如 clean build release",
        description=(
            "删除整个 build/ 目录，包括 debug、release、test 和覆盖率产物。\n\n"
            "可单独使用:\n"
            "  python3 scripts/qppjs.py clean\n\n"
            "也可作为前置动作:\n"
            "  python3 scripts/qppjs.py clean build release\n"
            "  python3 scripts/qppjs.py clean test --quiet\n"
            "  python3 scripts/qppjs.py clean coverage --open"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )

    build_parser = subparsers.add_parser(
        "build",
        help="配置并构建指定类型",
        description=(
            "配置并构建指定类型。\n\n"
            "构建类型:\n"
            "  debug    build/debug：Debug + QPPJS_BUILD_TESTS=ON + QPPJS_ENABLE_ASAN=ON\n"
            "  release  build/release：Release + QPPJS_BUILD_TESTS=OFF\n"
            "  test     build/test：Debug + QPPJS_BUILD_TESTS=ON + QPPJS_ENABLE_COVERAGE=ON"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    build_parser.add_argument("config", choices=["debug", "release", "test"], help="要构建的配置类型")

    test_parser = subparsers.add_parser(
        "test",
        help="构建 Debug 并运行单元测试",
        description=(
            "先构建 build/debug，然后运行 ctest。\n"
            "默认排除 qppjs_cli_ 开头的 CLI 测试；macOS 上会开启 LSan；--quiet 会区分成功和失败日志。"
        ),
    )
    test_parser.add_argument("--clean", action="store_true", help="构建前先删除整个 build/ 目录")
    test_parser.add_argument(
        "--quiet",
        action="store_true",
        help="静默构建和测试；成功日志写入 build/debug/run_ut_success.log，失败详情写入 build/debug/run_ut_failure.log，构建日志写入同目录下的 run_ut_build_success.log / run_ut_build_failure.log",
    )

    coverage_parser = subparsers.add_parser(
        "coverage",
        help="生成 HTML 覆盖率报告",
        description=(
            "先构建 build/test，然后运行 ctest、收集覆盖率数据并生成 HTML 报告。\n"
            "报告默认输出到 build/test/coverage/index.html；--quiet 会区分成功和失败日志。"
        ),
    )
    coverage_parser.add_argument("--clean", action="store_true", help="构建前先删除整个 build/ 目录")
    coverage_parser.add_argument(
        "--quiet",
        action="store_true",
        help="静默构建、测试和报告生成；成功日志写入 build/coverage_success.log，失败时仅将失败 UT 摘要写入 build/coverage_failure.log",
    )
    coverage_parser.add_argument("--open", action="store_true", help="生成报告后用系统浏览器打开 index.html")

    return parser


def main(argv: list[str]) -> int:
    paths = ProjectPaths.discover()
    runner = Runner()
    builder = Builder(paths, runner)
    parser = make_parser()
    clean_first = len(argv) > 1 and argv[0] == "clean" and argv[1] in {"build", "test", "coverage"}
    args = parser.parse_args(argv[1:] if clean_first else argv)

    try:
        if args.command == "clean":
            clean(paths)
        elif args.command == "build":
            if clean_first:
                clean(paths)
            builder.build(args.config)
        elif args.command == "test":
            TestRunner(paths, runner, builder).run(clean_first=clean_first or args.clean, quiet=args.quiet)
        elif args.command == "coverage":
            CoverageRunner(paths, runner, builder).run(
                clean_first=clean_first or args.clean,
                open_report=args.open,
                quiet=args.quiet,
            )
        else:
            parser.error("missing command")
    except CommandError as exc:
        return exc.returncode
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
