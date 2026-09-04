#include "JZCodeAgent.h"

#include "JZDir.h"
#include "JZFile.h"
#include "JZFileInfo.h"
#include "JZPlatform.h"
#include "JZStd.h"
#include "JZString.h"
#include "JZTextStream.h"

using namespace std;

// ==================== CodeAgentLog ====================
Json::Value CodeAgentLog::toJson() const
{
    Json::Value root;
    root["id"] = id;
    root["type"] = type;
    root["name"] = name;
    root["text"] = text;
    return root;
}

// ==================== JZCodeAgent ====================
JZCodeAgent::JZCodeAgent(JZObject *parent)
    : JZObject(parent)
{
}

JZCodeAgent::~JZCodeAgent()
{
    abortTools();
}

void JZCodeAgent::init(const JZAgentConfig &llm, const CodeAgentConfig &code)
{
    m_llmConfig = llm;
    // 系统提示词由本类自己生成，不走 rootPath/system.md 那套
    m_llmConfig.rootPath.clear();
    m_llmConfig.tokenStrategy = JZAgentConfig::TokenTruncate;
    if (m_llmConfig.maxToken <= 0)
        m_llmConfig.maxToken = 200000;

    m_codeConfig = code;
    if (!m_codeConfig.workspace.empty())
    {
        JZDir dir(m_codeConfig.workspace);
        m_codeConfig.workspace = dir.absolutePath();
    }

    m_logs.clear();
    m_logId = 0;
    m_busy = false;
    buildAgent();
    addLog("info", "代码助手已就绪，工作区: " + m_codeConfig.workspace);
    loadWorkspaceHistory();     // 自动接着上次的对话
}

bool JZCodeAgent::isBusy() const
{
    return m_busy;
}

bool JZCodeAgent::waitingReply() const
{
    return m_waiting;
}

void JZCodeAgent::request(const string &text)
{
    if (text.empty())
        return;

    addLog("user", text);
    if (m_busy)
    {
        // 正在运行: 指令缓存到自己队列, 等上一步发完后由 onFinished 继续发送
        m_pendingQueue.push_back(text);
        m_waiting = true;
        return;
    }

    m_busy = true;
    resetGuard();
    m_agent->request(text);
}

void JZCodeAgent::stop()
{
    if (!m_busy)
        return;

    m_stopping = true;
    abortTools();
    m_agent->stop();   // 同步触发 onError/onFinished, 二者均因 m_stopping 提前返回
    m_stopping = false;

    // 丢弃缓存的指令, 与"停止当前任务"语义一致
    m_pendingQueue.clear();
    m_waiting = false;
    m_busy = false;

    addLog("info", "已停止当前任务");
    // 停止时可能正好有工具结果在途中，历史会处于"assistant 有 tool_calls 但没有 tool 回复"的
    // 不一致状态，直接重建 agent 丢弃这段历史，保证下一轮请求是干净的。
    buildAgent();
}

void JZCodeAgent::clearHistory()
{
    abortTools();
    m_pendingQueue.clear();
    m_waiting = false;
    m_logs.clear();
    m_logId = 0;
    m_busy = false;

    // 清空会话 = 连存档一起删掉，否则下次启动/切回来旧对话又会自己出现
    string path = historyFilePath();
    if (!path.empty() && JZFileInfo::exists(path))
        JZFile::remove(path);

    buildAgent();
    addLog("info", "会话已清空，工作区: " + m_codeConfig.workspace);
}

int JZCodeAgent::logSeq() const
{
    return m_logId;
}

const vector<CodeAgentLog> &JZCodeAgent::logs() const
{
    return m_logs;
}

JZAgentTokenUsage JZCodeAgent::usage() const
{
    if (!m_agent)
        return JZAgentTokenUsage();
    return m_agent->context()->usage;
}

const CodeAgentConfig &JZCodeAgent::codeConfig() const
{
    return m_codeConfig;
}

const JZAgentConfig &JZCodeAgent::llmConfig() const
{
    return m_llmConfig;
}

const vector<string> &JZCodeAgent::toolNames() const
{
    return m_toolNames;
}

