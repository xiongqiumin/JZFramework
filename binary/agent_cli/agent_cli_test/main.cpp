// agent_cli_test —— 用 code_agent/benchmark 的同一套用例评测 JZAgentCli
//
// 评测模型在"给定工作区"里自驱完成一个编码任务的能力, 但驱动端换成 JZAgentCli:
//   JZAgentCli 自己就跑一个 JZAgent(直接驱动本地 agent_tool 工具), 通过 submitTask
//   提交任务, 走它原生的"提交 -> 验证脚本门控"循环; 循环结束带回 JSON 结果
//   {status, verify_rounds, agent_reply, last_verify_output, error?}, 以 status 判定。
//
// 与 code_agent_benchmark 的关键差异:
//   - 宿主只负责"铺工程骨架"(initProject: tsconfig outDir=build / rootDir=src + src/main.ts +
//     js_wrapper/), 之后编译/运行都由 agent 自己用 terminal/file 工具干, 或由验证脚本复核
//     (所以 tsc / code_runtime 要对 agent 可用, 靠下面的"程序映射"登记, 而不是宿主代跑)。
//   - 用例加载逻辑完全一致(同 parseCaseObject/loadCases), benchmark/data 里的用例直接作输入。
//     - 带 verify 字段的用例 -> submitTask 的 verify_cmd(benchmark 原样搬过来);
//     - 没有 verify 但带 expect.contains 的用例 -> 自动挂**通用验证**
//       (data/stdout_verify.py: tsc 先编译 -> 按 tsconfig 定位产物目录 -> 跑 code_runtime
//        -> 比对输出), 也就是 benchmark 宿主原本内联在 main.cpp 里的那段判定
//       (BenchCase.check + tsc 编译 + runCodeRuntime)搬成的脚本;
//     - 两者都没有 -> 不挂验证, status 以 agent 自身成败为准(烟雾测试)。
//
// 程序映射(动态): JZCliConfig::programPaths = 程序名 -> 可执行文件全路径,
//   登记后由 JZVirtualTerminal 做两件事: 进命令白名单(短名字能过审) + 执行时把命令里
//   这个短名换成全路径(`tsc xxx` 这种短命令名能找到程序, 不往子进程 PATH 里塞东西)。⚠ 只认短名字 —— 写全路径走路径审核(区外拒),
//   映射不会顺带授信那个目录。每个用例的 JZAgentCli 都按这套配置重建, 所以换一份配置就是
//   换一套可用程序 —— 不是编译期写死的。
//   tsc / code_runtime 由本程序(外部)自己按 exe 目录向上找到 build 再定位: 找到就填进
//   cfg.programPaths 传进去 —— JZAgentCli 只认配置, 自己不探测。
//
// 配置(命令行 / 环境变量):
//   --modelUrl / --modelName / --modelToken   大模型接入(缺省读 exe 同目录的 test_llm.json)
//   --data    评测集目录(缺省 <本 exe 同目录>/data/agent_cli_test)
//   --cases   逗号分隔的用例名/通配符, 仅跑匹配的（可多个；支持 * ? .）
//   --http-port   输入 HTTP 服务监听端口(JS/agent 用 fetch 读输入)
//   --cli-http-port 每个用例的 JZAgentCli 网页服务起始端口(默认 19090, 逐用例 +1)
//   --no-color   关闭 ANSI 彩色输出
//
// 实时过程打印:
//   每个用例的 JZAgentCli 会 startHttp()(就是网页控制台用的那套: POST /task + GET /events),
//   本程序再以 HTTP 客户端身份 GET /events 订阅同一条 SSE 事件流, 把思考/工具调用/验证
//   结果实时打到控制台 —— 没有为测试单独开接口, 走的完全是网页接口。
// 退出码: 0=全部用例通过; 1=有失败; 2=前置条件缺失(模型未配置 / 评测集加载失败)。

#define _CRT_SECURE_NO_WARNINGS

#include <cstdio>
#include <cctype>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <fstream>
#include <map>
#include <algorithm>

#include "JZApplication.h"
#include "json/json.h"
#include "JZByteArray.h"
#include "JZDebug.h"
#include "JZDir.h"
#include "JZElapsedTimer.h"
#include "JZEventLoop.h"
#include "JZFileInfo.h"
#include "JZHttpServer.h"
#include "JZHttpHandler.h"
#include "JZNetworkAccessManager.h"
#include "JZNetworkReply.h"
#include "JZNetworkRequest.h"
#include "JZPlatform.h"
#include "JZTimer.h"
#include "JZUuid.h"
#include "JZStd.h"

#include "JZAgentCli.h"

// ---------------------------------------------------------------------------
// 控制台 ANSI 颜色(同 benchmark, 不 include <windows.h>)
// ---------------------------------------------------------------------------
#ifdef _WIN32
extern "C" {
__declspec(dllimport) void *__stdcall GetStdHandle(unsigned long nStdHandle);
__declspec(dllimport) int  __stdcall GetConsoleMode(void *hConsoleHandle, unsigned long *lpMode);
__declspec(dllimport) int  __stdcall SetConsoleMode(void *hConsoleHandle, unsigned long dwMode);
}
#endif

namespace
{

const unsigned long kStdOutputHandle = (unsigned long)-11;
const unsigned long kVirtualTerminal = 0x0004;

bool g_colorEnabled = false;

bool enableConsoleColor()
{
#ifdef _WIN32
    void *out = GetStdHandle(kStdOutputHandle);
    if (!out)
        return false;
    unsigned long mode = 0;
    if (!GetConsoleMode(out, &mode))
        return false;
    if (!SetConsoleMode(out, mode | kVirtualTerminal))
        return false;
    return true;
#else
    return true;
#endif
}

const char *clr(const char *ansi) { return g_colorEnabled ? ansi : ""; }

const char *CLR_RESET  = "\033[0m";
const char *CLR_GRAY   = "\033[90m";
const char *CLR_GREEN  = "\033[32m";
const char *CLR_RED    = "\033[31m";
const char *CLR_YELLOW = "\033[33m";

// 毫秒 -> 秒文本, 保留 1 位小数。
// ⚠ 必须返回字符串: jzDebug 打 double 走 %f(6 位小数), 直接给 double 会打成 51.738000s。
static std::string sec1(int64_t ms)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f", ms / 1000.0);
    return std::string(buf);
}

// 占位符替换(定义在文件后部, 提前声明以便 CliTest 内联使用)
static void str_replace(std::string &s, const std::string &from, const std::string &to);

// ---- 用例定义(与 benchmark 对齐, 仅取 JZAgentCli 用得到的字段)----
struct BenchmarkCase
{
    std::string name;
    std::string prompt;
    int maxAttempts = 3;
    int timeoutMs = 30000;

    // 注入给本用例的输入(空串表示无需输入); 运行时由全局 HTTP 服务 /input 暴露给 agent
    std::string input;

    // 外部程序验证(非空时启用): 交给 JZAgentCli 的 verify_cmd(原生验证机制)
    std::string verifyCmd;
    int verifyPort = 3900;

    // setup 准备脚本: 相对『用例 json 所在目录』的 .py 路径; initProject 之后、agent 开始前
    // 由宿主同步执行, 传入 --root <工程根> --input <输入> --data <评测集目录>; 非零退出判 setup 失败。
    std::string setup;
    std::string sourceDir;     // 用例 json 所在目录(setup 相对路径在此解析)

    // daemon 后台服务(可选, 与 benchmark 同语义): setup 一次性跑完就退出, daemon 是整个用例
    // 期间常驻的辅助进程(模拟下位机 / 对端 MES 等), setup 之后、agent 之前拉起, finishCase() 杀掉。
    // 命令模板支持同样的占位符: ${root} ${build} ${runtime} ${port} ${bench} ${data}。
    std::string daemon;
    int daemonReadyMs = 3000;   // 启动后轮询等待其就绪的上限(毫秒); 超时不判失败

    // 输出预期(benchmark 的 expect.* : contains / equals / caseInsensitive):
    //   没有 verify 命令时, 由本程序自动组织成"通用 stdout 验证"(data/stdout_verify.py:
    //   跑 code_runtime 比对产物输出), 等价于 benchmark 宿主原本内联的 check()。
    //   contains = 子串命中(宽松); equals = 规范化后整段完全相等(严格, 见 normalizeForEquals)。
    std::string expectContains;
    std::string expectEquals;
    bool expectCaseInsensitive = false;
};

// 全局输入 HTTP 服务 handler
class InputHttpHandler : public JZHttpHandler
{
public:
    std::function<std::string()> getInput;
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        JZHttpRespone resp;
        resp.setText(getInput());
        client->response(resp.toByteArray());
    }
};

class HttpThread : public JZThread
{
public:
    JZHttpServer *server;
    virtual void run() override
    {
        exec();
        // 退出前先在自己这个线程里关掉监听, 再把 server 交还主线程 —— 之后才能安全析构。
        server->close();
        server->moveToThread(jzApp->mainThread());
    }
};

