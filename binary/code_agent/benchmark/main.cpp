// code_agent_benchmark - 用例评测（端到端：大模型生成代码 -> 宿主 tsc 编译
//   -> code_runtime 隔离运行 -> 校验输出）。headless、无 GUI、需要大模型。
//
// 评测模型在"给定工作区"里自驱完成一个编码任务的能力：
//   1) 用一个临时目录初始化标准 TS 工程（initProject）；
//   2) 把任务描述发给 JZCodeAgent，模型自行写文件 / 编译自检；
//   3) 宿主真实 tsc 打包/发布（buildProject）-> code_runtime 子进程跑产物；
//   4) 按用例的 check() 判定输出是否符合预期，不符则把实际输出反馈给模型重试，
//      最多 maxAttempts 次；任一尝试通过即记 1 分。
//
// 评测集从 data/ 加载（见 loadCases）：cases.jsonl 是一个"用例索引"文件，每行指向一个用例 JSON 文件的位置
//   （路径相对索引所在目录；支持 {"file":"x.json"} 对象写法，也支持纯字符串 "x.json"，并可带 "enabled":false 跳过）。
//   main 读取索引后，逐个加载被引用的用例 JSON 并执行；索引里缺失/解析失败的引用会被跳过并告警。
//   目录模式（--data 指向目录且无 cases.jsonl）则直接扫描目录内 *.json 当作用例。
// 第一个移植过去的用例 hello_world：要求模型"写一个函数，输出 hello world"，编译运行后检查 stdout 是否包含
//   "hello world"（大小写不敏感）。外部验证用例用 verify 命令（详见 BenchmarkCase）。
//
// 配置（命令行 / 环境变量）：
//   --modelUrl / --modelName / --modelToken   大模型接入（缺省沿用仓库测试默认值）
//   --tsc   宿主侧 tsc 全路径（也可走环境变量 JZ_TS_LSP；缺失则无法编译，退出 2）
//   --tsgo  原生 tsgo(tsc) LSP 全路径（缺省同 --tsc，用于 TS 工具；可空）
//   --no-color   关闭彩色输出（默认在支持 ANSI 的终端上自动开启）
//
// 退出码：0=全部用例通过；1=有失败；2=前置条件缺失（编译器 / 工程初始化失败）。
//
// 注意：本文件按需声明 kernel32 API，不 include <windows.h>（避免拉进整套 win32 头）。

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
#include "JZElapsedTimer.h"
#include "JZCommandLineParser.h"
#include "JZDebug.h"
#include "JZDir.h"
#include "JZEventLoop.h"
#include "JZFileInfo.h"
#include "JZHttpServer.h"
#include "JZPlatform.h"
#include "JZProcess.h"
#include "JZTimer.h"
#include "JZUuid.h"
#include "JZStd.h"

#include "JZCodeAgent.h"

// ---------------------------------------------------------------------------
// 控制台 ANSI 颜色
//   Windows 10+ 的 cmd / PowerShell / Windows Terminal 本身支持 ANSI 转义序列，
//   但默认是关闭的，必须对 stdout 打开 ENABLE_VIRTUAL_TERMINAL_PROCESSING 才会
//   解析 "\033[..m"（否则会原样打印出乱码）。本文件不 include <windows.h>，
//   只按需声明要用的几个 kernel32 API，不引入整套 win32 头。
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
static std::string detectPythonInterpreter();

const unsigned long kStdOutputHandle = (unsigned long)-11;
const unsigned long kStdErrorHandle  = (unsigned long)-12;
const unsigned long kVirtualTerminal = 0x0004;   // ENABLE_VIRTUAL_TERMINAL_PROCESSING

bool g_colorEnabled = false;   // 是否真的可以写 ANSI 颜色

// 打开 stdout（顺带 stderr）的虚拟终端模式；输出被重定向到文件/管道时返回 false
// （此时 GetConsoleMode 会失败，说明不是控制台，不该写颜色）
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

    void *err = GetStdHandle(kStdErrorHandle);
    unsigned long emode = 0;
    if (err && GetConsoleMode(err, &emode))
        SetConsoleMode(err, emode | kVirtualTerminal);
    return true;
#else
    return true;   // 类 Unix 终端默认支持 ANSI
#endif
}

// ANSI 颜色常量；未开启颜色时 clr() 返回空串，输出保持纯文本
const char *clr(const char *ansi) { return g_colorEnabled ? ansi : ""; }

const char *CLR_RESET  = "\033[0m";
const char *CLR_BOLD   = "\033[1m";
const char *CLR_GRAY   = "\033[90m";   // 工具参数
const char *CLR_CYAN   = "\033[36m";   // 工具名
const char *CLR_GREEN  = "\033[32m";   // 成功
const char *CLR_RED    = "\033[31m";   // 失败
const char *CLR_YELLOW = "\033[33m";   // 用例标题

// 毫秒 -> 秒文本, 保留 1 位小数。
// ⚠ 必须返回字符串: jzDebug 打 double 走 %f(6 位小数), 直接给 double 会打成 51.738000s。
static std::string sec1(int64_t ms)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f", ms / 1000.0);
    return std::string(buf);
}

// ---- 用例定义 ----
struct BenchmarkCase
{
    std::string name;            // 用例名（同时用作临时工程目录名）
    std::string prompt;          // 初始任务描述
    int maxAttempts = 3;         // 最多尝试次数（含首次）
    int timeoutMs = 30000;       // 单轮运行超时（毫秒，缺省 30s）

    // 判定：运行得到的全部输出（合并 console.* / 运行时日志）是否满足预期
    std::function<bool(const std::string &output)> check;

    // 重试时给模型的反馈（基于上一次实际输出 / 编译错误构造）
    std::function<std::string(const std::string &output, const std::string &buildError)> retry;

    // 注入给本用例的输入（空串表示无需输入）；运行时由全局 HTTP 服务以 /input 暴露给 JS
    std::string input;

    // 外部程序验证（非空时启用）：由本命令启动一个验证程序对模型产物做判定。
    // 命令模板支持占位符：${root}=用例临时根目录 ${build}=构建目录 ${runtime}=code_runtime 路径 ${port}=verifyPort ${data}=评测集目录(随用例发布校验脚本处)
    // 约定（验证程序退出码，只需区分两种）：
    //   0 = 通过；1 = 未通过（验证程序 stdout 将作为重试反馈输入）。
    //   验证程序只用 0/1：不自行区分"崩溃"。"模型生成的产物(JS 进程)本身异常退出/无法启动"
    //   由 code_runtime 子进程直接判定（见 runCodeRuntime）：其输出原样作为反馈，并在报告里标 [CRASH]。
    // 注：验证程序自行负责启动模型生成的 server 并测试（如 python verify.py --runtime ${runtime} --build ${build} --port ${port}）。
    std::string verifyCmd;
    int verifyPort = 3900;

    // setup 准备脚本: 相对『用例 json 所在目录』的 .py 路径; initProject 之后、agent 开始前
    // 由宿主同步执行, 传入 --root <工程根> --input <输入> --data <评测集目录>; 非零退出判 setup 失败。
    std::string setup;
    std::string sourceDir;     // 用例 json 所在目录(setup 相对路径在此解析)

