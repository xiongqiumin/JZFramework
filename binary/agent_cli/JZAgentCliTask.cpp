#include "JZAgentCli.h"

#include "JZByteArray.h"
#include "JZDebug.h"
#include "JZDir.h"
#include "JZFile.h"
#include "JZFileInfo.h"
#include "JZNetworkAccessManager.h"   // 联网工具共享的访问管理器(+ JZProxyConfig)
#include "JZPlatform.h"
#include "JZProcess.h"       // 验证命令的异步执行(超时/进程树收尾)
#include "JZString.h"        // str_toLower
#include "JZVirtualTerminal.h"
#include "json/json.h"
#include "JZLog.h"              // JZLogManager / LogModule_Agent / LOG_INFO
#include "JZUtf8Iterator.h"     // 验证输出按字符(码点)截断, 不切坏多字节字符

#include <algorithm>            // std::min(日志摘要里取首行片段)
#include <string>

// 回合诊断日志: 把工具调用 / 思考 / 回复 / 输入 记进独立的 Agent 日志模块(模块 id = LogModule_Agent)。
// 该模块不走 Console, 只有配置 logFile 才落盘到 <logFile目录>/Agent.log, 没配就不打印。
//
// 精简口径(骨架优先, 大文本只留摘要): 日志是给人**排查流程**看的, 不是模型上下文的备份 ——
//   * 工具行: "<工具> args=<简写> => ok/fail <结果摘要>"。长结果一律折叠(只留头 + 尾 + 总长),
//     read_file 成功更进一步只记大小(整份文件内容对排查没意义, 要看直接看文件)。
//   * think/reply/input: 只记"单行摘要"(标签 + 字节数 + 首行片段), 不写全文 —— 长思考/长回复
//     整段落盘会把日志刷成几万行, 真正要复读全文看的是对话历史 / 网页事件流, 不是日志。
namespace {
const int kToolResultBytes = 300;   // 工具结果摘要里保留的字节数(超出折叠成 头…尾)
const int kTextPreviewBytes = 200;  // think/reply/input 摘要里保留的首行字节数

// 单行化: 换行/制表符折叠成空格, 便于日志一条占一行
std::string logOneLine(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        out += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    return out;
}

// UTF-8 边界工具: 日志与验证输出都可能是中文/多字节, 按**字节** substr 会把一个字切成
// 半个(日志里表现为结尾一个乱码字符)。统一走框架的 JZUtf8Iterator(码点迭代 + byteIndex):
//   utf8CutAtByteLimit  = 按字节上限取前缀, 落点对齐到码点边界
//   utf8TailAtByteLimit = 按字节上限取后缀, 起点对齐到码点边界
//   utf8CountChars / utf8CutAtChar / utf8TailAtChar = 按码点个数取(验证输出用)
// 第 n 个码点的字节起始下标(不足 n 个码点则返回全长)
static int utf8CharByteOffset(const std::string &s, size_t chars)
{
    JZUtf8Iterator it(s);
    int n = 0;
    while (it.isValid() && (size_t)n < chars)
    {
        ++it;
        n++;
    }
    return it.byteIndex();
}

static size_t utf8CountChars(const std::string &s)
{
    return (size_t)JZUtf8Iterator::countCodepoints(s);
}

static std::string utf8CutAtChar(const std::string &s, size_t chars)
{
    return s.substr(0, (size_t)utf8CharByteOffset(s, chars));
}

// 按**字节上限**截断, 但落点对齐到码点边界(不留半个多字节字符)。
static std::string utf8CutAtByteLimit(const std::string &s, size_t maxBytes)
{
    if (s.size() <= maxBytes)
        return s;
    JZUtf8Iterator it(s);
    int last = 0;
    while (it.isValid() && (size_t)it.byteIndex() < maxBytes)
    {
        last = it.byteIndex();
        ++it;
    }
    return s.substr(0, (size_t)last);
}

// 从尾部按字节上限取后缀, 起点落在码点边界上(不会以半个字开头)。
static std::string utf8TailAtByteLimit(const std::string &s, size_t maxBytes)
{
    if (s.size() <= maxBytes)
        return s;
    const size_t floor = s.size() - maxBytes;
    JZUtf8Iterator it(s);
    size_t start = s.size();
    while (it.isValid())
    {
        if ((size_t)it.byteIndex() >= floor)
        {
            start = (size_t)it.byteIndex();
            break;
        }
        ++it;
    }
    return s.substr(start);
}

static std::string utf8TailAtChar(const std::string &s, size_t chars)
{
    size_t total = utf8CountChars(s);
    if (total <= chars)
        return s;
    return s.substr((size_t)utf8CharByteOffset(s, total - chars));
}

// 结果/文本摘要: 短就原样单行化; 长就给 "头…尾[共N字节]"。用于工具结果。
// 头尾都按 UTF-8 码点边界截, 中文不会被切成半个字。
std::string logSummary(const std::string &s, int keepBytes)
{
    if (keepBytes <= 0)
        return std::string();
    if ((int)s.size() <= keepBytes)
        return logOneLine(s);
    const size_t head = (size_t)keepBytes / 2;
    const size_t tail = (size_t)keepBytes - head;
    return logOneLine(utf8CutAtByteLimit(s, head)) + " … "
           + logOneLine(utf8TailAtByteLimit(s, tail))
           + "[共 " + std::to_string(s.size()) + " 字节]";
}

// Agent 模块日志。JZLog 多模块: 每个模块独立 type(Console/File/None), 互不干扰。
static void agentLog(const std::string &msg)
{
    JZLogManager::instance()->log(LogModule_Agent, LOG_INFO, msg);
}

// think/reply/input 摘要: 标签 + "N 字节" + 首行片段(不写全文)。
// 预览按 UTF-8 码点边界截, 结尾的 "…" 表示后面还有内容, 不是半个汉字。
static std::string logTextLine(const char *tag, const std::string &text)
{
    const std::string head = utf8CutAtByteLimit(text, (size_t)kTextPreviewBytes);
    std::string line = std::string(tag) + "[" + std::to_string(text.size()) + " 字节] " + logOneLine(head);
    if (head.size() < text.size())
        line += " …";
    return line;
}
}

#include "JZHttpServer.h"     // HTTP 服务(路由 / JZHttpServerClient / JZHttpRespone)
#include "JZHttpHandler.h"
#include "JZHttpStaticFiles.h"

#include "JZNet/JZNetPacket.h"  // JZNetPackInit(frpc 的 JZNetClient 依赖包注册)
#include "JZAgentNodePacket.h"  // link 模式 node 的执行面协议(remote_ssh)
#include "JZAgentHubPacket.h"   // 控制面: cli <-> server 的任务下发/上报
#include "JZNodeLink.h"         // cli -> node 的连接与 remote_ssh 工具
#include "JZHubLink.h"          // cli -> server 的控制面连接(managed 模式)

#include <cstdio>
#include <cstdlib>            // atoi(解析 "HTTP 402 ..." 里的状态码)
#ifndef _WIN32
#include <sys/wait.h>         // WIFEXITED / WEXITSTATUS(popen 取退出码)
#endif

#include "JZTimer.h"          // 重试前的延时(JZTimer::singleShot)

#include "JZTerminalTools.h"  // 本地终端工具(JZTerminalRunTool 等; 池定义在 JZVirtualTerminal.h)
#include "JZMcpClient.h"      // MCP 客户端(连外部 MCP 服务器, 把远端工具挂进本 agent)

#include <iostream>
#include <fstream>            // system.extraFile(宿主常驻说明)读文件
#include <sstream>
#include <thread>
#include <chrono>             // 历史记录的起止时间戳(nowMs)

using namespace std;

const int JZAgentCli::kHistoryMax;

// 单次读文件上限(与 node 一致): 避免把几百兆读进内存。
static const int kReadFileLimit = 8 * 1024 * 1024;

// 验证输出回包上限(与旧 popen 实现一致): 几十 KB 足够定位, 再大只会撑爆 JSON 回包与上下文。
static const size_t kMaxVerifyBytes = 65536;

// /task 的图片附件上限(张数 / 单张字符数 / 合计字符数)。
//   张数与 code_agent 输入框的附图上限对齐; 字符上限按"截图 base64 通常几百 KB"给足余量,
//   但必须有硬上限 —— 整个 body 都要进内存, 还要再塞进 LLM 请求体。
//   ⚠ 合计上限必须明显小于 main.cpp 里抬高的 net maxRecvSize(32MB), 否则先被 mongoose
//     的接收缓冲上限掐断(客户端只看到 connection aborted, 没有任何错误回包)。
static const int kMaxTaskImages = 8;
static const int kMaxImageChars = 8 * 1024 * 1024;
static const int kMaxImagesTotalChars = 24 * 1024 * 1024;

// JZByteArray -> std::string(进程输出按字节接回来, 原样存)
static std::string bytesToText(const JZByteArray &data)
{
    if (data.size() <= 0 || !data.data())
        return std::string();
    return std::string(data.data(), data.size());
}

// UTF-8 边界工具集中在文件顶部的匿名命名空间(日志摘要与验证输出共用同一份实现)。

// 临时目录(放验证命令的包装脚本用): TEMP/TMPDIR 都没有就退回当前目录。
static std::string tempDirPath()
{
#ifdef _WIN32
    const char *t = std::getenv("TEMP");
    if (!t || !*t)
        t = std::getenv("TMP");
#else
    const char *t = std::getenv("TMPDIR");
#endif
    std::string dir = (t && *t) ? std::string(t) : std::string(".");
    for (size_t i = 0; i < dir.size(); i++)
    {
        if (dir[i] == '\\')
            dir[i] = '/';
    }
    while (dir.size() > 1 && dir.back() == '/')
        dir.pop_back();
    return dir;
}

// ==========================================================================
// JZCliToolHost
// ==========================================================================