// ---- 单用例结果 ----
struct CaseResult
{
    std::string name;
    bool passed = false;
    int attempts = 0;          // = verify_rounds(verify 循环的轮次) 或 1(无验证); ⚠ 不是 LLM 请求轮数
    int promptTokens = 0;      // Σ 各轮"整段 prompt"(含缓存命中部分) ⇒ 计费口径
    int completionTokens = 0;  // Σ 各轮生成(含思考 token)
    int totalTokens = 0;       // = prompt + completion(计费总量, 横向比成本用这个)
    int cachedTokens = 0;      // 其中命中的前缀缓存(llama.cpp cache_n); 已包含在 prompt 里
    int contextTokens = 0;     // 最后一轮的上下文占用(JZAgent::windowTokens()) —— 与上面不是一个口径
    int llmMs = 0;
    // llama.cpp 计时(其他 provider 恒为 0)，速率按 token/ms 现算
    double promptMs = 0;
    double predMs = 0;
    int draftN = 0;            // MTP/投机采样草稿数
    int draftAccepted = 0;     // MTP/投机采样被接受数
    std::string lastStatus;
    std::string lastVerifyOutput;
    std::string lastAgentError;
};

// ---- 通用 stdout 验证命令的组织 ----
// 解释器候选顺序与 JZAgentCli::resolveProgram 一致: 配置映射(programPaths)优先, 然后逐个探测
// (`<name> --version` 退出码 0 算能跑), 都不行才退回 python3。
// 为什么不能硬写 python3: Windows 下装了 Python 往往只有 python.exe / py.exe, 硬写会让
// 验证脚本每一轮都以 9009 退出(看着像"测试一直失败"), 白烧掉所有续跑轮次。
static bool probeProgramRunnable(const std::string &name)
{
#ifdef _WIN32
    std::string probe = "\"" + name + "\" --version > nul 2>&1";
    FILE *f = _popen(probe.c_str(), "r");
#else
    std::string probe = "\"" + name + "\" --version > /dev/null 2>&1";
    FILE *f = popen(probe.c_str(), "r");
#endif
    if (!f)
        return false;
    char buf[256];
    while (std::fgets(buf, sizeof(buf), f)) { }
#ifdef _WIN32
    return _pclose(f) == 0;
#else
    return pclose(f) == 0;
#endif
}

static std::string detectVerifyInterpreter(const std::map<std::string, std::string> &programs)
{
    const char *cands[] = { "python3", "python", "py -3" };
    for (const char *c : cands)
    {
        std::string s = c;
        std::string name = s.substr(0, s.find(' '));
        std::map<std::string, std::string>::const_iterator it = programs.find(name);
        if (it != programs.end() && JZFileInfo::exists(it->second))
            return "\"" + it->second + "\"" + s.substr(name.size());   // 全路径要引号(含空格)
        if (probeProgramRunnable(name))
            return s;
    }
    return "python3";
}

// cmd 下双引号参数内的转义: 双引号本身要转义, % 会被当变量展开(转成 %%)。
static std::string escapeCmdArg(const std::string &s)
{
    std::string r;
    for (char c : s)
    {
        if (c == '"')
            r += "\\\"";
        else if (c == '%')
            r += "%%";
        else
            r += c;
    }
    return r;
}

// 把 detectVerifyInterpreter 返回的解释器串拆成 (程序, 前置参数) 并去掉外层引号,
// 以便直接用 JZProcess 拉起(不再经 cmd.exe /c, 避免 Windows 多层引号把路径拆坏)。
// 解释器串只可能是 "python" / "python3" / "py -3" 或配置映射的全路径(可能带引号)。
static void splitInterpreter(const std::string &interp, std::string &program,
                             std::vector<std::string> &preArgs)
{
    std::string s = interp;
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        s = s.substr(1, s.size() - 2);
    size_t i = 0, n = s.size();
    while (i < n)
    {
        while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
        if (i >= n) break;
        size_t j = i;
        while (j < n && s[j] != ' ' && s[j] != '\t') j++;
        std::string tok = s.substr(i, j - i);
        if (program.empty())
            program = tok;
        else
            preArgs.push_back(tok);
        i = j;
    }
}

// ---------------------------------------------------------------------------
// 工程骨架: 对齐 JZCodeAgent::initProject 的"标准 TS 工程"
//   每个用例先在固定位置铺好骨架(tsconfig.json + src/main.ts + js_wrapper/),
//   agent 只负责往 src/ 里写代码 —— 产物目录由 tsconfig 的 outDir 定死(<用例根>/build),
//   验证脚本据此去跑, 不需要在文件系统里搜 main.js。
//   js_wrapper 从 code_agent 的 bin 目录(code_runtime 所在目录)整棵拷一份, 使工程自包含
//   (fetch/WebSocket 等全局声明来自 js_wrapper/types, 缺了连编译都过不去)。
// ---------------------------------------------------------------------------
static bool writeTextIfAbsent(const std::string &path, const std::string &text)
{
    if (JZFileInfo::exists(path))
        return true;                       // 已存在不覆盖(与 JZCodeAgent::initProject 一致)
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;
    out << text;
    out.close();
    return JZFileInfo(path).isFile();
}

static bool initProject(const std::string &root, const std::string &codeBinDir, std::string &error)
{
    error.clear();
    JZDir mk;
    if (!mk.mkpath(root))
    {
        error = "创建工程目录失败: " + root;
        return false;
    }

    // js_wrapper 整棵拷一份进工程(部署树与源码同构: types/ + <包>/types/ + <包>/dist/)。
    bool hasJsWrapper = false;
    {
        std::string src = codeBinDir + "/js_wrapper";
        if (JZFileInfo(src).isDir())
        {
            std::string copyErr;
            if (!JZDir::copyRecursively(src, root + "/js_wrapper", false, &copyErr))
                error = "复制 js_wrapper 失败: " + copyErr;
            else
                hasJsWrapper = true;
        }
        // 源不存在: 不报错(工程只是不引用类型/运行时)
    }
    if (!error.empty())
        return false;

    std::string include = "\"src/**/*.ts\"";
    if (hasJsWrapper)
        include += ", \"js_wrapper/**/*.d.ts\"";

    // outDir=build 就是"固定的产物目录"; sourceMap 是运行期把 JS 栈翻译回 TS 源码的必备项。
    std::string tsconfig =
        "{\n"
        "  \"compilerOptions\": {\n"
        "    \"target\": \"ES2022\",\n"
        "    \"module\": \"esnext\",\n"
        "    \"moduleResolution\": \"bundler\",\n"
        "    \"lib\": [\"ES2025\"],\n"
        "    \"strict\": true,\n"
        "    \"skipLibCheck\": true,\n"
        "    \"noImplicitAny\": true,\n"
        "    \"noEmitOnError\": true,\n"
        "    \"sourceMap\": true,\n"
        "    \"outDir\": \"build\",\n"
        "    \"rootDir\": \"src\"\n"
        "  },\n"
        "  \"include\": [" + include + "],\n"
        "  \"jzCompilerOptions\": {}\n"
        "}\n";
    if (!writeTextIfAbsent(root + "/tsconfig.json", tsconfig))
    {
        error = "写入 tsconfig.json 失败: " + root;
        return false;
    }
    if (!writeTextIfAbsent(root + "/.gitignore", "build/\nnode_modules/\n"))
    {
        error = "写入 .gitignore 失败: " + root;
        return false;
    }

    // src 空则补一个可编译的占位入口(生成 dir 也建好, 与 initProject 一致)
    JZDir srcDir(root + "/src");
    std::vector<std::string> items =
        srcDir.exists() ? srcDir.entryList(JZDir::Files | JZDir::Dirs | JZDir::NoDotAndDotDot)
                        : std::vector<std::string>();
    if (items.empty())
    {
        mk.mkpath(root + "/src");
        std::string mainTs =
            "// 工程入口(示例, 可自行删改)\n"
            "// 编译: tsc -p tsconfig.json  ->  产物 build/main.js\n"
            "export function main(): void\n"
            "{\n"
            "    console.log(\"hello from project\");\n"
            "}\n"
            "main();\n";
        if (!writeTextIfAbsent(root + "/src/main.ts", mainTs))
        {
            error = "写入 src/main.ts 失败: " + root;
            return false;
        }
    }
    mk.mkpath(root + "/src/GenerateFiles");
    return true;
}

// ---- JZAgentCli 评测驱动 ----
class CliTest : public JZObject
{
public:
    // codeBinDir = code_runtime / tsc 所在的部署目录(bin/<CONFIG>): 工程骨架的
    // js_wrapper 从这里拷进每个用例工程(一棵与源码同构的树)。
    CliTest(JZEventLoop *loop, const JZCliConfig &cfg, const std::string &codeRuntime,
            const std::string &codeBinDir, const std::string &dataDir)
        : m_loop(loop), m_config(cfg), m_codeRuntime(codeRuntime),
          m_codeBinDir(codeBinDir), m_dataDir(dataDir)
    {
        m_net = new JZNetworkAccessManager(this);
    }

    void setCases(const std::vector<BenchmarkCase> &cases) { m_cases = cases; }
    void setCliPortBase(int p) { m_cliPortBase = p; }