    // daemon 后台服务(可选): 与 setup 的区别是生命周期 —— setup 是一次性脚本(跑完就退出),
    // daemon 是整个用例期间常驻的辅助进程, 典型场景是被测产物要连的"另一侧"：
    //   模拟下位机(Modbus TCP 从站) / 对端的 MES 服务端 / 持续吐数据的数据源。
    // 命令模板支持与 verify 相同的占位符: ${root} ${build} ${runtime} ${port} ${bench} ${data}
    //   （端口等额外参数由各脚本自己约定，宿主不强加 --port）。
    // 相对路径同 setup: 以 sourceDir（用例 json 所在目录）解析。
    // 时机: setup 之后、agent 之前启动(异步, 不等待其退出); finishCase() 时连子孙进程一起杀掉
    //   (JZProcess 在 Windows 下建了 Job Object, 能带走 python 拉起的子进程)。
    std::string daemon;
    int daemonReadyMs = 3000;   // 启动后轮询等待其就绪的上限(毫秒); 超时不判失败(daemon 可能本就无输出)
};

// 全局输入 HTTP 服务用的 handler：模型生成的 JS 通过 fetch('http://127.0.0.1:<port>/input')
// 读取当前用例注入的输入。getInput 在独立的网络线程上被调用，返回 Benchmark::currentInput()。
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

// 工具调用轨迹（仅本地循环/重复分析用，不实时打印）：按时间顺序记录每次调用的名称、参数、成败
struct ToolCallRecord
{
    std::string name;     // 工具名
    std::string args;     // 原始参数（function_args），含路径等，用于识别重复调用
    bool ok = false;      // 工具结果到达后回填
    int64_t t = 0;        // 调用时刻(ms)

    ToolCallRecord() = default;
    ToolCallRecord(std::string n, std::string a, bool o, int64_t ti)
        : name(std::move(n)), args(std::move(a)), ok(o), t(ti) {}
};

// 分析工具调用轨迹：识别"反复重读文件 / 重复调用 / 陷入循环"
//   - distinct : 去重后的 (name+args) 签名数
//   - maxRepeat: 单个签名最多出现次数（>=3 视为疑似循环）
//   - topRepeat: 重复>=2 的签名摘要（name(args简写)xN，最多取若干）
struct ToolTraceStat
{
    int distinct = 0;
    int maxRepeat = 0;
    std::string topRepeat;
};

ToolTraceStat analyzeToolTrace(const std::vector<ToolCallRecord> &trace)
{
    ToolTraceStat st;
    std::map<std::string, int> cnt;
    for (const auto &rec : trace)
    {
        std::string sig = rec.name + "|" + rec.args;
        cnt[sig]++;
    }
    st.distinct = (int)cnt.size();
    std::vector<std::pair<std::string, int>> v(cnt.begin(), cnt.end());
    std::sort(v.begin(), v.end(), [](const std::pair<std::string, int> &a,
                                     const std::pair<std::string, int> &b) {
        return a.second > b.second;
    });
    for (const auto &kv : v)
        if (kv.second > st.maxRepeat) st.maxRepeat = kv.second;

    std::string parts;
    int shown = 0;
    for (const auto &kv : v)
    {
        if (kv.second < 2 || shown >= 5)
            break;
        size_t sep = kv.first.find('|');
        std::string name = kv.first.substr(0, sep);
        std::string args = kv.first.substr(sep + 1);
        std::string a;
        for (char c : args)
        {
            if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            a.push_back(c);
        }
        if (a.size() > 40) a = a.substr(0, 40) + "...";
        if (!parts.empty()) parts += ", ";
        parts += name + "(" + a + ")x" + std::to_string(kv.second);
        shown++;
    }
    st.topRepeat = parts;
    return st;
}

// ---- 单用例结果 ----
struct CaseResult
{
    std::string name;
    bool passed = false;
    bool crashed = false;       // 模型生成的产物本身异常退出/无法启动（优先在报告中标出）
    int attempts = 0;
    int promptTokens = 0;       // 本用例消耗的 prompt token
    int completionTokens = 0;   // 本用例消耗的 completion token
    int totalTokens = 0;        // 本用例消耗的总 token
    int cachedTokens = 0;       // 本用例从 KV 缓存命中的输入 token（前缀缓存命中）
    int llmMs = 0;              // 本用例 LLM 生成耗时（含工具自驱循环，跨重试累计）
    // llama.cpp 计时(其他 provider 恒为 0)，速率按 token/ms 现算
    double promptMs = 0;        // 本用例累计 prefill 耗时
    double predMs = 0;          // 本用例累计生成耗时
    int draftN = 0;             // 本用例累计 MTP/投机采样草稿数
    int draftAccepted = 0;      // 本用例累计 MTP/投机采样被接受数
    int toolCalls = 0;         // 本用例工具调用总次数（含成功/失败）
    int toolFails = 0;         // 本用例工具调用失败次数
    std::map<std::string, int> toolCount;      // 本用例各工具调用次数（按 function_name）
    std::map<std::string, int> toolFailCount;  // 本用例各工具失败次数（按 function_name）
    std::vector<ToolCallRecord> toolTrace;      // 本用例工具调用轨迹（按时间序，仅做循环/重复分析，不打印）
    int buildMs = 0;            // 本用例宿主编译耗时
    int runMs = 0;              // 本用例 code_runtime 运行耗时
    int contextTokens = 0;      // 最后一轮的上下文占用(JZAgent::windowTokens())：与 totalTokens 不同口径
    std::string lastOutput;
    std::string lastBuildError;
    std::string lastAgentError;
};

enum class BenchState
{
    Idle,
    WaitingAgent,
    Done
};

class HttpThread : public JZThread
{
public:
    JZHttpServer *server;

    virtual void run() override
    {
        exec();
        server->moveToThread(jzApp->mainThread());
    }
};

class Benchmark : public JZObject
{
public:
    Benchmark(JZEventLoop *loop,
              const JZAgentConfig &llm,
              const CodeAgentConfig &code,
              const std::string &codeRuntime)
        : m_loop(loop), m_llm(llm), m_codeConfig(code), m_codeRuntime(codeRuntime)
    {   
        m_agent = nullptr;
        m_codeConfig.llmConfig = m_llm;
        m_poll = new JZTimer(this);
        connect(&m_poll->sigTimeout, this, &Benchmark::onPoll);
    }

    ~Benchmark()
    {
        stopDaemon();          // 兜底: 异常退出时也别把 python 后台进程留在端口上
        m_httpThread.quit();
        m_httpThread.wait();
    }

    void setCases(const std::vector<BenchmarkCase> &cases) { m_cases = cases; }
    void setDataDir(const std::string &dir) { m_dataDir = dir; }
    
