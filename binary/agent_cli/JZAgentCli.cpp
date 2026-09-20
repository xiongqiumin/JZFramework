#include "JZAgentCli.h"

#include "JZByteArray.h"
#include "JZDebug.h"
#include "JZDir.h"
#include "JZFile.h"
#include "JZFileInfo.h"
#include "JZPlatform.h"
#include "JZString.h"        // str_toLower
#include "JZVirtualTerminal.h"
#include "json/json.h"

#include "JZHttpServer.h"     // HTTP 服务(路由 / JZHttpServerClient / JZHttpRespone)
#include "JZHttpHandler.h"
#include "JZHttpStaticFiles.h"

#include <cstdio>
#ifndef _WIN32
#include <sys/wait.h>         // WIFEXITED / WEXITSTATUS(popen 取退出码)
#endif

#include "JZTerminalTools.h"  // 本地终端工具(JZTerminalRunTool 等)

#include <iostream>
#include <thread>

using namespace std;

// 单次读文件上限(与 node 一致): 避免把几百兆读进内存。
static const int kReadFileLimit = 8 * 1024 * 1024;

// ==========================================================================
// JZCliToolHost
// ==========================================================================

JZCliToolHost::JZCliToolHost(const JZCliConfig *config)
    : m_config(config)
{
}

void JZCliToolHost::refresh()
{
    m_terminalConfig.zone.workspace = m_config ? m_config->rootDir : string();
    m_terminalConfig.zone.trustedDirs = m_config ? m_config->trustedDirs : vector<TrustedDir>();
    if (m_config)
    {
        m_terminalConfig.level = m_config->unrestricted ? AccessLevel::Unrestricted
                                                        : AccessLevel::Standard;
        m_terminalConfig.allowPackageManager = m_config->allowPackageManager;
        m_terminalConfig.allowNetwork = m_config->allowNetwork;
    }
}

bool JZCliToolHost::toolReadFile(const std::string &absPath, std::string &text, std::string &error)
{
    JZFileInfo info(absPath);
    if (!info.exists())
    {
        error = "文件不存在: " + absPath;
        return false;
    }
    if (info.isDir())
    {
        error = "这是一个目录(用 list_dir): " + absPath;
        return false;
    }
    if (info.size() > kReadFileLimit)
    {
        error = "文件过大(>8MB), 读不了: " + absPath + "  可以改用终端里的命令按需处理";
        return false;
    }

    JZFile file;
    if (!file.open(absPath, "rb"))
    {
        error = "打开文件失败(权限?): " + absPath;
        return false;
    }
    JZByteArray data = file.readAll();
    file.close();

    text.assign(data.data(), data.size());
    return true;
}

// ==========================================================================
// JZAgentCli
// ==========================================================================

JZAgentCli::JZAgentCli(JZObject *parent)
    : JZObject(parent)
    , m_host(&m_config)
{
}

JZAgentCli::~JZAgentCli()
{
}

bool JZAgentCli::init(const JZCliConfig &config, std::string &error)
{
    m_config = config;

    if (m_config.rootDir.empty())
        m_config.rootDir = JZDir::currentPath();
    m_config.rootDir = codeNormalizePath(m_config.rootDir);
    if (m_config.rootDir.empty())
        m_config.rootDir = codeNormalizePath(JZDir::currentPath());
    for (TrustedDir &dir : m_config.trustedDirs)
        dir.path = codeNormalizePath(dir.path);

    m_host.refresh();

    m_agent = new JZAgent(this);
    m_agent->setStreamEnabled(true);
    m_agent->setThinkEnabled(false);

    JZAgentConfig ac;
    ac.rootPath = m_config.rootDir;
    ac.modelUrl = m_config.modelUrl;
    ac.modelName = m_config.modelName;
    ac.modelToken = m_config.modelToken;
    ac.netTimeout = m_config.netTimeout;
    ac.maxRounds = m_config.maxRounds;
    ac.maxToken = m_config.maxToken;
    m_agent->init(ac);

    connect(&m_agent->sigReply, this, &JZAgentCli::onReply);
    connect(&m_agent->sigError, this, &JZAgentCli::onError);
    connect(&m_agent->sigFinished, this, &JZAgentCli::onFinished);

    setupAgent();
    m_agent->setSystemPrompt(buildSystemPrompt());
    return true;
}

