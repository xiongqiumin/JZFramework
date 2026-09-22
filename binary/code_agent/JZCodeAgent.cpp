#include "JZCodeAgent.h"

#include "JZTerminalTools.h"
#include "JZVirtualTerminal.h"

#include "JZTsLangTools.h"
#include "JZTsEditTools.h"

#include "JZDir.h"
#include "JZFile.h"
#include "JZFileInfo.h"
#include "JZPlatform.h"
#include "JZExeResource.h"
#include "JZProcess.h"
#include "JZStd.h"
#include "JZString.h"
#include "JZTextStream.h"
#include "JZTimer.h"
#include "JZApplication.h"
#include "JZJSEngine/JZJSUiLoader.h"

#include <fstream>
#include <iterator>

using namespace std;

// ==================== 工程骨架辅助 ====================
// 写入文本文件；已存在则不覆盖（初始化工程骨架的幂等语义）
static bool writeIfAbsent(const string &path, const string &text)
{
    if (JZFileInfo::exists(path))
        return true;
    ofstream out(path, ios::binary | ios::trunc);
    if (!out)
        return false;
    out.write(text.data(), (streamsize)text.size());
    out.close();
    return true;
}

// 取路径的目录名（去掉末尾分隔符后最后一个分量）
static string codeBaseName(const string &path)
{
    string p = path;
    while (p.size() > 1 && (p.back() == '/' || p.back() == '\\'))
        p.pop_back();
    size_t slash = p.find_last_of("/\\");
    return (slash == string::npos) ? p : p.substr(slash + 1);
}

// ==================== CodeAgentLog ====================
Json::Value CodeAgentLog::toJson() const
{
    Json::Value root;
    root["id"] = id;
    root["type"] = type;
    root["name"] = name;
    root["text"] = text;
    if (!images.empty())
    {
        Json::Value arr(Json::arrayValue);
        for (const auto &url : images)
            arr.append(url);
        root["images"] = arr;
    }
    return root;
}

// ==================== JZCodeAgent ====================
JZCodeAgent::JZCodeAgent(JZObject *parent)
    : JZObject(parent)
    , sigRecoveryFinished(this)
    , sigBuildFinished(this)
    , m_toolHost(this)
{
    m_tsServer = new JZTsLangServer(this);
}

JZCodeAgent::~JZCodeAgent()
{
    abortBuild();
    abortTools();
    if (m_tsServer)
        m_tsServer->shutdown();
}

void JZCodeAgent::init(const CodeAgentConfig &code)
{
    m_llmConfig = code.llmConfig;
    // 系统提示词由本类自己生成，不走 rootPath/system.md 那套
    m_llmConfig.rootPath.clear();
    m_llmConfig.tokenStrategy = JZAgentConfig::TokenCompress;
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
    // 注：载入哪段对话（任务存档）由上层决定，见 JZCodeAgent::loadConversationJson()
}

bool JZCodeAgent::isBusy() const
{
    return m_busy;
}

void JZCodeAgent::request(const string &text)
{
    request(text, std::vector<string>());
}

void JZCodeAgent::request(const string &text, const vector<string> &images)
{
    // 只有文字与图片都为空才算空请求(允许"只发截图")
    if (text.empty() && images.empty())
        return;

    addLog("user", text, string(), images);
    if (m_busy)
    {
        JZ_ASSERT(0);
        return;
    }

    // 自动恢复：首次(外层)请求记为 attempt #1；恢复重发时 m_recoveryActive 已为 true，不重复计数
    if (m_recoveryEnabled && !m_recoveryActive)
    {
        m_recoveryActive = true;
        m_recoveryAttempt = 1;
        m_lastUserRequest = text;
        m_lastUserImages = images;
    }

    sendRequestDirect(text, images);
}

// 导出当前对话（交给调用方存进自己的介质，如 SQLite 任务库）；无有效对话返回 null
Json::Value JZCodeAgent::conversationJson() const
{
    if (!m_agent)
        return Json::Value();

    // 存储层要的是文本，所以在 code_agent 这一层把强类型的上下文序列化
    LLMContext ctx = m_agent->saveContext();
    if (ctx.history.empty())
        return Json::Value();
    return ctx.toJson();
}

// 取"上下文起点"及其之后的历史；没有起点标记时返回整段。
// 存档里存的是完整对话记录(界面回看用)，而发给模型的上下文只从最后一次压缩的起点开始。
static std::vector<std::shared_ptr<LLMHistory>> contextFromLastStart(
    const std::vector<std::shared_ptr<LLMHistory>> &history)
{
    for (size_t i = history.size(); i > 0; --i)
    {
        const auto &h = history[i - 1];
        // ContextStart(用户第一句话) 与 Summary(压缩摘要) 都是上下文起点
        if (h && h->kind != LLMHistory::Message)
            return std::vector<std::shared_ptr<LLMHistory>>(history.begin() + (i - 1), history.end());
    }
    return history;
}

// 载入一段对话（替换当前对话），并把界面日志按它重建
bool JZCodeAgent::loadConversationJson(const Json::Value &root)
{
    if (!m_agent || root.isNull())
        return false;

    // 从存储里读回来的是文本，先还原成完整上下文
    LLMContext saved;
    if (!saved.fromJson(root))
        return false;

    // 给 agent 的上下文只从最后一条"上下文起点"算起：它之前的消息在压缩时已被摘要替换、
    // 不再是模型上下文的一部分(只保留在存档里供界面回看)。
    LLMContext ctx;
    ctx.history = contextFromLastStart(saved.history);
    m_agent->loadContext(ctx);   // 内部会重算 usage 并裁到自洽

    // 界面按**完整**记录重建，这样压缩前的历史照样看得到
    m_logs.clear();
    m_logId = 0;
    rebuildLogsFromHistory(saved.history);
    return true;
}

void JZCodeAgent::rebuildLogsFromHistory(const std::vector<std::shared_ptr<LLMHistory>> &history)
{
    // 把一段对话历史重建成界面会话日志（导入存档后用）。
    // 传进来的是**完整**记录：压缩掉的那部分也要显示，用户能看到对话被压缩过。
    // 日志类型与实时流程保持一致：user / assistant / think / tool_call / tool_result，
    // tool_call / tool_result 的名字格式与 onToolCall / onToolResult 严格对齐，
    // 这样界面渲染（工具调用与结果配对）和落盘后再重建的结果一致。
    m_logs.clear();
    m_logId = 0;

    for (const auto &h : history)
    {
        if (!h)
            continue;

        // 压缩摘要(Summary)是纯内部消息，不进界面；
        // "上下文起点"(用户输入的第一句话)则照常显示，用户看到的是完整对话记录。
        if (h->kind == LLMHistory::Summary)
            continue;

        switch (h->role)
        {
        case LLMHistory::User:
            // 带图消息即使正文为空也要还原(仅截图的消息)
            if (!h->content.empty() || !h->images.empty())
                addLog("user", h->content, string(), h->images);
            break;

        case LLMHistory::Assistant:
            // 思考过程：实时上抛不落历史，这里仅在历史里带了 thinking 时还原（保险）。
            if (!h->thinking.empty())
                addLog("thinking", h->thinking);
            // 整条助手回复（历史重建用 assistant 类型，与流式 delta 区分）。
            if (!h->content.empty())
                addLog("assistant", h->content);
            // 工具调用：名字沿用 onToolCall 的 "function_name$$tool_gap$$id" 格式。
            for (const auto &tc : h->toolRequest)
                addLog("tool_call", "", tc.function_name + "$$tool_gap$$" + tc.id);
            break;

        case LLMHistory::Tool:
            // 工具返回：名字沿用 onToolResult 的 id（对应 tool_response_id / toolResponseId）。
            addLog("tool_result", h->content, h->toolResponseId);
            break;

        case LLMHistory::System:
        default:
            // 系统提示词不进入界面日志。
            break;
        }
    }
}

