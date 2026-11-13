// JZShell.cpp —— 解释器主体：展开 / 内建 / 执行 / 状态
//
// 词法与语法已拆到同目录:
//   JZShellLex.cpp    —— 源串 -> Token(引号 / $( ) / 反引号 / (( )))
//   JZShellParser.cpp —— Token -> 语法树
// 两者与主体之间只通过 JZShellInternal.h 里的声明(命名空间 jzsh)耦合。
#include "JZShell.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <regex>
#include <set>

#include "JZDir.h"
#include "JZFile.h"
#include "JZFileInfo.h"
#include "JZProcess.h"
#include "JZThread.h"   // 后台任务线程(JZProcess 要求跑在 JZThread 上)

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>        // _read: read 内建直接读 fd 0(不走 iostream 缓冲)
#else
#include <unistd.h>
#endif

using namespace std;
using namespace jzsh;   // Token / lex / Parser / 字符分类(见 JZShellInternal.h)

// 语法树类型(类外命名空间, 解析器也能用)
using JZShellAst::Node;
using JZShellAst::NodePtr;
using JZShellAst::Redirect;
using JZShellAst::SimpleCmd;
using JZShellAst::Word;

// ==========================================================================
// 小工具
// ==========================================================================
namespace
{
#ifdef _WIN32
const char kPathSep = ';';
#else
const char kPathSep = ':';
#endif

// ---- Git Bash 的"参数路径转换" ----
// MSYS 运行时在启动**原生程序**(tsc / code_runtime / node / cmd …)时, 会把参数里"像 POSIX 绝对路径"
// 的那部分翻成 Windows 路径: `/c/tmp` -> `C:/tmp`。
// 模型在终端里就是这么写的(`tsc -p /c/tmp/x`), 所以 shell 层必须跟着翻 —— 否则原样传给原生程序,
// 它根本不认识 `/c/...`(实测真 Node: argv 收到 "/c/tmp" 时 path.resolve 给 `C:\c\tmp`)。
// ⚠ 只翻**盘符挂载**形式(`/c/...`): `/usr`、`/etc`、正则里的 `/foo/`、URL 里的 `/api/x` 一律不动 ——
//   MSYS 那种"全量转换"连 `cmd /c` 都会误伤(所以 Git Bash 里得写 `cmd //c`), 不学。
// ⚠ 不认 `/cygdrive/...`: Git Bash(MSYS2) 没有这个挂载点, 那边原样透传(Cygwin 才有)。
// 关掉: `MSYS_NO_PATHCONV=1`(与 MSYS 同名, 便于对照)。
bool driveMountToWin(const string &s, string &out)
{
    size_t i = 0;
    if (!s.empty() && s[0] == '/')
        i = 1;
    else
        return false;

    if (i + 1 >= s.size())
        return false;
    const char d = s[i];
    if (!isalpha((unsigned char)d))
        return false;
    if (s[i + 1] != '/' && s[i + 1] != '\\')
        return false;

    out = string(1, (char)toupper((unsigned char)d)) + ":/" + s.substr(i + 2);
    return true;
}

// `--out=/c/tmp/x` 这种带 '=' 的, 只翻 '=' 之后那段(前半是选项名)
string convertBashPathArg(const string &a)
{
    if (a.empty())
        return a;

    string out;
    if (driveMountToWin(a, out))
        return out;

    const size_t eq = a.find('=');
    if (eq != string::npos && eq + 1 < a.size() && driveMountToWin(a.substr(eq + 1), out))
        return a.substr(0, eq + 1) + out;

    return a;
}

// ---- 动态变量用的两个时钟 ----
// 单调钟: $SECONDS 不能因为系统时间被调整而跳变
long long steadyMsec()
{
    using namespace chrono;
    return (long long)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
// 墙上钟(微秒): $EPOCHSECONDS / $EPOCHREALTIME
long long wallUsec()
{
    using namespace chrono;
    return (long long)duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}

int currentPid()
{
#ifdef _WIN32
    return (int)GetCurrentProcessId();
#else
    return (int)getpid();
#endif
}

string envGet(const string &name)
{
    string key = name;
    if (key.empty())
        return string();
#ifdef _WIN32
    // Windows 环境变量名大小写不敏感: 用 _dupenv_s 直接查
    char *buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, key.c_str()) != 0 || !buf)
        return string();
    string v(buf);
    free(buf);
    return v;
#else
    const char *v = getenv(key.c_str());
    return v ? string(v) : string();
#endif
}