void JZAgentCli::setupAgent()
{
    m_terminal = new JZVirtualTerminal(this);
    m_terminal->setId("default");
    m_terminal->setTerminalConfig(m_host.toolTerminalConfig());
    if (!m_config.shellProgram.empty())
        m_terminal->setShellProgram(m_config.shellProgram);
    if (m_config.recycleSec > 0)
        m_terminal->setRecycleMs(m_config.recycleSec * 1000);

    auto addTool = [this](JZCodeTool *tool)
    {
        tool->setHost(&m_host);
        m_agent->addTool(tool);
        m_tools.push_back(tool);
    };

    if (m_config.allowTerminal)
    {
        // 终端是这台机器上唯一的执行面: ls/cat/mkdir/rm/cp/mv/grep 都是 shell 命令,
        // 由 terminal_run 覆盖; 这里只挂终端会话管理 + 一个通用 shell 工具。
        addTool(new JZTerminalRunTool(m_terminal));
        addTool(new JZTerminalOpenTool(m_terminal));
        addTool(new JZTerminalListTool(m_terminal));
        addTool(new JZTerminalCloseTool(m_terminal));
        addTool(new JZShellTool());
    }

    if (m_config.allowFileTools)
    {
        // 文件工具: 读带行号/分段、写整文件、局部精确替换(校验唯一匹配)、搜索、时间。
        // 这些 shell 给不了(sed 不校验唯一匹配), 所以不走终端。
        addTool(new JZReadFileTool());
        addTool(new JZWriteFileTool());
        addTool(new JZModifyFileTool());
        addTool(new JZSearchFileTool());
        addTool(new JZGetDateTimeTool());
    }
}

string JZAgentCli::buildSystemPrompt() const
{
    string text;
    text += "You are an autonomous coding agent running on the user's machine, driven "
            "via an HTTP task interface (submit a task, optionally with a verify script "
            "that gates completion).\n";
    text += "Working directory: " + m_config.rootDir + "  —— 相对路径以它为基准。\n";
    text += "Available tools:\n";
    if (m_config.allowTerminal)
    {
        text += "  terminal_run    执行一条 shell 命令(命令整条交给 shell: 管道/重定向/串联/通配符/变量都生效)。\n";
        text += "  terminal_open / terminal_list / terminal_close   多会话管理(单终端场景下可不用)。\n";
        text += "  shell           在工作区内跑一条外部命令(带超时)。\n";
    }
    if (m_config.allowFileTools)
    {
        text += "  read_file / write_file / modify_file   按行读(带行号)/整文件写/局部精确替换。\n";
        text += "  search_code    在工作区内搜索文本/正则。\n";
        text += "  get_datetime   获取当前日期时间(需要\"今天\"这类信息时优先用)。\n";
    }
    text += "\nShell notes (important):\n";
    text += "  - 每条命令都是一个新 shell: 要进目录执行写 `cd <dir> && <cmd>`; "
            "单独的 `cd` 会被拒绝。cd/export 不跨命令保留。\n";
    text += "  - 不支持后台 `&`、heredoc、进程替换、for/while/if 复合语句。\n";
    text += "  - 优先用 read_file 而不是 cat, 用 modify_file 而不是 sed。\n";
    text += "  - 写权限: 工作区(读写执行)/授信目录(默认只读)/区外只读; 破坏性命令(dd/sudo...)被拒。\n";
    text += "\nWork through the user's request step by step using the tools. "
            "When done, give a concise summary of what you changed.\n";
    return text;
}

// ==========================================================================
// HTTP 传输 + 验证脚本门控循环
// ==========================================================================

// 路由处理器: 把 /task 请求转交给 JZAgentCli::onTaskSubmit
class TaskHandler : public JZHttpHandler
{
public:
    explicit TaskHandler(JZAgentCli *cli) : m_cli(cli) {}
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        m_cli->onTaskSubmit(client, req);
    }
private:
    JZAgentCli *m_cli;
};

// 路由处理器: 列出工作区内可当验证脚本的文件, 供网页下拉选择
class ScriptsHandler : public JZHttpHandler
{
public:
    explicit ScriptsHandler(JZAgentCli *cli) : m_cli(cli) {}
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        m_cli->onListScripts(client, req);
    }
private:
    JZAgentCli *m_cli;
};

// 路由处理器: 实时事件流(GET /events, SSE)
class EventsHandler : public JZHttpHandler
{
public:
    explicit EventsHandler(JZAgentCli *cli) : m_cli(cli) {}
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        m_cli->onEvents(client, req);
    }