#include "JZAgentCli.h"

#include "JZAgentCli.h"

JZAgentCliTask::JZAgentCliTask(JZAgentCli *cli, JZObject *parent)
    : JZObject(parent)
    , m_cli(cli)
    , m_host(&m_config)
{
}

JZAgentCliTask::~JZAgentCliTask()
{
    // 工具(m_agent 名下)先销毁, 再收池 —— 工具里存的是池指针
    delete m_termPool;
    m_termPool = nullptr;
}

bool JZAgentCliTask::init(const JZCliConfig &config, std::string &error)
{
    m_config = config;
    m_host.refresh();

    m_agent = new JZAgent(this);
    m_agent->setStreamEnabled(true);
    // 思考开关来自配置(llm.thinking): auto(默认, 不发字段) / on / off
    m_agent->setThinkMode(m_config.thinkMode);

    JZAgentConfig ac;
    ac.rootPath = m_config.rootDir;
    // 全局 .jzagent 的 skills: **总是加载**, 排在项目 skills 之后(同名不覆盖项目的)
    if (!m_config.skillsDir.empty())
        ac.globalSkillsDirs.push_back(m_config.skillsDir + "/skills");
    ac.modelUrl = m_config.modelUrl;
    ac.modelName = m_config.modelName;
    ac.modelToken = m_config.modelToken;
    ac.netTimeout = m_config.netTimeout;
    ac.maxRounds = m_config.maxRounds;
    ac.maxToken = m_config.maxToken;
    ac.extraBody = m_config.extraBody;
    ac.reuseConnection = false;
    ac.tokenStrategy = JZAgentConfig::TokenCompress;
    ac.loopDetect.enabled = m_config.loopDetect;
    ac.loopDetect.window = m_config.loopWindow;
    ac.loopDetect.repeat = m_config.loopRepeat;
    ac.loopDetect.sameResult = m_config.loopSameResult;
    ac.loopDetect.pollRepeat = m_config.loopPollRepeat;
    ac.loopDetect.pollSameResult = m_config.loopPollSameResult;
    ac.loopDetect.textLineRepeat = m_config.loopLineRepeat;
    ac.loopDetect.textBlockRepeat = m_config.loopBlockRepeat;
    ac.loopDetect.textPeriodMax = m_config.loopPeriodMax;
    ac.loopDetect.textPeriodRepeat = m_config.loopPeriodRepeat;
    m_agent->init(ac);

    connect(&m_agent->sigReply, this, &JZAgentCliTask::onReply);
    connect(&m_agent->sigThinking, this, &JZAgentCliTask::onThinking);
    connect(&m_agent->sigToolCall, this, &JZAgentCliTask::onToolCall);
    connect(&m_agent->sigToolResult, this, &JZAgentCliTask::onToolResult);
    connect(&m_agent->sigError, this, &JZAgentCliTask::onError);
    connect(&m_agent->sigFinished, this, &JZAgentCliTask::onFinished);

    setupAgent();
    const std::string sysPrompt = buildSystemPrompt();
    m_agent->setSystemPrompt(sysPrompt);
    m_systemPromptBytes = (int)sysPrompt.size();
    error.clear();
    return true;
}

void JZAgentCliTask::pushEvent(const std::string &type, const Json::Value &data)
{
    if (!m_cli)
        return;
    m_cli->pushEvent(type, data);                       // 本地 SSE 订阅者
    if (m_cli->m_hub && !m_taskId.empty())
        m_cli->m_hub->sendTaskEvent(m_taskId, type, data);  // 控制面(server)状态推进
}

Json::Value JZAgentCliTask::stateJson() const
{
    Json::Value j(Json::objectValue);
    j["busy"] = (bool)m_busy;
    j["taskId"] = m_taskId;
    j["round"] = m_loopRound;
    j["verify_cmd"] = m_verifyCmd;
    j["prompt"] = m_currentPrompt;
    j["reply"] = m_accumReply;
    j["think"] = m_accumThink;
    j["last_verify_output"] = m_lastVerifyOutput;
    return j;
}

void JZAgentCliTask::endLoop()
{
    killVerify();
    if (m_termPool)
    {
        int killed = m_termPool->cancelAll();     // 并发时可能停掉多条
        if (killed > 0)
        {
            Json::Value e;
            e["killed"] = killed;
            pushEvent("terminal_cleanup", e);
        }
    }
    m_cli->cancelNodeExec(this);

    m_loopActive = false;
    m_busy = false;
    m_respond = nullptr;
    m_verifyCmd.clear();
    // 任务真正空闲后才通知宿主派下一条(排队拾取时序, 见 agent-hub-design.md §9)
    m_cli->onTaskFinished(this);
}

bool JZAgentCliTask::cancelLocalTask(const std::string &reason)
{
    if (!m_agent || !m_loopActive)
        return false;
    m_cli->m_cancelling = true;
    m_loopActive = false;
    m_busy = false;
    respondTask("cancelled", m_loopRound, m_accumReply, m_lastVerifyOutput, reason);
    endLoop();
    resetSession(true);
    m_cli->m_cancelling = false;
    m_cli->pumpQueue();
    return true;
}

void JZAgentCliTask::resetSession(bool cancelRunning)
{
    m_loopEpoch++;
    killVerify();
    if (cancelRunning && m_agent)
        m_agent->stop();
    if (m_agent)
        m_agent->reset();
    m_loopActive = false;
    m_busy = false;
    m_respond = nullptr;
    m_accumReply.clear();
    m_accumThink.clear();
    m_thinkLogged = false;
    m_failed = false;
    m_cancelled = false;
    m_lastError.clear();
    m_retryPending = false;
    m_sameFailCount = 0;
    m_lastFeedbackSig.clear();
    m_toolSnaps.clear();
    m_sessionStarted = false;
}

void JZAgentCliTask::onReply(const std::string &text)
{
    // 回复开始 = 思考结束: 先把整条 think 落盘(一条 think 一条), 只记摘要不写全文
    if (!m_thinkLogged && !m_accumThink.empty())
    {
        agentLog(logTextLine("think", m_accumThink));
        m_thinkLogged = true;
    }

    m_accumReply += text;   // 累积本轮最终回复

    if (m_cli->hasEventConsumer())
    {
        Json::Value d;
        d["text"] = text;
        pushEvent("reply", d);
    }
}


void JZAgentCliTask::onThinking(const std::string &text)
{
    m_accumThink += text;          // 累积, 供连接快照(与回包无关); 完整 think 在结束时整条落盘

    if (!m_cli->hasEventConsumer())
        return;

    Json::Value d;
    d["text"] = text;
    pushEvent("thinking", d);
}


void JZAgentCliTask::onToolCall(const ToolCall &call)
{
    JZCliToolSnap snap;
    snap.id = call.id;
    snap.name = call.function_name;
    snap.args = call.function_args;
    m_toolSnaps.push_back(snap);   // 记录, 供连接快照(与合并日志)

    if (!m_cli->hasEventConsumer())
        return;

    Json::Value d;
    d["id"] = call.id;
    d["name"] = call.function_name;
    d["args"] = call.function_args;
    pushEvent("tool_call", d);
}


void JZAgentCliTask::onToolResult(const ToolCallResult &result)
{
    // 一次调用合并为一条: tool_call + tool_result(结果回来时才写, 不单独刷 tool_call)
    for (auto &t : m_toolSnaps)
    {
        if (t.id == result.id)
        {
            t.hasResult = true;
            t.ok = result.isOk;
            t.result = result.result;
            // 结果摘要: 长结果只留头…尾(见文件头"精简口径")。
            // read_file 成功更进一步 —— 结果就是整份文件内容, 对排查没意义(要看看文件本身),
            // 只记大小; 失败时仍照常给摘要(错误信息是排查要看的)。
            std::string resultLog;
            if (t.name == "read_file" && t.ok)
                resultLog = "<file content " + std::to_string(t.result.size()) + " bytes>";
            else
                resultLog = logSummary(t.result, kToolResultBytes);
            agentLog("tool: " + t.name + " args=" + logSummary(t.args, kToolResultBytes)
                     + " => " + std::string(t.ok ? "ok" : "fail") + " " + resultLog);
            break;
        }
    }

    if (!m_cli->hasEventConsumer())
        return;

    Json::Value d;
    d["id"] = result.id;
    d["ok"] = result.isOk;
    d["result"] = result.result;
    pushEvent("tool_result", d);
}

// ---------------------------------------------------------------------------
// 出错自动重试
//   瞬时错误(连接失败/超时/5xx)不该让整个任务直接判失败 —— 把错误原因告诉模型,
//   让它从中断处继续做。分类来自 JZAgent::errorCode(): llmError() 里先置分类再发
//   sigError, 所以 onError 里读到的就是本次错误的分类。
// ---------------------------------------------------------------------------

// "HTTP 402 错误: ..." -> 1=瞬时(原样重发有意义) / 2=可修复(让模型换一版) / 3=不重试。
// 408/409/425/429 是超时/冲突/限流, 归瞬时; **其余 4xx 一律不重试**:
// 请求本身有问题(模型不支持多模态/模型名写错/欠费/key 失效/参数不合法), 原样重发必然
// 同样的错。以前把 4xx 归"可修复"让模型改一版, 结果"400 not a multimodal model"这种
// 配置问题白重试 4 次 —— 用关键字表去猜哪些能救又太脆, 不如统一不重试, 把原文抛给用户。
// 没有状态码(连接失败/超时等)按瞬时处理。
static int httpErrorMode(const std::string &err)
{
    if (err.compare(0, 5, "HTTP ") != 0)
        return 1;

    int code = atoi(err.c_str() + 5);
    if (code <= 0)
        return 1;
    if (code >= 500 || code == 408 || code == 409 || code == 425 || code == 429)
        return 1;
    return (code < 400) ? 1 : 3;
}