// 中断当前任务并重置为一段干净的新对话（切换/新建任务时用）。
// 只动 agent 与日志，任务元数据与存档文件由 TaskHistoryManager 负责。
void JZCodeAgent::resetConversation()
{
    abortTools();
    if (m_busy)
    {
        m_stopping = true;
        m_agent->stop();
        m_stopping = false;
        m_busy = false;
    }
    m_logs.clear();
    m_logId = 0;
    buildAgent();
}

void JZCodeAgent::sendRequestDirect(const string &text, const vector<string> &images)
{
    // 直接发给底层 agent（request 的队列分支与宿主续修都走这里）。
    // 每条新指令 = 一轮完整的自动续修额度 + 重新统计代码改动。
    m_fixRoundsLeft = m_codeConfig.maxAutoFixRounds;
    m_roundTouchedTs = false;
    m_busy = true;
    if (images.empty())
        m_agent->request(text);
    else
        m_agent->request(text, images);   // 多模态: 文字 + 图片同属一条 user 消息
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

    // 只清内存里的这段对话；对应的存档文件由上层用 TaskHistoryManager::removeArchive() 删
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

int JZCodeAgent::windowTokens() const
{
    return m_agent ? m_agent->windowTokens() : 0;
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

bool JZCodeAgent::isTrustedDir(std::string dir) const
{
    auto dir_path = JZFileInfo::weaklyCanonicalPath(dir);
    for (auto &trusted : m_codeConfig.trustedDirs)
    {
        if (JZFileInfo::weaklyCanonicalPath(trusted.path) == dir_path)
            return true;
    }
    return false;
}

bool JZCodeAgent::isTrustedProgram(std::string prog_name) const
{
    for(auto &prog : m_codeConfig.trustedPrograms)
    { 
        if(prog.name == prog_name)
            return true;
    }
    return false;
}

void JZCodeAgent::applyLlmConfig(const JZAgentConfig &llm)
{
    abortTools();
    m_llmConfig = llm;
    m_llmConfig.rootPath.clear();
    m_llmConfig.tokenStrategy = JZAgentConfig::TokenCompress;
    if (m_llmConfig.maxToken <= 0)
        m_llmConfig.maxToken = 200000;

    m_busy = false;
    buildAgent();
    addLog("info", "大模型配置已更新: " + llm.modelName + " (" + llm.modelUrl + ")");
}

JZAgent *JZCodeAgent::agent()
{
    return m_agent;
}

bool JZCodeAgent::setWorkspace(const string &path)
{
    JZDir dir(path);
    if (!dir.exists())
        return false;

    string newPath = dir.absolutePath();
    if (newPath == m_codeConfig.workspace)
        return true;

    // 切换工作区 = 换一段对话。旧工作区的存档由上层（TaskHistoryManager 的持有者）
    // 在切换前保存，这里只负责把 agent 与工作区配置换掉。
    m_codeConfig.workspace = newPath;
    for (auto* tool : m_tools)
        tool->setHost(&m_toolHost);
    applyZoneConfig();

    // 重建 agent（工具与 systemPrompt 重新登记）；载入哪段对话由上层决定
    buildAgent();
    return true;
}

// ==================== TS 工程支持 ====================
CodeProjectStatus JZCodeAgent::probeProject(const string &path) const
{
    CodeProjectStatus st;
    string root = path;
    while (root.size() > 1 && (root.back() == '/' || root.back() == '\\'))
        root.pop_back();
    if (root.empty())
    {
        st.error = "路径为空";
        return st;
    }

    st.name = codeBaseName(root);
    if (st.name.empty())
        st.name = ".";
    st.pathOk = true;

    JZDir dir(root);
    if (!dir.exists() || !JZFileInfo(root).isDir())
    {
        st.pathOk = false;
        st.error = "目录不存在: " + root;
        return st;
    }

    st.hasTsconfig = JZFileInfo::exists(root + "/tsconfig.json");
    st.isProject = st.hasTsconfig;

    vector<string> entries =
        dir.entryList(JZDir::Files | JZDir::Dirs | JZDir::NoDotAndDotDot);
    st.isEmpty = entries.empty();

    if (!st.isProject)
    {
        if (st.isEmpty)
            st.summary = "「" + st.name + "」是空目录，还不是 TS 工程目录。可调用 initProject 初始化为标准 TS 工程。";
        else
            st.summary = "「" + st.name + "」不是 TS 工程目录（缺少 tsconfig.json，无法用 tsc 编译）。可调用 initProject 初始化为标准 TS 工程。";
    }
    else
        st.summary = "「" + st.name + "」是 TS 工程（tsconfig.json）。";
    return st;
}

bool JZCodeAgent::openProject(const string &path, CodeProjectStatus *status, string &error)
{
    error.clear();
    CodeProjectStatus st = probeProject(path);
    if (status)
        *status = st;
    if (!st.pathOk)
    {
        error = st.error;
        return false;
    }

    if (!setWorkspace(path))
    {
        error = "无法把目录切换为工作区: " + path;
        return false;
    }
    m_project = st;
    return true;
}

void JZCodeAgent::closeProject()
{
    // workspace 一并清空：否则再打开"同一个"目录时 setWorkspace 会因为路径相同直接返回，
    // 不重建 agent、也不导入存档，界面上是空的而 agent 里还留着上一段对话。
    m_codeConfig.workspace.clear();
    m_project = CodeProjectStatus();
    m_logs.clear();
    m_logId = 0;
    m_busy = false;
}

const CodeProjectStatus &JZCodeAgent::projectStatus() const
{
    return m_project;
}

// ==================== .ui -> TypeScript 生成 ====================
// 工程内 .ui 文件（Qt 风格 UI 描述 XML）经 JZJSUiLoader 生成可被 tsc 编译的
// TypeScript 类，产物统一写入 src/GenerateFiles/（位于 rootDir=src 之下，被 tsconfig
// 的 src/**/*.ts 自动纳入编译）。按"工程相对路径扁平化"命名避免同名冲突，且每次都重跑
// （uic 风格），因此用户新增/修改/删除 .ui 都会即时反映到编译产物。
//
// 自定义文件同样支持：
//   - 任意新增的 .ui 会被自动转换（扫描整个工程，排除 build/、node_modules/、GenerateFiles）；
//   - 手写 .ts（src 下、非 GenerateFiles 目录）照常编译；
//   - GenerateFiles 内与 .ui 同名的 .ts 由本步骤托管（请勿手改，会被覆盖）。
// 调用入口：打包/发布前自动触发；也可单独调用 generateUi()。

static bool codeIEndsWith(const string &s, const string &suffix)
{
    if (suffix.size() > s.size())
        return false;
    size_t off = s.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); i++)
    {
        char a = s[off + i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b)
            return false;
    }
    return true;
}

// 递归收集工程内所有 .ui 文件；跳过 build/、node_modules/、GenerateFiles（产物目录）。
static void collectUiFiles(const string &dir, vector<string> &out)
{
    JZDir d(dir);
    if (!d.exists())
        return;
    vector<string> items = d.entryList(JZDir::Files | JZDir::Dirs | JZDir::NoDotAndDotDot);
    for (const string &name : items)
    {
        string full = codeNormalizePath(dir + "/" + name);
        if (JZFileInfo(full).isDir())
        {
            if (name == "build" || name == "node_modules" || name == "GenerateFiles")
                continue;
            collectUiFiles(full, out);
        }
        else if (codeIEndsWith(name, ".ui"))
        {
            out.push_back(full);
        }
    }
}

