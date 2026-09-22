#ifndef JZ_LOOP_DETECT_H_
#define JZ_LOOP_DETECT_H_

#include <deque>
#include <stdint.h>
#include <string>

// ---------------------------------------------------------------------------
// 循环检测(JZAgent 用)
//
// 两类"原地打转", 命中都直接 llmError(LoopError) 中止本轮(不做软提醒):
//
//  1) 工具调用循环(JZToolLoopDetector)
//     模型反复用**完全相同的参数**调同一个工具(反复 read_file 同一个文件、
//     search_context 同一个 pattern、terminal_run 同一条命令)。每一步工具都成功
//     ⇒ 永远不报错, 只把上下文一轮轮堆大(每轮重发整段 prompt), 一路烧到 maxRounds。
//     判定依据 = 同签名(工具名 + 归一化参数)重复; 结果也相同 ⇒ 证据更强, 阈值更低。
//
//  2) 输出循环(JZTextLoopDetector)
//     思考/回复文本在尾部原地打转(同一行刷屏、同一段重复、周期串反复),
//     输出能涨到上万 token 还不结束。只看累计输出的尾部:
//       ① 同一行(trim 后)连续重复 >= lineRepeat
//       ② 同一段(空行分隔)连续重复 >= blockRepeat
//       ③ 周期串(周期 3..periodMax, 单元含 >=2 种字符)重复 >= periodRepeat
//     ③ 的"至少两种字符"是为了不把 `------` 这种分隔线当循环。
// ---------------------------------------------------------------------------

struct JZLoopConfig
{
    bool enabled = true;

    // ---- 工具调用 ----
    int window = 24;              // 只看最近 N 步工具调用(<=0 表示不限窗口)
    int repeat = 5;               // 窗口内同签名重复 N 次 -> 中止
    int sameResult = 2;           // 同签名**且结果也相同** N 次 -> 中止(证据更强)
    int resultCompareBytes = 4096;// 结果比较只取前 N 字节(0 = 全长)

    // 轮询类工具(见 JZAgentTool::isPollTool): "用同样的参数反复调"本身就是**正常用法**
    // (查后台任务进度 / 列会话), 用 repeat 的小阈值会把正常轮询误判成打转 —— 而发布给
    // 模型的提示词恰恰写着"用 terminal_query 反复查状态与日志"。这两条把它们放宽:
    // 只有"查了很多次且结果一直没变"这种真卡死才判(阈值 = 5~6 倍才算, 仍受 maxRounds 兜底)。
    int pollRepeat = 16;          // 轮询类工具: 同签名重复 N 次 -> 中止
    int pollSameResult = 6;       // 轮询类工具: 同签名且结果也相同 N 次 -> 中止

    // ---- 输出文本(思考/回复) ----
    int textTailBytes = 4096;     // 只看累计输出的尾部这么多字节
    int textLineRepeat = 6;       // 同一行连续重复 N 次 -> 中止
    int textBlockRepeat = 3;      // 同一段(空行分隔)连续重复 N 次 -> 中止
    int textPeriodMax = 256;      // 周期串最长多少字符(再长更可能是正常的长段复用)
    int textPeriodRepeat = 8;     // 周期串至少重复多少次
    // 累计输出每新增这么多字符才检查一次(流式每个 delta 都查没必要, 也省 CPU)
    int textCheckMinChars = 128;
};

// 工具调用登记后的判定
struct JZToolLoopEvent
{
    bool loop = false;        // 判定为循环(调用方应 llmError)
    std::string toolName;
    std::string argsPreview;  // 参数简写(单行 + 截断)
    int repeat = 0;           // 窗口内同签名次数(含本次)
    int sameResult = 0;       // 其中结果也相同的次数(含本次)
    std::string message;      // 现成的错误文案(作为 llmError 的原因)
};

// 输出文本判定结果
struct JZTextLoopEvent
{
    bool loop = false;
    std::string where;        // "思考" / "回复"(只用于文案)
    int period = 0;           // 重复单元长度(同一行/同一段时 = 该行/段长度)
    int repeat = 0;           // 重复次数
    std::string sample;       // 重复内容简写(单行 + 截断)
    std::string message;
};

class JZToolLoopDetector
{
public:
    void setConfig(const JZLoopConfig &config) { m_config = config; }
    const JZLoopConfig &config() const { return m_config; }

    void reset();
    int stepCount() const { return (int)m_steps.size(); }

    // 登记一步**已完成**的工具调用并判定。
    // argsJson 传原始 arguments 串(解析不了也能比: 退化成折叠空白后的原文)。
    // pollTool = 该工具是轮询类(见 JZAgentTool::isPollTool): 用放宽的阈值判定。
    JZToolLoopEvent step(const std::string &toolName, const std::string &argsJson,
                         const std::string &result, bool pollTool = false);

    // 签名归一化: 参数能解析成 JSON 就**按键排序**后序列化(免得 {"a":1,"b":2} 与
    // {"b":2,"a":1} 被当成两次不同的调用), 否则折叠空白按原文比。
    static std::string signature(const std::string &toolName, const std::string &argsJson);

    // 参数简写: 单行化 + 截断(文案里带上, 让人知道"哪一步"在打转)
    static std::string argsPreview(const std::string &argsJson, int maxLen = 120);

private:
    struct Step
    {
        std::string sig;
        uint64_t digest = 0;   // 结果摘要(前 N 字节 + 总长度)
    };

    JZLoopConfig m_config;
    std::deque<Step> m_steps;   // 最近 window 步
    bool m_hit = false;         // 已判过: 不重复判
};

class JZTextLoopDetector
{
public:
    void setConfig(const JZLoopConfig &config) { m_config = config; }
    const JZLoopConfig &config() const { return m_config; }

    void reset();
    size_t lastCheckedLen() const { return m_lastLen; }

    // 传入**累计**的思考/回复全文(不是增量), 返回本次判定。
    // where = "思考" / "回复", 只用于文案。
    JZTextLoopEvent check(const std::string &text, const std::string &where);

private:
    JZLoopConfig m_config;
    size_t m_lastLen = 0;   // 上次检查时的长度(控制检查频率)
};

#endif // JZ_LOOP_DETECT_H_