std::string JZAgentCliTask::buildRetryPrompt(RetryMode mode, JZAgent::AgentError code,
    const std::string& err) const
{
    std::string e = err.empty() ? std::string("(无错误信息)") : err;
    if (e.size() > 300)
        e = e.substr(0, 300) + "…";

    if (mode == RetryFix)
    {
        // 循环检测中止: 唯一的解法是"别再重复" —— 笼统的"换做法"模型容易理解成
        // "把同样的命令再发一次"或"把那段话再写一遍", 所以这里两样都点名。
        if (code == JZAgent::LoopError)
            return "上一轮被中止了, 原因不是任务失败, 而是检测到你在原地打转:\n" + e + "\n"
            "这一轮请**不要再重复**: 不要用完全相同的参数再调用同一个工具"
            "(不要重复读同一个文件、不要重复跑同一条命令), 也不要把同样的内容"
            "反复写出来。先换做法(换目标/换参数/换工具确认现状), "
            "如果信息其实已经足够, 就直接给出最终结论。";

        return "上一次请求被服务端拒绝了: " + e + "\n"
            "这是第 " + std::to_string(m_fixRetries) + " 次修正尝试: 原样重发还会被拒, "
            "请换一个做法把当前任务做完 —— 若上一条工具调用的参数不是合法 JSON, 就重新发一个参数合法的调用; "
            "若上下文太长, 不要再整段贴文件/整段贴命令输出, 改用搜索定位到具体行再看; "
            "已经完成的部分不要重复做, 已经确认的结论直接沿用。";
    }

    return "上一次请求因错误中断: " + e + "\n"
        "这是一次自动重试, 请从中断处继续完成当前任务: 已经完成的部分不要重复做, "
        "已经确认的结论直接沿用; 若任务其实已经完成, 直接给出最终结论即可。";
}

// 记一次重试 -> 等 retryDelayMs -> 再 request 一条"继续/修正"消息(结束后重新进 onFinished 走验证)

void JZAgentCliTask::scheduleErrorRetry(RetryMode mode)
{
    int epoch = m_loopEpoch;              // 记下当前任务世代, 延时期间任务换掉就不再续跑
    bool fix = (mode == RetryFix);
    int next = (fix ? m_fixRetries : m_errorRetries) + 1;
    int budget = fix ? m_maxFixRetries : m_maxErrorRetries;
    std::string err = m_lastError;
    JZAgent::AgentError code = m_lastErrorCode;   // 延时期间可能被覆盖, 先记下来给提示文案用

    // 半截回复作废, 重试后重新累积(与验证失败续跑同一套语义); 成败等重试结果出来再定
    m_accumReply.clear();
    m_thinkLogged = false;
    m_failed = false;
    m_lastError.clear();

    Json::Value d;
    d["error"] = err;
    d["code"] = (int)m_lastErrorCode;
    d["mode"] = fix ? "fix" : "continue";    // fix = 让模型换做法; continue = 从中断处接着干
    d["retry"] = next;
    d["max_retries"] = budget;
    d["delay_ms"] = m_retryDelayMs;
    pushEvent("retry", d);

    jzWarning().nospace() << "agent 报错, 第 " << next << "/" << budget
        << (fix ? " 次修正式重试" : " 次自动重试")
        << "(" << m_retryDelayMs << "ms 后): " << err;

    auto fire = [this, epoch, err, mode, code]() {
        m_retryPending = false;
        // 等待期间被取消/重置/换了任务: 不再续跑(否则会把消息发进新任务的会话里)
        if (!m_loopActive || epoch != m_loopEpoch || !m_busy)
            return;
        if (!m_agent)
            return;

        if (mode == RetryFix)
            m_fixRetries++;
        else
            m_errorRetries++;
        std::string retryPrompt = buildRetryPrompt(mode, code, err);
        agentLog(logTextLine(("input(retry/" + std::string(mode == RetryFix ? "fix" : "transient") + ")").c_str(), retryPrompt));
        m_agent->request(retryPrompt);
        };

    m_retryPending = true;
    if (m_retryDelayMs > 0)
        JZTimer::singleShot(m_retryDelayMs, this, fire);
    else
        fire();
}

// ---------------------------------------------------------------------------
// "多轮"由**外部驱动**, 不在这里再搞一个 --loop 模式:
//   一轮 = 一次 POST /task(prompt + verify_cmd), 走的就是上面 submitTask 那一套(同一会话、
//   同一"验证脚本门控"循环)。跑几轮 / 什么时候停, 是**提交方**的事 —— 验证程序在自己输出里
//   打 [loop-stop], 提交方看见后不再提交下一轮即可; CLI 不需要认识任何标记, 也不需要
//   第二个循环模式(它俩本来就是同一条链路的两种驱动方式, 分成两套只会各修一遍)。
//   落地见 binary/agent_cli/tools/agent_loop.py: 拉起 agent_cli(HTTP) + 逐轮提交 + 外部复验。
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 验证命令的解释器选择//   Windows(cmd)下 `python3` / `bash` 经常根本不存在 —— 装了 Python 往往只有 python.exe,
//   Git Bash 也没进 PATH。硬写 python3 会让验证脚本每一轮都以 9009/127 退出(看着像"测试
//   一直失败"), 白烧掉所有续跑轮次。所以推导时先挑一个"本机真能跑"的, 顺序:
//     ① 配置的程序映射(terminal.programs, 如 {"python":"C:/.../python.exe"})
//     ② 候选名逐个探测(`<name> --version` 退出码 0 算能跑)
//     ③ 都没有就退回默认名(保持旧行为: 至少错误信息会在验证输出里露出来)
//   探测结果进程内缓存(每条任务 spawn 一次探测进程没必要)。
// ---------------------------------------------------------------------------
static bool programRunnable(const std::string& name)
{
    static std::map<std::string, bool> cache;
    std::map<std::string, bool>::iterator it = cache.find(name);
    if (it != cache.end())
        return it->second;

#ifdef _WIN32
    std::string probe = "\"" + name + "\" --version > nul 2>&1";
    FILE* f = _popen(probe.c_str(), "r");
#else
    std::string probe = "\"" + name + "\" --version > /dev/null 2>&1";
    FILE* f = popen(probe.c_str(), "r");
#endif
    bool ok = false;
    if (f)
    {
        char buf[256];
        while (std::fgets(buf, sizeof(buf), f)) {}
#ifdef _WIN32
        ok = (_pclose(f) == 0);
#else
        ok = (pclose(f) == 0);
#endif
    }
    cache[name] = ok;
    return ok;
}

// 含空格的全路径要引号(cmd 会把 "C:/Program Files/..." 截在空格处)
static std::string quotePath(const std::string& p)
{
    if (p.find(' ') != std::string::npos && p.find('/') != std::string::npos)
        return "\"" + p + "\"";
    return p;
}

std::string JZAgentCliTask::resolveProgram(const std::vector<std::string>& cands, const std::string& fallback) const
{
    for (size_t i = 0; i < cands.size(); ++i)
    {
        const std::string& c = cands[i];
        // 全路径(含空格)整体当程序名; 否则按第一个空格拆掉参数("py -3" -> "py")
        std::string name = (c.find('/') != std::string::npos) ? c : c.substr(0, c.find(' '));

        std::map<std::string, std::string>::const_iterator it = m_config.programPaths.find(name);
        if (it != m_config.programPaths.end() && JZFileInfo::exists(it->second))
            return quotePath(it->second) + c.substr(name.size());   // 配置映射优先

        if (programRunnable(name))
            return (name == c) ? quotePath(c) : c;   // 带参数的候选原样返回
    }
    return fallback;
}

std::string JZAgentCliTask::deriveVerifyCmd(const std::string& script, const std::string& explicitCmd) const
{
    if (!explicitCmd.empty())
        return explicitCmd;      // 显式命令: 调用方自己给的, 原样用(包括它选定的解释器)
    if (script.empty())
        return "";
    size_t dot = script.find_last_of('.');
    std::string ext = (dot != std::string::npos) ? str_toLower(script.substr(dot + 1)) : "";
    std::string q = "\"" + script + "\"";   // 路径含空格时保险
    if (ext == "exe") return q;

    if (ext == "py")
        return resolveProgram({ "python3", "python", "py -3" }, "python3") + " " + q;

    // sh 及未知后缀按 shell 脚本处理; 终端配了 shell 就优先用它
    std::vector<std::string> shells;
    if (!m_config.shellProgram.empty())
        shells.push_back(m_config.shellProgram);
    shells.push_back("bash");
#ifdef _WIN32
    shells.push_back("C:/Program Files/Git/bin/bash.exe");
    shells.push_back("C:/Program Files/Git/usr/bin/bash.exe");
    shells.push_back("C:/Program Files (x86)/Git/bin/bash.exe");
#endif
    return resolveProgram(shells, "bash") + " " + q;
}