bool JZCodeAgent::initProject(const string &path, string &error)
{
    error.clear();
    string root = path;
    while (root.size() > 1 && (root.back() == '/' || root.back() == '\\'))
        root.pop_back();
    if (root.empty())
    {
        error = "目录不能为空";
        return false;
    }

    JZDir dir(root);
    if (!dir.exists())
    {
        JZDir mk;
        if (!mk.mkpath(root))
        {
            error = "创建目录失败: " + root;
            return false;
        }
    }
    else
    {
        if (!JZFileInfo(root).isDir())
        {
            error = "路径不是目录: " + root;
            return false;
        }
        if (dir.entryInfoList().size() != 0)
        {
            error = "目录不是空目录";
            return false;
        }
    }    

    string name = codeBaseName(root);
    if (name.empty())
        name = "project";
    // 标准 TS 工程骨架（产物统一输出到 build/）。所有写入都不覆盖已有文件。
    // sourceMap: true 为"调用运行时"必备 —— 运行期 JS 栈要靠 tsc 生成的
    // build/*.js.map 翻译回 TS 源码位置（JZEngineRuntime）。
    // project.json 不是骨架文件：它由打包/发布(buildProject)写在产物目录（main.js
    // 所在目录）里，作为补充信息（编译时间 / 原路径地址 ...）。
    // 生成的工程 lib 用 ES2025：对齐 quickjs 实际支持的范围（ES2022~ES2025 全部，
    // 另加部分 esnext 提案）；显式写 lib 同时关掉默认带进来的 DOM，jz_types 里声明的
    // 全局 fetch/WebSocket/URL 等才不会被 lib.dom.d.ts 撞成 duplicate identifier。
    // 不写 "ESNext"：那会放行 quickjs 根本没有的 Temporal 等 API（编译通过、运行时
    // undefined）。已知唯一假阳性：Atomics.waitAsync（lib.es2024.sharedmemory 有声明，
    // quickjs 未实现）。target 保持 ES2022：quickjs 对 ES2022 语法完整支持，且
    // ES2023~ES2025 没有新增语法，不会发生降级。
    //
    // jz_types / jz_js 各从 exe 目录**拷一份到工程里**（<工程根>/jz_types、<工程根>/jz_js）：
    // 工程因此自包含，tsconfig 只写相对路径，换机器 / 换 exe 目录 / 拷给别人都不会丢。
    // 新部署把两者统一放在 exe 同目录的 jz_engine/ 下（jz_engine/jz_types、jz_engine/jz_js），
    // 旧部署仍可能直接平铺在 exe 目录（jz_types/、jz_js/），这里两种布局都兼容。
    // - jz_types：类型声明，include 收 jz_types 下的 .d.ts；编译期类型检查用。
    // - jz_js：运行时 JS（core/net/serialport 的 dist），用户入口可
    //   import '../jz_js/<pkg>/dist/index.js' 被动加载运行期全局（纯 import 模型）。
    // 源目录不存在时仅收 src，不报错（jz_js 缺失仅代表工程不引用运行时，仍可编译运行）。
    auto copyIfPresent = [&](const string &srcRel, const string &dstRel,
                             const string &label, bool &flag) {
        string appDir = JZPlatform::applicationDirPath();
        string src = appDir + "/jz_engine/" + srcRel;
        if (!JZFileInfo(src).isDir())
            src = appDir + "/" + srcRel;          // 兼容旧部署（平铺在 exe 目录）
        if (!JZFileInfo(src).isDir())
            return;                                // 源不存在：保持 flag=false，不报错
        string copyErr;
        if (!JZDir::copyRecursively(src, root + "/" + dstRel, false, &copyErr))
            error = "复制 " + label + " 失败: " + copyErr;    // 真实失败：记 error
        else
            flag = true;
    };

    bool hasJzTypes = false;
    copyIfPresent("jz_types", "jz_types", "jz_types", hasJzTypes);
    bool hasJzJs = false;
    copyIfPresent("jz_js", "jz_js", "jz_js", hasJzJs);
    if (!error.empty())
        return false;
    string jzTypesInclude = "\"src/**/*.ts\"";
    if (hasJzTypes)
        jzTypesInclude += ", \"jz_types/**/*.d.ts\"";

    string tsconfig =
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
        "  \"include\": [" + jzTypesInclude + "],\n"
        "  \"jzCompilerOptions\": {}\n"
        "}\n";
    string gitignore =
        "build/\n"
        "node_modules/\n";

    bool ok = writeIfAbsent(root + "/tsconfig.json", tsconfig);
    if (!ok)
    {
        error = "写入 tsconfig.json 失败: " + root;
        return false;
    }
    ok = writeIfAbsent(root + "/.gitignore", gitignore);
    if (!ok)
    {
        error = "写入 .gitignore 失败: " + root;
        return false;
    }

    // src 目录不存在或为空时补一个可编译的占位入口
    JZDir srcDir(root + "/src");
    bool srcExists = srcDir.exists();
    bool srcEmpty = true;
    if (srcExists)
    {
        vector<string> items =
            srcDir.entryList(JZDir::Files | JZDir::Dirs | JZDir::NoDotAndDotDot);
        srcEmpty = items.empty();
    }
    if (srcEmpty)
    {
        if (!srcExists)
        {
            JZDir mk;
            mk.mkpath(root + "/src");
        }
        string mainTs =
            "// " + name + " 工程入口示例（可自行删改，跨文件用 import/export）\n"
            "// 打包/发布: tsc -p tsconfig.json（产物为原生 ESM 输出到 build/，主入口 main.js）\n"
            "// 运行: run_js 加载 main.js 并执行\n"
            "export function main(): void\n"
            "{\n"
            "    console.log(\"Hello from " + name + "!\");\n"
            "}\n"
            "main();\n";
        if (!writeIfAbsent(root + "/src/main.ts", mainTs))
        {
            error = "写入 src/main.ts 失败: " + root;
            return false;
        }
    }

    // 生成目录（.ui -> .ts 的产物目录，置于 src 下以满足 tsconfig 的 rootDir=src）
    {
        JZDir mk;
        if (!mk.mkpath(root + "/src/GenerateFiles"))
        {
            error = "创建 GenerateFiles 目录失败: " + root;
            return false;
        }
    }

    addLog("info", "已初始化 TS 工程");
    return true;
}

// 依据工程内 .ui 文件生成 TypeScript（JZJSUiLoader），产物写入 src/GenerateFiles/。
// rootArg 为空时用当前工作区（m_codeConfig.workspace）；打包/发布前自动调用，亦可单独触发。
bool JZCodeAgent::generateUi(const string &rootArg, string &error)
{
    error.clear();
    string root = codeNormalizePath(rootArg.empty() ? m_codeConfig.workspace : rootArg);
    if (root.empty())
    {
        error = "未设置工作区，无法生成 UI";
        return false;
    }
    if (!JZFileInfo(root + "/tsconfig.json").isFile())
    {
        error = "不是 TS 工程目录（缺少 tsconfig.json）: " + root;
        return false;
    }

    // 确保生成目录存在（位于 src 下，满足 tsconfig 的 rootDir=src 约束）
    string genDir = root + "/src/GenerateFiles";
    {
        JZDir mk;
        if (!mk.mkpath(genDir))
        {
            error = "创建 GenerateFiles 目录失败: " + genDir;
            return false;
        }
    }

    vector<string> uiFiles;
    collectUiFiles(root, uiFiles);

    JZJSUiLoader loader;
    for (const string &uiPath : uiFiles)
    {
        if (!loader.loadFromFile(uiPath))
        {
            error = "解析 UI 失败: " + uiPath + " -> " + loader.error();
            return false;
        }
        string ts;
        if (!loader.generateTsClass(ts, loader.className(), "widget"))
        {
            error = "生成 TS 失败: " + uiPath + " -> " + loader.error();
            return false;
        }

        // 输出文件名 = 工程相对路径扁平化（去扩展名、分隔符转 '_'），避免同名冲突
        string rel = uiPath.substr(root.size());
        while (!rel.empty() && (rel[0] == '/' || rel[0] == '\\'))
            rel = rel.substr(1);
        string base = rel;
        size_t dot = base.rfind('.');
        if (dot != string::npos)
            base = base.substr(0, dot);
        for (char &c : base)
            if (c == '/' || c == '\\')
                c = '_';

        string outPath = genDir + "/" + base + ".ts";
        ofstream f(outPath, ios::binary | ios::trunc);
        if (!f)
        {
            error = "写入生成文件失败: " + outPath;
            return false;
        }
        f.write(ts.data(), (streamsize)ts.size());
        f.close();
    }

    if (!uiFiles.empty())
        addLog("info", "已依据 " + to_string(uiFiles.size()) + " 个 .ui 文件生成 TypeScript 到 src/GenerateFiles");
    return true;
}

