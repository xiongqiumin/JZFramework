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
// 第一个用例：hello world
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
#include <sstream>
#include <string>
#include <vector>

#include "JZApplication.h"
#include "JZByteArray.h"
#include "JZCommandLineParser.h"
#include "JZDebug.h"
#include "JZDir.h"
#include "JZEventLoop.h"
#include "JZFileInfo.h"
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

    // 判定：运行得到的全部输出（合并 console.* / 运行时日志）是否满足预期
    std::function<bool(const std::string &output)> check;

    // 重试时给模型的反馈（基于上一次实际输出 / 编译错误构造）
    std::function<std::string(const std::string &output, const std::string &buildError)> retry;
};

// ---- 单用例结果 ----
struct CaseResult
{
    std::string name;
    bool passed = false;
    int attempts = 0;
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

class Benchmark : public JZObject
{
public:
    Benchmark(JZEventLoop *loop,
              const JZAgentConfig &llm,
              const CodeAgentConfig &code,
              const std::string &codeRuntime)
        : m_loop(loop), m_llm(llm), m_code(code), m_codeRuntime(codeRuntime)
    {
        m_agent = new JZCodeAgent(this);
        m_code.llmConfig = m_llm;
        m_agent->init(m_code);

        m_poll = new JZTimer(this);
        connect(&m_poll->sigTimeout, this, &Benchmark::onPoll);
    }

    void setCases(const std::vector<BenchmarkCase> &cases) { m_cases = cases; }

    void run()
    {
        if (m_cases.empty())
        {
            jzError() << "没有可运行的用例";
            m_loop->quit();
            return;
        }
        m_idx = 0;
        startCase(0);
    }

protected:
    void startCase(size_t i)
    {
        m_case = m_cases[i];
        m_result = CaseResult{};
        m_result.name = m_case.name;
        m_attempt = 0;
        m_state = BenchState::Idle;

        // 每个用例一个独立临时工程，避免互相污染
        std::string root = JZPlatform::applicationTmpPath() + "/" + JZUuid::createUuid().toString() +"/bench_" + m_case.name;

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

        jzDebug().nospace() << "==== 用例[" << m_case.name << "] 开始 (maxAttempts="
                            << m_case.maxAttempts << ", root=" << root << ") ====";
        doAttempt(m_case.prompt);
    }

    void doAttempt(const std::string &text)
    {
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
            for (int i = m_renderedId; i < seq; i++)
            {
                std::string text = str_utf8_to_local(logs[i].text);
                if (m_renderedType != logs[i].type)
                {
                    printf("\n%s:", logs[i].type.c_str());
                    m_renderedType = logs[i].type;
                }
                printf("%s", text.c_str());
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

        // 1) 宿主真实编译打包
        std::string buildErr;
        std::string buildOut;
        bool buildOk = m_agent->buildProject(m_root, buildErr, &buildOut);
        m_result.lastBuildError = buildErr;

        // 2) 隔离运行产物（code_runtime 子进程），捕获 stdout
        std::string output;
        bool runOk = false;
        if (buildOk)
            runOk = runCodeRuntime(output);
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
        if (!proc.waitForFinished(120000))
        {
            proc.killProcess();
            output = "[benchmark] code_runtime 超时(120s)";
            return false;
        }
        output = str_fromLocal(proc.readAllStandardOutput());
        return proc.exitCode() == 0;
    }

    void finishCase()
    {
        const CaseResult &r = m_result;
        jzDebug().nospace() << "---- 用例[" << r.name << "] 结束: "
                            << (r.passed ? "PASS" : "FAIL")
                            << " (attempts=" << r.attempts << ")";
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
                                << r.name << "  (attempts=" << r.attempts << ")";
            if (!r.passed)
            {
                if (!r.lastBuildError.empty())
                    jzDebug().nospace() << "    buildError: " << r.lastBuildError.substr(0, 300);
                jzDebug().nospace() << "    output: " << r.lastOutput.substr(0, 300);
            }
        }
        jzDebug().nospace() << "总分: " << passed << " / " << m_results.size();
        jzDebug() << "==========================================";

        m_loop->quit();
    }

    JZEventLoop *m_loop;
    JZAgentConfig m_llm;
    CodeAgentConfig m_code;
    std::string m_codeRuntime;

    JZCodeAgent *m_agent = nullptr;
    JZTimer *m_poll = nullptr;

    std::vector<BenchmarkCase> m_cases;
    size_t m_idx = 0;
    BenchmarkCase m_case;
    CaseResult m_result;
    int m_attempt = 0;
    std::string m_root;
    BenchState m_state = BenchState::Idle;

    std::vector<CaseResult> m_results;
    int m_renderedId = 0;
    std::string m_renderedType;
};

// 大小写不敏感地判断 output 是否包含子串
bool containsIC(const std::string &hay, const std::string &needle)
{
    std::string h = hay, n = needle;
    for (auto &c : h) c = (char)std::tolower((unsigned char)c);
    for (auto &c : n) c = (char)std::tolower((unsigned char)c);
    return h.find(n) != std::string::npos;
}

// 第一个用例：hello world
BenchmarkCase makeHelloWorldCase()
{
    BenchmarkCase c;
    c.name = "hello_world";
    c.prompt = "请用 TypeScript 写一个函数，调用后向控制台输出 hello world。";
    c.maxAttempts = 3;
    c.check = [](const std::string &output) {
        return containsIC(output, "hello world");
    };
    c.retry = [](const std::string &output, const std::string &buildError) {
        std::ostringstream ss;
        ss << "上一次运行的结果不对。请检查并修正你的代码后重新完成要求。"
              "要求是：写一个函数，调用后向控制台输出 hello world（严格为小写 "
              "\"hello world\"）。\n";
        if (!buildError.empty())
            ss << "编译错误：\n" << buildError << "\n";
        else
            ss << "上次实际输出：\n" << output << "\n";
        return ss.str();
    };
    return c;
}

static JZAgentConfig loadLlmConfig(JZCommandLineParser &parser)
{
    JZAgentConfig cfg;
    // 缺省沿用仓库 test/agent/test_agent.cpp 的接入；可用命令行 / 环境变量覆盖
    cfg.modelUrl = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions";
    cfg.modelName = "qwen3.7-max-2026-06-08";
    cfg.modelToken = "sk-c7cbd6d56e5049c69a640f9596c978d3";
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
    std::string tscPath = "E:/libs/ts_compiler/app/node_modules/@typescript/typescript-win32-x64/lib/tsc.exe";
    std::string tsgoPath = tscPath;   // 原生 tsc 也经同一 shim（支持 --lsp --stdio）

    JZCommandLineParser parser;
    parser.setDescription("code_agent 用例评测");
    JZAgentConfig llm = loadLlmConfig(parser);   // 注册 --model* 选项
    parser.addOption("--tsc", &tscPath);
    parser.addOption("--tsgo", &tsgoPath);
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

    // 注册用例（目前只第一个：hello world）
    bench.setCases({ makeHelloWorldCase() });
    bench.run();

    int ret = loop.exec();
    (void)ret;
    return 0;
}
