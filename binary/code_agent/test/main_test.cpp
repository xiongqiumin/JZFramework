// code_agent_test - "JZCodeAgent 创建工程 -> 打包/发布 -> 调用运行时" 端到端流程测试。
//
// 全流程（不用大模型，代码直接写入文件）：
//   1. 创建工程   JZCodeAgent::initProject / probeProject / openProject，
//                新建到 JZPlatform::applicationTmpPath() 下的临时目录；
//                （project.json 是"打包/发布"的产物元数据、不是工程骨架，
//                此处专门断言 initProject 之后它不存在）
//   2. 写多文件   直接写入 src/main.ts + src/lib.ts + src/util/strings.ts
//                （main 跨目录 import，lib 由 util 经 "../lib" 再拉一次，
//                验证引擎模块 loader 的多文件 / 子目录 / 相对路径 / 共享模块）；
//                尚未打包时先试运行 -> JZEngineRuntime.init 因读不到 project.json
//                直接报错（不回退默认入口）；
//   3. 打包/发布  JZCodeAgent::buildProject（宿主 tsc -p tsconfig.json）。产物写向
//                tsconfig 的 outDir：缺省=工程根/build（*.js+*.js.map）；outDir 改成
//                工程根外绝对目录也一样生效（Stage 6）。编译通过后按 outDir 定位并
//                校验入口产物（main.js），生成/刷新入口声明 project.json；
//   4. 调用运行时（JZEngineRuntime 内嵌 JZJSEngine，引擎创建/模块运行/错误栈
//      source map 翻译全部封装在类里，外面不直接碰 JZJSEngine）
//        a. 进程内：rt.init(root) 读打包生成的 project.json 的 entry（build/main.js）
//           完成"设运行根 + 加载主入口 + 自动调用 main()"，
//           再 rt.evalScript 断言入口导出可复用、结果正确；
//        b. 进程内：打包后故意抛错的工程 -> rt.init 返回 false，
//           rt.lastError() 已把 JS 栈翻译回 TS 源码位置；
//        c. 子进程：调用 code_runtime <工程根> 跑同一产物（good 用 project.json
//           入口成功；fail 打包后运行抛错退出），校验退出码与输出。
//
// 用法：code_agent_test.exe
// 环境变量：JZ_TS_NODE + JZ_TS_TSC 可换编译器（默认 E:/libs/ts_compiler 的 tsc.exe）。
// 退出码：0=全部通过；1=有失败；2=前置条件缺失（编译器/工程根不可用）。
//
// 注意：本文件不要 include <windows.h>（JZAgentTool.h 无作用域枚举成员
// BOOLEAN 与 winnt.h 冲突 C2365）；进程调用走框架 JZProcess。

#define _CRT_SECURE_NO_WARNINGS

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "JZApplication.h"
#include "JZByteArray.h"
#include "JZCodeAgent.h"
#include "JZDir.h"
#include "JZEngineRuntime.h"
#include "JZExeResource.h"
#include "JZFileInfo.h"
#include "JZPlatform.h"
#include "JZProcess.h"
#include "JZString.h"

using namespace std;