    bool startHttpServer(int p)
    {
        m_httpPort = p;
        m_http = new JZHttpServer(this);
        m_httpHandler = new InputHttpHandler();
        m_httpHandler->getInput = [this]() { return currentInput(); };
        m_http->addRoute("/input", m_httpHandler);
        if (!m_http->listen(m_httpPort))
        {
            jzError().nospace() << "无法在端口 " << m_httpPort << " 启动输入 HTTP 服务";
            return false;
        }
        std::string extra =
            "\n\n## 读取输入\n"
            "如果任务需要从输入读取数据, 用以下方式(异步 fetch, 顶层 await):\n"
            "  const res = await fetch('http://127.0.0.1:" + std::to_string(m_httpPort) + "/input');\n"
            "  const input = await res.text();\n"
            "input 即评测注入的用例输入; 无需输入的题目不要读取。";
        m_httpPromptExtra = extra;

        m_http->moveToThread(&m_httpThread);
        m_httpThread.server = m_http;
        m_httpThread.start();
        return true;
    }

    // 收尾: 让输入 HTTP 线程退出并 join。
    // 必须在 CliTest 析构之前调用 —— 否则 JZThread 析构时会撞上仍处于 joinable 的
    // std::thread, std::thread::~thread 直接 terminate(表现为崩在 ucrtbased.dll)。
    void stopHttpServer()
    {
        if (!m_http)
            return;

        m_httpThread.quit();
        m_httpThread.wait();     // join; 线程的 run() 里已 close() 并把 server 交还主线程

        delete m_http;           // parent 是 this, 这里先释放, 避免和 CliTest 析构重复
        m_http = nullptr;
        m_httpHandler = nullptr; // 随 m_http 一起没了
    }

    void run()
    {
        if (m_cases.empty())
        {
            jzError() << "没有可运行的用例";
            m_loop->quit();
            return;
        }
        // 每个用例一个独立临时工程根(避免互相污染)
        m_baseRoot = JZPlatform::applicationTmpPath() + "/" + JZUuid::createUuid().toString() + "/agent_cli_test";
        JZDir().mkpath(m_baseRoot);
        m_benchTimer.start();
        m_idx = 0;
        startCase(0);
    }

protected:
    void startCase(size_t i)
    {
        m_case = m_cases[i];
        setInput(m_case.input);
        m_result = CaseResult{};
        m_result.name = m_case.name;

        std::string root = m_baseRoot + "/case_" + std::to_string(i);

        // 先铺好标准 TS 工程骨架(tsconfig outDir=build / rootDir=src), agent 只往 src/ 里写代码。
        // 骨架铺不成(拷 js_wrapper 失败等)就没什么可评测的, 直接判该用例失败。
        {
            std::string perr;
            if (!initProject(root, m_codeBinDir, perr))
            {
                jzError().nospace() << "[用例 " << m_case.name << "] 初始化工程失败: " << perr;
                m_result.lastAgentError = "initProject: " + perr;
                m_result.passed = false;
                finishCase();
                return;
            }
        }

        // setup 脚本: initProject 之后、agent 开始前同步执行的 python 准备脚本(如把输入写成文件);
        // 非零退出视为该用例 setup 失败(与 initProject 失败同样处理)。
        if (!m_case.setup.empty())
        {
            std::string perr;
            if (!runSetupScript(m_case, root, m_dataDir, perr))
            {
                jzError().nospace() << "[用例 " << m_case.name << "] setup 脚本失败: " << perr;
                m_result.lastAgentError = "setup: " + perr;
                m_result.passed = false;
                finishCase();
                return;
            }
        }

        // daemon 后台服务: setup 之后、agent 之前拉起, 整个用例期间常驻(同 benchmark)。
        if (!m_case.daemon.empty())
        {
            std::string derr;
            if (!startDaemon(m_case, root, derr))
            {
                jzError().nospace() << "[用例 " << m_case.name << "] daemon 启动失败: " << derr;
                m_result.lastAgentError = "daemon: " + derr;
                m_result.passed = false;
                finishCase();
                return;
            }
        }

        JZCliConfig cfg = m_config;
        cfg.rootDir = root;
        cfg.httpPort = m_cliPortBase + (int)i;   // 每用例一个端口, 便于起网页服务 + 订阅 /events

        JZAgentCli *cli = new JZAgentCli(this);
        std::string err;
        if (!cli->init(cfg, err))
        {
            jzError().nospace() << "[用例 " << m_case.name << "] 初始化失败: " << err;
            m_result.lastAgentError = err;
            m_result.passed = false;
            m_cli = cli;        // finishCase 会 deleteLater(m_cli)
            finishCase();
            return;
        }
        m_cli = cli;

        // 构造提交体。工程骨架已由本程序初始化好(见 initProject): agent 不要再另建工程/改 tsconfig,
        // 只写 src/ 下的 .ts —— 产物固定落在 build/ 下的 main.js, 验证脚本按 tsconfig 的 outDir 去跑。
        std::string system_prompt = R"(你是 TypeScript 开发助手，工程骨架已初始化好，按这套固定流程干活：
1. 直接在 `src/` 下写 `.ts` 源码，入口必须是 `src/main.ts`（编译产物为 `build/main.js`）；
2. tsconfig.json 已配置好（target ES2022，module esnext，rootDir ./src，outDir ./build，noEmitOnError），不要改它，也不要另建工程；
3. 需要编译自检时执行：`tsc -p tsconfig.json`；需要跑起来时执行：`code_runtime --entry ./build/main.js`；
4. 只用 TS 写代码，不要直接写 .js（build/ 下的 js 是编译产物）。)";
        // 运行时的模块/生命周期边界(与 Node 不同, 模型猜不出来): 共用 agent_tool 里那一份
        system_prompt += "\n" + jzTsRuntimeNotes();
        // 起服务类用例怎么跑(terminal_run 的 background / query / cancel): 同上, 只有一份
        system_prompt += "\n" + jzTerminalRunNotes();

        std::string prompt = system_prompt + "\n" + m_case.prompt;
        // setup 脚本已把输入写成文件的用例, 不再注入 fetch 提示(避免与"读本地文件"冲突)
        if (!m_case.input.empty() && !m_httpPromptExtra.empty() && m_case.setup.empty())
            prompt += m_httpPromptExtra;

        // 验证命令: 用例给了 verify 就用它(benchmark 原样搬过来); 没给但有 expect.contains/equals,
        // 就挂通用 stdout 验证(data/stdout_verify.py: 先 tsc 编译 -> 按 tsconfig 定位产物
        // -> 跑 code_runtime -> 比对输出), 即 benchmark 宿主原本内联的那段判定。
        std::string verifyCmd = m_case.verifyCmd;
        if (verifyCmd.empty() && (!m_case.expectContains.empty() || !m_case.expectEquals.empty()))
            verifyCmd = buildStdoutVerifyCmd(cfg, root, m_case);

        Json::Value req;
        req["prompt"] = prompt;
        if (!verifyCmd.empty())
            req["verify_cmd"] = expandVerifyCmd(verifyCmd, root);
        req["verify_timeout"] = m_case.timeoutMs;
        req["max_verify_rounds"] = m_case.maxAttempts;

        jzDebug().nospace() << clr(CLR_YELLOW) << "==== 用例[" << m_case.name
                            << "] 开始 (maxAttempts=" << m_case.maxAttempts
                            << ", root=" << root << ") ====" << clr(CLR_RESET);

        // 打印本用例实际生效的程序映射与验证命令: 看不到 tsc/code_runtime 就说明外部没传进来;
        // 验证命令为空 = 本用例没有判定标准(收尾只看 agent 自己有没有报错)。
        std::string progs;
        for (const auto &kv : cli->programPaths())
        {
            if (!progs.empty())
                progs += ", ";
            progs += kv.first + "=" + kv.second;
        }

        // 起网页那套 HTTP(POST /task + GET /events), 再以客户端身份订阅事件流 -> 实时打印过程
        cli->startHttp();
        startSse(cfg.httpPort);

