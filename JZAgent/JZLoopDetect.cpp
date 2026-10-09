#include "JZLoopDetect.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

#include "json/json.h"

// ---------------------------------------------------------------------------
// 内部工具
// ---------------------------------------------------------------------------
namespace
{

// JSON 规范化序列化: 对象**按键排序**, 否则 {"a":1,"b":2} 与 {"b":2,"a":1}
// 会被当成两次不同的调用(模型换个键序就会漏检)。
std::string normalizeJson(const Json::Value &value)
{
    if (value.isObject())
    {
        std::vector<std::string> keys = value.getMemberNames();
        std::sort(keys.begin(), keys.end());

        std::string text = "{";
        for (size_t i = 0; i < keys.size(); i++)
        {
            if (i)
                text += ",";
            text += "\"" + keys[i] + "\":" + normalizeJson(value[keys[i]]);
        }
        text += "}";
        return text;
    }

    if (value.isArray())
    {
        std::string text = "[";
        for (Json::ArrayIndex i = 0; i < value.size(); i++)
        {
            if (i)
                text += ",";
            text += normalizeJson(value[i]);
        }
        text += "]";
        return text;
    }

    Json::FastWriter writer;
    std::string text = writer.write(value);
    while (!text.empty() && (text[text.size() - 1] == '\n' || text[text.size() - 1] == '\r'))
        text.erase(text.size() - 1);
    return text;
}

// 折叠空白 + 去首尾空格(解析不了 JSON 时的退化比较方式; 也用于把重复片段压成单行)
std::string collapseSpaces(const std::string &text)
{
    std::string out;
    out.reserve(text.size());

    bool inSpace = false;
    for (size_t i = 0; i < text.size(); i++)
    {
        char c = text[i];
        bool space = (c == ' ' || c == '\t' || c == '\r' || c == '\n');
        if (space)
        {
            if (!inSpace && !out.empty())
                out += ' ';
            inSpace = true;
        }
        else
        {
            out += c;
            inSpace = false;
        }
    }

    while (!out.empty() && out[out.size() - 1] == ' ')
        out.erase(out.size() - 1);
    return out;
}

std::string truncateText(const std::string &text, int maxLen)
{
    if (maxLen <= 0 || (int)text.size() <= maxLen)
        return text;
    return text.substr(0, maxLen) + "…";
}

// 行首尾空白(含 \r)
std::string trimLine(const std::string &text)
{
    size_t b = text.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return std::string();
    size_t e = text.find_last_not_of(" \t\r\n");
    return text.substr(b, e - b + 1);
}

// \r\n -> \n(比较前统一, 免得 Windows 换行把"同一行"拆成不同内容)
std::string normalizeNewlines(const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++)
    {
        if (text[i] == '\r')
        {
            if (i + 1 < text.size() && text[i + 1] == '\n')
                continue;
            out += '\n';
        }
        else
        {
            out += text[i];
        }
    }
    return out;
}

std::vector<std::string> splitLines(const std::string &text)
{
    std::vector<std::string> lines;
    size_t b = 0;
    while (true)
    {
        size_t e = text.find('\n', b);
        if (e == std::string::npos)
        {
            lines.push_back(text.substr(b));
            break;
        }
        lines.push_back(text.substr(b, e - b));
        b = e + 1;
    }
    return lines;
}

// 段落: 由连续非空行组成(空行分段)
std::vector<std::string> splitBlocks(const std::vector<std::string> &lines)
{
    std::vector<std::string> blocks;
    std::string cur;
    for (size_t i = 0; i < lines.size(); i++)
    {
        std::string line = trimLine(lines[i]);
        if (line.empty())
        {
            if (!cur.empty())
            {
                blocks.push_back(cur);
                cur.clear();
            }
            continue;
        }
        if (!cur.empty())
            cur += "\n";
        cur += line;
    }
    if (!cur.empty())
        blocks.push_back(cur);
    return blocks;
}