void JZAgentCliTask::runVerifyAsync(const std::string& cmd, int timeoutMs, const std::string& cwd,
    const std::function<void(int, const std::string&)>& cb)
{
    m_verifyCb = cb;
    m_verifyOut.clear();
    m_verifyTimedOut = false;
    const int gen = ++m_verifyGen;
    const int timeoutVal = timeoutMs;

    // 合并 stderr 到 stdout, 失败信息才能一并发给模型。
    // ⚠ 工作目录走 setWorkingDirectory, **不**拼 `cd "..." &&`: cmd.exe 对转义引号的
    //   解析跟 sh 完全不同(JZProcess 会给含空格的参数加引号), 拼进去轻则 cd 失败、
    //   重则整条命令被读错 —— 这也是旧 popen 那条链路的隐藏坑。
    // 解释器与旧 popen 保持一致: Windows = cmd.exe /c, POSIX = /bin/sh -c
    // (配了 terminal.shellProgram 就用它)。
    // ⚠ 这条只给 verify 脚本用: 终端里的命令走内建 JZShell, 跟这个配置无关。
    std::string run = cmd + " 2>&1";
    std::string shell;
    std::vector<std::string> args;
#ifdef _WIN32
    shell = m_config.shellProgram.empty() ? std::string("cmd.exe") : m_config.shellProgram;
    if (str_toLower(shell).find("cmd") == std::string::npos)
    {
        // 配了 bash 之类非 cmd 的 shell: 直接 `-c <命令>`(它们认 JZProcess 的 \" 转义)
        args.push_back("-c");
        args.push_back(run);
    }
    else
    {
        // ⭐ Windows 上要把命令**先写进临时 .cmd**, 再让 cmd.exe 只带一个脚本路径执行。
        //   直接 `cmd.exe /c "<含引号的命令>"` 会坏: JZProcess 按 argv 规则给整个参数加引号、
        //   并把内层 " 转义成 \" —— 而 **cmd.exe 不认 \"**, 内层引号会变成字面量。
        //   实测症状(agent_cli_test 的 hello_world 用例):
        //     python 收到带引号的路径 →
        //     can't open file 'C:\tmp\...\case_0\"C:\...\stdout_verify.py"' [Errno 22]
        //   (旧的 _popen 由 CRT 拼命令行, 所以没这个坑; 换成 JZProcess 后必须自己绕开。)
        std::string dir = tempDirPath();
        std::string path = dir + "/jz_verify_" + std::to_string(JZPlatform::getTickCountMs()) + "_" +
            std::to_string(gen) + ".cmd";
        JZFile script;
        std::string body = "@echo off\r\n" + run + "\r\n";
        if (script.open(path, "wb"))
        {
            script.write(body.data(), (int)body.size());
            script.flush();
            script.close();
            m_verifyScript = path;
            args.push_back("/c");
            args.push_back(path);   // 只有一个干净的路径参数(内层引号全在脚本内容里)
        }
        else
        {
            // 写不出临时脚本(权限/磁盘): 退回直传, 至少能跑无引号的命令
            jzWarning() << "验证命令的临时脚本写不出来, 退回直接 -c 传参:" << path;
            args.push_back("/c");
            args.push_back(run);
        }
    }
#else
    shell = m_config.shellProgram.empty() ? std::string("/bin/sh") : m_config.shellProgram;
    args.push_back("-c");
    args.push_back(run);   // POSIX 下 argv 是直传(没有 cmd 那套引号规则)
#endif

    JZProcess* proc = new JZProcess(this);
    proc->setWorkingDirectory(cwd);
    m_verifyProc = proc;
    m_verifyRunning = true;

    auto collect = [this, proc]()
        {
            m_verifyOut += bytesToText(proc->readAllStandardOutput());
            m_verifyOut += bytesToText(proc->readAllStandardError());
            // 累积上限: 验证输出几十 KB 足够定位问题, 别让它把内存/JSON 回包撑爆。
            // 按字节上限裁、落点对齐字符边界(UTF-8 安全), 免得把多字节字符切成半个。
            if (m_verifyOut.size() > kMaxVerifyBytes * 2)
                m_verifyOut = utf8CutAtByteLimit(m_verifyOut, kMaxVerifyBytes);
        };
    connect(&proc->sigReadyReadStandardOutput, collect);
    connect(&proc->sigReadyReadStandardError, collect);
    connect(&proc->sigFinished, [this, proc, gen, collect](int exitCode, JZProcess::ExitStatus)
        {
            if (gen != m_verifyGen)
                return;   // 上一轮验证的尾巴(已被 kill/replace), 丢掉
            collect();
            finishVerify(exitCode);
        });
    connect(&proc->sigErrorOccurred, [this, proc, gen](JZProcess::ProcessError)
        {
            if (gen != m_verifyGen)
                return;
            // 起不来(解释器/脚本不存在): 与 popen 失败同义 —— 报 -1 + 可读原因,
            // 让"验证命令本身跑不起来"这条信息照样能回到模型(见 onVerifyDone 的 127/9009 处理)
            m_verifyOut = "无法启动验证命令: " + m_verifyCmd;
            finishVerify(-1);
        });

    if (!proc->start(shell, args))
        return;   // sigErrorOccurred 已经走过 finishVerify(它会 deleteLater)

    // 超时: 到点连**整棵进程树**一起杀(JZProcess 的作业对象/进程组)。
    // ⚠ Windows 以前完全不包超时(旧实现是 #ifndef _WIN32 的 timeout 包装) ⇒ 验证命令
    //   不退出就永久卡死; 现在两个平台都走这条真超时。
    if (timeoutVal > 0)
    {
        JZTimer::singleShot(timeoutVal, [this, gen]()
            {
                if (gen != m_verifyGen || !m_verifyRunning)
                    return;
                m_verifyTimedOut = true;
                if (m_verifyProc)
                    m_verifyProc->killProcess();   // 同步收尾 -> finishVerify
            });
    }
}

void JZAgentCliTask::killVerify()
{
    if (!m_verifyRunning)
        return;
    // 主动停: 结果已经没人要了(任务被重置/收尾), 所以先把分发清掉再杀
    m_verifyCb = nullptr;
    m_verifyRunning = false;
    m_verifyGen++;   // 让在途的超时定时器与信号都作废
    JZProcess* proc = m_verifyProc;
    m_verifyProc = nullptr;
    if (proc)
    {
        proc->killProcess();   // 同步收尾: 此刻 finishVerify 会因为 m_verifyRunning=false 直接返回
        proc->deleteLater();
    }
    if (!m_verifyScript.empty())
    {
        JZFile::remove(m_verifyScript);
        m_verifyScript.clear();
    }
}

// 一次验证结束(自然退出 / 超时被杀 / 起不来): 截断输出 -> 回调

void JZAgentCliTask::finishVerify(int exitCode)
{
    if (!m_verifyRunning)
        return;
    m_verifyRunning = false;

    JZProcess* proc = m_verifyProc;
    m_verifyProc = nullptr;
    if (proc)
        proc->deleteLater();

    if (!m_verifyScript.empty())
    {
        JZFile::remove(m_verifyScript);   // Windows 的临时包装脚本: 用完就删
        m_verifyScript.clear();
    }

    std::string out = m_verifyOut;
    if (m_verifyTimedOut)
    {
        // 带个 ASCII 标记: 脚本/冒烟按它判定("验证命令超时"是中文, 日志编码不一定对得上)
        out += "\n...[verify-timeout] 验证命令超时(" + std::to_string(m_verifyTimeoutMs) +
            " ms), 进程已被强制终止";
        exitCode = -1;
    }
    if (out.size() > kMaxVerifyBytes)
    {
        size_t total = out.size();
        // 按字节上限截、落点对齐字符边界(UTF-8 安全, 不留半个中文字)
        out = utf8CutAtByteLimit(out, kMaxVerifyBytes) + "\n...(验证输出已截断, 共 " +
            std::to_string(total) + " 字节)";
    }

    std::function<void(int, const std::string&)> cb = m_verifyCb;
    m_verifyCb = nullptr;
    if (cb)
        cb(exitCode, out);   // 回调里可能再起一次验证 / 收尾, 放最后
}

// 喂回给模型的验证输出: 只留头尾(错误栈/断言通常在两端), 中间省略。
// 完整输出可能几十 KB, 整段贴回上下文很容易把 token 预算顶爆(顶爆只能靠压缩救, 成功率更低);
// 回包给界面/客户端的仍是完整输出(m_lastVerifyOutput)。
// ⚠ 按**字符**而非字节切(头尾各 4096 字符): 验证输出常是中文, 按字节切会把多字节字符切成半个。
static std::string clipForModel(const std::string& out)
{
    const size_t kHeadChars = 4096;
    const size_t kTailChars = 4096;
    size_t totalChars = utf8CountChars(out);   // 按"字符"而非字节判断长度, 更贴近直觉
    if (totalChars <= kHeadChars + kTailChars)
        return out;

    std::string head = utf8CutAtChar(out, kHeadChars);
    std::string tail = utf8TailAtChar(out, kTailChars);
    size_t omitted = totalChars - kHeadChars - kTailChars;
    return head + "\n...(中间省略 " + std::to_string(omitted) + " 字符, 需要时用工具自己去看)...\n" + tail;
}

void JZAgentCliTask::onLoopFinished()
{
    // 1) 无验证命令 = 无验证: 没有判定标准, 直接按"通过"收尾。
    //    agent 自身的报错只当提示带回去(网页显示成一行提示), 不把整个任务判成"失败"——
    //    任务本身没有可失败的验收项, 失败徽标只会误导(模型答完还在闲聊/轮次用尽很常见)。
    if (m_verifyCmd.empty())
    {
        respondTask("success", m_loopRound, m_accumReply, "", m_failed ? m_lastError : "");
        endLoop();
        return;
    }

    // 2) agent 这一轮自身报错(错误重试预算已用完 / 这条错误不值得重试): 验证循环救不了它 ——
    //    模型这会儿连请求都发不出去, 再喂回只是重复空转(白烧轮次)。但先给它一次机会:
    //    agent 之前几轮的改动可能已经让验证脚本通过了, 只是最后一条消息没取到 ——
    //    所以跑一次验证: 通过就算成功; 不通过就按 agent 错误收尾。
    //    (验证循环负责的是另一类失败: 模型能干活, 但结果不对。)
    if (m_failed)
    {
        runVerifyAsync(m_verifyCmd, m_verifyTimeoutMs, m_config.rootDir,
            [this](int code, const std::string& out)
            {
                if (!m_loopActive)
                    return;   // 验证途中任务被重置了
                if (code == 0)
                    respondTask("success", m_loopRound, m_accumReply, out, "");
                else
                    respondTask("failed", m_loopRound, m_accumReply, out,
                        m_lastError.empty() ? std::string("agent 执行失败")
                        : m_lastError);
                endLoop();
            });
        return;
    }

    // 3) 运行验证脚本(退出码 0 通过, 1 失败)。**异步**: 起进程后立刻返回, 结果经 onVerifyDone
    //    回到主线程 —— 期间事件循环照常转(SSE 心跳/网页请求/工具回调都不会被卡住)。
    runVerifyAsync(m_verifyCmd, m_verifyTimeoutMs, m_config.rootDir,
        [this](int code, const std::string& out) { onVerifyDone(code, out); });
}