        m_pendingReq = req;
        waitSseThenSubmit(0);
    }

    // 等 SSE 首帧(hello)到了再提交, 免得漏掉开头的过程; 连不上也不卡着: 3s 后照提
    void waitSseThenSubmit(int waitedMs)
    {
        if (m_sseHello || waitedMs >= 3000)
        {
            m_llmStartMs = JZPlatform::getTickCountMs();
            m_cli->submitTask(m_pendingReq, [this](const Json::Value &r) { onSubmitDone(r); });
            return;
        }
        // 服务端 listen() 是异步的: 头一两次订阅可能扑空(连接直接失败), 隔 500ms 重试一次
        if (!m_sse && waitedMs > 0 && (waitedMs % 500) == 0)
            startSse(m_ssePort);

        JZTimer::singleShot(100, [this, waitedMs]() { waitSseThenSubmit(waitedMs + 100); });
    }

    // -----------------------------------------------------------------------
    // 实时过程: 订阅当前用例 JZAgentCli 的 GET /events(网页同款 SSE), 打到控制台
    // -----------------------------------------------------------------------
    void startSse(int port)
    {
        m_ssePort = port;
        std::string url = "http://127.0.0.1:" + std::to_string(port) + "/events";
        JZNetworkRequest req(url);
        req.setHeader(JZNetworkRequest::AcceptHeader, "text/event-stream");
        m_sse = m_net->get(req);
        m_sse->setTimeOut(-1);          // 长连接: 别按默认 30s 空闲超时掐断
        connect(&m_sse->sigReadyRead, this, [this]() { onSseData(); });
        connect(&m_sse->sigFinished, this, [this]() { onSseFinished(); });
    }

    void stopSse()
    {
        flushStream();                  // 把攒着的思考/回复尾巴打出来
        if (!m_sse)
            return;
        m_sse->close();                 // -> UserCancelError; sigFinished 里 deleteLater
        m_sse = nullptr;
        m_sseBuf.clear();
        m_sseHello = false;
    }

    void onSseData()
    {
        if (!m_sse)
            return;
        m_sseBuf += str_fromUtf8(m_sse->readAll());

        size_t pos;
        while ((pos = m_sseBuf.find("\n\n")) != std::string::npos)
        {
            std::string block = m_sseBuf.substr(0, pos);
            m_sseBuf.erase(0, pos + 2);

            size_t d = block.find("data: ");
            if (d == std::string::npos)
                continue;
            Json::Value ev;
            Json::Reader rd;
            if (!rd.parse(block.substr(d + 6), ev) || !ev.isMember("type"))
                continue;
            onSseEvent(ev);
        }
    }

    void onSseEvent(const Json::Value &ev)
    {
        std::string type = ev["type"].asString();
        const Json::Value &d = ev["data"];

        if (type == "hello")
        {
            m_sseHello = true;          // 连接就绪
            return;
        }
        // 思考/回复是流式增量: 攒起来, 遇到别的事件或用例结束再整段打, 免得一个 token 一行
        if (type == "thinking")
        {
            m_thinkBuf += d.isMember("text") ? d["text"].asString() : "";
            return;
        }
        if (type == "reply")
        {
            m_sayBuf += d.isMember("text") ? d["text"].asString() : "";
            return;
        }
        flushStream();

        if (type == "tool_call")
        {
            std::string args = d.isMember("args") ? d["args"].asString() : "";
            procLine(clr(CLR_YELLOW), "[工具] " + (d.isMember("name") ? d["name"].asString() : "tool"),
                     args, 240);
            return;
        }
        if (type == "tool_result")
        {
            bool ok = d.isMember("ok") ? d["ok"].asBool() : false;
            std::string res = d.isMember("result") ? d["result"].asString() : "";
            procLine(ok ? clr(CLR_GREEN) : clr(CLR_RED), ok ? "[结果]" : "[结果-失败]", res, 240);
            return;
        }
        if (type == "verify")
        {
            bool passed = d.isMember("passed") ? d["passed"].asBool() : false;
            std::string txt = "第 " + std::to_string(d.isMember("round") ? d["round"].asInt() + 1 : 1)
                              + " 轮验证: " + (passed ? "通过" : "失败(退出码 "
                              + std::to_string(d.isMember("exit_code") ? d["exit_code"].asInt() : -1) + ")");
            jzDebug().nospace() << "  " << clr(passed ? CLR_GREEN : CLR_RED) << txt.c_str() << clr(CLR_RESET);
            return;
        }
        if (type == "error")
        {
            procLine(clr(CLR_RED), "[错误]", d.isMember("text") ? d["text"].asString() : "", 300);
            return;
        }
        if (type == "retry")
        {
            std::string tag = "[重试] "
                + std::to_string(d.isMember("retry") ? d["retry"].asInt() : 1) + "/"
                + std::to_string(d.isMember("max_retries") ? d["max_retries"].asInt() : 0);
            procLine(clr(CLR_YELLOW), tag, d.isMember("error") ? d["error"].asString() : "", 300);
            return;
        }
    }

    void onSseFinished()
    {
        // 用例结束时 CliTest 主动 close() -> UserCancelError; 服务端先关则是 ConnectError(无终止块),
        // 都不算问题 —— 过程已经在 sigReadyRead 里打过了
        if (m_sse)
            m_sse->deleteLater();
        m_sse = nullptr;
    }

    void flushStream()
    {
        if (!m_thinkBuf.empty())
        {
            procLine("", "思考", m_thinkBuf, 240);
            m_thinkBuf.clear();
        }
        if (!m_sayBuf.empty())
        {
            procLine("", "回复", m_sayBuf, 300);
            m_sayBuf.clear();
        }
    }

    // 单行过程输出: 折叠换行、截断长度, 免得长文本把控制台刷爆
    // (std::string 传给 jzDebug 会被加引号, 所以这里统一 .c_str())
    void procLine(const char *color, const std::string &tag, const std::string &text, int maxLen)
    {
        std::string t = text;
        for (char &c : t)
            if (c == '\r' || c == '\n')
                c = ' ';
        if ((int)t.size() > maxLen)
            t = t.substr(0, maxLen) + "…";
        if (!t.empty())
            t = " " + t;
        jzDebug().nospace() << "  " << color << tag.c_str() << t.c_str() << clr(CLR_RESET);
    }

    void onSubmitDone(const Json::Value &r)
    {
        m_result.llmMs += (int)(JZPlatform::getTickCountMs() - m_llmStartMs);

        std::string status;
        if (r.isMember("status") && r["status"].isString())
            status = r["status"].asString();
        else if (r.isMember("error"))
            status = "error";   // 忙 / 空 prompt 等前置错误

        m_result.lastStatus = status;
        m_result.passed = (status == "success");

        if (r.isMember("verify_rounds"))
            m_result.attempts = r["verify_rounds"].asInt();
        else
            m_result.attempts = 1;

        if (r.isMember("last_verify_output"))
            m_result.lastVerifyOutput = r["last_verify_output"].asString();
        if (r.isMember("error"))
            m_result.lastAgentError = r["error"].asString();
        if (r.isMember("agent_reply"))
            m_result.lastVerifyOutput += "\n[agent_reply] " + r["agent_reply"].asString();

        // 抓取本用例累计 token(下一用例会重建 agent / context 清零)
        if (m_cli && m_cli->agent())
        {
            const auto &u = m_cli->agent()->context()->usage;
            m_result.promptTokens = u.prompt_tokens;
            m_result.completionTokens = u.completion_tokens;
            m_result.totalTokens = u.total_tokens;
            m_result.cachedTokens = u.cached_tokens;
            // 与"计费总量"不同口径: windowTokens() = 最后一轮实际会发出的上下文
            // (锚点那条 usage.prompt_tokens, 含 system+工具+历史, 不含思考)。
            m_result.contextTokens = m_cli->agent()->windowTokens();
            if (m_result.contextTokens > m_maxContextTokens)
            {
                m_maxContextTokens = m_result.contextTokens;
                m_maxContextName = m_result.name;
            }
            m_result.promptMs = u.prompt_ms;
            m_result.predMs = u.predicted_ms;
            m_result.draftN = u.draft_tokens;
            m_result.draftAccepted = u.draft_accepted_tokens;
            m_totalPrompt += u.prompt_tokens;
            m_totalCompletion += u.completion_tokens;
            m_totalTokens += u.total_tokens;
            m_totalCached += u.cached_tokens;
            m_totalPromptMs += u.prompt_ms;
            m_totalPredMs += u.predicted_ms;
            m_totalDraft += u.draft_tokens;
            m_totalDraftAccepted += u.draft_accepted_tokens;
        }
        m_totalLlmMs += m_result.llmMs;

        finishCase();
    }

    void finishCase()
    {
        stopDaemon();       // 先收掉本用例的后台服务(避免残留占住端口影响下一个用例)

        const CaseResult &r = m_result;
        jzDebug().nospace() << "---- 用例[" << r.name.c_str() << "] 结束: "
                            << clr(r.passed ? CLR_GREEN : CLR_RED)
                            << (r.passed ? "PASS" : "FAIL") << clr(CLR_RESET)
                            << " (status=" << (r.lastStatus.empty() ? "(none)" : r.lastStatus.c_str())
                            << ", verify轮次=" << r.attempts << ")";
        // 两种口径分开打, 免得把"计费总量"当成"上下文大小":
        //   计费 = Σ 每轮的整段 prompt + 生成(同一段 system/工具/历史会被计入轮数遍)
        //   上下文 = 最后一轮实际会发出的量(windowTokens)
        // 缓存命中写成 "命中/整段 (百分比)" —— 命中部分**也算进了 prompt**, 所以只说"省了 prefill"
        const std::string cacheNote = (r.promptTokens > 0)
            ? (std::to_string(r.cachedTokens) + "/" + std::to_string(r.promptTokens) + " (" +
               std::to_string(r.cachedTokens * 100 / r.promptTokens) + "%)")
            : std::string("0");
        jzDebug().nospace() << "     计费 token=" << r.totalTokens
                            << " = prompt " << r.promptTokens << " + completion " << r.completionTokens
                            << "   " << clr(CLR_GRAY) << "[总成本口径: 每轮整段 prompt 相加; 其中缓存命中 "
                            << cacheNote.c_str() << " 已含在 prompt 里]" << clr(CLR_RESET);
        jzDebug().nospace() << "     上下文(最后一轮)=" << r.contextTokens
                            << "   " << clr(CLR_GRAY) << "[windowTokens: 预算/压缩看这个, 与计费总量不同口径]"
                            << clr(CLR_RESET);
        const std::string speed = (r.predMs > 0)
            ? ("，生成 " + std::to_string((int)(r.completionTokens * 1000.0 / r.predMs)) + " tok/s")
            : std::string();
        const std::string mtp = (r.draftN > 0)
            ? ("，MTP " + std::to_string(r.draftAccepted) + "/" + std::to_string(r.draftN))
            : std::string();
        jzDebug().nospace() << "     LLM " << sec1(r.llmMs).c_str() << "s" << speed.c_str()
                            << mtp.c_str();
        if (!r.passed && !r.lastAgentError.empty())
            jzDebug().nospace() << "    agentError: " << r.lastAgentError.substr(0, 400);
        if (!r.passed && !r.lastVerifyOutput.empty())
            jzDebug().nospace() << "    verifyOutput: " << r.lastVerifyOutput.substr(0, 400);

        m_results.push_back(r);

        stopSse();                  // 先断开本用例的事件流, 再销毁它的 agent_cli
        if (m_cli)
            m_cli->deleteLater();
        m_cli = nullptr;

        size_t next = m_idx + 1;
        if (next < m_cases.size())
        {
            m_idx = next;
            JZTimer::singleShot(0, [this, next]() { startCase(next); });
        }
        else
        {
            reportAndQuit();
        }
    }

    void reportAndQuit()
    {
        int passed = 0;
        jzDebug() << "\n================ agent_cli 评测结果 ================";
        for (const auto &r : m_results)
        {
            if (r.passed)
                passed++;
            const std::string cacheNote = (r.promptTokens > 0)
                ? (std::to_string(r.cachedTokens) + "/" + std::to_string(r.promptTokens) + " (" +
                   std::to_string(r.cachedTokens * 100 / r.promptTokens) + "%)")
                : std::string("0");
            const std::string speed = (r.predMs > 0)
                ? ("，生成 " + std::to_string((int)(r.completionTokens * 1000.0 / r.predMs)) + " tok/s")
                : std::string();
            const std::string mtp = (r.draftN > 0)
                ? ("，MTP " + std::to_string(r.draftAccepted) + "/" + std::to_string(r.draftN))
                : std::string();
            jzDebug().nospace() << clr(r.passed ? CLR_GREEN : CLR_RED)
                                << (r.passed ? "[PASS] " : "[FAIL] ") << clr(CLR_RESET)
                                << r.name.c_str()
                                << "  (status=" << (r.lastStatus.empty() ? "(none)" : r.lastStatus.c_str())
                                << ", verify轮次=" << r.attempts
                                << ", 计费=" << r.totalTokens
                                << " [prompt " << r.promptTokens
                                << " 其中缓存命中 " << cacheNote.c_str()
                                << " + completion " << r.completionTokens
                                << "], 上下文=" << r.contextTokens
                                << ", LLM " << sec1(r.llmMs).c_str() << "s"
                                << speed.c_str() << mtp.c_str() << ")";
            if (!r.passed)
            {
                if (!r.lastAgentError.empty())
                    jzDebug().nospace() << "    agentError: " << r.lastAgentError.substr(0, 400);
                if (!r.lastVerifyOutput.empty())
                    jzDebug().nospace() << "    verifyOutput: " << r.lastVerifyOutput.substr(0, 300);
            }
        }
        jzDebug().nospace() << "总分: " << passed << " / " << m_results.size();
        jzDebug().nospace() << "总耗时: " << sec1(m_benchTimer.elapsed()).c_str() << " s";
        // 计费口径: Σ prompt + Σ completion。prompt 里已经含缓存命中的那部分
        // (llama.cpp: prompt = prompt_n + cache_n; prompt_n 只是本次真正 prefill 的量),
        // 所以"这次烧了多少"看 total, 缓存命中率只说明 prefill 省了多少。
        const std::string totalCache = (m_totalPrompt > 0)
            ? (std::to_string(m_totalCached) + "/" + std::to_string(m_totalPrompt) + " (" +
               std::to_string(m_totalCached * 100 / m_totalPrompt) + "%)")
            : std::string("0");
        jzDebug().nospace() << "总消耗 token(计费口径) = " << m_totalTokens
                            << " = prompt " << m_totalPrompt
                            << " 其中缓存命中 " << totalCache.c_str()
                            << " + completion " << m_totalCompletion
                            << "   " << clr(CLR_GRAY) << "[每轮整段 prompt 相加, 横向比成本用]" << clr(CLR_RESET);
        if (m_maxContextTokens >= 0)
            jzDebug().nospace() << "上下文(各用例最后一轮的最大值) = " << m_maxContextTokens
                                << " (" << m_maxContextName.c_str() << ")"
                                << "   " << clr(CLR_GRAY) << "[windowTokens; 预算/压缩看这个]" << clr(CLR_RESET);
        // llama.cpp 计时: 其他 provider 不带 timings(恒为 0)，不打这两行
        if (m_totalPromptMs > 0 || m_totalPredMs > 0)
        {
            // ⚠ prefill 速率**只算未命中的 prompt token**: prompt_ms 是 llama.cpp 真正 prefill
            //   的耗时, 命中前缀缓存的那些 token 没走 prefill —— 拿整段 prompt 去除会虚高好几倍
            //   (实测命中 84% 时能算出 1 万多 tok/s)。分子分母的口径必须对得上。
            const int prefilled = m_totalPrompt > m_totalCached ? m_totalPrompt - m_totalCached : 0;
            jzDebug().nospace() << "推理速度(llama.cpp): prefill="
                                << (m_totalPromptMs > 0 ? prefilled * 1000.0 / m_totalPromptMs : 0.0)
                                << " tok/s [只算未命中 " << prefilled << "/" << m_totalPrompt
                                << ", 缓存命中 " << m_totalCached << "]"
                                << ", 生成="
                                << (m_totalPredMs > 0 ? m_totalCompletion * 1000.0 / m_totalPredMs : 0.0)
                                << " tok/s";
        }
        if (m_totalDraft > 0)
            jzDebug().nospace() << "投机采样(MTP): 接受 " << m_totalDraftAccepted << "/" << m_totalDraft
                                << " = " << (m_totalDraftAccepted * 100.0 / m_totalDraft) << "%";
        jzDebug().nospace() << "耗时分解: 总=" << sec1(m_benchTimer.elapsed()).c_str() << "s"
                            << "  LLM=" << sec1(m_totalLlmMs).c_str() << "s";
        jzDebug() << "=====================================================";

        m_loop->quit();
    }

    // 把 verify 命令模板里的占位符替换为实际路径/端口(与 benchmark 同语义)
    std::string expandVerifyCmd(const std::string &tpl, const std::string &root)
    {
        std::string s = tpl;
        str_replace(s, "${root}", root);
        str_replace(s, "${build}", root + "/build");
        str_replace(s, "${runtime}", m_codeRuntime);
        str_replace(s, "${port}", std::to_string(m_case.verifyPort));
        str_replace(s, "${bench}", JZPlatform::applicationDirPath());
        str_replace(s, "${data}", m_dataDir);
        return s;
    }

    // daemon 后台服务: setup 之后拉起, 整个用例期间常驻(供产物连接, 如模拟下位机/MES),
    // finishCase() 时连子孙进程一起杀掉。与 setup 脚本的差别是"不等待它退出":
    //   ①进程在启动瞬间就退出(端口被占等) => 判本用例失败;
    //   ②轮询 stdout, 有输出即视为就绪(脚本 listen 完通常打印一行 ready), 超时不判失败。
    bool startDaemon(const BenchmarkCase &c, const std::string &root, std::string &err)
    {
        std::string cmd = expandVerifyCmd(c.daemon, root);
        m_daemon = new JZProcess(this);
        m_daemon->setProcessChannelMode(JZProcess::MergedChannels);
        if (!m_daemon->start("cmd.exe", { "/c", cmd }))
        {
            std::string e = "无法启动 daemon: " + cmd;
            delete m_daemon;
            m_daemon = nullptr;
            err = e;
            return false;
        }

        int waited = 0;
        while (waited < c.daemonReadyMs)
        {
            JZThread::msleep(100);
            waited += 100;
            if (!m_daemon->readAllStandardOutput().isEmpty())
                break;
            if (m_daemon->state() == JZProcess::NotRunning)
                break;
        }

        if (m_daemon->state() == JZProcess::NotRunning)
        {
            std::string out = str_fromLocal(m_daemon->readAllStandardOutput());
            int rc = m_daemon->exitCode();
            delete m_daemon;
            m_daemon = nullptr;
            err = "daemon 启动后立即退出(退出码 " + std::to_string(rc) + "): " + cmd
                  + (out.empty() ? std::string() : ("\n" + out));
            return false;
        }
        jzDebug().nospace() << clr(CLR_GRAY) << "[cli_test] daemon 已启动: " << cmd << clr(CLR_RESET);
        return true;
    }

    // 收掉本用例的 daemon(kill 之前不读的话管道里会留脏数据, 读了丢掉即可)
    void stopDaemon()
    {
        if (!m_daemon)
            return;
        m_daemon->killProcess();
        m_daemon->readAllStandardOutput();
        delete m_daemon;
        m_daemon = nullptr;
    }

    // setup 脚本: initProject 之后、agent 开始前由宿主同步执行。解释器复用 detectVerifyInterpreter
    // (programPaths 优先, 否则探测 python3/python/py); 直接拉起解释器 + 脚本 + 参数(JZProcess, 不经
    // cmd.exe /c, 避免 Windows 多层引号把路径拆坏), 传入:
    //   --root <工程根>  --input <输入>  --data <评测集目录>
    // 脚本非零退出视为该用例 setup 失败。
    bool runSetupScript(const BenchmarkCase &c, const std::string &root,
                        const std::string &dataDir, std::string &err)
    {
        std::string setupPath = c.setup;
        if (!JZDir::isAbsolutePath(setupPath))
            setupPath = c.sourceDir + "/" + setupPath;
        if (!JZFileInfo::exists(setupPath))
        {
            err = "setup 脚本不存在: " + setupPath;
            return false;
        }
        // 直接拉起解释器 + 脚本 + 参数(不再经 cmd.exe /c), 避免 Windows 多层引号转义把路径拆坏。
        // 同样的坑见 JZAgentCli::runVerifyAsync 注释: JZProcess 给含空格参数加引号时把内层 " 转义成
        //  \", 而 cmd.exe 不认 \", 导致 python 收到 "...\"C:\..." 这样的坏路径。
        std::string interpreter = detectVerifyInterpreter(m_config.programPaths);
        std::string program;
        std::vector<std::string> args;
        splitInterpreter(interpreter, program, args);
        args.push_back(setupPath);
        args.push_back("--root");   args.push_back(root);
        args.push_back("--input");  args.push_back(c.input);
        args.push_back("--data");   args.push_back(dataDir);
        JZProcess proc;
        proc.setProcessChannelMode(JZProcess::MergedChannels);
        if (!proc.start(program, args))
        {
            err = "无法启动 setup 脚本: " + interpreter + " " + setupPath;
            return false;
        }
        if (!proc.waitForFinished(60000))
        {
            proc.killProcess();
            err = "setup 脚本超时(60s)";
            return false;
        }
        int rc = proc.exitCode();
        if (rc != 0)
        {
            err = "setup 脚本退出码 " + std::to_string(rc) + ": "
                  + str_fromLocal(proc.readAllStandardOutput());
            return false;
        }
        return true;
    }

    // 通用 stdout 验证命令(等价于 benchmark 宿主内联的 check):
    //   <python> "${data}/stdout_verify.py" --tsc "<tsc 全路径>" --runtime "${runtime}" \
    //       --root "${root}" [--contains "<expect.contains>"] [--equals "<expect.equals>"] \
    //       [--case-insensitive] --timeout <秒>
    // 脚本按传入的这几个路径干活(不搜索): 先 tsc -p <root>/tsconfig.json 编译, 再按 tsconfig 的
    // compilerOptions.outDir 定位产物目录(骨架里是 build), 检查 main.js 是否真的生成了,
    // 最后跑 code_runtime 比对 stdout。退出码 0=通过 / 1=未通过(输出喂回模型重试)。
    // 解释器(哪个 python)与 tsc 全路径在这里定死(都是外部探测到的), ${data}/${runtime}/${root}
    // 留给 expandVerifyCmd 展开。
    std::string buildStdoutVerifyCmd(const JZCliConfig &cfg, const std::string &root,
                                     const BenchmarkCase &c)
    {
        std::string tsc;
        std::map<std::string, std::string>::const_iterator it = cfg.programPaths.find("tsc");
        if (it != cfg.programPaths.end())
            tsc = it->second;

        std::string cmd = detectVerifyInterpreter(cfg.programPaths)
                          + " \"${data}/stdout_verify.py\""
                          + " --tsc \"" + escapeCmdArg(tsc) + "\""
                          + " --runtime \"${runtime}\""
                          + " --root \"${root}\"";
        // contains / equals 各自非空才下发(equals 型用例可以没有 contains)
        if (!c.expectContains.empty())
            cmd += " --contains \"" + escapeCmdArg(c.expectContains) + "\"";
        if (!c.expectEquals.empty())
            cmd += " --equals \"" + escapeCmdArg(c.expectEquals) + "\"";
        if (c.expectCaseInsensitive)
            cmd += " --case-insensitive";

        // 产物运行上限: 比验证总超时留出余量(验证超时 = 用例 timeout, 见 req["verify_timeout"])。
        int secs = c.timeoutMs / 1000 - 10;
        if (secs < 5)
            secs = 5;
        cmd += " --timeout " + std::to_string(secs);
        return cmd;
    }

    std::string currentInput()
    {
        std::lock_guard<std::mutex> lk(m_inputMutex);
        return m_input;
    }
    void setInput(const std::string &s)
    {
        std::lock_guard<std::mutex> lk(m_inputMutex);
        m_input = s;
    }

    JZEventLoop *m_loop;
    JZCliConfig m_config;
    std::string m_codeRuntime;
    std::string m_codeBinDir;      // code_runtime / tsc 所在目录(工程骨架取 js_wrapper/)
    std::string m_dataDir;

    JZAgentCli *m_cli = nullptr;
    JZProcess *m_daemon = nullptr;   // 本用例的后台服务(daemon 字段拉起), finishCase() 时杀掉
    std::string m_baseRoot;
    int64_t m_llmStartMs = 0;

    std::vector<BenchmarkCase> m_cases;
    size_t m_idx = 0;
    BenchmarkCase m_case;
    CaseResult m_result;

    std::vector<CaseResult> m_results;
    JZElapsedTimer m_benchTimer;
    int m_totalPrompt = 0;
    int m_totalCompletion = 0;
    int m_totalTokens = 0;
    int m_totalCached = 0;
    double m_totalPromptMs = 0;   // 全部用例累计 prefill 耗时(llama.cpp)
    double m_totalPredMs = 0;     // 全部用例累计生成耗时(llama.cpp)
    int m_totalDraft = 0;         // 全部用例累计投机采样草稿数
    int m_totalDraftAccepted = 0; // 全部用例累计投机采样被接受数
    int m_maxContextTokens = -1;  // 各用例"最后一轮上下文"的最大值(用例会话互相独立, 只报最大)
    std::string m_maxContextName;
    int64_t m_totalLlmMs = 0;

    // 网页服务的客户端侧: 订阅 GET /events, 把事件流实时打到控制台
    JZNetworkAccessManager *m_net = nullptr;
    JZNetworkReply *m_sse = nullptr;
    int m_ssePort = 0;
    std::string m_sseBuf;
    bool m_sseHello = false;
    std::string m_thinkBuf;
    std::string m_sayBuf;
    Json::Value m_pendingReq;
    int m_cliPortBase = 19090;

    JZHttpServer *m_http = nullptr;
    InputHttpHandler *m_httpHandler = nullptr;
    int m_httpPort = 18090;
    std::string m_input;
    std::mutex m_inputMutex;
    std::string m_httpPromptExtra;
    HttpThread m_httpThread;
};