private:
    JZAgentCli *m_cli;
};

// 路由处理器: 运行信息(GET /info)
class InfoHandler : public JZHttpHandler
{
public:
    explicit InfoHandler(JZAgentCli *cli) : m_cli(cli) {}
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        m_cli->onInfo(client, req);
    }
private:
    JZAgentCli *m_cli;
};

// 路由处理器: 重置会话(POST /reset)
class ResetHandler : public JZHttpHandler
{
public:
    explicit ResetHandler(JZAgentCli *cli) : m_cli(cli) {}
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        m_cli->onReset(client, req);
    }
private:
    JZAgentCli *m_cli;
};

void JZAgentCli::startHttp()
{
    m_http = new JZHttpServer(this);

    // 网页控制台(聊天式 UI): 从 httpDir 提供 index.html / app.js / style.css
    std::string httpDir = m_config.httpDir;
    if (httpDir.empty())
        httpDir = JZPlatform::applicationDirPath() + "/http";
    auto *staticFiles = new JZHttpStaticFiles(httpDir);
    m_http->addRoute("/", staticFiles);
    m_http->addRoute("/<path:path>", staticFiles);

    // 供网页下拉选择验证脚本
    m_http->addRoute("/scripts", new ScriptsHandler(this));

    // 网页顶栏显示: 模型地址 / 模型名 / 工作区
    m_http->addRoute("/info", new InfoHandler(this));

    m_http->addRoute("/task", new TaskHandler(this));
    m_http->addRoute("/reset", new ResetHandler(this));
    m_http->addRoute("/events", new EventsHandler(this));
    if (!m_http->listen(m_config.httpBind, m_config.httpPort))
        jzWarning() << "HttpServer 监听失败:" << m_config.httpBind << ":" << m_config.httpPort;
}

Json::Value JZAgentCli::makeInfo() const
{
    Json::Value j;
    j["agent"] = m_config.agentName;
    j["version"] = m_config.version;
    j["modelUrl"] = m_config.modelUrl;
    j["modelName"] = m_config.modelName;
    j["rootDir"] = m_config.rootDir;
    return j;   // 注意: 不含 modelToken, 别把凭据吐给网页
}

void JZAgentCli::onInfo(JZHttpServerClient *client, const JZHttpRequest &req)
{
    if (!checkToken(req))
    {
        sendJson(client, 401, Json::objectValue);
        return;
    }

    Json::Value j = makeInfo();
    j["busy"] = (bool)m_busy;
    j["round"] = m_loopRound;
    sendJson(client, 200, j);
}

void JZAgentCli::sendJson(JZHttpServerClient *client, int code, const Json::Value &j)
{
    JZHttpRespone resp;
    resp.setStatusCode(code);
    resp.setJson(Json::FastWriter().write(j));
    client->response(resp.toByteArray());
}

// ==========================================================================
// 实时事件流(GET /events, SSE) —— 任务进行中的即时反馈
// ==========================================================================

void JZAgentCli::onEvents(JZHttpServerClient *client, const JZHttpRequest &req)
{
    if (!checkToken(req))
    {
        sendJson(client, 401, Json::objectValue);
        return;
    }

    // 响应头不带 Content-Length(SSE 靠连接关闭界定结束)
    JZHttpRespone resp;
    resp.setStatusCode(200);
    resp.setContentType("text/event-stream");
    resp.setHeader("Cache-Control", "no-cache");
    resp.setHeader("Connection", "keep-alive");
    resp.setHeader("X-Accel-Buffering", "no");
    client->write(resp.toHeadByteArray(0, false), false);

    m_eventClients.push_back(client);
    connect(&client->sigDestroyed, this, [this, client](JZObject *) {
        removeEventClient(client);
    });

    Json::Value d = makeInfo();
    d["busy"] = (bool)m_busy;
    d["round"] = m_loopRound;
    Json::Value first;
    first["type"] = "hello";
    first["data"] = d;
    std::string payload = Json::FastWriter().write(first);
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();
    std::string frame = "data: " + payload + "\n\n";
    client->write(frame.data(), (int)frame.size(), false);
}

void JZAgentCli::removeEventClient(JZHttpServerClient *client)
{
    for (auto it = m_eventClients.begin(); it != m_eventClients.end(); )
    {
        if (*it == client)
            it = m_eventClients.erase(it);
        else
            ++it;
    }
}