// ==================== 打包/发布：tsconfig outDir 解析 ====================
// tsconfig.json 是 JSONC（tsc 容忍注释与尾逗号）：剥掉 // 与 /* */ 注释、
// 清掉结构末项的尾逗号后转成可交给 Json::Reader 的纯 JSON。
static string jsJsoncToJson(const string &text)
{
    string out;
    out.reserve(text.size());
    bool inString = false, inLine = false, inBlock = false;
    for (size_t i = 0; i < text.size(); i++)
    {
        char c = text[i];
        char n = (i + 1 < text.size()) ? text[i + 1] : '\0';
        if (inLine)
        {
            if (c == '\n' || c == '\r')
                inLine = false;
            continue;
        }
        if (inBlock)
        {
            if (c == '*' && n == '/')
            {
                inBlock = false;
                i++;
            }
            continue;
        }
        if (inString)
        {
            out += c;
            if (c == '\\' && n)
            {
                out += n;
                i++;
            }
            else if (c == '"')
                inString = false;
            continue;
        }
        if (c == '"')
        {
            inString = true;
            out += c;
            continue;
        }
        if (c == '/' && n == '/')
        {
            inLine = true;
            i++;
            continue;
        }
        if (c == '/' && n == '*')
        {
            inBlock = true;
            i++;
            continue;
        }
        if (c == '}' || c == ']')
        {
            // JSONC 尾逗号：清掉本结构最后一项后的 ','
            size_t j = out.size();
            while (j > 0 && (out[j - 1] == ' ' || out[j - 1] == '\t' ||
                             out[j - 1] == '\n' || out[j - 1] == '\r'))
                j--;
            if (j > 0 && out[j - 1] == ',')
                out.erase(j - 1);
        }
        out += c;
    }
    return out;
}

// 读 tsconfig.json 的 compilerOptions.<key>；未配置/解析失败回退默认值。
static string tsConfigOptionOf(const string &root, const char *key, const string &fallback)
{
    ifstream in(root + "/tsconfig.json", ios::binary);
    if (!in)
        return fallback;
    string text((istreambuf_iterator<char>(in)), istreambuf_iterator<char>());
    in.close();
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        text.erase(0, 3);

    Json::Value cfg;
    Json::Reader reader;
    if (!reader.parse(jsJsoncToJson(text), cfg) || !cfg.isObject())
        return fallback;
    const Json::Value &co = cfg["compilerOptions"];
    if (!co.isObject())
        return fallback;
    const Json::Value &od = co[key];
    if (!od.isString())
        return fallback;
    string dir = od.asString();
    for (size_t i = 0; i < dir.size(); i++)
    {
        if (dir[i] == '\\')
            dir[i] = '/';
    }
    while (dir.size() > 1 && dir.back() == '/')
        dir.pop_back();
    if (dir.empty())
        return fallback;
    return dir;
}

// 读 tsconfig.json 的 compilerOptions.outDir；未配置/解析失败回退默认值。
static string tsOutDirOf(const string &root, const string &fallback)
{
    return tsConfigOptionOf(root, "outDir", fallback);
}

// 读 tsconfig.json 的 compilerOptions.rootDir（源码根）；未配置回退 "src"。
static string tsRootDirOf(const string &root)
{
    return tsConfigOptionOf(root, "rootDir", "src");
}

// 产物目录（= 运行根，main.js 所在目录）的绝对路径：tsconfig 的 outDir（缺省
// build，可能相对工程根、也可能指向工程根之外）按工程根展开归一。
static string tsBuildAbsOf(const string &root)
{
    string outDir = tsOutDirOf(root, "build");
    bool abs = !outDir.empty() && (outDir[0] == '/' || outDir[0] == '\\' ||
               (outDir.size() > 1 && outDir[1] == ':'));
    return codeNormalizePath(abs ? outDir : (root + "/" + outDir));
}

// 本地时间串（YYYY-MM-DD HH:MM:SS），写进 project.json 的 buildTime
static string tsNowString()
{
    time_t t = time(nullptr);
    struct tm tm_buf;
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char buf[32] = {0};
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return string(buf);
}

// 打包/发布当前工作区工程（异步）：前置校验通过后启动 tsc 子进程立即返回 true；
// 编译结束由 onBuildFinished 校验入口产物并写 project.json，结果经 sigBuildFinished
// 通知（waitBuildFinished 同步等待时，收尾在该调用返回前完成）。
bool JZCodeAgent::buildProject(std::string &error)
{
    error.clear();
    if (m_buildRunning)
    {
        error = "已有打包/发布任务进行中，请等待其完成";
        return false;
    }

    string root = codeNormalizePath(m_codeConfig.workspace);
    if (root.empty())
    {
        error = "未设置工作区，无法打包/发布";
        return false;
    }
    if (!JZFileInfo::exists(root + "/tsconfig.json"))
    {
        error = "不是 TS 工程目录（缺少 tsconfig.json）: " + root;
        return false;
    }
    if (m_codeConfig.tscPath.empty())
    {
        error = "未配置宿主侧 tsc（CodeAgentConfig.tscPath），无法打包/发布工程。";
        return false;
    }
    if (!JZFileInfo::exists(m_codeConfig.tscPath))
    {
        error = "配置的宿主侧 tsc 不存在: " + m_codeConfig.tscPath;
        return false;
    }

    // 打包/发布前先依据工程内 .ui 文件重新生成 src/GenerateFiles/*.ts（每次重跑，
    // .ui 改动即时反映到编译产物）；生成失败则整体判失败，不启动 tsc。
    if (!generateUi(root, error))
        return false;

    // 异步启动 tsc 全量编译（tsc -p tsconfig.json -> outDir/*.js + *.js.map）
    JZProcess *proc = new JZProcess(this);
    proc->setProcessChannelMode(JZProcess::MergedChannels);
    proc->setWorkingDirectory(root);
    connect(&proc->sigFinished, this,
            [this, proc](int code, JZProcess::ExitStatus status)
            {
                onBuildFinished(proc, code, status);
            });
    if (!proc->start(m_codeConfig.tscPath, {"-p", "tsconfig.json", "--pretty", "false"}))
    {
        proc->deleteLater();
        error = "启动 tsc 失败: " + m_codeConfig.tscPath;
        return false;
    }

    m_buildProc = proc;
    m_buildRoot = root;
    m_buildOk = false;
    m_buildError.clear();
    m_buildRunning = true;

    // 墙钟超时保护（异步路径没有 waitBuildFinished 的兜底轮询）
    m_buildTimer = new JZTimer(this);
    connect(&m_buildTimer->sigTimeout, this, [this]()
            {
                if (!m_buildRunning || !m_buildProc)
                    return;
                JZProcess *proc = m_buildProc;
                proc->killProcess();
                onBuildFinished(proc, -1, JZProcess::CrashExit, true);
            });
    m_buildTimer->start(180000);

    addLog("info", "开始打包/发布工程: " + root + "\n(tsc -p tsconfig.json)");
    return true;
}

