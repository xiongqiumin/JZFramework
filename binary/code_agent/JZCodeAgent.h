#ifndef JZ_CODE_AGENT_H_
#define JZ_CODE_AGENT_H_

#include <deque>
#include <map>
#include <string>
#include <vector>

#include "JZAgent.h"
#include "JZAgentTool.h"
#include "JZCodeTools.h"

// 代码助手自身的配置（与大模型配置 JZAgentConfig 分开）
class CodeAgentConfig
{
public:
    std::string workspace;                    // 工作区根目录，可读写
    std::vector<TrustedDir> trustedDirs;     // 授信目录（编译器/SDK 等），默认只读，可单独标记可写
    std::vector<TrustedProgram> trustedPrograms;  // 显式指定的可信程序（全路径），优先于目录搜索

    // 轮末宿主编译把关（默认关闭，tscPath 非空才会启用）。
    // 模型每轮回复结束、且该轮改过 TS 相关文件时，宿主用 "tsc --noEmit -p tsconfig.json" 快检；
    // 未通过则自动再发一轮修复指令，直到通过或达到 maxAutoFixRounds（超出则把剩余错误上报用户）。
    std::string tscPath;            // 宿主侧 tsc 编译器全路径；空 = 关闭宿主把关
    int maxAutoFixRounds = 2;       // 自动续修轮数上限；<=0 = 即使编译失败也只上报不续修

    std::string webSearchUrl;                // 网络搜索接口模板(含 {q} 占位符)；空则用内置默认
    // HTTP 代理（仅对 http 目标生效；https 目标直连，不走代理）
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

// 单任务熔断配置
//   定位: 这类保护是 code_agent 这个应用的策略，不进入 JZAgent 通用框架。
//   tool 层只负责把"被拒绝"的原因结构化上报，统计与熔断由 JZCodeAgent 完成。
struct GuardConfig
{
    int maxToolCalls = 50;      // 单条指令内工具调用总次数上限
    int maxDurationSec = 900;   // 单条指令墙钟时长上限(秒)
    int maxSameDeny = 3;        // 同一类拒绝原因的累计次数上限
    int maxRepeatCall = 3;      // 完全相同的调用连续出现次数上限(判定陷入重复)
};

// 单任务熔断统计，每次用户下发新指令时重置
struct GuardState
{
    int toolCalls = 0;
    int totalDeny = 0;
    int64_t startTimeMs = 0;
    std::map<int, int> denyCount;            // DenyReason -> 次数
    std::deque<std::string> recentCalls;     // 最近的调用指纹(工具名 + 参数)
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

// 代码助手：把 JZAgent 与一组代码工具(编译/运行/改代码/找代码)组装起来，
// 并维护一份可供网页端增量拉取的会话日志。
class JZCodeAgent : public JZObject
{
public:
    JZCodeAgent(JZObject *parent = nullptr);
    ~JZCodeAgent();

    void init(const JZAgentConfig &llm, const CodeAgentConfig &code);
    void request(const std::string &text);
    void stop();
    void clearHistory();
    bool isBusy() const;

    // 是否有指令在排队/刚续发仍等待回复(供界面显示"等待回复中...")
    bool waitingReply() const;

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
    // 把 path 初始化为最小可编译的标准 TS 工程骨架
    // （生成 tsconfig.json(outDir=build)/.gitignore，src 为空时补 src/main.ts；
    //   已有文件一律不覆盖，可幂等重复调用补齐缺项。
    //   注意: 不生成 project.json —— 它是"打包/发布"时构建产物的元数据，见 buildProject）
    bool initProject(const std::string &path, std::string &error);

    // 把 path 工程"打包/发布"：用宿主侧 tsc（CodeAgentConfig.tscPath）真实全量编译
    // （tsc -p tsconfig.json）。产物写向 tsconfig.json 的 outDir —— 缺省/未配置按
    // 标准工程约定是"工程根/build"，也可以被改成任意目录（含工程根外的绝对路径）。
    // 编译通过后校验 outDir 下的 main.js（src/main.ts 的产物），刷新运行主入口声明
    // project.json：entry = 产物路径 —— 产物目录在工程根内写相对路径（build/main.js），
    // 在工程根之外写绝对路径（JZEngineRuntime/code_runtime 两种都支持）。
    // project.json = 构建产物的元数据，不属于工程骨架配置；运行前必须先打包/发布。
    // 阻塞至 tsc 结束；失败返回 false，error 说明原因，output 可接收 tsc 原始输出。
    bool buildProject(const std::string &path, std::string &error,
                      std::string *output = nullptr);

