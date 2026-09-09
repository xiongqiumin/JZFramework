#ifndef JZ_CODE_AGENT_H_
#define JZ_CODE_AGENT_H_

#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "JZAgent.h"
#include "JZAgentTool.h"
#include "JZCodeTools.h"
#include "JZTsLangServer.h"
#include "JZFileWatch.h"
#include "JZFileSystemWatcher.h"
#include "JZResult.h"

// 代码助手自身的配置（与大模型配置 JZAgentConfig 分开）
class CodeAgentConfig
{
public:
    JZAgentConfig llmConfig;

    std::string workspace;                    // 工作区根目录，可读写
    std::vector<TrustedDir> trustedDirs;     // 授信目录（编译器/SDK 等），默认只读，可单独标记可写
    std::vector<TrustedProgram> trustedPrograms;  // 显式指定的可信程序（全路径），优先于目录搜索

    // 轮末宿主编译把关（默认关闭，tscPath 非空才会启用）。
    // 模型每轮回复结束、且该轮改过 TS 相关文件时，宿主用 "tsc --noEmit -p tsconfig.json" 快检；
    // 未通过则自动再发一轮修复指令，直到通过或达到 maxAutoFixRounds（超出则把剩余错误上报用户）。
    std::string tscPath;            // 宿主侧 tsc 编译器全路径；空 = 关闭宿主把关
    int maxAutoFixRounds = 2;       // 自动续修轮数上限；<=0 = 即使编译失败也只上报不续修

    std::string webSearchUrl;                // 网络搜索接口模板(含 {q} 占位符)；空则用内置默认(Bing RSS, format=rss)

    // HTTP 代理（http/https 目标均生效，https 走 CONNECT 隧道）
    std::string proxyHost;
    int proxyPort = 0;
    std::string proxyUser;
    std::string proxyPassword;
};

// 目录作为“TS 工程”的探测结果。
// 工程判定只看根目录的 tsconfig.json（编译规则入口），不引入 npm/package.json 概念。
// 产物约定：编译/构建产物统一放在 <工程>/build/（initProject 生成的标准 tsconfig 的 outDir = "build"）。
struct CodeProjectStatus
{
    bool pathOk = false;         // 目录存在
    bool isProject = false;      // 是 TS 工程目录（根目录有 tsconfig.json）
    bool hasTsconfig = false;    // 根目录有 tsconfig.json（可被 tsc 编译/把关）
    bool isEmpty = false;        // 目录没有任何内容
    std::string name;            // 目录名（可作工程名）
    std::string error;           // pathOk=false 时的原因
    std::string summary;         // 中文一行摘要（供日志 / 界面展示）
};



// 任务级自动恢复（可注入）：把「编译/运行/判定 → 反馈重试」的闭环下沉到 agent 自身。
// 任务收尾（宿主编译把关之后）调用 verify；失败则按策略自动重发指令
// （重发原始 user request 或自定义内容），直到通过或达到 maxAttempts。
// 典型用途：评测用例自动重试、关键任务自愈。
struct JZCodeAgentRecovery
{
    int maxAttempts = 1;   // 含首次的总轮数；<=1 表示不启用自动恢复

    // 任务结束时的验证回调：返回 true=通过（无需恢复）；false=失败（需触发恢复）。
    // 由上层注入（例如 benchmark 在此做 build+run+check）；为空视为永远通过。
    std::function<bool()> verify;

    // 恢复时下发的内容构造器：attempt 为即将进行的轮次（从 2 起）。
    // 返回空串表示「重发最近一次原始 user request」；否则下发返回内容
    // （例如把编译错误/实际输出拼进去作为修复指引）。
    std::function<std::string(int attempt)> buildRecoveryRequest;
};

// 一条会话日志，网页端按 id 增量拉取
struct CodeAgentLog
{
    // user         用户提问
    // thinking     思考过程片段(流式增量, 实时上抛, 不入历史文件)
    // delta        大模型回复的文本片段(流式增量)
    // assistant    整条助手回复(历史重建用)
    // tool_call    工具被调用
    // tool_result  工具返回
    // error        出错
    // info         提示信息
    std::string type;
    std::string name;   // 工具名
    std::string text;
    int id = 0;

    Json::Value toJson() const;
};

struct ExportConfig
{
    std::string exeName = "";
    std::string iconPath = "";
    bool debug = true;
};

// 代码助手：把 JZAgent 与一组代码工具(编译/运行/改代码/找代码)组装起来，
// 并维护一份可供网页端增量拉取的会话日志。
class JZCodeAgent : public JZObject
{
public:
    JZCodeAgent(JZObject *parent = nullptr);
    ~JZCodeAgent();

    void init(const CodeAgentConfig &code);
    void request(const std::string &text);
    void stop();
    void clearHistory();
    bool isBusy() const;

    // 会话日志（id 单调递增，网页端用 since 做增量拉取）
    int logSeq() const;
    const std::vector<CodeAgentLog> &logs() const;
    JZAgentTokenUsage usage() const;