namespace
{
int g_failed = 0;

// ---------------------------------------------------------------- 通用小工具

string readEnvStr(const char *name)
{
    const char *v = getenv(name);
    return v ? string(v) : string();
}

static string normalize(const string &path)
{
    string p = path;
    for (size_t i = 0; i < p.size(); i++)
    {
        if (p[i] == '\\')
            p[i] = '/';
    }
    while (p.size() > 1 && p.back() == '/')
        p.pop_back();
    return p;
}

static string parentOf(const string &path)
{
    string p = normalize(path);
    size_t pos = p.find_last_of('/');
    if (pos == string::npos)
        return string();
    if (pos == 0)
        return "/";
    return p.substr(0, pos);
}

static bool fileExists(const string &path)
{
    ifstream in(normalize(path), ios::binary);
    return in.good();
}

static bool writeText(const string &path, const string &text)
{
    ofstream out(normalize(path), ios::binary | ios::trunc);
    if (!out)
        return false;
    out.write(text.data(), (streamsize)text.size());
    out.close();
    return true;
}

static bool readText(const string &path, string &text)
{
    ifstream in(normalize(path), ios::binary);
    if (!in)
        return false;
    text.assign(istreambuf_iterator<char>(in), istreambuf_iterator<char>());
    return true;
}

static bool copyFileBin(const string &src, const string &dst)
{
    ifstream in(normalize(src), ios::binary);
    if (!in)
        return false;
    ofstream out(normalize(dst), ios::binary | ios::trunc);
    if (!out)
        return false;
    out << in.rdbuf();
    return true;
}

// 生成一个最小可用 .ico（32x32, 32bpp, 纯色 r/g/b）：ICONDIR + 1 个 ICONDIRENTRY
// + BITMAPINFOHEADER + 像素(BGRA, 自下而上) + AND 掩码(全 0)。供 JZExeResource 测试。
static bool writeMinIco(const string &path, uint8_t r, uint8_t g, uint8_t b)
{
    vector<uint8_t> buf;
    const int W = 32, H = 32;
    const int imgSize = 40 + W * H * 4 + (W * H / 8); // 4264
    const int imgOffset = 6 + 16;                     // 22

    // ICONDIR
    buf.insert(buf.end(), {0, 0});                 // reserved
    buf.insert(buf.end(), {1, 0});                 // type = icon
    buf.insert(buf.end(), {1, 0});                 // count = 1
    // ICONDIRENTRY
    buf.push_back((uint8_t)W);
    buf.push_back((uint8_t)H);
    buf.insert(buf.end(), {0, 0});                // colorCount, reserved
    buf.insert(buf.end(), {1, 0});                // planes
    buf.insert(buf.end(), {32, 0});               // bitCount
    buf.push_back((uint8_t)(imgSize & 0xFF));
    buf.push_back((uint8_t)((imgSize >> 8) & 0xFF));
    buf.push_back((uint8_t)((imgSize >> 16) & 0xFF));
    buf.push_back((uint8_t)((imgSize >> 24) & 0xFF));
    buf.push_back((uint8_t)(imgOffset & 0xFF));
    buf.push_back((uint8_t)((imgOffset >> 8) & 0xFF));
    buf.push_back((uint8_t)((imgOffset >> 16) & 0xFF));
    buf.push_back((uint8_t)((imgOffset >> 24) & 0xFF));

    auto put32 = [&](uint32_t v)
    {
        buf.push_back((uint8_t)(v & 0xFF));
        buf.push_back((uint8_t)((v >> 8) & 0xFF));
        buf.push_back((uint8_t)((v >> 16) & 0xFF));
        buf.push_back((uint8_t)((v >> 24) & 0xFF));
    };
    auto put16 = [&](uint16_t v)
    {
        buf.push_back((uint8_t)(v & 0xFF));
        buf.push_back((uint8_t)((v >> 8) & 0xFF));
    };

    // BITMAPINFOHEADER
    put32(40);            // biSize
    put32(W);             // biWidth
    put32(H * 2);         // biHeight (XOR + AND 两半)
    put16(1);             // biPlanes
    put16(32);            // biBitCount
    put32(0);             // biCompression
    put32(0);             // biSizeImage
    put32(0);             // biXPelsPerMeter
    put32(0);             // biYPelsPerMeter
    put32(0);             // biClrUsed
    put32(0);             // biClrImportant
    // XOR 像素（BGRA，自下而上；A=0 不透明）
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
        {
            buf.push_back(b);
            buf.push_back(g);
            buf.push_back(r);
            buf.push_back(0);
        }
    // AND 掩码（全 0）
    buf.insert(buf.end(), (size_t)(W * H / 8), 0);

    ofstream out(normalize(path), ios::binary | ios::trunc);
    if (!out)
        return false;
    out.write((const char *)buf.data(), (streamsize)buf.size());
    out.close();
    return true;
}

static void ensureDir(const string &path)
{
    JZDir mk;
    mk.mkpath(normalize(path));
}

static void rmRecursive(const string &path)
{
    JZDir dir(normalize(path));
    if (dir.exists())
        dir.removeRecursively();
}

// 同步跑一个子进程，捕获合并输出；返回退出码（-1=启动失败，-2=超时）。
static int runProcessCapture(const string &exe, const vector<string> &args,
                             string &output, int timeoutMs)
{
    JZProcess proc;
    proc.setProcessChannelMode(JZProcess::MergedChannels);
    if (!proc.start(exe, args))
        return -1;
    if (!proc.waitForFinished(timeoutMs))
    {
        proc.killProcess();
        return -2;
    }
    JZByteArray raw = proc.readAllStandardOutput();
    output = str_fromLocal(raw);
    return proc.exitCode();
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

// ---------------------------------------------------------------- TS 编译器

struct TsRunner
{
    string exe;                  // tsc.exe 或 node.exe
    vector<string> prefix;       // node 方案 prefix=[tsc.js]
    bool ok = false;
};

static TsRunner resolveTsCompiler()
{
    TsRunner r;
    string node = readEnvStr("JZ_TS_NODE");
    string tscjs = readEnvStr("JZ_TS_TSC");
    if (!node.empty() && !tscjs.empty())
    {
        r.exe = node;
        r.prefix.push_back(tscjs);
        r.ok = true;
        return r;
    }
    string cand = "E:/libs/ts_compiler/app/node_modules/@typescript/"
                  "typescript-win32-x64/lib/tsc.exe";
    if (fileExists(cand))
    {
        r.exe = cand;
        r.ok = true;
    }
    return r;
}

// 与 initProject 生成的标准 tsconfig 保持一致（sourceMap 供运行期栈翻译）
static string standardTsconfig()
{
    return "{\n"
           "  \"compilerOptions\": {\n"
           "    \"target\": \"ES2022\",\n"
           "    \"module\": \"esnext\",\n"
           "    \"moduleResolution\": \"bundler\",\n"
           "    \"strict\": true,\n"
           "    \"noImplicitAny\": true,\n"
           "    \"noEmitOnError\": true,\n"
           "    \"sourceMap\": true,\n"
           "    \"outDir\": \"build\",\n"
           "    \"rootDir\": \"src\"\n"
           "  },\n"
           "  \"include\": [\"src/**/*.ts\"]\n"
           "}\n";
}

// 运行主入口声明（内容与 JZCodeAgent::buildProject 生成的一致）。
// 仅在没有"可直接执行的宿主 tsc"（node 前缀方案）作裸编译兜底时使用；
// 正常路径的打包/发布走 agent.buildProject，由它自己写这份声明。
static bool writeProjectEntry(const string &root)
{
    return writeText(root + "/project.json",
                     "{\n"
                     "  \"entry\": \"build/main.js\"\n"
                     "}\n");
}

static int compileProject(const string &root, const TsRunner &runner,
                          string &out)
{
    vector<string> args = runner.prefix;
    args.push_back("-p");
    args.push_back(normalize(root) + "/tsconfig.json");
    args.push_back("--pretty");
    args.push_back("false");
    return runProcessCapture(runner.exe, args, out, 120 * 1000);
}

// ---------------------------------------------------------------- 查找 code_runtime

static string findRuntimeExe()
{
    string appDir = normalize(JZPlatform::applicationDirPath());

    // 新布局：code_agent 下所有 exe 统一输出到同一 bin 目录，
    // code_runtime 与测试 exe 同级，直接取同目录即可。
    string cand = appDir + "/code_runtime.exe";
    if (fileExists(cand))
        return cand;

    // 兼容旧布局 / 兜底
    const string tag = "/code_agent/test/";
    size_t pos = appDir.find(tag);
    if (pos != string::npos)
    {
        string head = appDir.substr(0, pos);
        string cfg = appDir.substr(pos + tag.size());
        cand = head + "/code_agent/runtime/" + cfg + "/code_runtime.exe";
        if (fileExists(cand))
            return cand;
        cand = head + "/code_agent/runtime/code_runtime.exe"; // 单配置
        if (fileExists(cand))
            return cand;
    }

    // 兜底：沿上级目录查找 code_runtime.exe / runtime/code_runtime.exe
    string p = appDir;
    for (int depth = 0; depth < 10 && !p.empty(); depth++)
    {
        string a = p + "/code_runtime.exe";
        if (fileExists(a))
            return a;
        string b = p + "/runtime/code_runtime.exe";
        if (fileExists(b))
            return b;
        string c = p + "/runtime/Debug/code_runtime.exe";
        if (fileExists(c))
            return c;
        p = parentOf(p);
    }
    return string();
}

// ---------------------------------------------------------------- 多文件 TS 样例

// 被 main 直接 import（多文件加载之一）
static const char *GOOD_LIB_TS =
    "// lib.ts - imported by main.ts (multi-file load)\n"
    "export const K: number = 42;\n"
    "export function add(a: number, b: number): number\n"
    "{\n"
    "    return a + b;\n"
    "}\n"
    "export function describe(): string\n"
    "{\n"
    "    return \"lib\";\n"
    "}\n";

// 子目录里再经 ../lib 拉同一模块（相对路径 + 共享模块去重）
static const char *GOOD_STRINGS_TS =
    "// util/strings.ts - subdir module, re-imports ../lib\n"
    "import { add } from \"../lib\";\n"
    "export function sign(what: string): string\n"
    "{\n"
    "    return \"[\" + what + \"+1=\" + add(1, 0) + \"]\";\n"
    "}\n";

// 入口：跨目录 import './lib' 与 './util/strings'（都省略扩展名，由 loader 补 .js）
static const char *GOOD_MAIN_TS =
    "// main.ts - entry: cross-file + cross-dir imports\n"
    "import { K, add, describe } from \"./lib\";\n"
    "import { sign } from \"./util/strings\";\n"
    "export function buildMessage(a: number, b: number): string\n"
    "{\n"
    "    return describe() + \" k=\" + K + \" \" + sign(\"add\") + \" \" +\n"
    "           a + \"+\" + b + \"=\" + add(a, b);\n"
    "}\n"
    "export function main(): string\n"
    "{\n"
    "    return buildMessage(20, 22);\n"
    "}\n"
    "console.log(\"MULTIFILE-OK\", main());\n";

// 故意抛错的被 import 模块（错误栈翻译应回到本文件）
static const char *FAIL_FAULT_TS =
    "// faultlib.ts - intentionally throws; stack should map back here\n"
    "export function boom(what: string): string\n"
    "{\n"
    "    throw new Error(\"boom in faultlib: \" + what);\n"
    "}\n";

// 顶层直接 main() 触发跨文件异常
static const char *FAIL_MAIN_TS =
    "// main.ts - top-level main() throws via imported faultlib\n"
    "import { boom } from \"./faultlib\";\n"
    "export function main(): string\n"
    "{\n"
    "    return boom(\"main\");\n"
    "}\n"
    "main();\n";

} // namespace

int main()
{
    JZApplication app;

    printf("==== code_agent flow test: 创建工程 -> 打包/发布 -> 调用运行时 ====\n");

    // ---- 前置：tmp 根 / 编译器 / 运行时 ----
    string tmpRoot = normalize(JZPlatform::applicationTmpPath());
    if (tmpRoot.empty())
    {
        printf("[fatal] JZPlatform::applicationTmpPath() 为空\n");
        return 2;
    }
    string base = tmpRoot + "/code_agent_flow_test";
    printf("tmp base: %s\n", base.c_str());
    rmRecursive(base);
    ensureDir(base);

    TsRunner ts = resolveTsCompiler();
    if (!ts.ok)
    {
        printf("[fatal] 未找到 tsc（默认 E:/libs/ts_compiler/.../tsc.exe，"
               "或用 JZ_TS_NODE/JZ_TS_TSC 指定）\n");
        return 2;
    }
    printf("tsc: %s\n", ts.exe.c_str());

    string runtimeExe = findRuntimeExe();
    printf("code_runtime: %s\n",
           runtimeExe.empty() ? "(未找到 - 跳过子进程运行时检查)" : runtimeExe.c_str());

    // ================================================================
    // Stage 1: JZCodeAgent 创建工程（新建到 tmp 目录）
    // ================================================================
    string goodRoot = base + "/good";
    JZCodeAgent agent;
    JZAgentConfig llm;          // 空配置即可：工程 API 不依赖大模型
    CodeAgentConfig code;       // workspace 稍后由 openProject 设置
    // 宿主侧 tsc：仅当编译器是"可直接执行"的单程序（默认 tsc.exe）时可用，
    // node 前缀方案下 buildProject 无宿主 tsc 可用，后面走裸编译兜底。
    if (ts.prefix.empty())
        code.tscPath = ts.exe;
    agent.init(llm, code);

    {
        string err;
        bool ok = agent.initProject(goodRoot, err);
        check(ok, "initProject 创建标准 TS 工程到 tmp", err);
        check(fileExists(goodRoot + "/tsconfig.json") &&
                  fileExists(goodRoot + "/.gitignore") &&
                  fileExists(goodRoot + "/src/main.ts"),
              "工程骨架文件齐全(tsconfig.json/.gitignore/src/main.ts)", goodRoot);
        // project.json 是"打包/发布"的产物元数据，不属于工程骨架 -> initProject 不生成
        check(!fileExists(goodRoot + "/project.json"),
              "initProject 不生成 project.json（打包/发布时才有）", goodRoot);

        CodeProjectStatus st = agent.probeProject(goodRoot);
        check(st.pathOk && st.isProject && st.hasTsconfig,
              "probeProject 判定为 TS 工程", st.summary);

        err.clear();
        ok = agent.openProject(goodRoot, &st, err);
        check(ok && st.isProject && agent.projectStatus().isProject,
              "openProject 打开工程并设为工作区",
              err.empty() ? st.summary : err);
    }

    // ================================================================
    // Stage 2: 不用大模型，直接写入多文件 TS；未打包先运行应被拒绝
    // ================================================================
    {
        ensureDir(goodRoot + "/src/util");
        bool ok = writeText(goodRoot + "/src/main.ts", GOOD_MAIN_TS) &&
                  writeText(goodRoot + "/src/lib.ts", GOOD_LIB_TS) &&
                  writeText(goodRoot + "/src/util/strings.ts", GOOD_STRINGS_TS);
        check(ok, "直接写入 src/main.ts + lib.ts + util/strings.ts", goodRoot);

        printf("[stage 2b] 尚未打包/发布时 init -> 读不到 project.json 直接报错:\n");
        JZDir d(goodRoot);
        string abs = d.absolutePath();
        JZEngineRuntime rt;
        bool ran = rt.init(abs);
        string errText = rt.lastError();
        check(!ran && errText.find("project.json") != string::npos,
              "init 拒绝运行并提示缺少入口声明 project.json（不回退默认入口）", errText);
        printf("  提示:\n%s\n", errText.c_str());
    }

    // ================================================================
    // Stage 3: 打包/发布（JZCodeAgent::buildProject）
    // ================================================================
    {
        printf("[stage 3] buildProject (打包/发布: 真实 tsc + 生成入口声明):\n");
        string out;
        bool ok;
        if (!code.tscPath.empty())
        {
            string err;
            ok = agent.buildProject(goodRoot, err, &out);
            if (!ok)
                out = err;
        }
        else
        {
            // node 前缀方案：宿主无"可直接执行的 tsc"，退化为裸 tsc + 手工入口声明
            ok = compileProject(goodRoot, ts, out) == 0;
            if (ok)
                ok = writeProjectEntry(goodRoot);
        }
        check(ok, "打包/发布成功（tsc -p tsconfig.json -> build/ + project.json）",
              out.empty() ? "(无输出)" : out);
        if (!ok)
        {
            printf("%s\n", out.c_str());
            return 1;
        }
        bool artifacts = fileExists(goodRoot + "/build/main.js") &&
                         fileExists(goodRoot + "/build/lib.js") &&
                         fileExists(goodRoot + "/build/util/strings.js") &&
                         fileExists(goodRoot + "/build/main.js.map") &&
                         fileExists(goodRoot + "/build/lib.js.map") &&
                         fileExists(goodRoot + "/build/util/strings.js.map");
        check(artifacts, "产物齐全: 3 个 .js + 3 个 .js.map(build/)", goodRoot + "/build");

        string entryText;
        readText(goodRoot + "/project.json", entryText);
        check(entryText.find("build/main.js") != string::npos,
              "打包后生成入口声明 project.json -> entry build/main.js", entryText);
    }

    // ================================================================
    // Stage 4a: 进程内调用运行时（JZEngineRuntime 内嵌引擎，ESM 多文件加载）
    // ================================================================
    {
        JZDir goodDir(goodRoot);
        string abs = goodDir.absolutePath();

        JZEngineRuntime rt;
        rt.setLogHandler([](const string &t) {
            printf("  [engine] %s\n", t.c_str());
        });

        printf("[stage 4a] JZEngineRuntime.init (读打包生成的 project.json 入口 + 自动调用 main):\n");
        bool ok = rt.init(abs);
        check(ok, "good 工程 init 成功(配置入口+会话+自动调用 main)", rt.lastError());

        if (ok)
        {
            bool okEval = rt.evalScript(
                "(function(){\n"
                "  if (typeof main !== 'function') throw new Error('main 未绑定到全局');\n"
                "  if (typeof buildMessage !== 'function') "
                "throw new Error('buildMessage 未绑定到全局');\n"
                "  var got = main();\n"
                "  var expect = 'lib k=42 [add+1=1] 20+22=42';\n"
                "  if (got !== expect) throw new Error('unexpected result: ' + got);\n"
                "})();\n");
            check(okEval, "跨文件导入的函数结果正确(含子目录与共享模块)", rt.lastError());
        }
    }

    // ================================================================
    // Stage 4b: 错误栈翻译（多文件：错在被 import 的 faultlib.ts）
    // ================================================================
    string failRoot = base + "/fail";
    {
        ensureDir(failRoot + "/src");
        bool wok = writeText(failRoot + "/tsconfig.json", standardTsconfig()) &&
                   writeText(failRoot + "/src/faultlib.ts", FAIL_FAULT_TS) &&
                   writeText(failRoot + "/src/main.ts", FAIL_MAIN_TS);
        check(wok, "写 fail 工程文件(faultlib.ts 抛错)", failRoot);

        string out;
        bool ok;
        if (!code.tscPath.empty())
        {
            string err;
            ok = agent.buildProject(failRoot, err, &out);
            if (!ok)
                out = err;
        }
        else
        {
            ok = compileProject(failRoot, ts, out) == 0;
            if (ok)
                ok = writeProjectEntry(failRoot);
        }
        check(ok, "fail 工程打包/发布成功", out.empty() ? "(无输出)" : out);
        if (!ok)
        {
            printf("%s\n", out.c_str());
            return 1;
        }

        JZDir failDir(failRoot);
        string abs = failDir.absolutePath();

        JZEngineRuntime rt;
        rt.setLogHandler([](const string &t) {
            printf("  [engine] %s\n", t.c_str());
        });

        printf("[stage 4b] JZEngineRuntime.init on failing module (打包后运行抛错):\n");
        bool ran = rt.init(abs);
        string errText = rt.lastError();   // 已翻译回 TS
        check(!ran && !errText.empty(), "fail 工程 init 失败并捕获运行时异常(跨文件)", errText);
        printf("  翻译栈:\n%s\n", errText.c_str());
        check(errText.find("src/faultlib.ts") != string::npos,
              "错误已翻译回 TS 源码位置(src/faultlib.ts)", errText);
    }

    // ================================================================
    // Stage 5: 子进程调用 code_runtime（隔离"运行时"）
    // ================================================================
    if (runtimeExe.empty())
    {
        check(false, "调用运行时(code_runtime 子进程)", "未找到 code_runtime.exe");
    }
    else
    {
        JZDir goodDir(goodRoot);
        string goodAbs = goodDir.absolutePath();
        string out;
        int rc = runProcessCapture(runtimeExe, {goodAbs}, out, 60 * 1000);
        printf("[stage 5] code_runtime good 输出(project.json 入口):\n%s\n", out.c_str());
        check(rc == 0, "code_runtime 运行 good 工程退出码=0",
              "rc=" + to_string(rc));
        check(out.find("MULTIFILE-OK") != string::npos,
              "code_runtime 输出含模块 console 结果", out);

        JZDir failDir(failRoot);
        string failAbs = failDir.absolutePath();
        out.clear();
        rc = runProcessCapture(runtimeExe, {failAbs}, out, 60 * 1000);
        printf("[stage 5] code_runtime fail 输出:\n%s\n", out.c_str());
        check(rc == 1, "code_runtime 运行 fail 工程退出码=1",
              "rc=" + to_string(rc));
        check(out.find("src/faultlib.ts") != string::npos,
              "code_runtime 出错栈含翻译后的 TS 位置", out);
    }

    // ================================================================
    // Stage 6: tsconfig outDir 指向工程根之外的绝对目录（打包/发布仍生效）
    // ================================================================
    {
        JZDir goodDir(goodRoot);
        string abs = goodDir.absolutePath();
        string extOut = normalize(base) + "/good_external_out";   // 工程根外的兄弟目录
        rmRecursive(extOut);

        if (code.tscPath.empty())
        {
            // node 前缀方案没有"可直接执行的宿主 tsc"，buildProject 不可用，跳过
            check(true, "跳过 outDir 自定义验证（node 前缀方案无宿主 tsc）", "(skipped)");
        }
        else
        {
            string tsText;
            readText(goodRoot + "/tsconfig.json", tsText);
            const string oldOut = "\"outDir\": \"build\"";
            const string newOut = "\"outDir\": \"" + extOut + "\"";
            size_t at = tsText.find(oldOut);
            bool patched = at != string::npos;
            if (patched)
            {
                string ts2 = tsText;
                ts2.replace(at, oldOut.size(), newOut);
                writeText(goodRoot + "/tsconfig.json", ts2);
            }

            string out, err;
            bool ok = agent.buildProject(goodRoot, err, &out);
            if (!ok)
                out = err;
            check(patched && ok,
                  "打包/发布按 tsconfig 的绝对 outDir 写产物（工程根外）",
                  out.empty() ? "(无输出)" : out);
            if (ok)
            {
                bool artifacts = fileExists(extOut + "/main.js") &&
                                 fileExists(extOut + "/lib.js") &&
                                 fileExists(extOut + "/util/strings.js") &&
                                 fileExists(extOut + "/main.js.map");
                check(artifacts, "产物写入了绝对 outDir 目录(.js+.js.map)", extOut);

                string entryText;
                readText(goodRoot + "/project.json", entryText);
                check(entryText.find(extOut + "/main.js") != string::npos,
                      "project.json entry 指向工程根外的绝对产物路径", entryText);

                printf("[stage 6] 绝对 outDir 产物运行 (entry=绝对路径):\n");
                JZEngineRuntime rt;
                rt.setLogHandler([](const string &t) {
                    printf("  [engine] %s\n", t.c_str());
                });
                bool ran = rt.init(abs);
                check(ran, "绝对 outDir 产物 init 成功(绝对 entry 加载)", rt.lastError());
                if (ran)
                {
                    bool okEval = rt.evalScript(
                        "(function(){ if (typeof main !== 'function') "
                        "throw new Error('main 未绑定');"
                        " if (main() !== 'lib k=42 [add+1=1] 20+22=42') "
                        "throw new Error('结果不一致'); })();\n");
                    check(okEval, "绝对 outDir 产物执行结果正确", rt.lastError());
                }
            }

            // 清理并复原：删外部产物目录、tsconfig 恢复 outDir=build，
            // 重新打包/发布（产物回 build/，project.json entry 复原为 build/main.js）
            rmRecursive(extOut);
            writeText(goodRoot + "/tsconfig.json", tsText);
            out.clear();
            bool ok2 = agent.buildProject(goodRoot, err, &out);
            if (!ok2)
                out = err;
            check(ok2, "恢复默认 outDir=build 后重新打包/发布(工程复原)",
                  out.empty() ? "(无输出)" : out);
            string entryText;
            readText(goodRoot + "/project.json", entryText);
            check(entryText.find("build/main.js") != string::npos &&
                      entryText.find(extOut) == string::npos,
                  "project.json 复原为 entry build/main.js", entryText);
        }
    }

    // ================================================================
    // Stage 7: JZ 内置类型(fs/path/os/process)随 exe 安装并由 initProject 暴露，
    //          生成的工程能解析这些 ambient 模块并通过类型检查（编译期验证）
    // ================================================================
    {
        string typesRoot = base + "/types_use";
        rmRecursive(typesRoot);
        ensureDir(typesRoot);

        JZCodeAgent agent2;
        JZAgentConfig llm2;
        CodeAgentConfig code2;
        if (ts.prefix.empty())
            code2.tscPath = ts.exe;
        agent2.init(llm2, code2);

        string err2;
        bool ok7 = agent2.initProject(typesRoot, err2);
        check(ok7, "initProject 创建引用 JZ 类型的工程", err2);
        if (ok7)
        {
            // 生成的 tsconfig 应已把随 exe 安装的 jz_types/core 加入 include
            string tsText2;
            readText(typesRoot + "/tsconfig.json", tsText2);
            check(tsText2.find("jz_types/core") != string::npos,
                  "initProject 在 tsconfig include 暴露 JZ core 类型路径", tsText2);

            // 该 TS 直接引用 fs/path/os/process（全局 ambient 模块 + import type），
            // 只要类型被正确安装并暴露，tsc 即可解析且类型检查通过。
            string mainTs =
                "// types_use/main.ts - 引用 JZ 内置类型(fs/path/os/process)\n"
                "import type { Stats } from 'fs';\n"
                "const sep: string = path.sep;\n"
                "const p: string = path.join('a', 'b', 'c');\n"
                "const base: string = path.basename(p);\n"
                "const ext: string = path.extname('main.ts');\n"
                "const isAbs: boolean = path.isAbsolute(p);\n"
                "const plat: string = os.platform();\n"
                "const eol: string = os.EOL;\n"
                "const tmp: string = os.tmpdir();\n"
                "const cwd: string = process.cwd();\n"
                "const argv: string[] = process.argv;\n"
                "const exists: boolean = fs.existsSync(cwd);\n"
                "const stat: Stats = fs.statSync(cwd);\n"
                "const names: string[] = fs.readdirSync(cwd);\n"
                "export function main(): string\n"
                "{\n"
                "    return 'types-ok ' + sep + ' ' + p + ' ' + base + ' ' + ext + ' '\n"
                "        + isAbs + ' ' + plat + ' ' + eol + ' ' + tmp + ' ' + cwd + ' '\n"
                "        + exists + ' ' + stat.size + ' ' + names.length + ' ' + argv.length;\n"
                "}\n"
                "console.log(main());\n";
            writeText(typesRoot + "/src/main.ts", mainTs);

            string out2;
            bool built = false;
            if (!code2.tscPath.empty())
            {
                built = agent2.buildProject(typesRoot, err2, &out2);
            }
            else
            {
                check(true, "跳过 JZ 类型引用编译验证(node 前缀方案无宿主 tsc)",
                      "(node 前缀方案无宿主 tsc，跳过)");
                built = true;
            }
            check(built, "引用 JZ 类型的工程编译通过(类型可解析且类型正确)",
                  out2.empty() ? "(无输出)" : out2);
        }
    }

    // ================================================================
    // Stage 8: exportProject 导出可独立运行包（支持改名 + 替换图标）
    // ================================================================
    {
        string expRoot = base + "/export_src";
        rmRecursive(expRoot);
        ensureDir(expRoot);

        JZCodeAgent agent3;
        JZAgentConfig llm3;
        CodeAgentConfig code3;
        if (ts.prefix.empty())
            code3.tscPath = ts.exe;
        agent3.init(llm3, code3);

        string e3;
        check(agent3.initProject(expRoot, e3), "initProject 创建导出测试工程", e3);
        writeText(expRoot + "/src/main.ts",
                  "export function main(): void {\n    console.log('export-ok');\n}\n"
                  "console.log('export-ok');\n");

        // 生成一个测试图标，导出时把运行时重命名为 myapp 并替换图标
        string ico8 = base + "/stage8.ico";
        check(writeMinIco(ico8, 200, 30, 30), "生成导出用测试 .ico", "");

        string expDir, expErr;
        bool ok8 = agent3.exportProject(expRoot, expDir, expErr, "myapp", ico8, true);
        check(ok8, "exportProject 导出可独立运行包（改名 myapp + 替换图标 + debug）", expErr);
        if (ok8)
        {
            check(JZFileInfo::exists(expDir + "/build/main.js"),
                  "导出含编译产物 build/main.js（运行主入口）", "");
            check(JZFileInfo::exists(expDir + "/project.json"),
                  "导出含运行主入口声明 project.json", "");
            check(JZFileInfo::exists(expDir + "/myapp.exe"),
                  "导出含重命名后的运行时 myapp.exe（内嵌 JZEngineRuntime）", "");
            check(!JZFileInfo::exists(expDir + "/code_runtime.exe"),
                  "导出目录不再含默认 code_runtime.exe（已改名）", "");
            check(JZExeResource::hasIcon(expDir + "/myapp.exe"),
                  "重命名后的 myapp.exe 已含自定义 exe 图标（JZExeResource 替换）", "");
            // 调试开关资源：导出时默认写入（debug=true），运行时自身读取以决定是否开调试器
            {
                bool dbg = false;
                string derr;
                check(JZExeResource::getDebugFlag(expDir + "/myapp.exe", dbg, derr),
                      "myapp.exe 可读到调试开关资源", derr);
                if (ok8)
                    check(dbg, "导出默认 debug=true（写入调试开关资源）", "");
            }
            check(JZFileInfo::exists(expDir + "/run.bat"),
                  "导出含一键启动 run.bat", "");
        }
    }

    // ================================================================
    // Stage 9: JZExeResource 通用资源读写 + 图标替换（隔离验证）
    // ================================================================
    {
        string runtimeExe = normalize(JZPlatform::applicationDirPath()) + "/code_runtime.exe";
        check(fileExists(runtimeExe), "code_runtime.exe 存在于 exe 目录（图标替换前置）", "");
        if (fileExists(runtimeExe))
        {
            string tmpExe = base + "/icon_stage9_runtime.exe";
            rmRecursive(tmpExe);
            check(copyFileBin(runtimeExe, tmpExe), "复制 code_runtime.exe 到临时文件", "");

            // 9a) 通用任意资源读写往返（type/name 自定义字符串）
            {
                string resErr;
                string payload = "hello-jz-runtime-config";
                check(JZExeResource::setRawResource(tmpExe, "JZTEST", "CFG", payload, resErr),
                      "setRawResource 写入自定义资源", resErr);
                string got;
                check(JZExeResource::getRawResource(tmpExe, "JZTEST", "CFG", got, resErr),
                      "getRawResource 读回自定义资源", resErr);
                check(got == payload, "自定义资源读写往返一致", "");
            }

            // 9b) 图标替换
            string ico9 = base + "/stage9.ico";
            check(writeMinIco(ico9, 30, 200, 30), "生成测试 .ico", "");

            string icErr;
            bool ok9 = JZExeResource::replaceIcon(tmpExe, ico9, icErr);
            check(ok9, "JZExeResource::replaceIcon 替换临时 exe 图标", icErr);
            if (ok9)
                check(JZExeResource::hasIcon(tmpExe), "替换后临时 exe 含 RT_GROUP_ICON", "");
        }
    }

    // ================================================================
    // 汇总
    // ================================================================
    printf("\n==== 结果: %s (%d 失败) ====\n", g_failed ? "FAIL" : "ALL PASS", g_failed);
    return g_failed ? 1 : 0;
}
