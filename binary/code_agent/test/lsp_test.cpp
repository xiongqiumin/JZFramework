// code_agent_lsp_test - 直连 TSGO(tsc --lsp --stdio) LSP 接口的冒烟测试。
//
// 流程（headless、无 GUI、不用大模型）：
//   1. 在 JZPlatform::applicationTmpPath() 下新建一个最小 TS 工程（tsconfig.json +
//      sample.ts：含函数 / 类 / 调用，便于验证定义、引用、成员等各类查询）；
//   2. 创建 JZTsLangServer，指定原生 tsc.exe 路径（即 tsgo 语言服务），setRoot 到该工程；
//   3. 用事件循环(JZApplication::processEvents)驱动异步回调，逐个调用 7 个查找型 tool：
//        source / references / quickinfo / diagnostics / outline / members / symbols
//      一律以「file + 符号名」定位（name / query 参数），不使用行列参数；
//      并断言返回结构正确、定位/内容符合预期。另验证崩溃自愈（kill -> 自动重启）。
//   4. 结构化编辑闭环（JZTsEditTool，8 步）：add_class / summary / add_function /
//      replace_function / rename_function / delete_function / delete_class，
//      验证文件内容真实变化后再复原。
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
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "JZApplication.h"
#include "JZDir.h"
#include "JZFileInfo.h"
#include "JZPlatform.h"
#include "JZSignal.h"
#include "JZTimer.h"
#include "JZTsEditTools.h"
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

