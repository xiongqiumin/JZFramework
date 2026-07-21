#!/usr/bin/env python3
"""
读取 .qrc 文件，生成 C++ 资源注册文件。
用法: python jz_resources.py input.qrc -o output.cpp
"""

import argparse
import os
import sys
import xml.etree.ElementTree as ET


def parse_qrc(qrc_path):
    """解析 .qrc 文件，返回 [(alias_name, file_path, prefix), ...]"""
    tree = ET.parse(qrc_path)
    root = tree.getroot()
    base_dir = os.path.dirname(os.path.abspath(qrc_path))

    resources = []
    for qresource in root.findall("qresource"):
        prefix = qresource.get("prefix", "/")
        for file_elem in qresource.findall("file"):
            rel_path = file_elem.text.strip()
            alias = file_elem.get("alias")
            if alias is None:
                alias = prefix.rstrip("/") + "/" + rel_path.lstrip("/")
            else:
                alias = prefix.rstrip("/") + "/" + alias.lstrip("/")

            if not is_valid_path(alias):
                print(f"Error: invalid resource path: {alias}", file=sys.stderr)
                sys.exit(1)

            abs_path = os.path.join(base_dir, rel_path)
            if not os.path.exists(abs_path):
                print(f"Warning: file not found: {abs_path}", file=sys.stderr)
                continue
            resources.append((alias, abs_path, prefix))
    return resources


def is_valid_path(name):
    """检查是否为合法 Linux 路径：无空格、无引号、无控制字符"""
    for ch in name:
        if ch.isspace() or ch in ('"', "'", '\\', '*', '?', '<', '>', '|'):
            return False
    return True


def c_array_name(alias, index):
    """生成合法的 C++ 数组名"""
    safe = ''.join(ch if ch.isalnum() or ch == '_' else '_' for ch in alias)
    return f"_jzres_{safe}_{index}"


def write_depfile(depfile_path, output_path, qrc_path, resources):
    """生成 Makefile/Ninja 依赖文件（统一用正斜杠）"""
    def _norm(p):
        return os.path.abspath(p).replace('\\', '/')
    deps = [_norm(qrc_path)]
    for _, abs_path, _ in resources:
        deps.append(_norm(abs_path))
    dep_content = f"{_norm(output_path)}: {' '.join(deps)}\n"
    with open(depfile_path, "w") as f:
        f.write(dep_content)


def safe_identifier(prefix):
    """将 qrc 的 prefix 转换为合法的 C++ 标识符 (用于 jzres_xxx_init 的函数名)"""
    s = prefix.strip()
    if s.startswith('/'):
        s = s[1:]
    safe = ''.join(ch if ch.isalnum() or ch == '_' else '_' for ch in s)
    if safe and safe[0].isdigit():
        safe = '_' + safe
    return safe


def validate_prefix(prefix):
    """校验 prefix 合法性: 作为资源前缀与 jzres_<prefix>_init 的函数名。

    返回 (ok, info): ok 为 True 时 info 为安全的标识符; 否则 info 为错误说明。
    """
    if prefix is None:
        return False, "prefix attribute is missing"
    p = prefix.strip()
    s = p[1:] if p.startswith('/') else p
    if s == "":
        return False, "empty prefix (prefix must be a non-empty segment, e.g. prefix=\"/images\")"
    if '/' in s:
        return False, f"prefix '{prefix}' must be a single segment (no internal '/')"
    if not all(ch.isalnum() or ch == '_' for ch in s):
        return False, f"prefix '{prefix}' contains invalid characters; only [A-Za-z0-9_] are allowed"
    if s[0].isdigit():
        return False, f"prefix '{prefix}' must not start with a digit"
    return True, safe_identifier(prefix)