    bool startHttpServer(int p)
    {
        m_http = new JZHttpServer(this);
        m_httpHandler = new InputHttpHandler();
        m_httpHandler->getInput = [this]() { return currentInput(); };
        m_http->addRoute("/input", m_httpHandler);
        if (!m_http->listen(m_httpPort))
        {
            jzError().nospace() << "无法在端口 " << m_httpPort << " 启动输入 HTTP 服务";
            return false;
        }
        printf("??\n");
        jzDebug() << "??";
        jzDebug().nospace() << "[benchmark] 输入 HTTP 服务已启动: http://127.0.0.1:"
            << m_httpPort << "/input";

        // 告知模型从本服务读取输入（端口此时已确定）；只影响 benchmark 的 agent 实例
        std::string extra =
            "\n\n## 读取输入\n"
            "如果任务需要从输入读取数据，用以下方式（异步 fetch，顶层 await）：\n"
            "  const res = await fetch('http://127.0.0.1:" + std::to_string(m_httpPort) + "/input');\n"
            "  const input = await res.text();\n"
            "input 即评测注入的用例输入；无需输入的题目不要读取。";
        
        // setWorkspace 会重建 agent 并把 system prompt 重置为 buildSystemPrompt()，
        // 这里立即追加会在首个用例开始前被覆盖，所以只在成员里保存，稍后于 startCase 重建后注入。
        m_httpPromptExtra = extra;

        m_http->moveToThread(&m_httpThread);
        m_httpThread.server = m_http;
        m_httpThread.start();
        return true;
    }

    void run()
    {
        if (m_cases.empty())
        {
            jzError() << "没有可运行的用例";
            m_loop->quit();
            return;
        }
        m_benchTimer.start();   // 总耗时计时起点（覆盖全部用例）
        m_idx = 0;
        startCase(0);
    }

protected:
    void startCase(size_t i)
    {
        m_case = m_cases[i];
        setInput(m_case.input);   // 注入本用例输入，供全局 HTTP 服务的 /input 暴露给 JS
        m_result = CaseResult{};
        m_result.name = m_case.name;
        m_attempt = 0;
        m_state = BenchState::Idle;

        // 每个用例一个独立临时工程，避免互相污染
        std::string root = JZPlatform::applicationTmpPath() + "/" + JZUuid::createUuid().toString() +"/bench_" + m_case.name;

        m_agent = new JZCodeAgent();
        m_agent->init(m_codeConfig);

        std::string err;
        if (!m_agent->initProject(root, err))
        {
            jzError().nospace() << "[用例 " << m_case.name << "] 初始化工程失败: " << err;
            m_result.lastAgentError = err;
            finishCase();
            return;
        }
        if (!m_agent->setWorkspace(root))
        {
            jzError().nospace() << "[用例 " << m_case.name << "] 设置工作区失败: " << root;
            m_result.lastAgentError = "setWorkspace failed: " + root;
            finishCase();
            return;
        }
        m_root = root;

        // setup 脚本: initProject 之后、agent 开始前同步执行的 python 准备脚本(如把输入写成文件);
        // 非零退出视为该用例 setup 失败(与 initProject 失败同样处理)。
        if (!m_case.setup.empty())
        {
            std::string perr;
            if (!runSetupScript(m_case, m_root, m_dataDir, perr))
            {
                jzError().nospace() << "[用例 " << m_case.name << "] setup 脚本失败: " << perr;
                m_result.lastAgentError = "setup: " + perr;
                finishCase();
                return;
            }
        }

        // daemon 后台服务: setup 之后拉起, 整个用例期间常驻(供产物连接, 如模拟下位机/MES),
        // 直到 finishCase() 才连同子孙进程杀掉。启动即退出(端口被占等)视为本用例失败。
        if (!m_case.daemon.empty())
        {
            std::string derr;
            if (!startDaemon(m_case, m_root, derr))
            {
                jzError().nospace() << "[用例 " << m_case.name << "] daemon 启动失败: " << derr;
                m_result.lastAgentError = "daemon: " + derr;
                finishCase();
                return;
            }
        }

        if (i == 0)
        {
            jzDebug().noquote().nospace() << "系统提示词:\n" << m_agent->agent()->dumpSystemPrompt() << "\n";
            jzDebug().noquote().nospace() << "工具:\n" << m_agent->agent()->dumpToolsPrompt() << "\n";
        }

        // setWorkspace 内部会 buildAgent 重建 agent，把 startHttpServer 追加的"读取输入"约定冲掉，
        // 因此重建之后再注入一次，确保模型知道从 http://127.0.0.1:<port>/input 读取输入。
        auto jz_agent = m_agent->agent();
        // setup 脚本已把输入写成文件的用例, 不再注入 fetch 提示(避免与"读本地文件"冲突)
        if (!m_httpPromptExtra.empty() && m_case.setup.empty())
        {
            jz_agent->setSystemPrompt(jz_agent->systemPrompt() + m_httpPromptExtra);
        }
        // 工具调用统计：不实时打印（避免刷屏），按用例累计次数 / 失败次数 / 各工具分布；
        // 同时记录调用轨迹（名称+参数+成败+时刻），供末尾分析"反复重读/重复调用/循环"。
        m_toolNameById.clear();
        m_toolCallIdxById.clear();
        connect(&jz_agent->sigToolCall, this, [this](const ToolCall& t) {
            jzDebug() << "tool call:" << t.function_name.c_str();

            m_result.toolCalls++;
            m_result.toolCount[t.function_name]++;
            std::string args = t.function_args;
            if (args.empty() && !t.params.isNull())
                args = t.params.toStyledString();   // 少数模型只填了解析后的 params
            int idx = (int)m_result.toolTrace.size();
            m_result.toolTrace.push_back({t.function_name, args, false,
                                          JZPlatform::getTickCountMs()});
            m_toolNameById[t.id] = t.function_name;     // 记录 id->名称，供结果失败归因
            m_toolCallIdxById[t.id] = idx;              // 记录 id->轨迹下标，供结果回填 ok
        });
        connect(&jz_agent->sigToolResult, this, [this](const ToolCallResult& r) {
            jzDebug() << "tool result:" << r.isOk << "," << str_left(r.result,200);

            if (!r.isOk)
            {
                m_result.toolFails++;
                auto it = m_toolNameById.find(r.id);
                if (it != m_toolNameById.end())
                    m_result.toolFailCount[it->second]++;
            }
            auto it2 = m_toolCallIdxById.find(r.id);
            if (it2 != m_toolCallIdxById.end())
                m_result.toolTrace[it2->second].ok = r.isOk;
        });

        jzDebug().nospace() << clr(CLR_YELLOW) << "==== 用例[" << m_case.name << "] 开始 (maxAttempts="
                            << m_case.maxAttempts << ", root=" << root << ") ====" << clr(CLR_RESET);
        doAttempt(m_case.prompt);
    }

    void doAttempt(const std::string &text)
    {
        m_attemptLogBase = m_agent->logSeq();   // 本轮起点，用于识别本轮内 agent 是否报错
        m_llmStartMs = JZPlatform::getTickCountMs();  // LLM 耗时计时起点
        m_state = BenchState::WaitingAgent;
        m_agent->request(text);
        m_poll->start(200);
    }