// 行号(1-based，含首行注释)对照：
//   1  // sample.ts ...
//   2  export function add(...)
//   3      return a + b;
//   6  export function greet(...)
//  10  export class Calculator
//  11      value: number = 0;
//  12      add(n: number): number
//  18  export function main()
//  19      return add(1, 2) + greet("x").length;

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

    printf("==== code_agent LSP test: 新建 tmp TS 工程 -> 验证 7 个 TSGO 查找型接口 ====\n");

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

    // 入参构造小工具：file + 符号名（不再使用行列定位）
    auto byName = [](const string &name)
    {
        Json::Value p;
        p["file"] = g_sampleFile;
        p["name"] = name;
        return p;
    };
    auto fileOnly = []
    {
        Json::Value p;
        p["file"] = g_sampleFile;
        return p;
    };

    // ================================================================
    // 3~11) 顺序驱动 7 个查找型查询 + 2 个自愈步骤；全程只跑一个 exec()
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
    std::function<void()> eNext; // 编辑闭环段的驱动器（11 步完成后由 issueNext 启动）

    // 3) source：按符号名查 add 的声明源码（add 在第 2 行）
    steps.push_back({"source: add 声明源码(含 'return a + b', line=2)", TsMethod::Source,
        [&]() {
            Json::Value p = byName("add");
            p["max_chars"] = 4000;
            return p;
        },
        [&](bool ok, const Json::Value &out, const string &) {
            bool good = ok && out.isObject() &&
                        out.get("line", 0).asInt() == 2 &&
                        out.get("source", "").asString().find("return a + b") != string::npos &&
                        fileMatches(out.get("file", "").asString());
            if (good)
                printf("   -> %s:%d~%d\n", out.get("file", "").asString().c_str(),
                       out.get("line", 0).asInt(), out.get("endLine", 0).asInt());
            return good;
        }});

    // 4) references：按符号名查 add 应出现 2 处（定义 + 调用），且带所在行文本
    steps.push_back({"references: add 出现 >=2 处(定义+调用, 带行文本)", TsMethod::References,
        [&]() { return byName("add"); },
        [&](bool ok, const Json::Value &out, const string &) {
            bool good = ok && out.isArray() && out.size() >= 2;
            int selfCnt = 0, withText = 0;
            if (good)
                for (const auto &l : out)
                {
                    if (fileMatches(l.get("file", "").asString()))
                        selfCnt++;
                    if (!l.get("text", "").asString().empty())
                        withText++;
                }
            if (good && (selfCnt < 2 || withText < 1))
                good = false;
            if (ok)
                printf("   (共 %d 处, 本文件 %d, 带文本 %d)\n",
                       (int)out.size(), selfCnt, withText);
            return good;
        }});

    // 5) quickinfo：按符号名查 add 应返回类型/签名信息
    steps.push_back({"quickinfo: add 返回类型签名", TsMethod::QuickInfo,
        [&]() { return byName("add"); },
        [&](bool ok, const Json::Value &out, const string &) {
            return ok && out.isObject() &&
                   !out.get("contents", "").asString().empty();
        }});

    // 6) diagnostics：合法文件应返回数组（可空）
    steps.push_back({"diagnostics: 返回数组(合法文件可空)", TsMethod::Diagnostics,
        [&]() { return fileOnly(); },
        [&](bool ok, const Json::Value &out, const string &) {
            return ok && out.isArray();
        }});

    // 7) outline：归一化符号树应包含 add/greet/Calculator/main
    steps.push_back({"outline: 符号树含 add/greet/Calculator/main", TsMethod::Outline,
        [&]() { return fileOnly(); },
        [&](bool ok, const Json::Value &out, const string &) {
            return ok && out.isArray() &&
                   symArrHasName(out, "add") && symArrHasName(out, "greet") &&
                   symArrHasName(out, "Calculator") && symArrHasName(out, "main");
        }});

    // 8) members：按符号名取 Calculator 的成员（value/add）
    steps.push_back({"members: Calculator 成员含 value/add", TsMethod::Members,
        [&]() { return byName("Calculator"); },
        [&](bool ok, const Json::Value &out, const string &) {
            bool good = ok && out.isArray() && out.size() >= 2 &&
                        symArrHasName(out, "value") && symArrHasName(out, "add");
            if (ok)
                printf("   (共 %d 个成员)\n", (int)out.size());
            return good;
        }});

    // 9) symbols：项目内模糊搜索 "Calc" 应命中 Calculator 并给出所在文件
    steps.push_back({"symbols: 模糊搜索 'Calc' 命中 Calculator", TsMethod::WorkspaceSymbols,
        [&]() {
            Json::Value p;
            p["query"] = "Calc";
            return p;
        },
        [&](bool ok, const Json::Value &out, const string &) {
            bool good = ok && out.isArray();
            bool hit = false;
            if (good)
                for (const auto &s : out)
                    if (s.get("name", "").asString() == "Calculator" &&
                        fileMatches(s.get("file", "").asString()))
                        hit = true;
            if (ok)
                printf("   (命中 %d 个符号)\n", (int)out.size());
            return good && hit;
        }});

    // 10) 异常自愈：模拟底层进程崩溃(killProcessForTest)，验证服务能自动重启。
    //     该步拿到一次正常响应后杀掉进程，并延迟再发下一步（等重启握手完成），
    //     避免把请求派发到已死的进程上。
    steps.push_back({"recovery: kill 进程后自动重启", TsMethod::Definition,
        [&]() { return byName("add"); },
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

    // 11) 重启后再次查询，应当恢复正常（definition 仍能定位 add 定义）
    steps.push_back({"recovery: 重启后定义查询恢复正常", TsMethod::Definition,
        [&]() { return byName("add"); },
        [&](bool ok, const Json::Value &out, const string &) {
            bool good = ok && out.isArray() && out.size() >= 1 &&
                        fileMatches(out[0].get("file", "").asString()) &&
                        out[0].get("line", 0).asInt() == 2;
            if (good)
                printf("   -> 重启后定位 %s:%d\n", out[0].get("file","").asString().c_str(),
                       out[0].get("line",0).asInt());
            return good;
        }});

    // ================================================================
    // 12~19) 结构化编辑闭环（JZTsEditTool，不设 agent -> 跳过路径权限）：
    //   add_class -> summary(file) -> add_function -> replace_function ->
    //   rename_function -> delete_function -> delete_class ->
    //   summary(project, 无 agent 应拒绝)
    // ================================================================
    using K = JZTsEditTool::Kind;
    string scope2 = g_sampleFile + "#Vector2";
    auto readFileAll = [](const string &path)
    {
        ifstream in(normalize(path), ios::binary);
        stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    };

    struct EditStep
    {
        const char *tag;
        K kind;
        std::function<Json::Value()> buildParams;
        std::function<bool(const string &)> verify; // result 文本
        bool expectOk;                              // 负向用例 false（VS14 无默认成员初始化器的聚合初始化）
    };
    vector<EditStep> esteps;
    size_t eIdx = 0;

    map<K, std::unique_ptr<JZTsEditTool>> etools;
    std::function<JZTsEditTool &(K)> etool = [&](K k) -> JZTsEditTool &
    {
        auto it = etools.find(k);
        if (it == etools.end())
        {
            auto p = std::make_unique<JZTsEditTool>(&server, k);
            it = etools.emplace(k, std::move(p)).first;
            connect(&it->second->sigResult,
                    [&](const ToolCallResult &r) {
                        bool good = (r.isOk == esteps[eIdx].expectOk) &&
                                    esteps[eIdx].verify(r.result);
                        check(good, esteps[eIdx].tag, r.result);
                        eIdx++;
                        eNext();
                    });
        }
        return *it->second;
    };

    esteps.push_back({"edit: add_class Vector2", K::AddClass,
        [&]() {
            Json::Value p;
            p["file"] = g_sampleFile;
            p["name"] = "Vector2";
            p["code"] = "export class Vector2 {\n    x: number = 0;\n}";
            return p;
        },
        [&](const string &r) { return r.find("添加类 Vector2") != string::npos; }, true});

    // （summary 的 file/class 级已删除——与 ts_outline / ts_members 重复）

    esteps.push_back({"edit: add_function Vector2.len", K::AddFunction,
        [&]() {
            Json::Value p;
            p["scope"] = scope2;
            p["name"] = "len";
            p["code"] = "len(): number {\n        return this.x;\n    }";
            return p;
        },
        [&](const string &r) { return r.find("添加方法 len") != string::npos; }, true});

    esteps.push_back({"edit: replace_function len", K::ReplaceFunction,
        [&]() {
            Json::Value p;
            p["scope"] = scope2;
            p["name"] = "len";
            p["code"] = "len(): number {\n        return this.x * 2;\n    }";
            return p;
        },
        [&](const string &r) {
            return r.find("已替换") != string::npos &&
                   readFileAll(g_sampleFile).find("this.x * 2") != string::npos; }, true});

    esteps.push_back({"edit: rename_function len -> len2", K::RenameFunction,
        [&]() {
            Json::Value p;
            p["scope"] = scope2;
            p["name"] = "len";
            p["new_name"] = "len2";
            return p;
        },
        [&](const string &r) {
            return r.find("len2") != string::npos &&
                   readFileAll(g_sampleFile).find("len2(): number") != string::npos; }, true});

    esteps.push_back({"edit: delete_function len2", K::DeleteFunction,
        [&]() {
            Json::Value p;
            p["scope"] = scope2;
            p["name"] = "len2";
            return p;
        },
        [&](const string &r) {
            return r.find("已删除") != string::npos &&
                   readFileAll(g_sampleFile).find("len2") == string::npos; }, true});

    esteps.push_back({"edit: delete_class Vector2", K::DeleteClass,
        [&]() {
            Json::Value p;
            p["file"] = g_sampleFile;
            p["name"] = "Vector2";
            return p;
        },
        [&](const string &r) {
            return r.find("已删除") != string::npos &&
                   readFileAll(g_sampleFile).find("Vector2") == string::npos; }, true});

    esteps.push_back({"edit: ts_summary 无 agent 应拒绝", K::Summary,
        [&]() {
            Json::Value p;
            return p;
        },
        [&](const string &r) { return r.find("需要 agent 工作区") != string::npos; },
        false});

    // ---- 顺序驱动：每个请求回调里做完断言再发下一个；全程单 exec() ----
    issueNext = [&]() {
        if (stepIdx >= steps.size())
        {
            eNext(); // 查询步全部完成后进入编辑闭环段
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

    // 编辑闭环段驱动器：每步经 sigResult 回调后做断言再发下一步
    eNext = [&]() {
        if (eIdx >= esteps.size())
        {
            if (g_app)
                g_app->exit(0);
            return;
        }
        auto &s = esteps[eIdx];
        ToolCall tc;
        tc.id = "edit-step";
        tc.params = s.buildParams();
        etool(s.kind).call(tc);
    };

    // 整体看门狗：防止任一请求卡死导致测试无限挂起
    thread watchdog([&]() {
        this_thread::sleep_for(chrono::seconds(120));
        if (stepIdx < steps.size() || eIdx < esteps.size())
        {
            printf("[FAIL] 整体超时: 查询 %d/%d, 编辑 %d/%d 未完成\n",
                   (int)(steps.size() - stepIdx), (int)steps.size(),
                   (int)(esteps.size() - eIdx), (int)esteps.size());
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
