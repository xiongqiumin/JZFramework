#include "JZCodeAgent.h"

#include "JZDir.h"
#include "JZFile.h"
#include "JZFileInfo.h"
#include "JZStd.h"
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
}

bool JZCodeAgent::isBusy() const
{
    return m_busy;
}

void JZCodeAgent::request(const string &text)
{
    if (m_busy || text.empty())
        return;

    addLog("user", text);
    m_busy = true;
    m_agent->request(text);
}

void JZCodeAgent::stop()
{
    if (!m_busy)
        return;

    m_stopping = true;
    abortTools();
    m_agent->stop();
    m_stopping = false;
    m_busy = false;

    addLog("info", "已停止当前任务");
    // 停止时可能正好有工具结果在途中，历史会处于"assistant 有 tool_calls 但没有 tool 回复"的
    // 不一致状态，直接重建 agent 丢弃这段历史，保证下一轮请求是干净的。
    buildAgent();
}

void JZCodeAgent::clearHistory()
{
    abortTools();
    m_logs.clear();
    m_logId = 0;
    m_busy = false;
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

bool JZCodeAgent::setWorkspace(const string &path)
{
    JZDir dir(path);
    if (!dir.exists())
        return false;

    m_codeConfig.workspace = dir.absolutePath();
    for (auto *tool : m_tools)
        tool->setWorkspace(m_codeConfig.workspace);
    if (m_agent)
        m_agent->setSystemPrompt(buildSystemPrompt());
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
    m_agent->setSystemPrompt(buildSystemPrompt());

    connect(&m_agent->sigReply, this, &JZCodeAgent::onReply);
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

    // 通用命令工具: 命令完全由 command 参数给出, 默认超时 5 分钟
    auto *cmd_tool = new JZCommandTool();
    cmd_tool->setup("run_command",
                    "在工作区里执行一条命令行命令（编译、运行程序、跑脚本等都可以）。" \
                    "command 给出完整命令，如 cmake --build build、build/Debug/demo.exe、python test.py。" \
                    "程序必须能自行结束，超时会被强制终止。返回退出码、耗时与 stdout/stderr。",
                    string(), 300000);
    addTool(cmd_tool);
}

void JZCodeAgent::abortTools()
{
    for (auto *tool : m_tools)
    {
        // 阻断工具回传，避免"停止"之后再把结果交给已经废弃的 agent
        tool->blockSignals(true);
        auto *cmd_tool = dynamic_cast<JZCommandTool *>(tool);
        if (cmd_tool)
            cmd_tool->stopAll();
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
    text += "- run_command 执行一条命令行命令，返回退出码与 stdout/stderr（默认超时 5 分钟）\n";
    text += "\n";
    text += "\n## 工作原则\n";
    text += "1. 修改代码之前必须先用 read_file 或 search_code 确认原文，严禁凭记忆猜测文件内容。\n";
    text += "2. modify_code 的 old_str 必须与文件中的内容逐字符一致（含缩进与换行），且默认要求唯一匹配；\n";
    text += "   若匹配到多处，请补充更多上下文使其唯一，或显式设置 replace_all=true。\n";
    text += "3. 每次修改之后用 run_command 编译验证，必要时再运行程序验证，直到没有错误为止（编译统一用 cmake）。\n";
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
}

void JZCodeAgent::onError(const string &text)
{
    if (m_stopping)
        return;

    addLog("error", text);
    m_busy = false;
}

void JZCodeAgent::onFinished()
{
    m_busy = false;
}

void JZCodeAgent::onToolLog(const string &type, const string &name, const string &text)
{
    addLog(type == "call" ? "tool_call" : "tool_result", text, name);
}