    const CodeAgentConfig &codeConfig() const;
    const JZAgentConfig &llmConfig() const;
    bool setWorkspace(const std::string &path);

    // ---- TS 工程支持 ----
    // 工程目录 = 根目录有 tsconfig.json（编译规则入口）；不引入 npm/package.json 概念。
    // 标准 TS 工程编译产物统一输出到 <工程>/build/。
    // 只读探测 path 是不是工程目录（不改工作区、不写文件）
    CodeProjectStatus probeProject(const std::string &path) const;
    // 打开 path 作为新工作区（切换对话/存档语义同 setWorkspace），
    // 探测结果经 status 返回；目录不存在时返回 false
    bool openProject(const std::string &path, CodeProjectStatus *status, std::string &error);
    // 关闭当前工程：给当前工作区存档（下次打开/启动恢复时自动接上对话），清掉运行态，
    // 并把 workspace 置空（下次 openProject 才会真正重建 agent 并导入存档）。
    // 注意： MainWindow 侧的 m_cfg.workspace 不变，它同时承担"下次启动恢复哪条"的职责。
    void closeProject();
    // 把 path 初始化为最小可编译的标准 TS 工程骨架
    // （生成 tsconfig.json(outDir=build)/.gitignore，src 为空时补 src/main.ts；
    //   已有文件一律不覆盖，可幂等重复调用补齐缺项。
    //   注意: 不生成 project.json —— 它是"打包/发布"时构建产物的元数据，见 buildProject）
    bool initProject(const std::string &path, std::string &error);

    // 打包/发布当前工作区工程：异步启动宿主 tsc 全量编译（tsc -p tsconfig.json），
    // 编译结束后自动校验入口产物（tsconfig outDir 下的 main.js，缺省 build/main.js），
    // 存在才生成/刷新运行主入口声明 project.json，否则整体判失败。
    // 启动成功返回 true（前置校验失败返回 false 并填 error）；结果经 sigBuildFinished
    // 通知，或 waitBuildFinished() 同步等待后用 lastBuildOk/lastBuildError 查询。
    bool buildProject(std::string& error);
    // 阻塞等待进行中的打包/发布结束（无进行中任务时立即返回）；
    // 收尾（入口校验 + project.json 写入）在本调用返回前完成
    void waitBuildFinished();
    // 打包/发布是否进行中（工具并发保护/界面状态）
    bool isBuildRunning() const { return m_buildRunning; }
    // 最近一次打包/发布结果（waitBuildFinished 或 sigBuildFinished 之后读取）
    bool lastBuildOk() const { return m_buildOk; }
    const std::string &lastBuildError() const { return m_buildError; }
    // 终止进行中的打包/发布（重建 agent / 析构时调用）
    void abortBuild();

    // 把当前工作区工程"导出"为可独立运行的发行包：重新打包/发布后，复制编译产物(build/ 下的
    // *.js + *.js.map) + 运行主入口声明 project.json + 内嵌 JZEngineRuntime 的
    // code_runtime.exe 到 <工程根>/export，并生成一键启动的 run.bat。
    //   - config.exeName：导出运行时 exe 的文件名（不含扩展名，缺省/空=code_runtime）。
    //   - config.iconPath：可选的 .ico 路径，非空且存在则把该图标写入导出的运行时 exe
    //               （用 JZExeResource 替换资源图标，使连接后的 code_runtime 包含自定义 exe 图标）。
    //   - config.debug：是否写入"调试开关"资源（缺省 true）。运行时自身读取该资源决定是否开启
    //            JZRuntimeDebugger（基于 JZMcpServer 的 HTTP 调试器）。
    // outDir 返回导出目录（绝对路径）；失败返回 false，error 说明原因。
    bool exportProject(std::string &outDir, std::string &error, ExportConfig config);

    // 最近一次 openProject 的探测结果（未 open 过则为默认值）
    const CodeProjectStatus &projectStatus() const;

    // 任务级自动恢复（可注入 verify + 恢复重发策略）；maxAttempts<=1 视为不启用
    void setRecovery(const JZCodeAgentRecovery &r);
    void clearRecovery();

    // 更新大模型配置（保留会话日志，重建底层 agent）
    void applyLlmConfig(const JZAgentConfig &llm);
    JZAgent *agent();

    // 把 agent 里的对话历史重建到会话日志（导入存档后界面要用）
    void rebuildLogsFromHistory();

    // ---- 对话存取（"任务"这层由 TaskHistoryManager 管，这里只负责读写一段对话） ----
    // 把当前对话存到 path（任务存档路径由 TaskHistoryManager::archivePath() 给出）
    bool saveConversation(const std::string &path) const;
    // 从 path 载入一段对话（替换当前对话）并按它重建会话日志
    bool loadConversation(const std::string &path);
    // 中断进行中的任务并重置为一段干净的新对话（切换/新建任务时用）
    void resetConversation();