void JZCodeAgent::waitBuildFinished()
{
    if (!m_buildRunning || !m_buildProc)
        return;

    // 阻塞轮询；进程退出时 sigFinished 直连会同步触发 onBuildFinished（收尾在返回前完成）
    m_buildProc->waitForFinished(180000 + 10000);

    // 兜底：超时仍未结束则强杀并走超时收尾
    if (m_buildRunning && m_buildProc)
    {
        JZProcess *proc = m_buildProc;
        proc->killProcess();
        onBuildFinished(proc, -1, JZProcess::CrashExit, true);
    }
}

void JZCodeAgent::abortBuild()
{
    if (!m_buildRunning || !m_buildProc)
        return;

    if (m_buildTimer)
    {
        m_buildTimer->stop();
        m_buildTimer = nullptr;   // 父子关系随 agent 释放（可能处于析构路径，不 deleteLater）
    }
    JZProcess *proc = m_buildProc;
    m_buildProc = nullptr;
    m_buildRunning = false;
    m_buildOk = false;
    m_buildError = "打包/发布被终止";
    proc->killProcess();
    addLog("info", "已终止打包/发布进程: " + m_buildRoot);
    // 不发 sigBuildFinished：主动中止，等待方（工具 cancel / 停止任务）不依赖该通知
}

void JZCodeAgent::onBuildFinished(JZProcess *proc, int code, JZProcess::ExitStatus status, bool timeout)
{
    if (proc != m_buildProc)
        return;   // 过期回调（已被 abort/新任务顶替）

    if (m_buildTimer)
    {
        m_buildTimer->stop();
        m_buildTimer = nullptr;
    }

    string out = str_fromLocal(proc->readAllStandardOutput());
    m_buildProc = nullptr;
    m_buildRunning = false;
    proc->deleteLater();

    string root = m_buildRoot;
    bool ok = !timeout && status == JZProcess::NormalExit && code == 0;
    string err;
    if (timeout)
        err = "tsc 编译超时（180s）被终止。";
    else if (!ok)
    {
        const int kMaxErrChars = 8000;
        if ((int)out.size() > kMaxErrChars)
            out = out.substr(0, kMaxErrChars) + "\n...(编译输出过长已截断)";
        err = "编译失败:\n" + str_trimmed(out);
    }

    // 编译通过 -> 校验主入口产物：产物目录下必须有 main.js（src/main.ts 的产物；
    // 主入口统一为 main.js），存在才写 project.json（补充信息：编译时间、原路径
    // 地址...）；缺失则整体判失败。
    string entry = "main.js";
    if (ok)
    {
        // outDir 是 tsconfig 里的原值（可能相对工程根），产物目录要按它展开成绝对路径
        string outDir = tsOutDirOf(root, "build");
        bool absOut = !outDir.empty() && (outDir[0] == '/' || outDir[0] == '\\' ||
                      (outDir.size() > 1 && outDir[1] == ':'));
        string buildAbs = codeNormalizePath(absOut ? outDir : (root + "/" + outDir));
        string entryAbs = buildAbs + "/main.js";

        if (!JZFileInfo::exists(entryAbs) || !JZFileInfo(entryAbs).isFile())
        {
            ok = false;
            err = "编译通过但未找到主入口产物(main.js), 请保证 main.ts 编译产物为 main.js: " + entryAbs;
        }
        else
        {
            // project.json 与 main.js 同目录（= 运行根）：补充信息，不是入口声明。
            // 运行端用它做解析（projectRoot 让源路径能还原成 "src/x.ts"）。
            Json::Value meta;
            meta["entry"] = "main.js";                 // 主入口（固定）
            meta["name"] = codeBaseName(root);         // 工程名
            meta["projectRoot"] = root;                // 原路径地址：工程根
            meta["srcRoot"] = codeNormalizePath(root + "/" + tsRootDirOf(root));
            meta["outDir"] = outDir;                   // 产物输出目录（工程根外为绝对路径）
            meta["buildTime"] = tsNowString();         // 编译时间
            meta["tsc"] = m_codeConfig.tscPath;        // 编译时用的宿主 tsc

            Json::FastWriter writer;
            ofstream f(buildAbs + "/project.json", ios::binary | ios::trunc);
            if (!f)
            {
                ok = false;
                err = "写入 project.json 失败: " + buildAbs;
            }
            else
            {
                f << writer.write(meta);
                f.close();
            }
        }
    }

    m_buildOk = ok;
    m_buildError = ok ? string() : err;
    if (ok)
        addLog("info", "已打包/发布工程: " + root + "\n编译通过（tsc -p tsconfig.json）"
                       "\n入口产物: " + entry + "（tsconfig outDir 缺省=工程根/build）");
    else
        addLog("info", "打包/发布失败: " + root + "\n" + err);

    sigBuildFinished.emit(ok, ok ? string() : err);
}

// ------------------------------------------------------------------
// 导出辅助：把工程打包成可独立运行的发行目录
// ------------------------------------------------------------------
// 拷贝一律走框架 JZFile::copy / JZDir::copyRecursively（见 JZCore），本文件不再自己实现。

