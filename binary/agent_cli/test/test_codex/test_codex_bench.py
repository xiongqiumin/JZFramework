#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""test_codex_bench.py —— 用 Codex(OpenAI Codex SDK) 跑 binary/code_agent/benchmark 的**同一批用例**,
产出与 binary/agent_cli/agent_cli_test 可比的 PASS/FAIL。

为什么要这么搭(对比才成立)
------------------------------------------------------------
- **用例、工程骨架、验证命令**都取自同一份 benchmark(`binary/code_agent/benchmark/data`: cases.jsonl
  + 各 case 的 prompt / setup / verify / expect.contains), 工程骨架(tsconfig + src/main.ts +
  jz_types/jz_js)与 `agent_cli_test` 的 `initProject` 逐字一致, 验证命令的占位符
  (`${root} ${build} ${runtime} ${port} ${bench} ${data}`)与 `expandVerifyCmd` 同语义;
  ⇒ 两个 harness 的差别只剩"**谁来写代码**"(agent_cli 的 JZAgent vs Codex), 效果才可比。
- **判分不在本脚本手里**: status 只看**验证命令的退出码**(与 agent_cli 的 verify 门控同口径);
  Codex 自己怎么想、怎么说不参与判分。
- 每个用例一个独立工作区 `case_<i>` + 独立 Codex 会话(`ephemeral`), 互不污染; 汇总同时写
  `<work>/summary.json`(逐条: name/passed/attempts/verify_rc/耗时/usage), 便于与 agent_cli_test 对照。

⚠ 必须经 `codex_proxy.py` 这一层
------------------------------------------------------------
Codex 走 Responses API 且每轮都发 `role="developer"`; 本机 llama.cpp 的 Jinja 模板只认"system 必须在
开头" ⇒ 直接连会 HTTP 500(Codex 只显示 "high demand" 这种误导文案)。所以 Codex 的
`model_providers.<name>.base_url` 指向本脚本拉起的代理, 由代理把 developer 改写成 **user**
(见 codex_proxy.py 的说明)。汇总里会打印"改写了多少条", 为 0 就说明适配没生效。

与 agent_cli 的差异(有意为之, 对比时心里有数)
------------------------------------------------------------
1. Codex 的"工程规则"走 `developer_instructions`(→ 代理改写成 user), 用例 prompt 走 user 消息;
   agent_cli 那边是把 system_prompt 与用例 prompt 拼成**一条** user 消息。
2. Codex 自己会跑命令自检(tsc / code_runtime / apply_patch); 这里同样给了它**验证门控重试**:
   第 1 轮用例 prompt, 之后每轮把"上一次验证失败输出 + retryHint"喂回去, 最多 maxAttempts 轮
   —— 与 agent_cli 的 verify 循环一样, 谁都不比谁多一次机会。
3. 运行期提示词(TS 运行时边界 / 终端用法)是**从 `binary/agent_tool/JZCodeTools.cpp` 现读**的
   (jzTsRuntimeNotes / jzTerminalRunNotes), 保证两边看到的是同一份; 读不到时退化成内置精简版并告警。
4. 用时/用量口径: **token 用 Codex 自己报的**(`TurnResult.usage`: `total` = 本会话累计, 与 agent_cli 的
   "Σ各轮整段 prompt" 对应; `last` = 最后一轮 ≈ agent_cli 的"上下文占用"), 代理侧统计只做交叉核对;
   agent_cli 那边还有更细的统计(轮次数/缓存命中/prefill 速率)。比"通过率"最干净, 比 token 时注意口径差
   (Codex 会把自带的系统提示与工具说明也算进 prompt; 差异大了脚本会提示)。

用法
------------------------------------------------------------
    # 先看要跑什么(不连模型、不起 Codex): 用例/验证命令/provider 配置全打出来
    python test_codex_bench.py --dry-run

    python test_codex_bench.py                        # 全部用例(慢: 真模型 + 真 Codex 回合)
    python test_codex_bench.py --cases hello_world    # 单个用例(支持 * ? 通配, 逗号分隔多个)
    python test_codex_bench.py --max-cases 3 --turn-timeout 900
    python test_codex_bench.py --data <benchmark/data> --work <工作目录>

模型配置: 读 `test_llm.json`(顺序: --llm-json > 本文件同目录 > ../test_llm.json > agent_cli.exe 同目录),
形状见 `binary/agent_cli/test/test_llm.json`(url 是 chat/completions; 代理会转成同服务的 /responses)。

退出码: 0=全部通过; 1=有失败; 2=前置条件缺失(模型未配置 / 评测集加载失败 / 找不到 tsc 或 code_runtime);
        77=跳过(没装 openai-codex SDK)。
⚠ `--work` 默认放 `build/_codex_bench/<时间戳>` **不放 %TEMP%**: CODEX_HOME 在临时目录下会被 Codex
  拒绝(它要在里面建辅助 exe/别名), 会 WARNING。
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
TEST_DIR = os.path.dirname(HERE)                      # binary/agent_cli/test
REPO_ROOT = os.path.abspath(os.path.join(TEST_DIR, "..", "..", ".."))

# 没抽到 agent_tool 的原文时的精简版(内容一致, 少了例子); 正常都会走到"现读"
FALLBACK_RUNTIME_NOTES = (
    "TypeScript 运行时边界(本工程跑在 JZ 定制运行时上, 不是 Node; 下面几条与 Node 不同):\n"
    "  - 只支持 ESM: 没有 require, 也不认 module.exports / exports。\n"
    "  - 内置模块直接写裸名(写 'node:http' 也认), 默认导入与具名导入都能用; 内置名: net / dgram / "
    "http / mqtt / ws / url / events / fs / path / process / crypto / buffer / os / serialport / "
    "modbus / sqlite / stream / worker_threads。\n"
    "  - Web 标准全局与 Node 一致且不用 import: fetch / Headers / Response / WebSocket / URL / "
    "EventEmitter / Buffer / TextEncoder / atob / btoa。\n"
    "  - 想查某个名字有哪些成员: 读工程里的 jz_types/<名字>/types/index.d.ts。\n"
    "  - 退出/保活: 打开的套接字/服务端、ref 着的定时器、跑着的 worker、在途的异步请求都算"
    "「有事要等」, 进程不会退出; 用完记得 close()/terminate(), 想不吊住进程就 unref()。\n"
    "  - 起服务这类不自己退出的命令用 terminal_run 的 background=true + terminal_query 查日志。\n"
)


