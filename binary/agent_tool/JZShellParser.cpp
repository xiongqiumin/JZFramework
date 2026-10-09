// 见 JZShellParser.h: 这里是"词法 + 语法"两层, 只产出结构, 不执行任何命令
#include "JZShellParser.h"

#include <algorithm>
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
    std::size_t pos = 0;   // Word: 该词在原串里的起点(含引号)
    std::size_t end = 0;   // Word: 该词在原串里的终点(不含)
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
    Tokenizer(const std::string &text, bool trustUnknown = true)
        : m_s(text), m_trust(trustUnknown) {}

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
                if (!m_heredocs.empty())
                {
                    // 本行有 heredoc: 本行的换行 = 这条命令的结束(相当于一个 `;`),
                    // 之后才是 body(body 是数据, 不能当命令解析);
                    // 终止符行**尾**的换行不算分隔符, 一起吃掉 —— 否则会多出一个
                    // 空的命令段(`; ;`), 语法层会报"缺程序名"。
                    m_i++;
                    consumeHeredocBodies();
                    while (m_i < m_s.size() && isBreak(m_s[m_i]))
                        m_i++;
                    pushOp(out, ";");
                    continue;
                }
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
                if (m_trust)
                {
                    skipBacktick();      // 内容审不到: 整段跳过, 不进 AST
                    continue;
                }
                error = "不支持反引号命令替换: 里面的命令审核不到。请拆成多条命令";
                return false;
            }
            if (c == '(' || c == ')')
            {
                if (m_trust)
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
            t.pos = before;   // 原串位置: 上层换"短名 -> 全路径"要靠它原地替换
            t.end = m_i;
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

    // ---- "内容审不到"的跳过辅助: 整段吃掉、不进 AST、不报错(trustUnknown 口径) ----
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
    void skipSpaces()
    {
        while (m_i < m_s.size() && isSpace(m_s[m_i]))
            m_i++;
    }

    // 阅读顺序提醒: heredoc 的 body 不在 '<<' 后面, 而在**本行结束之后** ——
    //   `cat <<'EOF' && rm x` 里 `&& rm x` 是本行的命令(必须照常进 AST 去审),
    //   body 从下一行才开头, 且多个 heredoc 的 body 按登记顺序排队。
    //   ⇒ 这里只登记(吃掉 << 与定界符), body 由 consumeHeredocBodies() 在本行
    //     遇到换行时统一消费。以前是"从 << 一路跳到终止符行", 会把 `&& rm x`
    //     这种同行的命令整条吞掉(变成审核盲区)。
    struct Heredoc
    {
        std::string delim;       // 定界符(引号已去掉)
        bool stripTabs = false;  // <<-: 终止符行允许带前导 tab
    };

    // 从 '<<' 起读一个 heredoc 开头(<<、<<-、<<<)。调用前 m_i 指向第一个 '<' 且
    // m_s[m_i+1] == '<'。只消费"操作符 + 定界符", 不碰 body。
    void readHeredocOpener(std::string &error)
    {
        m_i += 2;                                   // 吃掉 <<
        if (m_i < m_s.size() && m_s[m_i] == '<')
        {
            // <<< here-string(bash 独有): 一个词直接当 stdin, 没有 body, 吃掉就走
            m_i++;
            skipSpaces();
            Token w;
            if (m_i < m_s.size() && !isBreak(m_s[m_i]))
                readWord(w, error);                 // 只消费, 不看它是什么
            return;
        }

        Heredoc h;
        if (m_i < m_s.size() && m_s[m_i] == '-')
        {
            h.stripTabs = true;
            m_i++;                                  // <<-: 终止符行允许前导 tab
        }
        skipSpaces();
        if (m_i < m_s.size() && (m_s[m_i] == '\'' || m_s[m_i] == '"'))
        {
            char q = m_s[m_i++];
            while (m_i < m_s.size() && m_s[m_i] != q)
                h.delim.push_back(m_s[m_i++]);
            if (m_i < m_s.size())
                m_i++;                              // 闭引号
        }
        else
        {
            while (m_i < m_s.size() && !isSpace(m_s[m_i]) && !isBreak(m_s[m_i]))
                h.delim.push_back(m_s[m_i++]);
        }

        // 定界符缺失(如结尾的 `cat <<`)不登记: 没有 body 可消费
        if (!h.delim.empty())
            m_heredocs.push_back(h);
    }

    // 消费本行登记的全部 heredoc body: 每个都读到"整行 == 定界符"为止。
    // 匹配比真实 shell **更宽松**(忽略行尾空格/tab; <<- 时忽略行首 tab; 把 \r 当换行),
    // 所以"我们认为的 body"永远不比 shell 认为的小 ⇒ 我们审的命令是 shell 实际会执行的
    // 命令的**超集**(只会多审, 不会漏审)。
    void consumeHeredocBodies()
    {
        for (const Heredoc &h : m_heredocs)
        {
            while (m_i < m_s.size())
            {
                std::string line;
                while (m_i < m_s.size() && !isBreak(m_s[m_i]))
                    line.push_back(m_s[m_i++]);
                if (h.stripTabs)
                {
                    size_t p = line.find_first_not_of('\t');
                    line = (p == std::string::npos) ? std::string() : line.substr(p);
                }
                while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
                    line.pop_back();
                if (line == h.delim)
                    break;
                while (m_i < m_s.size() && isBreak(m_s[m_i]))
                    m_i++;
            }
        }
        m_heredocs.clear();
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
                    // 双引号里会真展开(单引号里是字面量): 内容审不到 ⇒ 整段跳过、只标 expand
                    if (q == '"' && d == '$' && m_i + 1 < m_s.size() && m_s[m_i + 1] == '(')
                    {
                        if (!m_trust)
                        {
                            error = "不支持 $( ) 命令替换: 里面的命令审核不到。请拆成多条命令";
                            return false;
                        }
                        m_i++;                  // 指向 '('
                        skipToCloseParen();
                        expand = true;          // 展开结果是啥静态看不出
                        continue;
                    }
                    if (q == '"' && d == '`')
                    {
                        if (!m_trust)
                        {
                            error = "不支持反引号命令替换: 里面的命令审核不到。请拆成多条命令";
                            return false;
                        }
                        skipBacktick();
                        expand = true;
                        continue;
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
                    if (m_trust)
                    {
                        m_i++;               // 指向 '('
                        skipToCloseParen();
                        expand = true;       // 展开结果是啥静态看不出(当程序名时会被拒)
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
            if (!m_trust)
            {
                error = "不支持 heredoc(<<): 内容不在参数位置上, 审核不到";
                return false;
            }
            // 只登记(吃掉 << 与定界符), body 等本行结束再消费 —— 本行剩下的命令照常进 AST
            readHeredocOpener(error);
            return true;
        }
        if (m_i + 1 < m_s.size() && m_s[m_i + 1] == '(')
        {
            if (m_trust)
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
    bool m_trust = true;                 // trustUnknown: "内容审不到"就跳过而不是报错
    std::vector<Heredoc> m_heredocs;     // 本行登记的 heredoc, 换行时消费 body
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
                    error = "重定向目标带了展开字符($ ~ * ?), 审不出具体路径: " + target +
                            " —— 请直接写具体路径(或用文件工具 write_file)";
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

        if (cmd.argv.empty())
        {
            if (t.expand)
                cmd.programExpanded = true;

            // 第一个词就是程序名: 记下它在原串里的位置(供上层原地换成全路径)
            cmd.programPos = t.pos;
            cmd.programLen = (t.end > t.pos) ? (t.end - t.pos) : 0;
        }

        cmd.argv.push_back(t.text);
        i++;
    }
    return true;
}

} // namespace

bool parse(const std::string &text, Line &out, std::string &error, bool trustUnknown)
{
    std::vector<Token> toks;
    Tokenizer tz(text, trustUnknown);
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
                // 结尾的分号是合法的(`echo a;`、以及"命令串末尾带换行"都会产生它);
                // 其它操作符结尾都是残缺命令(如 `echo a &&`)。
                if (op == ";")
                    break;
                error = "命令不完整: `" + op + "` 后面没有命令";
                return false;
            }
            continue;
        }

        out.ops.push_back(std::string());
    }

    return true;
}

