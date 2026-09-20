// 见 JZShellParser.h: 这里是"词法 + 语法"两层, 只产出结构, 不执行任何命令
#include "JZShellParser.h"

#include <cctype>
#include <cstdlib>

namespace JZShell
{

namespace
{

// ==================== 词法 ====================

struct Token
{
    enum Type { Word, Op, Redirect };
    Type type = Word;
    std::string text;      // Word: 去引号后的值; Op: && || ; | &; Redirect: > >> < >& <& &>
    bool quoted = false;   // Word 是否曾被引号包住
    bool expand = false;   // Word 里含 $ ~ * ?(静态看不出真实值)
    int fd = -1;           // Redirect 的左侧 fd, 如 2>
};

bool isSpace(char c) { return c == ' ' || c == '\t'; }
bool isBreak(char c) { return c == '\n' || c == '\r'; }
bool isIdentStart(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
bool isIdentChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

// 词在这里断开(也就是遇到这些字符, 当前词就结束了)
// '#' 不在里面: 只有词首的 # 才是注释, 词中间的(a#b)是普通字符
bool isDelim(char c)
{
    return isSpace(c) || isBreak(c) || c == '|' || c == '&' || c == ';' ||
           c == '<' || c == '>' || c == '(' || c == ')' || c == '`';
}

class Tokenizer
{
public:
    Tokenizer(const std::string &text, bool unsafe = false) : m_s(text), m_unsafe(unsafe) {}

    bool run(std::vector<Token> &out, std::string &error)
    {
        while (m_i < m_s.size())
        {
            char c = m_s[m_i];

            if (isSpace(c))
            {
                m_i++;
                continue;
            }
            if (isBreak(c))
            {
                // 多行命令当成分号串起来
                while (m_i < m_s.size() && isBreak(m_s[m_i]))
                    m_i++;
                pushOp(out, ";");
                continue;
            }
            if (c == '#')
            {
                while (m_i < m_s.size() && !isBreak(m_s[m_i]))
                    m_i++;
                continue;
            }
            if (c == ';')
            {
                m_i++;
                pushOp(out, ";");
                continue;
            }
            if (c == '|')
            {
                if (m_i + 1 < m_s.size() && m_s[m_i + 1] == '|')
                {
                    m_i += 2;
                    pushOp(out, "||");
                }
                else
                {
                    m_i++;
                    pushOp(out, "|");
                }
                continue;
            }

            // fd 前缀: 数字后面紧跟 < 或 >(如 2> / 12>>)
            if (std::isdigit((unsigned char)c))
            {
                size_t k = m_i;
                while (k < m_s.size() && std::isdigit((unsigned char)m_s[k]))
                    k++;
                if (k < m_s.size() && (m_s[k] == '<' || m_s[k] == '>'))
                {
                    m_fd = std::atoi(m_s.substr(m_i, k - m_i).c_str());
                    m_i = k;
                    continue;
                }
            }

            if (c == '>' || c == '<')
            {
                if (!readRedirect(out, error))
                    return false;
                continue;
            }

            if (c == '&')
            {
                // &>file: 把 stdout/stderr 一起重定向(bash 写法)
                if (m_i + 1 < m_s.size() && m_s[m_i + 1] == '>')
                {
                    m_i++;                  // 落到 '>' 上, 交给 readRedirect 收尾
                    m_ampRedirect = true;   // 记一下, 好把操作符记成 &> 而不是 >
                    if (!readRedirect(out, error))
                        return false;
                    continue;
                }
                if (m_i + 1 < m_s.size() && m_s[m_i + 1] == '&')
                {
                    m_i += 2;
                    pushOp(out, "&&");
                }
                else
                {
                    m_i++;
                    pushOp(out, "&");   // 后台运行, 由调用方拒绝
                }
                continue;
            }

            if (c == '`')
            {
                if (m_unsafe)
                {
                    skipBacktick();
                    continue;
                }
                error = "不支持反引号命令替换: 里面的命令审核不到。请拆成多条命令";
                return false;
            }
            if (c == '(' || c == ')')
            {
                if (m_unsafe)
                {
                    if (c == '(')
                        skipToCloseParen();
                    else
                        m_i++;   // 散落的 ')': 直接跳过
                    continue;
                }
                error = "不支持子shell ( ): 里面的命令审核不到。请拆成多条命令";
                return false;
            }

            Token t;
            size_t before = m_i;
            if (!readWord(t, error))
                return false;
            // 兜底: 真出现没覆盖到的字符时也不能让主循环原地打转(会一直分配 token 直到 OOM)
            if (m_i == before)
            {
                error = "命令里有解析不了的字符: " + std::string(1, c);
                return false;
            }
            out.push_back(t);
        }
        return true;
    }

private:
    void pushOp(std::vector<Token> &out, const std::string &text)
    {
        Token t;
        t.type = Token::Op;
        t.text = text;
        out.push_back(t);
    }

    // ---- allowUnsafe 下的"跳过"辅助: 把不被支持的结构整段吃掉, 不报错 ----
    // 跳到下一个反引号(含闭合的反引号), 找不到就跳到串尾
    void skipBacktick()
    {
        m_i++;   // 跳过开头的 `
        while (m_i < m_s.size() && m_s[m_i] != '`')
            m_i++;
        if (m_i < m_s.size())
            m_i++;   // 吃掉闭合 `
    }
    // 从 '(' 起, 跳到匹配的 ')'(尊重引号; 内部嵌套的 ( ) 一起数)。
    // 调用前 m_i 必须指向 '('。
    void skipToCloseParen()
    {
        int depth = 1;
        m_i++;   // 吃掉 '('
        while (m_i < m_s.size() && depth > 0)
        {
            char c = m_s[m_i];
            if (c == '\'' || c == '"')
            {
                char q = c;
                m_i++;
                while (m_i < m_s.size() && m_s[m_i] != q)
                {
                    if (m_s[m_i] == '\\' && m_i + 1 < m_s.size())
                        m_i++;
                    m_i++;
                }
                if (m_i < m_s.size())
                    m_i++;   // 闭引号
                continue;
            }
            if (c == '`')
            {
                skipBacktick();
                continue;
            }
            if (c == '(')
            {
                depth++;
                m_i++;
                continue;
            }
            if (c == ')')
            {
                depth--;
                m_i++;
                continue;
            }
            m_i++;
        }
    }
    // 从 '<<'(或 '<<-') 起, 跳到终止符所在行(整行等于终止符才结束)。
    // 调用前 m_i 必须指向第一个 '<' 且 m_s[m_i+1] == '<'。
    void skipHeredoc()
    {
        m_i += 2;                       // 吃掉 <<
        if (m_i < m_s.size() && m_s[m_i] == '-')
            m_i++;                      // <<- 去过行首 tab
        while (m_i < m_s.size() && isSpace(m_s[m_i]))
            m_i++;
        std::string delim;
        if (m_i < m_s.size() && (m_s[m_i] == '\'' || m_s[m_i] == '"'))
        {
            char q = m_s[m_i++];
            while (m_i < m_s.size() && m_s[m_i] != q)
                delim.push_back(m_s[m_i++]);
            if (m_i < m_s.size())
                m_i++;                  // 闭引号
        }
        else
        {
            while (m_i < m_s.size() && !isSpace(m_s[m_i]) && !isBreak(m_s[m_i]))
                delim.push_back(m_s[m_i++]);
        }
        while (m_i < m_s.size())
        {
            std::string line;
            while (m_i < m_s.size() && !isBreak(m_s[m_i]))
                line.push_back(m_s[m_i++]);
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
                line.pop_back();
            if (line == delim)
                break;
            while (m_i < m_s.size() && isBreak(m_s[m_i]))
                m_i++;
        }
    }

    // 读一个词(可能由多段拼起来: a"b c"d)。遇到分隔符就停, 让主循环处理
    bool readWord(Token &out, std::string &error)
    {
        std::string cur;
        bool quoted = false;
        bool expand = false;

        while (m_i < m_s.size())
        {
            char c = m_s[m_i];

            if (c == '#' && cur.empty())
                break;          // 词首的 # 是注释, 交给主循环跳到行尾
            if (isDelim(c))
                break;

            if (c == '\\')
            {
                m_i++;
                if (m_i < m_s.size())
                    cur.push_back(m_s[m_i++]);
                continue;
            }

            if (c == '\'' || c == '"')
            {
                char q = c;
                m_i++;
                bool closed = false;
                while (m_i < m_s.size())
                {
                    char d = m_s[m_i];
                    // 双引号里同样不允命令替换: 里面的命令审核不到(单引号里才是字面量)
                    if (q == '"' && d == '$' && m_i + 1 < m_s.size() && m_s[m_i + 1] == '(')
                    {
                        error = "不支持 $( ) 命令替换: 里面的命令审核不到。请拆成多条命令";
                        return false;
                    }
                    if (q == '"' && d == '`')
                    {
                        error = "不支持反引号命令替换: 里面的命令审核不到。请拆成多条命令";
                        return false;
                    }
                    if (q == '"' && d == '\\' && m_i + 1 < m_s.size())
                    {
                        cur.push_back(m_s[m_i + 1]);
                        m_i += 2;
                        continue;
                    }
                    if (d == q)
                    {
                        m_i++;
                        closed = true;
                        break;
                    }
                    // 单引号里全是字面量, 不标展开; 双引号里 $ 仍会展开
                    if (q == '"' && (d == '$' || d == '*' || d == '?'))
                        expand = true;
                    cur.push_back(d);
                    m_i++;
                }
                if (!closed)
                {
                    error = "命令里的引号不配对, 无法审核";
                    return false;
                }
                quoted = true;
                continue;
            }

            if (c == '$')
            {
                if (m_i + 1 < m_s.size() && m_s[m_i + 1] == '(')
                {
                    if (m_unsafe)
                    {
                        m_i++;               // 指向 '('
                        skipToCloseParen();
                        continue;
                    }
                    error = "不支持 $( ) 命令替换: 里面的命令审核不到。请拆成多条命令";
                    return false;
                }
                expand = true;
            }
            if (c == '*' || c == '?')
                expand = true;
            if (c == '~' && cur.empty())
                expand = true;
            // ${VAR} / {a,b}: 展开后是啥静态看不出, 标上 expand(当程序名时会被拒)
            if (c == '{' || c == '}')
                expand = true;

            cur.push_back(c);
            m_i++;
        }

        out.type = Token::Word;
        out.text = cur;
        out.quoted = quoted;
        out.expand = expand;
        return true;
    }

    // 读重定向符 + 它后面的目标
    bool readRedirect(std::vector<Token> &out, std::string &error)
    {
        char first = m_s[m_i];

        if (first == '<' && m_i + 1 < m_s.size() && m_s[m_i + 1] == '<')
        {
            if (m_unsafe)
            {
                skipHeredoc();
                return true;
            }
            error = "不支持 heredoc(<<): 内容不在参数位置上, 审核不到";
            return false;
        }
        if (m_i + 1 < m_s.size() && m_s[m_i + 1] == '(')
        {
            if (m_unsafe)
            {
                m_i++;               // 指向 '('
                skipToCloseParen();
                return true;
            }
            error = (first == '<') ? "不支持进程替换 <( ): 里面的命令审核不到"
                                   : "不支持进程替换 >( ): 里面的命令审核不到";
            return false;
        }

        bool amp = m_ampRedirect;
        m_ampRedirect = false;

        Token t;
        t.type = Token::Redirect;
        t.fd = amp ? -1 : m_fd;
        m_fd = -1;

        if (first == '<')
        {
            if (m_i + 1 < m_s.size() && m_s[m_i + 1] == '&')
            {
                t.text = "<&";
                m_i += 2;
            }
            else
            {
                t.text = "<";
                m_i += 1;
            }
        }
        else   // '>'
        {
            if (amp)
            {
                if (m_i + 1 < m_s.size() && m_s[m_i + 1] == '>')
                {
                    t.text = "&>>";
                    m_i += 2;
                }
                else
                {
                    t.text = "&>";
                    m_i += 1;
                }
            }
            else if (m_i + 1 < m_s.size() && m_s[m_i + 1] == '>')
            {
                t.text = ">>";
                m_i += 2;
            }
            else if (m_i + 1 < m_s.size() && m_s[m_i + 1] == '&')
            {
                t.text = ">&";
                m_i += 2;
            }
            else
            {
                t.text = ">";
                m_i += 1;
            }
        }
        out.push_back(t);

        while (m_i < m_s.size() && isSpace(m_s[m_i]))
            m_i++;
        if (m_i >= m_s.size() || isDelim(m_s[m_i]))
        {
            error = "重定向 `" + t.text + "` 后面没有目标";
            return false;
        }

        // &1 / &-: fd 复制, 不是文件。整体当成一个词交给语法层
        if (m_s[m_i] == '&')
        {
            std::string dup = "&";
            m_i++;
            while (m_i < m_s.size() && !isDelim(m_s[m_i]))
                dup.push_back(m_s[m_i++]);
            Token d;
            d.type = Token::Word;
            d.text = dup;
            out.push_back(d);
            return true;
        }

        Token w;
        if (!readWord(w, error))
            return false;
        out.push_back(w);
        return true;
    }

    const std::string &m_s;
    size_t m_i = 0;
    int m_fd = -1;
    bool m_ampRedirect = false;
    bool m_unsafe = false;   // allowUnsafe: 跳过不支持的结构而不是报错
};

// ==================== 语法 ====================

// VAR=val(只在命令最前面才当赋值前缀; 加过引号的不算)
bool isAssignment(const std::string &s)
{
    size_t eq = s.find('=');
    if (eq == std::string::npos || eq == 0)
        return false;
    if (!isIdentStart(s[0]))
        return false;
    for (size_t k = 1; k < eq; k++)
    {
        if (!isIdentChar(s[k]))
            return false;
    }
    return true;
}

bool parseCommand(const std::vector<Token> &toks, size_t &i, Command &cmd, std::string &error)
{
    while (i < toks.size())
    {
        const Token &t = toks[i];
        if (t.type == Token::Op)
            break;

        if (t.type == Token::Redirect)
        {
            i++;
            if (i >= toks.size() || toks[i].type != Token::Word)
            {
                error = "重定向 `" + t.text + "` 后面没有目标";
                return false;
            }

            Redirect r;
            r.fd = t.fd;
            r.op = t.text;
            r.read = (t.text == "<" || t.text == "<&");
            const std::string &target = toks[i].text;
            if (!target.empty() && target[0] == '&')
            {
                r.fdDup = true;
                r.target = target.substr(1);
            }
            else
            {
                r.target = target;
                if (toks[i].expand)
                {
                    error = "重定向目标带了展开字符($ ~ * ?), 审不出具体路径: " + target;
                    return false;
                }
            }
            cmd.redirects.push_back(r);
            i++;
            continue;
        }

        if (cmd.argv.empty() && !t.quoted && isAssignment(t.text))
        {
            cmd.assign.push_back(t.text);
            i++;
            continue;
        }

        if (cmd.argv.empty() && t.expand)
            cmd.programExpanded = true;

        cmd.argv.push_back(t.text);
        i++;
    }
    return true;
}

} // namespace

bool parse(const std::string &text, Line &out, std::string &error, bool allowUnsafe)
{
    std::vector<Token> toks;
    Tokenizer tz(text, allowUnsafe);
    if (!tz.run(toks, error))
        return false;

    size_t i = 0;
    while (i < toks.size())
    {
        Pipeline pl;
        while (true)
        {
            Command cmd;
            if (!parseCommand(toks, i, cmd, error))
                return false;
            if (cmd.argv.empty() && cmd.assign.empty() && cmd.redirects.empty())
            {
                error = "命令不完整: 缺程序名";
                return false;
            }
            pl.commands.push_back(cmd);

            if (i < toks.size() && toks[i].type == Token::Op && toks[i].text == "|")
            {
                i++;
                continue;
            }
            break;
        }
        out.pipelines.push_back(pl);

        if (i < toks.size() && toks[i].type == Token::Op)
        {
            std::string op = toks[i].text;
            i++;
            if (op == "&")
            {
                out.background = true;
                out.ops.push_back(std::string());
                continue;
            }
            out.ops.push_back(op);
            if (i >= toks.size())
            {
                error = "命令不完整: `" + op + "` 后面没有命令";
                return false;
            }
            continue;
        }

        out.ops.push_back(std::string());
    }

    return true;
}

} // namespace JZShell