# ==================== 小工具 ====================
def log(msg: str = "") -> None:
    """控制台是本地编码(GBK): 编不出的字符换成 '?' 而不是抛异常。"""
    try:
        print(msg, flush=True)
    except UnicodeEncodeError:
        enc = (sys.stdout.encoding or "utf-8")
        print(msg.encode(enc, "replace").decode(enc, "replace"), flush=True)


def fwd(path: str) -> str:
    """路径统一用 '/' —— 验证命令要拼进 shell, 反斜杠会被吃掉。"""
    return os.path.abspath(path).replace("\\", "/")


def read_text(path: str) -> str:
    """按本地实际编码读(UTF-8 优先, 兜 GBK)。"""
    try:
        raw = open(path, "rb").read()
    except OSError:
        return ""
    for enc in ("utf-8-sig", "utf-8", "gbk", "latin-1"):
        try:
            return raw.decode(enc)
        except Exception:                                            # noqa: BLE001
            continue
    return raw.decode("utf-8", "replace")


def free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def find_build_dir() -> str:
    """从本文件往上找 repo 的 build 目录(与 agent_cli_test 的 detectProgram 同思路)。"""
    d = HERE
    for _ in range(12):
        cand = os.path.join(d, "build")
        if os.path.isdir(cand):
            return cand
        parent = os.path.dirname(d)
        if parent == d:
            break
        d = parent
    return ""


def detect_program(name_no_ext: str, build: str) -> str:
    """在 build 里按部署路径找程序(ts_compiler/tsc/tsc、code_runtime); 找不到返回空串。

    产物可能落在 Release / Debug 两个配置目录里, 逐个试 —— 与 agent_cli_test 的 findUnder 一致。
    """
    bases = [os.path.join(build, "binary", "code_agent", "bin", cfg) for cfg in ("Release", "Debug")]
    bases += [os.path.join(build, "binary", "code_agent", cfg) for cfg in ("Release", "Debug")]
    for base in bases:
        for suffix in (".exe", ""):
            p = os.path.join(base, name_no_ext + suffix)
            if os.path.isfile(p):
                return fwd(p)
    return ""


def wildcard_match(pat: str, text: str) -> bool:
    """* ? . 当通配符, 其余字符精确匹配(与 benchmark/agent_cli_test 的 wildcardMatch 同语义)。"""
    rx = "".join(".*" if c == "*" else "." if c in "?." else re.escape(c) for c in pat)
    return re.fullmatch(rx, text) is not None


def read_llm_config(explicit: str, agent_cli_exe: str = "") -> dict:
    """读模型配置(test_llm.json): 返回 {path,url,name,token}。

    顺序: --llm-json > 本目录 > ../test_llm.json(仓库里的模板) > agent_cli.exe 同目录。
    """
    cands = []
    if explicit:
        cands.append(explicit)
    cands.append(os.path.join(HERE, "test_llm.json"))
    cands.append(os.path.join(TEST_DIR, "test_llm.json"))
    if agent_cli_exe:
        cands.append(os.path.join(os.path.dirname(agent_cli_exe), "test_llm.json"))

    for path in cands:
        if not os.path.isfile(path):
            continue
        data = None
        raw = ""
        try:
            raw = open(path, "rb").read().decode("utf-8-sig")
            data = json.loads(raw)
        except Exception:                                            # noqa: BLE001
            log("  [warn] %s 解析失败, 当没配置" % fwd(path))
            continue
        if not isinstance(data, dict):
            continue
        group = data["llm"] if isinstance(data.get("llm"), dict) else data

        def pick(key):
            v = group.get(key, data.get(key))
            return v if isinstance(v, str) else ""

        cfg = {"path": fwd(path), "url": pick("url"), "name": pick("name"), "token": pick("token")}
        if cfg["url"] and cfg["name"]:
            return cfg
        log("  [warn] %s 里缺 url/name, 跳过" % fwd(path))
    return {"path": os.path.join(HERE, "test_llm.json"), "url": "", "name": "", "token": ""}


def extract_cpp_string_func(src_text: str, func: str) -> str:
    """从 C++ 源码里抽"全是字符串常量拼接"的函数体(jzTsRuntimeNotes / jzTerminalRunNotes)。

    这么做的理由: 那两段提示词是 **agent_tool 里的唯一一份**(code_agent / agent_cli / 评测共用),
    这里再抄一份必然漂移 ⇒ 现读源码, 保证两边喂给模型的边界说明逐字相同。
    只认 `s += "..."` 这种写法; 抽不到返回空串(调用方退化到内置精简版并告警)。
    """
    m = re.search(r"string\s+" + re.escape(func) + r"\s*\(\s*\)\s*\{(.*?)\n\}", src_text, re.S)
    if not m:
        return ""
    out = []
    for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1)):
        out.append(re.sub(r"\\(.)", lambda mm: {"n": "\n", "t": "\t", "r": "\r"}.get(mm.group(1), mm.group(1)), lit))
    return "".join(out)


def runtime_notes(explicit_src: str = "") -> tuple[str, str]:
    """返回 (提示词文本, 来源说明)。"""
    src = explicit_src or os.path.join(REPO_ROOT, "binary", "agent_tool", "JZCodeTools.cpp")
    text = read_text(src)
    if text:
        a = extract_cpp_string_func(text, "jzTsRuntimeNotes")
        b = extract_cpp_string_func(text, "jzTerminalRunNotes")
        if a and b:
            return a + "\n" + b, fwd(src)
    log("  [warn] 没能从 %s 抽到运行期提示词, 用内置精简版(对比时注意这段有差)" % fwd(src))
    return FALLBACK_RUNTIME_NOTES, "内置精简版"