// 把 src 中所有 from 替换为 to
static void replaceAll(std::string &s, const std::string &from, const std::string &to)
{
    if (from.empty())
        return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos)
    {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}
// 兼容 benchmark 里的 str_replace(别名)
static void str_replace(std::string &s, const std::string &from, const std::string &to)
{
    replaceAll(s, from, to);
}

// 简单通配符匹配（用于 --cases 过滤，与 code_agent_benchmark 一致）：
//   *  : 匹配任意长度（含空串）的任意字符
//   ?  : 匹配任意一个字符
//   .  : 也当作“任意一个字符”匹配（这样 jsapi.* 也能匹配 jsapi_core_fs 这类带下划线的名）
//   其余字符：逐字符精确比较（区分大小写）。无通配符时退化为精确匹配。
static bool wildcardMatch(const std::string &pat, const std::string &str)
{
    const char *p = pat.c_str();
    const char *s = str.c_str();
    const char *star = nullptr;
    const char *mark = s;
    while (*s)
    {
        if (*p == '*')
        {
            star = p++;
            mark = s;
        }
        else if (*p == *s || *p == '?' || *p == '.')
        {
            p++; s++;
        }
        else if (star)
        {
            p = star + 1;
            s = ++mark;
        }
        else
        {
            return false;
        }
    }
    while (*p == '*')
        p++;
    return *p == '\0';
}