    void onPoll()
    {
        int seq = m_agent->logSeq();
        if (seq > m_renderedId)
        {
            const auto& logs = m_agent->logs();
            // 注意：m_agent 会把超出上限(m_logs>3000)的旧日志从头部裁掉，
            // 而 logSeq() 返回的是全局单调递增的 id，二者下标并不一致。
            // 必须按 log.id 迭代，绝不能拿全局序号当 vector 下标访问 logs[]。
            for (const auto& log : logs)
            {
                if (log.id <= m_renderedId)
                    continue;   // 已渲染过
                if (log.id > seq)
                    break;      // logs 按 id 递增，后面都是更新的，直接停
                
                std::string text = str_utf8_to_local(log.text);
                if (m_renderedType != log.type)
                {
                    if (log.type == "user" || log.type == "thinking" || log.type == "delta")
                    {
                        if(!m_renderedType.empty())
                            printf("%s: ", str_utf8_to_local(log.type).c_str());
                    }
                    m_renderedType = log.type;
                }
                if (log.type == "user" || log.type == "thinking" || log.type == "delta")
                    printf("%s", str_utf8_to_local(log.text).c_str());
            }
            m_renderedId = seq;
        }

        if (m_state != BenchState::WaitingAgent)
            return;
        // 任务（含宿主自动续修轮）结束的标志：busy 落回 false
        if (m_agent->isBusy())
            return;

        m_poll->stop();
        m_state = BenchState::Idle;
        afterAgent();
    }

    void afterAgent()
    {
        m_attempt++;
        m_result.attempts = m_attempt;

        // 本轮 LLM（含工具自驱循环）耗时：从 doAttempt 发请求到此刻 agent 落闲
        m_result.llmMs += (int)(JZPlatform::getTickCountMs() - m_llmStartMs);

        // agent 自身报错（LLM/网络等基础设施错误，与模型能力无关，重试无意义）-> 直接判定失败，不重试
        {
            const auto& logs = m_agent->logs();
            for (const auto& l : logs)
            {
                if (l.id > m_attemptLogBase && l.type == "error")
                {
                    m_result.lastAgentError = l.text;
                    break;
                }
            }
        }
        if (!m_result.lastAgentError.empty())
        {
            m_result.passed = false;
            jzDebug().nospace() << "  [尝试 " << m_attempt << "] agent 报错，直接判定失败(不重试): "
                                << m_result.lastAgentError;
            finishCase();
            return;
        }

        // 1) 宿主真实编译打包（异步启动 + 阻塞等待收尾）
        std::string buildErr;
        int64_t b0 = JZPlatform::getTickCountMs();
        bool buildOk = m_agent->buildProject(buildErr);
        if (buildOk)
        {
            m_agent->waitBuildFinished();
            buildOk = m_agent->lastBuildOk();
            buildErr = m_agent->lastBuildError();
        }
        m_result.buildMs += (int)(JZPlatform::getTickCountMs() - b0);
        m_result.lastBuildError = buildErr;

        // 2) 判定：普通用例跑 code_runtime + stdout 校验；外部验证用例跑 verify 命令
        std::string output;
        bool runOk = false;
        std::string verifyOut;
        int verifyExit = -1;
        bool crashed = false;     // 模型产物本身异常退出/无法启动（优先标 [CRASH]）
        bool ok = false;

        int64_t r0 = JZPlatform::getTickCountMs();
        if (!m_case.verifyCmd.empty())
        {
            // ---- 外部程序验证：由验证程序自行启动模型 server 并测试 ----
            if (buildOk)
            {
                std::string cmd = expandVerifyCmd(m_case.verifyCmd);
                bool ran = runVerify(cmd, verifyExit, verifyOut);
                m_result.runMs += (int)(JZPlatform::getTickCountMs() - r0);
                m_result.lastOutput = verifyOut;
                if (!ran)
                {
                    ok = false;     // 验证程序都没跑起来（基础设施问题）
                }
                else if (verifyExit == 0)
                {
                    ok = true;
                }
                else
                {
                    ok = false;      // exit 1：未通过，验证程序 stdout 留作反馈（崩溃判定在 code_runtime JS 进程层）
                }
            }
            else
            {
                m_result.runMs += (int)(JZPlatform::getTickCountMs() - r0);
                m_result.lastOutput = "";
            }
        }
        else
        {
            // ---- 普通用例：隔离运行产物（code_runtime 子进程），捕获 stdout ----
            if (buildOk)
                runOk = runCodeRuntime(output);
            m_result.runMs += (int)(JZPlatform::getTickCountMs() - r0);
            m_result.lastOutput = output;
            if (buildOk && !runOk)
                crashed = true;   // 产物运行期崩溃（exit != 0）
            ok = buildOk && runOk && m_case.check(output);
        }

        jzDebug().nospace() << "  [尝试 " << m_attempt << "] build="
                            << clr(buildOk ? CLR_GREEN : CLR_RED) << (buildOk ? "OK" : "FAIL") << clr(CLR_RESET)
                            << " run=" << clr(runOk ? CLR_GREEN : CLR_RED) << (runOk ? "OK" : "FAIL") << clr(CLR_RESET)
                            << (m_case.verifyCmd.empty() ? "" : " verifyExit=" + std::to_string(verifyExit))
                            << (crashed ? " [CRASH]" : "")
                            << " output=\"" << m_result.lastOutput.substr(0, 200) << "\"";
        if (!buildOk)
            jzDebug().nospace() << "  [尝试 " << m_attempt << "] buildError: "
                                << buildErr.substr(0, 400);

        if (ok)
        {
            m_result.passed = true;
            finishCase();
            return;
        }

        // 3) 还有次数就反馈重试，否则判定失败
        if (m_attempt < m_case.maxAttempts)
        {
            std::string fb;
            if (crashed)
            {
                // JS 进程自身异常退出/无法启动：只返回 JS 进程自身的输出作为反馈（不附加额外说明）
                fb = m_result.lastOutput;
            }
            else if (!m_case.verifyCmd.empty())
            {
                // 约定：验证程序返回 1 -> 其 stdout 作为重试输入反馈
                fb = verifyOut;
            }
            else
            {
                fb = m_case.retry(output, buildErr);
            }
            jzDebug().nospace() << "  [尝试 " << m_attempt << "] 未通过，反馈重试: " << fb;
            doAttempt(fb);
        }
        else
        {
            m_result.passed = false;
            m_result.crashed = crashed;
            finishCase();
        }
    }

    // daemon 后台服务: setup 之后、agent 之前异步拉起, 整个用例期间常驻, finishCase() 时杀掉。
    // 与 runSetupScript 的关键差别是"不等待它退出" —— daemon 本来就该一直跑着,
    // 所以我们只做两件事: ①确认它没在启动瞬间就挂掉(端口被占 → 退出码非 0 → 判本用例失败);
    // ②轮询它的 stdout, 有输出即视为就绪(脚本惯用做法: listen 完成后打印一行 ready 并 flush)。
    // 超时不判失败: 有些 daemon 本来就不输出, 且真实使用发生在几十秒后的运行/验证阶段。
    bool startDaemon(const BenchmarkCase &c, const std::string &root, std::string &err)
    {
        std::string cmd = expandVerifyCmd(c.daemon);
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
                break;                                   // 有输出 -> 认为就绪
            if (m_daemon->state() == JZProcess::NotRunning)
                break;                                   // 进程已退出, 不再等
        }