void JZCodeAgent::applyLlmConfig(const JZAgentConfig &llm)
{
    abortTools();
    m_llmConfig = llm;
    m_llmConfig.rootPath.clear();
    m_llmConfig.tokenStrategy = JZAgentConfig::TokenTruncate;
    if (m_llmConfig.maxToken <= 0)
        m_llmConfig.maxToken = 200000;

    m_busy = false;
    buildAgent();
    addLog("info", "大模型配置已更新: " + llm.modelName + " (" + llm.modelUrl + ")");
}

string JZCodeAgent::historyFilePath() const
{
    if (m_codeConfig.workspace.empty())
        return string();
    return m_codeConfig.workspace + "/.jzcode";
}

void JZCodeAgent::saveWorkspaceHistory()
{
    if (!m_agent || m_codeConfig.workspace.empty())
        return;
    m_agent->saveHistory(historyFilePath());
}

void JZCodeAgent::loadWorkspaceHistory()
{
    if (!m_agent)
        return;
    if (!m_agent->loadHistory(historyFilePath()))
        return;

    // 界面上同步重建出来，否则历史已经在了但对话区是空的。
    // 注意 m_logId 归零后，调用方(MainWindow)需把 m_renderedId 一并归零并全量重绘，
    // 否则增量拉取会因为 id 倒退而漏掉新日志。
    m_logs.clear();
    m_logId = 0;
    rebuildLogsFromHistory();

    size_t count = m_agent->context() ? m_agent->context()->history.size() : 0;
    addLog("info", "已导入该工作区的对话历史（" + to_string(count) + " 条消息）。");
}

void JZCodeAgent::rebuildLogsFromHistory()
{
    const LLMContext *ctx = m_agent ? m_agent->context() : nullptr;
    if (!ctx)
        return;

    for (const auto &h : ctx->history)
    {
        if (h->role == LLMHistory::User)
        {
            addLog("user", h->content);
        }
        else if (h->role == LLMHistory::Assistant)
        {
            // 整条助手消息用 "assistant"（各占一个气泡），区别于流式的 "delta"
            if (!h->content.empty())
                addLog("assistant", h->content);
            for (const auto &c : h->toolRequest)
                addLog("tool_call", c.function_args, c.function_name);
        }
        else if (h->role == LLMHistory::Tool)
        {
            addLog("tool_result", h->content);
        }
    }
}

bool JZCodeAgent::setWorkspace(const string &path)
{
    JZDir dir(path);
    if (!dir.exists())
        return false;

    string newPath = dir.absolutePath();
    if (newPath == m_codeConfig.workspace)
        return true;

    // 切换工作区 = 换一段对话: 先给旧工作区存档
    saveWorkspaceHistory();

    m_codeConfig.workspace = newPath;
    for (auto *tool : m_tools)
        tool->setWorkspace(m_codeConfig.workspace);
    applyZoneConfig();
    if (m_agent)
        m_agent->setSystemPrompt(buildSystemPrompt());

    // 重建 agent（工具与 systemPrompt 重新登记），再导入新工作区的存档
    buildAgent();
    loadWorkspaceHistory();
    return true;
}

bool JZCodeAgent::listFiles(const string &rel, vector<string> &out, string &error) const
{
    return codeListDir(m_codeConfig.workspace, rel, out, error);
}

bool JZCodeAgent::readFileText(const string &rel, string &text, string &error) const
{
    return codeReadFile(m_codeConfig.workspace, rel, text, error);
}

void JZCodeAgent::applyZoneConfig()
{
    ZoneConfig zc;
    zc.workspace = m_codeConfig.workspace;
    zc.trustedDirs = m_codeConfig.trustedDirs;
    for (auto *tool : m_tools)
    {
        auto *term = dynamic_cast<JZVirtualTerminal *>(tool);
        if (term)
        {
            term->setZoneConfig(zc);
            term->setTrustedPrograms(m_codeConfig.trustedPrograms);
        }
    }
}