// 不同**非空白**字符数: `-----` / `=====` / "===\n" 这类分隔线的判据 ——
// 只有一种非空白字符的重复不算"内容在打转"(模型画分隔线是正常排版)。
int distinctBytes(const std::string &text)
{
    bool seen[256] = { false };
    int n = 0;
    for (size_t i = 0; i < text.size(); i++)
    {
        unsigned char c = (unsigned char)text[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            continue;
        if (!seen[c])
        {
            seen[c] = true;
            n++;
        }
    }
    return n;
}

// 结果摘要: 前 N 字节参与哈希, 再把**总长度**也混进去 ——
// "前缀相同但尾巴不同"的两个结果不该算"完全相同"。
uint64_t resultDigest(const std::string &result, int compareBytes)
{
    uint64_t hash = 1469598103934665603ULL;   // FNV-1a 64
    size_t n = result.size();
    if (compareBytes > 0 && n > (size_t)compareBytes)
        n = (size_t)compareBytes;
    for (size_t i = 0; i < n; i++)
    {
        hash ^= (unsigned char)result[i];
        hash *= 1099511628211ULL;
    }

    uint64_t len = (uint64_t)result.size();
    for (int i = 0; i < 8; i++)
    {
        hash ^= (len >> (i * 8)) & 0xFF;
        hash *= 1099511628211ULL;
    }
    return hash;
}

// 给一段子串算指纹(同一套 FNV-1a, 但不混长度) —— 文本重复率统计用
uint64_t textHash(const std::string &text, size_t pos, size_t len)
{
    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++)
    {
        hash ^= (unsigned char)text[pos + i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

}   // namespace

// ---------------------------------------------------------------------------
// JZToolLoopDetector
// ---------------------------------------------------------------------------
void JZToolLoopDetector::reset()
{
    m_steps.clear();
    m_hit = false;
}

std::string JZToolLoopDetector::signature(const std::string &toolName, const std::string &argsJson)
{
    std::string normalized;
    if (!argsJson.empty())
    {
        Json::Value value;
        Json::Reader reader;
        if (reader.parse(argsJson, value))
            normalized = normalizeJson(value);
    }
    if (normalized.empty())
        normalized = collapseSpaces(argsJson);

    return toolName + "|" + normalized;
}

std::string JZToolLoopDetector::argsPreview(const std::string &argsJson, int maxLen)
{
    return truncateText(collapseSpaces(argsJson), maxLen);
}

JZToolLoopEvent JZToolLoopDetector::step(const std::string &toolName, const std::string &argsJson,
                                        const std::string &result, bool pollTool)
{
    JZToolLoopEvent ev;
    ev.toolName = toolName;
    ev.argsPreview = argsPreview(argsJson);

    if (!m_config.enabled || m_hit)
        return ev;

    Step step;
    step.sig = signature(toolName, argsJson);
    step.digest = resultDigest(result, m_config.resultCompareBytes);

    m_steps.push_back(step);
    if (m_config.window > 0)
    {
        while ((int)m_steps.size() > m_config.window)
            m_steps.pop_front();
    }

    // 本次也算在内: 从尾部往前数**连续**的同签名步(夹了别的工具调用就断链)。
    // ⚠ 不能按滑窗计数: `build` 无参数 + 成功文案恒定 ⇒ "同签名同结果"必然成立, 于是
    //   "build → 改文件 → build" 这种正常迭代会在第 2 次被判死(2026-10-08 实测误报)。
    int repeat = 0;
    for (size_t i = m_steps.size(); i > 0; i--)
    {
        if (m_steps[i - 1].sig != step.sig)
            break;
        repeat++;
    }

    // "结果也相同"同样要求连续: 只数尾部那段"签名与结果都一致"的长度
    int sameResult = 0;
    for (size_t i = m_steps.size(); i > 0; i--)
    {
        if (m_steps[i - 1].sig != step.sig || m_steps[i - 1].digest != step.digest)
            break;
        sameResult++;
    }

    ev.repeat = repeat;
    ev.sameResult = sameResult;

    // 轮询类工具用放宽的阈值: 同参数反复查询是正常用法, 别把"查进度"当成打转
    const int repeatLimit = pollTool ? m_config.pollRepeat : m_config.repeat;
    const int sameResultLimit = pollTool ? m_config.pollSameResult : m_config.sameResult;

    bool hit = (sameResultLimit > 0 && sameResult >= sameResultLimit)
            || (repeatLimit > 0 && repeat >= repeatLimit);
    if (!hit)
        return ev;

    m_hit = true;
    ev.loop = true;

    std::string text = "循环检测: " + std::string(pollTool ? "轮询类工具 " : "工具 ") + toolName
                     + " 已被连续 " + std::to_string(repeat) + " 次用完全相同的参数调用";
    if (sameResult >= 2)
        text += "(其中 " + std::to_string(sameResult) + " 次返回结果也相同)";
    text += ", 判定为原地打转, 本轮已中止。参数: " + ev.argsPreview;
    text += "。请换一种做法(换目标/换参数/换工具), 或信息已足够就直接给出结论。";
    ev.message = text;
    return ev;
}

// ---------------------------------------------------------------------------
// JZTextLoopDetector
// ---------------------------------------------------------------------------
void JZTextLoopDetector::reset()
{
    m_lastLen = 0;
}

JZTextLoopEvent JZTextLoopDetector::check(const std::string &text, const std::string &where)
{
    JZTextLoopEvent ev;
    ev.where = where;

    if (!m_config.enabled || text.empty())
        return ev;

    // 检查频率控制: 累计新增够多字符才查一次(流式下每个 delta 都查既没必要也费 CPU)
    size_t step = (size_t)std::max(1, m_config.textCheckMinChars);
    if (text.size() < m_lastLen + step)
        return ev;
    m_lastLen = text.size();

    // 只看尾部: 循环发生在"正在写的地方", 前面的正常内容不参与
    size_t tailLen = m_config.textTailBytes > 0 ? (size_t)m_config.textTailBytes : text.size();
    std::string tail = text.size() > tailLen ? text.substr(text.size() - tailLen) : text;
    std::string norm = normalizeNewlines(tail);

    std::vector<std::string> lines = splitLines(norm);

    // ① 同一行连续重复
    //    收尾的空行/换行不算(模型收尾刷换行很常见, 无害), 纯分隔线(`-----`/`=====`)也不算。
    if (m_config.textLineRepeat > 1)
    {
        int last = (int)lines.size() - 1;
        while (last >= 0 && trimLine(lines[last]).empty())
            last--;

        if (last >= 0)
        {
            std::string line = trimLine(lines[last]);
            if (distinctBytes(line) >= 2)
            {
                int n = 0;
                for (int i = last; i >= 0; i--)
                {
                    if (trimLine(lines[i]) != line)
                        break;
                    n++;
                }
                if (n >= m_config.textLineRepeat)
                {
                    ev.loop = true;
                    ev.period = (int)line.size();
                    ev.repeat = n;
                    ev.sample = truncateText(line, 80);
                }
            }
        }
    }

    // ② 同一段(空行分隔)连续重复: 挡"整段/整句反复贴", 周期可能很长, ① 和 ③ 都盖不住
    if (!ev.loop && m_config.textBlockRepeat > 1)
    {
        std::vector<std::string> blocks = splitBlocks(lines);
        size_t need = (size_t)m_config.textBlockRepeat;
        if (blocks.size() >= need)
        {
            const std::string &last = blocks.back();
            int n = 0;
            for (int i = (int)blocks.size() - 1; i >= 0; i--)
            {
                if (blocks[i] != last)
                    break;
                n++;
            }
            if (n >= (int)need)
            {
                ev.loop = true;
                ev.period = (int)last.size();
                ev.repeat = n;
                ev.sample = truncateText(collapseSpaces(last), 80);
            }
        }
    }

    // ③ 周期串重复(尾部从最短周期开始找): 挡"同一句话/同一串 token 反复吐"
    if (!ev.loop && m_config.textPeriodRepeat > 1 && m_config.textPeriodMax >= 3)
    {
        int span = m_config.textPeriodRepeat;
        int maxP = m_config.textPeriodMax;
        if (maxP > (int)(norm.size() / (size_t)span))
            maxP = (int)(norm.size() / (size_t)span);

        for (int p = 3; p <= maxP; p++)
        {
            std::string unit = norm.substr(norm.size() - p);
            if (distinctBytes(unit) < 2)   // `-----` / `=====` 这类分隔线不算循环
                continue;

            int n = 1;
            size_t pos = norm.size() - (size_t)p;
            while (n < span && pos >= (size_t)p && norm.compare(pos - (size_t)p, (size_t)p, unit) == 0)
            {
                pos -= (size_t)p;
                n++;
            }
            if (n >= span)
            {
                ev.loop = true;
                ev.period = p;
                ev.repeat = n;
                ev.sample = truncateText(collapseSpaces(unit), 80);
                break;
            }
        }
    }

    // ④ 后缀重复率(n-gram): 补 ①②③ 的漏洞 —— 那三条都要求**逐字节完全相等**,
    //    对"长单元少次重复"(超过 ③ 的周期上限、又没有空行分隔)和"改写型重复"都认不出来。
    //    这里统计尾部里"重复出现过的 n 字片语"占比:
    //      单元重复 k 遍 => 占比 ≈ 1-1/k(3 遍 0.67 / 4 遍 0.75); 正常文本各句不同, 远低于阈值。
    //    先折叠空白再切 n-gram(换行/缩进差异不该把"同一段"拆散), 且只用尾部窗口。
    if (!ev.loop && m_config.textGramN >= 4 && m_config.textGramMin > 0
        && m_config.textGramRepeat > 0.0)
    {
        std::string flat = collapseSpaces(norm);
        const size_t n = (size_t)m_config.textGramN;
        if (flat.size() >= n + 1)
        {
            std::unordered_map<uint64_t, int> counts;
            int total = 0;
            for (size_t i = 0; i + n <= flat.size(); i++)
            {
                counts[textHash(flat, i, n)]++;
                total++;
            }

            const int dup = total - (int)counts.size();   // 重复出现过的 gram 个数
            const double rate = (total > 0) ? ((double)dup / (double)total) : 0.0;
            if (total >= m_config.textGramMin && rate >= m_config.textGramRepeat)
            {
                // 取样 = 出现次数最多的那个片语(让模型一眼看出自己在复读什么)
                int best = 0;
                uint64_t bestHash = 0;
                for (std::unordered_map<uint64_t, int>::const_iterator it = counts.begin();
                     it != counts.end(); ++it)
                {
                    if (it->second > best)
                    {
                        best = it->second;
                        bestHash = it->first;
                    }
                }
                std::string sample;
                for (size_t i = 0; i + n <= flat.size(); i++)
                {
                    if (textHash(flat, i, n) == bestHash)
                    {
                        sample = flat.substr(i, n);
                        break;
                    }
                }

                ev.loop = true;
                ev.period = (int)n;
                ev.repeat = 0;
                ev.detail = std::to_string((int)n) + " 字片语重复率 "
                            + std::to_string((int)(rate * 100.0 + 0.5)) + "%";
                ev.sample = truncateText(sample, 80);
            }
        }
    }

    if (!ev.loop)
        return ev;

    ev.message = "循环检测(" + where + "): 输出在尾部原地打转(";
    if (!ev.detail.empty())
        ev.message += ev.detail + ", ";
    else if (ev.period > 0 && ev.repeat > 0)
        ev.message += "重复单元 " + std::to_string(ev.period) + " 字符 x " + std::to_string(ev.repeat) + " 次, ";
    ev.message += "片段: " + ev.sample + "), 已中止本轮。"
                  "不要再重复同样的内容: 换个说法/推进下一步, 或者直接给出最终结论。";
    return ev;
}
