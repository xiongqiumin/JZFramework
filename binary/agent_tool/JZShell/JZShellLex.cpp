// JZShellLex.cpp —— 词法分析：源串 -> Token 序列
//
// 从 JZShell.cpp 拆出来：纯字符串处理，不依赖 JZShell 实例状态。
// 负责：空白/注释、运算符切分、引号、$( ) / 反引号 / (( )) 的整体识别。

#include "JZShell.h"

#include <cctype>
#include <cstring>

namespace jzsh
{


bool isSpaceChar(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

bool isNameStart(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool isNameChar(char c)
{
    return isNameStart(c) || (c >= '0' && c <= '9');
}

bool isAssignmentWord(const string &w)
{
    if (w.empty() || !isNameStart(w[0]))
        return false;
    size_t i = 0;
    while (i < w.size() && isNameChar(w[i]))
        i++;
    if (i == 0 || i >= w.size())
        return false;
    // 数组元素: `name[下标]=...` / `name[下标]+=...`(`x[4]+=1`)
    if (w[i] == '[')
    {
        int depth = 0;
        size_t j = i;
        for (; j < w.size(); j++)
        {
            if (w[j] == '[')
                depth++;
            else if (w[j] == ']' && --depth == 0)
            {
                j++;
                break;
            }
        }
        if (depth != 0 || j >= w.size())
            return false;
        i = j;
        if (w[i] == '=')
            return true;
        return w[i] == '+' && i + 1 < w.size() && w[i + 1] == '=';
    }
    if (w[i] == '=')
        return true;
    // `name+=value`: 追加赋值(也是赋值前缀, 不能当成普通命令词)
    return w[i] == '+' && i + 1 < w.size() && w[i + 1] == '=';
}
// ==========================================================================
// 词法
// ==========================================================================
// 注: Token / lex() 的声明在公共头 JZShell.h 的 namespace jzsh 里(三个 TU 共享),
//     这里**不要**再定义一份 —— 重复定义会 C2011, 且两份 struct 不一致时
//     vector<Token> 的成员会按"未定义类型"报一串 C2027。

bool isOpStart(char c)
{
    return c == '|' || c == '&' || c == ';' || c == '<' || c == '>' || c == '(' || c == ')' || c == '\n';
}

// s[i] 是 `((` 的第一个 '('; 返回配对的最外层 ')' 的位置(没有则 npos)。
// 算术表达式里 `(` / `)` 成对出现, 这里只数括号, 不认引号(算术里不写字符串)。
size_t findArithClose(const string &s, size_t i)
{
    int depth = 0;
    for (size_t j = i; j < s.size(); j++)
    {
        char c = s[j];
        if (c == '(')
            depth++;
        else if (c == ')')
        {
            depth--;
            if (depth == 0)
                return j;
            if (depth < 0)
                return string::npos;
        }
    }
    return string::npos;
}

// s[j] 处是不是独立的关键字(前后都不是名字字符)
static bool isKeywordAt(const string &s, size_t j, const char *w)
{
    size_t n = strlen(w);
    if (s.compare(j, n, w) != 0)
        return false;
    if (j > 0 && (isalnum((unsigned char)s[j - 1]) || s[j - 1] == '_'))
        return false;
    if (j + n < s.size() && (isalnum((unsigned char)s[j + n]) || s[j + n] == '_'))
        return false;
    return true;
}

// 从 s[i] 处的 '$' 开始, 吃掉整段 `$( ... )`(支持嵌套, 跳过引号和转义)。
// 成功时 inner 是括号内的原文, 返回值为 ')' 之后的位置; 没闭合返回 npos。
size_t consumeDollarParen(const string &s, size_t i, string &inner)
{
    if (i + 1 >= s.size() || s[i] != '$' || s[i + 1] != '(')
        return string::npos;

    size_t j = i + 2;
    size_t start = j;
    int depth = 1;
    // `case $x in pat) ... ;; esac` 里的 `)` 是**模式终止符**, 不是 `$(` 的收尾。
    // 所以进 case 之后一律不数括号, 直到 esac(期间嵌套的 $( ) 整段跳过)。
    bool inCase = false;
    while (j < s.size())
    {
        char c = s[j];

        // `#` 注释: 只在"词首"位置开始(前一个字符是空白/换行/分隔符), 注释里的 `)` 不算收尾
        if (c == '#' && (j == 0 || s[j - 1] == ' ' || s[j - 1] == '\t' ||
                         s[j - 1] == '\n' || s[j - 1] == ';' || s[j - 1] == '&' ||
                         s[j - 1] == '|' || s[j - 1] == '('))
        {
            while (j < s.size() && s[j] != '\n')
                j++;
            continue;
        }

        // heredoc: `<<WORD` / `<<-WORD` 之后到定界符行之间全是**数据**, 里面的 `)`
        // 不能当成 `$(` 的收尾(`echo $(cat <<eof\n)\neof\n)` 这种写法 bash 认)。
        // 只认"定界符之后就是行尾"的形式, 免得把 `$((a<<2))` 的移位当成 heredoc。
        if (c == '<' && j + 1 < s.size() && s[j + 1] == '<' &&
            (j + 2 >= s.size() || s[j + 2] != '<'))
        {
            bool stripTab = (j + 2 < s.size() && s[j + 2] == '-');
            size_t k = j + 2 + (stripTab ? 1 : 0);
            while (k < s.size() && (s[k] == ' ' || s[k] == '\t'))
                k++;

            string delim;
            while (k < s.size() && s[k] != '\n')
            {
                char d = s[k];
                if (d == '\'' || d == '"')   // 引号只影响是否展开, 定界符取名字本身
                {
                    k++;
                    continue;
                }
                if (d == '\\' && k + 1 < s.size())   // `<<\eof`: 反斜杠也只在关闭展开
                {
                    delim += s[k + 1];
                    k += 2;
                    continue;
                }
                // 注意: `)` 不能当分隔符 —— 定界符本身可能就是 `)`(bash tests 里
                // 就有 `cat << ')'`, 那是"引号包起来的右括号")
                if (isSpaceChar(d) || d == ';' || d == '|' || d == '&')
                    break;
                delim += d;
                k++;
            }

            // 定界符后面还有别的东西(分号/命令) → 不是"整行 heredoc", 不处理
            size_t after = k;
            while (after < s.size() && (s[after] == ' ' || s[after] == '\t'))
                after++;
            bool pureHeredoc = (after >= s.size() || s[after] == '\n');

            if (!delim.empty() && pureHeredoc)
            {
                while (k < s.size() && s[k] != '\n')
                    k++;
                if (k < s.size())
                    k++;
                while (k < s.size())   // 逐行找定界符行
                {
                    size_t lineStart = k;
                    while (k < s.size() && s[k] != '\n')
                        k++;
                    string line = s.substr(lineStart, k - lineStart);
                    if (!line.empty() && line.back() == '\r')   // CRLF 脚本: 行尾的 \r 不算
                        line.pop_back();
                    if (stripTab)
                    {
                        size_t t = line.find_first_not_of('\t');
                        line = (t == string::npos) ? string() : line.substr(t);
                    }
                    if (line == delim)
                    {
                        if (k < s.size())
                            k++;
                        break;
                    }
                    if (k < s.size())
                        k++;
                }
                j = k;
                continue;
            }
        }

        if (inCase)
        {
            if (c == '$' && j + 1 < s.size() && s[j + 1] == '(')
            {
                string dummy;
                size_t e = consumeDollarParen(s, j, dummy);
                if (e == string::npos)
                    return string::npos;
                j = e;
                continue;
            }
            if (c == '\\' && j + 1 < s.size())
            {
                j += 2;
                continue;
            }
            if (c == '\'')
            {
                j++;
                while (j < s.size() && s[j] != '\'')
                    j++;
                if (j < s.size())
                    j++;
                continue;
            }
            if (c == '"')
            {
                j++;
                while (j < s.size() && s[j] != '"')
                {
                    if (s[j] == '\\' && j + 1 < s.size())
                        j++;
                    j++;
                }
                if (j < s.size())
                    j++;
                continue;
            }
            if (isKeywordAt(s, j, "esac"))
            {
                inCase = false;
                j += 4;
                continue;
            }
            j++;
            continue;
        }
        if (isKeywordAt(s, j, "case"))
        {
            inCase = true;
            j += 4;
            continue;
        }
        if (c == '\\' && j + 1 < s.size())
        {
            j += 2;
            continue;
        }
        if (c == '\'')
        {
            j++;
            while (j < s.size() && s[j] != '\'')
                j++;
            if (j < s.size())
                j++;
            continue;
        }
        if (c == '"')
        {
            j++;
            while (j < s.size() && s[j] != '"')
            {
                if (s[j] == '\\' && j + 1 < s.size())
                {
                    j += 2;
                    continue;
                }
                j++;
            }
            if (j < s.size())
                j++;
            continue;
        }
        if (c == '(')
        {
            depth++;
            j++;
            continue;
        }
        if (c == ')')
        {
            depth--;
            if (depth == 0)
            {
                inner = s.substr(start, j - start);
                return j + 1;
            }
            j++;
            continue;
        }
        j++;
    }
    return string::npos;
}

// 词法: 引号内的内容**整体**留在 Word 里(含引号字符), 由展开阶段再处理。
// 这样 `echo "a b"` 是一个词, `echo a b` 是两个词。
// 去掉一个词里的引号与转义(未做 $ / 通配展开)。
// 用途: heredoc 定界符比对、审核视图里给上层看"程序名/重定向目标"的字面值。
string unquoteWord(const string &w)
{
    string r;
    for (size_t k = 0; k < w.size(); k++)
    {
        char ch = w[k];
        if (ch == '\'' || ch == '"')
            continue;
        if (ch == '\\' && k + 1 < w.size())
        {
            r += w[++k];
            continue;
        }
        r += ch;
    }
    return r;
}

vector<Token> lex(const string &s, string &error)
{
    vector<Token> out;
    size_t i = 0;

    // ---- heredoc 状态 ----
    // `<< WORD` 只登记定界符；正文在**下一个换行之后**才开始，且不能按普通语句切分
    // (正文里可以有 ; | 引号 甚至空行)。所以在这里记下"定界符 token 的位置",
    // 等读到换行时把正文整段取出来, 再插回该 token 之后。
    bool hdWait = false;                 // 刚读到 << , 下一个词是定界符
    bool hdStripTab = false;             // <<-  : 正文每行去掉前导 tab
    vector<size_t> hdPos;                // 定界符 token 在 out 里的下标
    vector<string> hdDelim;              // 定界符(已去引号, 见上面的 unquoteWord)
    // 换行 token 的下标 —— 词法里换行被当成 ';', 事后靠这张表把行号补上
    vector<size_t> nlPos;

    while (i < s.size())
    {
        char c = s[i];
        if (isSpaceChar(c))
        {
            i++;
            continue;
        }
        if (c == '#')
        {
            // 注释到行尾(多行脚本里不能把后面全吞掉)
            while (i < s.size() && s[i] != '\n')
                i++;
            continue;
        }
        if (c == '\n')
        {
            nlPos.push_back(out.size());
            out.push_back({ Token::Op, ";" });
            i++;

            // 这一行登记过 heredoc: 从这里开始吃掉正文, 直到各定界符
            if (!hdPos.empty())
            {
                size_t p = i;
                for (size_t k = 0; k < hdPos.size(); k++)
                {
                    string body;
                    bool closed = false;
                    while (p <= s.size())
                    {
                        size_t e = s.find('\n', p);
                        string line = (e == string::npos) ? s.substr(p) : s.substr(p, e - p);
                        if (!line.empty() && line.back() == '\r')
                            line.pop_back();
                        string cmp = line;
                        if (hdStripTab)
                        {
                            size_t t = cmp.find_first_not_of('\t');
                            cmp = (t == string::npos) ? string() : cmp.substr(t);
                        }
                        if (cmp == hdDelim[k])
                        {
                            p = (e == string::npos) ? s.size() : e + 1;
                            closed = true;
                            break;
                        }
                        body += cmp;
                        body += '\n';
                        if (e == string::npos)
                        {
                            p = s.size();
                            break;
                        }
                        p = e + 1;
                    }
                    (void)closed;
                    // 前面已经插入过正文, 后面的位置要整体右移
                    size_t insertAt = hdPos[k] + 1 + k;
                    out.insert(out.begin() + (ptrdiff_t)insertAt, Token{ Token::Heredoc, body });
                }
                i = p;
                hdPos.clear();
                hdDelim.clear();
            }
            continue;
        }

        // fd 前缀的重定向: `2>` 的 '2' 不是运算符, 要在这里单独认(否则会把 2 当参数)
        string rest = s.substr(i);
        // `0<&-` / `1>&-`(关闭 fd): 数字前缀并进运算符, 统一成不带数字的形式
        if (rest.size() >= 4 && isdigit((unsigned char)rest[0]) &&
            (rest.compare(1, 3, "<&-") == 0 || rest.compare(1, 3, ">&-") == 0))
        {
            out.push_back({ Token::Op, rest.substr(1, 3) });
            i += 4;
            continue;
        }
        if (rest.compare(0, 4, "2>&1") == 0)
        {
            out.push_back({ Token::Op, "2>&1" });
            i += 4;
            continue;
        }
        if (rest.compare(0, 3, "2>>") == 0)
        {
            out.push_back({ Token::Op, "2>>" });
            i += 3;
            continue;
        }
        if (rest.compare(0, 2, "2>") == 0)
        {
            out.push_back({ Token::Op, "2>" });
            i += 2;
            continue;
        }

        if (isOpStart(c))
        {
            // 算术命令 / 算术 for: `(( ... ))` 整段收成一个 Arith token。
            // 必须在这里拦: 否则 `i<3` 的 `<` 会被当成重定向, `i++` 的 `+` 会被当成普通词。
            if (rest.compare(0, 2, "((") == 0)
            {
                size_t end = findArithClose(s, i);
                if (end != string::npos)
                {
                    // end 指向收尾 "))" 的最后一个 ')', 内容到 end-2 为止
                    out.push_back({ Token::Arith, s.substr(i + 2, end - i - 3) });
                    i = end + 1;
                    continue;
                }
            }

            // 多字符运算符优先(顺序即优先级: ">>" 必须先于 ">", ";;" 先于 ";")
            static const struct
            {
                const char *op;
                size_t len;
            } kOps[] = {
                { ";;&", 3 }, { "<<<", 3 }, { "<<-", 3 }, { "<&-", 3 }, { ">&-", 3 },
                { "&>", 2 }, { ">>", 2 },
                { "&&", 2 }, { "||", 2 }, { ";&", 2 },
                { "<<", 2 }, { ";;", 2 }, { ">&", 2 },
                { ">", 1 }, { "<", 1 }, { "|", 1 }, { ";", 1 }, { "&", 1 }, { "(", 1 }, { ")", 1 },
            };
            bool matched = false;
            for (const auto &op : kOps)
            {
                if (rest.compare(0, op.len, op.op) == 0)
                {
                    out.push_back({ Token::Op, op.op });
                    i += op.len;
                    matched = true;
                    if (!strcmp(op.op, "<<") || !strcmp(op.op, "<<-"))
                    {
                        hdWait = true;
                        hdStripTab = !strcmp(op.op, "<<-");
                    }
                    break;
                }
            }
            if (matched)
                continue;

            out.push_back({ Token::Op, string(1, c) });
            i++;
            continue;
        }

        // 一个词: 直到空白 / 运算符(引号内的不算)
        string word;
        while (i < s.size())
        {
            char ch = s[i];

            // extglob 模式: `@( )` `?( )` `*( )` `+( )` `!( )` —— 括号整段并进同一个词。
            // bash 是在词法层就认掉的(它靠 shopt -s extglob 打开), 这里无条件认:
            // 普通脚本里 "词紧跟左括号" 本来就不是合法语法, 不会误伤。
            // 这样 `[[ x == @(a|b) ]]`、`case $x in +(a|b))` 都能拿到完整的模式词。
            if (ch == '(' && !word.empty() &&
                (word.back() == '@' || word.back() == '?' || word.back() == '*' ||
                 word.back() == '+' || word.back() == '!'))
            {
                int depth = 0;
                size_t j = i;
                while (j < s.size())
                {
                    char cj = s[j];
                    if (cj == '\\' && j + 1 < s.size())
                    {
                        word += cj;
                        word += s[j + 1];
                        j += 2;
                        continue;
                    }
                    if (cj == '(')
                        depth++;
                    else if (cj == ')')
                        depth--;
                    word += cj;
                    j++;
                    if (depth == 0)
                        break;
                }
                if (depth != 0)
                {
                    error = "unexpected EOF while looking for matching `)'";
                    return vector<Token>();
                }
                i = j;
                continue;
            }

            if (isSpaceChar(ch) || isOpStart(ch))
                break;
            if (ch == '`')
            {
                // 反引号命令替换: 内部原样取出, 转成 `$( ... )` 复用同一条展开路径
                size_t j = i + 1;
                string inner;
                while (j < s.size() && s[j] != '`')
                {
                    if (s[j] == '\\' && j + 1 < s.size() &&
                        (s[j + 1] == '`' || s[j + 1] == '\\' || s[j + 1] == '$'))
                    {
                        inner += s[j + 1];
                        j += 2;
                        continue;
                    }
                    inner += s[j++];
                }
                if (j >= s.size())
                {
                    error = "unexpected EOF while looking for matching ``'";
                    return vector<Token>();
                }
                word += "$(" + inner + ")";
                i = j + 1;
                continue;
            }
            if (ch == '$' && i + 1 < s.size() && s[i + 1] == '(')
            {
                string inner;
                size_t end = consumeDollarParen(s, i, inner);
                if (end == string::npos)
                {
                    error = "unexpected EOF while looking for matching `)'";
                    return vector<Token>();
                }
                word.append(s, i, end - i);
                i = end;
                continue;
            }
            if (ch == '\'')
            {
                word += ch;
                i++;
                while (i < s.size() && s[i] != '\'')
                    word += s[i++];
                if (i >= s.size())
                {
                    error = "unexpected EOF while looking for matching `''";
                    return vector<Token>();
                }
                word += s[i++];
                continue;
            }
            if (ch == '"')
            {
                word += ch;
                i++;
                while (i < s.size() && s[i] != '"')
                {
                    if (s[i] == '$' && i + 1 < s.size() && s[i + 1] == '(')
                    {
                        string inner;
                        size_t end = consumeDollarParen(s, i, inner);
                        if (end == string::npos)
                        {
                            error = "unexpected EOF while looking for matching `)'";
                            return vector<Token>();
                        }
                        word.append(s, i, end - i);
                        i = end;
                        continue;
                    }
                    if (s[i] == '`')
                    {
                        // 双引号里的反引号替换: 同样转成 `$( ... )`, 由展开阶段统一处理
                        size_t j = i + 1;
                        string inner;
                        while (j < s.size() && s[j] != '`')
                        {
                            if (s[j] == '\\' && j + 1 < s.size() &&
                                (s[j + 1] == '`' || s[j + 1] == '\\' || s[j + 1] == '$'))
                            {
                                inner += s[j + 1];
                                j += 2;
                                continue;
                            }
                            inner += s[j++];
                        }
                        if (j >= s.size())
                        {
                            error = "unexpected EOF while looking for matching ``'";
                            return vector<Token>();
                        }
                        word += "$(" + inner + ")";
                        i = j + 1;
                        continue;
                    }
                    if (s[i] == '\\' && i + 1 < s.size())
                    {
                        // 双引号里 `\` + 换行 同样是行继续, 两个字符都丢
                        if (s[i + 1] == '\n')
                        {
                            i += 2;
                            continue;
                        }
                        if (s[i + 1] == '\r' && i + 2 < s.size() && s[i + 2] == '\n')
                        {
                            i += 3;
                            continue;
                        }
                        word += s[i++];
                        word += s[i++];
                        continue;
                    }
                    word += s[i++];
                }
                if (i >= s.size())
                {
                    error = "unexpected EOF while looking for matching `\"'";
                    return vector<Token>();
                }
                word += s[i++];
                continue;
            }
            if (ch == '\\' && i + 1 < s.size())
            {
                // `\` + 换行 = 行继续, 两个字符**都丢掉**(bash 在分词前就删掉了)。
                // 留在词里的话, `echo AAA\` + 换行 会变成带换行的词 ⇒ 多打一个空行。
                if (s[i + 1] == '\n')
                {
                    i += 2;
                    continue;
                }
                if (s[i + 1] == '\r' && i + 2 < s.size() && s[i + 2] == '\n')
                {
                    i += 3;
                    continue;
                }
                word += s[i++];
                word += s[i++];
                continue;
            }
            word += ch;
            i++;
        }
        out.push_back({ Token::Word, word });
        if (hdWait)
        {
            // 这个词是 heredoc 定界符: 记下它的位置与(去引号后的)文本
            hdPos.push_back(out.size() - 1);
            hdDelim.push_back(unquoteWord(word));
            hdWait = false;
        }
    }
    out.push_back({ Token::Eof, string() });
    (void)error;

    // 补行号($LINENO / DEBUG trap 要按行报): 换行 token 之前算上一行,
    // 该 token 自己也算上一行(bash 里 newline 属于它结束的那一行)。
    {
        size_t k = 0;
        int ln = 1;
        for (size_t t = 0; t < out.size(); t++)
        {
            while (k < nlPos.size() && nlPos[k] < t)
            {
                ln++;
                k++;
            }
            out[t].line = ln;
        }
    }
    return out;
}

} // namespace jzsh