# ==================== 用例(与 benchmark / agent_cli_test 同口径) ====================
def load_cases(data_dir: str) -> tuple:
    """读 cases.jsonl(=index.jsonl, 每行 {"file": "<相对用例 json>"}); 没有索引就把 *.json 全收。

    返回 (用例表, 索引里指向不存在文件的条目) —— 后者要报出来: 两边 harness 都会静默跳过它们
    (仓库里现成的一例: 索引写 `algorithms/gen_*.json` 而目录已改名 `algo/`), 不报出来对比就失真。
    """
    cases = []
    skipped = []
    index = os.path.join(data_dir, "cases.jsonl")
    entries = []
    if os.path.isfile(index):
        for line in read_text(index).splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except ValueError:
                continue
            rel = obj.get("file") if isinstance(obj, dict) else (obj if isinstance(obj, str) else "")
            if rel and obj.get("enabled", True) is not False:
                entries.append(rel)
    if not entries:
        for p in sorted(glob.glob(os.path.join(data_dir, "**", "*.json"), recursive=True)):
            if os.path.basename(p) not in ("cases.jsonl", "baseline.json"):
                entries.append(os.path.relpath(p, data_dir).replace("\\", "/"))


    for rel in entries:
        path = rel if os.path.isabs(rel) else os.path.join(data_dir, rel)
        if not os.path.isfile(path):
            log("  [warn] 索引里的用例文件不存在, 跳过: %s" % fwd(path))
            skipped.append(rel)
            continue
        try:
            root = json.loads(read_text(path))
        except ValueError as e:
            log("  [warn] 用例解析失败, 跳过: %s (%s)" % (fwd(path), e))
            skipped.append(rel)
            continue
        if not isinstance(root, dict) or not root.get("prompt"):
            skipped.append(rel)
            continue
        exp = root.get("expect") if isinstance(root.get("expect"), dict) else {}
        cases.append({
            "name": root.get("name") or os.path.basename(path),
            "prompt": root["prompt"],
            "maxAttempts": int(root.get("maxAttempts", 3)),
            "timeout": int(root.get("timeout", 30)),          # 验证脚本/产物的上限(秒)
            "input": root.get("input", "") if isinstance(root.get("input"), str) else "",
            "verify": root.get("verify", "") if isinstance(root.get("verify"), str) else "",
            "verifyPort": int(root.get("verifyPort", 3900)),
            "setup": root.get("setup", "") if isinstance(root.get("setup"), str) else "",
            "daemon": root.get("daemon", "") if isinstance(root.get("daemon"), str) else "",
            "daemonReadyMs": int(root.get("daemonReadyMs", 3000)),
            "expectContains": exp.get("contains", "") if isinstance(exp.get("contains"), str) else "",
            "expectCI": bool(exp.get("caseInsensitive", False)),
            "retryHint": root.get("retryHint", "") if isinstance(root.get("retryHint"), str) else "",
            "sourceDir": os.path.dirname(path),
            "file": fwd(path),
        })
    return cases, skipped


def expand_verify(tpl: str, root: str, runtime: str, port: int, data_dir: str) -> str:
    """展开 verify/daemon 模板里的占位符(与 agent_cli_test::expandVerifyCmd 同语义)。"""
    return (tpl.replace("${root}", fwd(root))
               .replace("${build}", fwd(os.path.join(root, "build")))
               .replace("${runtime}", fwd(runtime))
               .replace("${port}", str(port))
               .replace("${bench}", fwd(HERE))
               .replace("${data}", fwd(data_dir)))


def init_project(root: str, code_bin_dir: str) -> str:
    """铺工程骨架(与 agent_cli_test::initProject 逐字一致): 返回错误文本, 空串=成功。

    agent 只往 src/ 里写 .ts; 产物目录由 tsconfig 的 outDir 定死(<root>/build)。
    """
    os.makedirs(root, exist_ok=True)

    has_types = False
    has_js = False
    for rel in ("jz_types", "jz_js"):
        src = os.path.join(code_bin_dir, "jz_engine", rel)
        if not os.path.isdir(src):
            src = os.path.join(code_bin_dir, rel)       # 旧部署: 平铺在 bin 目录
        if not os.path.isdir(src):
            continue                                    # 源不存在不算错(工程只是不引用运行时类型)
        dst = os.path.join(root, rel)
        if os.path.isdir(dst):
            shutil.rmtree(dst, ignore_errors=True)
        shutil.copytree(src, dst)
        if rel == "jz_types":
            has_types = True
        else:
            has_js = True
    del has_js

    include = '"src/**/*.ts"'
    if has_types:
        include += ', "jz_types/**/*.d.ts"'
    tsconfig = (
        "{\n"
        '  "compilerOptions": {\n'
        '    "target": "ES2022",\n'
        '    "module": "esnext",\n'
        '    "moduleResolution": "bundler",\n'
        '    "lib": ["ES2025"],\n'
        '    "strict": true,\n'
        '    "skipLibCheck": true,\n'
        '    "noImplicitAny": true,\n'
        '    "noEmitOnError": true,\n'
        '    "sourceMap": true,\n'
        '    "outDir": "build",\n'
        '    "rootDir": "src"\n'
        "  },\n"
        '  "include": [' + include + "],\n"
        '  "jzCompilerOptions": {}\n'
        "}\n"
    )
    for rel, text in (("tsconfig.json", tsconfig), (".gitignore", "build/\nnode_modules/\n")):
        p = os.path.join(root, rel)
        if not os.path.exists(p):
            with open(p, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)

    src_dir = os.path.join(root, "src")
    entries = [e for e in (os.listdir(src_dir) if os.path.isdir(src_dir) else [])]
    if not entries:
        os.makedirs(src_dir, exist_ok=True)
        with open(os.path.join(src_dir, "main.ts"), "w", encoding="utf-8", newline="\n") as f:
            f.write("// 工程入口(示例, 可自行删改)\n"
                    "// 编译: tsc -p tsconfig.json  ->  产物 build/main.js\n"
                    "export function main(): void\n"
                    "{\n"
                    '    console.log("hello from project");\n'
                    "}\n"
                    "main();\n")
    os.makedirs(os.path.join(root, "src", "GenerateFiles"), exist_ok=True)
    return ""


def build_stdout_verify_cmd(case: dict, tsc: str, python_exe: str, data_dir: str) -> str:
    """没有 verify 但有 expect.contains 时, 组织"通用 stdout 验证"(与 agent_cli_test 同款)。"""
    cmd = ('"%s" "${data}/stdout_verify.py" --tsc "%s" --runtime "${runtime}" --root "${root}"'
           ' --contains "%s"' % (fwd(python_exe), fwd(tsc), case["expectContains"].replace('"', '\\"')))
    if case["expectCI"]:
        cmd += " --case-insensitive"
    secs = max(5, case["timeout"] - 10)
    return cmd + " --timeout %d" % secs