bool JZCodeAgent::exportProject(string &outDir, string &error, ExportConfig config)
{
    error.clear();
    outDir.clear();

    string root = codeNormalizePath(m_codeConfig.workspace);
    if (root.empty())
    {
        error = "未设置工作区，无法导出工程";
        return false;
    }
    if (!JZFileInfo::exists(root + "/tsconfig.json"))
    {
        error = "不是 TS 工程目录（缺少 tsconfig.json）: " + root;
        return false;
    }

    // 0) 已有打包在进行中则先等它收尾，避免并发打包
    if (m_buildRunning)
        waitBuildFinished();

    // 1) 确保已打包/发布：优先重新编译（保证 build/ 与 project.json 最新）。
    //    未配置宿主侧 tsc 时退化为"已有 project.json 即可"（依赖先前产物）。
    if (!m_codeConfig.tscPath.empty())
    {
        if (!buildProject(error))
            return false;
        waitBuildFinished();               // 打包是异步的，导出前等收尾（含入口校验）
        if (!m_buildOk)
        {
            error = m_buildError.empty() ? "打包/发布失败" : m_buildError;
            return false;
        }
    }
    else if (!JZFileInfo::exists(tsBuildAbsOf(root) + "/main.js"))
    {
        error = "未配置宿主侧 tsc 且工程尚未打包/发布（产物目录缺少 main.js），无法导出。";
        return false;
    }

    // 2) 导出目录 = <工程根>/export，先清空再写入，保证一次性干净产物
    outDir = root + "/export";
    if (JZFileInfo::exists(outDir))
    {
        JZDir old(outDir);
        if (!old.removeRecursively())
        {
            error = "无法清空旧导出目录: " + outDir;
            return false;
        }
    }
    if (!JZDir(outDir).mkpath(outDir))
    {
        error = "无法创建导出目录: " + outDir;
        return false;
    }

    // 3) 复制编译产物到导出根：主入口统一为 main.js，而运行根 = exe 所在目录，
    //    所以产物（main.js + *.js + *.js.map + project.json）直接铺在导出根，
    //    不再套一层 build/ —— 否则运行时在导出根下找不到 main.js。
    string buildAbs = tsBuildAbsOf(root);
    if (!JZFileInfo::exists(buildAbs) || !JZFileInfo(buildAbs).isDir())
    {
        error = "找不到编译产物目录: " + buildAbs;
        return false;
    }
    if (!JZFileInfo::exists(buildAbs + "/main.js"))
    {
        error = "产物目录缺少主入口 main.js: " + buildAbs;
        return false;
    }
    // 导出目录刚清空重建，不会有同名文件 -> force=false
    if (!JZDir::copyRecursively(buildAbs, outDir, false, &error))
        return false;

    // 4) 复制内嵌 JZEngineRuntime 的 code_runtime.exe（与 code_agent 同目录安装），
    //    并按需改名 / 替换图标，让连接后的 code_runtime 看起来是独立程序。
    string runtimeExe = codeNormalizePath(JZPlatform::applicationDirPath()) + "/code_runtime.exe";
    if (!JZFileInfo::exists(runtimeExe))
    {
        error = "找不到 code_runtime.exe（应随 code_agent 一起安装到 exe 目录）: " + runtimeExe;
        return false;
    }
    // 目标运行时文件名：去扩展名后拼接 .exe（缺省 = code_runtime）
    string targetName = "code_runtime";
    string nameTrim = config.exeName;
    while (!nameTrim.empty() && (nameTrim.back() == '.' || nameTrim.back() == ' ' ||
                                 nameTrim.back() == '\t'))
        nameTrim.pop_back();
    if (!nameTrim.empty())
        targetName = nameTrim;
    string dstExe = outDir + "/" + targetName + ".exe";
    if (!JZFile::copy(runtimeExe, dstExe, false, &error))
        return false;
    if (!config.iconPath.empty() && JZFileInfo::exists(config.iconPath))
    {
        string resErr;
        if (!JZExeResource::replaceIcon(dstExe, config.iconPath, resErr))
        {
            error = "替换 exe 图标失败: " + resErr;
            return false;
        }
    }

    // 5.1) 写入"调试开关"资源（约定 type="JZRT_CFG" name="DEBUG"，JSON {"debug":bool}）。
    //      缺省 true：导出的运行时默认以 debug 版本发布，运行时自身可读此资源决定是否开启 JZRuntimeDebugger。
    {
        string resErr;
        if (!JZExeResource::setDebugFlag(dstExe, config.debug, resErr))
        {
            error = "写入调试开关资源失败: " + resErr;
            return false;
        }
    }

    // 6) 一键启动脚本：用（已改名/已替换图标的）运行时 exe 运行本目录工程
    {
        string bat = "@echo off\r\n"
                     "rem Run the exported JZ project (the exe embeds JZEngineRuntime)\r\n"
                     "\"%~dp0" + targetName + ".exe\" \"%~dp0\"\r\n";
        ofstream f(outDir + "/run.bat", ios::binary | ios::trunc);
        if (f)
        {
            f.write(bat.data(), (streamsize)bat.size());
            f.close();
        }
    }

    addLog("info", "已导出可独立运行包: " + outDir + "\n"
                   "（含 " + targetName + ".exe 内嵌 JZEngineRuntime（可改名/换图标）"
                   "+ 编译产物 main.js 等 + project.json；双击 run.bat 即可在本机运行）");
    return true;
}

bool JZCodeAgent::listFiles(const string &rel, vector<string> &out, string &error) const
{
    return codeListDir(m_codeConfig.workspace, rel, out, error);
}

JZResult<string> JZCodeAgent::readFile(const string &path)
{
    string abs;
    string err;
    if (!codeResolvePath(m_codeConfig.workspace, path, abs, err))
        return JZResult<string>::Error(err);

    // 直读磁盘，进程内不再做缓存。理由：OS 页缓存已经在做同一件事且失效是正确的；
    // 自建 LRU 还要额外背一套"变更失效"协议，而它的失效机制有洞（非 Windows 平台
    // JZFileSystemWatcher 是空壳、授信目录不在监视范围、自己写完不同步失效），
    // 一旦命中陈旧内容，modify_file 会基于旧内容整体回写，静默覆盖掉外部改动。
    JZFile file;
    if (!file.open(abs, "rb"))
        return JZResult<string>::Error("打开文件失败: " + abs);
    JZByteArray read = file.readAll();
    file.close();

    return JZResult<string>::Ok(str_fromUtf8(read));
}

bool JZCodeAgent::readFileText(const string &rel, string &text, string &error)
{
    // 网页端只读预览：越界 / 2MB / 二进制 限制与改造前一致；实际读取走 readFile。
    string abs;
    if (!codeResolvePath(m_codeConfig.workspace, rel, abs, error))
        return false;
    JZFileInfo info(abs);
    if (!info.exists() || !info.isFile())
    {
        error = "文件不存在: " + rel;
        return false;
    }
    if (info.size() > 2 * 1024 * 1024)
    {
        error = "文件过大(>2MB): " + rel;
        return false;
    }
    if (!isTextFile(abs))
    {
        error = "二进制文件无法预览: " + rel;
        return false;
    }

    JZResult<string> r = readFile(abs);
    if (r.isError())
    {
        error = r.errorMessage();
        return false;
    }
    text = r.value();
    return true;
}

// ==========================================================================
// JZCodeAgentToolHost —— 工具宿主(组合, 不让 JZCodeAgent 多继承 ICodeToolHost)
// 通用工具库(binary/agent_tool)只认 ICodeToolHost, 不认 CodeAgentConfig, 这里是两者的桥。
// ==========================================================================

JZCodeAgentToolHost::JZCodeAgentToolHost(JZCodeAgent *agent)
    : m_agent(agent)
{
}

void JZCodeAgentToolHost::setTerminalConfig(const JZVirtualTerminalConfig &config)
{
    m_config = config;
}

bool JZCodeAgentToolHost::toolReadFile(const std::string &absPath, std::string &text, std::string &error)
{
    if (!m_agent)
    {
        error = "工具宿主已失效";
        return false;
    }

    JZResult<std::string> r = m_agent->readFile(absPath);
    if (r.isError())
    {
        error = r.errorMessage();
        return false;
    }
    text = r.value();
    return true;
}

bool JZCodeAgentToolHost::toolIsBuildRunning() const
{
    return m_agent && m_agent->isBuildRunning();
}

bool JZCodeAgentToolHost::toolBuildProject(std::string &error)
{
    if (!m_agent)
    {
        error = "工具宿主已失效";
        return false;
    }
    return m_agent->buildProject(error);
}

JZSignal<bool, const std::string &> *JZCodeAgentToolHost::toolBuildFinished()
{
    return m_agent ? &m_agent->sigBuildFinished : nullptr;
}