    // 把工程"导出"为可独立运行的发行包：重新打包/发布后，复制编译产物(build/ 下的
    // *.js + *.js.map) + 运行主入口声明 project.json + 内嵌 JZEngineRuntime 的
    // code_runtime.exe 到 <工程根>/export，并生成一键启动的 run.bat。
    //   - exeName：导出运行时 exe 的文件名（不含扩展名，缺省/空=code_runtime）。
    //   - iconPath：可选的 .ico 路径，非空且存在则把该图标写入导出的运行时 exe
    //               （用 JZExeResource 替换资源图标，使连接后的 code_runtime 包含自定义 exe 图标）。
    //   - debug：是否写入"调试开关"资源（缺省 true）。运行时自身读取该资源决定是否开启
    //            JZRuntimeDebugger（基于 JZMcpServer 的 HTTP 调试器）。
    // outDir 返回导出目录（绝对路径）；失败返回 false，error 说明原因。
    bool exportProject(const std::string &path, std::string &outDir, std::string &error,
                       const std::string &exeName = "", const std::string &iconPath = "",
                       bool debug = true);

    // 最近一次 openProject 的探测结果（未 open 过则为默认值）
    const CodeProjectStatus &projectStatus() const;

    // 熔断配置（长时间跑/反复被拦时的保护）
    void setGuardConfig(const GuardConfig &cfg);
    const GuardConfig &guardConfig() const;

    // 更新大模型配置（保留会话日志，重建底层 agent）
    void applyLlmConfig(const JZAgentConfig &llm);

    // 工作区对话存档（<工作区>/.jzcode）：切换工作区时自动保存旧对话、导入新对话。
    // 只存对话历史，systemPrompt / 工具定义每次重建时重新生成，不进文件。
    std::string historyFilePath() const;
    void saveWorkspaceHistory();
    void loadWorkspaceHistory();
    void rebuildLogsFromHistory();

    // 文件浏览（网页端复用与工具一致的路径校验）
    bool listFiles(const std::string &rel, std::vector<std::string> &out, std::string &error) const;
    bool readFileText(const std::string &rel, std::string &text, std::string &error) const;

    const std::vector<std::string> &toolNames() const;

protected:
    void buildAgent();
    void abortTools();
    // 把当前配置转成虚拟终端的区域配置（工作区 + 授信目录）
    void applyZoneConfig();
    void addLog(const std::string &type, const std::string &text, const std::string &name = std::string());
    std::string buildSystemPrompt() const;

    // 熔断: 每次下发新指令时重置，工具调用/拒绝时累计，超阈值则停止当前任务
    void resetGuard();
    void checkGuard();
    void triggerGuard(const std::string &reason, const std::string &suggestion);

    void onReply(const std::string &text);
    void onThinking(const std::string &text);
    void onError(const std::string &text);
    void onFinished();
    void onToolLog(const std::string &type, const std::string &name, const std::string &text, int deny);

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

    // 所有网络工具（web_search / web_fetch）共享的访问管理器，代理只在此统一设置一次
    JZNetworkAccessManager *m_netMan = nullptr;

    std::vector<CodeAgentLog> m_logs;
    int m_logId = 0;
    bool m_busy = false;
    bool m_stopping = false;

    GuardConfig m_guardConfig;
    GuardState m_guard;

    // 轮末宿主把关状态
    bool m_roundTouchedTs = false;   // 当前指令轮是否写过 .ts / tsconfig（onToolLog 累计）
    int m_fixRoundsLeft = 0;         // 当前任务剩余可自动修复轮数（每条用户指令开始时重置）
    JZProcess *m_hostProc = nullptr; // 正在运行的宿主 tsc 进程
    JZTimer *m_hostTimer = nullptr;
    bool m_hostRunning = false;
    bool m_hostTimeout = false;

    // 运行期间新到的指令缓存(自管队列, 不依赖 JZAgent 的 m_requests)，
    // 等上一步(当前任务)发完后由 onFinished 取出继续发送。
    std::deque<std::string> m_pendingQueue;
    bool m_waiting = false;      // 队列有缓存 或 刚把缓存发出还在等回复
};

#endif // !JZ_CODE_AGENT_H_