class InputServer:
    """输入 HTTP 服务: 任何路径都返回"当前用例注入的输入"(与 agent_cli_test 的 InputHttpHandler 同语义)。

    用例里的 `hello_http` / `net_http` 就靠它 —— 给模型的提示词里会写上这个地址。
    """

    def __init__(self):
        self._text = ""
        srv = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *a):
                pass

            def do_GET(self):                                        # noqa: N802
                raw = srv.text.encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "text/plain; charset=utf-8")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)

        self._httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self._httpd.daemon_threads = True
        self.port = self._httpd.server_address[1]
        self._thread = threading.Thread(target=self._httpd.serve_forever, daemon=True)

    @property
    def text(self) -> str:
        return self._text

    @text.setter
    def text(self, value: str) -> None:
        self._text = value or ""

    def start(self) -> "InputServer":
        self._thread.start()
        return self

    def stop(self) -> None:
        try:
            self._httpd.shutdown()
            self._httpd.server_close()
        except Exception:                                            # noqa: BLE001
            pass


# ==================== Codex 驱动 ====================
def usage_dict(usage) -> dict:
    """`TurnResult.usage`(ThreadTokenUsage) -> 普通 dict, 便于写进 summary.json。

    Codex 会报用量(从 provider 响应的 usage 汇总): `total` = 本 thread 累计(多轮相加, 与 agent_cli 的
    "计费口径 = Σ各轮整段 prompt" 对应), `last` = 最后一轮(≈ agent_cli 的"上下文占用")。
    ⚠ 不是所有 provider 都回 usage ⇒ 可能为 None, 那时退化成代理侧统计(见 run_case)。
    """
    if usage is None:
        return {}
    out = {}
    for name in ("total", "last"):
        br = getattr(usage, name, None)
        if br is None:
            continue
        out[name] = {
            "input_tokens": getattr(br, "input_tokens", 0) or 0,
            "cached_input_tokens": getattr(br, "cached_input_tokens", 0) or 0,
            "output_tokens": getattr(br, "output_tokens", 0) or 0,
            "reasoning_output_tokens": getattr(br, "reasoning_output_tokens", 0) or 0,
            "total_tokens": getattr(br, "total_tokens", 0) or 0,
        }
    ctx = getattr(usage, "model_context_window", None)
    if ctx:
        out["model_context_window"] = ctx
    return out


class CodexRunner:
    """把 Codex SDK 包成"跑一个用例工作区"的角色。

    - 每个用例一个**新会话**(thread_start(ephemeral=True)), 多轮重试在**同一个会话**里继续(与
      agent_cli 的 verify 轮次对应);
    - provider 指向本地代理(developer -> user), wire_api 必须是 responses(chat/completions 已被
      Codex 移除); `model_context_window` 要显式给, 否则模型名不认识会走 fallback 元数据、过早压缩。
    """

    def __init__(self, model: str, proxy_base_url: str, codex_home: str, turn_timeout: float,
                 approval: str, sandbox: str, context_window: int, model_provider: str):
        os.makedirs(codex_home, exist_ok=True)
        try:
            from openai_codex import Codex, CodexConfig, Sandbox, ApprovalMode
        except ImportError as e:
            raise RuntimeError("没装 openai-codex SDK: %s" % e) from e

        self._Sandbox = Sandbox
        self._ApprovalMode = ApprovalMode
        self.sandbox = getattr(Sandbox, sandbox)
        self.approval = getattr(ApprovalMode, approval)
        self.model = model
        self.model_provider = model_provider
        self.turn_timeout = turn_timeout

        env = dict(os.environ)
        env["CODEX_HOME"] = codex_home                      # ⚠ 别放 %TEMP%(Codex 会 WARNING 拒绝建辅助 exe)
        env["JZ_CODEX_API_KEY"] = "jz-local"                # 本机 llama.cpp 不校验, 但 provider 要有个 env_key
        overrides = (
            "model_provider=" + model_provider,
            "model_providers.%s.name=jz-local" % model_provider,
            "model_providers.%s.base_url=%s" % (model_provider, proxy_base_url),
            "model_providers.%s.wire_api=responses" % model_provider,
            "model_providers.%s.env_key=JZ_CODEX_API_KEY" % model_provider,
            "model_context_window=%d" % context_window,
        )
        self.overrides = overrides
        self._codex = Codex(CodexConfig(config_overrides=overrides, env=env,
                                        client_name="jz-codex-bench", client_title="JZ Codex Bench"))
        self._thread = None

    def close(self) -> None:
        try:
            self._codex.close()
        except Exception:                                            # noqa: BLE001
            pass

    def new_session(self, ws: str, developer_instructions: str):
        self._thread = self._codex.thread_start(model=self.model,
                                                model_provider=self.model_provider,
                                                sandbox=self.sandbox,
                                                approval_mode=self.approval,
                                                developer_instructions=developer_instructions,
                                                cwd=ws,
                                                ephemeral=True)
        return self._thread

    def run_turn(self, prompt: str) -> dict:
        """跑一个回合(带超时)。

        ⚠ SDK 没有 cancel: 超时后那个回合仍在 Codex 子进程里跑。但它**活不过本用例** ——
          run_case 的 finally 会 runner.close() 收掉这个 Codex 实例(每个用例一个实例),
          所以 --on-timeout next(默认)不会让它去抢**下一个**用例的模型(只在"本用例验证那几秒"里还吃着)。
          返回 {ok, text, error, elapsed, status, timeout}。
        """
        box = {}

        def work():
            t0 = time.time()
            try:
                res = self._thread.run(prompt)
                box["text"] = (getattr(res, "final_response", "") or "")
                box["status"] = str(getattr(res, "status", ""))
                box["error"] = str(getattr(res, "error", "") or "")
                box["items"] = len(getattr(res, "items", []) or [])
                box["usage"] = usage_dict(getattr(res, "usage", None))   # Codex 自己报的 token 用量
            except Exception as e:                                   # noqa: BLE001
                box["error"] = "%s: %s" % (type(e).__name__, e)
            box["elapsed"] = time.time() - t0

        th = threading.Thread(target=work, daemon=True)
        t0 = time.time()
        th.start()
        th.join(self.turn_timeout)
        if th.is_alive():
            return {"ok": False, "timeout": True, "elapsed": time.time() - t0, "usage": {},
                    "text": "", "error": "回合超过 %.0fs 未结束" % self.turn_timeout, "status": "timeout"}
        out = {"ok": not box.get("error"), "timeout": False,
               "elapsed": box.get("elapsed", time.time() - t0), "text": box.get("text", ""),
               "error": box.get("error", ""), "status": box.get("status", ""),
               "usage": box.get("usage") or {}}
        return out


