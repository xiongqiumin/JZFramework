// code_agent_lsp_test - 直连 TSGO(tsc --lsp --stdio) LSP 接口的冒烟测试。
//
// 流程（headless、无 GUI、不用大模型）：
//   1. 在 JZPlatform::applicationTmpPath() 下新建一个最小 TS 工程（tsconfig.json +
//      sample.ts：含函数 / 类 / 调用，便于验证定义、引用、成员等各类查询）；
//   2. 创建 JZTsLangServer，指定原生 tsc.exe 路径（即 tsgo 语言服务），setRoot 到该工程；
//   3. 用事件循环(JZApplication::processEvents)驱动异步回调，逐个调用 10 个 tool：
//        definition / references / quickinfo / rename / completion /
//        document_highlights / diagnostics / source / outline / members
//      并断言返回结构正确、定位/内容符合预期。
//
// 用法：code_agent_lsp_test.exe
// 环境变量：JZ_TS_LSP 可换原生 tsc.exe 路径（默认 E:/libs/ts_compiler/.../tsc.exe）。
// 退出码：0=全部通过；1=有失败；2=前置条件缺失（编译器不可用）。
//
// 注意：本文件不要 include <windows.h>（JZAgentTool.h 无作用域枚举成员
// BOOLEAN 与 winnt.h 冲突 C2365）；事件循环等待用 std::this_thread::sleep_for。

#define _CRT_SECURE_NO_WARNINGS

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "JZApplication.h"
#include "JZDir.h"
#include "JZFileInfo.h"
#include "JZPlatform.h"
#include "JZSignal.h"
#include "JZTimer.h"
#include "JZTsLangServer.h"
#include "json/json.h"

using namespace std;

namespace
{
JZApplication *g_app = nullptr;
int g_failed = 0;
bool g_deferNext = false;   // 某步自行安排延迟再发下一步时置位

string normalize(const string &path)
{
    string p = path;
    for (size_t i = 0; i < p.size(); i++)
        if (p[i] == '\\')
            p[i] = '/';
    while (p.size() > 1 && p.back() == '/')
        p.pop_back();
    return p;
}

bool fileExists(const string &path)
{
    ifstream in(normalize(path), ios::binary);
    return in.good();
}

bool writeText(const string &path, const string &text)
{
    ofstream out(normalize(path), ios::binary | ios::trunc);
    if (!out)
        return false;
    out.write(text.data(), (streamsize)text.size());
    out.close();
    return true;
}

void ensureDir(const string &path)
{
    JZDir mk;
    mk.mkpath(normalize(path));
}

void rmRecursive(const string &path)
{
    JZDir dir(normalize(path));
    if (dir.exists())
        dir.removeRecursively();
}

static void check(bool ok, const char *tag, const string &detail)
{
    if (ok)
        printf("[PASS] %s\n", tag);
    else
    {
        printf("[FAIL] %s\n       %s\n", tag, detail.c_str());
        g_failed++;
    }
}

// 原生 tsgo(tsc) 二进制路径（即 LSP 服务器）：JZ_TS_LSP 可覆盖
static string resolveTsgoLsp()
{
    string env = "";
    if (const char *v = getenv("JZ_TS_LSP"))
        env = v;
    if (!env.empty() && fileExists(env))
        return env;
    string cand = "E:/libs/ts_compiler/app/node_modules/@typescript/"
                  "typescript-win32-x64/lib/tsc.exe";
    if (fileExists(cand))
        return cand;
    return string();
}

static string g_sampleFile; // 绝对路径，供断言 file 字段

// 解析 payload JSON；失败返回 false
static bool parsePayload(const string &payload, Json::Value &out)
{
    Json::Reader reader;
    return reader.parse(payload, out);
}

// 从 documentSymbol/成员数组里取所有 name 是否包含某值
static bool symArrHasName(const Json::Value &arr, const string &name)
{
    if (!arr.isArray())
        return false;
    for (const auto &s : arr)
        if (s.isMember("name") && s["name"].asString() == name)
            return true;
    return false;
}

static string toLowerStr(const string &s)
{
    string r = s;
    for (char &c : r)
        c = (char)tolower((unsigned char)c);
    return r;
}

// 把 LSP 返回的 file 字段与样本文件比对（都归一化为 / 分隔；Windows 路径大小写不敏感）
static bool fileMatches(const string &got)
{
    return toLowerStr(normalize(g_sampleFile)) == toLowerStr(normalize(got));
}

// 一次请求的可共享状态：放在堆上，避免 detached 看门狗线程引用已销毁的
// 局部变量（否则后续请求会被误判超时 / 内存被踩）。
struct ReqState
{
    std::atomic<bool> done{false};
    bool ok = false;
    std::string raw;
    Json::Value out;
};

// ---------------------------------------------------------------- 样本 TS
static const char *SAMPLE_TS =
    "// sample.ts - LSP 冒烟测试样本（函数 / 类 / 调用）\n"
    "export function add(a: number, b: number): number\n"
    "{\n"
    "    return a + b;\n"
    "}\n"
    "\n"
    "export function greet(name: string): string\n"
    "{\n"
    "    return \"hello \" + name;\n"
    "}\n"
    "\n"
    "export class Calculator\n"
    "{\n"
    "    value: number = 0;\n"
    "    add(n: number): number\n"
    "    {\n"
    "        this.value += n;\n"
    "        return this.value;\n"
    "    }\n"
    "}\n"
    "\n"
    "export function main()\n"
    "{\n"
    "    return add(1, 2) + greet(\"x\").length;\n"
    "}\n";

// 行号(1-based)对照：
//   1  export function add(...)
//   2      return a + b;
//   3  }
//   4  (空)
//   5  export function greet(...)
//   6      return "hello " + name;
//   7  }
//   8  (空)
//   9  export class Calculator
//  10      value: number = 0;
//  11      add(n: number): number
//  12          this.value += n;
//  13          return this.value;
//  14      }
//  15  }
//  16  (空)
//  17  export function main()
//  18      return add(1, 2) + greet("x").length;
//  19  }

static const char *TS_CONFIG =
    "{\n"
    "  \"compilerOptions\": {\n"
    "    \"target\": \"ES2022\",\n"
    "    \"module\": \"esnext\",\n"
    "    \"moduleResolution\": \"bundler\",\n"
    "    \"strict\": false,\n"
    "    \"outDir\": \"build\"\n"
    "  },\n"
    "  \"include\": [\"**/*.ts\"]\n"
    "}\n";

} // namespace

