#ifndef JZ_LOOP_DETECT_H_
#define JZ_LOOP_DETECT_H_
#include "JZGlobal.h"
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
//     判定依据 = 同签名(工具名 + 归一化参数)**连续**重复 N 次(含本次)。
//     ⚠ "连续"是关键: 中间夹了别的工具调用, 链就断了 —— "改一版编一次"这种正常迭代
//       (build → 改文件 → build → 跑 → build) 永远连不成一段, 不该被判成打转。
//       只按滑窗计数会**误报**: 实测 `build` **无参数** + 成功文案是**常量** ⇒ "同签名同结果"
//       必然成立, 于是第 2 次成功 build 就被判死(2026-10-08, aime25_02/03 因此 FAIL)。
//
//  2) 输出循环(JZTextLoopDetector)
//     思考/回复文本在尾部原地打转(同一行刷屏、同一段重复、周期串反复),
//     输出能涨到上万 token 还不结束。只看累计输出的尾部:
//       ① 同一行(trim 后)连续重复 >= lineRepeat
//       ② 同一段(空行分隔)连续重复 >= blockRepeat
//       ③ 周期串(周期 3..periodMax, 单元含 >=2 种字符)重复 >= periodRepeat
//       ④ n-gram 重复率: 尾部里"重复出现过的 n 字片语"占比 >= gramRepeat
//     ③ 的"至少两种字符"是为了不把 `------` 这种分隔线当循环。
//     ⚠ ①②③ 都要求**逐字节完全相等** ⇒ 漏掉两类真实循环:
//       · 长单元少次重复(单元超过 ③ 的周期上限、又没有空行分隔): "同一段 500 字贴 3 遍";
//       · 改写型重复(同一段话换个说法、多个标点/空格)。
//       ④ 用"重复片语占比"做统计判据, 不要求完全相等, 正好补这两个洞。
//       n 取 32 是刻意的: 只认"≥32 字的长片语"反复出现, 免得把列表/模板里的短句式复用误判成循环。
// ---------------------------------------------------------------------------

struct JZFRAMEWORK_API JZLoopConfig
{
    bool enabled = true;

    // ---- 工具调用 ----
    // ⚠ 阈值都只看**连续**的一段(从尾部往前数, 直到签名不同为止); window 只是内存上限,
    //   必须 >= 用到的最大阈值, 否则长连续段会被截断而漏检。
    int window = 24;              // 最多保留最近 N 步(<=0 不限; 仅内存, 不改变判定口径)
    int repeat = 5;               // 同签名**连续** N 次 -> 中止
    int sameResult = 5;           // 同签名**连续**且结果也相同 N 次 -> 中止
                                  //   (默认与 repeat 同值 ⇒ 非轮询工具等价于只看 repeat;
                                  //    调小可更快抓"连回话都一模一样"的死重复)
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
    // ④ n-gram 重复率: 统计尾部里"重复出现过的 n 字片语"占比(**不**要求连续逐字节相等)。
    //    单元重复 k 遍时该占比 ≈ 1-1/k(3 遍 -> 0.67, 4 遍 -> 0.75), 正常文本远低于阈值。
    //    ⚠ n 越大越保守: 32 只认"≥32 字的长片语"反复出现; 调小会开始吃"模板/列表里的句式复用"。
    int textGramN = 32;           // n-gram 长度(字符; CJK 友好, 不依赖分词)
    int textGramMin = 256;        // 至少要有这么多 gram 才判(尾部太短没有统计意义)
    double textGramRepeat = 0.60; // 重复 gram 占比 >= 该值 -> 中止
    // 累计输出每新增这么多字符才检查一次(流式每个 delta 都查没必要, 也省 CPU)
    int textCheckMinChars = 128;
};

// 工具调用登记后的判定
struct JZFRAMEWORK_API JZToolLoopEvent
{
    bool loop = false;        // 判定为循环(调用方应 llmError)
    std::string toolName;
    std::string argsPreview;  // 参数简写(单行 + 截断)
    int repeat = 0;           // 窗口内同签名次数(含本次)
    int sameResult = 0;       // 其中结果也相同的次数(含本次)
    std::string message;      // 现成的错误文案(作为 llmError 的原因)
};

// 输出文本判定结果
struct JZFRAMEWORK_API JZTextLoopEvent
{
    bool loop = false;
    std::string where;        // "思考" / "回复"(只用于文案)
    int period = 0;           // 重复单元长度(同一行/同一段时 = 该行/段长度)
    int repeat = 0;           // 重复次数
    std::string detail;       // ④ 用的说明(如 "32 字片语重复率 76%"); 非空时优先于 period/repeat
    std::string sample;       // 重复内容简写(单行 + 截断)
    std::string message;
};

class JZFRAMEWORK_API JZToolLoopDetector
{
public:
    void setConfig(const JZLoopConfig &config) { m_config = config; }
    const JZLoopConfig &config() const { return m_config; }

    void reset();
    int stepCount() const { return (int)m_steps.size(); }

    // 登记一步**已完成**的工具调用并判定。
    // argsJson 传原始 arguments 串(解析不了也能比: 退化成折叠空白后的原文)。
    // pollTool = 该工具是轮询类(见 JZAgentTool::isPollTool): 用放宽的阈值判定。
    // 判定只看**尾部连续段**(见类注释): 夹了别的工具调用就断链。
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

class JZFRAMEWORK_API JZTextLoopDetector
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
