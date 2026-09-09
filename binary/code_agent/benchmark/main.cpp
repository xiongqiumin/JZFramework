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
// 评测集从 data/ 下的 JSON 加载（见 loadCases）。第一个移植过去的用例 hello_world：
//   要求模型"写一个函数，输出 hello world"，编译运行后检查 stdout 是否包含
//   "hello world"（大小写不敏感）。
//
// 配置（命令行 / 环境变量）：
//   --modelUrl / --modelName / --modelToken   大模型接入（缺省沿用仓库测试默认值）
//   --tsc   宿主侧 tsc 全路径（也可走环境变量 JZ_TS_LSP；缺失则无法编译，退出 2）
//   --tsgo  原生 tsgo(tsc) LSP 全路径（缺省同 --tsc，用于 TS 工具；可空）
//
// 退出码：0=全部用例通过；1=有失败；2=前置条件缺失（编译器 / 工程初始化失败）。
//
// 注意：本文件不要 include <windows.h>（JZAgentTool.h 的 BOOLEAN 与 winnt.h 冲突）。

#define _CRT_SECURE_NO_WARNINGS

#include <cstdio>
#include <cctype>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <fstream>

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

#include "JZCodeAgent.h"

namespace
{

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

// ---- 单用例结果 ----
struct CaseResult
{
    std::string name;
    bool passed = false;
    int attempts = 0;
    int promptTokens = 0;       // 本用例消耗的 prompt token
    int completionTokens = 0;   // 本用例消耗的 completion token
    int totalTokens = 0;        // 本用例消耗的总 token
    int llmMs = 0;              // 本用例 LLM 生成耗时（含工具自驱循环，跨重试累计）
    int buildMs = 0;            // 本用例宿主编译耗时
    int runMs = 0;              // 本用例 code_runtime 运行耗时
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
        m_httpThread.quit();
        m_httpThread.wait();
    }

    void setCases(const std::vector<BenchmarkCase> &cases) { m_cases = cases; }
    
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

        if (i == 0)
        {
            jzDebug().noquote().nospace() << "系统提示词:\n" << m_agent->agent()->dumpSystemPrompt() << "\n";
            jzDebug().noquote().nospace() << "工具:\n" << m_agent->agent()->dumpToolsPrompt() << "\n";
        }

        // setWorkspace 内部会 buildAgent 重建 agent，把 startHttpServer 追加的"读取输入"约定冲掉，
        // 因此重建之后再注入一次，确保模型知道从 http://127.0.0.1:<port>/input 读取输入。
        auto jz_agent = m_agent->agent();
        if (!m_httpPromptExtra.empty())
        {
            jz_agent->setSystemPrompt(jz_agent->systemPrompt() + m_httpPromptExtra);
        }
        connect(&jz_agent->sigToolCall, [](const ToolCall& t) {
            jzDebug() << "tool call:" << t.function_name;
        });

        jzDebug().nospace() << "==== 用例[" << m_case.name << "] 开始 (maxAttempts="
                            << m_case.maxAttempts << ", root=" << root << ") ====";
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

        // 2) 隔离运行产物（code_runtime 子进程），捕获 stdout
        std::string output;
        bool runOk = false;
        int64_t r0 = JZPlatform::getTickCountMs();
        if (buildOk)
            runOk = runCodeRuntime(output);
        m_result.runMs += (int)(JZPlatform::getTickCountMs() - r0);
        m_result.lastOutput = output;

        jzDebug().nospace() << "  [尝试 " << m_attempt << "] build="
                            << (buildOk ? "OK" : "FAIL")
                            << " run=" << (runOk ? "OK" : "FAIL")
                            << " output=\"" << output.substr(0, 200) << "\"";
        if (!buildOk)
            jzDebug().nospace() << "  [尝试 " << m_attempt << "] buildError: "
                                << buildErr.substr(0, 400);

        bool ok = buildOk && runOk && m_case.check(output);
        if (ok)
        {
            m_result.passed = true;
            finishCase();
            return;
        }

