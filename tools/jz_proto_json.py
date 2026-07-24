#!/usr/bin/env python3
"""
根据类 protobuf 的 .proto 文件生成 C++ 序列化代码 (基于 JZJsonStream)。

特性:
  * 生成 struct + operator<< / operator>> (JZJsonStream), 支持 Json 读写。
  * proto 中的数据类型不做任何限制 / 转换, 原样透传为 C++ 类型 (例如
    `string` / `std::string` / `int32` / `MyType` 等均按字面用作成员类型)。
  * `optional` 字段使用 JZOptional<T>。
  * `repeated` 字段使用 std::vector<T>。
  * `map<K, V>` 字段使用 std::map<K, V>。
  * 支持顶层 / 嵌套 message 与 enum (嵌套类型扁平化为 `<Parent>_<Child>`)。
  * 不支持 proto 的 import / include (单文件内聚)。

用法:
  python jz_proto_json.py input.proto -o output_pb.cpp [-d depfile.d]

生成的 header 与 cpp 同名 (output_pb.h / output_pb.cpp), 置于 -o 所在目录。
"""

import argparse
import os
import re
import sys

# ====================== 词法分析 ======================
_TOKEN_RE = re.compile(
    r"""
      (?P<COMMENT_LINE>//[^\n]*)
    | (?P<COMMENT_BLOCK>/\*.*?\*/)
    | (?P<STRING>"(?:[^"\\]|\\.)*")
    | (?P<SCOPE>::)
    | (?P<LBRACE>\{)
    | (?P<RBRACE>\})
    | (?P<LBRACKET>\[)
    | (?P<RBRACKET>\])
    | (?P<LPAREN>\()
    | (?P<RPAREN>\))
    | (?P<SEMI>;)
    | (?P<COMMA>,)
    | (?P<EQ>=)
    | (?P<LT><)
    | (?P<GT>>)
    | (?P<DOTIDENT>[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)+)
    | (?P<IDENT>[A-Za-z_][A-Za-z0-9_]*)
    | (?P<NUMBER>\d+)
    | (?P<SKIP>\s+)
    | (?P<DOT>\.)
    """,
    re.VERBOSE,
)


class Token:
    __slots__ = ("type", "text")

    def __init__(self, ttype, text):
        self.type = ttype
        self.text = text

    def __repr__(self):
        return f"Token({self.type}, {self.text!r})"


def tokenize(text):
    tokens = []
    pos = 0
    while pos < len(text):
        m = _TOKEN_RE.match(text, pos)
        if not m:
            raise SyntaxError(f"无法识别的字符: {text[pos]!r} (位置 {pos})")
        pos = m.end()
        kind = m.lastgroup
        val = m.group()
        if kind in ("COMMENT_LINE", "COMMENT_BLOCK", "SKIP", "STRING"):
            # 字符串字面量目前不需要, 注释 / 空白直接丢弃
            continue
        if kind == "DOT":
            # 孤立的点 (理论上不该出现在 proto 中), 跳过
            continue
        tokens.append(Token(kind, val))
    return tokens


# ====================== 语法分析 ======================
class ProtoError(Exception):
    pass


def _expect(tokens, i, ttype):
    if i >= len(tokens) or tokens[i].type != ttype:
        got = tokens[i].text if i < len(tokens) else "<EOF>"
        raise ProtoError(f"期望 '{ttype}' 但得到 '{got}'")
    return tokens[i]


def _skip_options(tokens, i):
    """跳过形如 [ ... ] 的字段选项。"""
    if i < len(tokens) and tokens[i].type == "LBRACKET":
        depth = 1
        i += 1
        while i < len(tokens) and depth > 0:
            if tokens[i].type == "LBRACKET":
                depth += 1
            elif tokens[i].type == "RBRACKET":
                depth -= 1
            i += 1
        if depth != 0:
            raise ProtoError("字段选项括号不匹配")
    return i