std::string rewritePrograms(const std::string &text, const Line &line,
                            const std::function<std::string(const Command &cmd)> &resolver)
{
    if (!resolver)
        return text;

    struct Repl
    {
        std::size_t pos;
        std::size_t len;
        std::string text;
    };
    std::vector<Repl> repls;

    for (const Pipeline &pl : line.pipelines)
    {
        for (const Command &cmd : pl.commands)
        {
            // 没有程序名 / 位置未知 / 程序名带展开(审不出是哪个程序): 都不换
            if (cmd.argv.empty() || cmd.programLen == 0 || cmd.programExpanded)
                continue;

            std::string repl = resolver(cmd);
            if (repl.empty())
                continue;

            Repl r;
            r.pos = cmd.programPos;
            r.len = cmd.programLen;
            r.text = repl;
            repls.push_back(r);
        }
    }
    if (repls.empty())
        return text;

    // 从后往前换: 后面的替换不会把前面的位置挪歪
    std::sort(repls.begin(), repls.end(),
              [](const Repl &a, const Repl &b) { return a.pos > b.pos; });

    std::string out = text;
    for (const Repl &r : repls)
    {
        if (r.pos + r.len > out.size())
            continue;   // 兜底: 位置对不上就不动它
        out.replace(r.pos, r.len, r.text);
    }
    return out;
}

} // namespace JZShell