        // 3) 还有次数就反馈重试，否则判定失败
        if (m_attempt < m_case.maxAttempts)
        {
            std::string fb = m_case.retry(output, buildErr);
            jzDebug().nospace() << "  [尝试 " << m_attempt << "] 未通过，反馈重试: " << fb;
            doAttempt(fb);
        }
        else
        {
            m_result.passed = false;
            finishCase();
        }
    }

    bool runCodeRuntime(std::string &output)
    {
        JZProcess proc;
        proc.setProcessChannelMode(JZProcess::MergedChannels);
        proc.setWorkingDirectory(m_root);
        if (!proc.start(m_codeRuntime, {m_root}))
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
        // 及时抓取本用例累计 token：下一用例 setWorkspace 会重建底层 agent、context 清零，
        // 故必须在此处（当前用例 agent 尚在）抓取并累加到全局总量。
        const auto &u = m_agent->agent()->context()->usage;
        m_result.promptTokens = u.prompt_tokens;
        m_result.completionTokens = u.completion_tokens;
        m_result.totalTokens = u.total_tokens;
        m_totalPrompt     += u.prompt_tokens;
        m_totalCompletion += u.completion_tokens;
        m_totalTokens     += u.total_tokens;
        m_totalLlmMs      += m_result.llmMs;
        m_totalBuildMs    += m_result.buildMs;
        m_totalRunMs      += m_result.runMs;

        const CaseResult &r = m_result;
        jzDebug().nospace() << "---- 用例[" << r.name << "] 结束: "
                            << (r.passed ? "PASS" : "FAIL")
                            << " (attempts=" << r.attempts
                            << ", token=" << r.totalTokens << ")";
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
            jzDebug().nospace() << (r.passed ? "[PASS] " : "[FAIL] ")
                                << r.name << "  (attempts=" << r.attempts
                                << ", token=" << r.totalTokens
                                << ", LLM=" << (r.llmMs/1000.0) << "s"
                                << ", 编译=" << (r.buildMs/1000.0) << "s"
                                << ", 运行=" << (r.runMs/1000.0) << "s" << ")";
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
        jzDebug().nospace() << "总消耗 token: prompt=" << m_totalPrompt
                            << " completion=" << m_totalCompletion
                            << " total=" << m_totalTokens;
        int64_t totalMs = m_benchTimer.elapsed();
        int64_t accounted = m_totalLlmMs + m_totalBuildMs + m_totalRunMs;
        int64_t otherMs = totalMs > accounted ? totalMs - accounted : 0;
        jzDebug().nospace() << "耗时分解: 总=" << (totalMs/1000.0) << "s"
                            << "  LLM=" << (m_totalLlmMs/1000.0) << "s"
                            << "  编译=" << (m_totalBuildMs/1000.0) << "s"
                            << "  运行=" << (m_totalRunMs/1000.0) << "s"
                            << "  其他=" << (otherMs/1000.0) << "s";
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
    int64_t m_llmStartMs = 0;       // 本轮 LLM 请求发出时刻（doAttempt）
    int64_t m_totalLlmMs = 0;       // 全部用例累计 LLM 耗时
    int64_t m_totalBuildMs = 0;     // 全部用例累计编译耗时
    int64_t m_totalRunMs = 0;       // 全部用例累计运行耗时
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
};

// 大小写不敏感地判断 output 是否包含子串
bool containsIC(const std::string &hay, const std::string &needle)
{
    std::string h = hay, n = needle;
    for (auto &c : h) c = (char)std::tolower((unsigned char)c);
    for (auto &c : n) c = (char)std::tolower((unsigned char)c);
    return h.find(n) != std::string::npos;
}