void JZCodeAgent::applyZoneConfig()
{
    // 默认策略(工作区读+写+执行 / 授信目录读+执行 / 区外只读) + 本工程的目录边界
    JZVirtualTerminalConfig config;
    config.zone.workspace = m_codeConfig.workspace;
    config.zone.trustedDirs = m_codeConfig.trustedDirs;

    // 注意: 这一份是**给工具看的**区域视图, 必须与 CodeAgentConfig 保持同步 ——
    // JZCodeTool::checkPathPermission 拿的就是它, 漏刷新等于权限误判。
    m_toolHost.setTerminalConfig(config);

    // 终端是真正的执行面, 区域与策略同样要同步过去：
    // 重定向目标和 edit_file 的越界判定都靠它。
    // setTerminalConfig() 会把已有会话的 cwd 拉回工作区根 —— 换工作区时正好需要。
    if (m_terminal)
        m_terminal->setTerminalConfig(m_toolHost.toolTerminalConfig());
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

    // agent 重建 = 一次新任务起点：清空恢复闭环的瞬时状态（注入的 verify/策略保留）
    m_recoveryActive = false;
    m_recoveryAttempt = 0;
    m_lastUserRequest.clear();
    m_lastUserImages.clear();

    // 重建前终止进行中的打包/发布（tsc 子进程随旧任务作废）
    abortBuild();

    // 重建前丢弃上一轮的 TSGO 语言服务进程与未决请求（配置可能已变化）
    if (m_tsServer)
        m_tsServer->shutdown();

    m_agent = new JZAgent(this);
    m_agent->setStreamEnabled(true);
    m_agent->setThinkEnabled(false);

    m_llmConfig.reuseConnection = false;
    m_agent->init(m_llmConfig);

    connect(&m_agent->sigReply, this, &JZCodeAgent::onReply);
    connect(&m_agent->sigThinking, this, &JZCodeAgent::onThinking);
    connect(&m_agent->sigError, this, &JZCodeAgent::onError);
    connect(&m_agent->sigFinished, this, &JZCodeAgent::onFinished);
    connect(&m_agent->sigToolCall, this, &JZCodeAgent::onToolCall);
    connect(&m_agent->sigToolResult, this, &JZCodeAgent::onToolResult);

    auto addTool = [this](JZCodeTool *tool)
    {
        tool->setHost(&m_toolHost);
        m_agent->addTool(tool);
        m_tools.push_back(tool);
        m_toolNames.push_back(tool->toolName());
    };

    // ---- 文件操作 ----
    // ls / cat / mkdir / rm / cp / mv / grep 都是 shell 命令, 由 terminal_run 直接覆盖,
    // 不再各做一个工具(两套做法只会让模型不知道该用哪个)。
    // 专门的文件工具只留三个: read_file(带行号/分段读, 避免大文件灌上下文)、
    // write_file(整文件覆盖)、modify_file(局部替换, sed 不校验唯一匹配, 且回上下文片段)。
    if (!m_terminal)
    {
        m_terminal = new JZVirtualTerminal(this);
        m_terminal->setId("default");   // 单用户终端的会话 id(供 terminal_open/list 展示)
    }
    addTool(new JZTerminalRunTool(m_terminal));
    addTool(new JZTerminalQueryTool(m_terminal));
    addTool(new JZTerminalCancelTool(m_terminal));
    addTool(new JZTerminalOpenTool(m_terminal));
    addTool(new JZTerminalListTool(m_terminal));
    addTool(new JZTerminalCloseTool(m_terminal));
    addTool(new JZReadFileTool());
    addTool(new JZWriteFileTool());
    addTool(new JZModifyFileTool());
    addTool(new JZSearchFileTool());
    addTool(new JZGetDateTimeTool());

    auto shell_tool = new JZShellTool();
    shell_tool->setExeSearchPaths({ jzApp->applicationDirPath() + "/ts_compiler/tsc" });
    addTool(shell_tool);

    // build_project：打包/发布当前工作区（异步 tsc + main.js 入口校验 + project.json），
    // 是 run_tool 的前置步骤；编译错误直接回给模型自修。
    addTool(new JZBuildProjectTool());

    // run_js：用随包安装的 code_runtime 运行编译产物（exe 与 code_agent 同目录，
    // 见 exportProject 的同款定位）；args 透传给运行时（如工程根目录、--debug）。
    // 未编译的工程应先用 build_project 打包。
    string runtimeExe = codeNormalizePath(JZPlatform::applicationDirPath()) + "/code_runtime.exe";
    // 运行根 = 产物目录（main.js 所在目录），按 tsconfig 的 outDir 定位
    string run_work_dir = tsBuildAbsOf(codeNormalizePath(m_codeConfig.workspace));
    auto run_tool = new JZCommandTool();
    run_tool->setup("run_js", "run_js 加载 main.js 并执行, --debug 开启调试模式", runtimeExe, true, run_work_dir);
    addTool(run_tool);

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

    applyZoneConfig();

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

    // 直连 tsc 语言服务（TSGO）的 TS 工具：首次调用时惰性拉起 "tsc --lsp --stdio" 进程，
    // 对外暴露 7 个「查找信息」型查询（ts_source / ts_references / ts_quickinfo /
    // ts_diagnostics / ts_outline / ts_members / ts_symbols）与 8 个结构化编辑工具
    // （ts_summary / ts_add_class / ts_rename_class / ts_delete_class /
    //   ts_add_function / ts_rename_function / ts_replace_function / ts_delete_function），
    // 全部按符号名定位，不要求模型提供行列。
    // 配置随工作区/编译器的变化在 buildAgent 里刷新（进程在 shutdown 后下次请求自动重启）。
    if (m_tsServer)
    {
        m_tsServer->setTsgoPath(m_codeConfig.tscPath);
        m_tsServer->setRoot(m_codeConfig.workspace);
        for (TsMethod m : {TsMethod::Source, TsMethod::References, TsMethod::QuickInfo,
                           TsMethod::Diagnostics, TsMethod::Outline, TsMethod::Members,
                           TsMethod::WorkspaceSymbols})
            addTool(new JZTsTool(m_tsServer, m));

        using K = JZTsEditTool::Kind;
        for (K k : {K::Summary, K::AddClass, K::RenameClass, K::DeleteClass,
                    K::AddFunction, K::RenameFunction, K::ReplaceFunction, K::DeleteFunction})
            addTool(new JZTsEditTool(m_tsServer, k, &m_toolHost));
    }

    m_agent->setSystemPrompt(buildSystemPrompt());
}

void JZCodeAgent::abortTools()
{
    // 宿主侧的编译把关进程也要一并终止, 防止其回调在 agent 重建后误发续修
    abortHostTsc();
}

void JZCodeAgent::addLog(const string &type, const string &text, const string &name,
                         const vector<string> &images)
{
    CodeAgentLog log;
    log.id = ++m_logId;
    log.type = type;
    log.name = name;
    log.text = text;
    log.images = images;
    m_logs.push_back(log);

    const int kMaxLogs = 3000;
    if ((int)m_logs.size() > kMaxLogs)
        m_logs.erase(m_logs.begin(), m_logs.begin() + (m_logs.size() - kMaxLogs));
}

std::string JZCodeAgent::buildSystemPrompt() const
{
    std::ostringstream text;
    text << "你是一个运行在本地的代码助手 JZCodeAgent，用于 TypeScript 项目："
            "可查看、搜索、修改、用 tsc 编译并运行工作区中的代码。\n";
    text << "工作区:" + m_codeConfig.workspace + "\n";
    text << "这是标准 TypeScript 工程：根目录有 tsconfig.json，编译产物统一输出到 build/。\n";
    text << "编辑/查定义/引用/类型/诊断等语言信息时，优先调用 ts_* 系列工具（直连 tsc 语言服务）；"
            "改代码前用 ts_summary / ts_source / ts_outline 了解结构，而不是盲目翻文件。\n";
    text << "修改 TS 代码优先使用结构化编辑工具：新增用 ts_add_class / ts_add_function，"
            "改函数体用 ts_replace_function，重命名用 ts_rename_class / ts_rename_function"
            "（同步全项目引用），删除用 ts_delete_class / ts_delete_function；"
            "结构摘要用 ts_summary。只有无法按符号定位的局部小改动（注释、单行字面量）"
            "才退回 modify_file。\n";
    text << "代码风格class优先, 使用面向对象的风格进行编码\n";
    // 运行时边界(不是 Node: 没有 require / 裸名只能默认导入 / 监听后要留心跳定时器)
    text << "\n" << jzTsRuntimeNotes();
    // 起服务/长任务怎么跑(terminal_run 的 background / query / cancel): 与 agent_cli 同一份,
    // 否则这里注册了 background 能力却没告诉模型怎么用(它会拿很久的 timeout 前台干等)。
    text << "\n" << jzTerminalRunNotes();

    return text.str();
}

void JZCodeAgent::onReply(const string &text)
{
    addLog("delta", text);
}