void JZCodeAgent::buildAgent()
{
    if (m_agent)
    {
        m_agent->blockSignals(true);
        m_agent->deleteLater();
        m_agent = nullptr;
    }
    m_tools.clear();
    m_toolNames.clear();

    m_agent = new JZAgent(this);
    m_agent->setStreamEnabled(true);
    m_agent->init(m_llmConfig);

    connect(&m_agent->sigReply, this, &JZCodeAgent::onReply);
    connect(&m_agent->sigThinking, this, &JZCodeAgent::onThinking);
    connect(&m_agent->sigError, this, &JZCodeAgent::onError);
    connect(&m_agent->sigFinished, this, &JZCodeAgent::onFinished);

    auto addTool = [this](JZCodeTool *tool)
    {
        tool->setWorkspace(m_codeConfig.workspace);
        connect(&tool->sigToolLog, this, &JZCodeAgent::onToolLog);
        m_agent->addTool(tool);
        m_tools.push_back(tool);
        m_toolNames.push_back(tool->toolName());
    };

    addTool(new JZListDirTool());
    addTool(new JZReadFileTool());
    addTool(new JZSearchCodeTool());
    addTool(new JZModifyCodeTool());
    addTool(new JZVirtualTerminal());

    // 所有网络工具共享同一个访问管理器，代理只在此统一设置一次
    if (!m_netMan)
        m_netMan = new JZNetworkAccessManager(this);
    if (!m_codeConfig.proxyHost.empty())
    {
        JZProxyConfig pc;
        pc.host = m_codeConfig.proxyHost;
        pc.port = m_codeConfig.proxyPort;
        pc.user = m_codeConfig.proxyUser;
        pc.password = m_codeConfig.proxyPassword;
        m_netMan->setProxy(pc);
    }

    // 网络搜索工具：可配置搜索接口，共享 m_netMan（代理已统一设置）
    auto *searchTool = new JZWebSearchTool();
    if (!m_codeConfig.webSearchUrl.empty())
        searchTool->setSearchUrlTemplate(m_codeConfig.webSearchUrl);
    searchTool->setNetworkAccessManager(m_netMan);
    addTool(searchTool);

    // 网页抓取工具：与 web_search 共享同一个访问管理器与代理
    auto *fetchTool = new JZWebFetchTool();
    fetchTool->setNetworkAccessManager(m_netMan);
    addTool(fetchTool);

    applyZoneConfig();

    // 在 applyZoneConfig 之后生成提示词：availablePrograms() 依赖已下发的区域配置与可信程序
    m_agent->setSystemPrompt(buildSystemPrompt());

    // 注意: 这里不再注册 JZCommandTool(run_command)。
    // 它把整条命令原样交给 cmd /c，等于给模型开了一个绕过 terminal 全部审查的后门
    // （pip install / curl / 任意路径执行都能走通），因此一律走虚拟终端。
    // 若确需恢复，在下面加:
    //   auto *cmd_tool = new JZCommandTool();
    //   cmd_tool->setup("run_command", "...", string(), 300000);
    //   addTool(cmd_tool);
}

void JZCodeAgent::abortTools()
{
    for (auto *tool : m_tools)
    {
        // 阻断工具回传，避免"停止"之后再把结果交给已经废弃的 agent
        tool->blockSignals(true);
        auto *runner = dynamic_cast<JZProcessRunner *>(tool);
        if (runner)
            runner->stopAllProcesses();
    }
}

void JZCodeAgent::addLog(const string &type, const string &text, const string &name)
{
    CodeAgentLog log;
    log.id = ++m_logId;
    log.type = type;
    log.name = name;
    log.text = text;
    m_logs.push_back(log);

    const int kMaxLogs = 3000;
    if ((int)m_logs.size() > kMaxLogs)
        m_logs.erase(m_logs.begin(), m_logs.begin() + (m_logs.size() - kMaxLogs));
}