void JZAgentCli::pushEvent(const std::string &type, const Json::Value &data)
{
    if (m_eventClients.empty())
        return;

    Json::Value j;
    j["type"] = type;
    j["data"] = data;
    std::string payload = Json::FastWriter().write(j);
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();
    std::string frame = "data: " + payload + "\n\n";
    for (JZHttpServerClient *c : m_eventClients)
        c->write(frame.data(), (int)frame.size(), false);
}

bool JZAgentCli::checkToken(const JZHttpRequest &req) const
{
    if (m_config.httpToken.empty())
        return true;   // 未配置 token: 不鉴权

    std::string tk = req.queryParam("token");
    if (tk.empty())
    {
        std::string auth = req.header("Authorization");
        if (auth.size() > 7 && str_toLower(auth.substr(0, 7)) == "bearer ")
            tk = auth.substr(7);
    }
    return tk == m_config.httpToken;
}

void JZAgentCli::onTaskSubmit(JZHttpServerClient *client, const JZHttpRequest &req)
{
    // 1) token 校验(若配置了 httpToken)
    if (!checkToken(req))
    {
        sendJson(client, 401, Json::objectValue);
        return;
    }

    // 2) 解析 JSON body
    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(std::string(req.body().data(), req.body().size()), root) || !root.isObject())
    {
        Json::Value e; e["error"] = "invalid JSON body";
        sendJson(client, 400, e);
        return;
    }

    // 3) 基本校验(空 prompt / 忙) -> 立即回包
    if (extractUserText(root).empty())
    {
        Json::Value e; e["error"] = "no prompt found (need 'prompt' or 'messages')";
        sendJson(client, 400, e);
        return;
    }
    if (m_busy)
    {
        Json::Value e; e["error"] = "a task is already running";
        sendJson(client, 409, e);
        return;
    }

    // 4) 进入任务循环, 结果经 HTTP 回包(200)
    m_pendingClient = client;
    connect(static_cast<JZSignal<JZObject*>*>(&client->sigDestroyed), this, &JZAgentCli::onClientDestroyed);
    submitTask(root, [this, client](const Json::Value &j) {
        sendJson(client, 200, j);
    });
}

// 任务提交核心: 由 HTTP(onTaskSubmit) 调用, 结果经 respond 回调回包。
void JZAgentCli::submitTask(const Json::Value &req, std::function<void(const Json::Value &)> respond)
{
    if (m_busy)
    {
        Json::Value e; e["error"] = "a task is already running";
        if (respond) respond(e);
        return;
    }
    std::string prompt = extractUserText(req);
    if (prompt.empty())
    {
        Json::Value e; e["error"] = "no prompt found (need 'prompt' or 'messages')";
        if (respond) respond(e);
        return;
    }

    std::string script = req.isMember("verify_script") ? req["verify_script"].asString() : "";
    std::string explicitCmd = req.isMember("verify_cmd") ? req["verify_cmd"].asString() : "";
    m_verifyCmd = deriveVerifyCmd(script, explicitCmd);
    m_verifyTimeoutMs = req.isMember("verify_timeout") ? req["verify_timeout"].asInt() : 60000;
    m_maxLoopRounds = req.isMember("max_verify_rounds") ? req["max_verify_rounds"].asInt() : 10;
    if (m_verifyTimeoutMs <= 0) m_verifyTimeoutMs = 60000;
    if (m_maxLoopRounds <= 0) m_maxLoopRounds = 10;

    m_respond = std::move(respond);
    m_loopActive = true;
    m_loopRound = 0;
    m_accumReply.clear();
    m_lastVerifyOutput.clear();
    m_busy = true;
    m_failed = false;
    m_cancelled = false;
    m_lastError.clear();

    Json::Value start;
    start["verify_cmd"] = m_verifyCmd;
    start["max_rounds"] = m_maxLoopRounds;
    pushEvent("task_start", start);

    if (m_sessionStarted)
        m_agent->reset();
    m_agent->request(prompt);
    m_sessionStarted = true;
    // 注意: 不在此回包; onLoopFinished 在 agent 结束后被调用并回包(经 m_respond)。
}

