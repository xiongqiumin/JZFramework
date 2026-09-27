# -*- coding: utf-8 -*-
"""
给 JZFramework 框架自有头文件补齐 JZFRAMEWORK_API 导出宏, 为将来把框架编译成
动态库(DLL)做准备。默认静态库时该宏为空, 对现有编译零影响。

== 作用域(重要) ==
  * 仅处理框架模块目录(顶层 JZ* 目录)下, 文件名以 "JZ" 开头的 .h 文件。
    这两层过滤共同保证:
      - 框架模块(JZ* 目录)才会被编译进 JZFramework DLL;
      - binary/ test/ tools/ build/ skill/ cmake/ docs/ 等独立 target(可执行程序或
        静态库)的符号不在 DLL 内, 不会被误加导出宏(否则消费方会被误标 dllimport 而
        链接失败);
      - 第三方 vendored 头(mongoose / sqlite3 / json / miniz / quickjs / base64 /
        stb 等)因文件名不以 "JZ" 开头, 被文件名过滤自然排除, 无需黑名单。

== 导出规则 ==
  1. 名字以 "JZ" 开头的 class / struct 定义 -> 加 JZFRAMEWORK_API。
  2. 任意名字的 struct 定义, 只要位于"文件顶层"或"namespace 内"(非类/结构/枚举
     体内、非函数体内) -> 也加 JZFRAMEWORK_API。  (满足"struct 也补充"的需求)
  跳过: 前向声明(struct Foo;)、匿名 struct、类/结构内嵌套 struct、函数体内局部 struct。

== 用法 ==
  python jz_add_export.py              # dry-run, 打印将要修改的文件
  python jz_add_export.py --apply      # 实际写入
  python jz_add_export.py --apply JZNet  # 只处理某子目录(路径包含该字符串即可)
"""
import os, re, sys

# 仓库根目录 = 本脚本(tools/jz_add_export.py)的上一级
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 作用域由 main() 中的"文件名以 JZ 开头 + 仅 JZ* 模块目录"双重过滤决定:
#   * 文件名以 "JZ" 开头 -> 第三方 vendored 头(mongoose/sqlite3/json/miniz/quickjs/
#     base64/stb 等)因文件名不以 JZ 开头而自然被排除, 无需黑名单。
#   * 仅 JZ* 模块目录 -> binary/ test/ tools/ build/ 等独立 target 不被处理。

# class / struct 之后的标识符
KW_RE = re.compile(r'\b(class|struct)\s+([A-Za-z_]\w*)')

# ---------------------------------------------------------------------------
# 工具: 结构/嵌套分析
# ---------------------------------------------------------------------------
def strip_equal(s):
    """把注释/字符串替换成等长空格, 保持索引对齐。"""
    s = re.sub(r'/\*.*?\*/', lambda m: ' ' * len(m.group(0)), s, flags=re.S)
    s = re.sub(r'//[^\n]*', lambda m: ' ' * len(m.group(0)), s)
    s = re.sub(r'"(\\.|[^"\\])*"', lambda m: ' ' * len(m.group(0)), s)
    s = re.sub(r"'(\\.|[^'\\])*'", lambda m: ' ' * len(m.group(0)), s)
    return s

def classify_opener(pre):
    """根据 '{' 之前至多 60 字符, 判断该 '{' 开启的块类型。"""
    mkw = None
    for kw in ('class', 'struct', 'union', 'enum', 'namespace'):
        p = pre.rfind(kw)
        if p != -1 and (mkw is None or p > mkw[0]):
            mkw = (p, kw)
    if mkw is None:
        return 'block'
    between = pre[mkw[0] + len(mkw[1]):]
    if '{' in between or '}' in between:
        return 'block'
    return 'type' if mkw[1] != 'namespace' else 'ns'

def compute_enclosing_kinds(s):
    """返回与 s 等长的列表: 每个位置所处最内层块类型
    ('type'/'ns'/'block'/None)。struct 定义若位于 type/block 内 -> 嵌套, 跳过。"""
    n = len(s)
    encl = [None] * n
    stack = []
    cur = None
    i = 0
    while i < n:
        c = s[i]
        if c == '{':
            kind = classify_opener(s[max(0, i - 60):i])
            stack.append(kind)
            cur = kind
        elif c == '}':
            if stack:
                stack.pop()
            cur = stack[-1] if stack else None
        encl[i] = cur
        i += 1
    return encl

def is_definition_after(text, idx_after_name):
    """名字之后是否是定义体({ / : 继承 / final), 而非前向声明(;)。"""
    win = text[idx_after_name: idx_after_name + 400]
    win = re.sub(r'/\*.*?\*/', ' ', win, flags=re.S)
    win = re.sub(r'//[^\n]*', ' ', win)
    m = re.search(r'\S', win)
    if not m:
        return False
    return win[m.start()] in ('{', ':', 'f')  # '{' / ':' (继承) / 'f' (final)

# ---------------------------------------------------------------------------
# 文件处理
# ---------------------------------------------------------------------------
def process_file(full, apply):
    with open(full, "rb") as f:
        raw = f.read().decode("utf-8-sig")
    text = raw
    s = strip_equal(text)                 # 等长擦除, 供结构分析(索引对齐)
    encl = compute_enclosing_kinds(s)
    changed = False
    # 从后往前替换, 避免偏移错位
    for m in reversed(list(KW_RE.finditer(text))):
        kw = m.group(1)                   # 'class' / 'struct'
        name = m.group(2)
        # 已带宏 -> 跳过(幂等)
        if "JZFRAMEWORK_API" in text[m.start():m.start(2)]:
            continue
        if "JZFRAMEWORK_API" in text[m.end(2): m.end(2) + len("JZFRAMEWORK_API") + 1]:
            continue
        # 前向声明 -> 跳过
        if not is_definition_after(text, m.end(2)):
            continue
        # 导出判定
        if name.startswith("JZ"):
            # 规则1: JZ 开头的 class / struct
            pass
        elif kw == "struct":
            # 规则2: 非 JZ 前缀的 struct, 仅顶层 / namespace 内(非嵌套/局部)
            ek = encl[m.start()] if m.start() < len(encl) else None
            if ek in ('type', 'block'):
                continue
        else:
            continue
        # 插入: kw Name -> kw Name JZFRAMEWORK_API
        ins_at = m.end(2)
        text = text[:ins_at] + " JZFRAMEWORK_API" + text[ins_at:]
        changed = True
    if changed and apply:
        with open(full, "wb") as f:
            f.write(text.encode("utf-8"))
    return changed

def main():
    apply = "--apply" in sys.argv
    restrict = [a for a in sys.argv[1:] if not a.startswith("--")]
    restrict = restrict[0] if restrict else None
    count = 0
    for dirpath, dirnames, filenames in os.walk(ROOT):
        # 仅框架模块目录(JZ*), 才是编译进 DLL 的 API 面(binary/test/tools 等都是独立 target)
        rel = os.path.relpath(dirpath, ROOT)
        top = rel.split(os.sep)[0]
        if top and not top.startswith("JZ"):
            continue
        for fn in filenames:
            if not fn.endswith(".h"):
                continue
            # 只处理文件名以 "JZ" 开头的头文件(第三方 vendored 头自然被排除)
            if not fn.startswith("JZ"):
                continue
            full = os.path.join(dirpath, fn)
            if restrict and restrict not in full.replace("\\", "/"):
                continue
            if process_file(full, apply):
                count += 1
                if not apply:
                    print("  would modify:", os.path.relpath(full, ROOT))
    print(("APPLY " if apply else "DRYRUN ") + f"files_modified={count}")

if __name__ == "__main__":
    main()