def parse_type(tokens, i):
    """解析一个 C++ 类型表达式 (支持 :: 作用域与 <...> 模板), 返回 (类型文本, 新下标)。"""
    t = tokens[i]
    if t.type not in ("IDENT", "DOTIDENT"):
        raise ProtoError(f"期望类型标识符, 但得到 '{t.text}'")
    # 嵌套引用 Outer.Inner 扁平化为 Outer_Inner
    parts = [t.text.replace(".", "_")]
    i += 1

    # 处理 :: 作用域连接
    while i < len(tokens) and tokens[i].type == "SCOPE":
        parts.append("::")
        i += 1
        nt = tokens[i]
        if nt.type not in ("IDENT", "DOTIDENT"):
            raise ProtoError(f"'::' 之后期望标识符, 但得到 '{nt.text}'")
        parts.append(nt.text.replace(".", "_"))
        i += 1

    # 处理模板参数
    if i < len(tokens) and tokens[i].type == "LT":
        parts.append("<")
        i += 1
        depth = 1
        first = True
        while i < len(tokens) and depth > 0:
            ct = tokens[i]
            if ct.type == "LT":
                depth += 1
                sub, i = parse_type(tokens, i)
                parts.append(sub)
            elif ct.type == "GT":
                depth -= 1
                if depth == 0:
                    parts.append(">")
                    i += 1
                    break
                parts.append(">")
                i += 1
            elif ct.type == "COMMA":
                parts.append(", ")
                i += 1
                first = True
            else:
                if not first:
                    parts.append(" ")
                sub, i = parse_type(tokens, i)
                parts.append(sub)
                first = False
        # 模板之后可能还有 ::Nested<...>
        while i < len(tokens) and tokens[i].type == "SCOPE":
            parts.append("::")
            i += 1
            nt = tokens[i]
            parts.append(nt.text.replace(".", "_"))
            i += 1
            if i < len(tokens) and tokens[i].type == "LT":
                sub, i = parse_type(tokens, i)
                parts.append(sub)
    return "".join(parts), i


def parse_field(tokens, i):
    """解析一个字段, 返回 (field_dict, 新下标)。"""
    repeated = False
    optional = False
    t = tokens[i]
    if t.type == "IDENT" and t.text == "repeated":
        repeated = True
        i += 1
    elif t.type == "IDENT" and t.text == "optional":
        optional = True
        i += 1

    t = tokens[i]
    # map<K, V>
    if t.type == "IDENT" and t.text == "map":
        i += 1
        _expect(tokens, i, "LT")
        i += 1
        key_type, i = parse_type(tokens, i)
        _expect(tokens, i, "COMMA")
        i += 1
        val_type, i = parse_type(tokens, i)
        _expect(tokens, i, "GT")
        i += 1
        name = _expect(tokens, i, "IDENT").text
        i += 1
        _expect(tokens, i, "EQ")
        i += 1
        tag = _expect(tokens, i, "NUMBER").text
        i += 1
        i = _skip_options(tokens, i)
        _expect(tokens, i, "SEMI")
        i += 1
        cpp_type = f"std::map<{key_type}, {val_type}>"
        return {
            "qualifier": "map",
            "name": name,
            "cpp_type": cpp_type,
            "elem_type": val_type,
            "tag": tag,
        }, i

    elem_type, i = parse_type(tokens, i)
    name = _expect(tokens, i, "IDENT").text
    i += 1
    _expect(tokens, i, "EQ")
    i += 1
    tag = _expect(tokens, i, "NUMBER").text
    i += 1
    i = _skip_options(tokens, i)
    _expect(tokens, i, "SEMI")
    i += 1

    if repeated:
        cpp_type = f"std::vector<{elem_type}>"
        qual = "repeated"
    elif optional:
        cpp_type = f"JZOptional<{elem_type}>"
        qual = "optional"
    else:
        cpp_type = elem_type
        qual = "plain"

    return {
        "qualifier": qual,
        "name": name,
        "cpp_type": cpp_type,
        "elem_type": elem_type,
        "tag": tag,
    }, i