vector<string> splitChar(const string &s, char sep)
{
    vector<string> out;
    string cur;
    for (char c : s)
    {
        if (c == sep)
        {
            out.push_back(cur);
            cur.clear();
        }
        else
        {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

string joinArgs(const vector<string> &args, size_t from)
{
    string out;
    for (size_t i = from; i < args.size(); i++)
    {
        if (i > from)
            out += " ";
        out += args[i];
    }
    return out;
}

// echo -e 的转义展开: \n \t \r \a \b \f \v \\ \0NNN \xHH, \c 截断
string echoUnescape(const string &s)
{
    string out;
    for (size_t i = 0; i < s.size(); i++)
    {
        if (s[i] != '\\' || i + 1 >= s.size())
        {
            out += s[i];
            continue;
        }
        char n = s[++i];
        switch (n)
        {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'a': out += '\a'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'v': out += '\v'; break;
        case 'e': out += '\x1b'; break;   // ESC(bash 的 echo -e / printf 都认)
        case '\\': out += '\\'; break;
        case 'c': return out;
        case '0':
        {
            int v = 0, k = 0;
            while (k < 3 && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '7')
            {
                v = v * 8 + (s[++i] - '0');
                k++;
            }
            out += (char)v;
            break;
        }
        case 'x':
        {
            int v = 0, k = 0;
            while (k < 2 && i + 1 < s.size() && isxdigit((unsigned char)s[i + 1]))
            {
                char h = s[++i];
                v = v * 16 + (h <= '9' ? (h - '0') : (tolower((unsigned char)h) - 'a' + 10));
                k++;
            }
            out += (char)v;
            break;
        }
        default:
            out += '\\';
            out += n;
            break;
        }
    }
    return out;
}

// printf 的转义(格式串与 %b 共用): 与 echoUnescape 的区别是**也认不带 0 的八进制**
// (`\101` = 'A'; bash 的 printf 就是这样, echo -e 才要求 `\0NNN`)。
string printfUnescape(const string &s, bool *stop = nullptr)
{
    string out;
    for (size_t i = 0; i < s.size(); i++)
    {
        if (s[i] != '\\' || i + 1 >= s.size())
        {
            out += s[i];
            continue;
        }
        char n = s[++i];
        if (n >= '0' && n <= '7')
        {
            int v = n - '0', k = 1;
            while (k < 3 && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '7')
            {
                v = v * 8 + (s[++i] - '0');
                k++;
            }
            out += (char)v;
            continue;
        }
        switch (n)
        {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'a': out += '\a'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'v': out += '\v'; break;
        case 'e': out += '\x1b'; break;
        case '\\': out += '\\'; break;
        case '"': out += '"'; break;
        case '\'': out += '\''; break;
        case 'c':   // \c: 到此为止, 不再输出后面的内容
            if (stop)
                *stop = true;
            return out;
        case 'x':
        {
            int v = 0, k = 0;
            while (k < 2 && i + 1 < s.size() && isxdigit((unsigned char)s[i + 1]))
            {
                char h = s[++i];
                v = v * 16 + (h <= '9' ? (h - '0') : (tolower((unsigned char)h) - 'a' + 10));
                k++;
            }
            out += (char)v;
            break;
        }
        default:
            out += '\\';
            out += n;
            break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// $'...' (ANSI-C 引用) 的单条转义展开
//   调用约定: s[i] == '\\'; 返回展开后的文本, 并把 i 移到下一条待处理字符。
//   支持 bash 那套: \a \b \e \f \n \r \t \v \\ \' \" \? \NNN(八进制) \xHH \uHHHH \UHHHHHHHH
//   其它未知转义按"原样保留反斜杠 + 字符"处理(bash 也这样)。
// ---------------------------------------------------------------------------
static void appendUtf8(string &out, unsigned long cp)
{
    if (cp < 0x80)
        out += (char)cp;
    else if (cp < 0x800)
    {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
    else
    {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

static int hexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static string ansiCUnescape(const string &s, size_t &i)
{
    string out;
    i++;   // 跳过 '\'
    if (i >= s.size())
    {
        out += '\\';
        return out;
    }
    char c = s[i++];
    switch (c)
    {
    case 'a': out += '\a'; return out;
    case 'b': out += '\b'; return out;
    case 'e': out += '\x1b'; return out;
    case 'f': out += '\f'; return out;
    case 'n': out += '\n'; return out;
    case 'r': out += '\r'; return out;
    case 't': out += '\t'; return out;
    case 'v': out += '\v'; return out;
    case '\\': out += '\\'; return out;
    case '\'': out += '\''; return out;
    case '"': out += '"'; return out;
    case '?': out += '?'; return out;
    case 'x':
    {
        int v = 0, n = 0;
        while (i < s.size() && n < 2 && hexDigit(s[i]) >= 0)
        {
            v = v * 16 + hexDigit(s[i]);
            i++;
            n++;
        }
        if (n == 0)
        {
            out += "\\x";
            return out;
        }
        out += (char)v;
        return out;
    }
    case 'u':
    case 'U':
    {
        int maxd = (c == 'u') ? 4 : 8;
        unsigned long v = 0;
        int n = 0;
        while (i < s.size() && n < maxd && hexDigit(s[i]) >= 0)
        {
            v = v * 16 + (unsigned long)hexDigit(s[i]);
            i++;
            n++;
        }
        if (n == 0)
        {
            out += '\\';
            out += c;
            return out;
        }
        appendUtf8(out, v);
        return out;
    }
    default:
        break;
    }
    if (c >= '0' && c <= '7')
    {
        int v = c - '0';
        int n = 1;
        while (i < s.size() && n < 3 && s[i] >= '0' && s[i] <= '7')
        {
            v = v * 8 + (s[i] - '0');
            i++;
            n++;
        }
        out += (char)v;
        return out;
    }
    // 未知转义: 原样保留(bash 的行为)
    out += '\\';
    out += c;
    return out;
}

// ---------------------------------------------------------------------------
// 花括号展开(brace expansion, bash 的 bracecomp.c)
//   `a{b,c}d` -> abd acd          `{1..3}` -> 1 2 3        `{1..7..2}` -> 1 3 5 7
//   `{a..e}`  -> a b c d e        `x{a,b}{1,2}` -> xa1 xa2 xb1 xb2
// 规则要点(与 bash 一致):
//   * 只认**未加引号**的花括号(引号里的原样保留);
//   * 带 `,` 或 `..` 才算展开, 否则原样(`echo {x}` 保持 `{x}`);
//   * 展开在其它展开之前做, 所以这里的产物会再走一遍词级展开。
// ---------------------------------------------------------------------------
static bool braceRangeExpand(const string &body, vector<string> &out)
{
    // {a..z} / {1..10} / {1..10..2}
    size_t dd = body.find("..");
    if (dd == string::npos)
        return false;
    string lo = body.substr(0, dd);
    string rest = body.substr(dd + 2);
    string stepStr;
    size_t dd2 = rest.find("..");
    if (dd2 != string::npos)
    {
        stepStr = rest.substr(dd2 + 2);
        rest = rest.substr(0, dd2);
    }
    string hi = rest;
    if (lo.empty() || hi.empty())
        return false;

    bool numLo = lo.find_first_not_of("-0123456789") == string::npos;
    bool numHi = hi.find_first_not_of("-0123456789") == string::npos;
    if (numLo && numHi)
    {
        long long a = atoll(lo.c_str());
        long long b = atoll(hi.c_str());
        long long step = 1;
        if (!stepStr.empty())
            step = atoll(stepStr.c_str());
        if (step == 0)
            return false;
        if (step < 0)
            step = -step;
        if (a > b)
            step = -step;
        for (long long v = a; (step > 0) ? (v <= b) : (v >= b); v += step)
            out.push_back(to_string(v));
        return !out.empty();
    }
    if (lo.size() == 1 && hi.size() == 1)   // {a..e}
    {
        char a = lo[0];
        char b = hi[0];
        long long step = 1;
        if (!stepStr.empty())
            step = atoll(stepStr.c_str());
        if (step == 0)
            return false;
        if (step < 0)
            step = -step;
        if (a > b)
            step = -step;
        for (long long v = a; (step > 0) ? (v <= b) : (v >= b); v += step)
            out.push_back(string(1, (char)v));
        return !out.empty();
    }
    return false;
}

// 在一层上展开最靠左的那个花括号; 返回是否发生了展开
static bool braceExpandOnce(const string &in, vector<string> &out)
{
    int depth = 0;
    size_t start = string::npos;
    for (size_t i = 0; i < in.size(); i++)
    {
        char c = in[i];
        if (c == '\\' && i + 1 < in.size())
        {
            i++;
            continue;
        }
        if (c == '\'')
        {
            i++;
            while (i < in.size() && in[i] != '\'')
                i++;
            continue;
        }
        if (c == '"')
        {
            i++;
            while (i < in.size() && in[i] != '"')
            {
                if (in[i] == '\\' && i + 1 < in.size())
                    i++;
                i++;
            }
            continue;
        }
        if (c == '{')
        {
            if (depth == 0)
                start = i;
            depth++;
            continue;
        }
        if (c == '}')
        {
            if (depth == 0)
                continue;
            depth--;
            if (depth != 0 || start == string::npos)
                continue;

            string body = in.substr(start + 1, i - start - 1);
            vector<string> alts;
            // 顶层逗号切分
            int d2 = 0;
            size_t from = 0;
            for (size_t k = 0; k <= body.size(); k++)
            {
                if (k == body.size() || (body[k] == ',' && d2 == 0))
                {
                    alts.push_back(body.substr(from, k - from));
                    from = k + 1;
                    continue;
                }
                if (body[k] == '{')
                    d2++;
                else if (body[k] == '}')
                    d2--;
            }
            if (alts.size() < 2)
            {
                alts.clear();
                if (!braceRangeExpand(body, alts))
                    continue;   // 这个花括号不算展开, 继续往后找
            }

            string prefix = in.substr(0, start);
            string suffix = in.substr(i + 1);
            for (const string &a : alts)
                out.push_back(prefix + a + suffix);
            return true;
        }
    }
    return false;
}

static vector<string> braceExpandRaw(const string &raw)
{
    vector<string> cur;
    cur.push_back(raw);
    // 逐层展开; 上限防止 `{1..100000}` 之类把内存打爆
    for (int round = 0; round < 32; round++)
    {
        vector<string> next;
        bool changed = false;
        for (const string &s : cur)
        {
            vector<string> one;
            if (braceExpandOnce(s, one))
            {
                changed = true;
                next.insert(next.end(), one.begin(), one.end());
            }
            else
                next.push_back(s);
        }
        if (!changed)
            break;
        cur.swap(next);
        if (cur.size() > 4096)
            break;
    }
    return cur;
}

bool globMatch(const char *p, const char *s);   // 前置声明(extglob 辅助函数要用)

// ---------------------------------------------------------------------------
// extglob: @(a|b)  ?(a|b)  *(a|b)  +(a|b)  !(a|b)
//   @  恰好一个候选      ?  零或一个        *  零或多个重复
//   +  一个或多个重复    !  字符数任意但**不**匹配任何候选
// 语义与 bash 的 extglob 一致; 这里是穷举 + 递归, 模式都很短, 不做优化。
// ---------------------------------------------------------------------------
static size_t extGroupEnd(const char *p)   // p[0] == '('
{
    int depth = 0;
    for (size_t i = 0; p[i]; i++)
    {
        if (p[i] == '\\' && p[i + 1])
        {
            i++;
            continue;
        }
        if (p[i] == '(')
            depth++;
        else if (p[i] == ')')
        {
            depth--;
            if (depth == 0)
                return i;
        }
    }
    return (size_t)-1;
}

static void splitAlternatives(const string &body, vector<string> &out)
{
    int depth = 0;
    size_t from = 0;
    for (size_t k = 0; k <= body.size(); k++)
    {
        char c = (k < body.size()) ? body[k] : '\0';
        if (c == '\\' && k + 1 < body.size())
        {
            k++;
            continue;
        }
        if (k == body.size() || (c == '|' && depth == 0))
        {
            out.push_back(body.substr(from, k - from));
            from = k + 1;
            continue;
        }
        if (c == '(')
            depth++;
        else if (c == ')')
        {
            if (depth)
                depth--;
        }
    }
}

// s 的前 len 个字符是否匹配某个候选
static bool matchAnyAlt(const vector<string> &alts, const char *s, size_t len)
{
    string sub(s, len);
    for (const string &a : alts)
    {
        if (globMatch(a.c_str(), sub.c_str()))
            return true;
    }
    return false;
}

// `*(...)` / `+(...)`: 反复吃候选, 每吃一次都试一次剩余模式
static bool matchRepeat(const vector<string> &alts, const string &rest, const char *s,
                        size_t minTimes)
{
    if (minTimes == 0 && globMatch(rest.c_str(), s))
        return true;
    size_t slen = strlen(s);
    for (size_t len = 1; len <= slen; len++)
    {
        if (matchAnyAlt(alts, s, len) && matchRepeat(alts, rest, s + len, 0))
            return true;
    }
    return false;
}

// 简易 glob 匹配: * ? [abc] [a-z] [!...] 以及 extglob @() ?() *() +() !()
bool globMatch(const char *p, const char *s)
{
    while (*p)
    {
        if ((*p == '@' || *p == '?' || *p == '*' || *p == '+' || *p == '!') && p[1] == '(')
        {
            size_t e = extGroupEnd(p + 1);
            if (e != (size_t)-1)
            {
                char kind = *p;
                // p 指向 `X`，p+1 指向 '('，'(' 在 p+1+e 处 ⇒ 括号内是 p+2 起、共 (e-1) 个字符
                string body(p + 2, e - 1);
                vector<string> alts;
                splitAlternatives(body, alts);
                const char *rest = p + 1 + e + 1;
                size_t slen = strlen(s);
                bool ok = false;

                if (kind == '@')
                {
                    for (size_t len = 0; len <= slen && !ok; len++)
                        ok = matchAnyAlt(alts, s, len) && globMatch(rest, s + len);
                }
                else if (kind == '?')
                {
                    ok = globMatch(rest, s);   // 零次
                    for (size_t len = 1; len <= slen && !ok; len++)
                        ok = matchAnyAlt(alts, s, len) && globMatch(rest, s + len);
                }
                else if (kind == '*')
                {
                    ok = matchRepeat(alts, rest, s, 0);
                }
                else if (kind == '+')
                {
                    ok = matchRepeat(alts, rest, s, 1);
                }
                else   // '!': 不匹配任何候选
                {
                    for (size_t len = 0; len <= slen && !ok; len++)
                        ok = !matchAnyAlt(alts, s, len) && globMatch(rest, s + len);
                }
                return ok;
            }
        }
        if (*p == '*')
        {
            while (*p == '*')
                p++;
            if (!*p)
                return true;
            for (const char *t = s; ; t++)
            {
                if (globMatch(p, t))
                    return true;
                if (!*t)
                    return false;
            }
        }
        if (!*s)
            return false;
        if (*p == '?')
        {
            p++;
            s++;
            continue;
        }
        if (*p == '[')
        {
            const char *q = p + 1;
            bool negate = false;
            if (*q == '!' || *q == '^')
            {
                negate = true;
                q++;
            }
            bool hit = false;
            bool first = true;
            char prev = 0;
            while (*q && (*q != ']' || first))
            {
                first = false;
                if (*q == '-' && prev && q[1] && q[1] != ']')
                {
                    char hi = q[1];
                    if (*s >= prev && *s <= hi)
                        hit = true;
                    prev = 0;
                    q += 2;
                    continue;
                }
                if (*q == *s)
                    hit = true;
                prev = *q;
                q++;
            }
            if (*q != ']')
                return false;   // 不闭合: 按字面
            if (hit == negate)
                return false;
            p = q + 1;
            s++;
            continue;
        }
        if (*p != *s)
            return false;
        p++;
        s++;
    }
    return *s == 0;
}

// 归一: 去掉 "."、收掉 ".."、统一 '/'
string normalizePath(const string &in)
{
    string rest = in;
    string prefix;
#ifdef _WIN32
    if (rest.size() >= 2 && rest[1] == ':')
    {
        prefix = rest.substr(0, 2);
        rest = rest.substr(2);
    }
#endif
    bool absolute = !rest.empty() && (rest[0] == '/' || rest[0] == '\\');
    if (absolute)
        prefix += "/";

    vector<string> parts;
    string seg;
    for (size_t i = 0; i <= rest.size(); i++)
    {
        char c = (i < rest.size()) ? rest[i] : '/';
        if (c == '/' || c == '\\')
        {
            if (seg.empty() || seg == ".")
            {
                seg.clear();
                continue;
            }
            if (seg == "..")
            {
                if (!parts.empty() && parts.back() != "..")
                    parts.pop_back();
                else if (!absolute)
                    parts.push_back("..");
            }
            else
            {
                parts.push_back(seg);
            }
            seg.clear();
            continue;
        }
        seg += c;
    }

    string out = prefix;
    for (size_t i = 0; i < parts.size(); i++)
    {
        if (i > 0)
            out += "/";
        out += parts[i];
    }
    if (out.empty())
        out = absolute ? "/" : ".";
    return out;
}

// 路径拼接 + 归一(去掉 "." 和多余的 '/')
string joinPath(const string &base, const string &rel)
{
    string p = rel;
    if (!p.empty() && p[0] == '/')
        return p;
#ifdef _WIN32
    if (p.size() > 1 && p[1] == ':')
        return p;
#endif
    if (base.empty())
        return p;
    string out = base;
    if (!out.empty() && out.back() != '/' && out.back() != '\\')
        out += '/';
    out += p;
    return out;
}

bool readWholeFile(const string &path, string &out, string &error)
{
    JZFileInfo info(path);
    if (!info.exists())
    {
        error = "文件不存在: " + path;
        return false;
    }
    if (info.isDir())
    {
        error = "是目录, 不能作为输入: " + path;
        return false;
    }
    JZFile f;
    if (!f.open(path, "rb"))
    {
        error = "打开失败(权限?): " + path;
        return false;
    }
    JZByteArray data = f.readAll();
    f.close();
    out.assign(data.data(), data.size());
    return true;
}

bool writeWholeFile(const string &path, const string &text, bool append, string &error)
{
    JZFile f;
    if (!f.open(path, append ? "ab" : "wb"))
    {
        error = "写入失败(权限/目录不存在?): " + path;
        return false;
    }
    int n = f.write(text.data(), (int)text.size());
    f.close();
    if (n != (int)text.size())
    {
        error = "写入不完整: " + path;
        return false;
    }
    return true;
}

// 像 shell 脚本: 以 #! 开头, 且不含 \0(排除 PE/图片等二进制)
bool looksLikeShellScript(const string &path)
{
    string text, err;
    if (!readWholeFile(path, text, err))
        return false;
    if (text.size() < 2 || text[0] != '#' || text[1] != '!')
        return false;
    return text.find('\0') == string::npos;
}
} // namespace


// ==========================================================================
// 展开
// ==========================================================================
vector<string> JZShell::expandWord(const Word &w, string *error, bool splitFields, bool globbing,
                                   bool tildeAssign)
{
    // 花括号展开最靠前(bash 也是这个顺序): 一个词可能变成多个词。
    // braceExpandRaw 已经一层层展到不能再展, 所以下面的递归最多再进一次。
    {
        vector<string> braced = braceExpandRaw(w.raw);
        if (braced.size() > 1)
        {
            vector<string> all;
            for (const string &one : braced)
            {
                Word sub;
                sub.raw = one;
                vector<string> part = expandWord(sub, error, splitFields, globbing);
                all.insert(all.end(), part.begin(), part.end());
            }
            return all;
        }
    }

    const string &raw = w.raw;
    vector<pair<string, bool>> fields;   // (文本, 是否允许 glob)
    string cur;
    bool curStarted = false;
    bool curGlob = false;
    bool sawQuote = false;

    // 分字段用的分隔符: bash 按 IFS 切(默认 " \t\n"; 设为空串 = 不分词)。
    // 未加引号的 `$a` 才走这里; 引号内 / 赋值右侧(splitFields=false)不受影响。
    string ifs = " \t\n";
    if (varIsSet("IFS"))
        ifs = varValue("IFS");

    auto flush = [&]()
    {
        if (curStarted)
        {
            fields.push_back(make_pair(cur, curGlob));
            cur.clear();
            curStarted = false;
            curGlob = false;
        }
    };

    // 字面量: 原样拼进当前字段, **不按 IFS 拆**(bash 只拆分变量/命令替换展开的结果,
    // 不拆字面词 —— `echo a:b:c` 里 IFS=: 也是三个字符一个参数)
    auto appendLit = [&](const string &s)
    {
        cur += s;
        curStarted = true;
        for (char c : s)
        {
            if (c == '*' || c == '?' || c == '[')
                curGlob = true;
        }
    };

    auto append = [&](const string &s, bool quoted)
    {
        if (quoted || !splitFields)
        {
            // 引号里的内容 / 赋值右侧 不参与分字段
            cur += s;
            curStarted = true;
            if (quoted)
                sawQuote = true;
            return;
        }
        for (char c : s)
        {
            if (!ifs.empty() && ifs.find(c) != string::npos)
            {
                flush();
                continue;
            }
            cur += c;
            curStarted = true;
            if (c == '*' || c == '?' || c == '[')
                curGlob = true;
        }
    };

    // 变量取值统一走 varValue(), 免得这里和 ${...} 求值两条规则漂移。
    // `set -u` 下引用未定义变量要报错: 展开函数没有返回值通道, 先记到 m_expandError,
    // 由 runSimple 收尾报出去(见那里对 m_expandError 的处理)。
    auto lookup = [&](const string &name) -> string
    {
        if (m_nounset && m_expandError.empty() && !varIsSet(name))
            m_expandError = name + ": unbound variable";
        return varValue(name);
    };

    // 直接落成一个独立字段: 用于 `${a[@]}` —— 一个元素一个字段, 不再按空格切分。
    // ⚠ 赋值右侧(splitFields=false)不能拆字段, 否则 `i=${a[0]}` 会被拆成 ["i=", "aa"]
    //   然后取 one[0] 当完整赋值, 值就丢了。
    auto pushField = [&](const string &s)
    {
        if (!splitFields)
        {
            cur += s;
            curStarted = true;
            return;
        }
        flush();
        fields.push_back(make_pair(s, false));
    };

    for (size_t i = 0; i < raw.size();)
    {
        char c = raw[i];
        if (c == '\'')
        {
            i++;
            string lit;
            while (i < raw.size() && raw[i] != '\'')
                lit += raw[i++];
            if (i < raw.size())
                i++;
            append(lit, true);
            continue;
        }
        if (c == '"')
        {
            i++;
            string lit;
            // 这一段引号里是否已经因为 `${a[@]}` 拆出过多个字段 —— 决定收尾时要不要
            // 再补一个(空的)字段: `""` 要补, `"${a[@]}"` 不要(否则会多出空参数)
            bool quoteMulti = false;
            while (i < raw.size() && raw[i] != '"')
            {
                if (raw[i] == '$' && i + 1 < raw.size() && raw[i + 1] == '(')
                {
                    string inner;
                    size_t end = consumeDollarParen(raw, i, inner);
                    if (end == string::npos)
                    {
                        if (error)
                            *error = "unexpected EOF while looking for matching `)'";
                        return vector<string>();
                    }
                    string sub;
                    bool isArith = false;
                    if (!inner.empty() && inner[0] == '(')
                    {
                        // `$((...))` 先当算术; 算不出来就退回命令替换 —— bash 也是这个口径
                        // (`$((echo a);(echo b))` 是 `$(` + 子 shell, 不是算术)
                        long long v = 0;
                        if (arithEval(inner, v, nullptr))
                        {
                            sub = to_string(v);
                            isArith = true;
                        }
                    }
                    if (!isArith && !captureCommand(inner, sub, error))
                        return vector<string>();
                    lit += sub;      // 双引号内: 不再分字段
                    i = end;
                    continue;
                }
                if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '\n')
                {
                    i += 2;   // 双引号内的续行: 反斜杠+换行都删掉(bash 的行为)
                    continue;
                }
                if (raw[i] == '\\' && i + 1 < raw.size() &&
                    (raw[i + 1] == '"' || raw[i + 1] == '\\' || raw[i + 1] == '$'))
                {
                    lit += raw[i + 1];
                    i += 2;
                    continue;
                }
                if (raw[i] == '$')
                {
                    size_t j = i + 1;
                    string name;
                    if (j < raw.size() && raw[j] == '{')
                    {
                        j++;
                        while (j < raw.size() && raw[j] != '}')
                            name += raw[j++];
                        if (j < raw.size())
                            j++;
                        // ${VAR:-word} / ${VAR:=word} / ${VAR:+word}: 引号里 `~` 不展开
                        string dv;
                        if (resolveDefaultOps(name, true, dv))
                        {
                            lit += dv;
                            i = j;
                            continue;
                        }
                        {
                            // ${a[i]} / ${a[@]} / ${#a[@]} 等
                            vector<string> vals;
                            if (expandBraced(name, vals))
                            {
                                // `${*:1:2}` / `${a[*]:..}` 在引号里是**一个字段**:
                                // 用 IFS 首字符把元素连起来(与 `${a[*]}` 同口径)。
                                // `${@:..}` / `${a[@]:..}` 才是"一个元素一个字段"。
                                const bool starForm = name.compare(0, 2, "*:") == 0 ||
                                                      name.find("[*]:") != string::npos;
                                if (starForm && vals.size() > 1)
                                {
                                    string ifs = envVar("IFS");
                                    const char sep = ifs.empty() ? ' ' : ifs[0];
                                    string joined;
                                    for (size_t k2 = 0; k2 < vals.size(); k2++)
                                    {
                                        if (k2)
                                            joined += sep;
                                        joined += vals[k2];
                                    }
                                    lit += joined;
                                    i = j;
                                    continue;
                                }
                                if (vals.size() <= 1)
                                {
                                    lit += vals.empty() ? string() : vals[0];
                                }
                                else
                                {
                                    // "pre${a[@]}post": pre 贴到**首元素**、post 贴到**末元素**
                                    // (以前 pre 自己单独成段, `"A:${a[@]:1:2}"` 会多出一个 "A:")
                                    vals[0] = lit + vals[0];
                                    lit.clear();
                                    for (size_t k = 0; k + 1 < vals.size(); k++)
                                        pushField(vals[k]);
                                    lit = vals.back();
                                    quoteMulti = true;
                                }
                                i = j;
                                continue;
                            }
                        }
                        lit += lookup(name);
                        i = j;
                        continue;
                    }
                    if (j < raw.size() && raw[j] == '?')
                    {
                        lit += lookup("?");
                        i = j + 1;
                        continue;
                    }
                    // "$@": **一个位置参数一个字段**, 空参数也要保留(bash 语义)。
                    // 以前是 lookup("@") 拼成一个串, `set -- "" a` 之后 "$@" 只剩一个参数。
                    if (j < raw.size() && raw[j] == '@')
                    {
                        vector<string> ps;
                        for (size_t k = 1; k < m_params.size(); k++)
                            ps.push_back(m_params[k]);
                        if (!ps.empty())
                        {
                            ps[0] = lit + ps[0];
                            lit.clear();
                            for (size_t k = 0; k + 1 < ps.size(); k++)
                                pushField(ps[k]);
                            lit = ps.back();   // 尾部文本拼到最后一个参数上
                            quoteMulti = true;
                        }
                        i = j + 1;
                        continue;
                    }
                    if (j < raw.size() && ((raw[j] >= '0' && raw[j] <= '9') ||
                                           raw[j] == '#' || raw[j] == '*' ||
                                           raw[j] == '!' || raw[j] == '$'))
                    {
                        lit += lookup(string(1, raw[j]));
                        i = j + 1;
                        continue;
                    }
                    if (j < raw.size() && isNameStart(raw[j]))
                    {
                        while (j < raw.size() && isNameChar(raw[j]))
                            name += raw[j++];
                        lit += lookup(name);
                        i = j;
                        continue;
                    }
                    lit += '$';
                    i++;
                    continue;
                }
                lit += raw[i++];
            }
            if (i < raw.size())
                i++;
            if (!lit.empty() || !quoteMulti)
                append(lit, true);   // `""` 要产生一个空字段; `"${a[@]}"` 已经拆完了
            continue;
        }
        if (c == '\\' && i + 1 < raw.size())
        {
            cur += raw[i + 1];
            curStarted = true;
            i += 2;
            continue;
        }
        // $'...' : ANSI-C 引用。只有**不加引号**时才有这层含义(bash 同):
        //   $'a\tb'  -> a<TAB>b        $'\x41\u4e2d' -> A中
        if (c == '$' && i + 1 < raw.size() && raw[i + 1] == '\'')
        {
            size_t j = i + 2;
            string lit;
            bool closed = false;
            while (j < raw.size())
            {
                if (raw[j] == '\'')
                {
                    closed = true;
                    j++;
                    break;
                }
                if (raw[j] == '\\')
                {
                    lit += ansiCUnescape(raw, j);
                    continue;
                }
                lit += raw[j++];
            }
            if (!closed)
            {
                if (error)
                    *error = "unexpected EOF while looking for matching `''";
                return vector<string>();
            }
            append(lit, true);   // 引号内容不分字段
            i = j;
            continue;
        }
        // $"..." : 本地化字符串; 没有翻译目录时等价于 "...", 丢掉 '$' 即可
        if (c == '$' && i + 1 < raw.size() && raw[i + 1] == '"')
        {
            i++;
            continue;
        }
        if (c == '$' && i + 1 < raw.size() && raw[i + 1] == '(')
        {
            string inner;
            size_t end = consumeDollarParen(raw, i, inner);
            if (end == string::npos)
            {
                if (error)
                    *error = "unexpected EOF while looking for matching `)'";
                return vector<string>();
            }
            string sub;
            bool isArith = false;
            if (!inner.empty() && inner[0] == '(')
            {
                long long v = 0;
                if (arithEval(inner, v, nullptr))
                {
                    sub = to_string(v);
                    isArith = true;
                }
            }
            if (!isArith && !captureCommand(inner, sub, error))
                return vector<string>();
            append(sub, false);      // 未加引号: 参与分字段/通配
            i = end;
            continue;
        }
        if (c == '$')
        {
            size_t j = i + 1;
            string name;
            if (j < raw.size() && raw[j] == '{')
            {
                j++;
                while (j < raw.size() && raw[j] != '}')
                    name += raw[j++];
                if (j < raw.size())
                    j++;
                // 未加引号: ${VAR:-word} 里词首的 `~` 要展开
                string dv;
                if (resolveDefaultOps(name, false, dv))
                {
                    append(dv, false);
                    i = j;
                    continue;
                }
                {
                    // ${a[i]} / ${a[@]} / ${a[*]} / ${#a} / ${#a[@]}
                    vector<string> vals;
                    if (expandBraced(name, vals))
                    {
                        for (const string &pv : vals)
                            pushField(pv);
                        i = j;
                        continue;
                    }
                }
                append(lookup(name), false);
                i = j;
                continue;
            }
            if (j < raw.size() && raw[j] == '?')
            {
                append(lookup("?"), false);
                i = j + 1;
                continue;
            }
            if (j < raw.size() && ((raw[j] >= '0' && raw[j] <= '9') ||
                                   raw[j] == '#' || raw[j] == '@' || raw[j] == '*' ||
                                   raw[j] == '!' || raw[j] == '$'))
            {
                append(lookup(string(1, raw[j])), false);
                i = j + 1;
                continue;
            }
            if (j < raw.size() && isNameStart(raw[j]))
            {
                while (j < raw.size() && isNameChar(raw[j]))
                    name += raw[j++];
                append(lookup(name), false);
                i = j;
                continue;
            }
            appendLit("$");
            i++;
            continue;
        }
        // `~` 展开的位置规则(bash manual "Tilde Expansion"):
        //   · 词首的 `~`;
        //   · **赋值词**里 `=` 之后、每个 `:` 之后(`PATH=~/bin:~/sbin` 两处都展开)。
        //   其它位置(如 `abcd~chet`、`echo a:~b`)一律原样保留。
        // 非 posix 模式: 任何"长得像赋值"的词(`echo foo=bar:~`)都展开;
        // posix 模式: 只有真正的赋值词(前缀赋值 / export 之类)才展开 —— bash 的差别就在这。
        const bool assignWord = tildeAssign || (!m_posix && isAssignmentWord(raw));
        // 赋值词里只有**第一个** `=` 之后、以及其后的 `:` 之后才展开。
        // 所以 `h=HOME=~` 的 `~` 保持原样(bash 同), 而 `foo=~:/x` 两个位置都算。
        const size_t firstEq = raw.find('=');
        const bool afterAssignEq = i > 0 && raw[i - 1] == '=' && firstEq != string::npos && firstEq + 1 == i;
        const bool afterAssignColon = i > 0 && raw[i - 1] == ':' && firstEq != string::npos && i > firstEq;
        const bool tildeCtx = (i == 0) || (assignWord && (afterAssignEq || afterAssignColon));
        if (c == '~' && tildeCtx)
        {
            // `~` / `~/x` -> HOME; `~user` 只在真能定位到该用户目录时展开, 否则原样保留;
            // `~+` = PWD, `~-` = OLDPWD
            size_t j = i + 1;
            bool esc = false;   // `~\chet` / `~"chet"`: 名字被引号/转义打断 -> 不展开
            while (j < raw.size() && raw[j] != '/' && raw[j] != ' ' && raw[j] != '\t' && raw[j] != ':')
            {
                if (raw[j] == '\\' || raw[j] == '"' || raw[j] == '\'')
                {
                    esc = true;
                    break;
                }
                j++;
            }
            string user = raw.substr(i + 1, j - i - 1);
            string home;
            if (esc)
                user = string();   // 走"不展开"分支
            if (!esc && user.empty())
            {
                // 用 shell 变量(HOME=/usr/xyz 这种脚本里设的也要生效), 没有再退回 USERPROFILE
                home = envVar("HOME");
                if (home.empty())
                    home = envGet("USERPROFILE");
                // HOME 在 Windows 上是 `C:\Users\x`, 拼绝对路径会变成 `C:\Users\x/foo` —— 统一成正斜杠
                for (char &hc : home)
                {
                    if (hc == '\\')
                        hc = '/';
                }
            }
            else if (!esc && (user == "+" || user == "-"))
            {
                home = envVar(user == "+" ? "PWD" : "OLDPWD");
            }
            else if (!esc)
            {
                string cand = string("C:/Users/") + user;
                if (JZFileInfo(cand).isDir())
                    home = cand;
            }
            if (!esc && !home.empty())
            {
                append(home, true);
                i = j;
                continue;
            }
            // 不展开: 原样吐一个 '~' 继续(后面的内容按普通字符处理)
            appendLit("~");
            i++;
            continue;
        }
        appendLit(string(1, c));
        i++;
    }
    flush();

    if (fields.empty() && sawQuote)
        fields.push_back(make_pair(string(), false));

    // glob
    vector<string> out;
    if (!globbing)
    {
        // case 的模式: 原样返回(变量/引号已经展开过), 不做路径展开
        for (auto &f : fields)
            out.push_back(f.first);
        return out;
    }
    for (auto &f : fields)
    {
        if (!f.second || f.first.find_first_of("*?[") == string::npos)
        {
            out.push_back(f.first);
            continue;
        }
        string pattern = f.first;
        string dir = ".";
        string base = pattern;
        size_t slash = pattern.find_last_of('/');
        if (slash != string::npos)
        {
            dir = pattern.substr(0, slash);
            if (dir.empty())
                dir = "/";
            base = pattern.substr(slash + 1);
        }
        string listDir = absolutePath(dir.empty() ? "." : dir);
        vector<JZFileInfo> entries =
            JZDir(listDir).entryInfoList(JZDir::Files | JZDir::Dirs | JZDir::NoDotAndDotDot, JZDir::SortName);
        vector<string> hits;
        for (const JZFileInfo &e : entries)
        {
            if (globMatch(base.c_str(), e.fileName().c_str()))
            {
                hits.push_back(slash == string::npos ? e.fileName()
                                                     : (dir + "/" + e.fileName()));
            }
        }
        if (hits.empty())
        {
            // 没有匹配: 默认保留字面量; `shopt -s nullglob` 时整个词消失
            // (否则 `for f in *.log` 会拿一个叫 "*.log" 的假文件去处理)
            if (!m_nullglob)
                out.push_back(f.first);
        }
        else
            out.insert(out.end(), hits.begin(), hits.end());
    }
    (void)error;
    return out;
}

string JZShell::expandString(const string &s)
{
    Word w;
    w.raw = s;
    vector<string> parts = expandWord(w, nullptr);
    if (parts.empty())
        return string();
    return parts[0];
}

// case 的模式: 展开变量/去引号, 但**不**做路径展开(bash 的 case 模式也不做)
string JZShell::expandPattern(const string &s)
{
    Word w;
    w.raw = s;
    vector<string> parts = expandWord(w, nullptr, /*splitFields=*/false, /*globbing=*/false);
    if (parts.empty())
        return string();
    return parts[0];
}

// ${...} 求值。expr 是花括号内的原文(不含 `${` `}`)。
// 只处理**带下标/带 #** 的形式, 普通 `${VAR}` 交给调用方原有的 lookup 路径。
// shell 内建共用的选项解析。以前 set / shopt / export / read / declare 各写一段
// 几乎相同的 for 循环, 而且都不支持紧凑写法(`set -euo pipefail` 只认 -o)。
size_t JZShell::parseBuiltinOptions(const vector<string> &args, size_t from,
                                    const string &valueFlags,
                                    std::map<string, std::pair<bool, string>> &opts)
{
    opts.clear();
    size_t i = from;
    while (i < args.size())
    {
        const string &a = args[i];
        if (a == "--")
        {
            i++;
            break;
        }
        if (a.size() < 2 || (a[0] != '-' && a[0] != '+'))
            break;   // 第一个非选项就是操作数

        if (a[1] == '-')
        {
            // 长选项: --name / --name=value
            string nm = a.substr(2);
            string val;
            const size_t eq = nm.find('=');
            if (eq != string::npos)
            {
                val = nm.substr(eq + 1);
                nm = nm.substr(0, eq);
            }
            opts[nm] = std::make_pair(true, val);
            i++;
            continue;
        }

        const bool on = (a[0] == '-');
        for (size_t k = 1; k < a.size(); k++)
        {
            const string nm(1, a[k]);
            if (valueFlags.find(a[k]) != string::npos)
            {
                string val = a.substr(k + 1);       // `-opipefail` 这种贴着的写法
                if (val.empty())
                {
                    if (i + 1 >= args.size())
                    {
                        opts[nm] = std::make_pair(on, string());
                        return i + 1;
                    }
                    val = args[++i];                // `-o pipefail`
                }
                opts[nm] = std::make_pair(on, val);
                break;                              // 值吃掉了簇里剩下的字符
            }
            opts[nm] = std::make_pair(on, string());
        }
        i++;
    }
    return i;
}

// `set -u` 用: 这个变量算不算"已定义"
bool JZShell::varIsSet(const string &name)
{
    if (name.empty())
        return false;
    const char c0 = name[0];
    if (name.size() == 1 &&
        (c0 == '?' || c0 == '#' || c0 == '@' || c0 == '*' || c0 == '$' || c0 == '!' || c0 == '-'))
        return true;
    if (c0 >= '0' && c0 <= '9')
        return true;   // 位置参数按"未设即空"处理, 不触发 -u(bash 也只看 $1.. 是否给出)
    if (m_env.find(name) != m_env.end())
        return true;
    if (m_arrays.find(name) != m_arrays.end())
        return true;
    if (!envGet(name).empty())
        return true;
    bool found = false;
    dynamicVar(name, found);
    return found;
}

std::string JZShell::varValue(const string &name)
{
    if (name == "?")
        return to_string(m_lastStatus);
    if (name == "$")
        return to_string(currentPid());   // `$$` 在子 shell 里**不变**(与 $BASHPID 的区别)
    // `$!`: 最近一个后台任务的 pid。慢路径(内建/函数/管道 `&`)没有真进程, 留空。
    if (name == "!")
        return m_lastBgPid > 0 ? to_string(m_lastBgPid) : string();
    if (name == "#")
        return to_string(m_params.size() > 1 ? m_params.size() - 1 : 0);
    if (name == "@" || name == "*")
    {
        // `"$*"` / `$*` 用 **IFS 首字符**连接(bash/POSIX 的行为; IFS=: 时是 a:b:c)。
        // 未加引号的 `$*` 之后还会按 IFS 再拆, 所以恰好等价于逐个参数。
        char sep = ' ';
        if (varIsSet("IFS"))
        {
            const string ifs = varValue("IFS");
            sep = ifs.empty() ? '\0' : ifs[0];   // IFS="" = 不插分隔符
        }
        string all;
        for (size_t k = 1; k < m_params.size(); k++)
        {
            if (k > 1 && sep != '\0')
                all += sep;
            all += m_params[k];
        }
        return all;
    }
    if (name.size() == 1 && name[0] >= '0' && name[0] <= '9')
    {
        size_t idx = (size_t)(name[0] - '0');
        if (idx == 0)
            return argv0();
        return idx < m_params.size() ? m_params[idx] : string();
    }

    // 动态变量要在普通变量之前: 它们即使被赋值也不落在 m_env 里(只改基准)
    bool dyn = false;
    string dv = dynamicVar(name, dyn);
    if (dyn)
        return dv;

    auto it = m_env.find(name);
    if (it != m_env.end())
        return it->second;
    // 数组名当标量用 = 取第 0 个元素(`$a` / `${a}`)
    auto ai = m_arrays.find(name);
    if (ai != m_arrays.end())
        return ai->second.empty() ? string() : arrayElemValue(name, ai->second[0]);
    return envGet(name);
}

std::string JZShell::expandHeredocBody(const std::string &body)
{
    string out;
    for (size_t i = 0; i < body.size();)
    {
        const char c = body[i];
        // 反斜杠只在 $ ` \ 换行 前面起转义作用(bash heredoc 的规则), 其它原样保留
        if (c == '\\' && i + 1 < body.size() &&
            (body[i + 1] == '$' || body[i + 1] == '`' || body[i + 1] == '\\' || body[i + 1] == '\n'))
        {
            out += body[i + 1];
            i += 2;
            continue;
        }
        if (c != '$' && c != '`')
        {
            out += c;
            i++;
            continue;
        }
        if (i + 1 >= body.size())
        {
            out += c;
            break;
        }

        size_t end = i + 1;
        if (c == '$')
        {
            if (body[i + 1] == '{')
            {
                int depth = 0;
                for (size_t k = i + 1; k < body.size(); k++)
                {
                    if (body[k] == '{')
                        depth++;
                    else if (body[k] == '}' && --depth == 0)
                    {
                        end = k + 1;
                        break;
                    }
                }
                if (end == i + 1)
                    end = body.size();     // 没闭合: 当到末尾
            }
            else if (body[i + 1] == '(')
            {
                int depth = 0;
                for (size_t k = i + 1; k < body.size(); k++)
                {
                    if (body[k] == '(')
                        depth++;
                    else if (body[k] == ')' && --depth == 0)
                    {
                        end = k + 1;
                        break;
                    }
                }
                if (end == i + 1)
                    end = body.size();
            }
            else if (isNameStart(body[i + 1]))
            {
                end = i + 1;
                while (end < body.size() && isNameChar(body[end]))
                    end++;
            }
            else
                end = i + 2;               // $? $# $@ $* $$ $! $0..$9
        }
        else
        {
            const size_t k = body.find('`', i + 1);
            end = (k == string::npos) ? body.size() : k + 1;
        }

        Word w;
        w.raw = body.substr(i, end - i);
        vector<string> one = expandWord(w, nullptr, /*splitFields=*/false, /*globbing=*/false);
        for (size_t k = 0; k < one.size(); k++)
        {
            if (k)
                out += ' ';                // "$@" 在正文里按空格连起来(bash 同)
            out += one[k];
        }
        i = end;
    }
    return out;
}

std::string JZShell::arrayElemValue(const std::string &base, const std::string &raw)
{
    if (raw.empty() || m_intVars.find(base) == m_intVars.end())
        return raw;
    long long v = 0;
    if (!arithEval(raw, v, nullptr))
        return "0";
    return to_string(v);
}

std::string JZShell::errorPrefix(int line, bool parseError) const
{
    const string n = to_string(line <= 0 ? 1 : line);
    if (m_errMode == 1)
        return argv0() + (parseError ? ": -c: line " : ": line ") + n + ": ";
    if (m_errMode == 2)
        return m_errScript + ": line " + n + ": ";
    return string();
}

std::string JZShell::argv0() const
{
    if (!m_argv0.empty())
        return m_argv0;                     // BASH_ARGV0 赋过值
    if (!m_params.empty())
        return m_params[0];
    return "jz_shell";
}

std::string JZShell::dynamicVar(const string &name, bool &found) const
{
    found = true;
    if (name == "BASHPID")
        return to_string(m_bashPid);
    if (name == "SECONDS")
        return to_string((steadyMsec() - m_startMsec) / 1000 + m_secondsBase);
    if (name == "EPOCHSECONDS")
        return to_string(wallUsec() / 1000000 + m_epochBase);
    if (name == "EPOCHREALTIME")
    {
        // bash 的格式: "秒.微秒", 微秒固定 6 位(`1700000000.000123`)
        long long us = wallUsec() + m_epochRealBase;
        long long sec = us / 1000000;
        long long frac = us % 1000000;
        if (frac < 0)
        {
            frac += 1000000;
            sec -= 1;
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "%lld.%06lld", sec, frac);
        return buf;
    }
    if (name == "BASH_ARGV0")
        return argv0();
    if (name == "BASH_COMMAND")
        return m_currentCmd;
    if (name == "LINENO")
        return to_string(m_lineno);
    found = false;
    return string();
}

void JZShell::syncFuncName()
{
    // $FUNCNAME[0] = 当前函数, [1] = 调用者 ... 栈底是 "main"
    // 顶层(不在任何函数里)时 bash 里这个数组是**空**的(`${#FUNCNAME[@]}` = 0,
    // `$FUNCNAME` 展开成空串), 所以栈空了就不要再补 "main"。
    vector<string> v;
    for (size_t k = m_funcStack.size(); k > 0; k--)
        v.push_back(m_funcStack[k - 1]);
    if (!v.empty())
        v.push_back("main");
    m_arrays["FUNCNAME"] = v;
}

JZShell::StageResult JZShell::runDebugTrap(int targetLine)
{
    StageResult r;
    string action = m_trapDebug;

    // `$LINENO` 在 DEBUG trap 里指的是**即将执行的那条命令**的行号, 而不是 trap
    // 动作自己的行号。动作通常就一句(`print_trap $LINENO`), 直接按字面替换最省事;
    // 动作里其它展开(cd/函数调用/变量)仍按原样执行。
    const string ln = to_string(targetLine);
    for (auto *pat : { "$LINENO", "${LINENO}" })
    {
        string p = pat;
        size_t pos = 0;
        while ((pos = action.find(p, pos)) != string::npos)
        {
            action.replace(pos, p.size(), ln);
            pos += ln.size();
        }
    }

    string perr;
    vector<Token> tokens = lex(action, perr);
    if (!perr.empty())
    {
        r.failed = true;
        r.failReason = perr;
        return r;
    }
    NodePtr node;
    Parser p(tokens);
    if (!p.parse(node, perr))
    {
        r.failed = true;
        r.failReason = perr;
        return r;
    }
    return runNode(node);
}

void JZShell::storeVar(const string &name, const string &value)
{
    // 动态变量的赋值 = 改基准, 让"下一次读"得到 value(而不是把值存起来)
    if (name == "SECONDS")
    {
        m_secondsBase = atoll(value.c_str()) - (steadyMsec() - m_startMsec) / 1000;
        return;
    }
    if (name == "EPOCHSECONDS")
    {
        m_epochBase = atoll(value.c_str()) - wallUsec() / 1000000;
        return;
    }
    if (name == "EPOCHREALTIME")
    {
        m_epochRealBase = (long long)(atof(value.c_str()) * 1000000.0) - wallUsec();
        return;
    }
    if (name == "BASH_ARGV0")
    {
        m_argv0 = value;    // 等价于改 $0(bash 的行为)
        return;
    }
    if (name == "BASHPID")
        return;             // 只读: bash 报 assignment error, 这里静默忽略
    if (name == "FUNCNAME")
        return;             // 由调用栈维护; bash 里赋值既不生效也不报错

    // `declare -i x` 之后: 赋值按算术处理(`x=1+2` -> 3)
    if (m_intVars.find(name) != m_intVars.end())
    {
        long long v = 0;
        if (arithEval(value, v, nullptr))
        {
            m_env[name] = to_string(v);
            return;
        }
    }
    m_env[name] = value;
}

// `${VAR:-word}` / `${VAR:=word}` / `${VAR:+word}`
//   · word 外面那一层引号要去掉, 但引号里的 `~` **不**展开(bash 的 quoted 语义)
//   · 未加引号时, word 词首的 `~` 展开; 只展开词首那一个
//     (`${A:=~/bin:~/bin2}` 里第二个 ~ 保持原样, 与 bash 一致)
bool JZShell::resolveDefaultOps(const string &expr, bool quoted, string &out)
{
    // 运算符必须**紧跟参数名**(名字后面可以带一个下标), 这条约束很重要:
    // 否则 `${v/-/x}` 的替换模式里的 '-' 会被当成 `${v-...}`。
    size_t k = 0;
    while (k < expr.size() && (isalnum((unsigned char)expr[k]) || expr[k] == '_' ||
                               expr[k] == '@' || expr[k] == '*' || expr[k] == '#' ||
                               expr[k] == '?' || expr[k] == '$' || expr[k] == '!'))
        k++;
    if (k < expr.size() && expr[k] == '[')
    {
        const size_t rb = expr.find(']', k);
        if (rb != string::npos)
            k = rb + 1;
    }
    if (k == 0 || k >= expr.size())
        return false;

    // 带冒号的形式优先(`:-` 而不是 `-`)
    static const char *kOps[] = {":-", ":=", ":+", "-", "=", "+", nullptr};
    size_t pos = string::npos;
    string op;
    for (int i = 0; kOps[i]; i++)
    {
        const size_t p = expr.find(kOps[i], k);
        if (p != k)
            continue;                     // 不在名字紧后面 -> 不是这个表达式
        if (pos == string::npos || op.size() < strlen(kOps[i]))
        {
            pos = p;
            op = kOps[i];
        }
    }
    if (pos == string::npos)
        return false;

    const string var = expr.substr(0, pos);
    string word = expr.substr(pos + op.size());   // `:-` 吃两个字符, `+` 只吃一个

    bool wordQuoted = quoted;
    if (word.size() >= 2 && ((word.front() == '"' && word.back() == '"') ||
                             (word.front() == '\'' && word.back() == '\'')))
    {
        word = word.substr(1, word.size() - 2);
        wordQuoted = true;
    }
    if (!wordQuoted && !word.empty() && word[0] == '~')
    {
        size_t j = 1;
        while (j < word.size() && word[j] != '/' && word[j] != ':')
            j++;
        const string user = word.substr(1, j - 1);
        string home;
        if (user.empty())
            home = envVar("HOME");
        else if (user == "+")
            home = envVar("PWD");
        else if (user == "-")
            home = envVar("OLDPWD");
        if (home.empty())
            home = envGet("USERPROFILE");
        for (char &c : home)
        {
            if (c == '\\')
                c = '/';
        }
        if (!home.empty())
            word = home + word.substr(j);
    }

    // word 里的 `$VAR` / `${VAR}` 也要展开(`${A:=~/bin:$XPATH}` 这种最常见)
    {
        string ex;
        for (size_t i = 0; i < word.size();)
        {
            if (word[i] != '$')
            {
                ex += word[i++];
                continue;
            }
            size_t j = i + 1;
            string nm;
            if (j < word.size() && word[j] == '{')
            {
                j++;
                while (j < word.size() && word[j] != '}')
                    nm += word[j++];
                if (j < word.size())
                    j++;
            }
            else if (j < word.size() && (isNameStart(word[j])))
            {
                while (j < word.size() && isNameChar(word[j]))
                    nm += word[j++];
            }
            else
            {
                ex += word[i++];
                continue;
            }
            ex += varValue(nm);
            i = j;
        }
        word = ex;
    }

    const string cur = varValue(var);
    const bool setNow = varIsSet(var);
    const bool emptyNow = cur.empty();
    const char kind = op.back();          // '-', '=' , '+'
    const bool colon = op.size() == 2;    // `:op` 形式看"空", 无冒号只看"未设置"
    if (kind == '-')
        out = (colon ? emptyNow : !setNow) ? word : cur;
    else if (kind == '=')
    {
        if (colon ? emptyNow : !setNow)
        {
            storeVar(var, word);
            out = word;
        }
        else
            out = cur;
    }
    else
        out = (colon ? !emptyNow : setNow) ? word : string();
    return true;
}

bool JZShell::expandBraced(const string &expr, vector<string> &out)
{
    if (expr.empty() || expr == "#")   // ${#} = 位置参数个数
        return false;

    string body = expr;
    bool count = false;
    if (body[0] == '#')
    {
        count = true;
        body = body.substr(1);
    }
    if (body.empty())
        return false;

    // ---- ${a[@]:off:len} / ${@:off:len} / ${*:off:len} ----
    // bash 是"先取元素列表, 再按元素切片"。以前 `${a[@]:1:2}` 会走到
    // 标量 substring 分支(把 "[@]:1:2" 当成后缀运算), 结果整个展开成空串。
    if (!count)
    {
        const size_t rb = body.find(']');
        // 只认 `${@:..}` `${*:..}` `${a[@]:..}` `${a[*]:..}`;
        // `${a[0]:1}` 是"元素 0 的子串", 不能走这里(留给下面的标量分支)
        string nm, spec;
        bool elemSlice = false;
        bool charMode = false;   // `${a[1]:..}` = 元素内子串(按字符), 不是按元素切
        if (body.compare(0, 2, "@:") == 0 || body.compare(0, 2, "*:") == 0)
        {
            elemSlice = true;
            nm = body.substr(0, 1);
            spec = body.substr(2);
        }
        else if (rb != string::npos && rb + 1 < body.size() && body[rb + 1] == ':' && rb >= 3)
        {
            const string cand = body.substr(0, rb + 1);   // 含结尾的 ']'
            elemSlice = true;
            nm = cand;
            spec = body.substr(rb + 2);
            charMode = !(cand.compare(cand.size() - 3, 3, "[@]") == 0 ||
                         cand.compare(cand.size() - 3, 3, "[*]") == 0);
        }
        if (elemSlice)
        {
            vector<string> elems;
            const size_t ab = nm.find('[');
            if (ab == string::npos)
            {
                // ${@:...} / ${*:...}: m_params 里 [0] 是 $0, 正好对上 bash 的下标
                elems = m_params;
            }
            else
            {
                const string b = nm.substr(0, ab);
                const string ix = nm.substr(ab + 1, nm.size() - ab - 2);
                auto ai = m_arrays.find(b);
                if (charMode)
                {
                    string v;
                    if (ai != m_arrays.end())
                    {
                        long long k = atoll(ix.c_str());
                        if (k >= 0 && (size_t)k < ai->second.size())
                            v = arrayElemValue(b, ai->second[(size_t)k]);
                    }
                    else if (ix == "0")
                        v = varValue(b);
                    elems.push_back(v);
                }
                else if (ai != m_arrays.end())
                {
                    // 切片里 @ 和 * 等价: 都是逐元素
                    for (const string &v : ai->second)
                        elems.push_back(arrayElemValue(b, v));
                }
            }

            const size_t colon = spec.find(':');
            const string offStr = colon == string::npos ? spec : spec.substr(0, colon);
            const string lenStr = colon == string::npos ? string() : spec.substr(colon + 1);

            auto toInt = [this](const string &s) -> long long
            {
                string t;
                for (char c : s)
                {
                    if (!isspace((unsigned char)c))
                        t += c;
                }
                if (t.empty())
                    return 0;
                long long v = 0;
                if (arithEval(t, v, nullptr))
                    return v;
                return atoll(t.c_str());
            };

            const long long n = charMode ? (long long)elems[0].size() : (long long)elems.size();
            long long off = toInt(offStr);
            long long len = lenStr.empty() ? -1 : toInt(lenStr);
            if (off < 0)
                off += n;               // 负数从尾部算(`${@: -1}` 是最后一个)
            if (off < 0)
                off = 0;
            if (off > n)
                off = n;
            long long take;
            if (len < 0)
                take = (len == -1) ? (n - off) : (n + len - off);   // 负长度=留尾部
            else
                take = len;
            if (take < 0)
                take = 0;
            if (off + take > n)
                take = n - off;

            if (charMode)
                out.push_back(elems[0].substr((size_t)off, (size_t)take));
            else
            {
                for (long long k = 0; k < take; k++)
                    out.push_back(elems[(size_t)(off + k)]);
            }
            return true;
        }
    }

    string base = body;
    string idx;
    bool subscript = false;
    size_t br = body.find('[');
    if (br != string::npos && !body.empty() && body.back() == ']')
    {
        base = body.substr(0, br);
        idx = body.substr(br + 1, body.size() - br - 2);
        subscript = true;
    }
    if (!subscript && !count)
    {
        // 参数展开运算符(bash subst.c 里那套):
        //   ${v#pat} ${v##pat} ${v%pat} ${v%%pat}   —— 去前缀/后缀(最短/最长)
        //   ${v:off} ${v:off:len}                    —— 子串
        //   ${v/pat/rep} ${v//pat/rep}               —— 替换(首个/全部)
        size_t k = 0;
        while (k < body.size() && (isalnum((unsigned char)body[k]) || body[k] == '_'))
            k++;
        string nm = body.substr(0, k);
        string rest = body.substr(k);
        if (nm.empty() || rest.empty())
            return false;   // 普通 ${VAR}: 交给老路径
        string val = varValue(nm);

        if (rest[0] == '#' || rest[0] == '%')
        {
            bool prefixOp = (rest[0] == '#');
            bool longest = rest.size() > 1 && rest[1] == rest[0];
            string pat = expandPattern(rest.substr(longest ? 2 : 1));

            size_t best = string::npos;
            for (size_t L = 0; L <= val.size(); L++)
            {
                string piece = prefixOp ? val.substr(0, L) : val.substr(val.size() - L);
                if (!globMatch(pat.c_str(), piece.c_str()))
                    continue;
                best = L;
                if (!longest)
                    break;
            }
            if (best != string::npos)
                val = prefixOp ? val.substr(best) : val.substr(0, val.size() - best);
            out.push_back(val);
            return true;
        }

        if (rest[0] == ':')
        {
            // :off / :off:len(负数按"从尾部算"处理)
            string spec = rest.substr(1);
            size_t colon = spec.find(':');
            string offStr = colon == string::npos ? spec : spec.substr(0, colon);
            string lenStr = colon == string::npos ? string() : spec.substr(colon + 1);
            long long off = atoll(offStr.c_str());
            long long len = lenStr.empty() ? -1 : atoll(lenStr.c_str());
            long long n = (long long)val.size();
            if (off < 0)
                off += n;
            if (off < 0)
                off = 0;
            if (off > n)
                off = n;
            long long take = (len < 0) ? (n - off) : len;
            if (off + take > n)
                take = n - off;
            out.push_back(val.substr((size_t)off, (size_t)take));
            return true;
        }

        if (rest[0] == '/')
        {
            bool all = rest.size() > 1 && rest[1] == '/';
            string spec = rest.substr(all ? 2 : 1);
            size_t slash = spec.find('/');
            string pat = slash == string::npos ? spec : spec.substr(0, slash);
            string rep = slash == string::npos ? string() : spec.substr(slash + 1);
            string res;
            if (pat.empty())
            {
                res = val;   // ${v//}: 无操作
            }
            else
            {
                size_t i = 0;
                bool done = false;
                while (i < val.size())
                {
                    bool hit = false;
                    if (!done || all)
                    {
                        // 贪心: 从最长开始试, 命中就替换
                        for (size_t L = val.size() - i; L > 0; L--)
                        {
                            if (globMatch(pat.c_str(), val.substr(i, L).c_str()))
                            {
                                res += rep;
                                i += L;
                                hit = true;
                                if (!all)
                                    done = true;
                                break;
                            }
                        }
                    }
                    if (!hit)
                        res += val[i++];
                    if (done && !all)
                    {
                        res += val.substr(i);
                        break;
                    }
                }
            }
            out.push_back(res);
            return true;
        }
        return false;
    }

    auto it = m_arrays.find(base);
    bool isArray = (it != m_arrays.end());

    if (subscript && idx == "@")
    {
        if (count)
            out.push_back(to_string(isArray ? it->second.size() : (varValue(base).empty() ? 0u : 1u)));
        else if (isArray)
        {
            // 空数组不产生字段(bash 同); 非空则一个元素一个字段
            for (const string &v : it->second)
                out.push_back(arrayElemValue(base, v));
        }
        return true;
    }
    if (subscript && idx == "*")
    {
        string all;
        if (isArray)
        {
            for (size_t k = 0; k < it->second.size(); k++)
            {
                if (k)
                    all += " ";
                all += arrayElemValue(base, it->second[k]);
            }
        }
        out.push_back(count ? to_string(all.size()) : all);
        return true;
    }
    if (subscript)
    {
        long long n = -1;
        if (!idx.empty())
        {
            bool digits = true;
            for (char c : idx)
            {
                if (c < '0' || c > '9')
                    digits = false;
            }
            if (digits)
                n = atoll(idx.c_str());
        }
        string v;
        if (n >= 0)
        {
            if (isArray)
            {
                if ((size_t)n < it->second.size())
                    v = arrayElemValue(base, it->second[(size_t)n]);
            }
            else if (n == 0)
            {
                v = varValue(base);
            }
        }
        out.push_back(count ? to_string(v.size()) : v);
        return true;
    }

    // 只有 #{...}: ${#VAR} = 值长度
    out.push_back(to_string(varValue(base).size()));
    return true;
}

// [[ ... ]] 求值。&& / || 短路; 返回 false 表示求值过程出错(例如正则非法)。
bool JZShell::condEval(const NodePtr &n, bool &out, string &error)
{
    if (!n)
    {
        out = false;
        return true;
    }

    // 括号分组
    if (n->cond)
    {
        if (!condEval(n->cond, out, error))
            return false;
        if (n->negate)
            out = !out;
        return true;
    }

    // && / || 链(左结合 + 短路)
    if (!n->parts.empty())
    {
        bool acc = false;
        for (size_t i = 0; i < n->parts.size(); i++)
        {
            bool v = false;
            if (!condEval(n->parts[i], v, error))
                return false;
            if (i == 0)
            {
                acc = v;
                continue;
            }
            const string &op = n->ops[i - 1];
            if (op == "&&")
            {
                acc = acc && v;
                if (!acc)
                {
                    out = false;
                    return true;   // 短路
                }
            }
            else
            {
                acc = acc || v;
                if (acc)
                {
                    out = true;
                    return true;   // 短路
                }
            }
        }
        out = acc;
        return true;
    }

    // 单个比较/测试
    string a, b;
    if (!n->words.empty())
    {
        vector<string> lv = expandWord(n->words[0], nullptr);
        a = lv.empty() ? string() : lv[0];
    }
    const string &op = n->var;
    bool result = false;

    if (n->words.size() > 1)
    {
        // 右操作数: ==/!=/=~ 要保留通配/正则字符 ⇒ 用 expandPattern(不做路径展开)
        if (op == "==" || op == "=" || op == "!=" || op == "=~")
            b = expandPattern(n->words[1].raw);
        else
            b = expandString(n->words[1].raw);

        // 右操作数**加了引号**就是字面量, 不做通配(bash: `[[ x == "a*" ]]` 为假)
        bool rhsQuoted = !n->words[1].raw.empty() &&
                         (n->words[1].raw[0] == '"' || n->words[1].raw[0] == '\'');
        if (op == "=" || op == "==")
            result = rhsQuoted ? (a == b) : globMatch(b.c_str(), a.c_str());
        else if (op == "!=")
            result = rhsQuoted ? (a != b) : !globMatch(b.c_str(), a.c_str());
        else if (op == "=~")
        {
            try
            {
                result = std::regex_search(a, std::regex(b));
            }
            catch (const std::exception &e)
            {
                error = string("[[ ]] 正则无效: ") + e.what();
                return false;
            }
        }
        else if (op == "<")
            result = a < b;
        else if (op == ">")
            result = a > b;
        else if (op == "-eq" || op == "-ne" || op == "-lt" || op == "-le" ||
                 op == "-gt" || op == "-ge")
        {
            long long x = atoll(a.c_str());
            long long y = atoll(b.c_str());
            if (op == "-eq") result = (x == y);
            else if (op == "-ne") result = (x != y);
            else if (op == "-lt") result = (x < y);
            else if (op == "-le") result = (x <= y);
            else if (op == "-gt") result = (x > y);
            else result = (x >= y);
        }
        else if (op == "-nt" || op == "-ot")
        {
            JZFileInfo fa(absolutePath(a));
            JZFileInfo fb(absolutePath(b));
            if (!fa.exists() || !fb.exists())
            {
                error = "[[ ]] -nt/-ot: 文件不存在";
                return false;
            }
            bool newer = fa.lastModified() > fb.lastModified();
            result = (op == "-nt") ? newer : !newer;
        }
        else
        {
            error = "[[ ]] 不认识的运算符: " + op;
            return false;
        }
    }
    else if (!op.empty() && op[0] == '-')
    {
        // 单目测试
        string path = a.empty() ? string() : absolutePath(a);
        if (op == "-n") result = !a.empty();
        else if (op == "-z") result = a.empty();
        else if (op == "-t") result = false;                 // 无终端
        else if (op == "-e") result = !path.empty() && JZFileInfo::exists(path);
        else if (op == "-f") result = JZFileInfo(path).isFile();
        else if (op == "-d") result = JZFileInfo(path).isDir();
        else if (op == "-s") result = JZFileInfo(path).size() > 0;
        else if (op == "-r" || op == "-w" || op == "-x")
            result = !path.empty() && JZFileInfo::exists(path);   // 无 ACL 语义, 退化为"存在"
        else if (op == "-L" || op == "-h") result = false;   // JZFileInfo 没有链接信息, 恒假
        else if (op == "-p" || op == "-S" || op == "-b" || op == "-c") result = false;
        else
        {
            error = "[[ ]] 不认识的测试: " + op;
            return false;
        }
    }
    else
    {
        // 单个词: 非空为真(bash 语义)
        result = !a.empty();
    }

    if (n->negate)
        result = !result;
    out = result;
    return true;
}

// ==========================================================================
// $(( )) 算术展开(整数; 支持常用运算符、括号、变量、三元)
// ==========================================================================
namespace
{
struct Arith
{
    const string &s;
    function<string(const string &)> var;   // 按值存: 传进来的 lambda 会先转成临时 function
    size_t i = 0;
    string err;

    Arith(const string &src, function<string(const string &)> v) : s(src), var(std::move(v)) {}

    void ws()
    {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
            i++;
    }
    bool eat(const char *op)
    {
        ws();
        size_t n = strlen(op);
        if (s.compare(i, n, op) == 0)
        {
            i += n;
            return true;
        }
        return false;
    }
    // 单字符运算符, 且不能是两个相同字符组成的 "&&" / "||" / "<<" / ">>" / "==" / "<=" ...
    bool eat1(char c)
    {
        ws();
        if (i < s.size() && s[i] == c && !(i + 1 < s.size() && s[i + 1] == c) &&
            !(i + 1 < s.size() && s[i + 1] == '='))
        {
            i++;
            return true;
        }
        return false;
    }
    long long atom()
    {
        ws();
        if (i >= s.size())
            return 0;
        if (s[i] == '$')
            i++;
        if (i < s.size() && (isalpha((unsigned char)s[i]) || s[i] == '_'))
        {
            string name;
            while (i < s.size() && (isalnum((unsigned char)s[i]) || s[i] == '_'))
                name += s[i++];
            string v = var(name);
            return v.empty() ? 0 : strtoll(v.c_str(), nullptr, 0);
        }
        if (i < s.size() && isdigit((unsigned char)s[i]))
        {
            size_t start = i;
            while (i < s.size() && isalnum((unsigned char)s[i]))
                i++;
            return strtoll(s.substr(start, i - start).c_str(), nullptr, 0);
        }
        err = string("syntax error: invalid arithmetic operator (error token is \"") +
              (i < s.size() ? s.substr(i, 1) : string()) + "\")";
        i = s.size();
        return 0;
    }
    long long unary()
    {
        if (eat("!"))
            return !unary();
        if (eat("~"))
            return ~unary();
        if (eat("-"))
            return -unary();
        if (eat("+"))
            return unary();
        if (eat("("))
        {
            long long v = ternary();
            if (!eat(")"))
                err = "syntax error: missing `)' in arithmetic expression";
            return v;
        }
        return atom();
    }
    long long power()
    {
        long long v = unary();
        if (eat("**"))
        {
            long long e = power();
            long long r = 1;
            for (long long k = 0; k < e; k++)
                r *= v;
            return r;
        }
        return v;
    }
    long long mul()
    {
        long long v = power();
        for (;;)
        {
            if (eat("*")) v *= power();
            else if (eat("/")) { long long d = power(); v = d ? v / d : 0; }
            else if (eat("%")) { long long d = power(); v = d ? v % d : 0; }
            else return v;
        }
    }
    long long add()
    {
        long long v = mul();
        for (;;)
        {
            if (eat("+")) v += mul();
            else if (eat("-")) v -= mul();
            else return v;
        }
    }
    long long shift()
    {
        long long v = add();
        for (;;)
        {
            if (eat("<<")) v <<= add();
            else if (eat(">>")) v >>= add();
            else return v;
        }
    }
    long long rel()
    {
        long long v = shift();
        for (;;)
        {
            if (eat("<=")) v = (v <= shift());
            else if (eat(">=")) v = (v >= shift());
            else if (eat("<")) v = (v < shift());
            else if (eat(">")) v = (v > shift());
            else return v;
        }
    }
    long long eq()
    {
        long long v = rel();
        for (;;)
        {
            if (eat("==")) v = (v == rel());
            else if (eat("!=")) v = (v != rel());
            else return v;
        }
    }
    long long band() { long long v = eq(); while (eat1('&')) v &= eq(); return v; }
    long long bxor() { long long v = band(); while (eat1('^')) v ^= band(); return v; }
    long long bor() { long long v = bxor(); while (eat1('|')) v |= bxor(); return v; }
    long long land()
    {
        long long v = bor();
        for (;;)
        {
            ws();
            if (s.compare(i, 2, "&&") == 0) { i += 2; long long r = bor(); v = (v && r); }
            else return v;
        }
    }
    long long lor()
    {
        long long v = land();
        for (;;)
        {
            ws();
            if (s.compare(i, 2, "||") == 0) { i += 2; long long r = land(); v = (v || r); }
            else return v;
        }
    }
    long long ternary()
    {
        long long c = lor();
        if (eat("?"))
        {
            long long a = ternary();
            if (!eat(":"))
                err = "syntax error: missing `:' in arithmetic expression";
            long long b = ternary();
            return c ? a : b;
        }
        return c;
    }
    bool run(long long &out)
    {
        out = ternary();
        ws();
        if (err.empty() && i < s.size())
            err = "syntax error in expression (error token is \"" + s.substr(i) + "\")";
        return err.empty();
    }
};
} // namespace

bool JZShell::arithEval(const string &expr, long long &out, string *error)
{
    auto var = [this](const string &name) -> string
    {
        if (name == "?")
            return to_string(m_lastStatus);
        auto it = m_env.find(name);
        if (it != m_env.end())
            return it->second;
        return envGet(name);
    };
    Arith a(expr, var);
    bool ok = a.run(out);
    if (!ok && error)
        *error = a.err;
    return ok;
}

// (( )) 与 for (( )) 用: 在 arithEval 之上补"赋值 + 自增自减", 并把结果写回变量表。
//   i=0 / i+=2 / i++ / ++i / i-- / --i / 逗号分隔的多条
// 变量表就是 m_env(与 export 同一个), 所以循环变量的生命周期与 shell 变量一致。
bool JZShell::arithRun(const string &expr, long long &out, string *error)
{
    out = 0;

    // 按顶层逗号切成多条
    vector<string> parts;
    {
        int depth = 0;
        size_t start = 0;
        for (size_t i = 0; i < expr.size(); i++)
        {
            char c = expr[i];
            if (c == '(')
                depth++;
            else if (c == ')')
                depth--;
            else if (c == ',' && depth == 0)
            {
                parts.push_back(expr.substr(start, i - start));
                start = i + 1;
            }
        }
        parts.push_back(expr.substr(start));
    }

    for (const string &rawPart : parts)
    {
        size_t a = rawPart.find_first_not_of(" \t\r\n");
        if (a == string::npos)
            continue;
        size_t b = rawPart.find_last_not_of(" \t\r\n");
        string s = rawPart.substr(a, b - a + 1);

        // 变量名(可带 $)
        size_t p = 0;
        if (p < s.size() && s[p] == '$')
            p++;
        size_t nameStart = p;
        while (p < s.size() && (isalnum((unsigned char)s[p]) || s[p] == '_'))
            p++;
        string name = s.substr(nameStart, p - nameStart);

        auto getVar = [&]() -> long long
        {
            string v = envVar(name);
            return v.empty() ? 0 : strtoll(v.c_str(), nullptr, 0);
        };
        auto setVar = [&](long long v) { m_env[name] = to_string(v); };

        bool done = false;
        if (!name.empty())
        {
            string pre = s.substr(0, nameStart);
            string rest = s.substr(p);
            size_t ra = rest.find_first_not_of(" \t");
            string tail = (ra == string::npos) ? string() : rest.substr(ra);

            if (tail == "++" || pre == "++")
            {
                setVar(getVar() + 1);
                out = getVar();
                done = true;
            }
            else if (tail == "--" || pre == "--")
            {
                setVar(getVar() - 1);
                out = getVar();
                done = true;
            }
            else if (!tail.empty())
            {
                // 复合赋值要排在 "=" 前面判断
                static const char *kAssign[] = {
                    "<<=", ">>=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "="
                };
                for (const char *op : kAssign)
                {
                    size_t n = strlen(op);
                    if (tail.compare(0, n, op) != 0)
                        continue;
                    // `==` 是比较不是赋值(单字符 "=" 会误命中)
                    if (n == 1 && op[0] == '=' && tail.size() > 1 && tail[1] == '=')
                        continue;

                    string rhs = tail.substr(n);
                    size_t ra2 = rhs.find_first_not_of(" \t");
                    if (ra2 == string::npos)
                        rhs.clear();
                    else
                        rhs = rhs.substr(ra2);

                    long long v = 0;
                    if (!rhs.empty() && !arithEval(rhs, v, error))
                        return false;

                    long long cur = getVar();
                    if (!strcmp(op, "="))
                        cur = v;
                    else if (!strcmp(op, "+="))
                        cur += v;
                    else if (!strcmp(op, "-="))
                        cur -= v;
                    else if (!strcmp(op, "*="))
                        cur *= v;
                    else if (!strcmp(op, "/="))
                        cur = (v == 0) ? 0 : cur / v;
                    else if (!strcmp(op, "%="))
                        cur = (v == 0) ? 0 : cur % v;
                    else if (!strcmp(op, "<<="))
                        cur <<= v;
                    else if (!strcmp(op, ">>="))
                        cur >>= v;
                    else if (!strcmp(op, "&="))
                        cur &= v;
                    else if (!strcmp(op, "|="))
                        cur |= v;
                    else if (!strcmp(op, "^="))
                        cur ^= v;
                    setVar(cur);
                    out = cur;
                    done = true;
                    break;
                }
            }
        }

        if (done)
            continue;

        long long v = 0;
        if (!arithEval(s, v, error))
            return false;
        out = v;
    }
    return true;
}

// `$( )`: 跑内层命令, 只取 stdout。cwd/env/退出码/退出标志全部回滚 —— 等价于子 shell。
bool JZShell::captureCommand(const string &text, string &out, string *error)
{
    if (m_depth > 32)
    {
        if (error)
            *error = "command substitution nested too deeply";
        return false;
    }

    string savedCwd = m_cwd;
    map<string, string> savedEnv = m_env;
    map<string, vector<string>> savedArrays = m_arrays;   // 数组也是变量: 子 shell 里的改动不回传
    int savedStatus = m_lastStatus;
    bool savedExiting = m_exiting;
    int savedExitCode = m_exitCode;
    int savedBashPid = m_bashPid;     // `$( )` 是子 shell: $BASHPID 变, $$ 不变
    // 命令替换**不继承**父 shell 的 -e(bash 的行为: `set -e; echo $(false; echo ok)`
    // 照样输出 ok); 里面自己写 `set -e` 仍然生效。退出时恢复父的状态。
    const bool savedErrexit = m_errexit;
    const bool savedInCond = m_inCond;
    m_errexit = false;
    m_inCond = false;

    m_bashPid++;
    bool ok = true;
    m_depth++;
    m_captureLevel++;      // 这里的输出是字符串, 不能直写

    {
        string perr;
        NodePtr node;
        vector<Token> tokens = lex(text, perr);
        bool parsed = perr.empty();
        if (parsed)
        {
            Parser p(tokens);
            parsed = p.parse(node, perr);
        }
        if (!parsed)
        {
            if (error)
                *error = perr;
            ok = false;
        }
        else
        {
            StageResult r = runNode(node);
            // `x=$(cmd)` 的退出码 = 这条命令替换的退出码(bash 的语义), 由赋值语句取用
            m_lastCaptureStatus = r.status;
            if (r.failed)
            {
                if (error)
                    *error = r.failReason;
                ok = false;
            }
            else
            {
                out = r.out;
            }
        }
    }
    m_depth--;
    m_captureLevel--;

    m_bashPid = savedBashPid;
    m_cwd = savedCwd;
    m_env = savedEnv;
    m_arrays = savedArrays;
    m_lastStatus = savedStatus;
    m_exiting = savedExiting;
    m_exitCode = savedExitCode;
    m_errexit = savedErrexit;
    m_inCond = savedInCond;

    // bash 会去掉结尾的换行
    while (!out.empty() && out.back() == '\n')
        out.pop_back();
    if (!out.empty() && out.back() == '\r')
        out.pop_back();
    return ok;
}

string JZShell::absolutePath(const string &p) const
{
    if (p.empty())
        return m_cwd;

    // MSYS/Git-Bash 风格路径: `/c/...`、`/tmp`、`/usr/bin` 先翻成真实 Windows 路径。
    // 不翻的话 `cd /tmp`、`/c/.../jz_shell.exe` 这类全都会被当成区外/不存在。
#ifdef _WIN32
    // ⚠ 必须把临时目录表传进去: 否则 `/tmp/x` 会走挂载表翻成 <GitRoot>/tmp/x(不存在),
    //   `cd /tmp/xxx` 直接报"不是目录" —— 而 /tmp 正是 agent 最常用的落点。
    string in = codeBashPathToWindows(p, zoneTempDirs());
#else
    string in = p;
#endif
    if (in.empty())
        return m_cwd;
    if (in[0] == '/')
        return normalizePath(in);
#ifdef _WIN32
    if (in.size() > 1 && in[1] == ':')
        return normalizePath(in);
#endif
    return normalizePath(joinPath(m_cwd, in));
}

string JZShell::resolveSpawnProgram(const string &name) const
{
    if (pathConvEnabled())
    {
        string conv;
        if (driveMountToWin(name, conv))
            return resolveProgram(conv);
    }
    return resolveProgram(name);
}

bool JZShell::pathConvEnabled() const
{
#ifdef _WIN32
    // 与 MSYS 同名开关; 脚本里 `MSYS_NO_PATHCONV=1 cmd /c ...` 也能生效
    auto it = m_env.find("MSYS_NO_PATHCONV");
    if (it != m_env.end())
        return it->second.empty();
    const string fromOs = envGet("MSYS_NO_PATHCONV");
    return fromOs.empty();
#else
    return false;
#endif
}

string JZShell::resolveProgram(const string &name) const
{
    if (name.empty())
        return string();

    // 带路径: 直接按 cwd 解析
    if (name.find('/') != string::npos || name.find('\\') != string::npos)
    {
        string p = absolutePath(name);
        return JZFileInfo(p).isFile() ? p : string();
    }

    // 显式映射优先: 打包进来的编译器/运行时登记成短名(见 addProgramPath)
    {
        string mapped = programPath(name);
        if (!mapped.empty() && JZFileInfo(mapped).isFile())
            return mapped;
    }

    vector<string> dirs = m_pathDirs;
    for (const string &d : splitChar(envGet("PATH"), kPathSep))
    {
        if (d.empty())
            continue;
        // MSYS 的 PATH 里是 `/usr/bin` 这种形式, 同样要翻成真实路径才找得到
#ifdef _WIN32
        dirs.push_back(codeBashPathToWindows(d));
#else
        dirs.push_back(d);
#endif
    }
#ifdef _WIN32
    static const char *kExeSuffix[] = { "", ".exe", ".cmd", ".bat" };
#else
    static const char *kExeSuffix[] = { "" };
#endif
    for (const string &d : dirs)
    {
        for (const char *suf : kExeSuffix)
        {
            string p = joinPath(d, name + suf);
            if (JZFileInfo(p).isFile())
                return p;
        }
    }
    return string();
}

// ==========================================================================
// 执行
// ==========================================================================
// 直写: 语句边界把攒下的文本落 fd 并清空。这样才能和"子进程直写"的顺序对上 ——
// 否则我们自己的文本会攒到最后一并打印, 全排在子进程输出后面。
void JZShell::flushDirect(StageResult &r)
{
    if (!m_directOutput || m_captureLevel > 0)
        return;
    if (!r.out.empty())
    {
        fwrite(r.out.data(), 1, r.out.size(), stdout);
        fflush(stdout);
        r.out.clear();
    }
    if (!r.err.empty())
    {
        fwrite(r.err.data(), 1, r.err.size(), stderr);
        if (r.err.back() != '\n')
            fputc('\n', stderr);   // 错误行必须自带换行, 否则下一条输出会粘上来
        fflush(stderr);
        r.err.clear();
    }
}

// 子进程环境: `export` 标记过的变量 + 本条命令的前缀赋值 + PATH(短名目录前插)。
// 前台(runExternal)和后台(detach)共用一份, 否则 `export PORT=8080; ./server &`
// 里服务读不到 PORT —— 前台能看到、后台看不到, 是最难查的那种不一致。
std::map<std::string, std::string> JZShell::childEnv()
{
    std::map<std::string, std::string> env = m_childEnvExtra;
    m_childEnvExtra.clear();
    for (const std::string &nm : m_exported)
    {
        auto it = m_env.find(nm);
        if (it != m_env.end())
            env[nm] = it->second;
    }
    // 短名所在目录前插进 PATH: `timeout 8 code_runtime ...` 里内层名字只能靠 PATH 找
    // (见 childPathValue 的注释)。前缀赋值 `PATH=x cmd` 给的 base 也要保留。
    auto pit = env.find("PATH");
    const string pathValue = childPathValue(pit == env.end() ? string() : pit->second);
    if (!pathValue.empty())
        env["PATH"] = pathValue;
    return env;
}

JZShell::StageResult JZShell::runExternal(const string &program, const vector<string> &args,
                                          const string &stdinText, bool merged, bool inheritOutput)
{
    StageResult r;
    vector<string> argv(args.begin() + 1, args.end());

    // Git Bash 在启动原生程序时会翻参数里的盘符挂载路径(见 driveMountToWin)
    if (pathConvEnabled())
    {
        for (string &a : argv)
            a = convertBashPathArg(a);
    }

    // 整条命令行的预算已经用光: 别起进程了(否则等于白跑一条)
    if (remainingTimeoutMs() < 0)
    {
        r.failed = true;
        r.status = 124;
        r.failReason = "命令超时, 已被终止: " + program;
        return r;
    }

    JZProcess proc;
    proc.setWorkingDirectory(m_cwd);
    proc.setProcessChannelMode(merged ? JZProcess::MergedChannels : JZProcess::SeparateChannels);
    // 直写模式下让子进程直接写我们的 fd 1 / fd 2(bash 的做法): 两路的先后由内核保证,
    // 不会出现"整块 stdout 拼整块 stderr"那种错序。
    if (inheritOutput)
        proc.setOutputChannelMode(JZProcess::InheritOutputChannel);

    // 子进程环境: `export` 标记过的变量 + 本条命令的前缀赋值(见 childEnv)。
    {
        std::map<std::string, std::string> env = childEnv();
        if (!env.empty())
            proc.setEnvironmentVariables(env);
    }
    // 脚本是从 stdin 读进来的, 且我们没给这条命令喂输入 ⇒ 让它继承我们的 stdin,
    // 这样它能接着读脚本剩下的内容(bash 的行为)
    if (m_inheritStdin && stdinText.empty())
        proc.setInputChannelMode(JZProcess::InheritInputChannel);
    if (!proc.start(program, argv))
    {
        r.failed = true;
        r.status = 127;
        r.failReason = proc.lastStartError().empty() ? ("无法启动: " + program) : proc.lastStartError();
        return r;
    }
    if (!stdinText.empty())
        proc.write(JZByteArray(stdinText.data(), (int)stdinText.size()));
    proc.closeWriteChannel();

    // 分片等待: 既要能到点强杀, 也要能让另一个线程 requestCancel() 打断。
    // 这里等的是**整条命令行剩下的预算**(不是重新给这一条命令一整份 timeout)。
    const int leftMs = remainingTimeoutMs();
    int timeout = (leftMs == 0) ? 0x7fffffff : leftMs;
    const int kSliceMs = 100;
    int waited = 0;

    // ⚠ 被杀之前子进程**已经产出的内容不能丢**: 以前超时/取消两条分支直接 return, 管道里的
    //   输出(以及它调的 console.log)全被扔掉, 模型只看到一句"命令超时" —— 实测它据此断定
    //   "服务没起来/程序没输出", 反复重跑同一条命令(五子棋那例烧了 2.26M token)。
    //   真实终端里进程被 kill 前打印的东西是看得到的, 这里照做。
    auto drainOutput = [&]()
    {
        if (inheritOutput)   // 继承模式: 子进程直接写我们的 fd, 没有管道可读
            return;
        JZByteArray out = proc.readAllStandardOutput();
        r.out.assign(out.data(), out.size());
        if (!merged)
        {
            JZByteArray err = proc.readAllStandardError();
            if (err.size() > 0)
            {
                if (!r.out.empty() && r.out.back() != '\n')
                    r.out += '\n';
                r.out.append(err.data(), err.size());
            }
        }
    };

    while (true)
    {
        if (proc.waitForFinished(kSliceMs))
            break;
        waited += kSliceMs;
        if (m_cancelRequested)
        {
            proc.killProcess();
            proc.waitForFinished(2000);
            drainOutput();
            r.status = 130;
            r.err = "命令已被取消: " + args[0];
            return r;
        }
        if (waited >= timeout)
        {
            proc.killProcess();
            proc.waitForFinished(2000);
            drainOutput();
            r.status = 124;
            r.err = "命令超时, 已被终止: " + args[0];
            return r;
        }
    }

    if (!inheritOutput)   // 继承模式下子进程已经自己写完了, 没有管道可读
    {
        JZByteArray out = proc.readAllStandardOutput();
        r.out.assign(out.data(), out.size());
        if (!merged)
        {
            JZByteArray err = proc.readAllStandardError();
            r.err.assign(err.data(), err.size());
        }
    }
    r.status = proc.exitCode();
    return r;
}

namespace
{
// 定义在文件后部(内建分发表旁), 这里前置声明: type / command -v 也要用它判断"是不是内建"
bool isBuiltinName(const std::string &n);
// 语法保留字, 供 `type -t` 区分 keyword / builtin
bool isReservedWord(const std::string &n);
}

JZShell::StageResult JZShell::runBuiltin(const string &name, const vector<string> &args,
                                         const string &stdinText, bool *handled)
{
    StageResult r;
    *handled = true;

    if (name == "cd")
    {
        // `cd -` = 回 OLDPWD, 并把新目录打出来(bash 的行为)
        bool back = (args.size() >= 2 && args[1] == "-");
        string target = args.size() >= 2 ? args[1] : envGet("HOME");
        if (back)
            target = envVar("OLDPWD");
        if (target.empty())
        {
            r.status = 1;
            r.err = errorPrefix(m_lineno, false) + "cd: HOME not set";
            return r;
        }
        string abs = absolutePath(target);
        if (!JZFileInfo(abs).isDir())
        {
            r.status = 1;
            // 与 bash 一致: `<script>: line N: cd: <path>: No such file or directory`
            r.err = errorPrefix(m_lineno, false) + "cd: " + target + ": No such file or directory";
            return r;
        }
        const string old = m_cwd;
        m_cwd = abs;

        // PWD / OLDPWD 记的是**逻辑路径**(用户敲的那个), 不是 Windows 物理路径:
        // bash 的 `pwd` / `~+` / `~-` 都按逻辑路径给, 例如 `cd /usr` 之后打 `/usr`,
        // 而不是 `C:/Program Files/Git/usr`。这里纯文本地规范化(处理 . 和 ..)。
        auto logicalJoin = [](const string &base, const string &p) -> string
        {
            string s = p;
            for (char &ch : s)
            {
                if (ch == '\\')
                    ch = '/';
            }
            // 绝对路径: `/...` 或盘符形式 `C:/...`(以前只认前者, 于是 `cd C:/tmp` 会被当成相对路径
            // 拼到 PWD 后面 —— 物理 cwd 是对的, 但 `pwd`/`$PWD` 给出垃圾值)
            const bool abs = (!s.empty() && s[0] == '/') ||
                             (s.size() > 2 && isalpha((unsigned char)s[0]) && s[1] == ':' && s[2] == '/');
            string work = abs ? s : (base + "/" + s);
            string drive;                        // `C:` 这类盘符要原样留着
            if (work.size() > 1 && work[1] == ':')
            {
                drive = work.substr(0, 2);
                work = work.substr(2);
            }
            vector<string> segs;
            size_t i = 0;
            while (i <= work.size())
            {
                size_t e = work.find('/', i);
                if (e == string::npos)
                    e = work.size();
                const string seg = work.substr(i, e - i);
                i = e + 1;
                if (seg.empty() || seg == ".")
                    continue;
                if (seg == "..")
                {
                    if (!segs.empty())
                        segs.pop_back();
                    continue;
                }
                segs.push_back(seg);
            }
            string out = drive + "/";
            for (size_t k = 0; k < segs.size(); k++)
            {
                if (k)
                    out += "/";
                out += segs[k];
            }
            return out;
        };

        string oldPwd = envVar("PWD");
        if (oldPwd.empty())
            oldPwd = old;
        const string logical = logicalJoin(oldPwd, target);
        m_env["OLDPWD"] = oldPwd;
        m_env["PWD"] = logical;
        if (back)
            r.out = logical + "\n";
        return r;
    }

    if (name == "pwd")
    {
        // bash 默认给**逻辑**路径(见 cd 里的 PWD 维护)
        const string p = envVar("PWD");
        r.out = (p.empty() ? m_cwd : p) + "\n";
        return r;
    }

    // ---- 兼容用内建: 接受但不改变语义(让脚本能往下跑, 不再报"找不到命令") ----
    // shopt: 只认 extdebug(DEBUG trap 返回 2 时跳过命令); 其它开关一律当成功
    if (name == "shopt")
    {
        // 选项走共用解析:`shopt -su nullglob` 这种紧凑写法也认
        std::map<string, std::pair<bool, string>> opts;
        const size_t first = parseBuiltinOptions(args, 1, "", opts);
        const bool set = opts.count("s") > 0 && opts["s"].first;
        const bool unset = opts.count("u") > 0 && opts["u"].first;
        for (size_t i = first; i < args.size(); i++)
        {
            const string &a = args[i];
            if (a == "extdebug" && (set || unset))
                m_extdebug = set;
            else if (a == "lastpipe" && (set || unset))
                m_lastpipe = set;
            else if (a == "nullglob" && (set || unset))
                m_nullglob = set;
            // extglob / failglob 等: 我们的 glob 本来就是扩展语义, 静默接受
        }
        return r;
    }

    // trap: 真正支持 `trap '动作' DEBUG`(调试/单步要它); 其它信号记为接受
    // (信号语义要等 PTY/信号层, 这里不假装能拦)
    if (name == "trap")
    {
        if (args.size() >= 3 && args.back() == "DEBUG")
        {
            if (args[1] == "-")
                m_trapDebug.clear();
            else
                m_trapDebug = args[1];
        }
        return r;
    }

    // typeset / declare: 只认 -i(整数属性, `declare -i n` 之后 n 的赋值按算术算) 与
    // name=value; 其它开关(-x/-a/-r …)忽略
    if (name == "typeset" || name == "declare")
    {
        bool intAttr = false;
        for (size_t i = 1; i < args.size(); i++)
        {
            const string &a = args[i];
            if (!a.empty() && a[0] == '-' && a.size() > 1 && a[1] != '-')
            {
                if (a.find('i') != string::npos)
                    intAttr = true;
                continue;
            }
            if (a.empty() || a[0] == '+' || a == "--")
                continue;
            size_t eq = a.find('=');
            string nm = (eq == string::npos) ? a : a.substr(0, eq);
            if (nm.empty())
                continue;
            if (intAttr)
                m_intVars.insert(nm);
            if (eq != string::npos)
            {
                // 函数里的 typeset/declare 也是"局部变量"(bash): 记一份原值,
                // 函数返回时由 runFunction 恢复(`typeset IFS=:` 不该泄漏到调用方)
                if (!m_funcStack.empty())
                {
                    LocalSave ls;
                    ls.name = nm;
                    auto it = m_env.find(nm);
                    if (it != m_env.end())
                    {
                        ls.had = true;
                        ls.value = it->second;
                    }
                    m_locals.push_back(ls);
                }
                storeVar(nm, a.substr(eq + 1));
            }
        }
        return r;
    }

    // set: `set -- a b c` 改位置参数; `set -o posix` / `set +o posix` 记下来
    // (posix 模式收窄 tilde 展开: 只有真正的赋值语句才在 '=' / ':' 之后展开)
    if (name == "set")
    {
        // 选项解析走共用实现: `set -euo pipefail` / `set +e` / `set -o posix` 都吃
        std::map<string, std::pair<bool, string>> opts;
        const size_t i = parseBuiltinOptions(args, 1, "oO", opts);
        for (const auto &kv : opts)
        {
            const bool on = kv.second.first;
            const string &nm = kv.first;
            if (nm == "e")
                m_errexit = on;
            else if (nm == "u")
                m_nounset = on;
            else if (nm == "x")
                m_xtrace = on;
            else if (nm == "o" || nm == "O")
            {
                const string &opt = kv.second.second;
                if (opt == "posix")
                    m_posix = on;
                else if (opt == "pipefail")
                    m_pipefail = on;
                else if (opt == "errexit")
                    m_errexit = on;
                else if (opt == "nounset")
                    m_nounset = on;
                else if (opt == "xtrace")
                    m_xtrace = on;
                else if (opt == "lastpipe")
                    m_lastpipe = on;
            }
            // 其余开关(-f -n -v -C -H -B -m -b ...)对解释器没有影响, 静默接受
        }
        if (i < args.size())
        {
            // m_params[0] 是 $0 —— `set -- a b` 只改 $1.., 不能把 $0 挤掉
            vector<string> np;
            np.push_back(m_params.empty() ? string("jz_shell") : m_params[0]);
            np.insert(np.end(), args.begin() + i, args.end());
            m_params = np;
        }
        return r;
    }

    // exec: 有命令就替换执行(这里退化成"跑完就结束本条"), 只有重定向则当成功
    if (name == "exec")
    {
        if (args.size() < 2)
            return r;
        vector<string> argv(args.begin() + 1, args.end());
        for (const string &a : argv)
        {
            if (a.empty() || a[0] != '>')
                continue;
            return r;   // `exec > file` 这类只做重定向的, 交给外层
        }
        string program = resolveSpawnProgram(argv[0]);
        if (program.empty())
        {
            r.status = 127;
            r.err = errorPrefix(m_lineno, false) + argv[0] + ": command not found";
            return r;
        }
        r = runExternal(program, argv, stdinText, false);
        // bash 的 `exec cmd` 是**替换进程**: 后面的语句不再执行。
        // 不置这个标志的话脚本会继续往下跑(实测 jzsh 包装器执行两遍:
        // 一遍 -c 正确、一遍把 $0 当脚本打开 → "打不开脚本: bash5")。
        m_exiting = true;
        m_exitCode = r.status;
        return r;
    }

    // ---- alias / 目录栈 / 历史等常用内建 ----
    if (name == "alias")
    {
        if (args.size() == 1)
        {
            for (const auto &kv : m_aliases)
                r.out += "alias " + kv.first + "='" + kv.second + "'\n";
            return r;
        }
        for (size_t i = 1; i < args.size(); i++)
        {
            size_t eq = args[i].find('=');
            if (eq == string::npos || eq == 0)
                continue;
            string v = args[i].substr(eq + 1);
            if (v.size() >= 2 && ((v.front() == '\'' && v.back() == '\'') ||
                                  (v.front() == '"' && v.back() == '"')))
                v = v.substr(1, v.size() - 2);
            m_aliases[args[i].substr(0, eq)] = v;
        }
        return r;
    }

    if (name == "unalias")
    {
        for (size_t i = 1; i < args.size(); i++)
        {
            if (args[i] == "-a")
                m_aliases.clear();
            else
                m_aliases.erase(args[i]);
        }
        return r;
    }

    if (name == "dirs" || name == "pushd" || name == "popd")
    {
        auto dump = [&]()
        {
            r.out = m_cwd;
            for (auto it = m_dirStack.rbegin(); it != m_dirStack.rend(); ++it)
                r.out += " " + *it;
            r.out += "\n";
        };

        if (name == "pushd")
        {
            string target = args.size() >= 2 ? args[1] : envGet("HOME");
            string abs = absolutePath(target);
            if (!JZFileInfo(abs).isDir())
            {
                r.status = 1;
                r.err = "pushd: 不是目录: " + target;
                return r;
            }
            m_dirStack.push_back(m_cwd);
            m_cwd = abs;
            dump();
            return r;
        }
        if (name == "popd")
        {
            if (m_dirStack.empty())
            {
                r.status = 1;
                r.err = "popd: 目录栈是空的";
                return r;
            }
            m_cwd = m_dirStack.back();
            m_dirStack.pop_back();
            dump();
            return r;
        }
        dump();
        return r;
    }

    // history/hash/umask/wait/jobs/times/ulimit: 接受不实现(不再报"找不到命令")
    if (name == "history" || name == "hash" || name == "umask" || name == "wait" ||
        name == "jobs" || name == "times" || name == "ulimit" || name == "disown")
        return r;

    // kill: 只接受信号名/无参的形式(真杀进程需要 pid 归属, 这里不冒进)
    if (name == "kill")
    {
        for (size_t i = 1; i < args.size(); i++)
        {
            if (!args[i].empty() && args[i][0] == '-')
                continue;
            r.status = 1;
            r.err = "kill: 不支持按 pid 杀进程(停自己的命令请用 terminal_cancel)";
            return r;
        }
        return r;
    }

    // type / which: 函数 → "X is a function"; 别名 → "X is aliased to `...'";
    // 内建 → "X is a shell builtin"; 否则打印解析出的路径; 都没有才 not found。
    if (name == "type" || name == "which")
    {
        // 开关: -t 只输出一个类别词(alias/keyword/function/builtin/file), -p 只输出路径,
        // -a 把全部匹配都列出来; -P(强制走 PATH)、-f(不查函数) 忽略。
        bool terse = false, pathOnly = false, listAll = false;
        size_t ti = 1;
        for (; ti < args.size(); ti++)
        {
            const string &a = args[ti];
            if (a == "--")
            {
                ti++;
                break;
            }
            if (a.size() < 2 || a[0] != '-')
                break;
            for (size_t k = 1; k < a.size(); k++)
            {
                if (a[k] == 't')
                    terse = true;
                else if (a[k] == 'p')
                    pathOnly = true;
                else if (a[k] == 'a')
                    listAll = true;
            }
        }
        for (size_t i = ti; i < args.size(); i++)
        {
            const string &w = args[i];
            const bool isFunc = (name == "type") && m_functions.find(w) != m_functions.end();
            auto ait = (name == "type") ? m_aliases.find(w) : m_aliases.end();
            const bool isAlias = ait != m_aliases.end();
            // 内建要排在外部程序前面: 否则 `type echo` 会解析到 Git 的 echo.exe,
            // 与 bash 的 "echo is a shell builtin" 不一致(模型据此判断能不能用内建语义)
            const bool isBuiltin = isBuiltinName(w);
            const bool isKw = isReservedWord(w);

            if (terse)
            {
                if (isFunc)
                    r.out += "function\n";
                else if (isAlias)
                    r.out += "alias\n";
                else if (isBuiltin)
                    r.out += "builtin\n";
                else if (isKw)
                    r.out += "keyword\n";
                else if (!resolveProgram(w).empty())
                    r.out += "file\n";
                else
                    r.status = 1;    // bash: 找不到时既不输出也非 0
                continue;
            }
            if (pathOnly)
            {
                // bash 的 -p: 只有 PATH 里真能找到**文件**才输出路径, 函数/别名/内建/关键字
                // 一律不输出(`type -p echo` 在 bash 里是空的)
                const string p = (isFunc || isAlias || isBuiltin || isKw) ? string()
                                                                        : resolveProgram(w);
                if (p.empty())
                    r.status = 1;
                else
                    r.out += p + "\n";
                continue;
            }

            bool any = false;
            if (isFunc)
            {
                r.out += w + " is a function\n";
                any = true;
                if (!listAll)
                    continue;
            }
            if (isAlias)
            {
                r.out += w + " is aliased to `" + ait->second + "'\n";
                any = true;
                if (!listAll)
                    continue;
            }
            if (isBuiltin)
            {
                r.out += w + " is a shell builtin\n";
                any = true;
                if (!listAll)
                    continue;
            }
            if (isKw)
            {
                r.out += w + " is a shell keyword\n";
                any = true;
                if (!listAll)
                    continue;
            }
            const string p = resolveProgram(w);
            if (!p.empty())
            {
                r.out += w + " is " + p + "\n";
                any = true;
            }
            if (!any)
            {
                r.status = 1;
                r.out += w + ": not found\n";
            }
        }
        return r;
    }

    // command: `-v`/`-V` 查解析结果(内建/函数/别名 → 名字, 外部 → 路径); 其余开关忽略,
    // 把剩下的当命令跑(不替换 shell)
    if (name == "command")
    {
        bool verbose = false;
        size_t i = 1;
        while (i < args.size() && !args[i].empty() && args[i][0] == '-')
        {
            if (args[i] == "-v" || args[i] == "-V")
                verbose = true;
            i++;
        }
        if (i >= args.size())
            return r;
        if (verbose)
        {
            // 只要有一个找不到就非 0(bash 的 command -v 语义); 找不到的那个不输出
            for (; i < args.size(); i++)
            {
                const string &w = args[i];
                if (isBuiltinName(w) || m_functions.find(w) != m_functions.end() ||
                    m_aliases.find(w) != m_aliases.end())
                {
                    r.out += w + "\n";
                    continue;
                }
                const string p = resolveProgram(w);
                if (p.empty())
                    r.status = 1;
                else
                    r.out += p + "\n";
            }
            return r;
        }
        vector<string> argv2(args.begin() + i, args.end());
        bool handled2 = false;
        if (m_functions.find(argv2[0]) != m_functions.end())
            return runFunction(argv2[0], argv2);
        r = runBuiltin(argv2[0], argv2, stdinText, &handled2);
        if (handled2)
            return r;
        string program = resolveSpawnProgram(argv2[0]);
        if (program.empty())
        {
            r.status = 127;
            r.err = errorPrefix(m_lineno, false) + argv2[0] + ": command not found";
            return r;
        }
        r = runExternal(program, argv2, stdinText, false);
        return r;
    }

    // shift: 位置参数前移(⚠ m_params[0] 是 $0, 只能从下标 1 开始丢)
    if (name == "shift")
    {
        int n = (args.size() >= 2) ? atoi(args[1].c_str()) : 1;
        if (n < 0)
            n = 0;
        const int avail = (int)m_params.size() - 1;   // 不含 $0
        if (n > avail)
        {
            // bash: 超出范围时返回 1, 参数**不动**
            r.status = 1;
            r.err = errorPrefix(m_lineno, false) + "shift: " + to_string(n) + ": shift count out of range";
            return r;
        }
        while (n-- > 0 && m_params.size() > 1)
            m_params.erase(m_params.begin() + 1);
        return r;
    }

    // eval: 拼成一行重新解析执行(状态保留, 与 bash 一致)
    if (name == "eval")
    {
        string text;
        for (size_t i = 1; i < args.size(); i++)
        {
            if (!text.empty())
                text += ' ';
            text += args[i];
        }
        if (text.empty())
            return r;
        string perr;
        vector<Token> tk = lex(text, perr);
        NodePtr node;
        bool ok = perr.empty();
        if (ok)
        {
            Parser pp(tk);
            ok = pp.parse(node, perr);
        }
        if (!ok)
        {
            r.failed = true;
            r.status = 2;
            r.failReason = perr.empty() ? string("eval 解析失败") : perr;
            return r;
        }
        return runNode(node);
    }

    // let: 逐个求值算术表达式(支持赋值/自增), 状态 = 最后一个表达式的值非 0 ? 0 : 1
    if (name == "let")
    {
        if (args.size() < 2)
        {
            r.status = 1;
            return r;
        }
        int st = 0;
        for (size_t i = 1; i < args.size(); i++)
        {
            string err;
            long long one = 0;
            if (!arithRun(args[i], one, &err))
            {
                r.status = 1;
                r.err = errorPrefix(m_lineno, false) + "let: " +
                        (err.empty() ? ("语法错误: " + args[i]) : err);
                return r;
            }
            st = (one != 0) ? 0 : 1;
        }
        r.status = st;
        return r;
    }

    // mapfile / readarray: 把输入按行读进数组(`mapfile -t arr < f`)
    //   -t 去掉每行结尾换行(bash 里不加 -t 会连着换行一起存)
    if (name == "mapfile" || name == "readarray")
    {
        bool stripNl = true;   // 我们只实现 -t 的语义, 默认按 -t 处理更符合预期
        string arrName = "MAPFILE";
        for (size_t i = 1; i < args.size(); i++)
        {
            if (args[i] == "-t")
            {
                stripNl = true;
                continue;
            }
            if (!args[i].empty() && args[i][0] == '-')
                continue;
            arrName = args[i];
        }
        string data = stdinText;
        if (data.empty())
        {
            data = m_inBuf;
            m_inBuf.clear();
        }
        if (data.empty() && !m_stageIn.empty())
        {
            data = m_stageIn;
            m_stageIn.clear();
        }
        vector<string> lines;
        size_t pos = 0;
        while (pos < data.size())
        {
            size_t nl = data.find('\n', pos);
            if (nl == string::npos)
            {
                lines.push_back(data.substr(pos));
                break;
            }
            string one = data.substr(pos, nl - pos);
            if (!stripNl || !one.empty())
                lines.push_back(one);
            pos = nl + 1;
        }
        m_arrays[arrName] = lines;
        m_env.erase(arrName);
        (void)stripNl;
        r.status = 0;
        return r;
    }

    // readonly: 标记变量只读(可带初值/数组), 之后赋值报 `name: readonly variable`
    if (name == "readonly")
    {
        bool intAttr = false;
        for (size_t i = 1; i < args.size(); i++)
        {
            const string &a = args[i];
            if (a == "-a" || a == "-A" || a == "-f" || a == "-p")
                continue;
            if (a == "-i")
            {
                intAttr = true;
                continue;
            }
            if (a.empty() || a[0] == '-')
                continue;   // 其它选项先忽略

            const size_t eq = a.find('=');
            string nm = (eq == string::npos) ? a : a.substr(0, eq);
            string val = (eq == string::npos) ? string() : a.substr(eq + 1);

            // `readonly a=(1 2)`: 数组形式 —— 括号里的元素按空白切
            if (!val.empty() && val.front() == '(' && val.back() == ')')
            {
                vector<string> elems;
                string cur;
                for (size_t k = 1; k + 1 < val.size(); k++)
                {
                    if (val[k] == ' ' || val[k] == '\t')
                    {
                        if (!cur.empty())
                        {
                            elems.push_back(cur);
                            cur.clear();
                        }
                        continue;
                    }
                    cur += val[k];
                }
                if (!cur.empty())
                    elems.push_back(cur);
                m_arrays[nm] = elems;
                m_env.erase(nm);
            }
            else if (eq != string::npos)
            {
                if (intAttr)
                    m_intVars.insert(nm);
                storeVar(nm, val);
            }
            m_readonly.insert(nm);
        }
        r.status = 0;
        return r;
    }

    // read: 从标准输入取一行(整行给第一个变量, 其余置空; 简化版, 不做 IFS 逐字段)
    if (name == "read")
    {
        // 选项走共用解析: `read -ra arr`(紧凑)也认; -r/-p 只吃掉, 不影响语义
        std::map<string, std::pair<bool, string>> opts;
        const size_t firstVar = parseBuiltinOptions(args, 1, "ap", opts);
        string arrayName;
        if (opts.count("a") > 0)
            arrayName = opts["a"].second;
        vector<string> vars(args.begin() + firstVar, args.end());
        // 管道/heredoc 的数据可能有多行: 一次只吃一行, 剩下的留在 m_inBuf 里,
        // 这样 `printf "1\n2\n3\n" | while read x; do ...; done` 能跑三轮。
        if (m_inBuf.empty())
        {
            if (!stdinText.empty())
                m_inBuf = stdinText;
            else if (!m_stageIn.empty())
            {
                m_inBuf = m_stageIn;
                m_stageIn.clear();
            }
        }

        string line;
        if (!m_inBuf.empty())
        {
            size_t nl = m_inBuf.find('\n');
            if (nl == string::npos)
            {
                line = m_inBuf;
                m_inBuf.clear();
            }
            else
            {
                line = m_inBuf.substr(0, nl);
                m_inBuf.erase(0, nl + 1);
            }
        }
        else
        {
            // 管道/heredoc 没有数据时, 从**进程 stdin** 读一行。
            // ⚠ 逐字节读、不用 iostream: 缓冲区预读会把后面几 KB 吞掉, 而脚本里
            //   后续的命令(或子进程)还要接着读那些数据。
            bool got = false;
            while (true)
            {
                char c = 0;
#ifdef _WIN32
                int n = _read(0, &c, 1);
#else
                int n = (int)::read(0, &c, 1);
#endif
                if (n <= 0)
                    break;
                got = true;
                if (c == '\n')
                    break;
                line += c;
            }
            if (!got)
            {
                r.status = 1;   // 真的 EOF
                return r;
            }
        }
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        // 按 IFS 切字段(bash 的规则): 前 n-1 个变量各拿一个字段, 最后一个拿剩下的
        // ⚠ IFS **没设过**时默认是 " \t\n"; 显式设成空(`IFS= read -r line`)才不切分。
        //   以前一律取 envVar("IFS")(没设过就是空), 于是 `read k v` 永远不切,
        //   整行都进了 k —— agent 读配置文件时最常踩的一条。
        const string ifs = (m_env.find("IFS") != m_env.end()) ? m_env["IFS"] : string(" \t\n");
        string seps = ifs.empty() ? string(" \t") : ifs;
        string body = line;
        if (ifs.empty())
        {
            // IFS= 空: 不切、也不去掉首尾空白(常见写法 `IFS= read -r line`)
        }
        else
        {
            size_t b = body.find_first_not_of(seps);
            if (b == string::npos)
                body.clear();
            else
                body = body.substr(b);
            size_t e = body.find_last_not_of(seps);
            if (e != string::npos)
                body = body.substr(0, e + 1);
        }

        // `read -a arr`: 整行切成字段全放进数组(IFS 为空时整体一个元素)
        if (!arrayName.empty())
        {
            vector<string> parts;
            if (ifs.empty())
            {
                if (!body.empty())
                    parts.push_back(body);
            }
            else
            {
                size_t pos2 = 0;
                while (pos2 < body.size())
                {
                    const size_t b2 = body.find_first_not_of(seps, pos2);
                    if (b2 == string::npos)
                        break;
                    const size_t e2 = body.find_first_of(seps, b2);
                    parts.push_back(body.substr(b2, e2 == string::npos ? string::npos : e2 - b2));
                    if (e2 == string::npos)
                        break;
                    pos2 = e2 + 1;
                }
            }
            m_arrays[arrayName] = parts;
            m_env.erase(arrayName);
            return r;
        }

        if (vars.empty())
            storeVar("REPLY", body);
        else if (vars.size() == 1 || ifs.empty())
        {
            storeVar(vars[0], body);
            for (size_t i = 1; i < vars.size(); i++)
                storeVar(vars[i], string());
        }
        else
        {
            size_t pos = 0;
            for (size_t i = 0; i < vars.size(); i++)
            {
                const bool last = (i + 1 == vars.size());
                if (last)
                {
                    // 最后一个变量拿剩下的整段, 但要把字段间的**空白**分隔符吃掉
                    // (`read x y` 对 "a b c" ⇒ x=a, y=b c, 不是 " b c")
                    size_t b = pos;
                    while (b < body.size() && (body[b] == ' ' || body[b] == '\t' || body[b] == '\n'))
                        b++;
                    storeVar(vars[i], b < body.size() ? body.substr(b) : string());
                    break;
                }
                size_t b = body.find_first_not_of(seps, pos);
                if (b == string::npos)
                {
                    storeVar(vars[i], string());
                    pos = body.size();
                    continue;
                }
                size_t e = body.find_first_of(seps, b);
                if (e == string::npos)
                {
                    storeVar(vars[i], body.substr(b));
                    pos = body.size();
                }
                else
                {
                    storeVar(vars[i], body.substr(b, e - b));
                    pos = e + 1;   // 吃掉这个分隔符: `IFS=: read a b <<< "x:y"` ⇒ b=y(不是 :y)
                }
            }
        }
        return r;
    }

    // source / . : 读文件后在本 shell 里执行(状态保留)
    if (name == "source" || name == ".")
    {
        if (args.size() < 2)
        {
            r.status = 1;
            r.err = "source: 缺少文件名";
            return r;
        }
        string path = absolutePath(args[1]);
        string text, ferr;
        if (!readWholeFile(path, text, ferr))
        {
            r.status = 1;
            r.err = "source: " + ferr;
            return r;
        }
        Result sr = exec(text);
        r.status = sr.exitCode;
        r.out = sr.output;
        if (!sr.error.empty())
            r.err = sr.error;
        return r;
    }

    // getopts: 先按"没有更多选项"收尾(避免死循环), 真语义等需要时再补
    if (name == "getopts")
    {
        r.status = 1;
        return r;
    }

    if (name == "export")
    {
        std::map<string, std::pair<bool, string>> opts;
        const size_t first = parseBuiltinOptions(args, 1, "", opts);
        const bool unexport = opts.count("n") > 0;
        for (size_t i = first; i < args.size(); i++)
        {
            const string &a = args[i];
            const size_t eq = a.find('=');
            const string nm = (eq == string::npos) ? a : a.substr(0, eq);
            if (nm.empty())
                continue;
            if (eq != string::npos)
                m_env[nm] = a.substr(eq + 1);
            // 记下来: 只有**标记过**的变量才进子进程环境(`export NAME` 也算标记)
            if (unexport)
                m_exported.erase(nm);
            else
                m_exported.insert(nm);
        }
        return r;
    }

    if (name == "unset")
    {
        for (size_t i = 1; i < args.size(); i++)
            m_env.erase(args[i]);
        return r;
    }

    // break / continue [n]: 记层数, 由循环体跑完后统一处理(见 runNode 的循环分支)
    if (name == "break" || name == "continue")
    {
        int level = 1;
        if (args.size() >= 2)
            level = atoi(args[1].c_str());
        if (level <= 0)
        {
            r.status = 1;
            r.err = name + ": " + (args.size() >= 2 ? args[1] : string()) + ": loop count out of range";
            return r;
        }
        if (name == "break")
            m_breakLevel = level;
        else
            m_continueLevel = level;
        return r;
    }

    if (name == "return")
    {
        m_returning = true;
        m_returnCode = args.size() >= 2 ? atoi(args[1].c_str()) : m_lastStatus;
        r.status = m_returnCode;
        return r;
    }

    if (name == "local")
    {
        for (size_t i = 1; i < args.size(); i++)
        {
            const string &a = args[i];
            size_t eq = a.find('=');
            string var = (eq == string::npos) ? a : a.substr(0, eq);
            string val = (eq == string::npos) ? string() : a.substr(eq + 1);
            if (var.empty())
                continue;
            LocalSave ls;
            ls.name = var;
            auto it = m_env.find(var);
            if (it != m_env.end())
            {
                ls.had = true;
                ls.value = it->second;
            }
            m_locals.push_back(ls);
            m_env[var] = val;
        }
        return r;
    }

    if (name == "echo")
    {
        // 选项可以合并: -n / -e / -E / -ne / -en ...(都是 bash 认的写法)
        bool noNewline = false;
        bool escapes = false;
        size_t from = 1;
        while (from < args.size() && args[from].size() > 1 && args[from][0] == '-' &&
               args[from].find_first_not_of("neE", 1) == string::npos)
        {
            for (size_t k = 1; k < args[from].size(); k++)
            {
                if (args[from][k] == 'n')
                    noNewline = true;
                else if (args[from][k] == 'e')
                    escapes = true;
                else if (args[from][k] == 'E')
                    escapes = false;
            }
            from++;
        }
        r.out = joinArgs(args, from);
        if (escapes)
            r.out = echoUnescape(r.out);
        if (!noNewline)
            r.out += "\n";
        return r;
    }

    if (name == "printf")
    {
        // `printf -v var 格式 [参数...]`: 结果写进变量而不是 stdout
        size_t base = 1;
        string varName;
        if (args.size() > 2 && args[1] == "-v")
        {
            varName = args[2];
            base = 3;
        }
        if (args.size() <= base)
        {
            r.status = 1;
            r.err = "printf: usage: printf [-v var] format [arguments]";
            return r;
        }

        const string fmt = args[base];
        size_t argi = base + 1;
        size_t before = argi;
        string out;
        do
        {
            // ⚠ 格式串里没有任何 % 时 argi 不会前进 —— 必须记下"这一轮有没有消费参数",
            //   否则 `printf -v vv '%b\n' x` 会无限循环(以前就卡在这)
            before = argi;
            for (size_t i = 0; i < fmt.size(); i++)
            {
                if (fmt[i] == '\\' && i + 1 < fmt.size())
                {
                    // 格式串里的反斜杠按 bash 的规则展开: 单字符转义 + `\NNN`(八进制) +
                    // `\xHH`。未知转义原样保留(与 bash 一致)。
                    const char n = fmt[i + 1];
                    if (n >= '0' && n <= '7')
                    {
                        int v = n - '0', k = 1;
                        size_t p = i + 2;
                        while (k < 3 && p < fmt.size() && fmt[p] >= '0' && fmt[p] <= '7')
                        {
                            v = v * 8 + (fmt[p] - '0');
                            p++;
                            k++;
                        }
                        out += (char)v;
                        i = p - 1;
                        continue;
                    }
                    if (n == 'x')
                    {
                        int v = 0, k = 0;
                        size_t p = i + 2;
                        while (k < 2 && p < fmt.size() && isxdigit((unsigned char)fmt[p]))
                        {
                            const char h = fmt[p];
                            v = v * 16 + (h <= '9' ? (h - '0') : (tolower((unsigned char)h) - 'a' + 10));
                            p++;
                            k++;
                        }
                        if (k > 0)
                        {
                            out += (char)v;
                            i = p - 1;
                            continue;
                        }
                    }
                    switch (n)
                    {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'a': out += '\a'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'v': out += '\v'; break;
                    case 'e': out += '\x1b'; break;
                    case '\\': out += '\\'; break;
                    case '"': out += '"'; break;
                    case '\'': out += '\''; break;
                    default: out += '\\'; out += n; break;
                    }
                    i++;
                    continue;
                }
                if (fmt[i] != '%')
                {
                    out += fmt[i];
                    continue;
                }
                if (i + 1 < fmt.size() && fmt[i + 1] == '%')
                {
                    out += '%';
                    i++;
                    continue;
                }

                // %[flags][width][.prec]conv
                size_t j = i + 1;
                string flags;
                while (j < fmt.size() && strchr("-+ 0#'", fmt[j]) != nullptr)
                    flags += fmt[j++];
                string width;
                while (j < fmt.size() && fmt[j] >= '0' && fmt[j] <= '9')
                    width += fmt[j++];
                string prec;
                if (j < fmt.size() && fmt[j] == '.')
                {
                    prec = ".";
                    j++;
                    while (j < fmt.size() && fmt[j] >= '0' && fmt[j] <= '9')
                        prec += fmt[j++];
                }
                if (j >= fmt.size())
                {
                    out += '%';
                    break;
                }
                const char conv = fmt[j];
                const string v = (argi < args.size()) ? args[argi++] : string();
                string piece;
                bool bStop = false;   // %b 里的 `\c`: 到此为止(不再输出后面的内容)
                switch (conv)
                {
                case 's': piece = v; break;
                case 'b':                                       // %b: 参数里的转义也展开
                {
                    bool stop = false;
                    piece = printfUnescape(v, &stop);
                    if (stop)
                        bStop = true;
                    break;
                }
                case 'c': piece = v.empty() ? string() : string(1, v[0]); break;
                case 'd':
                case 'i': piece = to_string(atoll(v.c_str())); break;
                case 'u': piece = to_string((unsigned long long)atoll(v.c_str())); break;
                case 'o':
                {
                    char buf[40];
                    snprintf(buf, sizeof(buf), "%llo", (unsigned long long)atoll(v.c_str()));
                    piece = buf;
                    break;
                }
                case 'x':
                case 'X':
                {
                    char buf[40];
                    snprintf(buf, sizeof(buf), conv == 'x' ? "%llx" : "%llX",
                             (unsigned long long)atoll(v.c_str()));
                    piece = buf;
                    break;
                }
                case 'f':
                case 'e':
                case 'E':
                case 'g':
                case 'G':
                {
                    char buf[128];
                    const string pf = "%" + prec + string(1, conv);
                    snprintf(buf, sizeof(buf), pf.c_str(), atof(v.c_str()));
                    piece = buf;
                    break;
                }
                case 'q':
                {
                    // %q: 输出"还能被 shell 原样读回来"的形式 —— 特殊字符前加反斜杠
                    if (v.empty())
                    {
                        piece = "''";
                        break;
                    }
                    for (char qc : v)
                    {
                        if (!(isalnum((unsigned char)qc) || qc == '_' || qc == '-' ||
                              qc == '.' || qc == '/' || qc == ':'))
                            piece += '\\';
                        piece += qc;
                    }
                    break;
                }
                default:
                    piece = string("%") + conv;   // 不认识的转换: 原样保留
                    break;
                }

                // 宽度(只做左对齐/零填充两种, 够覆盖常见用法)
                const int w = width.empty() ? 0 : atoi(width.c_str());
                if (w > (int)piece.size())
                {
                    const bool left = flags.find('-') != string::npos;
                    const char padc = (flags.find('0') != string::npos && !left) ? '0' : ' ';
                    const string pad((size_t)(w - (int)piece.size()), padc);
                    piece = left ? piece + pad : pad + piece;
                }
                out += piece;
                if (bStop)
                {
                    // `printf '%b' 'a\cb'`: bash 输出 a 就停下(连结尾换行也不加)
                    if (!varName.empty())
                        storeVar(varName, out);
                    else
                        r.out = out;
                    return r;
                }
                i = j;
            }
        } while (argi < args.size() && argi > before);

        if (!varName.empty())
        {
            storeVar(varName, out);
            return r;
        }
        r.out = out;
        return r;
    }

    if (name == "exit")
    {
        m_exiting = true;
        m_exitCode = args.size() >= 2 ? atoi(args[1].c_str()) : m_lastStatus;
        r.status = m_exitCode;
        return r;
    }

    if (name == "true" || name == ":")
        return r;

    if (name == "false")
    {
        r.status = 1;
        return r;
    }

    if (name == "test" || name == "[")
    {
        vector<string> a(args.begin() + 1, args.end());
        if (name == "[" && !a.empty() && a.back() == "]")
            a.pop_back();

        auto fileTest = [](char op, const string &p) -> bool
        {
            JZFileInfo info(p);
            if (op == 'e') return info.exists();
            if (op == 'f') return info.isFile();
            if (op == 'd') return info.isDir();
            return false;
        };
        auto cmpNum = [](const string &op, long x, long y) -> bool
        {
            if (op == "-eq") return x == y;
            if (op == "-ne") return x != y;
            if (op == "-lt") return x < y;
            if (op == "-le") return x <= y;
            if (op == "-gt") return x > y;
            if (op == "-ge") return x >= y;
            return false;
        };

        bool negate = false;
        size_t i = 0;
        if (i < a.size() && a[i] == "!") { negate = true; i++; }

        bool result = false;
        if (i + 1 < a.size() && a[i].size() == 2 && a[i][0] == '-')
        {
            char op = a[i][1];
            if (op == 'e' || op == 'f' || op == 'd' || op == 's' || op == 'r' || op == 'w' || op == 'x')
            {
                if (op == 's')
                {
                    JZFileInfo info(a[i + 1]);
                    result = info.isFile() && info.size() > 0;
                }
                else if (op == 'r' || op == 'w' || op == 'x')
                {
                    result = JZFileInfo(a[i + 1]).exists();   // 权限细判交给 OS
                }
                else
                {
                    result = fileTest(op, a[i + 1]);
                }
                i += 2;
            }
            else if (op == 'z') { result = a[i + 1].empty(); i += 2; }
            else if (op == 'n') { result = !a[i + 1].empty(); i += 2; }
        }
        if (i == 0 && a.size() >= 3 && (a[1] == "=" || a[1] == "!=" ||
                                        a[1] == "-eq" || a[1] == "-ne" || a[1] == "-lt" ||
                                        a[1] == "-le" || a[1] == "-gt" || a[1] == "-ge"))
        {
            if (a[1] == "=" || a[1] == "!=")
                result = (a[0] == a[2]) == (a[1] == "=");
            else
                result = cmpNum(a[1], atol(a[0].c_str()), atol(a[2].c_str()));
            i = 3;
        }
        else if (i == 0 && !a.empty() && a.size() == 1)
        {
            result = !a[0].empty();
            i = 1;
        }

        if (i == 0 && !a.empty())
        {
            r.status = 2;
            r.err = "test: 不支持的表达式(当前只支持 -e/-f/-d/-z/-n/= /!= 和数值比较)";
            return r;
        }
        if (negate)
            result = !result;
        r.status = result ? 0 : 1;
        return r;
    }

    *handled = false;
    return r;
}

// 调用用户函数: 位置参数 + local 作用域; 返回后全部还原
JZShell::StageResult JZShell::runFunction(const string &name, const vector<string> &args)
{
    StageResult r;
    auto it = m_functions.find(name);
    if (it == m_functions.end())
    {
        r.failed = true;
        r.status = 127;
        r.failReason = name + ": command not found";
        return r;
    }

    vector<string> savedParams = m_params;
    bool savedReturning = m_returning;
    int savedReturnCode = m_returnCode;
    size_t localMark = m_locals.size();

    m_params.clear();
    m_params.push_back(savedParams.empty() ? name : savedParams[0]);
    for (size_t i = 1; i < args.size(); i++)
        m_params.push_back(args[i]);

    m_returning = false;
    m_funcStack.push_back(name);   // $FUNCNAME[0] = 当前函数
    syncFuncName();
    r = runNode(it->second);
    m_funcStack.pop_back();
    syncFuncName();

    if (m_returning)
    {
        r.failed = false;
        r.status = m_returnCode;
        m_returning = false;
    }

    while (m_locals.size() > localMark)
    {
        const LocalSave &ls = m_locals.back();
        if (ls.had)
            m_env[ls.name] = ls.value;
        else
            m_env.erase(ls.name);
        m_locals.pop_back();
    }

    m_params = savedParams;
    m_returning = savedReturning;
    m_returnCode = savedReturnCode;
    return r;
}

// 跑一个 shell 脚本文件: 状态(env/cwd/位置参数)按新实例隔离
JZShell::StageResult JZShell::runScriptFile(const string &path, const vector<string> &argv)
{
    StageResult r;
    string text, ferr;
    if (!readWholeFile(path, text, ferr))
    {
        r.failed = true;
        r.status = 126;
        r.failReason = ferr;
        return r;
    }

    if (text.size() >= 2 && text[0] == '#' && text[1] == '!')
    {
        size_t nl = text.find('\n');
        text = (nl == string::npos) ? string() : text.substr(nl + 1);
    }

    string savedCwd = m_cwd;
    map<string, string> savedEnv = m_env;
    vector<string> savedParams = m_params;

    m_params.clear();
    for (const string &a : argv)
        m_params.push_back(a);

    string perr;
    vector<Token> tokens = lex(text, perr);
    NodePtr root;
    bool ok = perr.empty();
    if (ok)
    {
        Parser p(tokens);
        ok = p.parse(root, perr);
    }
    if (!ok)
    {
        r.failed = true;
        r.status = 2;
        r.failReason = perr;
    }
    else
    {
        r = runNode(root);
    }

    m_cwd = savedCwd;
    m_env = savedEnv;
    m_params = savedParams;
    return r;
}

JZShell::StageResult JZShell::runSimple(const SimpleCmd &cmd, const string &stdinText)
{
    StageResult r;

    // 管道里上一段留在 m_stageIn 的数据(复合命令段用它把 stdout 传给内部第一条命令)。
    // 本工程没有真 fd, 只能"谁先读谁消费"。
    string pipedIn = stdinText;
    if (pipedIn.empty() && !m_stageIn.empty())
    {
        pipedIn = m_stageIn;
        m_stageIn.clear();
    }

    // 1) 展开 argv
    // $BASH_COMMAND = 本条命令的**原文**(未展开, 连引号/变量都保留) —— 必须在展开之前记下来。
    // 复合命令/函数内部会在自己的 runSimple 里覆盖它, 这正是 bash 的行为。
    {
        string text;
        for (const Word &w : cmd.argv)
        {
            if (!text.empty())
                text += ' ';
            text += w.raw;
        }
        if (!text.empty())
            m_currentCmd = text;
    }

    vector<string> argv;
    // `export NAME=~/x` / `local NAME=~/x`: 这些内建后面的赋值词也算"赋值上下文",
    // `~` 在 `=` 和 `:` 之后要展开; 而 `echo foo=bar:~` 是普通参数, 不能展开。
    bool declCmd = false;
    if (!cmd.argv.empty())
    {
        const string &p0 = cmd.argv[0].raw;
        declCmd = (p0 == "export" || p0 == "local" || p0 == "declare" ||
                   p0 == "typeset" || p0 == "readonly");
    }
    for (size_t wi = 0; wi < cmd.argv.size(); wi++)
    {
        const bool asgCtx = declCmd && wi > 0 && isAssignmentWord(cmd.argv[wi].raw);
        vector<string> one = expandWord(cmd.argv[wi], nullptr, true, true, asgCtx);
        argv.insert(argv.end(), one.begin(), one.end());
    }

    // 1.4) `set -u`: 展开期撞到未定义变量(存在 m_expandError 里)
    if (!m_expandError.empty())
    {
        const string msg = m_expandError;
        m_expandError.clear();
        r.status = 1;
        r.err = errorPrefix(m_lineno, false) + msg;
        // `set -u` 命中在 bash 里是**致命**的(非交互 shell 直接退出), 不是只报一行
        m_exiting = true;
        m_exitCode = 1;
        return r;
    }

    // 1.45) `set -x`: 执行前把命令打到 stderr(bash 的 `+ cmd args`)
    if (m_xtrace && !argv.empty())
    {
        string trace = "+ ";
        for (size_t k = 0; k < argv.size(); k++)
        {
            if (k)
                trace += ' ';
            trace += argv[k];
        }
        r.err += trace + "\n";
    }

    // 1.5) 别名展开(只展开一次, 不递归): `alias ll='ls -l'` 之后 `ll x` -> `ls -l x`
    if (!argv.empty())
    {
        auto ait = m_aliases.find(argv[0]);
        if (ait != m_aliases.end())
        {
            string aerr;
            vector<Token> at = lex(ait->second, aerr);
            vector<string> repl;
            for (const Token &tk : at)
            {
                if (tk.type != Token::Word)
                    continue;
                Word w;
                w.raw = tk.text;
                vector<string> one = expandWord(w, nullptr);
                repl.insert(repl.end(), one.begin(), one.end());
            }
            if (!repl.empty())
            {
                argv.erase(argv.begin());
                argv.insert(argv.begin(), repl.begin(), repl.end());
            }
        }
    }

    // 2) 展开 assigns
    vector<pair<string, string>> assigns;
    // 数组元素赋值(`x[4]=7` / `x[4]+=1`): 左边不是合法变量名, 单独收
    struct ElemAssign
    {
        string name;
        string index;
        bool append = false;
        string value;
    };
    vector<ElemAssign> elemAssigns;
    for (const Word &w : cmd.assigns)
    {
        vector<string> one = expandWord(w, nullptr, /*splitFields=*/false,
                                        /*globbing=*/true, /*tildeAssign=*/true);
        if (one.empty())
            continue;
        size_t eq = one[0].find('=');
        if (eq == string::npos)
            continue;
        string lhs = one[0].substr(0, eq);
        string rhs = one[0].substr(eq + 1);
        bool append = !lhs.empty() && lhs.back() == '+';
        if (append)
            lhs.pop_back();
        const size_t br = lhs.find('[');
        if (br != string::npos && br > 0 && lhs.back() == ']')
        {
            ElemAssign ea;
            ea.name = lhs.substr(0, br);
            ea.index = lhs.substr(br + 1, lhs.size() - br - 2);
            ea.append = append;
            ea.value = rhs;
            elemAssigns.push_back(ea);
            continue;
        }
        if (append)
            lhs += '+';   // 交回下面的标量路径处理(`foo+=two`)
        assigns.push_back(make_pair(lhs, rhs));
    }

    // 数组元素赋值落地: `x[i]=v` / `x[i]+=v`(下标可以是算术表达式)
    auto applyElemAssigns = [&]()
    {
        for (const ElemAssign &ea : elemAssigns)
        {
            vector<string> &arr = m_arrays[ea.name];
            long long iv = 0;
            const bool ok = arithEval(ea.index, iv, nullptr);
            size_t pos = (ok && iv >= 0) ? (size_t)iv : arr.size();
            while (arr.size() <= pos)
                arr.push_back(string());
            if (ea.append)
            {
                // `typeset -i x` 之后 `x[4]+=7` 是加法(5+7=12), 不是字符串拼接
                if (m_intVars.find(ea.name) != m_intVars.end())
                {
                    long long v = 0;
                    // 空元素当 0(`typeset -i x` 之后 `x[2]+=7` -> 7)
                    const string old = arr[pos].empty() ? string("0") : arr[pos];
                    if (!arithEval(old + "+(" + ea.value + ")", v, nullptr))
                        v = 0;
                    arr[pos] = to_string(v);
                }
                else
                    arr[pos] += ea.value;
            }
            else
                arr[pos] = ea.value;
            m_env.erase(ea.name);   // 同名标量让位给数组
        }
    };

    // 数组赋值落地: `a=(x y)` / `a+=(z)`
    auto applyArrayAssigns = [&]()
    {
        for (auto &na : cmd.arrayAssigns)
        {
            string name = na.first;
            bool appendMode = !name.empty() && name[0] == '+';
            if (appendMode)
                name = name.substr(1);

            vector<string> &arr = m_arrays[name];
            if (!appendMode)
                arr.clear();
            // 顺序元素的下标游标: 显式 `[N]=` 之后从 N+1 接着排(bash 的规则)
            size_t next = arr.size();
            for (const Word &w : na.second)
            {
                vector<string> one = expandWord(w, nullptr);
                for (const string &v : one)
                {
                    // `x=([0]=7+11)` / `x=([2]+=7)`: 带下标的元素
                    if (v.size() > 3 && v[0] == '[')
                    {
                        const size_t rb = v.find("]=");
                        const size_t ra = v.find("]+=");
                        const bool isApp = (ra != string::npos) && (rb == string::npos || ra < rb);
                        const size_t cut = isApp ? ra : rb;
                        if (cut != string::npos && cut > 1)
                        {
                            long long iv = 0;
                            if (arithEval(v.substr(1, cut - 1), iv, nullptr) && iv >= 0)
                            {
                                const size_t pos = (size_t)iv;
                                while (arr.size() <= pos)
                                    arr.push_back(string());
                                const string val = v.substr(isApp ? cut + 3 : cut + 2);
                                if (isApp)
                                {
                                    if (m_intVars.find(name) != m_intVars.end())
                                    {
                                        long long sum = 0;
                                        const string oldv = arr[pos].empty() ? string("0") : arr[pos];
                                        if (!arithEval(oldv + "+(" + val + ")", sum, nullptr))
                                            sum = 0;
                                        arr[pos] = to_string(sum);
                                    }
                                    else
                                        arr[pos] += val;
                                }
                                else
                                    arr[pos] = val;
                                next = pos + 1;
                                continue;
                            }
                        }
                    }
                    while (arr.size() <= next)
                        arr.push_back(string());
                    arr[next] = v;
                    next++;
                }
            }
            m_env.erase(name);   // 同名标量让位给数组
        }
    };

    // 3) 只赋值不执行: 持久写入环境(标量 + 数组)
    if (argv.empty())
    {
        for (auto &kv : assigns)
        {
            // 只读变量: bash 报一行错误、返回 1, 脚本继续往下跑
            {
                const string base = (!kv.first.empty() && kv.first.back() == '+')
                                        ? kv.first.substr(0, kv.first.size() - 1)
                                        : kv.first;
                if (m_readonly.find(base) != m_readonly.end())
                {
                    r.status = 1;
                    r.err = errorPrefix(m_lineno, false) + base + ": readonly variable";
                    return r;
                }
            }
            // `foo+=two` 的变量名末尾带 '+': 追加而不是覆盖
            if (!kv.first.empty() && kv.first.back() == '+')
            {
                string n = kv.first.substr(0, kv.first.size() - 1);
                if (m_intVars.find(n) != m_intVars.end())
                {
                    // `declare -i n` 之后 `n+=x` 是加法, 不是字符串拼接
                    long long v = 0;
                    if (!arithEval(varValue(n) + "+(" + kv.second + ")", v, nullptr))
                        v = 0;
                    storeVar(n, to_string(v));
                }
                else
                    storeVar(n, varValue(n) + kv.second);
            }
            else
                storeVar(kv.first, kv.second);   // 动态变量(SECONDS=10 之类)在这里分流
        }
        applyElemAssigns();
        applyArrayAssigns();
        // 纯赋值语句: `x=$(false)` 的退出码是那条命令替换的(0 才能让 set -e 不退出)
        r.status = m_lastCaptureStatus;
        m_lastCaptureStatus = 0;
        return r;
    }

    // `local a=(...)` / `declare -a a=(...)`: 数组部分先落地。
    // (local 的"作用域"目前本来就没做栈式隔离, 与 runBuiltin 里 local 的处理保持一致)
    if (!cmd.arrayAssigns.empty())
        applyArrayAssigns();

    // 4) 重定向: 输入先读进内存; 输出等命令跑完再落盘
    string stdinData = pipedIn;
    bool merged = false;
    string outFile, errFile, bothFile;
    bool outAppend = false, errAppend = false;
    bool outToErr = false;      // `>&2`: 标准输出改道到标准错误

    for (const Redirect &rd : cmd.redirects)
    {
        if (rd.op == "2>&1")
        {
            merged = true;
            continue;
        }
        if (rd.op == "<&-" || rd.op == ">&-")
            continue;   // 关闭 fd(`exec 0<&-`): 本工程没有真 fd, 当空操作
        // 只复制 fd 的形式(`>&1` / `>&2`), 后面没有文件名
        if (rd.op.size() == 3 && rd.op.compare(0, 2, ">&") == 0)
        {
            if (rd.op[2] == '2')
                outToErr = true;
            continue;
        }
        // heredoc: 正文在词法阶段就收好了(不再是文件路径, 也不能当路径展开)
        if (rd.op == "<<")
        {
            stdinData = rd.noExpand ? rd.target.raw : expandHeredocBody(rd.target.raw);
            continue;
        }
        // here-string: `cmd <<< "文本"` —— 文本 + 换行当成标准输入
        if (rd.op == "<<<")
        {
            vector<string> hv = expandWord(rd.target, nullptr);
            string text = hv.empty() ? string() : hv[0];
            // 空 here-string(`cmd <<<""`)也要给一行空行: bash 里 read 会把它读成空串,
            // 以前 text 为空就直接不给输入, `read x` 读到 EOF ⇒ x 保留了旧值。
            if (text.empty() || text.back() != '\n')
                text += '\n';
            stdinData = text;
            continue;
        }
        vector<string> target = expandWord(rd.target, nullptr);
        string path = target.empty() ? string() : absolutePath(target[0]);
        if (path.empty())
        {
            r.failed = true;
            r.status = 1;
            r.failReason = "重定向目标为空";
            return r;
        }
        if (rd.op == "<")
        {
            string err;
            if (!readWholeFile(path, stdinData, err))
            {
                r.status = 1;
                r.err = err;
                return r;
            }
        }
        else if (rd.op == ">") { outFile = path; outAppend = false; }
        else if (rd.op == ">>") { outFile = path; outAppend = true; }
        else if (rd.op == "2>") { errFile = path; errAppend = false; }
        else if (rd.op == "2>>") { errFile = path; errAppend = true; }
        else if (rd.op == "&>") { bothFile = path; outAppend = false; }
        else if (rd.op == ">&") { bothFile = path; outAppend = false; }   // `>&file`
        else
        {
            r.status = 1;
            r.err = "syntax error near unexpected token `" + rd.op + "'";
            return r;
        }
    }

    // 4.5) 输出文件先打开一次: 打不开就**不执行命令**(与 bash 一致: 重定向失败不跑),
    //      同时按 > / >> 的语义建好/截断文件。
    auto preopen = [](const string &path, bool append) -> bool
    {
        JZFile f;
        if (!f.open(path, append ? "ab" : "wb"))
            return false;
        f.close();
        return true;
    };
    if ((!bothFile.empty() && !preopen(bothFile, outAppend)) ||
        (!outFile.empty() && !preopen(outFile, outAppend)) ||
        (!errFile.empty() && !preopen(errFile, errAppend)))
    {
        r.status = 1;
        r.err = "重定向目标打不开: " +
                (bothFile.empty() ? (outFile.empty() ? errFile : outFile) : bothFile);
        return r;
    }

    // 5) 赋值前缀只对这条命令生效(实现上先写进环境表, 命令结束后恢复)
    map<string, string> saved;
    for (auto &kv : assigns)
    {
        // `a+=5 cmd`: 前缀的追加赋值(名字末尾的 '+' 是标记, 不是名字的一部分)
        if (!kv.first.empty() && kv.first.back() == '+')
        {
            const string n = kv.first.substr(0, kv.first.size() - 1);
            auto it = m_env.find(n);
            if (it != m_env.end())
                saved[n] = it->second;
            if (m_intVars.find(n) != m_intVars.end())
            {
                long long v = 0;
                if (!arithEval(varValue(n) + "+(" + kv.second + ")", v, nullptr))
                    v = 0;
                m_env[n] = to_string(v);
            }
            else
                m_env[n] = varValue(n) + kv.second;
            m_childEnvExtra[n] = m_env[n];   // 前缀赋值对这条命令的子进程可见
            continue;
        }
        auto it = m_env.find(kv.first);
        if (it != m_env.end())
            saved[kv.first] = it->second;
        m_env[kv.first] = kv.second;
        m_childEnvExtra[kv.first] = kv.second;
    }
    applyElemAssigns();   // `x[i]=v cmd` 这种前缀形式

    // 5.5) 直写: 只有"输出确实要落到我们自己的 fd 上"时才让子进程继承 fd。
    //      被 `$( )` / 管道 / 重定向捕获时必须自己收, 否则文本会跑掉。
    const bool inheritOutput = m_directOutput && m_captureLevel == 0 && !merged && !outToErr &&
                               outFile.empty() && errFile.empty() && bothFile.empty();

    // 6) 用户函数 → 内建 → 外部
    bool handled = false;
    if (m_functions.find(argv[0]) != m_functions.end())
    {
        // 函数在管道里(`... | f`)时, 上游数据要交进函数体, 由体内的命令去读:
        // 走复合命令段同一条路(m_stageIn), 否则函数里的 `cat` 永远读不到上游。
        string savedIn = m_stageIn;
        string savedBuf = m_inBuf;
        if (!stdinData.empty())
        {
            m_inBuf.clear();
            m_stageIn = stdinData;
        }
        r = runFunction(argv[0], argv);
        m_stageIn = savedIn;
        m_inBuf = savedBuf;
        handled = true;
    }
    if (!handled)
        r = runBuiltin(argv[0], argv, stdinData, &handled);
    if (!handled)
    {
        string program = resolveSpawnProgram(argv[0]);
        if (program.empty())
        {
            r.status = 127;
            r.err = errorPrefix(m_lineno, false) + argv[0] + ": command not found";
        }
        else if (looksLikeShellScript(program))
        {
            // 脚本用本解释器跑, 不再交回系统 bash(也不需要 PE 头)
            // 外层有重定向/合并时, 脚本内部的输出必须攒起来交给我们落盘, 不能直写
            const bool needCapture = !outFile.empty() || !errFile.empty() || !bothFile.empty() ||
                                     merged || outToErr;
            if (needCapture)
                m_captureLevel++;
            r = runScriptFile(program, argv);
            if (needCapture)
                m_captureLevel--;
        }
        else
        {
            r = runExternal(program, argv, stdinData, merged, inheritOutput);
        }
    }

    m_childEnvExtra.clear();   // 前缀赋值只对刚才那条命令有效

    // POSIX: 特殊内建前面的赋值是**持久**的(bash 在 posix 模式下如此):
    // `IFS=: export x` 之后 IFS 就是 ":"。非 posix 模式不做这条(bash 也一样)。
    bool specialBuiltin = false;
    if (m_posix && !argv.empty())
    {
        static const char *kSpecial[] = {
            ":", ".", "break", "continue", "eval", "exec", "exit", "export",
            "readonly", "return", "set", "shift", "times", "trap", "unset", nullptr
        };
        for (int si = 0; kSpecial[si] != nullptr; si++)
        {
            if (argv[0] == kSpecial[si])
            {
                specialBuiltin = true;
                break;
            }
        }
    }

    // 7) 恢复赋值前缀
    if (!specialBuiltin)
    {
        for (auto &kv : assigns)
        {
            auto it = saved.find(kv.first);
            if (it != saved.end())
                m_env[kv.first] = it->second;
            else
                m_env.erase(kv.first);
        }
    }

    // 7.5) `>&2`: 标准输出改道到标准错误(没开真 fd, 就在出结果时挪一次)
    if (outToErr)
    {
        r.err += r.out;
        r.out.clear();
    }

    // 8) 输出重定向
    if (!bothFile.empty())
    {
        string err;
        if (!writeWholeFile(bothFile, r.out + r.err, outAppend, err))
        {
            r.status = 1;
            r.err = err;
            return r;
        }
        r.out.clear();
        r.err.clear();
    }
    if (!outFile.empty())
    {
        string err;
        if (!writeWholeFile(outFile, r.out, outAppend, err))
        {
            r.status = 1;
            r.err = err;
            return r;
        }
        r.out.clear();
    }
    if (!errFile.empty())
    {
        string err;
        if (!writeWholeFile(errFile, r.err, errAppend, err))
        {
            r.status = 1;
            r.err = err;
            return r;
        }
        r.err.clear();
    }

    return r;
}

namespace
{
// 我们自己的内建: 同名时不能当外部命令(否则会跑到系统里同名程序上, 语义不一样)
bool isBuiltinName(const string &n)
{
    static const char *kB[] = {
        "cd", "pwd", "export", "unset", "echo", "printf", "exit", "true", "false",
        "test", "[", ":", "local", "return", "trap", "shopt", "set", "read",
        "source", ".", "eval", "exec", "alias", "unalias", "declare", "typeset",
        "readonly", "shift", "getopts", "pushd", "popd", "dirs", "history", "hash",
        "umask", "wait", "jobs", "times", "ulimit", "disown", "kill", "type",
        "command", "which", nullptr
    };
    for (int i = 0; kB[i] != nullptr; i++)
    {
        if (n == kB[i])
            return true;
    }
    return false;
}

// 语法保留字(`type -t if` → keyword)。不含 `time`/`coproc`: 它们当命令名时按普通词处理。
bool isReservedWord(const string &n)
{
    static const char *kK[] = {
        "if", "then", "else", "elif", "fi", "for", "while", "until", "do", "done",
        "case", "esac", "in", "function", "select", "{", "}", "!", "[[", "]]", nullptr
    };
    for (int i = 0; kK[i] != nullptr; i++)
    {
        if (n == kK[i])
            return true;
    }
    return false;
}
} // namespace

bool JZShell::tryRunExternalPipe(const std::vector<NodePtr> &stages, StageResult &r)
{
    // 整条命令行已经超时: 直接按超时收尾(不能退化成字符串管道, 那等于又跑一遍)
    if (remainingTimeoutMs() < 0)
    {
        r.failed = true;
        r.status = 124;
        r.failReason = "命令超时, 已被终止";
        return true;
    }

    struct Stage
    {
        string program;
        vector<string> args;   // 不含 argv[0]
    };

    // ---- 第一遍: 只做"能不能走真管道"的判断, 不做任何展开(展开会执行 $( ), 有副作用) ----
    vector<Stage> plan;
    for (const NodePtr &n : stages)
    {
        if (!n || n->kind != Node::Simple)
            return false;
        const SimpleCmd &c = n->cmd;
        if (!c.redirects.empty() || !c.assigns.empty() || !c.arrayAssigns.empty() || c.argv.empty())
            return false;

        // 程序名必须是字面量(带 $ / 引号 / ~ 时先不折腾)
        const string head = c.argv[0].raw;
        if (head.empty() || head.find('$') != string::npos || head.find('\'') != string::npos ||
            head.find('"') != string::npos || head.find('~') != string::npos ||
            head.find('*') != string::npos || head.find('?') != string::npos)
            return false;
        const string progName = unquoteWord(head);
        if (progName.empty() || isBuiltinName(progName))
            return false;
        if (m_functions.find(progName) != m_functions.end() ||
            m_aliases.find(progName) != m_aliases.end())
            return false;

        const string prog = resolveSpawnProgram(progName);
        // 脚本文件由本解释器跑(没有真 stdout), 只能走字符串管道
        if (prog.empty() || looksLikeShellScript(prog))
            return false;

        Stage st;
        st.program = prog;
        plan.push_back(st);
    }
    if (plan.size() < 2)
        return false;

    // ---- 第二遍: 现在确定要用了, 才展开各段参数 ----
    for (size_t i = 0; i < plan.size(); i++)
    {
        vector<string> argv;
        for (const Word &w : stages[i]->cmd.argv)
        {
            vector<string> one = expandWord(w, nullptr);
            argv.insert(argv.end(), one.begin(), one.end());
        }
        if (argv.empty())
            return false;
        plan[i].args.assign(argv.begin() + 1, argv.end());
        // 真管道直连也是"启动原生程序": 参数里的盘符挂载路径同样要翻(见 driveMountToWin)
        if (pathConvEnabled())
        {
            for (string &a : plan[i].args)
                a = convertBashPathArg(a);
        }
    }

    // ---- 起进程: 从下游往上游起(上游要拿到下游的 stdin 管道) ----
    const size_t n = plan.size();
    vector<unique_ptr<JZProcess>> procs(n);
    for (int i = (int)n - 1; i >= 0; i--)
    {
        procs[i].reset(new JZProcess());
        procs[i]->setWorkingDirectory(m_cwd);
        // 真管道这条捷径也得带上 export 过的环境, 否则 `export X=1; env | grep X` 是空的
        {
            std::map<std::string, std::string> childEnv = m_childEnvExtra;
            for (const std::string &nm : m_exported)
            {
                auto it = m_env.find(nm);
                if (it != m_env.end())
                    childEnv[nm] = it->second;
            }
            {
                auto pit = childEnv.find("PATH");
                const string pathValue = childPathValue(pit == childEnv.end() ? string() : pit->second);
                if (!pathValue.empty())
                    childEnv["PATH"] = pathValue;
            }
            if (!childEnv.empty())
                procs[i]->setEnvironmentVariables(childEnv);
        }
        if (i + 1 < (int)n)
            procs[i]->setStandardOutputProcess(procs[i + 1].get());
        if (!procs[i]->start(plan[i].program, plan[i].args))
        {
            const string why = procs[i]->lastStartError();
            for (size_t k = (size_t)i + 1; k < n; k++)
            {
                procs[k]->killProcess();
                procs[k]->waitForFinished(2000);
            }
            r.failed = true;
            r.status = 127;
            r.failReason = why.empty() ? ("无法启动: " + plan[i].program) : why;
            return true;   // 已经尽力了, 不再退回字符串管道(那样会重复执行)
        }
    }

    // 第一段的 stdin: 用上游留下来的输入 + 立刻关写端, 让它读到 EOF
    string stdinData = m_stageIn;
    m_stageIn.clear();
    if (!stdinData.empty())
        procs[0]->write(JZByteArray(stdinData.data(), (int)stdinData.size()));
    procs[0]->closeWriteChannel();

    // ---- 等全部结束: 每段都分片等(顺带把各自的管道读空), 支持超时/取消 ----
    // 同样等"整条命令行剩下的预算"
    const int leftMs = remainingTimeoutMs();
    const int timeout = (leftMs == 0) ? 0x7fffffff : leftMs;
    const int kSlice = 50;
    int waited = 0;
    while (true)
    {
        bool allDone = true;
        for (size_t i = 0; i < n; i++)
        {
            if (procs[i]->state() == JZProcess::NotRunning)
                continue;
            procs[i]->waitForFinished(kSlice);
            if (procs[i]->state() != JZProcess::NotRunning)
                allDone = false;
        }
        if (allDone)
            break;

        waited += kSlice;
        // 被杀前各段已产出的内容要带回去(见 runExternal 里 drainOutput 的说明)
        auto drainAll = [&]()
        {
            for (size_t i = 0; i < n; i++)
            {
                JZByteArray o = procs[i]->readAllStandardOutput();
                if (o.size() > 0)
                    r.out.append(o.data(), o.size());
                JZByteArray e = procs[i]->readAllStandardError();
                if (e.size() > 0)
                    r.out.append(e.data(), e.size());
            }
        };
        if (m_cancelRequested)
        {
            for (auto &p : procs)
                p->killProcess();
            drainAll();
            r.status = 130;
            r.err = "命令已被取消";
            return true;
        }
        if (waited >= timeout)
        {
            for (auto &p : procs)
                p->killProcess();
            drainAll();
            r.status = 124;
            r.err = "命令超时, 已被终止";
            return true;
        }
    }

    // ---- 收结果: 最后一段的 stdout + 所有段的 stderr(与字符串管道的行为一致) ----
    JZByteArray last = procs[n - 1]->readAllStandardOutput();
    r.out.assign(last.data(), last.size());
    string errAll;
    for (auto &p : procs)
    {
        JZByteArray e = p->readAllStandardError();
        if (e.size() > 0)
            errAll.append(e.data(), e.size());
    }
    if (!errAll.empty())
    {
        if (!r.out.empty() && r.out.back() != '\n')
            r.out += '\n';
        r.out += errAll;
        if (!r.out.empty() && r.out.back() != '\n')
            r.out += '\n';
    }
    // $PIPESTATUS + pipefail(与字符串管道一致)
    vector<string> ps;
    for (size_t i = 0; i < n; i++)
        ps.push_back(to_string(procs[i]->exitCode()));
    m_arrays["PIPESTATUS"] = ps;
    r.status = procs[n - 1]->exitCode();
    if (m_pipefail)
    {
        for (size_t i = 0; i < n; i++)
        {
            if (procs[i]->exitCode() != 0)
            {
                r.status = procs[i]->exitCode();
                break;
            }
        }
    }
    return true;
}

// ==========================================================================
// `cmd &` 后台
//
// 语义按 bash: 立即返回 0, 父 shell 不等它、不看它的退出码、不受 timeout 约束。
// 输出**由命令自己决定**(和前台一样): `cmd > log 2>&1 &` 写那个文件,
// `./server &` 没重定向 ⇒ 输出丢弃。因为父 shell 已经回包了, 那一路输出
// 没有任何接收者(终端那次运行已经结束), 丢掉就是唯一的结果 —— bash 是把它们
// 混进终端, 而这里没有"终端"可混。
// ==========================================================================
namespace
{
// 后台任务线程表。必须挂在**静态**上而不是 shell 实例上: 任务跑的时候终端可能已经
// 被销毁了。只会从 exec 线程 push(每条命令行都是串行执行的), 所以不需要加锁。
vector<JZTaskThread *> &bgThreads()
{
    static vector<JZTaskThread *> list;
    return list;
}
} // namespace

// 还有后台任务(`cmd &`)在跑吗? 进程退出前要问一句: 它们的线程还在跑,
// 直接走 JZApplication 析构会命中 `JZ_ASSERT(m_threads.empty())`(实测 jz_shell 崩在
// main 的 return 处: ~JZApplication → assert)。bash 的行为是"脚本结束就退出, 后台任务
// 继续跑", 所以那里直接 _exit, 让系统收尾。
bool JZShell::hasRunningBackgroundJobs()
{
    vector<JZTaskThread *> &ts = bgThreads();
    for (JZTaskThread *t : ts)
    {
        if (t != nullptr && t->isRunning())
            return true;
    }
    return false;
}

// 取出这条后台语句自己的输出重定向(Simple 的在 cmd.redirects, 复合命令的在 node 上)。
// 只有 `>` `>>` `&>` `>&文件` 这几种能直接交给分离进程; 出现别的(输入重定向 /
// heredoc / fd 复制)就返回 false, 让慢路径按完整语义跑。
bool JZShell::backgroundOutputs(const NodePtr &n, string *outFile, bool *outAppend, string *errFile)
{
    *outFile = string();
    *errFile = string();
    *outAppend = false;
    if (!n)
        return true;
    const vector<Redirect> &rds = (n->kind == Node::Simple) ? n->cmd.redirects : n->redirects;
    bool merged = false;
    for (const Redirect &rd : rds)
    {
        if (rd.op == "2>&1")
        {
            merged = true;
            continue;
        }
        if (rd.op != ">" && rd.op != ">>" && rd.op != "&>" && rd.op != ">&")
            return false;   // < / << / <<< / 2> / >&2 / 关闭 fd: 交给慢路径
        vector<string> t = expandWord(rd.target, nullptr);
        if (t.empty() || t[0].empty())
            return false;
        const string path = absolutePath(t[0]);
        if (rd.op == ">" || rd.op == ">>")
        {
            *outFile = path;
            *outAppend = (rd.op == ">>");
        }
        else
        {
            *outFile = path;
            *errFile = path;   // `&>f` / `>&f`: 两路合并
            *outAppend = false;
        }
    }
    if (merged && !outFile->empty())
        *errFile = *outFile;
    return true;
}

// 快路径: `./server &` / `python app.py > log 2>&1 &` 这类纯外部命令 →
// 直接起分离进程, 输出按命令自己的重定向接好(没重定向就是空设备), 拿到真实 pid。
bool JZShell::spawnDetachedSimple(const NodePtr &n, const string &outFile, const string &errFile,
                                  bool append, int *pid, string *notFound)
{
    if (!n || n->kind != Node::Simple)
        return false;
    const SimpleCmd &cmd = n->cmd;
    // 赋值前缀(`FOO=1 cmd &`)会改状态, 交给慢路径
    if (!cmd.assigns.empty() || !cmd.arrayAssigns.empty() || cmd.argv.empty())
        return false;

    vector<string> argv;
    for (const Word &w : cmd.argv)
    {
        vector<string> one = expandWord(w, nullptr, true, true, false);
        argv.insert(argv.end(), one.begin(), one.end());
    }
    if (argv.empty())
        return false;

    const string name = argv[0];
    if (m_aliases.find(name) != m_aliases.end())
        return false;
    if (m_functions.find(name) != m_functions.end())
        return false;
    if (isBuiltinName(name))
        return false;   // echo/cd/... 只能在本解释器里跑

    const string program = resolveSpawnProgram(name);
    if (program.empty())
    {
        // 命令根本不存在: 交给调用方在**父 shell** 里直接报出来。bash 也是这个信息
        // (它由后台子进程打到 stderr; 我们这边没人接, 只能父 shell 代打)。
        *notFound = name;
        return false;
    }
    if (looksLikeShellScript(program))
        return false;                       // 脚本要用本解释器跑, 不能直接 exec

    vector<string> args(argv.begin() + 1, argv.end());
    if (pathConvEnabled())                  // 与 runExternal 同一套路径转换
    {
        for (string &a : args)
            a = convertBashPathArg(a);
    }

    // 与前台命令同一套设置: 工作目录 / 环境(export + PATH) / 三路 IO。
    // 环境必须传, 否则 `export PORT=8080; ./server &` 里服务读不到 PORT。
    // IO 必须在 start() 之前定好: 输出接文件的进程才不会占管道, 之后 detach 才安全。
    JZProcess proc;
    proc.setWorkingDirectory(m_cwd);
    const JZProcess::FileOpenMode mode = append ? JZProcess::Append : JZProcess::Truncate;
    if (outFile.empty())
        proc.setStandardOutputNull();      // 没有重定向 = 丢弃
    else
        proc.setStandardOutputFile(outFile, mode);
    if (errFile.empty())
        proc.setStandardErrorNull();
    else
        proc.setStandardErrorFile(errFile, mode);
    proc.setStandardInputNull();           // 后台任务不读 stdin
    proc.setEnvironmentVariables(childEnv());
    if (!proc.start(program, args))
        return false;
    if (pid)
        *pid = proc.processId();
    return proc.detach();                  // 纯应用层放手: 不等、不读、不回收
}

shared_ptr<JZShell> JZShell::forkForBackground()
{
    auto c = std::make_shared<JZShell>();
    c->m_cwd = m_cwd;
    c->m_env = m_env;
    c->m_arrays = m_arrays;
    c->m_pathDirs = m_pathDirs;
    c->m_programPaths = m_programPaths;
    c->m_aliases = m_aliases;
    c->m_functions = m_functions;   // bash 的 fork: 函数、变量都带得过去
    c->m_exported = m_exported;
    c->m_intVars = m_intVars;
    c->m_readonly = m_readonly;
    c->m_params = m_params;
    c->m_argv0 = m_argv0;
    c->m_errMode = m_errMode;
    c->m_errScript = m_errScript;
    c->m_posix = m_posix;
    c->m_errexit = m_errexit;
    c->m_pipefail = m_pipefail;
    c->m_nounset = m_nounset;
    c->m_nullglob = m_nullglob;
    c->m_timeoutMs = 0;             // 后台任务不受 timeout 约束(0 = 不超时)
    c->m_bashPid = m_bashPid + 1;   // 子 shell 的 $BASHPID 变大($$ 不变)
    // 不开直写: 子 shell 的输出就是没人要的(父 shell 已经回包了), 攒在 StageResult 里
    // 丢掉即可; 语句自己的重定向(`{ ...; } > log &`)由 runNode 的常规路径落盘。
    return c;
}

JZShell::StageResult JZShell::spawnBackground(const NodePtr &n)
{
    StageResult r;
    ++m_bgCount;   // 只计数(不再打印 job 号: bash 脚本里 `cmd &` 是静默的, pid 用 $!)

    // 顺手回收已经跑完的线程对象(它们的任务已经结束, join 立即返回)
    {
        vector<JZTaskThread *> &ts = bgThreads();
        for (size_t i = 0; i < ts.size();)
        {
            if (!ts[i]->isRunning())
            {
                ts[i]->wait();
                delete ts[i];
                ts.erase(ts.begin() + static_cast<vector<JZTaskThread *>::difference_type>(i));
            }
            else
                ++i;
        }
    }

    // 输出方向由命令自己的重定向决定; 没有重定向 = 丢弃。
    // `>`(Truncate) / `>>`(Append) 由 setStandardOutputFile 的 mode 表达, 不用在这里先清文件。
    string outFile, errFile;
    bool outAppend = false;
    const bool simpleOut = backgroundOutputs(n, &outFile, &outAppend, &errFile);

    int pid = 0;
    string notFound;
    if (simpleOut && spawnDetachedSimple(n, outFile, errFile, outAppend, &pid, &notFound))
    {
        m_lastBgPid = pid;
        r.status = 0;
        // bash 在脚本里 `cmd &` 什么都不打印(pid 用 $! 取) —— 输出保持干净
        return r;
    }

    if (!notFound.empty())
    {
        // 命令根本不存在: 报错和 bash 一致(它由后台子进程打到 stderr, 我们这边没人接,
        // 只能父 shell 代打)。退出码仍是 0 —— bash 里 `cmd &` 的 $? 只看 fork 成不成,
        // `set -e` 也不会因为它退出。
        r.status = 0;
        r.err = errorPrefix(m_lineno, false) + notFound + ": command not found\n";
        return r;
    }

    // 慢路径: 内建 / 函数 / 管道 / 复合命令 —— 复制一份 shell 状态放后台线程跑
    // (bash 的 fork)。它自己的重定向照常生效, 其余输出没人要, 跑完丢掉。
    auto child = forkForBackground();
    // 交给子 shell 的节点必须清掉 background, 否则它自己又会去起一个后台任务(递归)
    auto body = std::make_shared<Node>(*n);
    body->background = false;

    // ⚠ 必须用 JZTaskThread, 不能用裸 std::thread: JZProcess 建定时器时会校验
    //   "当前线程是 JZApplication 登记的 JZThread", 裸线程直接 assert 崩掉(实测 FATAL)。
    //   这些线程对象放静态表里(不在 shell 实例上), 终端被销毁也不会悬空。
    auto *t = new JZTaskThread();
    t->setTask([child, body]()
    {
        StageResult rr = child->runNode(body);   // 退出码不回传给父 shell
        // 没被重定向掉的错误(语法错 / `cd` 失败 / 命令不存在…)按 bash 的做法打到 stderr:
        // 后台任务的 stdout 没人要, 但这些"起不来"的信息丢了就完全无从查起。
        if (!rr.err.empty())
        {
            string msg = rr.err;
            if (msg.back() != '\n')
                msg += '\n';
            fwrite(msg.data(), 1, msg.size(), stderr);
            fflush(stderr);
        }
    });
    bgThreads().push_back(t);   // 先登记再 start: 线程可能瞬间就跑完
    t->start();

    r.status = 0;
    return r;
}

JZShell::StageResult JZShell::runPipeline(const std::vector<NodePtr> &stages)
{
    StageResult r;

    // 先试"真管道": 两段以上、全部是外部命令时, 用 JZProcess 直接把上游 stdout 接到
    // 下游 stdin, 两个进程并发跑。`yes | head -1` / `cat 大文件 | head` 只有这样才不卡。
    if (stages.size() > 1 && tryRunExternalPipe(stages, r))
        return r;

    // 字符串管道: 每段的 stdout 要当字符串用(喂给下一段), 这段期间禁止直写/继承
    struct CaptureGuard
    {
        int &lvl;
        explicit CaptureGuard(int &l) : lvl(l) { ++lvl; }
        ~CaptureGuard() { --lvl; }
    } captureGuard(m_captureLevel);

    string stdinData;
    string errAll;
    vector<int> statuses;       // 每段各自的退出码 -> $PIPESTATUS
    for (size_t i = 0; i < stages.size(); i++)
    {
        const NodePtr &stage = stages[i];
        if (!stage)
            continue;

        // 管道里除最后一段外都在**子 shell** 里跑(bash 语义): 里面的 exit/cd/赋值不外泄。
        // 最后一段: 开了 shopt -s lastpipe 才留在当前 shell(`cmd | read v` 才拿得到 v)。
        const bool subshell = (i + 1 < stages.size()) || !m_lastpipe;
        string savedCwd = m_cwd;
        map<string, string> savedEnv = m_env;
        map<string, vector<string>> savedArrays = m_arrays;
        vector<string> savedParams = m_params;
        bool savedExiting = m_exiting;
        int savedExitCode = m_exitCode;

        StageResult s;
        if (stage->kind == Node::Simple)
        {
            s = runSimple(stage->cmd, stdinData);
        }
        else
        {
            // 复合命令段(`while ...; done | grep x`): 本工程没有真 fd, 上一段的 stdout
            // 交给它内部**第一条**要读输入的命令(m_stageIn), 读完即消耗。
            // ⚠ 外层如果有没读完的缓冲(m_inBuf), 必须先存起来并清空 —— 否则内层 read
            //   会去读外层的残留(`echo A B | while read a; do echo 1 2 | while read b; ...`
            //   里 b 会读到外层剩下的 "B", 内层的管道数据反而没人用)。
            string savedIn = m_stageIn;
            string savedBuf = m_inBuf;
            m_inBuf.clear();
            m_stageIn = stdinData;
            s = runNode(stage);
            m_stageIn = savedIn;
            m_inBuf = savedBuf;
        }
        if (subshell)
        {
            m_cwd = savedCwd;
            m_env = savedEnv;
            m_arrays = savedArrays;
            m_params = savedParams;
            m_exiting = savedExiting;
            m_exitCode = savedExitCode;
        }
        if (s.failed)
            return s;
        stdinData = s.out;      // 把上一段的 stdout 当下一段的 stdin
        errAll += s.err;        // stderr 不进下一段管道, 只并进最终输出
        statuses.push_back(s.status);
        r.status = s.status;
    }

    // $PIPESTATUS: 每段各自的退出码(数组, 下标 0 是第一段)
    vector<string> ps;
    for (int st : statuses)
        ps.push_back(to_string(st));
    m_arrays["PIPESTATUS"] = ps;
    // pipefail: 整个管道的退出码 = 最后一个非 0 的那段
    if (m_pipefail)
    {
        for (int st : statuses)
        {
            if (st != 0)
            {
                r.status = st;
                break;
            }
        }
    }

    r.out = stdinData;          // 最后一段的 stdout
    if (!errAll.empty())
    {
        if (!r.out.empty() && r.out.back() != '\n')
            r.out += '\n';
        r.out += errAll;
        if (r.out.back() != '\n')
            r.out += '\n';
    }
    return r;
}

// 复合命令的输入重定向(`while read x; do ...; done < file` / `<<< "text"`):
// 在整段跑之前把输入备好, 供内部的简单命令消费。
bool JZShell::prepareNodeInput(const NodePtr &n, StageResult &r)
{
    for (const Redirect &rd : n->redirects)
    {
        if (rd.op == "<<")
        {
            m_stageIn = rd.noExpand ? rd.target.raw : expandHeredocBody(rd.target.raw);
            continue;
        }
        if (rd.op == "<<<")
        {
            vector<string> hv = expandWord(rd.target, nullptr);
            string text = hv.empty() ? string() : hv[0];
            if (text.empty() || text.back() != '\n')
                text += '\n';
            m_stageIn = text;
            continue;
        }
        if (rd.op != "<")
            continue;

        vector<string> target = expandWord(rd.target, nullptr);
        string path = target.empty() ? string() : absolutePath(target[0]);
        if (path.empty())
        {
            r.failed = true;
            r.status = 1;
            r.failReason = "重定向目标为空";
            return false;
        }
        string err, data;
        if (!readWholeFile(path, data, err))
        {
            r.failed = true;
            r.status = 1;
            r.failReason = err;
            return false;
        }
        m_stageIn = data;
    }
    return true;
}

// 复合命令的输出重定向(`fi > f` / `done >> log` / `} 2>&1`): 跑完之后落盘。
void JZShell::flushNodeOutputs(const NodePtr &n, StageResult &r)
{
    bool merged = false;
    string outFile, errFile, bothFile;
    bool outAppend = false, errAppend = false;

    for (const Redirect &rd : n->redirects)
    {
        if (rd.op == "2>&1")
        {
            merged = true;
            continue;
        }
        if (rd.op == "<" || rd.op == "<<" || rd.op == "<<<")
            continue;   // 输入重定向已在 prepareNodeInput 里处理
        if (rd.op == "<&-" || rd.op == ">&-")
            continue;   // 关闭 fd: 空操作

        vector<string> target = expandWord(rd.target, nullptr);
        string path = target.empty() ? string() : absolutePath(target[0]);
        if (path.empty())
            continue;
        if (rd.op == ">") { outFile = path; outAppend = false; }
        else if (rd.op == ">>") { outFile = path; outAppend = true; }
        else if (rd.op == "2>") { errFile = path; errAppend = false; }
        else if (rd.op == "2>>") { errFile = path; errAppend = true; }
        else if (rd.op == "&>") { bothFile = path; outAppend = false; }
        else if (rd.op == ">&") { bothFile = path; outAppend = false; }
    }

    if (merged)
    {
        r.out += r.err;
        r.err.clear();
    }

    string err;
    if (!bothFile.empty())
    {
        writeWholeFile(bothFile, r.out + r.err, outAppend, err);
        r.out.clear();
        r.err.clear();
        return;
    }
    if (!outFile.empty())
    {
        if (!writeWholeFile(outFile, r.out, outAppend, err))
            r.err += err;
        r.out.clear();
    }
    if (!errFile.empty())
    {
        if (!writeWholeFile(errFile, r.err, errAppend, err))
            r.err += err;
        r.err.clear();
    }
}

// 整条命令行还剩多少毫秒(>0 剩余 / 0 不限 / <0 已超时)。判据是 exec() 记下的绝对截止时间。
int JZShell::remainingTimeoutMs() const
{
    if (m_deadlineMsec <= 0)
        return 0;
    const long long left = m_deadlineMsec - steadyMsec();
    if (left <= 0)
        return -1;
    return left > 0x7fffffff ? 0x7fffffff : (int)left;
}

JZShell::StageResult JZShell::runNode(const NodePtr &n)
{
    StageResult r;
    if (!n)
        return r;

    // `cmd &`: 起后台任务后立刻返回 0(bash 的行为: 父 shell 不等, 也不看它的退出码)。
    // 不在这里拦的话, `&` 就退化成"同步跑一遍", 常驻服务会一直卡到超时。
    if (n->background)
        return spawnBackground(n);

    if (++m_depth > 64)
    {
        m_depth--;
        r.failed = true;
        r.status = 1;
        r.failReason = "嵌套太深(可能是自引用脚本)";
        return r;
    }

    // 收到取消: 停掉**整条命令行**, 不只是当前那个外部命令。
    // 否则 `sleep 3; rm -rf x` 只是把 sleep 杀掉, 后半截照样跑完(实测踩过:
    // 后台任务 cancel 后 printf 还是执行了, 整体 exit=0 还被当成"正常结束")。
    if (m_cancelRequested.load())
    {
        m_depth--;
        r.failed = true;
        r.status = 130;
        r.failReason = "命令已被取消";
        return r;
    }

    // 超时同样停**整条命令行**: 外部命令等待处只是"等到点", 这里兜住"还在往后跑"的情况
    // (`a; b; c` 里 b 超时被杀之后, c 不该再起)。
    if (remainingTimeoutMs() < 0)
    {
        m_depth--;
        r.failed = true;
        r.status = 124;
        r.failReason = "命令超时, 已被终止";
        return r;
    }

    // 复合命令上的输入重定向必须**先**生效(`while read x; do ...; done < file`),
    // 输出重定向等跑完再落盘(见函数末尾的 flushNodeOutputs)。
    if (n->kind != Node::Simple && !n->redirects.empty())
    {
        if (!prepareNodeInput(n, r))
        {
            m_depth--;
            return r;
        }
    }

    switch (n->kind)
    {
    case Node::Simple:
    {
        m_lineno = n->line > 0 ? n->line : m_lineno;

        // DEBUG trap: 每条简单命令**执行前**跑一次动作(bash 的行为)。
        // extdebug 打开时, 动作返回 2 表示"跳过这条命令"。
        string trapOut, trapErr;
        int trapStatus = 0;
        StageResult trapResult;
        if (!m_trapDebug.empty() && !m_inTrap)
        {
            m_inTrap = true;
            trapResult = runDebugTrap(m_lineno);
            m_inTrap = false;
            trapStatus = trapResult.status;
            trapOut = trapResult.out;   // 动作自己的输出要留在结果里(排在命令输出之前)
            trapErr = trapResult.err;
        }
        if (trapResult.failed)
        {
            r = trapResult;
            break;
        }
        if (m_extdebug && trapStatus == 2)
        {
            r.status = 0;          // 跳过: 不执行这条命令
            r.out = trapOut;
            r.err = trapErr;
            break;
        }
        r = runSimple(n->cmd, string());
        if (!trapOut.empty())
            r.out = trapOut + r.out;
        if (!trapErr.empty())
            r.err = trapErr + r.err;
        break;
    }

    case Node::Pipeline:
        r = runPipeline(n->stages);
        break;

    case Node::Function:
        m_functions[n->var] = n->body;
        r.status = 0;
        break;

    case Node::Subshell:
    {
        // 子 shell 的状态一律不外泄(bash 的 fork 语义): cwd / env / 参数 / 退出标志,
        // 还有 set -e —— `( set -e; false )` 不能让父 shell 也变成 errexit。
        const bool savedErrexit = m_errexit;
        const bool savedInCond = m_inCond;
        string savedCwd = m_cwd;
        map<string, string> savedEnv = m_env;
        map<string, vector<string>> savedArrays = m_arrays;
        vector<string> savedParams = m_params;
        bool savedExiting = m_exiting;
        int savedBashPid = m_bashPid;
        m_bashPid++;                    // `( ... )` 也是子 shell: $BASHPID 要变
        r = runNode(n->body);
        m_bashPid = savedBashPid;
        m_errexit = savedErrexit;
        m_inCond = savedInCond;
        m_cwd = savedCwd;
        m_env = savedEnv;
        m_arrays = savedArrays;
        m_params = savedParams;
        m_exiting = savedExiting;
        // 子 shell 里 set -e 触发的"脚本中止"只结束它自己: 对父 shell 这就是一条
        // 非零退出的普通命令(父 shell 若自己也开了 -e, 由上层按普通失败判断)
        r.failed = false;
        break;
    }

    case Node::Group:
        // { ...; } —— 就在当前 shell 里跑, 状态(cd/变量/函数)保留
        r = runNode(n->body);
        break;

    case Node::AndOr:
    {
        int status = 0;
        for (size_t i = 0; i < n->parts.size(); i++)
        {
            bool run = true;
            if (i > 0)
            {
                const string &op = n->ops[i - 1];
                if (op == "&&")
                    run = (status == 0);
                else if (op == "||")
                    run = (status != 0);
            }
            if (!run)
            {
                // 短路 = 失败发生在 &&/|| 列表的**非末段**(`false && echo x`):
                // bash 的 set -e 对这段豁免, 整条列表的失败不能让脚本退出
                r.errexitExempt = true;
                continue;
            }
            StageResult s = runNode(n->parts[i]);
            if (s.failed)
            {
                r = s;
                break;
            }
            status = s.status;
            r.out += s.out;
            if (!s.err.empty())
            {
                if (!r.out.empty() && r.out.back() != '\n')
                    r.out += '\n';
                r.out += s.err;
                if (r.out.back() != '\n')
                    r.out += '\n';   // 错误信息后面补一个换行, 免得和下一条输出粘在一起
            }
            r.status = status;
            // `set -e`: 命令失败就中止整个脚本。豁免: && / || 链里**非末段**的命令
            // (`false && echo x` 不能退出, 见 AndOr 分支的 errexitExempt) 以及 if/while 的条件。
            // 不实现这条, agent 写的 `set -e` 等于没写。
            const bool chained = (i < n->ops.size() &&
                                  (n->ops[i] == "&&" || n->ops[i] == "||"));
            if (m_errexit && !m_inCond && !chained && status != 0 && !s.failed &&
                !s.errexitExempt)
            {
                m_exiting = true;
                m_exitCode = status;
                break;
            }
            flushDirect(r);   // 直写模式: 每条语句结束就落 fd, 别攒到最后
            // `return` / `exit` 必须**立刻**跳出当前语句列表: 否则
            // `f() { if ...; then return 2; fi; return 0; }` 里后一个 return 会把
            // 状态覆盖回 0(DEBUG trap 的 return 2 正是踩到这个)
            // `break` / `continue` 同理: 它们只置标志位, 若这里不跳出,
            // `for i in 1 2 3; do continue; echo $i; done` 里的 echo 照样会执行。
            if (m_exiting || m_returning || m_breakLevel > 0 || m_continueLevel > 0)
                break;
        }
        break;
    }

    case Node::If:
    {
        bool done = false;
        for (size_t i = 0; i < n->conds.size(); i++)
        {
            // 条件里的失败不算 `set -e` 的触发点(`if false; then` 是正常的)
            const bool savedCond = m_inCond;
            m_inCond = true;
            StageResult c = runNode(n->conds[i]);
            m_inCond = savedCond;
            if (c.failed) { r = c; done = true; break; }
            if (c.status == 0)
            {
                r = runNode(n->bodies[i]);
                done = true;
                break;
            }
        }
        if (!done && n->elseBody)
            r = runNode(n->elseBody);
        break;
    }

    case Node::For:
    {
        // 变量名必须是合法标识符(bash: `\`x-y': not a valid identifier`, 运行期报, 脚本继续)
        bool badName = n->var.empty();
        for (char vc : n->var)
        {
            if (!(isNameChar(vc)))
                badName = true;
        }
        if (!n->var.empty() && !isNameStart(n->var[0]))
            badName = true;
        if (badName)
        {
            r.status = 1;
            r.err = errorPrefix(m_lineno, false) + "`" + n->var + "': not a valid identifier";
            break;
        }

        // `select 名字 [in 词...]`: 正常应该从 stdin 读, 这里 stdin 通常立刻 EOF,
        // 所以只处理 `in` 里给出的词(没有就一次都不循环), 避免把测试卡住。
        vector<string> items;
        if (n->selectLoop)
        {
            for (const Word &w : n->words)
            {
                vector<string> one = expandWord(w, nullptr);
                items.insert(items.end(), one.begin(), one.end());
            }
        }
        else if (n->forNoIn)
        {
            // 没写 `in`: 遍历 "$@"(从 1 开始, m_params[0] 是 $0)
            for (size_t pi = 1; pi < m_params.size(); pi++)
                items.push_back(m_params[pi]);
        }
        else
        {
            for (const Word &w : n->words)
            {
                vector<string> one = expandWord(w, nullptr);
                items.insert(items.end(), one.begin(), one.end());
            }
        }
        for (const string &item : items)
        {
            m_env[n->var] = item;
            StageResult s = runNode(n->body);
            if (s.failed) { r = s; break; }
            r.status = s.status;
            r.out += s.out;
            if (!s.err.empty())
            {
                if (!r.out.empty() && r.out.back() != '\n')
                    r.out += '\n';
                r.out += s.err;
                if (r.out.back() != '\n')
                    r.out += '\n';   // 错误信息后面补一个换行, 免得和下一条输出粘在一起
            }
            if (m_exiting)
                break;
            // break/continue 在本层消耗一层; 还剩下的层数留给外层循环
            if (m_breakLevel > 0)
            {
                m_breakLevel--;
                break;
            }
            if (m_continueLevel > 0)
            {
                m_continueLevel--;
                continue;
            }
        }
        break;
    }

    case Node::While:
    {
        r.status = 0;   // 循环一次都没跑: bash 的退出码是 0(`set -e` 下不能因此退出)
        int guard = 0;
        while (true)
        {
            if (++guard > 100000)
            {
                r.failed = true;
                r.status = 1;
                r.failReason = "while 循环超过 100000 次, 已中止(防止把进程卡死)";
                break;
            }
            const bool savedCond = m_inCond;
            m_inCond = true;
            StageResult c = runNode(n->cond);
            m_inCond = savedCond;
            if (c.failed) { r = c; break; }
            // while: 条件非 0 就退出; until: 条件为 0 才退出
            if (n->until ? (c.status == 0) : (c.status != 0))
                break;
            StageResult s = runNode(n->body);
            if (s.failed) { r = s; break; }
            r.status = s.status;
            r.out += s.out;
            if (!s.err.empty())
            {
                if (!r.out.empty() && r.out.back() != '\n')
                    r.out += '\n';
                r.out += s.err;
                if (r.out.back() != '\n')
                    r.out += '\n';   // 错误信息后面补一个换行, 免得和下一条输出粘在一起
            }
            if (m_exiting)
                break;
            // break/continue 在本层消耗一层; continue 直接回到条件判断
            if (m_breakLevel > 0)
            {
                m_breakLevel--;
                break;
            }
            if (m_continueLevel > 0)
            {
                m_continueLevel--;
                continue;
            }
        }
        break;
    }

    case Node::Case:
    {
        // 主语先展开(取第一个词)
        vector<string> subjWords = expandWord(Word{ n->var }, nullptr);
        string subj = subjWords.empty() ? string() : subjWords[0];

        r.status = 0;

        // 分支终止符: `;;` 结束; `;&` 落到下一支(不再匹配); `;;&` 继续匹配后面的模式
        size_t bi = 0;
        bool runNext = false;
        while (bi < n->words.size() && bi < n->bodies.size())
        {
            bool hit = runNext;
            runNext = false;

            if (!hit)
            {
                // 模式里只展开变量/去引号, 不能做路径展开(否则 `*` 变成当前目录文件名)
                string pat = expandPattern(n->words[bi].raw);
                // 顶层 `|` 分隔候选; extglob 里的 `|`(`@(a|b)`)不算分隔
                size_t start = 0;
                while (start <= pat.size())
                {
                    int depth = 0;
                    size_t k = start;
                    for (; k < pat.size(); k++)
                    {
                        char c = pat[k];
                        if (c == '\\' && k + 1 < pat.size())
                        {
                            k++;
                            continue;
                        }
                        if (c == '(')
                            depth++;
                        else if (c == ')')
                        {
                            if (depth > 0)
                                depth--;
                        }
                        else if (c == '|' && depth == 0)
                            break;
                    }
                    string one = pat.substr(start, k - start);
                    if (globMatch(one.c_str(), subj.c_str()))
                    {
                        hit = true;
                        break;
                    }
                    if (k >= pat.size())
                        break;
                    start = k + 1;
                }
            }

            if (!hit)
            {
                bi++;
                continue;
            }

            StageResult s = runNode(n->bodies[bi]);
            if (s.failed)
            {
                r = s;
                break;
            }
            r.status = s.status;
            r.out += s.out;
            if (!s.err.empty())
            {
                if (!r.out.empty() && r.out.back() != '\n')
                    r.out += '\n';
                r.out += s.err;
                if (r.out.back() != '\n')
                    r.out += '\n';
            }

            const string term = (bi < n->ops.size()) ? n->ops[bi] : string(";;");
            if (term == ";&")
            {
                runNext = true;    // 下一支无条件执行
                bi++;
                continue;
            }
            if (term == ";;&")
            {
                bi++;              // 继续匹配后面的模式
                continue;
            }
            break;                 // ";;"
        }
        break;
    }

    case Node::Cond:
    {
        bool v = false;
        string cerr;
        if (!condEval(n, v, cerr))
        {
            r.failed = true;
            r.status = 2;
            r.failReason = cerr;
            break;
        }
        r.status = v ? 0 : 1;
        break;
    }

    case Node::Arith:
    {
        long long v = 0;
        string aerr;
        if (!arithRun(n->var, v, &aerr))
        {
            r.failed = true;
            r.status = 1;
            r.failReason = aerr;
            break;
        }
        r.status = (v != 0) ? 0 : 1;
        break;
    }

    case Node::ForArith:
    {
        vector<string> parts = splitChar(n->var, ';');
        string init = parts.size() > 0 ? parts[0] : string();
        string cond = parts.size() > 1 ? parts[1] : string();
        string step = parts.size() > 2 ? parts[2] : string();

        long long v = 0;
        string aerr;
        if (!init.empty() && !arithRun(init, v, &aerr))
        {
            r.failed = true;
            r.status = 1;
            r.failReason = aerr;
            break;
        }

        int guard = 0;
        while (true)
        {
            if (!cond.empty())
            {
                if (!arithRun(cond, v, &aerr))
                {
                    r.failed = true;
                    r.status = 1;
                    r.failReason = aerr;
                    break;
                }
                if (v == 0)
                    break;
            }
            if (++guard > 100000)
            {
                r.failed = true;
                r.status = 1;
                r.failReason = "for (( )) 循环超过 100000 次, 已中止(防止把进程卡死)";
                break;
            }

            StageResult s = runNode(n->body);
            if (s.failed)
            {
                r = s;
                break;
            }
            r.status = s.status;
            r.out += s.out;
            if (!s.err.empty())
            {
                if (!r.out.empty() && r.out.back() != '\n')
                    r.out += '\n';
                r.out += s.err;
                if (r.out.back() != '\n')
                    r.out += '\n';
            }
            if (m_exiting)
                break;

            // break/continue: `continue` 仍然要执行步进, 否则原地死循环
            bool doBreak = false;
            if (m_breakLevel > 0)
            {
                m_breakLevel--;
                doBreak = true;
            }
            else if (m_continueLevel > 0)
                m_continueLevel--;
            if (doBreak)
                break;

            if (!step.empty() && !arithRun(step, v, &aerr))
            {
                r.failed = true;
                r.status = 1;
                r.failReason = aerr;
                break;
            }
        }
        break;
    }
    }

    // 复合命令自己的输出重定向(`fi > f` / `done >> log` / `} 2>&1`)
    if (n->kind != Node::Simple && !n->redirects.empty())
        flushNodeOutputs(n, r);

    // `!` 前缀: 退出码取反(只影响 status, 不影响输出)
    // ⚠ Cond 节点自己处理 negate(condEval 里做, 因为 `[[ ! a && b ]]` 的 `!` 只作用于
    //   那个 term), 这里再取反一次就成了双重否定。
    if (n->negate && n->kind != Node::Cond)
        r.status = (r.status == 0) ? 1 : 0;

    // $? 必须反映"上一条真正执行过的命令": 展开下一条命令时马上就要用到,
    // 所以在这里(而不是只在 exec 收尾时)更新。
    if (!r.failed)
        m_lastStatus = r.status;

    m_depth--;
    return r;
}

// ==========================================================================
// 公共接口
// ==========================================================================
JZShell::JZShell(JZObject *parent)
    : JZObject(parent)
{
    m_cwd = envGet("PWD");
    if (m_cwd.empty())
    {
#ifdef _WIN32
        // 宽字符: cwd 里可能有 ANSI(GBK) 表示不了的字符(emoji 等), A 版会变 '?'
        m_cwd = JZDir::currentPath();
#else
        char buf[1024] = { 0 };
        if (getcwd(buf, sizeof(buf)))
            m_cwd = buf;
#endif
    }
    if (!m_cwd.empty())
        m_cwd = normalizePath(m_cwd);
    // 审核的相对路径基准 = 执行器自己的 cwd(terminal 传 base 时以 base 为准)
    m_policy.setBaseCwd(m_cwd);

    // 动态变量的起点: $BASHPID 初值 = 真 pid, $SECONDS 从 0 开始
    m_bashPid = currentPid();
    m_startMsec = steadyMsec();
    syncFuncName();   // 顶层: $FUNCNAME = ("main")
    // bash 兼容 shell 的惯例: 脚本靠 BASH_VERSION 判断能用哪些特性
    // (bash 测试套件里 `bashname=bash${BASH_VERSION%%.*}` 就是这么用的)
    if (m_env.find("BASH_VERSION") == m_env.end())
        m_env["BASH_VERSION"] = "5.2.0(1)-release";
    if (m_env.find("PWD") == m_env.end())
    {
        string pwd = m_cwd;
        for (char &pc : pwd)
        {
            if (pc == '\\')
                pc = '/';
        }
        m_env["PWD"] = pwd;   // `~+` 读它
    }
    // resetState() 的恢复点: 此刻的 m_env 就是"解释器刚建好"该有的样子
    m_baseEnv = m_env;
    m_baseExported = m_exported;
}

JZShell::~JZShell()
{
}

bool JZShell::setCwd(const string &dir, string *error)
{
    string abs = absolutePath(dir.empty() ? m_cwd : dir);
    if (!JZFileInfo(abs).isDir())
    {
        if (error)
            *error = "不是目录: " + abs;
        return false;
    }
    const string old = m_cwd;
    m_cwd = abs;
    m_policy.setBaseCwd(m_cwd);

    // ⚠ PWD / OLDPWD 必须跟着走。这个入口是**宿主**直接改 cwd 用的(终端 attach 时把
    //   工作区根交给解释器), 不经过 `cd` 内建 —— 以前只改 m_cwd, PWD 还停在解释器构造
    //   时的进程 cwd 上, 于是 `cd 子目录` 之后 `pwd` / `$PWD` 打出来的是"旧 base + 子目录"
    //   的假路径: 工作区明明是 .../workspace/cli2, pwd 却给 .../agent_env/xxx。
    //   模型照着 pwd 拼路径必然全错, 这是"cd 老出错"的一个根因。
    auto toSlash = [](string s)
    {
        for (char &c : s)
            if (c == '\\')
                c = '/';
        return s;
    };
    if (!old.empty() && old != abs)
        m_env["OLDPWD"] = toSlash(old);
    m_env["PWD"] = toSlash(abs);
    return true;
}

void JZShell::setEnvVar(const string &name, const string &value)
{
    m_env[name] = value;
}

void JZShell::addExportedEnv(const string &name, const string &value)
{
    if (name.empty())
        return;
    m_env[name] = value;
    m_exported.insert(name);
    // 宿主注入的配置 env 属于"解释器固有状态", 不是某条命令的残留 —— 进基线, 每条命令都要有
    m_baseEnv[name] = value;
    m_baseExported.insert(name);
}

void JZShell::resetState(const string &cwd)
{
    m_env = m_baseEnv;
    m_exported = m_baseExported;
    m_arrays.clear();
    m_aliases.clear();
    m_readonly.clear();
    m_dirStack.clear();
    m_functions.clear();
    m_funcStack.clear();
    m_params.clear();
    m_locals.clear();
    m_childEnvExtra.clear();
    m_lastStatus = 0;
    m_returning = false;
    if (!cwd.empty())
        setCwd(cwd);          // cwd 要在 PWD/OLDPWD 里, 否则 pwd 与真实位置对不上
}

void JZShell::unsetEnvVar(const string &name)
{
    m_env.erase(name);
}

string JZShell::envVar(const string &name) const
{
    auto it = m_env.find(name);
    if (it != m_env.end())
        return it->second;
    return envGet(name);
}

map<string, string> JZShell::envAll() const
{
    map<string, string> all = m_env;
    return all;
}

void JZShell::addPathDir(const string &dir)
{
    if (!dir.empty())
        m_pathDirs.push_back(dir);
}

// 子进程的 PATH。
// 为什么需要它: 短名 -> 全路径的映射(resolveProgram)只改写**命令行的第一个词**。
//   `timeout 8 code_runtime --entry x` 的命令词是 timeout, 内层的 code_runtime 只是 argv,
//   由 timeout 自己 execvp —— 它只看 PATH, 而 tsc / code_runtime 本来就不在 PATH 上
//   (它们是 programPaths 里的短名), 于是 `failed to run command 'code_runtime'` + exit=127。
// 把 m_pathDirs(Git 的 usr/bin、bin)与 programPaths 各自所在目录前插进子进程 PATH 之后,
// timeout / xargs / env / find -exec 这类"包一层"的写法都能正常工作。
string JZShell::childPathValue(const string &base) const
{
    vector<string> dirs;
    auto push = [&dirs](const string &d)
    {
        if (d.empty())
            return;
        const string key = str_toLower(d);
        for (const string &e : dirs)
        {
            if (str_toLower(e) == key)
                return;
        }
        dirs.push_back(d);
    };

    for (const string &d : m_pathDirs)
        push(d);
    for (const auto &kv : m_programPaths)
    {
        const string &p = kv.second;
        const size_t slash = p.find_last_of("/\\");
        if (slash != string::npos && slash > 0)
            push(p.substr(0, slash));
    }
    for (const string &d : splitChar(base.empty() ? envGet("PATH") : base, kPathSep))
        push(d);

    string out;
    for (size_t i = 0; i < dirs.size(); i++)
    {
        if (i)
            out += kPathSep;
        out += dirs[i];
    }
    return out;
}

void JZShell::addProgramPath(const string &name, const string &fullPath)
{
    if (name.empty() || fullPath.empty())
        return;
    string key = name;
    for (char &c : key)
        c = (char)tolower((unsigned char)c);
    m_programPaths[key] = fullPath;
}

string JZShell::programPath(const string &name) const
{
    if (name.empty())
        return string();
    string key = name;
    for (char &c : key)
        c = (char)tolower((unsigned char)c);
    auto it = m_programPaths.find(key);
    return it == m_programPaths.end() ? string() : it->second;
}

bool JZShell::parseOnly(const string &line, string *error) const
{
    string err;
    vector<Token> tokens = lex(line, err);
    if (!err.empty())
    {
        if (error)
            *error = err;
        return false;
    }
    NodePtr root;
    Parser p(tokens);
    if (!p.parse(root, err))
    {
        if (error)
            *error = err;
        return false;
    }
    return true;
}

// 取脚本的第 n 行(1 起, 去掉行尾 \r\n)。语法错误回显源码要用。
static string textLineAt(const string &text, int n)
{
    if (n <= 0)
        return string();
    size_t pos = 0;
    int cur = 1;
    while (pos <= text.size())
    {
        size_t e = text.find('\n', pos);
        const size_t end = (e == string::npos) ? text.size() : e;
        if (cur == n)
        {
            string s = text.substr(pos, end - pos);
            if (!s.empty() && s.back() == '\r')
                s.pop_back();
            return s;
        }
        if (e == string::npos)
            break;
        pos = e + 1;
        cur++;
    }
    return string();
}

JZShell::Result JZShell::exec(const string &line)
{
    Result out;
    // `source` 会在 runBuiltin 里再进一次 exec: 嵌套调用不能把外层的截止时间/取消信号冲掉
    // (退出时恢复外层状态)。
    struct ExecScopeGuard
    {
        JZShell *s;
        bool nested;
        long long prevDeadline;
        ~ExecScopeGuard()
        {
            s->m_inExec = nested;
            s->m_deadlineMsec = prevDeadline;
        }
    } guard{this, m_inExec, m_deadlineMsec};
    const bool nested = m_inExec;

    m_exiting = false;
    m_exitCode = 0;
    m_depth = 0;
    m_inBuf.clear();      // 上一行命令剩下的管道输入不能串到这一条
    if (!nested)
        m_cancelRequested = false;   // 只清"上一条命令"留下的取消; 嵌套时外层还在取消中
    // 超时是**整条命令行**的预算: 进来记一次截止时间, 后面每个外部命令等待都按剩余时间算。
    // 以前是"每个外部命令各算一遍 timeout", `for d in a b c; do du -s "$d"; done` 就能
    // 跑到 3×timeout, 把调用方的任务超时(server 侧 15min)撞掉 —— 20:20 那条任务就是这么挂的。
    {
        long long d = (m_timeoutMs > 0) ? (steadyMsec() + m_timeoutMs) : 0;
        if (nested && m_deadlineMsec > 0 && (d == 0 || m_deadlineMsec < d))
            d = m_deadlineMsec;      // 嵌套: 取更早的那个截止时间, 不允许"续命"
        m_deadlineMsec = d;
    }
    m_inExec = true;
    // ⚠ 不要在这里把 m_cwd 写回 policy: exec 跑在执行线程上, 而宿主(终端/文件工具)会在
    //   主线程用 policy() 做审核 —— 两边同时动 m_baseCwd 就是 std::string 数据竞争
    //   (实测: Debug 堆被踩坏, ucrtbased 里随机 0x80000003)。审核基准请由**调用方**
    //   显式传 base(终端就是这么做的), 或由宿主线程 setBaseCwd 一次定好。

    // 空行/纯空白: 什么都不做, 状态保持不变(bash 的行为)
    if (line.find_first_not_of(" \t\r\n") == string::npos)
    {
        out.ok = true;
        out.exitCode = m_lastStatus;
        out.cwd = m_cwd;
        return out;
    }

    string err;
    vector<Token> tokens = lex(line, err);
    if (!err.empty())
    {
        out.ok = false;
        out.exitCode = 2;
        out.error = errorPrefix(1) + err;
        out.cwd = m_cwd;
        m_lastStatus = 2;
        return out;
    }
    NodePtr root;
    Parser p(tokens);
    if (!p.parse(root, err))
    {
        out.ok = false;
        out.exitCode = 2;
        out.error = errorPrefix(p.failLine()) + err;
        // bash 报完语法错误会**回显出错那一行**:
        //   bash5: -c: line 1: `case x in esac) echo done; esac'
        // `unexpected end of file` / 引号没闭合这类没有具体行, 不回显。
        if (err.find("unexpected end of file") == string::npos &&
            err.find("unexpected EOF while looking for matching") == string::npos)
        {
            const string src = textLineAt(line, p.failLine());
            if (!src.empty())
                out.error += "\n" + errorPrefix(p.failLine()) + "`" + src + "'";
        }
        out.cwd = m_cwd;
        m_lastStatus = 2;
        return out;
    }

    StageResult r = runNode(root);
    if (m_exiting)
    {
        r.status = m_exitCode;
        m_exiting = false;
    }
    flushDirect(r);   // 直写模式: 剩下的尾巴也落 fd(之后 out.output 为空, 宿主不必再打印)

    out.cwd = m_cwd;
    out.exitCode = r.status;
    out.ok = !r.failed && r.status == 0;
    if (r.failed && !r.failReason.empty())
        out.error = r.failReason;
    out.output = r.out;
    if (!r.err.empty())
    {
        if (!out.output.empty() && out.output.back() != '\n')
            out.output += '\n';
        out.output += r.err;
        if (out.output.back() != '\n')
            out.output += '\n';   // 错误行必须自带换行, 否则下一条命令的输出会粘上来
    }
    m_lastStatus = r.status;
    return out;
}