        // 启动后立刻就退出的(端口被占 / 脚本报错)才算失败, 其输出作为失败原因回给报告
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
        jzDebug().nospace() << clr(CLR_GRAY) << "[benchmark] daemon 已启动: " << cmd << clr(CLR_RESET);
        return true;
    }

    // 收掉本用例的 daemon(连子孙进程一起): 不先读残留输出的话管道可能留脏数据, 读了丢掉即可
    void stopDaemon()
    {
        if (!m_daemon)
            return;
        m_daemon->killProcess();
        m_daemon->readAllStandardOutput();
        delete m_daemon;
        m_daemon = nullptr;
    }

    // 把 verify 命令模板里的占位符替换为实际路径/端口
    std::string expandVerifyCmd(const std::string &tpl)
    {
        std::string s = tpl;
        str_replace(s, "${root}", m_root);
        str_replace(s, "${build}", m_root + "/build");
        str_replace(s, "${runtime}", m_codeRuntime);
        str_replace(s, "${port}", std::to_string(m_case.verifyPort));
        str_replace(s, "${bench}", JZPlatform::applicationDirPath());
        str_replace(s, "${data}", m_dataDir);   // 评测集（cases.jsonl 所在）目录，用于定位随用例发布的验证脚本
        return s;
    }

    // 把 detectPythonInterpreter 返回的解释器串拆成 (程序, 前置参数) 并去掉外层引号,
    // 以便直接用 JZProcess 拉起(不再经 cmd.exe /c, 避免 Windows 多层引号把路径拆坏)。
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

    // setup 脚本: initProject 之后、agent 开始前由宿主同步执行。解释器用 detectPythonInterpreter
    // (python3/python/py); 直接拉起解释器 + 脚本 + 参数(JZProcess, 不经 cmd.exe /c, 避免 Windows
    // 多层引号把路径拆坏), 传入:
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
        // 同样的坑见 JZAgentCli::runVerifyAsync 注释。
        std::string py = detectPythonInterpreter();
        std::string program;
        std::vector<std::string> args;
        splitInterpreter(py, program, args);
        args.push_back(setupPath);
        args.push_back("--root");   args.push_back(root);
        args.push_back("--input");  args.push_back(c.input);
        args.push_back("--data");   args.push_back(dataDir);
        JZProcess proc;
        proc.setProcessChannelMode(JZProcess::MergedChannels);
        if (!proc.start(program, args))
        {
            err = "无法启动 setup 脚本: " + py + " " + setupPath;
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

    // 运行外部验证程序（Windows 下经 cmd /c 启动，命令可含空格与重定向）。
    // 返回 true 表示成功启动并收尾；exitCode/errOut 带出验证程序退出码与合并输出。
    bool runVerify(const std::string &cmd, int &exitCode, std::string &errOut)
    {
        JZProcess proc;
        proc.setProcessChannelMode(JZProcess::MergedChannels);
        if (!proc.start("cmd.exe", { "/c", cmd }))
        {
            errOut = "[benchmark] 无法启动验证命令: " + cmd;
            exitCode = -1;
            return false;
        }
        if (!proc.waitForFinished(m_case.timeoutMs))
        {
            proc.killProcess();
            errOut = "[benchmark] 验证程序超时(" + std::to_string(m_case.timeoutMs / 1000) + "s)";
            exitCode = -1;
            return false;
        }
        errOut = str_fromLocal(proc.readAllStandardOutput());
        exitCode = proc.exitCode();
        return true;
    }

    bool runCodeRuntime(std::string &output)
    {
        JZProcess proc;
        proc.setProcessChannelMode(JZProcess::MergedChannels);
        // 运行根 = 子进程工作目录（code_runtime 不再收位置参数）
        proc.setWorkingDirectory(m_root + "/build");
        if (!proc.start(m_codeRuntime, {}))
        {
            output = "[benchmark] 启动 code_runtime 失败: " + m_codeRuntime;
            return false;
        }
        if (!proc.waitForFinished(m_case.timeoutMs))
        {
            proc.killProcess();
            output = "[benchmark] code_runtime 超时(" + std::to_string(m_case.timeoutMs / 1000) + "s)";
            return false;
        }
        output = str_fromLocal(proc.readAllStandardOutput());
        return proc.exitCode() == 0;
    }

    void finishCase()
    {
        stopDaemon();       // 先收掉本用例的后台服务, 再统计/切下一个用例(避免残留占端口)

        // 及时抓取本用例累计 token：下一用例 setWorkspace 会重建底层 agent、context 清零，
        // 故必须在此处（当前用例 agent 尚在）抓取并累加到全局总量。
        const auto &u = m_agent->agent()->context()->usage;
        m_result.promptTokens = u.prompt_tokens;
        m_result.completionTokens = u.completion_tokens;
        m_result.totalTokens = u.total_tokens;
        m_result.cachedTokens = u.cached_tokens;
        m_result.promptMs = u.prompt_ms;
        m_result.predMs = u.predicted_ms;
        m_result.draftN = u.draft_tokens;
        m_result.draftAccepted = u.draft_accepted_tokens;
        // 与"计费总量"不同口径: windowTokens() = 最后一轮实际会发出的上下文
        // (锚点那条 usage.prompt_tokens, 含 system+工具+历史, 不含思考)。
        m_result.contextTokens = m_agent->agent()->windowTokens();
        if (m_result.contextTokens > m_maxContextTokens)
        {
            m_maxContextTokens = m_result.contextTokens;
            m_maxContextName = m_result.name;
        }
        m_totalPrompt     += u.prompt_tokens;
        m_totalCompletion += u.completion_tokens;
        m_totalTokens     += u.total_tokens;
        m_totalCached     += u.cached_tokens;
        m_totalPromptMs   += u.prompt_ms;
        m_totalPredMs     += u.predicted_ms;
        m_totalDraft      += u.draft_tokens;
        m_totalDraftAccepted += u.draft_accepted_tokens;
        m_totalLlmMs      += m_result.llmMs;
        m_totalBuildMs    += m_result.buildMs;
        m_totalRunMs      += m_result.runMs;
        m_totalToolCalls  += m_result.toolCalls;
        m_totalToolFails  += m_result.toolFails;
        for (const auto& kv : m_result.toolCount)
            m_toolTotalCount[kv.first] += kv.second;
        for (const auto& kv : m_result.toolFailCount)
            m_toolTotalFail[kv.first] += kv.second;

        const CaseResult &r = m_result;
        jzDebug().nospace() << "---- 用例[" << r.name.c_str() << "] 结束: "
                            << clr(r.passed ? CLR_GREEN : CLR_RED) << (r.passed ? "PASS" : "FAIL") << clr(CLR_RESET)
                            << " (attempts=" << r.attempts << ")";
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
        m_results.push_back(r);

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
        jzDebug() << "\n================ 评测结果 ================";
        for (const auto &r : m_results)
        {
            if (r.passed)
                passed++;
            ToolTraceStat ts = analyzeToolTrace(r.toolTrace);
            std::string loopNote;
            if (ts.maxRepeat >= 3)
                loopNote = std::string(" [疑似循环: ") + ts.topRepeat + "]";
            else if (!ts.topRepeat.empty())
                loopNote = std::string(" [重复: ") + ts.topRepeat + "]";
            std::string crashTag = r.crashed
                ? (std::string(clr(CLR_RED)) + "[CRASH] " + clr(CLR_RESET))
                : std::string();
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
                                << crashTag.c_str()
                                << r.name.c_str() << "  (attempts=" << r.attempts
                                << ", 计费=" << r.totalTokens
                                << " [prompt " << r.promptTokens
                                << " 其中缓存命中 " << cacheNote.c_str()
                                << " + completion " << r.completionTokens
                                << "], 上下文=" << r.contextTokens
                                << ", tool=" << r.toolCalls
                                << ", distinct=" << ts.distinct
                                << ", toolFail=" << r.toolFails
                                << ", LLM " << sec1(r.llmMs).c_str() << "s"
                                << speed.c_str() << mtp.c_str()
                                << ", 编译 " << sec1(r.buildMs).c_str() << "s"
                                << ", 运行 " << sec1(r.runMs).c_str() << "s)"
                                << loopNote.c_str();
            if (!r.passed)
            {
                if (!r.lastAgentError.empty())
                    jzDebug().nospace() << "    agentError: " << r.lastAgentError.substr(0, 400);
                if (!r.lastBuildError.empty())
                    jzDebug().nospace() << "    buildError: " << r.lastBuildError.substr(0, 300);
                jzDebug().nospace() << "    output: " << r.lastOutput.substr(0, 300);
            }
        }
        jzDebug().nospace() << "总分: " << passed << " / " << m_results.size();
        jzDebug().nospace() << "总耗时: " << (m_benchTimer.elapsed() / 1000.0) << " s";

        // ---- 崩溃用例优先汇总（模型生成的产物本身异常退出/无法启动）----
        {
            int nCrash = 0;
            for (const auto &r : m_results)
                if (!r.passed && r.crashed) nCrash++;
            if (nCrash > 0)
            {
                jzDebug().nospace() << clr(CLR_RED) << "崩溃用例(产物异常退出): " << nCrash << " 个" << clr(CLR_RESET);
                for (const auto &r : m_results)
                {
                    if (!r.passed && r.crashed)
                        jzDebug().nospace() << "  [CRASH] " << r.name.c_str() << "  output: "
                                            << r.lastOutput.substr(0, 300);
                }
            }
        }
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
        int64_t totalMs = m_benchTimer.elapsed();
        int64_t accounted = m_totalLlmMs + m_totalBuildMs + m_totalRunMs;
        int64_t otherMs = totalMs > accounted ? totalMs - accounted : 0;
        jzDebug().nospace() << "耗时分解: 总=" << sec1(totalMs).c_str() << "s"
                            << "  LLM=" << sec1(m_totalLlmMs).c_str() << "s"
                            << "  编译=" << sec1(m_totalBuildMs).c_str() << "s"
                            << "  运行=" << sec1(m_totalRunMs).c_str() << "s"
                            << "  其他=" << sec1(otherMs).c_str() << "s";

        // ---- 工具调用汇总（静默统计后的结果）----
        jzDebug().nospace() << "工具调用: 总计=" << m_totalToolCalls
                            << " (fail=" << m_totalToolFails << ")";
        if (!m_toolTotalCount.empty())
        {
            jzDebug() << "  工具分布(全局):";
            for (const auto& kv : m_toolTotalCount)
            {
                int fail = m_toolTotalFail.count(kv.first) ? m_toolTotalFail[kv.first] : 0;
                jzDebug().nospace() << "    " << kv.first << ": " << kv.second
                                    << " 次 (fail=" << fail << ")";
            }
        }

        // ---- 循环 / 重复调用检测（基于工具调用轨迹）----
        jzDebug() << "循环/重复检测:";
        {
            bool anyLoop = false;
            for (const auto &r : m_results)
            {
                ToolTraceStat ts = analyzeToolTrace(r.toolTrace);
                if (ts.maxRepeat >= 2)
                {
                    anyLoop = true;
                    jzDebug().nospace() << "  " << r.name
                        << (ts.maxRepeat >= 3 ? " [疑似循环]" : " [重复]")
                        << " distinct=" << ts.distinct
                        << " maxRepeat=" << ts.maxRepeat
                        << " : " << ts.topRepeat;
                }
            }
            if (!anyLoop)
                jzDebug() << "  无显著重复调用";
        }

        jzDebug() << "==========================================";

        m_loop->quit();
    }


    // 当前用例输入（网络线程读，加锁）
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
    JZAgentConfig m_llm;
    CodeAgentConfig m_codeConfig;
    std::string m_codeRuntime;

    JZCodeAgent *m_agent = nullptr;
    JZTimer *m_poll = nullptr;

    std::vector<BenchmarkCase> m_cases;
    std::string m_dataDir;        // 评测集（cases.jsonl 所在）目录，供 ${data} 占位符定位随用例发布的验证脚本
    size_t m_idx = 0;
    BenchmarkCase m_case;
    CaseResult m_result;
    int m_attempt = 0;
    int m_attemptLogBase = 0;   // 本轮尝试开始时的 logSeq，用于识别本轮内 agent 是否报错
    std::string m_root;
    BenchState m_state = BenchState::Idle;

    std::vector<CaseResult> m_results;
    JZElapsedTimer m_benchTimer;   // 全流程耗时计时（run 启动，reportAndQuit 读取）
    int m_totalPrompt = 0;         // 全部用例累计 token
    int m_totalCompletion = 0;
    int m_totalTokens = 0;
    int m_totalCached = 0;       // 全部用例累计从 KV 缓存命中的输入 token
    double m_totalPromptMs = 0;  // 全部用例累计 prefill 耗时(llama.cpp)
    double m_totalPredMs = 0;    // 全部用例累计生成耗时(llama.cpp)
    int m_totalDraft = 0;        // 全部用例累计 MTP/投机采样草稿数
    int m_totalDraftAccepted = 0;// 全部用例累计 MTP/投机采样被接受数
    int m_maxContextTokens = -1; // 各用例"最后一轮上下文"的最大值(用例会话互相独立, 只报最大)
    std::string m_maxContextName;
    int64_t m_llmStartMs = 0;       // 本轮 LLM 请求发出时刻（doAttempt）
    int64_t m_totalLlmMs = 0;       // 全部用例累计 LLM 耗时
    int64_t m_totalBuildMs = 0;     // 全部用例累计编译耗时
    int64_t m_totalRunMs = 0;       // 全部用例累计运行耗时
    int m_totalToolCalls = 0;       // 全部用例累计工具调用次数
    int m_totalToolFails = 0;       // 全部用例累计工具调用失败次数
    std::map<std::string, int> m_toolTotalCount;   // 全局各工具调用次数（按 function_name）
    std::map<std::string, int> m_toolTotalFail;    // 全局各工具失败次数（按 function_name）
    std::map<std::string, std::string> m_toolNameById; // 本轮工具 id->名称，供结果失败归因
    std::map<std::string, int> m_toolCallIdxById;       // 本轮工具 id->轨迹下标，供结果回填 ok
    int m_renderedId = 0;
    std::string m_renderedType;

    // ---- 全局输入 HTTP 服务：JS 用 fetch 读取当前用例注入的输入 ----
    JZHttpServer *m_http = nullptr;
    InputHttpHandler *m_httpHandler = nullptr;
    int m_httpPort = 18080;
    std::string m_input;          // 当前用例注入的输入（由 startCase 写入）
    std::mutex m_inputMutex;
    std::string m_httpPromptExtra; // startHttpServer 构造的"读取输入"约定，重建 agent 后需重新注入   
    HttpThread m_httpThread;

    // 本用例的后台服务(daemon 字段拉起): 用例期间常驻, finishCase() 时 killProcess() 带走
    JZProcess *m_daemon = nullptr;
};