// 验证结果的分发(正常路径): SSE 事件 -> 通过/失败 -> 超过轮次 -> 把输出喂回 agent 续跑

void JZAgentCliTask::onVerifyDone(int exitCode, const std::string& out)
{
    if (!m_loopActive)
        return;   // 验证途中任务被重置/取消了

    m_lastVerifyOutput = out;

    Json::Value ve;
    ve["round"] = m_loopRound;
    ve["exit_code"] = exitCode;
    ve["output"] = out;
    ve["passed"] = (exitCode == 0);
    ve["max_rounds"] = m_maxLoopRounds;
    pushEvent("verify", ve);

    if (exitCode == 0)
    {
        respondTask("success", m_loopRound, m_accumReply, out, "");
        endLoop();
        return;
    }

    // 4) 失败: 超过最大轮次则放弃
    m_loopRound++;
    if (m_loopRound >= m_maxLoopRounds)
    {
        respondTask("failed", m_loopRound, m_accumReply, out,
            "验证脚本在达到最大轮次(" + std::to_string(m_maxLoopRounds) + ")后仍失败");
        endLoop();
        return;
    }

    // 5) 把验证输出喂回 agent, 自动续跑
    //    先看看这一轮的失败是不是和上一轮一模一样(原地打转: 同样的改动再做一次还是会失败)
    std::string sig = std::to_string(exitCode) + "|" + out;
    if (sig == m_lastFeedbackSig)
        m_sameFailCount++;
    else
    {
        m_lastFeedbackSig = sig;
        m_sameFailCount = 1;
    }

    std::string feedback = "验证脚本执行失败(退出码 " + std::to_string(exitCode) + ")。"
        "其输出如下:\n```\n" + clipForModel(out) + "\n```\n"
        "请根据以上输出定位并修复问题, 然后我会重新运行验证脚本。只做必要的修改, 不要做无关改动。";
    // 9009(cmd: 不是内部或外部命令) / 127,126(sh: 命令未找到/不可执行):
    // 这是"验证命令本身跑不起来", 不是"测试没通过" —— 得让模型知道, 否则它会一直去改被测代码。
    if (exitCode == 9009 || exitCode == 127 || exitCode == 126)
        feedback += "\n注意: 这个退出码说明**验证命令本身**没找到或不能执行(不是测试没通过)。"
        "请先把这条命令依赖的程序/脚本准备好, 或换成一条确实能跑的命令。";
    if (m_sameFailCount >= 3)
        feedback += "\n注意: 你已经连续 " + std::to_string(m_sameFailCount) +
        " 次拿到完全相同的失败输出, 说明前面的做法没有生效。这一轮请先换做法: "
        "用 read_file 读真正相关的文件、先把问题复现出来再改, 不要重复上一轮已经做过的修改。";
    m_accumReply.clear();
    m_thinkLogged = false;
    m_failed = false;
    m_cancelled = false;
    m_lastError.clear();
    agentLog(logTextLine("input(continue)", feedback));
    m_agent->request(feedback);   // 重新驱动 agent; 结束后再次进入 onLoopFinished
}

void JZAgentCliTask::setupAgent()
{
    // 终端池: 一条任务一个池, 池里每台终端一次跑一条命令 —— 并发就多发几台
    m_termPool = new JZTerminalPool(8);
    m_termPool->setConfig(m_host.toolTerminalConfig());
    if (m_config.recycleSec > 0)
        m_termPool->setRecycleMs(m_config.recycleSec * 1000);

    auto addTool = [this](JZCodeTool *tool)
    {
        tool->setHost(&m_host);
        m_agent->addTool(tool);
        m_tools.push_back(tool);
    };

    if (m_config.allowTerminal)
    {
        // 终端是这台机器上唯一的执行面: ls/cat/mkdir/rm/cp/mv/grep 都是 shell 命令,
        // 由 terminal_run 覆盖。
        //   ⚠ 不再注册 `command`（JZShellTool，Windows 上是 cmd /c）：它和 terminal_run 是
        //   两套 shell 规则（cmd 认 dir/echo 却不认 bash 语法、也不认 tsc/code_runtime 短名）,
        //   模型在里面敲 `tsc` 只会得到 "'tsc' 不是内部或外部命令"。执行面统一收敛到 terminal_run。
        //   ⚠ 也不再注册 terminal_open/list/close：执行是无状态的(只有 cwd 跨命令保留),
        //   没有会话要开/查/关, 那两个工具只是让模型多绕一步。
        addTool(new JZTerminalRunTool(m_termPool));
        addTool(new JZTerminalCancelTool(m_termPool));
    }

    if (m_config.allowFileTools)
    {
        // 文件工具: 读带行号/分段、写整文件、局部精确替换(校验唯一匹配)、按名找文件、搜内容、时间。
        // 这些 shell 给不了(sed 不校验唯一匹配), 所以不走终端。
        addTool(new JZReadFileTool());
        addTool(new JZWriteFileTool());
        addTool(new JZModifyFileTool());
        addTool(new JZSearchFileTool());      // 按文件名/路径找文件(如 "*.d.ts")
        addTool(new JZSearchContextTool());   // 搜文本/正则(原 search_file, 已改名)
        addTool(new JZGetDateTimeTool());
    }

    if (m_config.allowNetworkTools)
    {
        // 联网工具: web_search(搜索) + web_fetch(抓正文)。两者共享同一个访问管理器,
        // 代理只在管理器上设一次(http / https 都生效)。
        // 没有宿主也能跑 —— 它们不出工作区, 也不需要 ICodeToolHost, 所以不参与区域审核。
        if (!m_netMan)
            m_netMan = new JZNetworkAccessManager(this);
        if (!m_config.proxyHost.empty())
        {
            JZProxyConfig pc;
            pc.host = m_config.proxyHost;
            pc.port = m_config.proxyPort;
            pc.user = m_config.proxyUser;
            pc.password = m_config.proxyPassword;
            m_netMan->setProxy(pc);
        }

        // web_search: 默认源是必应 RSS → HTML(直连可用, 无需配置);
        // 需要更高频/更稳时再换源(DeepSeek / 博查 / SerpAPI), 配 network.searchUrl 即可
        // —— 见 docs/web-search-design.md。
        auto *searchTool = new JZWebSearchTool();
        if (!m_config.webSearchUrl.empty())
            searchTool->setSearchUrlTemplate(m_config.webSearchUrl);
        searchTool->setNetworkAccessManager(m_netMan);
        addTool(searchTool);

        auto *fetchTool = new JZWebFetchTool();
        fetchTool->setNetworkAccessManager(m_netMan);
        addTool(fetchTool);
    }

    if (m_config.memoryEnabled)
    {
        // 长期记忆: 跨任务的结论沉淀 —— 开工时注入任务消息(见 buildMemoryBlock),
        // 任务里由模型自己用 memory append 写。文件路径由宿主给定(模型改不了), 所以
        // 这个工具不参与路径审核; 配到工作区外就等于授权它往那儿写。
        m_memory = new JZMemoryTool();
        m_memory->setup(memoryFilePath(), m_config.memoryMaxBytes);
        addTool(m_memory);
    }

    if (!m_config.nodes.empty())
    {
        // 远端执行: 多台 node 在模型侧只暴露**一个** remote_ssh 工具(按 host = 节点名 路由)。
        // 反过来"每台 node 一个 MCP server"会让模型看到 node1__run / node2__run 一堆名字、
        // 还要自己挑节点, 也不解决 session 误用 —— 路由收在 cli 侧最省心。
        std::string list;
        for (const JZCliNodeConfig &n : m_config.nodes)
        {
            list += "  - " + n.name + " (" + n.host + ":" + std::to_string(n.port) + ")\n";
        }

        // worker 不自己连 node: 借用 master 的那批连接(host 必填, 没有默认节点)
        auto *ssh = new JZRemoteSshTool(
            [this](const std::string &host) { return m_cli->nodeLink(host); }, list, this);
        m_agent->addTool(ssh);
        m_tools.push_back(ssh);
    }
}

// ---------------------------------------------------------------------------
// 远端节点(link 模式 agent_node)
// ---------------------------------------------------------------------------