def parse_body(tokens, i, scope, messages, enums):
    """解析一个 { ... } 内部, 返回 (当前作用域下的字段列表, 新下标)。"""
    fields = []
    while i < len(tokens) and tokens[i].type != "RBRACE":
        t = tokens[i]
        if t.type == "IDENT":
            if t.text == "message":
                i += 1
                name = _expect(tokens, i, "IDENT").text
                i += 1
                _expect(tokens, i, "LBRACE")
                i += 1
                sub_fields, i = parse_body(
                    tokens, i, scope + [name], messages, enums
                )
                _expect(tokens, i, "RBRACE")
                i += 1
                full = "_".join(scope + [name]) if scope else name
                messages.append({"name": full, "fields": sub_fields})
                continue
            elif t.text == "enum":
                i += 1
                name = _expect(tokens, i, "IDENT").text
                i += 1
                _expect(tokens, i, "LBRACE")
                i += 1
                values = []
                while i < len(tokens) and tokens[i].type != "RBRACE":
                    vname = _expect(tokens, i, "IDENT").text
                    i += 1
                    _expect(tokens, i, "EQ")
                    i += 1
                    vnum = _expect(tokens, i, "NUMBER").text
                    i += 1
                    if i < len(tokens) and tokens[i].type == "COMMA":
                        i += 1
                    if i < len(tokens) and tokens[i].type == "SEMI":
                        i += 1
                    values.append((vname, vnum))
                _expect(tokens, i, "RBRACE")
                i += 1
                full = "_".join(scope + [name]) if scope else name
                enums.append({"name": full, "values": values})
                continue
            elif t.text in ("syntax", "package", "import"):
                # 单文件内聚, import 等语句直接忽略
                while i < len(tokens) and tokens[i].type != "SEMI":
                    i += 1
                i += 1
                continue
            elif t.text == "option":
                while i < len(tokens) and tokens[i].type != "SEMI":
                    i += 1
                i += 1
                continue
            else:
                field, i = parse_field(tokens, i)
                fields.append(field)
                continue
        elif t.type == "SEMI":
            # 多余的分号, 跳过
            i += 1
            continue
        else:
            raise ProtoError(f"无法解析的 token: {t}")
    return fields, i


def parse_proto(text):
    tokens = tokenize(text)
    messages = []
    enums = []
    _, _ = parse_body(tokens, 0, [], messages, enums)

    message_names = {m["name"] for m in messages}
    # 计算消息依赖 (用于拓扑排序, 保证被引用的 struct 先定义)
    for m in messages:
        deps = set()
        for f in m["fields"]:
            if f["qualifier"] == "map":
                candidates = [f["elem_type"], f.get("key_type")]
            else:
                candidates = [f["elem_type"]]
            for c in candidates:
                if c is None:
                    continue
                base = c.split("<", 1)[0]
                if base in message_names and base != m["name"]:
                    deps.add(base)
        m["depends_on"] = deps

    ordered = _topo_sort(messages)
    return enums, ordered


def _topo_sort(messages):
    by_name = {m["name"]: m for m in messages}
    order = []
    state = {}  # 0=遍历中, 1=完成

    def visit(name):
        st = state.get(name, 2)  # 2=未访问, 0=遍历中, 1=完成
        if st == 1:
            return
        if st == 0:
            raise ProtoError(f"检测到循环依赖: {name}")
        state[name] = 0
        for dep in by_name[name]["depends_on"]:
            if dep in by_name:
                visit(dep)
        state[name] = 1
        order.append(name)

    for m in messages:
        visit(m["name"])
    return [by_name[n] for n in order]


# ====================== 代码生成 ======================
def _guard_name(header_path):
    base = os.path.splitext(os.path.basename(header_path))[0]
    return "JZ_PROTO_" + "".join(
        c if c.isalnum() else "_" for c in base
    ).upper() + "_H_"


def _enum_code(e):
    name = e["name"]
    value_lines = ",\n    ".join(f"{v} = {n}" for v, n in e["values"])
    return f"""enum class {name} : int
{{
    {value_lines}
}};

inline void operator>>(JZJsonStream& s, {name}& t)
{{
    int v = 0;
    s >> v;
    if (!s.isError())
        t = static_cast<{name}>(v);
}}

inline void operator<<(JZJsonStream& s, const {name}& t)
{{
    s << static_cast<int>(t);
}}
"""


def _message_code(m):
    name = m["name"]
    members = []
    read_lines = []
    write_lines = []
    for f in m["fields"]:
        members.append(f"    {f['cpp_type']} {f['name']};")
        jn = f["name"]
        if f["qualifier"] == "optional":
            read_lines.append(f'    s.readMemberOptional("{jn}", t.{f["name"]});')
            write_lines.append(f"    if (t.{f['name']}.isSome())")
            write_lines.append("    {")
            write_lines.append(
                f'        s.writeMember("{jn}", t.{f["name"]}.value());'
            )
            write_lines.append("    }")
        else:
            read_lines.append(f'    s.readMember("{jn}", t.{f["name"]});')
            write_lines.append(f'    s.writeMember("{jn}", t.{f["name"]});')

    member_block = "\n".join(members) if members else "    // (无字段)"
    read_block = "\n".join(read_lines) if read_lines else "    // (无字段)"
    write_block = "\n".join(write_lines) if write_lines else "    // (无字段)"

    return f"""struct {name}
{{
{member_block}

    bool fromJson(const Json::Value& obj, std::string& errMsg)
    {{
        JZJsonStream s;
        s.open((Json::Value*)&obj);
        s >> *this;
        if (s.isError())
        {{
            errMsg = s.errorString();
            return false;
        }}
        return true;
    }}

    Json::Value toJson() const
    {{
        Json::Value obj;
        JZJsonStream s;
        s.open(&obj);
        s << *this;
        return obj;
    }}
}};

void operator>>(JZJsonStream& s, {name}& t)
{{
{read_block}
}}

void operator<<(JZJsonStream& s, const {name}& t)
{{
{write_block}
}}
"""