// 探测某程序是否可运行(Windows 下用 cmd /c 跑 "<name> --version" 看退出码)
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

// 选一个能用的 python 解释器(顺序: python3 / python / py)
static std::string detectPythonInterpreter()
{
    const char *cands[] = { "python3", "python", "py" };
    for (const char *c : cands)
        if (probeProgramRunnable(c))
            return std::string(c);
    return "python3";
}

// 把 src 中所有 from 替换为 to（std::string 无内置 replace-all）
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

// 简单通配符匹配（用于 --cases 过滤）：
//   *  : 匹配任意长度（含空串）的任意字符
//   ?  : 匹配任意一个字符
//   .  : 也当作“任意一个字符”匹配（这样 jsapi.* 也能匹配 jsapi_core_fs 这类带下划线的名）
//   其余字符：逐字符精确比较（区分大小写）。
// 没有通配符的 pattern 退化为精确匹配，与原有 --cases 行为兼容。
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

// 大小写不敏感地判断 output 是否包含子串
bool containsIC(const std::string &hay, const std::string &needle)
{
    std::string h = hay, n = needle;
    for (auto &c : h) c = (char)std::tolower((unsigned char)c);
    for (auto &c : n) c = (char)std::tolower((unsigned char)c);
    return h.find(n) != std::string::npos;
}