void JZCodeAgent::onThinking(const string &text)
{
    addLog("thinking", text);
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

    // 无排队指令: 本轮回复真正结束。若该轮改过 TS 代码, 宿主先做一次编译把关
    // (可能自动续修一轮); 只有确认不需要检查时才在此收尾。
    if (startHostTscCheck())
        return;

    nextOrIdle();
}

void JZCodeAgent::nextOrIdle()
{
    // 收尾统一入口: 某个执行单元(模型一轮回复 / 宿主把关完成)结束后调用。
    // 期间若用户又发过指令, 继续发送下一条; 否则整个空闲下来。
    // 落盘 / 计数由上层在检测到空闲时通过 TaskHistoryManager 完成。
    m_busy = false;

    // 任务级自动恢复：仅在外层任务进行中、且仍有恢复额度时介入
    if (m_recoveryEnabled && m_recoveryActive)
    {
        bool pass = !m_recovery.verify || m_recovery.verify();
        if (pass)
        {
            m_recoveryActive = false;
            sigRecoveryFinished.emit(true, m_recoveryAttempt);
            return;
        }
        if (m_recoveryAttempt >= m_recovery.maxAttempts)
        {
            m_recoveryActive = false;
            sigRecoveryFinished.emit(false, m_recoveryAttempt);
            return;
        }
        // 构造恢复请求：buildRecoveryRequest 返回空串表示"重发最近一次原始 user request"
        string recoveryText;
        if (m_recovery.buildRecoveryRequest)
            recoveryText = m_recovery.buildRecoveryRequest(m_recoveryAttempt + 1);
        // 空串 = 重发最近一次原始 user request(连同它附带的图片一起重发)
        const bool resendOriginal = recoveryText.empty();
        if (resendOriginal)
            recoveryText = m_lastUserRequest;
        m_recoveryAttempt++;
        // 复用 request：仍会走工具调用 / 宿主编译把关；m_recoveryActive 已 true 故不重置计数
        request(recoveryText, resendOriginal ? m_lastUserImages : std::vector<string>());
        return;
    }
}

// ---- 轮末宿主编译把关 ----
bool JZCodeAgent::startHostTscCheck()
{
    if (m_stopping || m_hostRunning || m_codeConfig.tscPath.empty())
        return false;
    if (!m_roundTouchedTs)
        return false;   // 纯问答轮/没动过代码: 不检查
    // 续修额度为 0 也要再跑一次: 那是最后一轮修复结束, 用于终检上报
    // (失败时 onHostTscDone 因额度为 0 只上报不续修)
    if (!JZFileInfo::exists(m_codeConfig.tscPath))
    {
        addLog("info", "宿主编译把关已启用但找不到 tsc: " + m_codeConfig.tscPath);
        return false;
    }
    if (!JZFileInfo::exists(m_codeConfig.workspace + "/tsconfig.json"))
        return false;   // 没有 tsconfig 无从编译, 跳过

    m_roundTouchedTs = false;   // 本轮改动已进入检查, 避免重复触发

    JZProcess *proc = new JZProcess(this);
    proc->setWorkingDirectory(m_codeConfig.workspace);
    proc->setProcessChannelMode(JZProcess::MergedChannels);
    connect(&proc->sigFinished, this,
            [this, proc](int code, JZProcess::ExitStatus status)
            {
                if (proc != m_hostProc || !m_hostRunning)
                    return;   // 已被 abort 的旧进程, 忽略
                bool timeout = m_hostTimeout;
                string output = str_fromLocal(proc->readAllStandardOutput());
                proc->deleteLater();
                m_hostProc = nullptr;
                m_hostRunning = false;
                m_hostTimeout = false;
                if (m_hostTimer)
                {
                    m_hostTimer->stop();
                    m_hostTimer->deleteLater();
                    m_hostTimer = nullptr;
                }
                onHostTscDone(status == JZProcess::NormalExit && code == 0, output, timeout);
            });
    m_hostProc = proc;
    m_hostRunning = true;
    m_hostTimeout = false;

    // 兜底超时: 120s 后强制结束(走上面的 sigFinished 回调)
    m_hostTimer = new JZTimer(this);
    connect(&m_hostTimer->sigTimeout, this, [this]()
            {
                m_hostTimeout = true;
                if (m_hostProc)
                    m_hostProc->killProcess();
            });
    m_hostTimer->start(120000);

    if (!proc->start(m_codeConfig.tscPath, {"--noEmit", "-p", "tsconfig.json"}))
    {
        addLog("info", "宿主编译把关启动 tsc 失败: " + m_codeConfig.tscPath);
        proc->deleteLater();
        m_hostProc = nullptr;
        m_hostRunning = false;
        if (m_hostTimer)
        {
            m_hostTimer->stop();
            m_hostTimer->deleteLater();
            m_hostTimer = nullptr;
        }
        return false;
    }
    return true;
}

void JZCodeAgent::onHostTscDone(bool ok, const string &output, bool timeout)
{
    if (m_stopping)
    {
        m_busy = false;
        return;
    }

    if (ok)
    {
        addLog("info", "宿主编译检查通过 (tsc --noEmit)。");
        nextOrIdle();
        return;
    }

    string errText = str_trimmed(output);
    if (timeout)
        errText = (errText.empty() ? string("编译超时被终止。")
                                   : string("编译超时被终止。\n") + errText);
    const int kMaxErrChars = 12000;
    if ((int)errText.size() > kMaxErrChars)
        errText = errText.substr(0, kMaxErrChars) + "\n...(编译输出过长已截断)";

    if (m_fixRoundsLeft > 0)
    {
        // 自动续修: 额度减一, 把编译错误作为一条指令直接发给模型。
        // 期间用户若发过新指令则不再续修(旧任务收尾, 让用户指令先走)。
        m_fixRoundsLeft--;
        addLog("info", "宿主编译未通过，已自动请求模型修复（本任务剩余自动修复轮数: " +
                           to_string(m_fixRoundsLeft) + "）。");
        string fix = "宿主自动检查：tsc --noEmit 编译未通过，请修复下面的错误后再编译，直到通过为止。\n" + errText;
        addLog("user", fix);
        sendRequestDirect(fix);
        return;
    }

    // 额度用尽(或用户已插队): 收尾并把剩余错误上报给用户
    if (m_fixRoundsLeft <= 0)
        addLog("info", "宿主编译未通过，且自动修复轮数已用完，剩余错误如下（可继续向助手下达修复指令）:\n" + errText);
    nextOrIdle();
}

void JZCodeAgent::abortHostTsc()
{
    if (m_hostTimer)
    {
        m_hostTimer->stop();
        m_hostTimer->deleteLater();
        m_hostTimer = nullptr;
    }
    if (m_hostProc)
    {
        JZProcess *p = m_hostProc;
        m_hostProc = nullptr;
        p->killProcess();
        p->deleteLater();
    }
    m_hostRunning = false;
    m_hostTimeout = false;
}

void JZCodeAgent::setRecovery(const JZCodeAgentRecovery &r)
{
    m_recovery = r;
    // maxAttempts<=1 视为不启用（单次尝试即终局，无需恢复闭环）
    m_recoveryEnabled = r.maxAttempts > 1;
    // 不在此重置 m_recoveryActive：新任务由首次 request() 重新计数
}

void JZCodeAgent::clearRecovery()
{
    m_recovery = JZCodeAgentRecovery();
    m_recoveryEnabled = false;
    m_recoveryActive = false;
    m_recoveryAttempt = 0;
    m_lastUserRequest.clear();
    m_lastUserImages.clear();
}

void JZCodeAgent::onToolCall(const ToolCall& call)
{
    addLog("tool_call", "", call.function_name + "$$tool_gap$$" + call.id);
}

void JZCodeAgent::onToolResult(const ToolCallResult& result)
{
    addLog("tool_result", result.result, result.id);
}