# ==================== 单用例 ====================
def run_case(case: dict, idx: int, ctx: dict) -> dict:
    """跑一个用例: 铺骨架 -> setup -> daemon -> Codex 回合 x maxAttempts(每轮验证) -> 记录结果。"""
    root = os.path.join(ctx["work"], "case_%d_%s" % (idx, case["name"]))
    result = {"name": case["name"], "file": case["file"], "root": fwd(root), "passed": False,
              "attempts": 0, "verify_rc": None, "verify_cmd": "", "last_verify_output": "",
              "turns": [], "final_response": "", "error": "", "elapsed": 0.0,
              # 用量(两套口径, 见下面的赋值)：codex = SDK 报的会话累计; proxy = 代理侧交叉核对
              "usage": {}, "usage_source": "none", "prompt_tokens": 0, "completion_tokens": 0,
              "cached_tokens": 0, "context_tokens": 0,
              "proxy_prompt_tokens": 0, "proxy_completion_tokens": 0, "proxy_cached_tokens": 0}
    t_case = time.time()

    log("")
    log("==== 用例[%s] 开始 (maxAttempts=%d, root=%s) ====" % (case["name"], case["maxAttempts"], fwd(root)))

    # ⚠ 下面这段(铺骨架 / setup / 起 daemon)原来在 try **之外**, 而外层那个 try 的 finally 只负责
    #    收 daemon/runner ⇒ 这三步只要抛异常(initProject 出错、setup 超 300s、Popen 失败…),
    #    异常就会冒到 main 的用例循环之外, **整轮评测中断**。
    #    既然要求"失败了也能跑下一个", 这里自己兜住: 记成本用例失败, 直接返回让 main 继续。
    daemon = None
    runner = None
    try:
        err = init_project(root, ctx["code_bin_dir"])
        if err:
            result["error"] = "initProject: " + err
            result["elapsed"] = time.time() - t_case
            log("  [FAIL] 初始化工程失败: %s" % err)
            return result

        # setup 脚本(相对用例 json 目录): 非零退出 = 本用例 setup 失败
        if case["setup"]:
            script = case["setup"]
            if not os.path.isabs(script):
                script = os.path.join(case["sourceDir"], script)
            cmd = ('"%s" "%s" --root "%s" --input "%s" --data "%s"'
                   % (fwd(ctx["python"]), fwd(script), fwd(root), case["input"], fwd(ctx["data"])))
            p = subprocess.run(cmd, shell=True, cwd=root, capture_output=True, text=True,
                               errors="replace", timeout=300)
            if p.returncode != 0:
                result["error"] = "setup 退出码 %d: %s" % (p.returncode, (p.stdout or "").strip()[:500])
                result["elapsed"] = time.time() - t_case
                log("  [FAIL] setup 脚本失败: %s" % result["error"])
                return result

        # daemon 后台服务(可选): 整个用例期间常驻, 结束时连子孙一起收
        if case["daemon"]:
            cmd = expand_verify(case["daemon"], root, ctx["runtime"], case["verifyPort"], ctx["data"])
            log("  [daemon] %s" % cmd)
            daemon = subprocess.Popen(cmd, shell=True, cwd=root, stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT, text=True, errors="replace", bufsize=1)
            time.sleep(max(0.5, case["daemonReadyMs"] / 1000.0))     # 有输出/超时都继续(与 C++ 一致)
    except subprocess.TimeoutExpired as e:
        result["error"] = "初始化/setup 超时: %s" % e
        result["elapsed"] = time.time() - t_case
        log("  [FAIL] %s" % result["error"])
        if daemon:
            kill_tree(daemon.pid)
        return result
    except Exception as e:                                           # noqa: BLE001
        result["error"] = "初始化/setup 异常: %s: %s" % (type(e).__name__, e)
        result["elapsed"] = time.time() - t_case
        log("  [FAIL] %s" % result["error"])
        if daemon:
            kill_tree(daemon.pid)
        return result

    try:
        # 验证命令: 用例给了就用它; 没有但有 expect.contains 就挂通用 stdout 验证
        verify_tpl = case["verify"]
        if not verify_tpl and case["expectContains"]:
            verify_tpl = build_stdout_verify_cmd(case, ctx["tsc"], ctx["python"], ctx["data"])
        result["verify_cmd"] = expand_verify(verify_tpl, root, ctx["runtime"], case["verifyPort"], ctx["data"]) if verify_tpl else ""

        prompt = ctx["system_prompt"] + "\n" + case["prompt"]
        if case["input"] and not case["setup"]:
            prompt += ("\n\n## 读取输入\n如果任务需要从输入读取数据, 用以下方式(异步 fetch, 顶层 await):\n"
                       "  const res = await fetch('http://127.0.0.1:%d/input');\n"
                       "  const input = await res.text();\n"
                       "input 即评测注入的用例输入; 无需输入的题目不要读取。" % ctx["input_port"])

        ctx["proxy"].reset_stats()
        runner = ctx["make_runner"](root)
        runner.new_session(root, ctx["system_prompt"])
        last_verify = ""


        for attempt in range(1, case["maxAttempts"] + 1):
            result["attempts"] = attempt
            text = prompt if attempt == 1 else (
                "上一次运行**未通过验证**(输出见下), 请在同一个工程里修正后重新完成要求。\n"
                "%s\n--- 验证输出 ---\n%s\n---" % (case["retryHint"] or "", last_verify[-2000:]))
            log("  [回合 %d/%d] 提交给 Codex(%d 字)..." % (attempt, case["maxAttempts"], len(text)))
            turn = runner.run_turn(text)
            result["turns"].append({"attempt": attempt, "elapsed": round(turn["elapsed"], 1),
                                    "status": turn.get("status", ""), "error": turn.get("error", ""),
                                    "timeout": bool(turn.get("timeout")), "usage": turn.get("usage", {})})
            if turn["text"]:
                result["final_response"] = turn["text"]
                log("  [回复] %s" % turn["text"].strip().replace("\n", " ")[:300])
            if turn["error"]:
                log("  [warn] 回合报错: %s" % turn["error"])

            if not result["verify_cmd"]:
                # 没有判定标准(烟雾用例): 以"Codex 回合没报错"为准 —— 与 agent_cli_test 同口径
                result["passed"] = not turn["error"] and not turn.get("timeout")
                result["verify_rc"] = 0 if result["passed"] else 1
                break

            p = subprocess.run(result["verify_cmd"], shell=True, cwd=root, capture_output=True,
                               text=True, errors="replace", timeout=case["timeout"] + 60)
            last_verify = ((p.stdout or "") + (p.stderr or "")).strip()
            result["verify_rc"] = p.returncode
            result["last_verify_output"] = last_verify[-4000:]
            log("  [验证] rc=%d %s" % (p.returncode, last_verify.replace("\n", " | ")[:300]))
            if p.returncode == 0:
                result["passed"] = True
                break
            if turn.get("timeout"):
                break                       # 回合都没跑完, 再喂验证输出也没意义

        # 用量: **优先用 Codex 自己报的**(SDK 从 provider 的 usage 汇总; total=本会话累计), 它才是
        # "这次 agent 真花了多少"; 代理侧的统计留着交叉核对(也兜底 provider 不回 usage 的情况)。
        stats = ctx["proxy"].stats()
        result["proxy_prompt_tokens"] = stats.get("prompt_tokens", 0)
        result["proxy_completion_tokens"] = stats.get("completion_tokens", 0)
        result["proxy_cached_tokens"] = stats.get("cached_tokens", 0)

        usage = {}
        for t in result["turns"]:
            if t.get("usage"):
                usage = t["usage"]                   # 同一个会话: 取最后一轮的(它已是累计值)
        result["usage"] = usage
        tot = usage.get("total") or {}
        last = usage.get("last") or {}
        if tot:
            result["prompt_tokens"] = tot.get("input_tokens", 0)
            result["completion_tokens"] = tot.get("output_tokens", 0)
            result["cached_tokens"] = tot.get("cached_input_tokens", 0)
            result["context_tokens"] = last.get("total_tokens", 0)
            result["usage_source"] = "codex"
        else:
            result["prompt_tokens"] = result["proxy_prompt_tokens"]
            result["completion_tokens"] = result["proxy_completion_tokens"]
            result["cached_tokens"] = result["proxy_cached_tokens"]
            result["context_tokens"] = 0
            result["usage_source"] = "proxy"      # Codex 没报(或回合被超时打断)
        result["elapsed"] = time.time() - t_case
        n_to = sum(1 for t in result["turns"] if t.get("timeout"))
        log("  ---- 用例[%s] 结束: %s (attempts=%d, verify_rc=%s, 用时 %.1fs, tokens=%d [%s])%s"
            % (case["name"], "PASS" if result["passed"] else "FAIL", result["attempts"],
               result["verify_rc"], result["elapsed"],
               result["prompt_tokens"] + result["completion_tokens"], result["usage_source"],
               ("  [注意] 有 %d 个回合超时(按 --on-timeout 继续)" % n_to) if n_to else ""))
        return result
    except subprocess.TimeoutExpired:
        result["error"] = "验证命令超时"
        result["elapsed"] = time.time() - t_case
        result["passed"] = False
        log("  [FAIL] 验证命令超时")
        return result
    except Exception as e:                                           # noqa: BLE001
        result["error"] = "%s: %s" % (type(e).__name__, e)
        result["elapsed"] = time.time() - t_case
        result["passed"] = False
        log("  [FAIL] 运行异常: %s" % result["error"])
        return result
    finally:
        if daemon:
            kill_tree(daemon.pid)
        if runner:
            # 每个用例一个 Codex 实例(独立会话/日志): 用完要收掉子进程。
            # ⚠ 这也是"回合超时被放弃后不会抢下一个用例模型"的依据(--on-timeout next 安全):
            #    僵尸回合在这个 finally 里随子进程一起消失。
            runner.close()