// 从评测集加载用例，支持两种形态：
//   - 单个 .jsonl 文件：每行一个 JSON 对象（用例），空行忽略；
//   - 目录：扫描 *.json（每文件一用例），若存在 cases.jsonl 也一并解析。
// 用例 JSON 约定：
//   name            : 用例名（缺省取文件名/行号）
//   prompt          : 初始任务描述（必填，缺失则跳过）
//   maxAttempts     : 最大尝试次数（缺省 3）
//   timeout         : 单轮运行超时（秒，缺省 30）；超过则判本次运行失败
//   input           : 注入给本用例的输入文本（缺省空）；运行时经全局 HTTP 服务
//                      http://127.0.0.1:<port>/input 暴露给 JS（模型用 fetch 读取）
//   expect.contains        : 判定输出需包含的子串（普通用例用）
//   expect.caseInsensitive : contains 比较是否忽略大小写（缺省 false）
//   verify          : 外部验证命令模板（非空则启用外部验证，详见 BenchmarkCase 注释）
//   verifyPort      : 外部验证用端口（缺省 3900，注入 ${port}）
//   retryHint       : 重试时给模型的提示（实际输出/编译错误/验证输出会自动追加在后）
//   setup           : 准备脚本（相对用例 json 目录），一次性执行，跑完就退出
//   daemon          : 后台服务命令模板（相对路径同 setup；占位符同 verify），整个用例期间常驻
//                     —— 用于起"被测程序的另一侧"（如 Modbus 从站 / 对端 MES 服务端），
//                     setup 之后、agent 之前拉起，用例结束（含失败）时连子孙进程一起杀掉。
//                     端口等参数由各脚本自己约定不强加（宿主只透传占位符）。
//   daemonReadyMs   : daemon 启动后轮询等待其就绪的上限毫秒（缺省 3000）；超时不判失败
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

    std::string contains;
    bool ci = false;
    if (root.isMember("expect") && root["expect"].isObject())
    {
        const Json::Value &exp = root["expect"];
        if (exp.isMember("contains") && exp["contains"].isString())
            contains = exp["contains"].asString();
        if (exp.isMember("caseInsensitive"))
            ci = exp["caseInsensitive"].asBool();
    }
    c.check = [contains, ci](const std::string &output) {
        if (contains.empty())
            return true;
        return ci ? containsIC(output, contains)
                  : output.find(contains) != std::string::npos;
    };

    std::string hint = root.isMember("retryHint") && root["retryHint"].isString()
                           ? root["retryHint"].asString()
                           : "上一次运行未通过，请根据反馈修正后重试。";
    c.retry = [hint](const std::string &output, const std::string &buildError) {
        std::ostringstream ss;
        ss << hint;
        if (!buildError.empty())
            ss << "\n编译错误：\n" << buildError;
        else
            ss << "\n上次实际输出：\n" << output;
        return ss.str();
    };

    if (c.prompt.empty())
    {
        jzWarning().nospace() << "[评测集] 用例缺少 prompt，跳过: " << src;
        return false;
    }
    return true;
}

// 解析一段 JSON 文本（去 BOM）为一个用例；失败/缺 prompt 返回 false
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

// 取文件路径的父目录（用于让索引里的相对 file 基于索引所在目录解析）
static std::string parentDirOf(const std::string &path)
{
    size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? "." : path.substr(0, p);
}

// 加载"用例索引"文件：每行一个索引项，指向一个用例 JSON 文件的位置。
// 索引项支持两种写法：
//   纯字符串： "hello_world.json"
//   对象：     {"file":"hello_world.json","enabled":true}
// 相对路径基于索引所在目录（baseDir）解析；缺失/解析失败/被禁用/无 prompt 的项会被跳过并告警。
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
            continue;   // 空行忽略
        size_t b = line.find_last_not_of(" \t\r\n");
        std::string trimmed = line.substr(a, b - a + 1);

        // 解析索引项
        Json::Value idx;
        Json::Reader reader;
        if (!reader.parse(trimmed, idx))
        {
            jzWarning().nospace() << "[评测集] 索引行解析失败，跳过: " << indexPath
                                  << ":" << lineNo;
            continue;
        }
        std::string rel;
        bool enabled = true;
        if (idx.isString())
        {
            rel = idx.asString();
        }
        else if (idx.isObject())
        {
            if (idx.isMember("file") && idx["file"].isString())
                rel = idx["file"].asString();
            if (idx.isMember("enabled"))
                enabled = idx["enabled"].asBool();
        }
        if (rel.empty())
        {
            jzWarning().nospace() << "[评测集] 索引项缺少 file 字段，跳过: " << indexPath
                                  << ":" << lineNo;
            continue;
        }
        if (!enabled)
        {
            jzDebug().nospace() << "[评测集] 索引项被禁用，跳过: " << rel;
            continue;
        }

        // 解析被引用的用例 JSON：相对路径基于索引所在目录
        std::string casePath = rel;
        if (!JZDir::isAbsolutePath(rel))
            casePath = baseDir + "/" + rel;
        if (!JZFileInfo::exists(casePath))
        {
            jzWarning().nospace() << "[评测集] 用例文件不存在，跳过: " << casePath
                                  << " (索引 " << indexPath << ":" << lineNo << ")";
            continue;
        }
        std::ifstream fin(casePath, std::ios::binary);
        if (!fin)
        {
            jzWarning().nospace() << "[评测集] 无法打开用例文件，跳过: " << casePath;
            continue;
        }
        std::stringstream ss;
        ss << fin.rdbuf();
        BenchmarkCase c;
        if (parseCaseText(ss.str(), casePath, c))
            out.push_back(std::move(c));
    }

    if (out.empty())
    {
        err = "索引中无可用用例（文件缺失/解析失败/缺少 prompt/全部禁用）";
        return false;
    }
    return true;
}