// ---- 用例加载(与 code_agent_benchmark 一致)----
static bool parseCaseObject(const Json::Value &root, const std::string &src, BenchmarkCase &c)
{
    if (!root.isObject())
    {
        jzWarning().nospace() << "[评测集] 不是 JSON 对象，跳过: " << src;
        return false;
    }
    c.name = root.isMember("name") && root["name"].isString()
                 ? root["name"].asString() : src;
    c.prompt = root.isMember("prompt") && root["prompt"].isString()
                   ? root["prompt"].asString() : "";
    c.maxAttempts = root.isMember("maxAttempts") ? root["maxAttempts"].asInt() : 3;
    c.timeoutMs = (root.isMember("timeout") ? root["timeout"].asInt() : 30) * 1000;
    c.input = (root.isMember("input") && root["input"].isString())
                  ? root["input"].asString() : "";
    c.verifyCmd = (root.isMember("verify") && root["verify"].isString())
                      ? root["verify"].asString() : "";
    c.verifyPort = root.isMember("verifyPort") ? root["verifyPort"].asInt() : 3900;
    c.setup = (root.isMember("setup") && root["setup"].isString())
                ? root["setup"].asString() : "";
    c.daemon = (root.isMember("daemon") && root["daemon"].isString())
                ? root["daemon"].asString() : "";
    c.daemonReadyMs = root.isMember("daemonReadyMs") ? root["daemonReadyMs"].asInt() : 3000;
    {
        std::string s = src;
        size_t p = s.find_last_of("/\\");
        c.sourceDir = (p == std::string::npos) ? std::string(".") : s.substr(0, p);
    }

    // 输出预期(与 benchmark 的 expect 同解析): 没有 verify 时用它组织通用 stdout 验证
    // 互斥优先级: equals > contains(与 benchmark 宿主一致)
    if (root.isMember("expect") && root["expect"].isObject())
    {
        const Json::Value &exp = root["expect"];
        if (exp.isMember("contains") && exp["contains"].isString())
            c.expectContains = exp["contains"].asString();
        if (exp.isMember("equals") && exp["equals"].isString())
            c.expectEquals = exp["equals"].asString();
        if (exp.isMember("caseInsensitive"))
            c.expectCaseInsensitive = exp["caseInsensitive"].asBool();
    }

    if (c.prompt.empty())
    {
        jzWarning().nospace() << "[评测集] 用例缺少 prompt，跳过: " << src;
        return false;
    }
    return true;
}

