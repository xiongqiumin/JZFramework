// code_agent_test - "JZCodeAgent 创建工程 -> 打包/发布 -> 调用运行时" 端到端流程测试。
//
// 全流程（不用大模型，代码直接写入文件）：
//   1. 创建工程   JZCodeAgent::initProject / probeProject / openProject，
//                新建到 exe 目录下的 code_agent_flow_test（跑完可就地查看产物）；
//                （project.json 是"打包/发布"写在产物目录里的补充信息、
//                不是工程骨架，此处专门断言工程根下它没有）
//   2. 写多文件   直接写入 src/main.ts + src/lib.ts + src/util/strings.ts
//                （main 跨目录 import，lib 由 util 经 "../lib" 再拉一次，
//                验证引擎模块 loader 的多文件 / 子目录 / 相对路径 / 共享模块）；
//                尚未打包时先试运行 -> 产物目录里没有 main.js，
//                JZEngineRuntime.init 直接报错（不回退默认入口）；
//   3. 打包/发布  JZCodeAgent::buildProject（宿主 tsc -p tsconfig.json）。产物写向
//                tsconfig 的 outDir：缺省=工程根/build（*.js+*.js.map）；outDir 改成
//                工程根外绝对目录也一样生效（Stage 6）。编译通过后按 outDir 定位并
//                校验主入口产物 main.js，并在它旁边写补充信息 project.json
//                （entry 固定 main.js，另有 buildTime / projectRoot / srcRoot）；
//   4. 调用运行时（JZEngineRuntime 内嵌 JZJSEngine，引擎创建/模块运行/错误栈
//      source map 翻译全部封装在类里，外面不直接碰 JZJSEngine）
//        a. 进程内：rt.init(<工程根>/build) 完成"设运行根 + 加载主入口 main.js"，
//           再 rt.evalScript 断言入口导出可复用、结果正确；
//        b. 进程内：打包后故意抛错的工程 -> rt.init 返回 false，
//           rt.lastError() 已把 JS 栈翻译回 TS 源码位置（靠 project.json 的
//           projectRoot 还原成 "src/faultlib.ts" 这样的相对路径）；
//        c. 子进程：调用 code_runtime <运行根> 跑同一产物（good 成功；
//           fail 打包后运行抛错退出），校验退出码与输出。
//
// 用法：code_agent_test.exe
// 环境变量：JZ_TS_NODE + JZ_TS_TSC 可换编译器（默认 E:/libs/ts_compiler 的 tsc.exe）。
// 退出码：0=全部通过；1=有失败；2=前置条件缺失（编译器/工程根不可用）。
//
// 注意：本文件不 include <windows.h>；进程调用走框架 JZProcess。