def generate_cpp(resources, qrc_path, output_path, depfile=None):
    """生成 C++ 源文件和头文件。一个 .qrc 只允许一个资源组(prefix), 生成单个 jzres_<prefix>_init()"""
    qrc_dir = os.path.dirname(os.path.abspath(output_path))
    os.makedirs(qrc_dir, exist_ok=True)

    header_path = os.path.splitext(output_path)[0] + ".h"
    header_name = os.path.basename(header_path)
    guard = "JZRES_" + '_'.join(ch.upper() if ch.isalnum() else '_' for ch in os.path.splitext(os.path.basename(output_path))[0]) + "_H_"

    # === 校验 prefix 合法性, 且一个 .qrc 只允许一个资源组 ===
    prefix_errors = []
    distinct = {}   # safe -> prefix
    for alias, abs_path, prefix in resources:
        ok, info = validate_prefix(prefix)
        if not ok:
            prefix_errors.append(f"  prefix '{prefix}': {info}")
            continue
        safe = info
        if safe in distinct:
            if distinct[safe] != prefix:
                prefix_errors.append(
                    f"  prefix '{prefix}' and '{distinct[safe]}' "
                    f"both map to 'jzres_{safe}_init' (conflict)")
        else:
            distinct[safe] = prefix

    if not prefix_errors and len(distinct) != 1:
        found = ", ".join(f"'{p}'" for p in distinct.values())
        prefix_errors.append(
            f"  a .qrc must contain exactly one resource group (prefix); found {len(distinct)}: {found}")

    if prefix_errors:
        print("Error: invalid or conflicting resource prefixes:", file=sys.stderr)
        for e in prefix_errors:
            print(e, file=sys.stderr)
        sys.exit(1)

    safe = next(iter(distinct))
    group_prefix = distinct[safe]

    # === 读取文件内容并生成 C 数组 ===
    entries = []
    for i, (alias, abs_path, _) in enumerate(resources):
        with open(abs_path, "rb") as f:
            data = f.read()
        size = len(data)
        arr_name = c_array_name(alias, i)
        entries.append((alias, arr_name, size, data))

    # === 生成头文件 ===
    hlines = []
    hlines.append("// Auto-generated by jz_resources.py. DO NOT EDIT.")
    hlines.append(f"#ifndef {guard}")
    hlines.append(f"#define {guard}")
    hlines.append("")
    hlines.append(f"bool jzres_{safe}_init();")
    hlines.append("")
    hlines.append(f"#endif // {guard}")
    hlines.append("")

    with open(header_path, "w", encoding="utf-8") as f:
        f.write("\n".join(hlines))

    # === 生成源文件 ===
    lines = []
    lines.append("// Auto-generated by jz_resources.py. DO NOT EDIT.")
    lines.append('#include "JZResources.h"')
    lines.append(f'#include "{header_name}"')
    lines.append("")

    for alias, arr_name, size, data in entries:
        lines.append(f"// {alias}")
        lines.append(f"static const unsigned char {arr_name}[] = {{")
        for j in range(0, size, 20):
            chunk = data[j:j+20]
            hex_list = [f"0x{b:02x}" for b in chunk]
            lines.append("    " + ", ".join(hex_list) + ",")
        lines.append("};")
        lines.append("")

    lines.append("// resource init")
    lines.append("namespace {")
    lines.append("static bool _jz_resources_inited_ = false;")
    lines.append("")
    lines.append("static void _jz_resources_register_() {")
    lines.append("    if (_jz_resources_inited_) return;")
    lines.append("    _jz_resources_inited_ = true;")
    for alias, arr_name, size, data in entries:
        lines.append(f'    JZResources::instance()->add(":{alias}", {arr_name}, {size});')
    lines.append("}")
    lines.append("} // namespace")
    lines.append("")
    lines.append(f"bool jzres_{safe}_init() {{")
    lines.append("    if (_jz_resources_inited_) return false;")
    lines.append("    _jz_resources_register_();")
    lines.append("    return true;")
    lines.append("}")
    lines.append("")

    with open(output_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    if depfile:
        write_depfile(depfile, output_path, qrc_path, resources)

    print(f"Generated: {header_path}")
    print(f"Generated: {output_path} ({len(entries)} resources, group '{group_prefix}')")


def main():
    parser = argparse.ArgumentParser(description="Generate C++ resource file from .qrc")
    parser.add_argument("qrc", help="Input .qrc file")
    parser.add_argument("-o", "--output", required=True, help="Output C++ file")
    parser.add_argument("-d", "--depfile", default=None, help="Output dependency file (.d)")
    args = parser.parse_args()

    resources = parse_qrc(args.qrc)
    if not resources:
        print("No resources found.", file=sys.stderr)
        sys.exit(1)

    generate_cpp(resources, args.qrc, args.output, depfile=args.depfile)


if __name__ == "__main__":
    main()