static bool parseCaseText(const std::string &rawText, const std::string &src, BenchmarkCase &c)
{
    std::string text = rawText;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        text.erase(0, 3);
    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(text, root))
    {
        jzWarning().nospace() << "[评测集] 解析失败: " << src;
        return false;
    }
    return parseCaseObject(root, src, c);
}

static std::string parentDirOf(const std::string &path)
{
    size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? "." : path.substr(0, p);
}

static bool loadIndex(const std::string &indexPath, const std::string &baseDir,
                      std::vector<BenchmarkCase> &out, std::string &err)
{
    std::ifstream in(indexPath, std::ios::binary);
    if (!in)
    {
        err = "无法打开评测集索引: " + indexPath;
        return false;
    }
    int lineNo = 0;
    std::string line;
    while (std::getline(in, line))
    {
        lineNo++;
        size_t a = line.find_first_not_of(" \t\r\n");
        if (a == std::string::npos)
            continue;
        size_t b = line.find_last_not_of(" \t\r\n");
        std::string trimmed = line.substr(a, b - a + 1);

        Json::Value idx;
        Json::Reader reader;
        if (!reader.parse(trimmed, idx))
            continue;
        std::string rel;
        bool enabled = true;
        if (idx.isString())
            rel = idx.asString();
        else if (idx.isObject())
        {
            if (idx.isMember("file") && idx["file"].isString())
                rel = idx["file"].asString();
            if (idx.isMember("enabled"))
                enabled = idx["enabled"].asBool();
        }
        if (rel.empty() || !enabled)
            continue;

        std::string casePath = rel;
        if (!JZDir::isAbsolutePath(rel))
            casePath = baseDir + "/" + rel;
        if (!JZFileInfo::exists(casePath))
            continue;
        std::ifstream fin(casePath, std::ios::binary);
        if (!fin)
            continue;
        std::stringstream ss;
        ss << fin.rdbuf();
        BenchmarkCase c;
        if (parseCaseText(ss.str(), casePath, c))
            out.push_back(std::move(c));
    }
    if (out.empty())
    {
        err = "索引中无可用用例";
        return false;
    }
    return true;
}

static bool loadJsonDir(const std::string &dataDir, std::vector<BenchmarkCase> &out, std::string &err)
{
    JZDir dir(dataDir);
    auto files = dir.entryInfoList({ "*.json" }, JZDir::Files);
    for (const auto &fi : files)
    {
        std::string path = dataDir + "/" + fi.fileName();
        std::ifstream in(path, std::ios::binary);
        if (!in)
            continue;
        std::stringstream ss;
        ss << in.rdbuf();
        BenchmarkCase c;
        if (parseCaseText(ss.str(), path, c))
            out.push_back(std::move(c));
    }
    if (out.empty())
    {
        err = "目录下无可用用例";
        return false;
    }
    return true;
}

static bool loadCases(const std::string &dataDir, std::vector<BenchmarkCase> &out, std::string &err)
{
    if (!JZFileInfo::exists(dataDir))
    {
        err = "评测集路径不存在: " + dataDir;
        return false;
    }
    JZFileInfo dirInfo(dataDir);
    if (dirInfo.isDir())
    {
        std::string index = dataDir + "/cases.jsonl";
        if (JZFileInfo::exists(index))
            return loadIndex(index, dataDir, out, err);
        return loadJsonDir(dataDir, out, err);
    }
    if (dataDir.size() >= 6 && dataDir.compare(dataDir.size() - 6, 6, ".jsonl") == 0)
        return loadIndex(dataDir, parentDirOf(dataDir), out, err);
    err = "评测集路径必须是目录或 .jsonl 索引文件: " + dataDir;
    return false;
}

// 路径的最后一段目录名(不看尾斜杠)
static std::string lastDirName(const std::string &path)
{
    std::string s = path;
    while (s.size() > 1 && s.back() == '/')
        s.pop_back();
    size_t pos = s.find_last_of('/');
    return pos == std::string::npos ? s : s.substr(pos + 1);
}

// 在某个基目录下按相对路径找程序: 产物可能在 Debug / Release 两个子目录里, 逐个试。
static std::string findUnder(const std::string &baseDir, const std::vector<std::string> &names)
{
    // binary/ 下的 exe 统一在 <build>/binary/bin/<配置>/(见 binary/CMakeLists.txt),
    // 后面几项是历史布局的兜底。
    static const char *kSubs[] = { "/binary/bin/Release", "/binary/bin/Debug",
                                   "/bin/Release", "/bin/Debug", "" };

    std::string base = baseDir;
    while (base.size() > 1 && base.back() == '/')
        base.pop_back();
    if (base.empty())
        return std::string();

    for (const std::string &name : names)
    {
        for (int k = 0; k < (int)(sizeof(kSubs) / sizeof(kSubs[0])); k++)
        {
            std::string cand = base + kSubs[k] + "/" + name;
            if (JZFileInfo::exists(cand))
                return codeNormalizePath(cand);
        }
    }
    return std::string();
}

// 定位程序(tsc / code_runtime 随部署走, 路径不固定): 从 exe 目录逐级**向上找到 build 目录**,
// 再在 build 里按名字找 —— 仓库内构建就是 <build>/binary/bin/<CONFIG>/xxx.exe。
// 找不到返回空串(那就没得用: 不填进配置)。
static std::string detectProgram(const std::string &nameNoExt)
{
    std::vector<std::string> names;   // Windows 带 .exe, 其它平台不带后缀
    names.push_back(nameNoExt + ".exe");
    names.push_back(nameNoExt);

    JZDir dir(JZPlatform::applicationDirPath());   // 构造即把 '\' 归一成 '/'
    std::string exeDir = dir.path();
    for (int level = 0; level < 12; level++)
    {
        if (lastDirName(dir.path()) == "build")
            return findUnder(dir.path(), names);   // 找到 build 就在它里面找, 不再往外找
        if (!dir.cdUp())
            break;
    }
    return findUnder(exeDir, names);   // 没有 build 祖先: exe 附近找
}

// ---- 大模型接入: 一律从 **exe 同目录的 test_llm.json** 读 ----
// 这里刻意不写死任何端点/模型名: 写死的默认值换机器后只会变成"连着一个不存在的地址,
// 报错还看不出是配置问题"。文件长这样(也接受平铺写法, 即 key 直接放顶层):
//     { "llm": { "url": "http://主机:端口/v1/chat/completions", "name": "模型名", "token": "" } }
// 取值规则与 agent_cli 的 config.json 一致: 先看 llm.<key>, 没有再回落顶层 <key>。
// 优先级: 命令行 > test_llm.json。
static std::string llmConfigPath()
{
    JZDir dir(JZPlatform::applicationDirPath());     // 构造即把 '\' 归一成 '/'
    return dir.path() + "/test_llm.json";
}

static void loadLlmConfigFile(JZCliConfig &cfg, const std::string &path, bool &found)
{
    found = false;
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return;                                      // 没有这个文件: 交给上层明确报错

    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        text.erase(0, 3);                            // 去掉 UTF-8 BOM (记事本另存常见)

    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(text, root) || !root.isObject())
    {
        jzWarning().nospace() << "[agent_cli_test] test_llm.json 解析失败, 按未配置处理: " << path;
        return;
    }
    found = true;

    const Json::Value &g = (root["llm"].isObject() ? root["llm"] : root);
    // "分组优先, 回落顶层" —— 与 JZAgentCli 读 config.json 的 findConfigMember 同一口味
    auto str = [&g, &root](const char *key) -> std::string {
        if (g[key].isString())
            return g[key].asString();
        if (root[key].isString())
            return root[key].asString();
        return std::string();
    };
    auto num = [&g, &root](const char *key, int def) -> int {
        if (g[key].isInt())
            return g[key].asInt();
        if (root[key].isInt())
            return root[key].asInt();
        return def;
    };

    const std::string url = str("url");
    const std::string name = str("name");
    const std::string token = str("token");
    if (!url.empty())   cfg.modelUrl = url;
    if (!name.empty())  cfg.modelName = name;
    if (!token.empty()) cfg.modelToken = token;
    cfg.netTimeout = num("netTimeoutMs", cfg.netTimeout);
    cfg.maxRounds = num("maxRounds", cfg.maxRounds);
}

