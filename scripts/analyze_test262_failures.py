#!/usr/bin/env python3
"""分析 test262 失败原因，按错误类型分类"""

import os, re, sys
from collections import Counter, defaultdict
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
TEST262_DIR = PROJECT_ROOT / "tests" / "test262" / "test262" / "test"

# Post-ES2021 features to identify out-of-scope tests
POST_ES2021_FEATURES = {
    'Temporal', 'class-fields-public', 'class-fields-private', 'class-methods-private',
    'class-static-methods-private', 'class-static-fields-public', 'class-static-fields-private',
    'class-static-block', 'resizable-arraybuffer', 'iterator-helpers', 'explicit-resource-management',
    'Intl.Era-monthcode', 'error-cause', 'String.prototype.at', 'Array.prototype.at',
    'Object.hasOwn', 'regexp-match-indices', 'import-assertions',
    'json-modules', 'import-attributes', 'change-array-by-copy', 'array-grouping',
    'array-find-from-last', 'hashbang', 'shadow-realm', 'promise-try', 'Float16Array',
    'set-methods', 'map-groupby', 'promise-with-resolvers', 'regexp-v-flag',
    'regexp-duplicate-named-groups', 'arraybuffer-transfer', 'uint8array-to-base64',
    'uint8array-from-base64', 'Atomics.waitAsync', 'FinalizationRegistry',
    'WeakRef',
}

SYNTAX_PATTERNS = {
    'regex_literal': (r'(?<![a-zA-Z0-9_)\]}"\'])\s*/(?!/|\*)[^/]+/[gimsuvy]*', False),
    'arrow_function': (r'=>\s*\{', False),
    'template_literal': (r'`[^`]*`', False),
    'spread_operator': (r'\.\.\.\s*[a-zA-Z_]', False),
    'for_of': (r'for\s*\(.*\bof\b', False),
    'class_syntax': (r'\bclass\s+[A-Z]', False),
    'optional_chaining': (r'\?\.', False),
    'nullish_coalescing': (r'\?\?', False),
    'destructuring': (r'\[.*\]\s*=\s*', False),
    'generator': (r'\bfunction\s*\*\s*', False),
    'async_function': (r'\basync\s+function\b', True),  # we support partial async
}


def parse_frontmatter(content):
    m = re.search(r"/\*---\s*\n(.*?)\n\s*---\*/", content, re.DOTALL)
    if not m:
        return {}, []
    fm_text = m.group(1)
    metadata = {'features': [], 'includes': [], 'flags': []}
    for line in fm_text.split('\n'):
        line = line.strip()
        if line.startswith('features:'):
            feats = re.findall(r'\[(.*?)\]', line)
            if feats:
                metadata['features'] = [f.strip() for f in feats[0].split(',')]
        elif line.startswith('includes:'):
            incs = re.findall(r'\[(.*?)\]', line)
            if incs:
                metadata['includes'] = [i.strip() for i in incs[0].split(',')]
        elif line.startswith('flags:'):
            fl = re.findall(r'\[(.*?)\]', line)
            if fl:
                metadata['flags'] = [f.strip() for f in fl[0].split(',')]
    # Find used syntax patterns
    used_syntax = []
    for name, (pattern, _supported) in SYNTAX_PATTERNS.items():
        if re.search(pattern, content):
            used_syntax.append(name)
    return metadata, used_syntax


def is_post_es2021(metadata):
    return any(f in POST_ES2021_FEATURES for f in metadata.get('features', []))


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else '.'
    target_dir = TEST262_DIR
    if target != '.':
        target_dir = target_dir / target

    # Collect
    entries = []
    for root, dirs, files in os.walk(str(target_dir)):
        dirs[:] = [d for d in dirs if d not in ('.git', 'harness', '_FIXTURES')]
        for f in files:
            if f.endswith('.js'):
                path = os.path.join(root, f)
                relpath = os.path.relpath(path, str(TEST262_DIR))
                try:
                    with open(path) as fh:
                        content = fh.read()
                    meta, syntax = parse_frontmatter(content)
                except:
                    meta, syntax = {}, []
                entries.append({
                    'relpath': relpath,
                    'meta': meta,
                    'used_syntax': syntax,
                    'is_post_es2021': is_post_es2021(meta),
                })

    # Stats
    total = len(entries)
    post_es2021 = sum(1 for e in entries if e['is_post_es2021'])
    es2021_compat = total - post_es2021

    print(f"=== {target or 'built-ins'} ===")
    print(f"总测试数: {total}")
    print(f"Post-ES2021: {post_es2021} ({post_es2021/total*100:.1f}%)")
    print(f"ES2021 兼容: {es2021_compat} ({es2021_compat/total*100:.1f}%)")
    print()

    # Syntax usage in ES2021-compatible tests
    syntax_count = Counter()
    for e in entries:
        if not e['is_post_es2021']:
            for s in e['used_syntax']:
                syntax_count[s] += 1
    print("=== ES2021 兼容测试中使用的语法 (QppJS 不支持的部分) ===")
    for s, c in syntax_count.most_common():
        print(f"  {s}: {c} ({c/es2021_compat*100:.1f}%)")
    print()

    # Include dependencies
    include_count = Counter()
    for e in entries:
        for inc in e['meta'].get('includes', []):
            include_count[inc] += 1
    print("=== Harness include 依赖 (前 20) ===")
    for inc, c in include_count.most_common(20):
        print(f"  {inc}: {c}")

    # Flag distribution
    flag_count = Counter()
    for e in entries:
        for fl in e['meta'].get('flags', []):
            flag_count[fl] += 1
    print(f"\n=== Flag 分布 ===")
    for fl, c in flag_count.most_common():
        print(f"  {fl}: {c}")


if __name__ == '__main__':
    main()