string JZAgentCliTask::buildSystemPrompt() const
{
    string text;
    text += "You are an autonomous coding agent running on the user's machine, driven "
            "via an HTTP task interface (submit a task, optionally with a verify script "
            "that gates completion).\n";
    text += "Working directory: " + m_config.rootDir + "  —— 相对路径以它为基准。\n";
    text += "Available tools:\n";
    if (m_config.allowTerminal)
    {
        text += "  terminal_run    执行一条 shell 命令(整条交给 shell, 一次一条; 细则见下面的 Shell notes)。\n";
        text += "  terminal_cancel 停掉当前正在跑的那条命令(不需要参数)。\n";
    }
    if (m_config.allowFileTools)
    {
        text += "  read_file / write_file / modify_file   按行读(带行号)/整文件写/局部精确替换。\n";
        text += "  search_file    按文件名/路径找文件(通配符 * ?，如 \"*.d.ts\"、\"js_wrapper/*/types/*.d.ts\")，只回路径。\n";
        text += "  search_context 在工作区内搜索文本/正则(找某个串在哪些文件哪些行出现过)。\n";
        text += "  get_datetime   获取当前日期时间(需要\"今天\"这类信息时优先用)。\n";
    }
    if (m_config.allowNetworkTools)
    {
        text += "  web_search     联网搜索(给查询词, 回标题/链接/摘要): 查官方文档、API 用法、报错原因、"
                "第三方库这类需要实时信息的问题。\n";
        text += "  web_fetch      抓取某个 URL 的正文(转纯文本), 与 web_search 配合看某条结果的详情。\n";
    }
    if (m_config.memoryEnabled)
        text += "  memory         跨任务长期记忆: read / append / replace (" + memoryFilePath() + ")。\n";
    if (!m_config.nodes.empty())
    {
        text += "  remote_ssh     远端节点上的 terminal(host = 节点名, 必填):"
                " action=run 跑一条命令并同步回包; action=open/close/list 管理远端会话(session)。\n"
                "                 语义与本地 terminal **完全一致**(同一套 bash 语法 / 有状态 / 单前台 / 后台 `&`),"
                " 只是走网络; 远端改文件、拷贝文件也都靠它。\n"
                "                 细则同下面 Shell notes; 每台 node 的会话各自独立, 要并行就在同一节点上开多个 session。\n";
        for (const JZCliNodeConfig &nc : m_config.nodes)
            text += "    - node \"" + nc.name + "\"  " + nc.host + ":" + std::to_string(nc.port) + "\n";
    }
    text += "\nShell notes (important):\n";
    // "起服务再测"怎么跑必须说清楚: 模型容易给服务器那条一个很长的 timeout_ms 然后前台干等 ——
    // 那条要挂满超时才回(中途取不到输出), 而且超时那一刻服务就被杀掉了。正确做法是 background=true。
    text += jzTerminalRunNotes();
    text += "  - 优先用 read_file 而不是 cat, 用 modify_file 而不是 sed。\n";
    text += "  - 读写: 全盘可读, 只有工作区(读写执行)与临时目录可写; 破坏性命令(dd/sudo...)"
            "以及装/卸依赖(npm install 之类)被拒。\n";
    if (!m_config.programPaths.empty())
    {
        text += "\n本机程序(命令名 -> 全路径): 执行时会把命令里的命令名换成全路径, **只写命令名**(写全路径会被路径审核拒):\n";
        for (const auto &kv : m_config.programPaths)
            text += "  " + kv.first + "    " + kv.second + "\n";
    }
    // 配了 code_runtime = 本机就是 JZ 的 TS 运行时: 把它的模块/生命周期边界一并告诉模型。
    // 模型只会按 Node 的习惯写, 这些边界它猜不出来(猜错就是"编译过了但跑不起来")。
    if (m_config.programPaths.count("code_runtime"))
        text += "\n" + jzTsRuntimeNotes();
    if (m_config.memoryEnabled)
    {
        text += "\nMemory (跨任务的长期记忆):\n";
        text += "  - 每次任务的开头, 消息里可能带一段 <memory> —— 那是之前任务留下的结论, 先看再用。\n";
        text += "  - 得到值得跨任务复用的结论时, 用 memory 工具 append 记下来: 环境事实(构建/运行命令、"
                "程序路径、依赖版本)、踩过的坑、接口/目录约定、最终结论。一条一句, 写清\"是什么\"和"
                "\"为什么\"; 别记流水账(过程已经在对话历史里了)。\n";
        text += "  - 记忆可能过期: 与当前代码/文件冲突时以实际文件为准, 并把更正后的结论写回记忆"
                "(用 replace 精简, 别只往上堆)。\n";
    }
    // MCP 远端工具: 工具本身由 JZAgent 的 tools 参数带给模型, 这里只说清"名字带前缀的那些
    // 是什么、在哪执行"—— 配置里声明了但没连上时函数列表里就没有, 故措辞不写成"一定存在"。
    if (m_config.mcpEnabled && !m_config.mcpServers.empty())
    {
        text += "\nMCP remote tools:\n";
        text += "  本端还配置了若干个外部 MCP 服务器; 若函数列表里出现名字形如 <server>__<tool> 的工具,\n";
        text += "  那些就是它们提供的远端工具 —— 用法与本地工具完全一样, 但**在对方那台机器上执行**\n";
        text += "  (不经过本机的区域/路径审核), 出错时先看是不是那边的问题。配置的服务器:\n";
        for (const JZCliMcpServer &s : m_config.mcpServers)
            text += "    - " + (s.name.empty() ? s.transport : s.name) + "  (" + s.transport + ")\n";
    }
    // 宿主**常驻说明**(如"玩法任务书"): 只进 system prompt 一次 —— 之后每轮 /task 只叠加新消息,
    // 不用把同一段介绍重复贴一遍。extraFile(相对 rootDir)在前, 内联 extra 在后(可补充/覆盖)。
    string extra = m_config.systemExtra;
    if (!m_config.systemExtraFile.empty())
    {
        string p = m_config.systemExtraFile;
        if (!JZDir::isAbsolutePath(p))
            p = m_config.rootDir + "/" + p;
        p = codeNormalizePath(p);
        ifstream ifs(p, ios::binary);
        if (ifs)
        {
            stringstream ss;
            ss << ifs.rdbuf();
            extra = ss.str() + "\n" + extra;
        }
        else
            jzWarning() << "system.extraFile 读不到: " << p;
    }
    if (!extra.empty())
    {
        text += "\n===== 常驻说明（宿主给：本任务从头到尾都适用，优先级高于上面的通用说明）=====\n";
        text += extra;
        if (extra[extra.size() - 1] != '\n')
            text += "\n";
    }
    text += "\nWork through the user's request step by step using the tools. "
            "When done, give a concise summary of what you changed.\n";
    return text;
}

// ---------------------------------------------------------------------------
// 长期记忆
//   记忆是"跨任务"的: 进程内每个任务都会 reset 会话(提交前 m_agent->reset()),
//   所以结论必须落成文件才有意义 —— 落在工作区根目录, 换个人/换个 CLI 也能接着用。
// ---------------------------------------------------------------------------

string JZAgentCliTask::memoryFilePath() const
{
    string path = m_config.memoryFile.empty() ? string(".agent_memory.md") : m_config.memoryFile;
    if (!JZDir::isAbsolutePath(path))
        path = m_config.rootDir + "/" + path;
    return codeNormalizePath(path);
}

string JZAgentCliTask::buildMemoryBlock() const
{
    if (!m_memory || !m_memory->enabled())
        return string();

    int total = 0;
    bool cut = false;
    string mem = m_memory->readMemory(&total, &cut);
    if (str_trimmed(mem).empty())
        return string();

    // 注入时明确标出"这是记忆、可能过期": 记忆最危险的失败模式就是拿过期的旧结论
    // 覆盖当前代码 —— 所以每次都提醒它"以实际文件为准"。
    string s;
    s += "<memory file=\"" + m_memory->file() + "\" bytes=\"" + to_string(total) + "\"";
    if (cut)
        s += " truncated=\"true\"";
    s += ">\n";
    s += mem + "\n</memory>\n";
    s += "上面是之前任务留下的长期记忆(可能已经过期; 与当前代码/文件冲突时以实际文件为准)。"
         "本次如果得到值得跨任务复用的结论, 用 memory 工具 append 记下来。\n\n";
    return s;
}

