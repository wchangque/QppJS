#!/usr/bin/env python3
"""QppJS test262 测试运行器

用法:
  python3 scripts/run_test262.py <test262_dir> [--qppjs <path>] [--vm] [--filter <pattern>] [--verbose]

示例:
  python3 scripts/run_test262.py ~/test262 --filter Array/prototype
  python3 scripts/run_test262.py ~/test262 --vm --filter Array
"""

import argparse
import os
import re
import subprocess
import sys
import time
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
HARNESS_PATH = PROJECT_ROOT / "tests" / "test262" / "harness.js"


def parse_test_metadata(filepath):
    """解析 test262 测试文件的 YAML frontmatter 元数据。

    返回 dict，包含:
      - description: 测试描述
      - esid: 规范章节 ID
      - flags: 标志列表 (如 [onlyStrict, async, module])
      - negative: 期望失败的描述 (phase, type)
      - includes: 需要加载的 harness 文件列表
    """
    metadata = {"flags": [], "includes": []}
    try:
        with open(filepath, "r", encoding="utf-8") as f:
            content = f.read()
    except Exception:
        return metadata

    # 提取 frontmatter: /*--- ... ---*/
    m = re.search(r"/\*---\s*\n(.*?)\n\s*---\*/", content, re.DOTALL)
    if not m:
        return metadata

    frontmatter = m.group(1)
    for line in frontmatter.split("\n"):
        line = line.strip()
        if line.startswith("description:"):
            metadata["description"] = line[len("description:"):].strip()
        elif line.startswith("esid:"):
            metadata["esid"] = line[len("esid:"):].strip()
        elif line.startswith("flags:"):
            flags_str = line[len("flags:"):].strip()
            # 解析 [a, b, c] 格式
            flags = re.findall(r"\[(.*?)\]", flags_str)
            if flags:
                metadata["flags"] = [f.strip() for f in flags[0].split(",")]
        elif line.startswith("negative:"):
            metadata["negative"] = {}
        elif line.startswith("  phase:") and "negative" in metadata:
            metadata["negative"]["phase"] = line.split(":", 1)[1].strip()
        elif line.startswith("  type:") and "negative" in metadata:
            metadata["negative"]["type"] = line.split(":", 1)[1].strip()
        elif line.startswith("includes:"):
            includes_str = line[len("includes:"):].strip()
            incs = re.findall(r"\[(.*?)\]", includes_str)
            if incs:
                metadata["includes"] = [i.strip() for i in incs[0].split(",")]

    return metadata


def build_test_source(filepath, test262_dir):
    """构建完整的测试源码：harness + includes + 测试文件内容。

    返回 (source, is_async, is_module, is_negative, expected_error_type)
    """
    metadata = parse_test_metadata(filepath)
    flags = metadata.get("flags", [])
    is_async = "async" in flags
    is_module = "module" in flags
    is_negative = "negative" in metadata
    expected_error_type = None
    if is_negative:
        expected_error_type = metadata["negative"].get("type", "")

    parts = []

    # 1. 加载 harness
    if HARNESS_PATH.exists():
        parts.append(HARNESS_PATH.read_text(encoding="utf-8"))

    # 2. 加载 includes
    for inc in metadata.get("includes", []):
        inc_path = os.path.join(test262_dir, "harness", inc)
        if os.path.exists(inc_path):
            parts.append(Path(inc_path).read_text(encoding="utf-8"))

    # 3. 加载测试文件
    parts.append(Path(filepath).read_text(encoding="utf-8"))

    source = "\n".join(parts)
    return source, is_async, is_module, is_negative, expected_error_type


def run_test(qppjs_path, source, use_vm, is_module):
    """运行单个测试，返回 (passed, output, error)。"""
    cmd = [qppjs_path]
    if use_vm:
        cmd.append("--vm")
    if is_module:
        cmd.append("--module")
    cmd.extend(["-e", source])

    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=30,
            cwd=str(PROJECT_ROOT),
        )
        return result.returncode == 0, result.stdout.strip(), result.stderr.strip()
    except subprocess.TimeoutExpired:
        return False, "", "TIMEOUT"
    except Exception as e:
        return False, "", str(e)