string JZCodeAgent::buildSystemPrompt() const
{
    string text;
    text += "你是一个运行在本地的代码助手 JZCodeAgent，可以查看、搜索、修改、编译和运行工作区中的代码。编译统一使用cmake\n";
    text += "\n## 工作区\n";
    text += "根目录: " + m_codeConfig.workspace + "\n";
    text += "所有路径参数都使用相对于该目录的相对路径（根目录用 \".\" 表示）。" \
            "绝对路径必须位于工作区之内，越界会被拒绝。\n";

    text += "\n## 可用工具\n";
    text += "- list_dir    列出目录内容，目录名以 \"/\" 结尾\n";
    text += "- read_file   读取文本文件，可按行号范围读取，返回内容带行号\n";
    text += "- search_code 在代码中搜索文本或正则表达式，返回 \"相对路径:行号:内容\"\n";
    text += "- modify_code 修改或新建文件（old_str 留空表示用 new_str 覆盖整个文件）\n";
    text += "- terminal    虚拟终端：cd / ls / cat / pwd / mkdir / rm / cp / mv，也可执行外部程序\n";
    text += "              输入 \"help\" 查看完整用法与当前可用程序\n";
    text += "- web_search 联网搜索：查文档、API 用法、报错原因、第三方库等实时信息时使用\n";
    text += "- web_fetch  抓取网页正文：拿到 web_search 返回的链接后，取其详细内容/官方文档时使用\n";
    text += "\n";
    text += "terminal 不是通用 shell：不支持管道(|)、重定向(>)与命令串联(&&)，" \
            "换其它写法也会被拒绝，不要反复重试。\n";
    if (m_codeConfig.trustedDirs.empty())
        text += "除工作区外没有授信目录，因此只能访问工作区内的路径。\n";
    else
    {
        text += "授信目录:\n";
        for (const auto &d : m_codeConfig.trustedDirs)
            text += "  " + d.path + (d.writable ? "（可读写）" : "（只读）") + "\n";
    }

    // 可用程序动态探测：只列真实存在的，避免模型去反复尝试没装的程序
    vector<string> progs;
    for (auto *tool : m_tools)
    {
        auto *term = dynamic_cast<JZVirtualTerminal *>(tool);
        if (term)
            progs = term->availablePrograms();
    }
    if (progs.empty())
        text += "可执行的外部程序: (无，未配置授信目录)\n";
    else
        text += "可执行的外部程序: " + str_join(progs, ", ") + "\n";
    text += "缺失程序时不要换其它写法重试，直接告诉用户需要安装什么、装好后要加为授信目录。\n";
    text += "\n## 工作原则\n";
    text += "1. 修改代码之前必须先用 read_file 或 search_code 确认原文，严禁凭记忆猜测文件内容。\n";
    text += "2. modify_code 的 old_str 必须与文件中的内容逐字符一致（含缩进与换行），且默认要求唯一匹配；\n";
    text += "   若匹配到多处，请补充更多上下文使其唯一，或显式设置 replace_all=true。\n";
    text += "3. 每次修改之后用 terminal 编译验证（cmake --build build），必要时再运行程序验证，直到没有错误为止。\n";
    text += "4. 编译或运行报错时，先从输出里定位到具体文件与行号再动手改，不要盲目试错。\n";
    text += "5. 一次只做最小必要的改动，不要顺手重构无关代码。\n";
    text += "6. 任务完成后用简洁的中文总结：改了哪些文件、编译与运行的结果如何。\n";

    // 项目自带的补充说明（可选）
    if (!m_codeConfig.workspace.empty())
    {
        JZFile file;
        if (file.open(m_codeConfig.workspace + "/system.md", "rt"))
        {
            JZTextStream stream(&file);
            string extra = stream.readAll();
            file.close();
            if (!str_trimmed(extra).empty())
                text += "\n## 项目补充说明（system.md）\n" + extra + "\n";
        }
    }
    return text;
}

void JZCodeAgent::onReply(const string &text)
{
    addLog("delta", text);

    // 收到任何文本: 当前这条已开始回复; 若队列已空则不再等待
    if (m_pendingQueue.empty())
        m_waiting = false;
}

void JZCodeAgent::onThinking(const string &text)
{
    addLog("thinking", text);

    // 思考也是本轮输出的开始, 与 onReply 同样解除"等待回复中"
    if (m_pendingQueue.empty())
        m_waiting = false;
}

void JZCodeAgent::onError(const string &text)
{
    if (m_stopping)
        return;

    addLog("error", text);
    // busy 终态交给随后同步触发的 onFinished 统一裁决
    m_busy = false;
}

void JZCodeAgent::onFinished()
{
    if (m_stopping)
        return;

    if (!m_pendingQueue.empty())
    {
        // 上一步已发完: 取出缓存的第一条继续发送, 期间 busy 保持占用
        string next = m_pendingQueue.front();
        m_pendingQueue.pop_front();
        m_busy = true;
        m_waiting = true;
        resetGuard();
        m_agent->request(next);
    }
    else
    {
        m_waiting = false;
        m_busy = false;
    }
}