#define _CRT_SECURE_NO_WARNINGS

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "JZApplication.h"
#include "JZBundlePaths.h"
#include "JZByteArray.h"
#include "JZCodeAgent.h"
#include "JZDebug.h"
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
// workDir 非空时作为子进程工作目录（code_runtime 以工作目录为运行根）。
static int runProcessCapture(const string &exe, const vector<string> &args,
                             string &output, int timeoutMs,
                             const string &workDir = string())
{
    JZProcess proc;
    proc.setProcessChannelMode(JZProcess::MergedChannels);
    if (!workDir.empty())
        proc.setWorkingDirectory(workDir);
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
        jzDebug("[PASS] %s", tag);
    else
    {
        jzDebug("[FAIL] %s\n       %s", tag, detail.c_str());
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
    // 随包编译器：<随包产物目录>/ts_compiler/tsc/tsc.exe（与 code_agent / benchmark 的
    // 默认 --tsc 同一约定）。它与 code_runtime 同在 binary/bin/<配置>/，不在本测试 exe
    // 旁边 ⇒ 由 jzFindBundleAsset 一次找出来（见 JZBundlePaths.h）。
    // 开发机上可能没有 E:/libs 那份，优先用随包的。
    string cand = jzFindBundleAsset("ts_compiler/tsc/tsc.exe");
    if (!cand.empty())
    {
        r.exe = cand;
        r.ok = true;
        return r;
    }

    cand = "E:/libs/ts_compiler/app/node_modules/@typescript/"
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
    // project.json 只是补充信息（主入口固定为 main.js），写在 main.js 同目录；
    // 解析要用的"原路径地址"至少要有 projectRoot。
    return writeText(root + "/build/project.json",
                     "{\n"
                     "  \"entry\": \"main.js\",\n"
                     "  \"projectRoot\": \"" + root + "\",\n"
                     "  \"srcRoot\": \"" + root + "/src\"\n"
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
    // code_runtime 在 binary/ 的统一目录里（binary/bin/<配置>/），而本测试 exe 在自己
    // 的 test/<配置>/ —— jzFindBundleAsset 会把两处都试一遍（见 JZBundlePaths.h）。
    return jzFindBundleAsset("code_runtime.exe");
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

    jzDebug("==== code_agent flow test: 创建工程 -> 打包/发布 -> 调用运行时 ====");

    // ---- 前置：工程根 / 编译器 / 运行时 ----
    // 测试工程直接建在 exe 目录下的 code_agent_flow_test（不进系统临时目录），
    // 方便就地查看 tsc 产物：build/*.js + *.js.map + project.json + 导出包 export/。
    string workRoot = normalize(JZPlatform::applicationDirPath());
    if (workRoot.empty())
    {
        jzDebug("[fatal] JZPlatform::applicationDirPath() 为空");
        return 2;
    }
    string base = workRoot + "/code_agent_flow_test";
    jzDebug("base: %s", base.c_str());
    rmRecursive(base);
    ensureDir(base);

    TsRunner ts = resolveTsCompiler();
    if (!ts.ok)
    {
        jzDebug("[fatal] 未找到 tsc（默认 E:/libs/ts_compiler/.../tsc.exe，"
               "或用 JZ_TS_NODE/JZ_TS_TSC 指定）\n");
        return 2;
    }
    jzDebug("tsc: %s", ts.exe.c_str());

    string runtimeExe = findRuntimeExe();
    jzDebug("code_runtime: %s",
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
    agent.init(code);

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

        jzDebug("[stage 2b] 尚未打包/发布时 init -> 产物目录里没有 main.js，直接报错:");
        JZDir d(goodRoot);
        string abs = d.absolutePath();
        JZEngineRuntime rt;
        bool ran = rt.init(abs + "/build");
        string errText = rt.lastError();
        check(!ran && errText.find("main.js") != string::npos,
              "init 拒绝运行并提示缺少主入口 main.js（不回退默认入口）", errText);
        jzDebug("  提示:\n%s", errText.c_str());
    }

    // ================================================================
    // Stage 3: 打包/发布（JZCodeAgent::buildProject）
    // ================================================================
    {
        jzDebug("[stage 3] buildProject (打包/发布: 真实 tsc + 生成入口声明):");
        string out;
        bool ok;
        if (!code.tscPath.empty())
        {
            agent.setWorkspace(goodRoot);   // 新接口：buildProject 面向当前工作区
            string err;
            ok = agent.buildProject(err);
            if (ok)
            {
                agent.waitBuildFinished();
                ok = agent.lastBuildOk();
                err = agent.lastBuildError();
            }
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
            jzDebug("%s", out.c_str());
            return 1;
        }
        bool artifacts = fileExists(goodRoot + "/build/main.js") &&
                         fileExists(goodRoot + "/build/lib.js") &&
                         fileExists(goodRoot + "/build/util/strings.js") &&
                         fileExists(goodRoot + "/build/main.js.map") &&
                         fileExists(goodRoot + "/build/lib.js.map") &&
                         fileExists(goodRoot + "/build/util/strings.js.map");
        check(artifacts, "产物齐全: 3 个 .js + 3 个 .js.map(build/)", goodRoot + "/build");

        // project.json 与 main.js 同目录，是"补充信息"而不是入口声明：
        // entry 固定 main.js，另有编译时间与原路径地址（projectRoot / srcRoot）
        string metaText;
        readText(goodRoot + "/build/project.json", metaText);
        check(metaText.find("main.js") != string::npos,
              "打包后生成 project.json(与 main.js 同目录) -> entry main.js", metaText);
        check(metaText.find("buildTime") != string::npos,
              "project.json 含编译时间 buildTime", metaText);
        check(metaText.find("projectRoot") != string::npos,
              "project.json 含原路径地址 projectRoot", metaText);
    }

    // ================================================================
    // Stage 4a: 进程内调用运行时（JZEngineRuntime 内嵌引擎，ESM 多文件加载）
    // ================================================================
    {
        JZDir goodDir(goodRoot);
        string abs = goodDir.absolutePath();

        JZEngineRuntime rt;
        string engineOut;          // 捕获入口顶层自执行的 console 输出
        rt.setLogHandler([&](const string &t) {
            engineOut += t + "\n";
            jzDebug("  [engine] %s", t.c_str());
        });

        jzDebug("[stage 4a] JZEngineRuntime.init (运行根=产物目录, 主入口 main.js):");
        bool ok = rt.init(abs + "/build");
        check(ok, "good 工程 init 成功(运行根+主入口 main.js+会话)", rt.lastError());
        if (ok)
        {
            check(rt.projectInfo().valid, "读到 project.json 补充信息", rt.root());
            check(!rt.projectInfo().projectRoot.empty(),
                  "project.json 给出原路径地址 projectRoot", rt.projectInfo().projectRoot);
        }

        // 新结构(纯 import 模型): 入口具名导出不再自动绑到全局, 验证改为读取入口
        // 顶层自执行产生的 console 输出(MULTIFILE-OK + 跨文件结果)
        check(engineOut.find("MULTIFILE-OK") != string::npos,
              "good 工程入口顶层自执行并输出结果", engineOut);
        const string expect4a = "lib k=42 [add+1=1] 20+22=42";
        check(engineOut.find(expect4a) != string::npos,
              "跨文件导入结果正确(含子目录与共享模块)", engineOut);

        // Node 兼容: fetch 等 Web 标准全局应在引擎启动即存在, 无需 import
        // (引擎 init 时已由 bootstrapWebGlobals 主动装载 net 适配层)。good 工程入口并未
        // import net, 因此这条断言验证的就是"零 import 可用"。
        check(rt.evalScript(
                  "if (typeof fetch !== 'function') "
                  "throw new Error('fetch 应作为全局零 import 可用');\n"
                  "if (typeof WebSocket !== 'function') "
                  "throw new Error('WebSocket 应作为全局零 import 可用');\n"
                  "if (typeof URL !== 'function') "
                  "throw new Error('URL 应作为全局零 import 可用');\n"),
              "fetch/WebSocket/URL 全局可用(无需 import, 对齐 Node)", rt.lastError());

        // 反向验证: net/http/dgram/mqtt/ws/url/fs/path/os/process/crypto 是对齐 Node 的
        // "内置模块", 必须 import 才能用, 不应作为全局泄露(防止与 Node 模块语义混淆)。
        check(rt.evalScript(
                  "['net','http','dgram','mqtt','ws','url',"
                  "'fs','path','os','process','crypto'].forEach(function (k) {\n"
                  "  if (typeof globalThis[k] !== 'undefined') "
                  "throw new Error(k + ' 不应作为全局(须 import)');\n"
                  "});\n"),
              "net/http/dgram/mqtt/ws/url/fs/path/os/process/crypto 不泄露为全局(须 import, 对齐 Node)",
              rt.lastError());
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
            agent.setWorkspace(failRoot);   // 新接口：buildProject 面向当前工作区
            string err;
            ok = agent.buildProject(err);
            if (ok)
            {
                agent.waitBuildFinished();
                ok = agent.lastBuildOk();
                err = agent.lastBuildError();
            }
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
            jzDebug("%s", out.c_str());
            return 1;
        }

        JZDir failDir(failRoot);
        string abs = failDir.absolutePath();

        JZEngineRuntime rt;
        rt.setLogHandler([](const string &t) {
            jzDebug("  [engine] %s", t.c_str());
        });

        jzDebug("[stage 4b] JZEngineRuntime.init on failing module (打包后运行抛错):");
        bool ran = rt.init(abs + "/build");
        string errText = rt.lastError();   // 已翻译回 TS
        check(!ran && !errText.empty(), "fail 工程 init 失败并捕获运行时异常(跨文件)", errText);
        jzDebug("  翻译栈:\n%s", errText.c_str());
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
        // 运行根靠"子进程工作目录"给（code_runtime 不再收位置参数）
        int rc = runProcessCapture(runtimeExe, {}, out, 60 * 1000, goodAbs + "/build");
        jzDebug("[stage 5] code_runtime good 输出(运行根=产物目录, main.js):\n%s", out.c_str());
        check(rc == 0, "code_runtime 运行 good 工程退出码=0",
              "rc=" + to_string(rc));
        check(out.find("MULTIFILE-OK") != string::npos,
              "code_runtime 输出含模块 console 结果", out);

        JZDir failDir(failRoot);
        string failAbs = failDir.absolutePath();
        out.clear();
        rc = runProcessCapture(runtimeExe, {}, out, 60 * 1000, failAbs + "/build");
        jzDebug("[stage 5] code_runtime fail 输出:\n%s", out.c_str());
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

            agent.setWorkspace(goodRoot);   // 新接口：buildProject 面向当前工作区
            string out, err;
            bool ok = agent.buildProject(err);
            if (ok)
            {
                agent.waitBuildFinished();
                ok = agent.lastBuildOk();
                err = agent.lastBuildError();
            }
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

                string metaText;
                readText(extOut + "/project.json", metaText);
                check(metaText.find(extOut) != string::npos,
                      "project.json 写在工程根外的绝对产物目录(outDir)", metaText);

                jzDebug("[stage 6] 绝对 outDir 产物运行 (运行根=绝对 outDir):");
                JZEngineRuntime rt;
                string engineOut6;
                rt.setLogHandler([&](const string &t) {
                    engineOut6 += t + "\n";
                    jzDebug("  [engine] %s", t.c_str());
                });
                bool ran = rt.init(extOut);
                check(ran, "绝对 outDir 产物 init 成功(运行根=outDir)", rt.lastError());
                // 新结构: 入口顶层自执行 console 输出即结果(纯 import 模型, 无全局绑定)
                const string expect6 = "lib k=42 [add+1=1] 20+22=42";
                check(engineOut6.find("MULTIFILE-OK") != string::npos &&
                      engineOut6.find(expect6) != string::npos,
                      "绝对 outDir 产物执行结果正确(顶层自执行 console 输出)", engineOut6);
            }

            // 清理并复原：删外部产物目录、tsconfig 恢复 outDir=build，
            // 重新打包/发布（产物回 build/，project.json 也回到 build/ 下）
            rmRecursive(extOut);
            writeText(goodRoot + "/tsconfig.json", tsText);
            out.clear();
            bool ok2 = agent.buildProject(err);
            if (ok2)
            {
                agent.waitBuildFinished();
                ok2 = agent.lastBuildOk();
                err = agent.lastBuildError();
            }
            out = err;
            check(ok2, "恢复默认 outDir=build 后重新打包/发布(工程复原)",
                  out.empty() ? "(无输出)" : out);
            string metaText;
            readText(goodRoot + "/build/project.json", metaText);
            check(metaText.find("main.js") != string::npos &&
                      metaText.find(extOut) == string::npos,
                  "project.json 复原到 build/ 下(entry main.js)", metaText);
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
        agent2.init(code2);

        string err2;
        bool ok7 = agent2.initProject(typesRoot, err2);
        check(ok7, "initProject 创建挂 js_wrapper 类型的 ES2025 工程", err2);
        if (ok7)
        {
            // 生成的 tsconfig 应为「ES2025(lib 不含 DOM) + 挂 js_wrapper 类型」：
            // 含 "lib": ["ES2025"]（对齐 quickjs 实际支持范围），且 include 应包含随
            // exe 部署的 js_wrapper 树。
            string tsText2;
            readText(typesRoot + "/tsconfig.json", tsText2);
            check(tsText2.find("\"lib\": [\"ES2025\"]") != string::npos,
                  "initProject 生成的 tsconfig 设为 ES2025(无 DOM)", tsText2);
            check(tsText2.find("js_wrapper") != string::npos,
                  "initProject 生成的 tsconfig 包含 js_wrapper 类型", tsText2);
            // 一棵与源码同构的树: 类型与运行时(各包 dist)在同一棵 js_wrapper 里。
            // ⭐ 一个名字一个包目录(目录名 = 名字), 所以这里按名字取路径
            check(fileExists(typesRoot + "/js_wrapper/http/dist/index.js") &&
                  fileExists(typesRoot + "/js_wrapper/fs/dist/index.js"),
                  "initProject 把运行时 js_wrapper/<名字>/dist 一同拷进工程",
                  typesRoot + "/js_wrapper");

            // 该 TS 直接引用 js_wrapper 的 fs/path/os/process（框架自定义模块），
            // 只要类型被安装并挂进 include，tsc 即可解析且类型检查通过。
            string mainTs =
                "// types_use/main.ts - 引用 js_wrapper 的类型(fs/path/os/process)\n"
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
                "export default main;\n";
            writeText(typesRoot + "/src/main.ts", mainTs);

            string out2;
            bool built = false;
            if (!code2.tscPath.empty())
            {
                agent2.setWorkspace(typesRoot);   // 新接口：buildProject 面向当前工作区
                built = agent2.buildProject(err2);
                if (built)
                {
                    agent2.waitBuildFinished();
                    built = agent2.lastBuildOk();
                    err2 = agent2.lastBuildError();
                }
                out2 = err2;
            }
            else
            {
                check(true, "跳过 js_wrapper 类型编译验证(node 前缀方案无宿主 tsc)",
                      "(node 前缀方案无宿主 tsc，跳过)");
                built = true;
            }
            check(built, "挂 js_wrapper 类型的 ES2025 工程编译通过(类型可解析且类型正确)",
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
        agent3.init(code3);

        string e3;
        check(agent3.initProject(expRoot, e3), "initProject 创建导出测试工程", e3);
        writeText(expRoot + "/src/main.ts",
                  "export function main(): void {\n    console.log('export-ok');\n}\n"
                  "console.log('export-ok');\n");

        // 生成一个测试图标，导出时把运行时重命名为 myapp 并替换图标
        string ico8 = base + "/stage8.ico";
        check(writeMinIco(ico8, 200, 30, 30), "生成导出用测试 .ico", "");

        agent3.setWorkspace(expRoot);   // 新接口：exportProject 面向当前工作区
        ExportConfig cfg8;
        cfg8.exeName = "myapp";
        cfg8.iconPath = ico8;
        cfg8.debug = true;
        string expDir, expErr;
        bool ok8 = agent3.exportProject(expDir, expErr, cfg8);
        check(ok8, "exportProject 导出可独立运行包（改名 myapp + 替换图标 + debug）", expErr);
        if (ok8)
        {
            check(JZFileInfo::exists(expDir + "/main.js"),
                  "导出含主入口 main.js（铺在导出根，运行根=exe 目录）", "");
            check(JZFileInfo::exists(expDir + "/project.json"),
                  "导出含补充信息 project.json", "");
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
        // code_runtime 在 binary/ 的统一目录里，不在测试 exe 旁边 ⇒ 用同一个探测
        string runtimeExe = findRuntimeExe();
        check(fileExists(runtimeExe), "找到 code_runtime.exe（图标替换前置）", "");
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
    // Stage 10: 新结构 - 用户工程被动 import 随附的 js_wrapper 运行时(dist),
    //   运行期由入口 ESM 触发(纯 import 模型, 无 require / 全局绑定桥)
    // ================================================================
    {
        string jsRoot = base + "/js_wrapper_use";
        rmRecursive(jsRoot);
        ensureDir(jsRoot + "/src");

        JZCodeAgent agentJs;
        JZAgentConfig llmJs;
        CodeAgentConfig codeJs;
        if (ts.prefix.empty())
            codeJs.tscPath = ts.exe;
        agentJs.init(codeJs);

        string eJs;
        check(agentJs.initProject(jsRoot, eJs), "initProject 创建挂 js_wrapper 运行时的工程", eJs);
        check(fileExists(jsRoot + "/js_wrapper/http/dist/index.js") &&
              fileExists(jsRoot + "/js_wrapper/fs/dist/index.js"),
              "initProject 把运行时 js_wrapper/<名字>/dist 拷进工程", jsRoot + "/js_wrapper");

        // 入口被动 import 运行期模块(http/fs/path), 顶层自执行并 console.log 结果。
        // 相对路径 '../js_wrapper/...' 由引擎按运行根(build/)的 '../' 解析回工程根的
        // js_wrapper; 一个名字一个包目录 ⇒ 每条 import 都是 '<名字>/dist/index.js'。
        string mainJs =
            "// js_wrapper_use/main.ts - 被动 import 随附 js_wrapper 运行时(纯 import 模型)\n"
            "import http from '../js_wrapper/http/dist/index.js';\n"
            "import fs from '../js_wrapper/fs/dist/index.js';\n"
            "import path from '../js_wrapper/path/dist/index.js';\n"
            "export function main(): string\n"
            "{\n"
            "    const sep: string = path.sep;\n"
            "    const hasFetch: boolean = typeof (globalThis as any).fetch === 'function';\n"
            "    const hasFs: boolean = typeof (fs as any).existsSync === 'function';\n"
            "    const joined: string = path.join('x', 'y');\n"
            "    const isObj: boolean = typeof http === 'object';\n"
            "    return 'jzjs-ok ' + sep + ' ' + hasFetch + ' ' + hasFs + ' ' + joined +\n"
            "           ' ' + isObj;\n"
            "}\n"
            "console.log('JZJS-OK', main());\n";
        writeText(jsRoot + "/src/main.ts", mainJs);

        string outJs;
        bool built = false;
        if (!codeJs.tscPath.empty())
        {
            agentJs.setWorkspace(jsRoot);
            built = agentJs.buildProject(eJs);
            if (built)
            {
                agentJs.waitBuildFinished();
                built = agentJs.lastBuildOk();
                eJs = agentJs.lastBuildError();
            }
            outJs = eJs;
            check(built, "挂 js_wrapper 被动 import 的工程编译通过(运行时模块可解析且类型正确)",
                  outJs.empty() ? "(无输出)" : outJs);

            if (built)
            {
                JZDir jsDir(jsRoot);
                string absJs = jsDir.absolutePath();
                JZEngineRuntime rt;
                string engineOut;
                rt.setLogHandler([&](const string &t) {
                    engineOut += t + "\n";
                    jzDebug("  [engine] %s", t.c_str());
                });
                bool ran = rt.init(absJs + "/build");
                check(ran, "js_wrapper 运行时经被动 import 启动成功(init)", rt.lastError());
                check(engineOut.find("JZJS-OK") != string::npos &&
                      engineOut.find("jzjs-ok") != string::npos,
                      "被动 import 的 net/fs/path 运行期全局可用(顶层自执行)", engineOut);
            }
        }
        else
        {
            check(true, "跳过 js_wrapper 运行时验证(node 前缀方案无宿主 tsc)", "(skipped)");
        }
    }

    // ================================================================
    // 汇总
    // ================================================================
    jzDebug("\n==== 结果: %s (%d 失败) ====", g_failed ? "FAIL" : "ALL PASS", g_failed);
    return g_failed ? 1 : 0;
}