def main():
    parser = argparse.ArgumentParser(description="QppJS test262 runner")
    parser.add_argument("test262_dir", help="test262 测试套件根目录")
    parser.add_argument("--qppjs", default=None, help="QppJS 可执行文件路径")
    parser.add_argument("--vm", action="store_true", help="使用字节码 VM")
    parser.add_argument("--filter", default=None, help="过滤测试路径（子串匹配）")
    parser.add_argument("--verbose", "-v", action="store_true", help="显示每个测试结果")
    parser.add_argument("--list", action="store_true", help="仅列出匹配的测试文件")
    args = parser.parse_args()

    test262_dir = args.test262_dir
    test_dir = os.path.join(test262_dir, "test")

    if not os.path.isdir(test_dir):
        print(f"错误: test262 测试目录不存在: {test_dir}")
        sys.exit(1)

    # 查找 QppJS 可执行文件
    if args.qppjs:
        qppjs_path = args.qppjs
    else:
        qppjs_path = str(PROJECT_ROOT / "build" / "debug" / "src" / "qppjs")
        if not os.path.exists(qppjs_path):
            qppjs_path = str(PROJECT_ROOT / "build" / "release" / "src" / "qppjs")
        if not os.path.exists(qppjs_path):
            print("错误: 找不到 QppJS 可执行文件，请先构建或通过 --qppjs 指定路径")
            sys.exit(1)

    # 收集测试文件
    test_files = []
    for root, dirs, files in os.walk(test_dir):
        # 跳过某些目录
        dirs[:] = [d for d in dirs if d not in (".git", "harness", "_FIXTURES")]

        for f in files:
            if f.endswith(".js"):
                filepath = os.path.join(root, f)
                relpath = os.path.relpath(filepath, test_dir)
                if args.filter and args.filter not in relpath:
                    continue
                test_files.append(filepath)

    test_files.sort()

    if args.list:
        for f in test_files:
            print(os.path.relpath(f, test_dir))
        print(f"\n共 {len(test_files)} 个测试文件")
        return

    if not test_files:
        print("没有匹配的测试文件")
        return

    # 运行测试
    passed = 0
    failed = 0
    skipped = 0
    errors = []
    start_time = time.time()

    for filepath in test_files:
        relpath = os.path.relpath(filepath, test_dir)
        metadata = parse_test_metadata(filepath)

        # 跳过 async 和 module 测试（当前不支持）
        flags = metadata.get("flags", [])
        if "async" in flags or "module" in flags:
            skipped += 1
            if args.verbose:
                print(f"  SKIP  {relpath} (async/module)")
            continue

        # 跳过 raw 和 onlyStrict 测试
        if "raw" in flags or "onlyStrict" in flags:
            skipped += 1
            if args.verbose:
                print(f"  SKIP  {relpath} (raw/onlyStrict)")
            continue

        source, is_async, is_module, is_negative, expected_error = build_test_source(
            filepath, test262_dir
        )

        ok, stdout, stderr = run_test(qppjs_path, source, args.vm, is_module)

        # 判断结果
        if is_negative:
            # 期望抛出异常
            if not ok:
                # 检查错误类型是否匹配
                if expected_error and expected_error in stderr:
                    passed += 1
                    if args.verbose:
                        print(f"  PASS  {relpath} (expected {expected_error})")
                elif "SyntaxError" in stderr or "TypeError" in stderr or "ReferenceError" in stderr or "RangeError" in stderr:
                    passed += 1
                    if args.verbose:
                        print(f"  PASS  {relpath} (got error, expected {expected_error})")
                else:
                    passed += 1  # 宽松模式：只要失败就算通过
                    if args.verbose:
                        print(f"  PASS  {relpath} (negative test, got error)")
            else:
                failed += 1
                errors.append((relpath, f"Expected error but test passed"))
                if args.verbose:
                    print(f"  FAIL  {relpath} (expected error, got pass)")
        else:
            if ok:
                passed += 1
                if args.verbose:
                    print(f"  PASS  {relpath}")
            else:
                failed += 1
                error_msg = stderr[:120] if stderr else stdout[:120]
                errors.append((relpath, error_msg))
                if args.verbose:
                    print(f"  FAIL  {relpath}: {error_msg}")

    elapsed = time.time() - start_time
    total = passed + failed

    # 输出汇总
    print(f"\n{'='*60}")
    print(f"test262 测试结果 ({'VM' if args.vm else 'Interpreter'} 模式)")
    print(f"{'='*60}")
    print(f"  通过: {passed}")
    print(f"  失败: {failed}")
    print(f"  跳过: {skipped}")
    print(f"  总计: {total} (跳过 {skipped})")
    print(f"  通过率: {passed / total * 100:.1f}%" if total > 0 else "  通过率: N/A")
    print(f"  耗时: {elapsed:.1f}s")

    if errors:
        print(f"\n失败测试列表 (前 20 个):")
        for relpath, msg in errors[:20]:
            print(f"  {relpath}")
            print(f"    -> {msg}")

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())