def generate(enums, messages, proto_path, header_path, cpp_path):
    guard = _guard_name(header_path)
    hparts = []
    hparts.append("// Auto-generated by jz_proto_json.py. DO NOT EDIT.")
    hparts.append(f"// source: {os.path.basename(proto_path)}")
    hparts.append("")
    hparts.append(f"#ifndef {guard}")
    hparts.append(f"#define {guard}")
    hparts.append("")
    hparts.append('#include "JZJsonStream.h"')
    hparts.append("#include <string>")
    hparts.append("#include <vector>")
    hparts.append("#include <map>")
    hparts.append("")

    # forward declarations
    fwd = []
    for e in enums:
        fwd.append(f"enum class {e['name']} : int;")
    for m in messages:
        fwd.append(f"struct {m['name']};")
    if fwd:
        hparts.append("// ---- forward declarations ----")
        hparts.extend(fwd)
        hparts.append("")

    # enums (must precede messages: a message may use an enum as a member type)
    if enums:
        hparts.append("// ---- enums ----")
        for e in enums:
            hparts.append(_enum_code(e))
            hparts.append("")

    # messages (topological order)
    if messages:
        hparts.append("// ---- messages ----")
        for m in messages:
            hparts.append(f"// message {m['name']}")
            hparts.append("void operator>>(JZJsonStream& s, %s& t);" % m["name"])
            hparts.append(
                "void operator<<(JZJsonStream& s, const %s& t);" % m["name"]
            )
            hparts.append("")
            hparts.append(_message_code(m))
            hparts.append("")

    hparts.append(f"#endif // {guard}")
    hparts.append("")

    header = "\n".join(hparts)

    cpp = (
        "// Auto-generated by jz_proto_json.py. DO NOT EDIT.\n"
        f'#include "{os.path.basename(header_path)}"\n'
    )

    os.makedirs(os.path.dirname(os.path.abspath(header_path)), exist_ok=True)
    with open(header_path, "w", encoding="utf-8") as f:
        f.write(header)
    with open(cpp_path, "w", encoding="utf-8") as f:
        f.write(cpp)

    return header_path, cpp_path


def write_depfile(depfile_path, cpp_path, header_path, proto_path):
    if not depfile_path:
        return
    norm = lambda p: os.path.abspath(p).replace("\\", "/")
    lines = [f"{norm(cpp_path)}: {norm(proto_path)}"]
    if header_path:
        lines.append(f"{norm(header_path)}: {norm(proto_path)}")
    with open(depfile_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


# ====================== 入口 ======================
def main():
    parser = argparse.ArgumentParser(
        description="根据 .proto 生成基于 JZJsonStream 的 C++ 序列化代码"
    )
    parser.add_argument("proto", help="输入的 .proto 文件")
    parser.add_argument(
        "-o", "--output", required=True, help="输出 .cpp 路径 (同名 .h 一并生成)"
    )
    parser.add_argument(
        "-d", "--depfile", default=None, help="输出依赖文件 (.d)"
    )
    args = parser.parse_args()

    if not os.path.exists(args.proto):
        print(f"Error: proto 文件不存在: {args.proto}", file=sys.stderr)
        sys.exit(1)

    with open(args.proto, "r", encoding="utf-8") as f:
        text = f.read()

    try:
        enums, messages = parse_proto(text)
    except (ProtoError, SyntaxError) as ex:
        print(f"Error: 解析 proto 失败: {ex}", file=sys.stderr)
        sys.exit(1)

    if not enums and not messages:
        print("Warning: proto 文件未包含任何 message / enum", file=sys.stderr)

    out_cpp = os.path.abspath(args.output)
    out_h = os.path.splitext(out_cpp)[0] + ".h"

    generate(enums, messages, args.proto, out_h, out_cpp)
    write_depfile(args.depfile, out_cpp, out_h, args.proto)

    print(f"Generated: {out_h}")
    print(f"Generated: {out_cpp} ({len(messages)} messages, {len(enums)} enums)")


if __name__ == "__main__":
    main()