int main()
{
    JZApplication app;
    g_app = &app;

    printf("==== code_agent LSP test: 新建 tmp TS 工程 -> 验证 10 个 TSGO 接口 ====\n");

    // ---- 前置：tmp 根 / 编译器 ----
    string tmpRoot = normalize(JZPlatform::applicationTmpPath());
    if (tmpRoot.empty())
    {
        printf("[fatal] JZPlatform::applicationTmpPath() 为空\n");
        return 2;
    }
    string base = tmpRoot + "/code_agent_lsp_test";
    printf("tmp base: %s\n", base.c_str());
    rmRecursive(base);
    ensureDir(base);

    string tsgo = resolveTsgoLsp();
    if (tsgo.empty())
    {
        printf("[fatal] 未找到原生 tsgo(tsc) 二进制（默认 E:/libs/ts_compiler/.../tsc.exe，"
               "或用 JZ_TS_LSP 指定）\n");
        return 2;
    }
    printf("tsgo(tsc): %s\n", tsgo.c_str());

    // ================================================================
    // 1) 新建 TS 工程（tsconfig + sample.ts）
    // ================================================================
    g_sampleFile = base + "/sample.ts";
    bool wok = writeText(base + "/tsconfig.json", TS_CONFIG) &&
               writeText(g_sampleFile, SAMPLE_TS);
    check(wok, "新建 tmp TS 工程(tsconfig.json + sample.ts)", base);
    if (!wok)
        return 1;

    // ================================================================
    // 2) 启动 JZTsLangServer 并验证可用性
    // ================================================================
    JZTsLangServer server;
    connect(&server.sigLog, [](const string &t) {
        printf("  [tsgo] %s\n", t.c_str());
    });
    server.setTsgoPath(tsgo);
    server.setRoot(base);
    check(server.isAvailable(), "JZTsLangServer::isAvailable (tsc 存在)", tsgo);
    if (!server.isAvailable())
        return 2;

    // 入参构造小工具
    auto at = [](int line, int offset)
    {
        Json::Value p;
        p["file"] = g_sampleFile;
        p["line"] = line;
        p["offset"] = offset;
        return p;
    };
    // 实际样本行号(1-based，含首行注释)：
    //   1  // sample.ts ...
    //   2  export function add(...)
    //  24      return add(1, 2) + greet("x").length;   <- add 调用在 (24,12)
    //  12  export class Calculator                    <- 光标在类声明行 (12,1) 取成员
    auto fileOnly = []
    {
        Json::Value p;
        p["file"] = g_sampleFile;
        return p;
    };

    // ================================================================
    // 3~12) 顺序驱动 10 个 LSP 查询；全程只跑一个 exec()
    // （app.exit/quit 会永久置 m_running=false，不能多次 exec/exit）
    // ================================================================
    struct Step
    {
        const char *tag;
        TsMethod method;
        std::function<Json::Value()> buildParams;
        std::function<bool(bool, const Json::Value &, const string &)> verify;
    };

    vector<Step> steps;

    // 提前声明驱动状态（verify 回调里可能引用 issueNext，须在 steps 之前声明）
    size_t stepIdx = 0;
    auto state = std::make_shared<ReqState>();
    std::function<void()> issueNext;

    // 3) definition：main 里的 add 调用 -> 定位到 add 定义(第 2 行)
    steps.push_back({"definition: add 调用 -> add 定义(第2行)", TsMethod::Definition,
        [&]() { return at(24, 12); },
        [&](bool ok, const Json::Value &out, const string &) {
            bool good = ok && out.isArray() && out.size() >= 1 &&
                        fileMatches(out[0].get("file", "").asString()) &&
                        out[0].get("line", 0).asInt() == 2;
            if (good)
                printf("   -> %s:%d:%d\n", out[0].get("file", "").asString().c_str(),
                       out[0].get("line", 0).asInt(), out[0].get("offset", 0).asInt());
            return good;
        }});

    // 4) references：add 应出现 2 处（定义 + 调用）
    steps.push_back({"references: add 出现 >=2 处(定义+调用)", TsMethod::References,
        [&]() { return at(24, 12); },
        [&](bool ok, const Json::Value &out, const string &) {
            bool good = ok && out.isArray() && out.size() >= 2;
            int selfCnt = 0;
            if (good)
                for (const auto &l : out)
                    if (fileMatches(l.get("file", "").asString()))
                        selfCnt++;
            if (good && selfCnt < 2)
                good = false;
            if (ok)
                printf("   (共 %d 处, 本文件 %d)\n", out.size(), selfCnt);
            return good;
        }});

    // 5) quickinfo：悬停 add 应返回类型/签名信息
    steps.push_back({"quickinfo: add 悬停返回内容", TsMethod::QuickInfo,
        [&]() { return at(24, 12); },
        [&](bool ok, const Json::Value &out, const string &) {
            bool good = ok && out.isObject() && out.isMember("contents");
            if (good)
            {
                Json::Value c = out["contents"];
                string txt = c.isString() ? c.asString()
                                : (c.isObject() ? c.get("value", "").asString() : string());
                good = !txt.empty();
            }
            return good;
        }});

    // 6) rename：等同 references（含声明）应 >=2
    steps.push_back({"rename: 重命名定位(含声明) >=2 处", TsMethod::Rename,
        [&]() { return at(24, 12); },
        [&](bool ok, const Json::Value &out, const string &) {
            bool good = ok && out.isArray() && out.size() >= 2;
            if (ok)
                printf("   (共 %d 处)\n", out.size());
            return good;
        }});

    // 7) completion：在 add 调用后补全会返回候选项
    steps.push_back({"completion: 返回补全候选项(>0)", TsMethod::Completion,
        [&]() { return at(24, 12); },
        [&](bool ok, const Json::Value &out, const string &) {
            bool good = ok && out.isArray() && out.size() > 0;
            if (ok)
                printf("   (共 %d 项)\n", out.size());
            return good;
        }});

    // 8) document_highlights：返回高亮数组（TSGO 对部分符号可能返回空数组，
    //    此处只验证接口可用、返回结构合法）
    steps.push_back({"document_highlights: 返回高亮数组(接口可用)", TsMethod::DocumentHighlights,
        [&]() { return at(24, 12); },
        [&](bool ok, const Json::Value &out, const string &) {
            bool good = ok && out.isArray();
            if (ok)
                printf("   (共 %d 处)\n", out.size());
            return good;
        }});

    // 9) diagnostics：合法文件应返回 items 数组（可空）
    steps.push_back({"diagnostics: 返回 items 数组(合法文件可空)", TsMethod::Diagnostics,
        [&]() { return fileOnly(); },
        [&](bool ok, const Json::Value &out, const string &) {
            return ok && out.isArray();
        }});

    // 10) source：取 add 定义处的源码文本
    steps.push_back({"source: 返回 add 定义源码(含 'return a + b')", TsMethod::Source,
        [&]() { Json::Value p = at(24, 12); p["max_chars"] = 4000; return p; },
        [&](bool ok, const Json::Value &out, const string &raw) {
            bool hasText = false;
            if (ok)
            {
                Json::Value parsed;
                if (parsePayload(raw, parsed) && parsed.isString())
                    hasText = parsed.asString().find("return a + b") != string::npos;
            }
            return ok && hasText;
        }});

    // 11) outline：文档符号树应包含 add/greet/Calculator/main
    steps.push_back({"outline: 符号树含 add/greet/Calculator/main", TsMethod::Outline,
        [&]() { return fileOnly(); },
        [&](bool ok, const Json::Value &out, const string &) {
            return ok && out.isArray() &&
                   symArrHasName(out, "add") && symArrHasName(out, "greet") &&
                   symArrHasName(out, "Calculator") && symArrHasName(out, "main");
        }});

    // 12) members：光标在 class 声明行 -> 返回类的成员(value/add)
    steps.push_back({"members: Calculator 成员含 value/add", TsMethod::Members,
        [&]() { return at(12, 1); },
        [&](bool ok, const Json::Value &out, const string &) {
            bool good = ok && out.isArray() && out.size() >= 2 &&
                        symArrHasName(out, "value") && symArrHasName(out, "add");
            if (ok)
                printf("   (共 %d 个成员)\n", out.size());
            return good;
        }});

    // 13) 异常自愈：模拟底层进程崩溃(killProcessForTest)，验证服务能自动重启。
    //     该步拿到一次正常响应后杀掉进程，并延迟再发下一步（等重启握手完成），
    //     避免把请求派发到已死的进程上。
    steps.push_back({"recovery: kill 进程后自动重启", TsMethod::Definition,
        [&]() { return at(24, 12); },
        [&](bool ok, const Json::Value &out, const string &) {
            bool good = ok && out.isArray() && out.size() >= 1 &&
                        fileMatches(out[0].get("file", "").asString()) &&
                        out[0].get("line", 0).asInt() == 2;
            if (good)
            {
                printf("   (崩溃前正常响应, 准备 kill 进程)\n");
                server.killProcessForTest();   // 模拟崩溃 -> onFinished -> 自动重启
                g_deferNext = true;            // 本步自行安排下一步的发起时机
                JZTimer::singleShot(800, [&]() { issueNext(); });
            }
            return good;
        }});

    // 14) 重启后再次查询，应当恢复正常（definition 仍能定位 add 定义）
    steps.push_back({"recovery: 重启后定义查询恢复正常", TsMethod::Definition,
        [&]() { return at(24, 12); },
        [&](bool ok, const Json::Value &out, const string &) {
            bool good = ok && out.isArray() && out.size() >= 1 &&
                        fileMatches(out[0].get("file", "").asString()) &&
                        out[0].get("line", 0).asInt() == 2;
            if (good)
                printf("   -> 重启后定位 %s:%d:%d\n", out[0].get("file","").asString().c_str(),
                       out[0].get("line",0).asInt(), out[0].get("offset",0).asInt());
            return good;
        }});

    // ---- 顺序驱动：每个请求回调里做完断言再发下一个；全程单 exec() ----
    issueNext = [&]() {
        if (stepIdx >= steps.size())
        {
            if (g_app)
                g_app->exit(0);
            return;
        }
        const Step &s = steps[stepIdx];
        Json::Value p = s.buildParams();
        state = std::make_shared<ReqState>();
        server.request(s.method, p,
                       [s, &stepIdx, &issueNext, state](bool o, const string &pl) {
                           state->ok = o;
                           state->raw = pl;
                           if (o)
                               parsePayload(pl, state->out);
                           state->done = true;
                           bool good = s.verify(o, state->out, state->raw);
                           check(good, s.tag, o ? state->raw : state->raw);
                           stepIdx++;
                           if (g_deferNext)
                           {
                               g_deferNext = false; // 延迟步会自行调用 issueNext
                           }
                           else
                           {
                               issueNext();
                           }
                       });
    };

    // 整体看门狗：防止任一请求卡死导致测试无限挂起
    thread watchdog([&]() {
        this_thread::sleep_for(chrono::seconds(120));
        if (stepIdx < steps.size())
        {
            printf("[FAIL] 整体超时: 仍有 %d/%d 个请求未完成\n",
                   (int)(steps.size() - stepIdx), (int)steps.size());
            if (g_app)
                g_app->exit(1);
        }
    });
    watchdog.detach();

    issueNext();
    g_app->exec();

    // 收尾：析构会 shutdown 子进程
    server.shutdown();

    printf("\n==== 结果: %s (%d 失败) ====\n", g_failed ? "FAIL" : "ALL PASS", g_failed);
    return g_failed ? 1 : 0;
}