std::string JZAgentCli::deriveVerifyCmd(const std::string &script, const std::string &explicitCmd)
{
    if (!explicitCmd.empty())
        return explicitCmd;
    if (script.empty())
        return "";
    size_t dot = script.find_last_of('.');
    std::string ext = (dot != std::string::npos) ? str_toLower(script.substr(dot + 1)) : "";
    std::string q = "\"" + script + "\"";   // 路径含空格时保险
    if (ext == "py")  return "python3 " + q;
    if (ext == "sh")  return "bash " + q;
    if (ext == "exe") return q;
    return "bash " + q;   // 默认按 shell 脚本处理
}

void JZAgentCli::runVerify(const std::string &cmd, int timeoutMs, const std::string &cwd,
                           int &exitCode, std::string &output)
{
    output.clear();
    // 在 cwd 下执行, 合并 stderr 到 stdout 以便把失败信息一并发给模型
    std::string run = "cd \"" + cwd + "\" && " + cmd + " 2>&1";
    if (timeoutMs > 0)
    {
#ifdef _WIN32
        // Windows 下暂不包 timeout 包装(可按需改用 timeout.exe)
#else
        run = "timeout " + std::to_string(timeoutMs / 1000 + 1) + "s " + run;
#endif
    }

#ifdef _WIN32
    FILE *f = _popen(run.c_str(), "r");
#else
    FILE *f = popen(run.c_str(), "r");
#endif
    if (!f)
    {
        exitCode = -1;
        output = "无法启动验证命令: " + cmd;
        return;
    }
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), f))
        output += buf;
#ifdef _WIN32
    int st = _pclose(f);
    exitCode = (st == -1) ? -1 : st;
#else
    int st = pclose(f);
    if (st == -1)
        exitCode = -1;
    else if (WIFEXITED(st))
        exitCode = WEXITSTATUS(st);
    else
        exitCode = -1;   // 被信号杀掉 / timeout
#endif
    // 截断超大输出, 避免 JSON 体积爆炸
    const size_t kMax = 65536;
    if (output.size() > kMax)
    {
        output = output.substr(0, kMax) + "\n...(验证输出已截断, 共 " +
                std::to_string(output.size()) + " 字节)";
    }
}

void JZAgentCli::onLoopFinished()
{
    // 1) 无验证脚本时, 以 agent 自身成败为准
    if (m_verifyCmd.empty())
    {
        std::string status = m_failed ? "failed" : "success";
        std::string err = m_failed ? m_lastError : "";
        respondTask(status, m_loopRound, m_accumReply, "", err);
        endLoop();
        return;
    }

    // 2) 运行验证脚本(退出码 0 通过, 1 失败)
    int exitCode = 0;
    std::string out;
    runVerify(m_verifyCmd, m_verifyTimeoutMs, m_config.rootDir, exitCode, out);
    m_lastVerifyOutput = out;

    Json::Value ve;
    ve["round"] = m_loopRound;
    ve["exit_code"] = exitCode;
    ve["output"] = out;
    ve["passed"] = (exitCode == 0);
    pushEvent("verify", ve);

    if (exitCode == 0)
    {
        respondTask("success", m_loopRound, m_accumReply, out, "");
        endLoop();
        return;
    }

    // 2) 失败: 超过最大轮次则放弃
    m_loopRound++;
    if (m_loopRound >= m_maxLoopRounds)
    {
        respondTask("failed", m_loopRound, m_accumReply, out,
                    "验证脚本在达到最大轮次(" + std::to_string(m_maxLoopRounds) + ")后仍失败");
        endLoop();
        return;
    }

    // 3) 把验证输出喂回 agent, 自动续跑
    std::string feedback = "验证脚本执行失败(退出码 " + std::to_string(exitCode) + ")。"
        "其完整输出如下:\n```\n" + out + "\n```\n"
        "请根据以上输出定位并修复问题, 然后我会重新运行验证脚本。只做必要的修改, 不要做无关改动。";
    if (m_failed)
        feedback += "\n(上一轮 agent 自身也报错: " + m_lastError + ")";
    m_accumReply.clear();
    m_failed = false;
    m_cancelled = false;
    m_lastError.clear();
    m_agent->request(feedback);   // 重新驱动 agent; 结束后再次进入 onLoopFinished
}

void JZAgentCli::respondTask(const std::string &status, int rounds, const std::string &reply,
                             const std::string &verifyOut, const std::string &err)
{
    if (!m_respond)   // 客户端已断开 / 不应回包
        return;
    Json::Value r;
    r["status"] = status;
    r["verify_rounds"] = rounds;
    r["agent_reply"] = reply;
    r["last_verify_output"] = verifyOut;
    if (!err.empty())
        r["error"] = err;
    pushEvent("task_end", r);
    auto cb = std::move(m_respond);
    cb(r);
}