void JZCodeAgent::setGuardConfig(const GuardConfig &cfg)
{
    m_guardConfig = cfg;
}

const GuardConfig &JZCodeAgent::guardConfig() const
{
    return m_guardConfig;
}

void JZCodeAgent::resetGuard()
{
    m_guard = GuardState();
    m_guard.startTimeMs = JZPlatform::getTickCountMs();
}

void JZCodeAgent::checkGuard()
{
    // 1. 工具调用总次数（最直接的一道闸）
    if (m_guard.toolCalls > m_guardConfig.maxToolCalls)
    {
        triggerGuard("工具调用次数超过上限（" + to_string(m_guardConfig.maxToolCalls) + " 次）",
                     "任务可能被拆得过细，或已陷入反复试探。建议缩小范围后重试。");
        return;
    }

    // 2. 墙钟时长
    int64_t costMs = JZPlatform::getTickCountMs() - m_guard.startTimeMs;
    if (m_guard.startTimeMs > 0 && costMs > (int64_t)m_guardConfig.maxDurationSec * 1000)
    {
        triggerGuard("任务运行超过时长上限（已用 " + to_string(costMs / 1000) + " 秒）",
                     "建议把任务拆成更小的步骤分步执行。");
        return;
    }

    // 3. 同一类拒绝反复出现：说明模型在绕同一堵墙
    for (const auto &it : m_guard.denyCount)
    {
        if (it.second >= m_guardConfig.maxSameDeny)
        {
            triggerGuard(string("同一类错误反复出现：") + denyReasonText((DenyReason)it.first) +
                             "（累计 " + to_string(it.second) + " 次）",
                         "不要换其它写法继续尝试同一件事；先确认原因，或直接告诉用户缺少什么。");
            return;
        }
    }

    // 4. 完全相同的调用连续重复：判定已卡住
    if ((int)m_guard.recentCalls.size() >= m_guardConfig.maxRepeatCall)
    {
        bool allSame = true;
        const string &first = m_guard.recentCalls.front();
        for (const auto &c : m_guard.recentCalls)
        {
            if (c != first)
            {
                allSame = false;
                break;
            }
        }
        if (allSame)
        {
            triggerGuard("完全相同的调用连续执行了 " + to_string(m_guard.recentCalls.size()) + " 次",
                         "已陷入重复尝试。请换一种思路，或明确告诉用户当前卡在哪一步。");
            return;
        }
    }
}

void JZCodeAgent::triggerGuard(const string &reason, const string &suggestion)
{
    string text = "已触发保护，停止当前任务。\n";
    text += "原因: " + reason + "\n";
    text += "统计: 工具调用 " + to_string(m_guard.toolCalls) +
            " 次，其中失败/被拒 " + to_string(m_guard.totalDeny) + " 次。\n";
    text += "建议: " + suggestion;

    addLog("info", text);
    stop();
}

void JZCodeAgent::onToolLog(const string &type, const string &name, const string &text, int deny)
{
    // notice: 工具主动给用户的提示（如缺少某个程序）。模型不一定转述，
    // 所以单独走 info 让用户能直接看到；它不是一次工具调用，不参与熔断计数。
    if (type == "notice")
    {
        addLog("info", text, name);
        return;
    }

    addLog(type == "call" ? "tool_call" : "tool_result", text, name);

    if (type == "call")
    {
        m_guard.toolCalls++;

        // 指纹 = 工具名 + 参数文本，用于识别"换汤不换药"的重复尝试
        m_guard.recentCalls.push_back(name + "|" + str_trimmed(text));
        while ((int)m_guard.recentCalls.size() > m_guardConfig.maxRepeatCall)
            m_guard.recentCalls.pop_front();
    }
    if (deny != (int)DenyReason::None)
    {
        m_guard.denyCount[deny]++;
        m_guard.totalDeny++;
    }
    checkGuard();

    // 工具已开始执行也算"有输出", 若队列已空则结束等待提示
    if (m_pendingQueue.empty())
        m_waiting = false;
}