def kill_tree(pid: int) -> None:
    """连子孙一起收: Windows 用 taskkill /T, POSIX 发进程组信号。"""
    if os.name == "nt":
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(pid)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return
    try:
        os.killpg(os.getpgid(pid), 15)
    except Exception:                                                # noqa: BLE001
        pass


# ==================== main ====================
def build_system_prompt(runtime_notes_text: str) -> str:
    """与 agent_cli_test 的 system_prompt 同一套工程约定(逐条对应), 后面接运行期边界说明。"""
    return ("你是 TypeScript 开发助手，工程骨架已初始化好，按这套固定流程干活：\n"
            "1. 直接在 `src/` 下写 `.ts` 源码，入口必须是 `src/main.ts`（编译产物为 `build/main.js`）；\n"
            "2. tsconfig.json 已配置好（target ES2022，module esnext，rootDir ./src，outDir ./build，"
            "noEmitOnError），不要改它，也不要另建工程；\n"
            "3. 需要编译自检时执行：`tsc -p tsconfig.json`；需要跑起来时执行："
            "`code_runtime --entry ./build/main.js`；\n"
            "4. 只用 TS 写代码，不要直接写 .js（build/ 下的 js 是编译产物）。\n"
            + runtime_notes_text)


def main(argv=None) -> int:
    # ⚠ Windows 控制台默认 GBK: 帮助/日志里的 `⚠` 这类字符编不出去 ⇒ `--help` 直接 UnicodeEncodeError。
    #    这里只放宽错误处理(不换编码), 编不出的字符退化成 '?'。
    for _s in (sys.stdout, sys.stderr):
        try:
            _s.reconfigure(errors="replace")
        except Exception:                                            # noqa: BLE001
            pass

    ap = argparse.ArgumentParser(description="用 Codex SDK 跑 benchmark 用例(与 agent_cli_test 同口径)")
    ap.add_argument("--data", default="", help="评测集目录(默认 <build>/binary/code_agent/bin/<cfg>/data/agent_cli_test, "
                                               "回退仓库里的 binary/code_agent/benchmark/data)")
    ap.add_argument("--cases", default="", help="逗号分隔的用例名/通配符(* ? .), 只跑匹配的")
    ap.add_argument("--work", default="", help="工作目录(默认 <build>/_codex_bench/<时间戳>; ⚠ 别用 %%TEMP%%, "
                                               "CODEX_HOME 在临时目录下会被 Codex 拒绝)")
    ap.add_argument("--llm-json", default="", help="模型配置文件(默认见文件头说明)")
    ap.add_argument("--agent-cli", default=os.environ.get("JZ_AGENT_CLI", ""),
                    help="agent_cli 可执行文件(只用于找它同目录的 test_llm.json; 可不给)")
    ap.add_argument("--tsc", default="", help="tsc 全路径(默认在 build 里探测)")
    ap.add_argument("--code-runtime", default="", help="code_runtime 全路径(默认在 build 里探测)")
    ap.add_argument("--python", default=sys.executable, help="验证脚本/tsc 用的 python")
    ap.add_argument("--model", default="", help="模型名(默认取 test_llm.json 的 name)")
    ap.add_argument("--provider", default="jz-local", help="Codex 里自定义 provider 的名字")
    ap.add_argument("--context-window", type=int, default=204800,
                    help="model_context_window(必须显式给: 模型名不被 Codex 认识时否则走 fallback 元数据)")
    ap.add_argument("--sandbox", default="workspace_write", choices=["read_only", "workspace_write", "full_access"],
                    help="Codex 沙箱(默认 workspace-write: 只许改用例工程)")
    ap.add_argument("--approval", default="auto_review", choices=["deny_all", "auto_review"],
                    help="Codex 审批模式(默认 auto_review: 无人值守也能跑)")
    ap.add_argument("--turn-timeout", type=float, default=1800.0, help="单个 Codex 回合的上限(秒, 默认 1800)")
    ap.add_argument("--on-timeout", default="next", choices=["stop", "next"],
                    help="回合超时后: next=接着跑下一个用例(默认; 被放弃的回合活不过本用例, "
                         "用例结束时会 runner.close() 收掉 Codex 子进程); stop=直接收工")
    ap.add_argument("--max-cases", type=int, default=0, help="只跑前 N 个用例(冒烟用)")
    ap.add_argument("--dry-run", action="store_true", help="只打印要跑什么(不连模型、不起 Codex)")
    args = ap.parse_args(argv)

    # ---- 依赖与路径 ----
    build = find_build_dir()
    if not build:
        log("[ERROR] 找不到 build 目录(从 %s 往上找); 先构建一次" % fwd(HERE))
        return 2
    tsc = fwd(args.tsc) if args.tsc else detect_program("ts_compiler/tsc/tsc", build)
    runtime = fwd(args.code_runtime) if args.code_runtime else detect_program("code_runtime", build)
    code_bin_dir = os.path.dirname(runtime) if runtime else ""

    data_dir = args.data
    if not data_dir:
        cands = [os.path.join(build, "binary", "code_agent", "bin", cfg, "data", "agent_cli_test")
                 for cfg in ("Release", "Debug")]
        cands += [os.path.join(REPO_ROOT, "binary", "code_agent", "benchmark", "data")]
        for c in cands:
            if os.path.isdir(c):
                data_dir = c
                break
    data_dir = fwd(data_dir)

    llm = read_llm_config(args.llm_json, args.agent_cli)
    model = args.model or llm["name"]

    notes_text, notes_src = runtime_notes()
    system_prompt = build_system_prompt(notes_text)

    log("=" * 78)
    log("Codex 跑 benchmark 用例(与 agent_cli_test 同套用例/骨架/验证)")
    log("  评测集    : %s" % data_dir)
    log("  tsc       : %s" % (tsc or "(没找到!)"))
    log("  code_runtime: %s" % (runtime or "(没找到!)"))
    log("  模型配置  : %s" % llm["path"])
    log("  模型      : %s @ %s" % (model or "(未配置!)", llm["url"] or "(未配置!)"))
    log("  运行期提示: %s" % notes_src)
    log("=" * 78)

    cases, skipped = load_cases(data_dir)
    if skipped:
        log("⚠ 索引里有 %d 条用例指向不存在的文件(两个 harness 都会跳过, 对比时两边一致): %s"
            % (len(skipped), ", ".join(skipped[:6]) + (" ..." if len(skipped) > 6 else "")))
        log("  (仓库现状: cases.jsonl 写 `algorithms/gen_*.json`, 而目录已改名 `algo/` —— 要跑这些用例"
            "得修数据索引与用例里的 `${data}/algorithms/...` 路径)")
    if not cases:
        log("[ERROR] 没加载到用例: %s" % data_dir)
        return 2
    if args.cases:
        want = [s.strip() for s in args.cases.split(",") if s.strip()]
        cases = [c for c in cases if any(wildcard_match(w, c["name"]) or w == c["name"] for w in want)]
        if not cases:
            log("[ERROR] --cases 没匹配到任何用例")
            return 2
    if args.max_cases > 0:
        cases = cases[: args.max_cases]

    # --dry-run: 不连模型也能看到"要跑什么/怎么判定"(写好代码先自检用)
    if args.dry_run:
        log("用例 %d 个:" % len(cases))
        for i, c in enumerate(cases):
            tpl = c["verify"] or ("<通用 stdout 验证: 期望输出含 %r>" % c["expectContains"]
                                  if c["expectContains"] else "")
            log("  [%d] %-22s maxAttempts=%d timeout=%ds setup=%s input=%s"
                % (i, c["name"], c["maxAttempts"], c["timeout"], c["setup"] or "-",
                   ("有" if c["input"] else "-")))
            if tpl:
                log("      验证: %s" % expand_verify(tpl, "<work>/case_%d" % i, runtime or "<runtime>",
                                                     c["verifyPort"], data_dir))
        log("Codex provider(会随实际代理端口填 base_url):")
        log("  model_provider=%s / model_providers.%s.wire_api=responses / model=%s"
            % (args.provider, args.provider, model or "<model>"))
        log("  sandbox=%s approval=%s context_window=%d turn_timeout=%.0fs"
            % (args.sandbox, args.approval, args.context_window, args.turn_timeout))
        try:
            import openai_codex
            log("  openai-codex 版本: %s" % getattr(openai_codex, "__version__", "?"))
        except ImportError as e:
            log("  [warn] 没装 openai-codex: %s" % e)
        return 0

    if not llm["url"] or not model:
        log("[ERROR] 没配置模型: 请在 %s 里写 {\"llm\":{\"url\":...,\"name\":...}}" % llm["path"])
        return 2
    if not runtime or not tsc:
        log("[ERROR] 找不到 code_runtime / tsc(先构建 binary/code_agent, 或用 --code-runtime/--tsc 指定)")
        return 2
    try:
        import openai_codex                                       # noqa: F401
    except ImportError as e:
        log("[SKIP] 没装 openai-codex SDK: %s" % e)
        return 77

    work = args.work
    if not work:
        work = os.path.join(build, "_codex_bench", time.strftime("%Y%m%d_%H%M%S"))
    work = fwd(work)
    os.makedirs(work, exist_ok=True)
    codex_home = os.path.join(work, "codex_home")

    from codex_proxy import CodexProxy                          # 同目录(本脚本也是从这里跑起来的)

    input_srv = InputServer().start()
    proxy = CodexProxy(llm["url"], 0, os.path.join(work, "codex_proxy.log")).start()

    def make_runner(ws: str) -> CodexRunner:
        r = CodexRunner(model=model, proxy_base_url=proxy.base_url, codex_home=codex_home,
                        turn_timeout=args.turn_timeout, approval=args.approval, sandbox=args.sandbox,
                        context_window=args.context_window, model_provider=args.provider)
        # 每个用例一个子目录: Codex 的会话/日志互相独立(cwd 用用例工作区)
        os.makedirs(ws, exist_ok=True)
        return r

    ctx = {"work": work, "data": data_dir, "tsc": tsc, "runtime": runtime, "code_bin_dir": code_bin_dir,
           "python": args.python, "system_prompt": system_prompt, "input_port": input_srv.port,
           "proxy": proxy, "make_runner": make_runner}

    log("代理: %s -> %s" % (proxy.base_url, proxy.request_url))
    log("工作目录: %s" % work)
    log("Codex provider: model_provider=%s sandbox=%s approval=%s" % (args.provider, args.sandbox, args.approval))

    results = []
    t_all = time.time()
    stopped_early = False
    for i, case in enumerate(cases):
        input_srv.text = case["input"]
        try:
            res = run_case(case, i, ctx)
        except Exception as e:                                       # noqa: BLE001
            # 兜底(与 run_case 里那两处 try 互补): 漏出来的异常不许中断整轮 —— 记成本用例失败继续。
            res = {"name": case["name"], "file": case["file"], "root": "", "passed": False,
                   "attempts": 0, "verify_rc": None, "verify_cmd": "", "last_verify_output": "",
                   "turns": [], "final_response": "",
                   "error": "harness 异常: %s: %s" % (type(e).__name__, e), "elapsed": 0.0,
                   "usage": {}, "usage_source": "none", "prompt_tokens": 0, "completion_tokens": 0,
                   "cached_tokens": 0, "context_tokens": 0,
                   "proxy_prompt_tokens": 0, "proxy_completion_tokens": 0, "proxy_cached_tokens": 0}
            log("  [FAIL] 用例[%s] 抛出未捕获异常, 记失败后继续下一个: %s" % (case["name"], res["error"]))
        results.append(res)
        if any(t.get("timeout") for t in res["turns"]) and args.on_timeout == "stop":
            log("!! 有回合超时, 按 --on-timeout=stop 收工(剩 %d 个用例不跑)" % (len(cases) - i - 1))
            stopped_early = True
            break

    total_ms = time.time() - t_all
    passed = [r for r in results if r["passed"]]
    prompt_tok = sum(r["prompt_tokens"] for r in results)
    comp_tok = sum(r["completion_tokens"] for r in results)
    cached_tok = sum(r["cached_tokens"] for r in results)
    proxy_prompt = sum(r["proxy_prompt_tokens"] for r in results)
    proxy_comp = sum(r["proxy_completion_tokens"] for r in results)
    ctx_max = max((r["context_tokens"] for r in results), default=0)
    ctx_name = max(results, key=lambda r: r["context_tokens"])["name"] if results else ""
    src = results[0]["usage_source"] if results else "?"
    stats = proxy.stats()

    log("")
    log("================ codex 评测结果 ================")
    for r in results:
        log("[%s] %-24s (attempts=%d, verify_rc=%s, 用时 %.1fs, tokens=%d [%s], 上下文=%d)"
            % ("PASS" if r["passed"] else "FAIL", r["name"], r["attempts"], r["verify_rc"],
               r["elapsed"], r["prompt_tokens"] + r["completion_tokens"], r["usage_source"],
               r["context_tokens"]))
        if not r["passed"]:
            tail = (r["last_verify_output"] or r["error"]).strip().replace("\n", " | ")
            log("        最后一次: %s" % tail[:220])
    log("总分: %d / %d" % (len(passed), len(results)))
    log("总耗时: %.1f s" % total_ms)
    log("Token(Codex 口径, 会话累计): prompt=%d [其中缓存命中 %d] + completion=%d = %d"
        % (prompt_tok, cached_tok, comp_tok, prompt_tok + comp_tok))
    log("  交叉核对(代理侧): prompt=%d + completion=%d   [Codex 不回 usage 时才以它为准]"
        % (proxy_prompt, proxy_comp))
    if src == "codex" and proxy_prompt and abs(proxy_prompt - prompt_tok) > max(64, prompt_tok * 0.05):
        log("  ⚠ 两个口径差得有点多(Codex %d vs 代理 %d): Codex 会把自己的系统提示/工具说明也算进去"
            % (prompt_tok, proxy_prompt))
    log("上下文(各用例最后一轮的最大值) = %d (%s)" % (ctx_max, ctx_name))
    log("适配统计: 请求 %d 次, developer->user 改写 %d 条, 上游失败 %d 次"
        % (stats.get("requests", 0), stats.get("role_rewrites", 0), stats.get("failures", 0)))
    if stats.get("role_rewrites", 0) == 0:
        log("⚠ 一条都没改写: 适配没生效(Codex 换协议了?) —— 结果不可信, 先看 codex_proxy.log")
    log("===============================================")

    summary = {
        "kind": "codex_bench",
        "time": time.strftime("%Y-%m-%d %H:%M:%S"),
        "config": {"model": model, "llm_json": llm["path"], "data": data_dir, "work": work,
                   "tsc": tsc, "runtime": runtime, "cases": args.cases, "max_cases": args.max_cases,
                   "sandbox": args.sandbox, "approval": args.approval, "provider": args.provider,
                   "turn_timeout": args.turn_timeout, "context_window": args.context_window,
                   "runtime_notes_from": notes_src, "proxy": proxy.base_url, "upstream": proxy.request_url},
        "totals": {"passed": len(passed), "total": len(results), "elapsed_s": round(total_ms, 1),
                   # 用量两套口径: codex = SDK 报的(会话累计, 实际用它), proxy = 代理侧交叉核对
                   "usage_source": src,
                   "prompt_tokens": prompt_tok, "completion_tokens": comp_tok,
                   "cached_tokens": cached_tok,
                   "proxy_prompt_tokens": proxy_prompt, "proxy_completion_tokens": proxy_comp,
                   "context_tokens_last_round_max": ctx_max,
                   "role_rewrites": stats.get("role_rewrites", 0),
                   "proxy_requests": stats.get("requests", 0), "proxy_failures": stats.get("failures", 0),
                   "stopped_early": stopped_early},
        "skipped_index_entries": skipped,      # 索引里指向不存在文件的条目(两边 harness 都跳过)
        "cases": results,
    }
    out = os.path.join(work, "summary.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(summary, f, ensure_ascii=False, indent=2)
    log("汇总已写: %s" % fwd(out))

    proxy.stop()
    input_srv.stop()
    if stopped_early:
        return 1
    return 0 if len(passed) == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