    // 文件浏览（网页端复用与工具一致的路径校验）
    bool listFiles(const std::string &rel, std::vector<std::string> &out, std::string &error) const;
    // 统一的文件读取入口：解析路径（相对工作区则拼接，绝对路径做越界校验）、走 LRU 缓存、
    // 文件变更由 m_fileWatcher 自动失效。读取失败（越界 / 打开失败）返回 Error，否则返回内容。
    JZResult<std::string> readFile(const std::string &path);

    // 网页端只读预览：在 readFile 之上做 2MB / 二进制 限制，行为保持与改造前一致
    bool readFileText(const std::string &rel, std::string &text, std::string &error);

    const std::vector<std::string> &toolNames() const;
    bool isTrustedDir(std::string dir) const;
    bool isTrustedProgram(std::string prog) const;

    // 文件内容缓存（默认 20MB LRU）。工具读文件时优先命中，文件变更由 m_fileWatcher 自动失效。
    JZFileWatch* fileCache() { return &m_fileCache; }

    // 任务级自动恢复序列结束通知：(passed, attempts)
    // 打包/发布结束通知：(ok, error)；ok=false 时 error 为失败原因/编译输出
jz_signals:
    JZSignal<bool, int> sigRecoveryFinished;
    JZSignal<bool, const std::string &> sigBuildFinished;

    // [keep-index]
protected:
    void buildAgent();
    void abortTools();
    // 把当前配置转成虚拟终端的区域配置（工作区 + 授信目录）
    void applyZoneConfig();
    void addLog(const std::string &type, const std::string &text, const std::string &name = std::string());
    std::string buildSystemPrompt() const;

    void onReply(const std::string &text);
    void onThinking(const std::string &text);
    void onError(const std::string &text);
    void onFinished();

    void onToolCall(const ToolCall &call);
    void onToolResult(const ToolCallResult& result);

    // ---- 打包/发布（异步）----
    void onBuildFinished(JZProcess *proc, int code, JZProcess::ExitStatus status, bool timeout = false);

    // ---- 轮末宿主编译把关 ----
    bool startHostTscCheck();                                  // 符合条件则异步起 tsc --noEmit; true=已进入检查
    void onHostTscDone(bool ok, const std::string &output, bool timeout);  // 检查结果: 通过收尾 / 失败续修
    void abortHostTsc();                                       // 终止进行中的宿主检查(停止/清空/析构时)
    void sendRequestDirect(const std::string &text);           // 绕过排队把一条指令直接发给 agent(宿主续修用)
    void nextOrIdle();                                         // 收尾统一入口: 队列有排队指令就继续发, 否则置空闲

    JZAgent *m_agent = nullptr;
    JZAgentConfig m_llmConfig;
    CodeAgentConfig m_codeConfig;
    CodeProjectStatus m_project;   // 最近一次 openProject 的探测结果

    std::vector<JZCodeTool *> m_tools;
    std::vector<std::string> m_toolNames;

    JZTsLangServer *m_tsServer = nullptr;   // 直连 TSGO 语言服务；随 agent 生命周期常驻

    // 文件内容缓存（20MB LRU）与文件系统监视器：工具读文件优先命中缓存，
    // 监视工作区变更并自动失效对应缓存条目（见 setupFileWatch）。
    JZFileWatch m_fileCache;
    JZFileSystemWatcher m_fileWatcher;

    void setupFileWatch();

    // 所有网络工具（web_search / web_fetch）共享的访问管理器，代理只在此统一设置一次
    JZNetworkAccessManager *m_netMan = nullptr;

    std::vector<CodeAgentLog> m_logs;
    int m_logId = 0;
    bool m_busy = false;
    bool m_stopping = false;

    // 轮末宿主把关状态
    bool m_roundTouchedTs = false;   // 当前指令轮是否写过 .ts / tsconfig（onToolLog 累计）
    int m_fixRoundsLeft = 0;         // 当前任务剩余可自动修复轮数（每条用户指令开始时重置）
    JZProcess *m_hostProc = nullptr; // 正在运行的宿主 tsc 进程
    JZTimer *m_hostTimer = nullptr;
    bool m_hostRunning = false;
    bool m_hostTimeout = false;

    // 打包/发布（异步）状态
    JZProcess *m_buildProc = nullptr;   // 正在运行的打包/发布 tsc 进程
    JZTimer *m_buildTimer = nullptr;    // 墙钟超时保护
    bool m_buildRunning = false;
    bool m_buildOk = false;             // 最近一次打包/发布结果
    std::string m_buildRoot;            // 本次打包的工程根（workspace 快照）
    std::string m_buildError;           // 最近一次失败原因（成功为空）

    // 任务级自动恢复状态（m_recovery 为注入的策略，buildAgent 不清空）
    JZCodeAgentRecovery m_recovery;
    bool m_recoveryEnabled = false;   // m_recovery.maxAttempts > 1 时启用
    bool m_recoveryActive = false;    // 一次外层任务进行中（含其恢复轮次）
    int m_recoveryAttempt = 0;        // 已尝试轮次（首次 = 1）
    std::string m_lastUserRequest;    // 最近一次外层 user request（重发原始请求用）
};

#endif // !JZ_CODE_AGENT_H_