// 从 data/ 目录加载评测集（每个 .json 一个用例）。JSON 约定：
//   name            : 用例名（缺省取文件名去扩展名）
//   prompt          : 初始任务描述（必填，缺失则跳过该文件）
//   maxAttempts     : 最大尝试次数（缺省 3）
//   timeout         : 单轮运行超时（秒，缺省 30）；超过则判本次运行失败
//   input           : 注入给本用例的输入文本（缺省空）；运行时经全局 HTTP 服务
//                      http://127.0.0.1:<port>/input 暴露给 JS（模型用 fetch 读取）
//   expect.contains        : 判定输出需包含的子串
//   expect.caseInsensitive : contains 比较是否忽略大小写（缺省 false）
//   retryHint       : 重试时给模型的提示（实际输出/编译错误会自动追加在后）
static bool loadCases(const std::string &dataDir, std::vector<BenchmarkCase> &out, std::string &err)
{
    if (!JZFileInfo::exists(dataDir))
    {
        err = "评测集目录不存在: " + dataDir;
        return false;
    }

    JZDir dir(dataDir);
    auto files = dir.entryInfoList({ "*.json" }, JZDir::Files);
    if (files.empty())
    {
        err = "目录下无 *.json 用例文件";
        return false;
    }

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
        std::string text = ss.str();
        // 去掉 UTF-8 BOM（Json::Reader 不认）
        if (text.size() >= 3 && (unsigned char)text[0] == 0xEF &&
            (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
            text.erase(0, 3);

        Json::Value root;
        Json::Reader reader;
        if (!reader.parse(text, root) || !root.isObject())
        {
            jzWarning().nospace() << "[评测集] 解析失败: " << path;
            continue;
        }

        BenchmarkCase c;
        std::string base = fi.fileName();
        size_t dot = base.rfind('.');
        if (dot != std::string::npos)
            base = base.substr(0, dot);
        c.name = root.isMember("name") && root["name"].isString()
                     ? root["name"].asString() : base;
        c.prompt = root.isMember("prompt") && root["prompt"].isString()
                       ? root["prompt"].asString() : "";
        c.maxAttempts = root.isMember("maxAttempts") ? root["maxAttempts"].asInt() : 3;
        c.timeoutMs = (root.isMember("timeout") ? root["timeout"].asInt() : 30) * 1000;
        c.input = (root.isMember("input") && root["input"].isString())
                      ? root["input"].asString() : "";

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
            jzWarning().nospace() << "[评测集] 用例缺少 prompt，跳过: " << path;
            continue;
        }

        out.push_back(std::move(c));
    }

    if (out.empty())
        err = "没有可用用例（全部解析失败或缺少 prompt）";
    return !out.empty();
}

static JZAgentConfig loadLlmConfig(JZCommandLineParser &parser)
{
    JZAgentConfig cfg;
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
    return cfg;
}

} // namespace

int main(int argc, char *argv[])
{
    JZApplication app;

    // 编译器 / LSP 路径
    // 缺省：环境变量 JZ_TS_LSP -> 否则同目录下的 tsc_shim.exe（本机 node 版 tsc 的包装）。
    std::string tscPath = jzApp->applicationDirPath() + "/ts_compiler/tsc/tsc.exe";
    std::string tsgoPath = tscPath;   // 原生 tsc 也经同一 shim（支持 --lsp --stdio）
    std::string dataDir = JZPlatform::applicationDirPath() + "/data/benchmark";  // 评测集目录（可 --data 覆盖）
    std::string casesFilter;   // --cases：逗号分隔的用例名，仅跑这些（可多个）
    int httpPort = 18080;      // --http-port：输入 HTTP 服务监听端口（JS 用 fetch 读输入）
    for (size_t i = 0; i < dataDir.size(); i++)
        if (dataDir[i] == '\\')
            dataDir[i] = '/';

    JZCommandLineParser parser;
    parser.setDescription("code_agent 用例评测");
    JZAgentConfig llm = loadLlmConfig(parser);   // 注册 --model* 选项
    parser.addOption("--tsc", &tscPath);
    parser.addOption("--tsgo", &tsgoPath);
    parser.addOption("--data", &dataDir);
    parser.addOption("--cases", &casesFilter);
    parser.addOption("--http-port", &httpPort);
    if (!parser.parse(argc, argv))
    {
        jzError() << parser.errorString().c_str();
        return 2;
    }

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

    // 从评测集（data/ 下的 *.json）加载用例，先把 hello_world 移植过去
    std::vector<BenchmarkCase> cases;
    std::string loadErr;
    if (!loadCases(dataDir, cases, loadErr))
    {
        jzError().nospace() << "加载评测集失败: " << loadErr;
        return 2;
    }

    // 按 --cases 过滤（逗号分隔，可多个；忽略前后空白；名字不匹配的跳过）
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
                if (w == c.name) { hit = true; break; }
            if (hit)
                filtered.push_back(std::move(c));
        }
        cases = std::move(filtered);
        if (cases.empty())
        {
            jzError().nospace() << "指定的用例都不存在: " << casesFilter;
            return 2;
        }
    }

    bench.setCases(cases);

    // 启动全局输入 HTTP 服务（JS 用 fetch 读取当前用例注入的输入）    
    if (!bench.startHttpServer(httpPort))
        return 2;

    bench.run();

    int ret = loop.exec();
    (void)ret;
    return 0;
}