// ==========================================================================
// HTTP 传输 + 验证脚本门控循环
void JZAgentCliTask::runTask(const Json::Value &req, std::function<void(const Json::Value &)> respond)
{
    if (m_busy)
    {
        // 队列内串行: 走到这里说明调度有 bug, 或者提交方绕过了 submitTask
        Json::Value e; e["error"] = "a task is already running";
        if (respond) respond(e);
        return;
    }
    // MCP 还没初始化完(有 server 没连出结果): 先拒绝 —— 任务一旦开跑, 稍后连上的远端工具
    // 就挂不进 agent 了(JZAgent 只允许在 Inactive 改工具表), 那还不如让提交方等几秒再来。
    if (!m_cli->m_mcpInitDone)
    {
        std::string waiting;
        for (const JZCliMcpState &s : m_cli->m_mcpStates)
        {
            if (s.ready || !s.error.empty())
                continue;   // 已就绪 / 已有结论(失败) 的都不算在等
            if (!waiting.empty())
                waiting += ", ";
            waiting += s.name;
        }
        Json::Value e;
        e["error"] = "MCP 初始化中("
                     + (waiting.empty() ? (to_string(m_cli->m_mcpPending) + " 个连接") : waiting)
                     + "), 稍后再提交 —— 连不上也没关系, 全部有结果后就会放行";
        e["mcp_pending"] = m_cli->m_mcpPending;
        if (respond) respond(e);
        return;
    }
    std::string imgError;
    std::vector<std::string> images = extractImages(req, imgError);
    if (!imgError.empty())
    {
        Json::Value e; e["error"] = imgError;
        if (respond) respond(e);
        return;
    }
    std::string prompt = extractUserText(req);
    // 文字与图片都空才算空任务(允许"只发截图", 与 code_agent 的 request() 同一口径)
    if (prompt.empty() && images.empty())
    {
        Json::Value e; e["error"] = "no prompt found (need 'prompt' / 'messages', or 'images')";
        if (respond) respond(e);
        return;
    }

    // 长期记忆: 开工时把记忆注入任务消息(不是 system prompt —— 那个是进程级的, 改一次
    // 就把 KV 前缀缓存整段作废)。use_memory=false 可单次关掉(评测/一次性任务用)。
    std::string memoryBlock;
    if (req.isMember("use_memory") ? req["use_memory"].asBool() : true)
        memoryBlock = buildMemoryBlock();

    std::string script = req.isMember("verify_script") ? req["verify_script"].asString() : "";
    std::string explicitCmd = req.isMember("verify_cmd") ? req["verify_cmd"].asString() : "";
    m_verifyCmd = deriveVerifyCmd(script, explicitCmd);
    // 默认值来自配置(verify.maxRounds / verify.timeoutMs), 本次请求可覆盖
    m_verifyTimeoutMs = req.isMember("verify_timeout") ? req["verify_timeout"].asInt() : m_config.verifyTimeoutMs;
    m_maxLoopRounds = req.isMember("max_verify_rounds") ? req["max_verify_rounds"].asInt() : m_config.verifyMaxRounds;
    if (m_verifyTimeoutMs <= 0) m_verifyTimeoutMs = 60000;
    if (m_maxLoopRounds <= 0) m_maxLoopRounds = 30;

    // 出错自动重试: 默认取配置(llm.maxRetries / llm.maxFixRetries / llm.retryDelayMs), 本次请求可覆盖
    m_maxErrorRetries = req.isMember("max_retries") ? req["max_retries"].asInt() : m_config.maxRetries;
    m_maxFixRetries = req.isMember("max_fix_retries") ? req["max_fix_retries"].asInt() : m_config.maxFixRetries;
    m_retryDelayMs = req.isMember("retry_delay_ms") ? req["retry_delay_ms"].asInt() : m_config.retryDelayMs;
    if (m_maxErrorRetries < 0) m_maxErrorRetries = 0;
    if (m_maxFixRetries < 0) m_maxFixRetries = 0;
    if (m_retryDelayMs < 0) m_retryDelayMs = 0;

    m_respond = std::move(respond);
    m_loopActive = true;
    m_loopRound = 0;
    m_accumReply.clear();
    m_thinkLogged = false;
    m_lastVerifyOutput.clear();
    m_currentPrompt = prompt;          // 记录 prompt, 供连接快照
    m_accumThink.clear();
    m_thinkLogged = false;
    m_toolSnaps.clear();
    m_busy = true;
    // 忙闲由 master 在 dispatchTo 里按全部队列汇总上报, 不能用 q0 的局部状态。
    m_failed = false;
    m_cancelled = false;
    m_lastError.clear();
    m_lastErrorCode = JZAgent::None;
    m_errorRetries = 0;
    m_fixRetries = 0;
    m_retryPending = false;
    m_lastFeedbackSig.clear();
    m_sameFailCount = 0;
    m_loopEpoch++;          // 新任务: 作废上一个任务残留的延时重试
    m_dropHistorySeq = 0;   // 上一个任务被删过, 不能连累这一条的回填

    // 续会话: 上游(server)把这条会话的对话快照带在 req.context 里 —— 没有就是新会话。
    // 任务对象是一次性的(每任务新建), 所以历史必须显式灌进来, 不能指望对象里残留上一轮。
    if (req.isMember("context") && req["context"].isObject())
    {
        LLMContext ctx;
        if (ctx.fromJson(req["context"]))
        {
            m_agent->loadContext(ctx);
            agentLog("续会话: 载入历史 " + std::to_string(ctx.history.size()) + " 条");
        }
    }

    Json::Value start;
    start["verify_cmd"] = m_verifyCmd;
    start["max_rounds"] = m_maxLoopRounds;
    start["max_retries"] = m_maxErrorRetries;
    start["max_fix_retries"] = m_maxFixRetries;
    start["memory_bytes"] = (int)memoryBlock.size();      // 0 = 没带记忆(未启用/还没记忆/本次关掉)
    start["images"] = (int)images.size();                 // 附图张数(网页/脚本据此提示; 图本身不进事件流)
    pushEvent("task_start", start);
    pushHistory(prompt, m_verifyCmd);   // 记一条"最近任务"(running), 收尾时回填结果

    // 多模态: 图片与文字同属这一条 user 消息(走 JZAgent 的 request(text, images) 重载, 与桌面版同一条路)。
    // 后续续跑/验证失败的喂回仍是纯文本 —— 图只在首轮给一次。
    agentLog(logTextLine("input", memoryBlock + prompt));
    if (images.empty())
        m_agent->request(memoryBlock + prompt);
    else
        m_agent->request(memoryBlock + prompt, images);
    m_sessionStarted = true;
    // 注意: 不在此回包; onLoopFinished 在 agent 结束后被调用并回包(经 m_respond)。
}

// ---------------------------------------------------------------------------
// 最近任务历史
//   长程任务跑几十分钟很常见, 网页不可能一直开着 —— 服务端记住最近若干次请求与它们的结果,
//   新连接(SSE hello 帧)或 GET /info 时以 recent 数组吐出去, 界面据此把历史卡片补回来。
//   只存"摘要"(请求 / 结论 / 最后一次验证输出), 不存逐轮过程 —— 那是流式事件该干的事。
// ---------------------------------------------------------------------------