void JZAgentCli::endLoop()
{
    m_loopActive = false;
    m_busy = false;
    m_pendingClient = nullptr;
    m_respond = nullptr;
    m_verifyCmd.clear();
}

void JZAgentCli::onReset(JZHttpServerClient *client, const JZHttpRequest &req)
{
    if (!checkToken(req))
    {
        sendJson(client, 401, Json::objectValue);
        return;
    }
    if (!m_agent)
    {
        Json::Value e; e["error"] = "agent not initialized";
        sendJson(client, 500, e);
        return;
    }

    Json::Value root;
    Json::Reader reader;
    std::string scope = "task";
    if (reader.parse(std::string(req.body().data(), req.body().size()), root) && root.isObject())
        scope = root.get("scope", "task").asString();

    bool cancelRunning = (scope == "task");
    bool taskCancelled = false;

    // 有任务在跑: 先给调用方一个明确结果(status:"cancelled"), 再停 agent
    if (cancelRunning && m_loopActive)
    {
        m_loopActive = false;   // stop() 引发的 sigFinished 不再驱动验证循环
        m_busy = false;
        respondTask("cancelled", m_loopRound, m_accumReply, m_lastVerifyOutput, "任务已被重置");
        endLoop();
        taskCancelled = true;
    }

    resetSession(cancelRunning);

    Json::Value d;
    d["scope"] = scope;
    d["cancelled"] = taskCancelled;
    pushEvent("reset", d);

    Json::Value r;
    r["status"] = "ok";
    r["scope"] = scope;
    r["cancelled"] = taskCancelled;
    sendJson(client, 200, r);
}

void JZAgentCli::resetSession(bool cancelRunning)
{
    if (!m_agent)
        return;

    // stop() = llmError → llmEnd: 同步回到 Idle(并取消在途工具), 之后才允许 reset()
    if (cancelRunning)
        m_agent->stop();
    m_agent->reset();          // 仅 Idle 允许: 清对话历史 / 用量 / 待发请求
    m_sessionStarted = false;  // 下次提交按"新会话"处理
    m_loopRound = 0;
    m_accumReply.clear();
    m_lastVerifyOutput.clear();
    m_failed = false;
    m_cancelled = false;
    m_lastError.clear();
}

void JZAgentCli::onClientDestroyed(JZObject *)
{
    // HTTP 客户端在循环结束前断开: 终止任务, 避免回包到野指针
    m_pendingClient = nullptr;
    m_respond = nullptr;   // 断开即不再回包
    if (m_loopActive)
    {
        m_loopActive = false;
        if (m_agent)
            m_agent->stop();
        m_busy = false;
    }
}





void JZAgentCli::onListScripts(JZHttpServerClient *client, const JZHttpRequest &req)
{
    (void)req;
    static const char *kExts[] = {".py", ".sh", ".bat", ".cmd", ".js", ".ps1", nullptr};
    std::vector<std::string> filters;
    for (int i = 0; kExts[i]; ++i)
        filters.push_back(kExts[i]);
    JZDir dir(m_config.rootDir);
    std::vector<std::string> files = dir.entryList(filters, JZDir::Files | JZDir::NoDotAndDotDot);

    Json::Value arr(Json::arrayValue);
    for (const auto &f : files)
        arr.append(f);
    Json::Value r;
    r["scripts"] = arr;
    sendJson(client, 200, r);
}

string JZAgentCli::extractUserText(const Json::Value &params)
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

// ==========================================================================
// JZAgent 信号
// ==========================================================================

void JZAgentCli::onReply(const std::string &text)
{
    m_accumReply += text;   // 累积本轮最终回复(循环结束一次性回包)
}

void JZAgentCli::onError(const std::string &err)
{
    m_failed = true;
    m_lastError = err;

    // sigError 实时推给网页: 任务中途出错(LLM 失败/拒答/取消等)不再只能等最后回包
    Json::Value d;
    d["text"] = err;
    d["round"] = m_loopRound;
    d["loop_active"] = m_loopActive;
    pushEvent("error", d);
}

void JZAgentCli::onFinished()
{
    // 任务被取消 / 客户端断开时不跑验证
    if (!m_loopActive)
        return;
    // agent 每结束一轮就跑验证, 失败则把输出喂回 agent 自动续跑
    onLoopFinished();
}