// 目录模式（无索引）：直接扫描目录内所有 *.json，每文件一用例
static bool loadJsonDir(const std::string &dataDir, std::vector<BenchmarkCase> &out, std::string &err)
{
    JZDir dir(dataDir);
    auto files = dir.entryInfoList({ "*.json" }, JZDir::Files);
    for (const auto &fi : files)
    {
        std::string path = dataDir + "/" + fi.fileName();
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            jzWarning().nospace() << "[评测集] 无法打开: " << path;
            continue;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        BenchmarkCase c;
        if (parseCaseText(ss.str(), path, c))
            out.push_back(std::move(c));
    }
    if (out.empty())
    {
        err = "目录下无可用用例（*.json 均为空/解析失败/缺少 prompt）";
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
        // 目录模式：优先用 cases.jsonl 作为索引；没有则直接扫描 *.json
        std::string index = dataDir + "/cases.jsonl";
        if (JZFileInfo::exists(index))
            return loadIndex(index, dataDir, out, err);
        return loadJsonDir(dataDir, out, err);
    }

    // 单文件模式：必须是 .jsonl 索引文件（每行指向一个用例 JSON）
    if (dataDir.size() >= 6 && dataDir.compare(dataDir.size() - 6, 6, ".jsonl") == 0)
    {
        return loadIndex(dataDir, parentDirOf(dataDir), out, err);
    }

    err = "评测集路径必须是目录或 .jsonl 索引文件: " + dataDir;
    return false;
}

// ⚠ 必须"填调用方的对象"(传引用进来), 不能自己建局部 cfg 再按值返回:
//   parser.addOption 存的是**指针**, 局部对象在函数返回后已析构, 调用方 parse() 时
//   会写进已经销毁的内存 —— 传 --modelUrl/--modelName 就 0xC0000005(实测)。
static void loadLlmConfig(JZCommandLineParser &parser, JZAgentConfig &cfg)
{
    // 缺省沿用仓库 test/agent/test_agent.cpp 的接入；可用命令行 / 环境变量覆盖
    cfg.modelUrl = "http://172.17.160.147:8080/v1/chat/completions";
    cfg.modelName = "my_model";
    cfg.maxRounds = 100;

    const char *eUrl = std::getenv("JZ_BENCH_MODEL_URL");
    const char *eName = std::getenv("JZ_BENCH_MODEL_NAME");
    const char *eToken = std::getenv("JZ_BENCH_MODEL_TOKEN");
    if (eUrl) cfg.modelUrl = eUrl;
    if (eName) cfg.modelName = eName;
    if (eToken) cfg.modelToken = eToken;

    parser.addOption("--modelUrl", &cfg.modelUrl);
    parser.addOption("--modelName", &cfg.modelName);
    parser.addOption("--modelToken", &cfg.modelToken);
}

} // namespace

int main(int argc, char *argv[])
{
    JZApplication app;

    fprintf(stderr, "[bench] JZApplication constructed\n");
    fflush(stderr);

    printf("hello world\n");

    // 编译器 / LSP 路径
    // 缺省：环境变量 JZ_TS_LSP -> 否则同目录下的 tsc_shim.exe（本机 node 版 tsc 的包装）。
    std::string tscPath = jzApp->applicationDirPath() + "/ts_compiler/tsc/tsc.exe";
    std::string tsgoPath = tscPath;   // 原生 tsc 也经同一 shim（支持 --lsp --stdio）
    std::string dataDir = JZPlatform::applicationDirPath() + "/data/benchmark";  // 评测集目录（可 --data 覆盖）
    std::string casesFilter;   // --cases：逗号分隔的用例名/通配符，仅跑匹配的（可多个）
    int httpPort = 18080;      // --http-port：输入 HTTP 服务监听端口（JS 用 fetch 读输入）
    bool noColor = false;      // --no-color：关闭 ANSI 彩色输出（重定向时本就自动关闭）
    for (size_t i = 0; i < dataDir.size(); i++)
        if (dataDir[i] == '\\')
            dataDir[i] = '/';

    JZCommandLineParser parser;
    parser.setDescription("code_agent 用例评测");
    JZAgentConfig llm;                 // 生命周期必须覆盖 parse(): parser 里存的是它的成员地址
    loadLlmConfig(parser, llm);        // 注册 --model* 选项
    parser.addOption("--tsc", &tscPath);
    parser.addOption("--tsgo", &tsgoPath);
    parser.addOption("--data", &dataDir);
    parser.addOption("--cases", &casesFilter);
    parser.addOption("--http-port", &httpPort);
    parser.addOption("--no-color", &noColor);
    if (!parser.parse(argc, argv))
    {
        jzError() << parser.errorString().c_str();
        return 2;
    }

    // 打开 cmd 的虚拟终端模式（Win10+ 支持），之后 printf 的 ANSI 转义才会被当颜色解析；
    // 输出重定向到文件/管道时自动降级为纯文本。
    g_colorEnabled = !noColor && enableConsoleColor();
    if (tscPath.empty() || !JZFileInfo::exists(tscPath))
    {
        jzError().nospace() << "未配置可用的宿主 tsc（--tsc 或环境变量 JZ_TS_LSP），无法编译工程。";
        return 2;
    }

    std::string codeRuntime = JZPlatform::applicationDirPath();
    for (size_t i = 0; i < codeRuntime.size(); i++)
        if (codeRuntime[i] == '\\')
            codeRuntime[i] = '/';
    codeRuntime += "/code_runtime.exe";
    if (!JZFileInfo::exists(codeRuntime))
    {
        jzError().nospace() << "未找到 code_runtime.exe: " << codeRuntime
                            << "（请先构建 code_runtime 目标）";
        return 2;
    }

    CodeAgentConfig code;
    code.tscPath = tscPath;

    JZEventLoop loop;
    Benchmark bench(&loop, llm, code, codeRuntime);

    // 从评测集（--data 指向目录或 cases.jsonl 索引文件）加载用例；索引里的每个 file 引用一个用例 JSON
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

    bench.setCases(cases);

    // 评测集目录：--data 指向目录时即其本身；指向 cases.jsonl 索引文件时取所在目录。
    // 供 verify 命令里的 ${data} 占位符定位随用例发布的校验脚本（如 data/five_chess_server/...）。
    {
        std::string dataBase = dataDir;
        JZFileInfo fi(dataDir);
        if (!fi.isDir())
            dataBase = parentDirOf(dataDir);
        bench.setDataDir(dataBase);
    }


    // 启动全局输入 HTTP 服务（JS 用 fetch 读取当前用例注入的输入）    
    if (!bench.startHttpServer(httpPort))
        return 2;

    bench.run();

    int ret = loop.exec();
    (void)ret;
    return 0;
}