int64_t JZAgentCliTask::nowMs()
{
    return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void JZAgentCliTask::pushHistory(const std::string &prompt, const std::string &verifyCmd)
{
    JZCliHistoryItem it;
    it.seq = ++m_historySeq;
    it.prompt = prompt;
    it.verifyCmd = verifyCmd;
    it.status = "running";
    it.startMs = nowMs();
    m_history.push_back(it);

    while ((int)m_history.size() > kHistoryMax)
        m_history.erase(m_history.begin());   // 最旧的先走
}

void JZAgentCliTask::finishHistory(const std::string &status, int rounds, const std::string &reply,
                               const std::string &verifyOut, const std::string &err)
{
    if (m_dropHistorySeq)   // 这条记录已被网页删掉: 收尾不再补写
    {
        m_dropHistorySeq = 0;
        return;
    }

    // 回填"最后一条还没收尾的"(正常就是刚跑完这条); 极端情况(如重置时才回包)没有就补一条
    for (int i = (int)m_history.size() - 1; i >= 0; --i)
    {
        if (m_history[i].status != "running")
            continue;
        JZCliHistoryItem &it = m_history[i];
        it.status = status;
        it.rounds = rounds;
        it.reply = reply;
        it.verifyOut = verifyOut;
        it.error = err;
        it.endMs = nowMs();
        return;
    }

    pushHistory(m_currentPrompt, m_verifyCmd);
    m_history.back().status = status;
    m_history.back().rounds = rounds;
    m_history.back().reply = reply;
    m_history.back().verifyOut = verifyOut;
    m_history.back().error = err;
    m_history.back().endMs = nowMs();
}

bool JZAgentCliTask::removeHistory(int seq)
{
    for (auto it = m_history.begin(); it != m_history.end(); ++it)
    {
        if (it->seq != seq)
            continue;
        if (it->status == "running")
            m_dropHistorySeq = it->seq;   // 还在跑: 收尾时别再把它写回来
        m_history.erase(it);
        return true;
    }
    return false;
}

bool JZAgentCliTask::dropRunningHistory()
{
    for (auto it = m_history.rbegin(); it != m_history.rend(); ++it)
        if (it->status == "running")
            return removeHistory(it->seq);
    return false;
}

Json::Value JZAgentCliTask::recentJson() const
{
    Json::Value arr(Json::arrayValue);
    for (auto it = m_history.rbegin(); it != m_history.rend(); ++it)
    {
        Json::Value j;
        j["seq"] = it->seq;
        j["prompt"] = it->prompt;
        j["verify_cmd"] = it->verifyCmd;
        j["status"] = it->status;
        j["reply"] = it->reply;
        j["verify_output"] = it->verifyOut;
        j["error"] = it->error;
        j["rounds"] = it->rounds;
        j["start_ms"] = (Json::Int64)it->startMs;
        j["end_ms"] = (Json::Int64)it->endMs;
        arr.append(j);
    }
    return arr;
}

void JZAgentCliTask::respondTask(const std::string &status, int rounds, const std::string &reply,
                             const std::string &verifyOut, const std::string &err)
{
    Json::Value r;
    r["status"] = status;
    r["verify_rounds"] = rounds;
    r["retries"] = m_errorRetries + m_fixRetries;   // 本次任务用掉的重试次数(两档合计)
    r["retries_transient"] = m_errorRetries;        // 其中"继续"档
    r["retries_fix"] = m_fixRetries;                // 其中"改一版"档
    r["agent_reply"] = reply;
    r["last_verify_output"] = verifyOut;
    // 本任务累计 token 用量随包带出: 任务对象是一次性的, 结束后 agent 就没了 ——
    // 统计方(评测/网页)读这个字段即可。agent 走本类已有的 agent() 访问器。
    JZAgent *a = agent();
    if (a && a->context())
    {
        const auto &u = a->context()->usage;
        Json::Value usage(Json::objectValue);
        usage["prompt_tokens"] = u.prompt_tokens;
        usage["completion_tokens"] = u.completion_tokens;
        usage["total_tokens"] = u.total_tokens;
        usage["cached_tokens"] = u.cached_tokens;
        usage["prompt_ms"] = u.prompt_ms;
        usage["pred_ms"] = u.predicted_ms;
        usage["draft_tokens"] = u.draft_tokens;
        usage["draft_accepted_tokens"] = u.draft_accepted_tokens;
        // 与"计费总量"不同口径: windowTokens() = 最后一轮实际会发出的上下文
        usage["context_tokens"] = a->windowTokens();
        r["usage"] = usage;
    }
    if (!err.empty())
        r["error"] = err;
    // 先推事件再回包: 提交方可能已经断开(见 onClientDestroyed), 那样只有 SSE 订阅者
    // (重连上来的网页)还能看到这次收尾 —— 它是那时唯一的结果出口。
    pushEvent("task_end", r);
    m_lastResult = r;              // 留一份: 断连期间跑完的任务, 重连时靠 hello 帧补给界面
    m_hasLastResult = true;
    finishHistory(status, rounds, reply, verifyOut, err);   // 归档进"最近任务"

    // 回包带上这一轮的对话快照: 只走回包, **不进 task_end 事件流** ——
    // 事件会被记进 tasks.jsonl(有 200 条上限), 塞快照就把过程挤没了。
    if (m_agent)
    {
        LLMContext ctx = m_agent->saveContext();
        if (!ctx.history.empty())
            r["context"] = ctx.toJson();
    }

    if (!m_respond)   // 客户端已断开 / 不应回包
        return;
    auto cb = std::move(m_respond);
    cb(r);
}

void JZAgentCliTask::resetHistory()
{
    m_history.clear();
    m_historySeq = 0;
    m_hasLastResult = false;
    m_lastResult = Json::nullValue;
}

void JZAgentCliTask::onClientDestroyed(JZObject *obj)
{
    // 提交方(POST /task 的那条连接)在任务跑完前断开: 只是"没人收回包"了, 任务继续跑完。
    //   典型场景是网页刷新/关页面 —— 浏览器会 abort 掉那条 /task 请求, 这种断连不该把在跑的
    //   任务杀掉(改了一半的工作就丢了)。结果仍会经 SSE 推给在线订阅者(task_end / verify ...),
    //   网页重连时也能用 state 快照接着看进行中的任务。
    //   ⚠ 所以这里只丢回包通道(避免写野指针), 刻意不动 m_loopActive / 不 stop agent。
    //   真要中止任务走 POST /reset(那是用户明确的操作)。
    if (obj != m_cli->m_pendingClient)
        return;   // 不是当前在途任务的那条连接(上一个任务早已回包, 连接延后析构): 与本任务无关

    m_cli->m_pendingClient = nullptr;
    m_respond = nullptr;   // 断开即不再回包; 任务本身照跑, 结束由 endLoop() 复位状态
}

string JZAgentCliTask::extractUserText(const Json::Value &params)
{
    const Json::Value &msgs = params["messages"];
    if (msgs.isArray())
    {
        string out;
        for (Json::ArrayIndex i = 0; i < msgs.size(); ++i)
        {
            const Json::Value &m = msgs[i];
            string role = m.get("role", "").asString();
            const Json::Value &c = m["content"];
            if (role != "user")
                continue;
            if (c.isString())
            {
                out += c.asString();
            }
            else if (c.isArray())
            {
                for (Json::ArrayIndex j = 0; j < c.size(); ++j)
                {
                    const Json::Value &p = c[j];
                    if (p.isObject() && p["type"].asString() == "text")
                        out += p["text"].asString();
                    else if (p.isString())
                        out += p.asString();
                }
            }
        }
        if (!out.empty())
            return out;
    }
    // 退化: 直接给 prompt / text 字段
    if (params.isMember("prompt") && params["prompt"].isString())
        return params["prompt"].asString();
    if (params.isMember("text") && params["text"].isString())
        return params["text"].asString();
    return string();
}

// 收集图片附件(多模态)。两种写法都收 —— 与 OpenAI 的 messages 一致, 网页/脚本可直接搬:
//   "images": ["data:image/png;base64,...", "http://host/a.png"]     (单张也允许直接给字符串)
//   "messages": [{role:"user", content:[{type:"text"},{type:"image_url",image_url:{url}}]}]
// 只认 data: URL 与 http(s) 链接(别的形态喂给模型只会拿到 400)。
// ⚠ 上限是硬约束(kMaxTaskImages / kMaxImageChars): 超了就报错, 不静默截断 —— 否则
//   提交方以为图发过去了、模型却看不到, 排查起来很费劲。

std::vector<std::string> JZAgentCliTask::extractImages(const Json::Value &params, string &error)
{
    std::vector<std::string> out;
    string bad;
    size_t total = 0;

    auto add = [&](const Json::Value &v) {
        if (!v.isString())
            return;
        string url = v.asString();
        if (url.empty())
            return;
        if ((int)url.size() > kMaxImageChars)
        {
            if (bad.empty()) bad = "单张图片过大(超过 " + to_string(kMaxImageChars) + " 字符)";
            return;
        }
        if (total + url.size() > (size_t)kMaxImagesTotalChars)
        {
            if (bad.empty()) bad = "图片合计过大(超过 " + to_string(kMaxImagesTotalChars) + " 字符)";
            return;
        }
        string head = str_toLower(url.substr(0, 5));
        if (head.compare(0, 5, "data:") != 0 && head.compare(0, 4, "http") != 0)
        {
            if (bad.empty()) bad = "images 只接受 data: URL 或 http(s) 链接";
            return;
        }
        if ((int)out.size() >= kMaxTaskImages)
        {
            if (bad.empty()) bad = "图片数量过多(超过 " + to_string(kMaxTaskImages) + " 张)";
            return;
        }
        total += url.size();
        out.push_back(url);
    };

    const Json::Value &images = params["images"];
    if (images.isArray())
    {
        for (Json::ArrayIndex i = 0; i < images.size(); ++i)
            add(images[i]);
    }
    else if (images.isString())
    {
        add(images);
    }

    // messages 写法: 只取 user 消息 content 数组里的 image_url 部分(与 extractUserText 同一套遍历)
    const Json::Value &msgs = params["messages"];
    if (msgs.isArray())
    {
        for (Json::ArrayIndex i = 0; i < msgs.size(); ++i)
        {
            const Json::Value &m = msgs[i];
            if (m.get("role", "").asString() != "user")
                continue;
            const Json::Value &c = m["content"];
            if (!c.isArray())
                continue;
            for (Json::ArrayIndex j = 0; j < c.size(); ++j)
            {
                const Json::Value &p = c[j];
                if (!p.isObject() || p["type"].asString() != "image_url")
                    continue;
                const Json::Value &iu = p["image_url"];
                if (iu.isObject())
                    add(iu.get("url", ""));
                else if (iu.isString())
                    add(iu);
            }
        }
    }

    // 一张都没收进来但确实有毛病 -> 报错; 收了部分(超数量)也报错, 让提交方知道被截断了
    if (!bad.empty())
        error = bad;
    return out;
}

JZAgentCliTask::RetryMode JZAgentCliTask::classifyError(JZAgent::AgentError code, const std::string &err)
{
    switch (code)
    {
        case JZAgent::ConnectError:
        case JZAgent::TimeOutError:
        case JZAgent::NoError:          // 内部/解析类错误: 多半是瞬时协议问题
        case JZAgent::ToolCallError:
            return RetryTransient;
        case JZAgent::HttpError:
        {
            int mode = httpErrorMode(err);
            if (mode == 3)
                return RetryNone;          // 4xx: 请求本身有问题, 重试只会重复同一条错误
            return (mode == 2) ? RetryFix : RetryTransient;
        }
        case JZAgent::MaxRoundError:
            // 轮次用尽只是"这一轮预算跑完": llmEnd() 已把轮次计数清零, 发条"继续"就能接着干
            // (总量仍受重试次数约束, 不会无限跑)。
            return RetryTransient;
        case JZAgent::MaxTokenError:
            // 上下文撑爆: 原样重发还是超, 但"让模型收着点"(别再整段贴文件/贴输出)常能救回来。
            return RetryFix;
        case JZAgent::LoopError:
            // 在原地打转(工具反复相同调用 / 思考回复复读), 已被循环检测中止: 原样重发
            // 必然接着打转, 但"换做法"通常能解 —— 归可修复(预算受 maxFixRetries 约束)。
            return RetryFix;
        default:                        // None / UserCancelError
            return RetryNone;
    }
}

void JZAgentCliTask::onError(const std::string &err)
{
    m_failed = true;
    m_lastError = err;
    m_lastErrorCode = m_agent ? m_agent->errorCode() : JZAgent::NoError;
    agentLog("error: " + logSummary(err, kToolResultBytes));

    RetryMode mode = classifyError(m_lastErrorCode, err);
    bool retryable = (mode == RetryTransient) ? (m_errorRetries < m_maxErrorRetries)
                   : (mode == RetryFix)       ? (m_fixRetries < m_maxFixRetries)
                                              : false;

    // sigError 实时推给网页: 任务中途出错(LLM 失败/拒答/取消等)不再只能等最后回包
    Json::Value d;
    d["text"] = err;
    d["round"] = m_loopRound;
    d["loop_active"] = m_loopActive;
    d["code"] = (int)m_lastErrorCode;    // 错误分类, 见 JZAgent::AgentError
    d["retry"] = (mode == RetryFix) ? m_fixRetries : m_errorRetries;   // 该档已用掉的重试次数
    d["retry_mode"] = (mode == RetryFix) ? "fix" : (mode == RetryTransient ? "continue" : "none");
    d["retryable"] = retryable;          // 还会自动重试吗(网页据此提示"将自动重试")
    pushEvent("error", d);
}

void JZAgentCliTask::onFinished()
{
    // 任务被取消 / 客户端断开时不跑验证
    if (!m_loopActive)
        return;

    // 本轮 LLM 生成结束: 记 think / reply 摘要(一条一条; think 若已写则不重复)
    if (!m_thinkLogged && !m_accumThink.empty())
    {
        agentLog(logTextLine("think", m_accumThink));
        m_thinkLogged = true;
    }
    if (!m_accumReply.empty())
        agentLog(logTextLine("reply", m_accumReply));

    // agent 报错: 在(分档的)重试预算内"接着发继续任务"/"让它改一版", 而不是直接判失败
    // (预算用完才走原流程: 有验证脚本就交给验证循环处理)
    if (m_failed)
    {
        RetryMode mode = classifyError(m_lastErrorCode, m_lastError);
        if ((mode == RetryTransient && m_errorRetries < m_maxErrorRetries)
            || (mode == RetryFix && m_fixRetries < m_maxFixRetries))
        {
            scheduleErrorRetry(mode);
            return;
        }
    }

    // agent 每结束一轮就跑验证, 失败则把输出喂回 agent 自动续跑
    onLoopFinished();
}