// ⚠ 必须"填调用方的对象"(传引用进来), 不能自己建局部 cfg 再按值返回:
//   parser.addOption 存的是**指针**, 局部对象在函数返回后已析构, 调用方 parse() 时
//   会写进已经销毁的内存 —— 传 --modelUrl/--modelName 就 0xC0000005(实测)。
//   所以这个对象要活到 parse() 之后。
static void loadCliConfig(JZCommandLineParser &parser, JZCliConfig &cfg)
{
    cfg.maxRounds = 100;

    // ① 底线: exe 同目录的 test_llm.json (缺失/缺 url+name 时由 main 里明确报错, 不猜端点)
    bool haveJson = false;
    loadLlmConfigFile(cfg, llmConfigPath(), haveJson);
    if (haveJson)
        jzInfo().nospace() << "[agent_cli_test] 模型配置: " << llmConfigPath()
                           << " (" << (cfg.modelName.empty() ? "?" : cfg.modelName.c_str()) << ")";

    // ② 命令行最后落 (临时试另一台机器用, 不必改文件; 优先级最高)
    parser.addOption("--modelUrl", &cfg.modelUrl);
    parser.addOption("--modelName", &cfg.modelName);
    parser.addOption("--modelToken", &cfg.modelToken);
}

} // namespace

int main(int argc, char *argv[])
{
    JZApplication app;

    std::string dataDir = JZPlatform::applicationDirPath() + "/data/agent_cli_test";
    for (size_t i = 0; i < dataDir.size(); i++)
        if (dataDir[i] == '\\')
            dataDir[i] = '/';
    std::string casesFilter;
    int httpPort = 18090;
    int cliHttpPort = 19090;
    bool noColor = false;

    JZCommandLineParser parser;
    parser.setDescription("agent_cli 用例评测(复用 code_agent/benchmark 用例)");
    JZCliConfig cfg;                 // 生命周期必须覆盖 parse(): parser 里存的是它的成员地址
    loadCliConfig(parser, cfg);
    parser.addOption("--data", &dataDir);
    parser.addOption("--cases", &casesFilter);
    parser.addOption("--http-port", &httpPort);
    parser.addOption("--cli-http-port", &cliHttpPort);
    parser.addOption("--no-color", &noColor);
    if (!parser.parse(argc, argv))
    {
        jzError() << parser.errorString().c_str();
        return 2;
    }

    g_colorEnabled = !noColor && enableConsoleColor();

    if (cfg.modelUrl.empty() || cfg.modelName.empty())
    {
        jzError().nospace()
            << "未配置模型。请在 " << llmConfigPath()
            << " 里写 {\"llm\":{\"url\":\"http://主机:端口/v1/chat/completions\",\"name\":\"模型名\"}}"
               " (也可用 --modelUrl/--modelName 临时覆盖)";
        return 2;
    }

    // 程序映射: 本程序(外部)自己定位 tsc / code_runtime, 填进配置传给 JZAgentCli ——
    // agent 侧只认配置, 不做探测。
    std::string tscPath = detectProgram("ts_compiler/tsc/tsc");
    std::string runtimePath = detectProgram("code_runtime");
    // ⚠ tsc 找不到就**报错退出**(2026-10-14): 以前是"探不到就不填映射", 结果是模型在终端里敲
    //   `tsc` 变成 command not found(实测在一个用例里连试 18+ 次, 最后撞轮次上限), 用例白跑。
    //   tsc 路径不对要在**开跑之前**炸掉。
    //   (随包 tsc 的期望位置: <build>/binary/bin/<配置>/ts_compiler/tsc/tsc.exe)
    if (tscPath.empty())
    {
        jzError().nospace()
            << "找不到随包 tsc（找过 exe 同目录与 <build>/binary/bin/<配置>/ 里的 "
               "ts_compiler/tsc/tsc.exe）。tsc 路径不对就报错: 请先构建/铺好 ts_compiler。";
        return 2;
    }
    cfg.programPaths["tsc"] = tscPath;
    if (!runtimePath.empty())
        cfg.programPaths["code_runtime"] = runtimePath;
    else
        jzWarning().nospace() << "找不到 code_runtime（<build>/binary/bin/<配置>/）："
                                 "模型跑程序与 ${runtime} 型验证都会失效。";

    // code_runtime 路径: 仅用于展开 verify 命令里的 ${runtime}(server 型用例); 探测不到就留空。
    std::string codeRuntime = runtimePath;
    if (!JZFileInfo::exists(codeRuntime))
        codeRuntime.clear();

    // code_agent 的部署目录(bin/<CONFIG>): code_runtime / tsc / js_wrapper 都在这儿。
    // 工程骨架的 js_wrapper 从这里拷, 所以要求能定位到。
    std::string codeBinDir;
    if (!codeRuntime.empty())
        codeBinDir = parentDirOf(codeRuntime);

    if (!cfg.programPaths.empty())
    {
        std::string line;
        for (const auto &kv : cfg.programPaths)
        {
            if (!line.empty())
                line += ", ";
            line += kv.first + "=" + kv.second;
        }
    }
    else
    {
        jzWarning() << "[agent_cli_test] 未探测到 tsc / code_runtime, agent 可能编不出产物";
    }

    std::vector<BenchmarkCase> cases;
    std::string loadErr;
    if (!loadCases(dataDir, cases, loadErr))
    {
        jzError().nospace() << "加载评测集失败: " << loadErr;
        return 2;
    }

    // 按 --cases 过滤（逗号分隔，可多个；忽略前后空白；支持通配符 * ? .）。
    // 例如：jsapi.* 跑全部 jsapi 用例；algo_* 跑全部算法用例；net_http 仅跑单个精确名。
    if (!casesFilter.empty())
    {
        std::vector<std::string> want;
        std::stringstream cs(casesFilter);
        std::string item;
        while (std::getline(cs, item, ','))
        {
            size_t a = item.find_first_not_of(" \t\r\n");
            if (a == std::string::npos)
                continue;
            size_t b = item.find_last_not_of(" \t\r\n");
            want.push_back(item.substr(a, b - a + 1));
        }
        std::vector<BenchmarkCase> filtered;
        for (auto &c : cases)
        {
            bool hit = false;
            for (const auto &w : want)
            {
                if (wildcardMatch(w, c.name) || w == c.name)
                {
                    hit = true;
                    break;
                }
            }
            if (hit)
                filtered.push_back(std::move(c));
        }
        cases = std::move(filtered);
        if (cases.empty())
        {
            jzError().nospace() << "指定的用例都不存在(或通配符无匹配): " << casesFilter;
            return 2;
        }
    }

    // 需要"通用 stdout 验证"的用例(expect.contains 且没给 verify)必须有 code_runtime + tsc:
    // 验证脚本要 tsc 先编译、code_runtime 再跑产物, 两者都从 build 目录定位(见 detectProgram)。
    // 找不到就直接失败(与 benchmark 一致) —— 否则这些用例会一路"无验证"跑成假 PASS。
    {
        bool needRuntime = false;
        for (const auto &c : cases)
            if (c.verifyCmd.empty() && (!c.expectContains.empty() || !c.expectEquals.empty()))
                needRuntime = true;
        if (needRuntime && (codeRuntime.empty() || tscPath.empty()))
        {
            jzError().nospace() << "未在 build 目录里找到 "
                                << (codeRuntime.empty() ? "code_runtime " : "")
                                << (tscPath.empty() ? "tsc " : "")
                                << "(请先构建对应目标), 无法对 expect.contains 型用例做 stdout 验证";
            return 2;
        }
        // 验证脚本是随评测集(data/agent_cli_test)一起拷过来的: 老构建产物里可能还没有它
        // (POST_BUILD 的 copy_directory 只在重新构建 agent_cli_test 时跑), 提示重新构建。
        if (needRuntime && !JZFileInfo::exists(dataDir + "/stdout_verify.py"))
        {
            jzError().nospace() << "评测集目录里没有 stdout_verify.py: " << dataDir
                                << "(请重新构建 agent_cli_test 以同步评测集)";
            return 2;
        }
    }

    JZEventLoop loop;
    CliTest test(&loop, cfg, codeRuntime, codeBinDir, dataDir);
    test.setCases(cases);
    test.setCliPortBase(cliHttpPort);

    // 输入 HTTP 服务(供需要输入的用例, 如 hello_http)
    if (!test.startHttpServer(httpPort))
        return 2;

    test.run();

    int ret = loop.exec();
    // 先停 HTTP 线程(join), 再让 CliTest / loop 析构
    test.stopHttpServer();
    (void)ret;
    return 0;
}
