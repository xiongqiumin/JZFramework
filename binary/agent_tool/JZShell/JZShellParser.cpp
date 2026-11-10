// JZShellParser.cpp —— 语法分析：Token 序列 -> JZShellAst::Node
//
// 从 JZShell.cpp 拆出来：只管建语法树，不执行、不碰 JZShell 状态。
// 类体在 JZShellInternal.h（词法/执行两侧都要能看到 Parser 的签名）。

#include "JZShell.h"
#include "JZShellAudit.h"

namespace jzsh
{

// 重定向运算符判定(定义在 parseSimple 之后, 这里先声明)
static bool isRedirectOp(const string &op);

// ==========================================================================
// 语法分析
// ==========================================================================
bool Parser::parse(NodePtr &out, string &error)
{
    if (!parseList(out, set<string>(), error))
    {
        m_failLine = failedLine();   // 报错行: 前缀 `$0: line N:` 要用
        return false;
    }
    if (peek().type != Token::Eof)
    {
        if (peek().type == Token::Op && peek().text == "&")
            error = "syntax error near unexpected token `&'";
        else
            error = syntaxNear();
        m_failLine = failedLine();
        return false;
    }
    return true;
}

// 出错位置的行号: 尽量取"出错 token"那一行(m_pos 可能已经越过它, 往前退一格更准)
int Parser::failedLine() const
{
    if (m_t.empty())
        return 1;
    size_t p = m_pos < m_t.size() ? m_pos : m_t.size() - 1;
    return m_t[p].line > 0 ? m_t[p].line : 1;
}

bool Parser::parseList(NodePtr &out, const set<string> &stops, string &error)
{
    vector<NodePtr> parts;
    vector<string> ops;

    while (true)
    {
        // 跳过空语句(连续换行 / 多余的分号 / 注释行留下的 ';')
        while (acceptOp(";"))
        {
        }
        if (peek().type == Token::Eof)
            break;
        if (peek().type == Token::Word && stops.count(peek().text))
            break;
        if (peek().type == Token::Op && stops.count(peek().text))
            break;   // 子 shell 的 `)` / case 分支的 `;;`
        if (acceptOp(")"))
        {
            error = syntaxNear();
            return false;
        }

        NodePtr part;
        if (!parseAndOr(part, stops, error))
            return false;
        parts.push_back(part);

        if (acceptOp(";"))
        {
            ops.push_back(";");
            continue;
        }
        // `cmd &` 后台: 没有作业控制, 这里只把 `&` 当作语句分隔符
        // (同步执行, 不等价于 bash 的后台; 但语法上不再拒绝)
        if (acceptOp("&"))
        {
            ops.push_back(";");
            continue;
        }
        break;
    }

    if (parts.empty())
    {
        // 整段只有注释/空行(到 EOF): 合法, 当空脚本处理(bash 也不会报错)。
        // 其余情况(如 `if ; then`)仍然报错。
        if (peek().type == Token::Eof)
        {
            out = make_shared<Node>();
            out->kind = Node::Pipeline;   // stages 为空: 执行时什么都不做, status 0
            return true;
        }
        error = syntaxNear();
        return false;
    }
    if (parts.size() == 1)
    {
        out = parts[0];
        return true;
    }
    out = make_shared<Node>();
    out->kind = Node::AndOr;
    out->parts = parts;
    out->ops = ops;
    return true;
}

bool Parser::parseAndOr(NodePtr &out, const set<string> &stops, string &error)
{
    NodePtr first;
    if (!parsePipeline(first, error))
        return false;

    vector<NodePtr> parts;
    vector<string> ops;
    parts.push_back(first);

    while (isOp("&&") || isOp("||"))
    {
        string op = next().text;
        skipNewlines();     // `a &&\n b`: 换行不是语句结束
        NodePtr rhs;
        if (!parsePipeline(rhs, error))
            return false;
        ops.push_back(op);
        parts.push_back(rhs);
    }
    (void)stops;

    if (parts.size() == 1)
    {
        out = first;
        return true;
    }
    out = make_shared<Node>();
    out->kind = Node::AndOr;
    out->parts = parts;
    out->ops = ops;
    return true;
}

// `! cmd` / `! a | b`: 整体退出码取反(可以叠加 !!)
bool Parser::parsePipeline(NodePtr &out, string &error)
{
    bool negate = false;
    while (isWord("!"))
    {
        m_pos++;
        negate = !negate;
        // `!` 后面允许换行(bash 也认): 词法里换行就是一个 ';'
        while (acceptOp(";"))
        {
        }
    }
    if (!parsePipelineInner(out, error))
        return false;
    if (negate && out)
        out->negate = !out->negate;
    return true;
}

bool Parser::parsePipelineInner(NodePtr &out, string &error)
{
    NodePtr first;
    if (!parseCommand(first, error))
        return false;

    // 管道的每一段都是**完整命令**: 可以是简单命令, 也可以是复合命令
    // (`while ...; done | grep x`、`{ ...; } | wc`、`( ... ) | cat`、`if ...; fi | cat`)。
    vector<NodePtr> stages;
    stages.push_back(first);

    while (acceptOp("|"))
    {
        skipNewlines();     // `a |\n b`
        NodePtr rhs;
        if (!parseCommand(rhs, error))
            return false;
        stages.push_back(rhs);
    }

    if (stages.size() == 1)
    {
        out = first;
        return true;
    }

    out = make_shared<Node>();
    out->kind = Node::Pipeline;
    out->stages = stages;
    return true;
}

bool Parser::parseCommand(NodePtr &out, string &error)
{
    if (looksLikeFunctionDef())
        return parseFunction(out, error);

    // (( 算术命令 )) —— 状态 = (值 != 0) ? 0 : 1
    if (isArith())
    {
        auto node = make_shared<Node>();
        node->kind = Node::Arith;
        node->var = next().text;
        out = node;
        return true;
    }

    if (peek().type == Token::Word)
    {
        const string &w = peek().text;
        // `time [-p] 管道`: 保留字前缀。只执行后面的管道, **不打计时输出**
        // (没有等价于 POSIX TIMEFORMAT 的东西, 输出格式对不上; 至少不再报"找不到命令")。
        if (w == "time")
        {
            m_pos++;
            while (isWord("-p") || isWord("--") || isWord("-p-"))
                m_pos++;
            // `time` 后面没有命令(`time` 单独一行 / `{ time ; }`): bash 允许, 当 no-op
            if (peek().type == Token::Eof || isOp(";") || isOp(")") ||
                isWord("}") || isWord("done") || isWord("fi") || isWord("esac"))
            {
                auto node = make_shared<Node>();
                node->kind = Node::Simple;
                SimpleCmd colon;
                Word cw;
                cw.raw = ":";
                colon.argv.push_back(cw);
                node->cmd = colon;
                out = node;
                return true;
            }
            return parsePipelineInner(out, error);
        }
        if (w == "if")
            return parseIf(out, error);
        if (w == "for")
            return parseFor(out, error);
        if (w == "select")
        {
            // `select 名字 [in 词...]; do ... done` —— 语法与 for 相同, 只是执行时
            // 从标准输入逐行取(见 runNode 的 Node::For)。测试里 stdin 通常立刻 EOF,
            // 所以绝大多情况下循环体一次都不跑。
            bool ok = parseFor(out, error);
            if (ok && out)
                out->selectLoop = true;
            return ok;
        }
        if (w == "while")
        {
            m_until = false;
            return parseWhile(out, error);
        }
        if (w == "until")
        {
            m_until = true;
            return parseWhile(out, error);
        }
        if (w == "case")
            return parseCase(out, error);
        if (w == "then" || w == "elif" || w == "else" || w == "fi" || w == "do" ||
            w == "done" || w == "esac" || w == "}")
        {
            error = syntaxNear();
            return false;
        }
    }
    if (isWord("[["))
        return parseCond(out, error);
    if (isWord("{"))
        return parseCompound(out, error);
    if (isOp("("))
        return parseSubshell(out, error);

    return parseSimple(out, error);
}

// 子 shell: ( ... ) —— 里面的状态改动不泄漏到外层
bool Parser::parseSubshell(NodePtr &out, string &error)
{
    m_pos++;   // (
    auto node = make_shared<Node>();
    node->kind = Node::Subshell;

    NodePtr body;
    if (!parseList(body, { ")" }, error))
        return false;
    if (!acceptOp(")"))
    {
        error = "syntax error: unexpected end of file";
        return false;
    }
    node->body = body;
    if (!parseRedirects(node->redirects, error))   // `{ ...; } > f` / `( ... ) 2>&1` / `done >> log`
        return false;
    out = node;
    return true;
}

// 花括号组: { list; } —— 在当前 shell 里执行(与子 shell 相反, 状态会保留)
bool Parser::parseCompound(NodePtr &out, string &error)
{
    m_pos++;   // {
    auto node = make_shared<Node>();
    node->kind = Node::Group;

    NodePtr body;
    if (!parseList(body, { "}" }, error))
        return false;
    if (!expectWord("}", error))
        return false;
    node->body = body;
    if (!parseRedirects(node->redirects, error))   // `{ ...; } > f` / `( ... ) 2>&1` / `done >> log`
        return false;
    out = node;
    return true;
}

bool Parser::parseSimple(NodePtr &out, string &error)
{
    auto node = make_shared<Node>();
    node->kind = Node::Simple;
    node->line = peek().line;    // 语句起始行: $LINENO / DEBUG trap 用
    SimpleCmd cmd;

    while (true)
    {
        if (peek().type == Token::Eof)
            break;
        if (peek().type == Token::Op)
        {
            if (isRedirectOp(peek().text))
            {
                // 重定向可以出现在命令的任意位置(`> f cmd` / `cmd > f`), 与 bash 一致
                if (!parseRedirects(cmd.redirects, error))
                    return false;
                continue;
            }
            // 其它 op 交给上层(; | && || 等)
            break;
        }

        // 赋值前缀只允许出现在命令名之前; `local a=(...)` / `declare -a a=(...)` 这种
        // "声明内建 + 数组赋值" 也按赋值处理(bash 同样允许)。
        bool canAssign = cmd.argv.empty();
        // "声明内建 + 赋值": 只有**数组**赋值才由解析器接走(`declare -a a=(1)`),
        // 普通 `export FOO=bar` 必须留在 argv 里交给内建 —— 否则会被当成"命令前缀赋值",
        // runSimple 执行完就回滚, export 等于没做。
        bool declBuiltin = false;
        if (!canAssign && !cmd.argv.empty())
        {
            const string &p = cmd.argv[0].raw;
            if (p == "local" || p == "declare" || p == "typeset" ||
                p == "readonly" || p == "export")
            {
                // 只允许中间夹选项: `declare -a a=(1)` / `readonly -a a=(2)`
                bool onlyOpts = true;
                for (size_t ai = 1; ai < cmd.argv.size(); ai++)
                {
                    if (cmd.argv[ai].raw.empty() || cmd.argv[ai].raw[0] != '-')
                        onlyOpts = false;
                }
                canAssign = onlyOpts;
                declBuiltin = onlyOpts;
            }
        }
        if (canAssign && isAssignmentWord(peek().text))
        {
            string word = next().text;
            // `a=( ... )` / `a+=( ... )`: 数组赋值(元素是词, 可以带引号/展开)
            if (!word.empty() && word.back() == '=' &&
                peek().type == Token::Op && peek().text == "(")
            {
                string name = word.substr(0, word.size() - 1);
                string prefix;   // "+" 表示追加
                if (!name.empty() && name.back() == '+')
                {
                    prefix = "+";
                    name.pop_back();
                }
                m_pos++;   // (
                vector<Word> elems;
                while (peek().type == Token::Word && !isOp(")"))
                {
                    Word e;
                    e.raw = next().text;
                    elems.push_back(e);
                }
                if (!acceptOp(")"))
                {
                    error = syntaxUnclosed(")");
                    return false;
                }
                cmd.arrayAssigns.push_back(make_pair(prefix + name, elems));
                continue;
            }
            Word w;
            w.raw = word;
            if (declBuiltin)
                cmd.argv.push_back(w);    // `export FOO=bar`: 由内建持久写入
            else
                cmd.assigns.push_back(w);
            continue;
        }

        Word w;
        w.raw = next().text;
        cmd.argv.push_back(w);
    }

    if (cmd.argv.empty() && cmd.assigns.empty() && cmd.arrayAssigns.empty() &&
        cmd.redirects.empty())
    {
        error = syntaxNear();
        return false;
    }
    node->cmd = cmd;
    out = node;
    return true;
}

// 重定向运算符判定(简单命令与复合命令共用)
static bool isRedirectOp(const string &op)
{
    return op == ">" || op == ">>" || op == "<" || op == "2>" || op == "2>>" ||
           op == "2>&1" || op == "&>" || op == "<<" || op == "<<-" || op == "<<<" ||
           op == ">&" || op == "<&-" || op == ">&-";
}

// 收集连续的重定向。语义与 bash 一致: 一条命令可以挂任意个重定向, 后者覆盖前者。
bool Parser::parseRedirects(vector<Redirect> &out, string &error)
{
    while (peek().type == Token::Op && isRedirectOp(peek().text))
    {
        const string op = peek().text;
        m_pos++;

        Redirect r;
        if (op == "2>&1")
        {
            // fd 复制: 不需要目标词
            r.op = op;
            out.push_back(r);
            continue;
        }

        if (op == "<&-" || op == ">&-")
        {
            // 关闭 fd(`exec 0<&-`): 不需要目标词。本工程没有真 fd, 运行期当空操作。
            r.op = op;
            out.push_back(r);
            continue;
        }

        if (op == ">&")
        {
            // `>&2`(只复制 fd) / `>&文件`(= `> 文件 2>&1`)
            if (peek().type != Token::Word)
            {
                error = syntaxNear();
                return false;
            }
            const string target = next().text;
            if (!target.empty() && target.find_first_not_of("0123456789") == string::npos)
                r.op = ">&" + target;        // ">&2": 只有 fd
            else
            {
                r.op = ">&";                 // ">&file": 标准输出+标准错误
                r.target.raw = target;
            }
            out.push_back(r);
            continue;
        }

        // heredoc / here-string: 正文由词法阶段收进紧跟定界符的 Heredoc token
        if (op == "<<" || op == "<<-")
        {
            if (peek().type != Token::Word)
            {
                error = syntaxNear();
                return false;
            }
            // 定界符带引号/反斜杠(`<<'EOF'` / <<\EOF) ⇒ 正文按字面收, 不展开
            const string &delimRaw = peek().text;
            const bool quotedDelim = delimRaw.find_first_of("'\"\\") != string::npos;
            m_pos++;   // 定界符
            r.op = "<<";
            r.noExpand = quotedDelim;
            if (peek().type == Token::Heredoc)
                r.target.raw = next().text;   // 正文
            out.push_back(r);
            continue;
        }

        if (peek().type != Token::Word)
        {
            error = syntaxNear();
            return false;
        }
        r.op = op;
        r.target.raw = next().text;
        out.push_back(r);
    }
    return true;
}

bool Parser::parseIf(NodePtr &out, string &error)
{
    m_pos++;   // if
    auto node = make_shared<Node>();
    node->kind = Node::If;

    NodePtr cond;
    if (!parseList(cond, { "then" }, error))
        return false;
    if (!expectWord("then", error))
        return false;
    node->conds.push_back(cond);

    NodePtr body;
    if (!parseList(body, { "elif", "else", "fi" }, error))
        return false;
    node->bodies.push_back(body);

    while (isWord("elif"))
    {
        m_pos++;
        NodePtr c2;
        if (!parseList(c2, { "then" }, error))
            return false;
        if (!expectWord("then", error))
            return false;
        NodePtr b2;
        if (!parseList(b2, { "elif", "else", "fi" }, error))
            return false;
        node->conds.push_back(c2);
        node->bodies.push_back(b2);
    }

    if (isWord("else"))
    {
        m_pos++;
        NodePtr eb;
        if (!parseList(eb, { "fi" }, error))
            return false;
        node->elseBody = eb;
    }

    if (!expectWord("fi", error))
        return false;
    // `fi > f` / `fi 2>&1`: 复合命令自己的重定向(bash 也允许)
    if (!parseRedirects(node->redirects, error))
        return false;
    out = node;
    return true;
}

// 循环体: bash 两种写法都认 —— `; do ... done` 和 `; { ... }`
bool Parser::parseLoopBody(NodePtr &out, string &error)
{
    acceptOp(";");

    if (isWord("{"))
    {
        m_pos++;
        if (!parseList(out, { "}" }, error))
            return false;
        return expectWord("}", error);
    }

    if (!expectWord("do", error))
        return false;
    if (!parseList(out, { "done" }, error))
        return false;
    return expectWord("done", error);
}

bool Parser::parseFor(NodePtr &out, string &error)
{
    m_pos++;   // for

    // for (( init; cond; step )); do ... done
    if (isArith())
    {
        auto node = make_shared<Node>();
        node->kind = Node::ForArith;
        node->var = next().text;
        NodePtr body;
        if (!parseLoopBody(body, error))
            return false;
        node->body = body;
        if (!parseRedirects(node->redirects, error))
            return false;
        out = node;
        return true;
    }

    if (peek().type != Token::Word || !isNameStart(peek().text[0]))
    {
        error = syntaxNear();
        return false;
    }
    auto node = make_shared<Node>();
    node->kind = Node::For;
    node->var = next().text;

    node->forNoIn = !isWord("in");   // `for i; do` 遍历 "$@", `for i in; do` 是空列表
    if (isWord("in"))
    {
        m_pos++;
        while (peek().type == Token::Word)
        {
            Word w;
            w.raw = next().text;
            node->words.push_back(w);
        }
    }
    NodePtr body;
    if (!parseLoopBody(body, error))   // `; do ... done` 或 `; { ... }`
        return false;
    node->body = body;
    if (!parseRedirects(node->redirects, error))   // `{ ...; } > f` / `( ... ) 2>&1` / `done >> log`
        return false;
    out = node;
    return true;
}

bool Parser::parseWhile(NodePtr &out, string &error)
{
    m_pos++;   // while / until
    auto node = make_shared<Node>();
    node->kind = Node::While;
    node->until = m_until;
    m_until = false;

    NodePtr cond;
    if (!parseList(cond, { "do" }, error))
        return false;
    if (!expectWord("do", error))
        return false;
    NodePtr body;
    if (!parseList(body, { "done" }, error))
        return false;
    if (!expectWord("done", error))
        return false;
    node->cond = cond;
    node->body = body;
    if (!parseRedirects(node->redirects, error))   // `{ ...; } > f` / `( ... ) 2>&1` / `done >> log`
        return false;
    out = node;
    return true;
}

// case 主语 in 模式) 分支 ;; ... esac
//   模式里用 | 分隔候选, 支持通配(* ? []) —— 匹配用 globMatch。
bool Parser::parseCase(NodePtr &out, string &error)
{
    m_pos++;   // case

    if (peek().type != Token::Word)
    {
        error = syntaxNear();
        return false;
    }
    auto node = make_shared<Node>();
    node->kind = Node::Case;
    node->var = next().text;

    // POSIX: 主语和 `in` 之间可以换行(`case "$x"\nin ...`), 这很常见
    skipNewlines();
    if (!expectWord("in", error))
        return false;
    acceptOp(";");

    while (true)
    {
        while (acceptOp(";;") || acceptOp(";;&") || acceptOp(";&") || acceptOp(";"))
        {
        }
        if (isWord("esac"))
        {
            m_pos++;
            break;
        }
        if (peek().type == Token::Eof)
        {
            error = "syntax error: unexpected end of file";
            return false;
        }

        // 分支模式: word (| word)* 直到 `)`
        // POSIX 允许可选的左括号: `(pattern) cmd ;;` 与 `pattern) cmd ;;` 等价
        acceptOp("(");
        string pattern;
        while (peek().type == Token::Word || isOp("|"))
        {
            if (peek().type == Token::Word)
                pattern += next().text;
            else
            {
                pattern += "|";
                m_pos++;
            }
        }
        if (!acceptOp(")"))
        {
            error = syntaxNear();
            return false;
        }
        if (pattern.empty())
            pattern = "*";

        // 空分支: `* )` 后面直接换行 + esac / `;;`(换行在词法里就是 ';')
        while (acceptOp(";"))
        {
        }

        NodePtr body;
        bool atStop = (peek().type == Token::Word && peek().text == "esac") ||
                      (peek().type == Token::Op && (peek().text == ";;" ||
                                                    peek().text == ";&" ||
                                                    peek().text == ";;&"));
        if (atStop)
        {
            // `a) ;;` / `a) esac` —— 空分支合法
            auto noop = make_shared<Node>();
            noop->kind = Node::Simple;
            SimpleCmd colon;
            Word colonWord;
            colonWord.raw = ":";
            colon.argv.push_back(colonWord);
            noop->cmd = colon;
            body = noop;
        }
        else if (!parseList(body, { ";;", ";&", ";;&", "esac" }, error))
        {
            return false;
        }

        node->words.push_back(Word{ pattern });
        node->bodies.push_back(body);

        // 分支终止符: `;;` 结束; `;&` 落到下一支(不再匹配); `;;&` 继续匹配后面的模式
        string term = ";;";
        if (acceptOp(";;&"))
            term = ";;&";
        else if (acceptOp(";&"))
            term = ";&";
        else
            acceptOp(";;");
        node->ops.push_back(term);
    }
    if (!parseRedirects(node->redirects, error))   // `esac > f`
        return false;
    out = node;
    return true;
}

// 函数定义: name () { ... }   或   name () ( ... )
bool Parser::parseFunction(NodePtr &out, string &error)
{
    auto node = make_shared<Node>();
    node->kind = Node::Function;
    node->var = next().text;   // 函数名
    m_pos += 2;                // 吃掉 ( ) 两个 token
    while (isOp(";"))
        m_pos++;               // `f ()\n{ ... }`: 换行在词法里就是 ';'

    if (isWord("{"))
    {
        m_pos++;
        NodePtr body;
        if (!parseList(body, { "}" }, error))
            return false;
        if (!expectWord("}", error))
            return false;
        node->body = body;
    }
    else if (isOp("("))
    {
        m_pos++;
        NodePtr body;
        if (!parseList(body, { ")" }, error))
            return false;
        if (!acceptOp(")"))
        {
            error = syntaxNear();
            return false;
        }
        node->body = body;
    }
    else
    {
        error = syntaxNear();
        return false;
    }

    // `f() { ...; } > out`: 重定向属于**函数体**, 每次调用都生效(bash 同义),
    // 所以挂到 body 上, 而不是定义这个动作上。
    if (!parseRedirects(node->body->redirects, error))
        return false;

    out = node;
    return true;
}

// ==========================================================================
// [[ ... ]] 条件表达式
//   文法(与 bash parse.y 的 cond 段同构):
//     or      := and ( '||' and )*
//     and     := ( '!' )* term ( '&&' ( '!' )* term )*
//     term    := '(' or ')' | 词 二元运算符 词 | 单目运算符 词 | 词(非空为真)
//   运算符靠词法后剩下的 Token 形态判断: `&& ||` 与 `( )` 是 Op; `== != =~ -eq` 等
//   因为 `=`、`-` 不是运算符起始字符, 会原样留在 Word 里; `<` `>` 是 Op。
// ==========================================================================
static bool isCondUnaryOp(const string &w)
{
    static const char *ops[] = { "-n", "-z", "-f", "-d", "-e", "-r", "-w", "-x",
                                 "-s", "-L", "-h", "-t", "-p", "-S", "-b", "-c" };
    for (const char *o : ops)
    {
        if (w == o)
            return true;
    }
    return false;
}

static bool isCondBinOp(const string &w)
{
    static const char *ops[] = { "=", "==", "!=", "=~", "-eq", "-ne", "-lt", "-le",
                                 "-gt", "-ge", "-nt", "-ot", "-ef" };
    for (const char *o : ops)
    {
        if (w == o)
            return true;
    }
    return false;
}

bool Parser::parseCond(NodePtr &out, string &error)
{
    m_pos++;   // [[
    NodePtr inner;
    if (!parseCondOr(inner, error))
        return false;
    if (!expectWord("]]", error))
        return false;

    if (!inner)
    {
        inner = make_shared<Node>();
        inner->kind = Node::Cond;
    }
    out = inner;
    // `[[ ... ]] > f` 这种重定向(少见, 但语法上合法)
    if (!parseRedirects(out->redirects, error))
        return false;
    return true;
}

bool Parser::parseCondOr(NodePtr &out, string &error)
{
    NodePtr first;
    if (!parseCondAnd(first, error))
        return false;

    vector<NodePtr> parts;
    vector<string> ops;
    parts.push_back(first);
    while (isOp("||"))
    {
        m_pos++;
        NodePtr rhs;
        if (!parseCondAnd(rhs, error))
            return false;
        ops.push_back("||");
        parts.push_back(rhs);
    }
    if (parts.size() == 1)
    {
        out = first;
        return true;
    }
    auto node = make_shared<Node>();
    node->kind = Node::Cond;
    node->parts = parts;
    node->ops = ops;
    out = node;
    return true;
}

bool Parser::parseCondAnd(NodePtr &out, string &error)
{
    NodePtr first;
    if (!parseCondTerm(first, error))
        return false;

    vector<NodePtr> parts;
    vector<string> ops;
    parts.push_back(first);
    while (isOp("&&"))
    {
        m_pos++;
        NodePtr rhs;
        if (!parseCondTerm(rhs, error))
            return false;
        ops.push_back("&&");
        parts.push_back(rhs);
    }
    if (parts.size() == 1)
    {
        out = first;
        return true;
    }
    auto node = make_shared<Node>();
    node->kind = Node::Cond;
    node->parts = parts;
    node->ops = ops;
    out = node;
    return true;
}

bool Parser::parseCondTerm(NodePtr &out, string &error)
{
    // `!` 前缀(可叠加)
    bool negate = false;
    while (isWord("!"))
    {
        m_pos++;
        negate = !negate;
    }

    auto node = make_shared<Node>();
    node->kind = Node::Cond;
    node->negate = negate;

    // 括号分组
    if (isOp("("))
    {
        m_pos++;
        NodePtr inner;
        if (!parseCondOr(inner, error))
            return false;
        if (!acceptOp(")"))
        {
            error = "syntax error: unexpected end of file";
            return false;
        }
        node->cond = inner;
        out = node;
        return true;
    }

    if (peek().type != Token::Word)
    {
        error = "syntax error in conditional expression: unexpected token `" + peek().text + "'";
        return false;
    }

    string w = next().text;

    // 单目测试: `[[ -f FILE ]]`
    if (isCondUnaryOp(w) && peek().type == Token::Word)
    {
        node->var = w;
        Word ow;
        ow.raw = next().text;
        node->words.push_back(ow);
        out = node;
        return true;
    }

    // 二元比较
    string op;
    bool haveOp = false;
    if (peek().type == Token::Word && isCondBinOp(peek().text))
    {
        op = next().text;
        haveOp = true;
    }
    else if (peek().type == Token::Op && (peek().text == "<" || peek().text == ">"))
    {
        op = next().text;
        haveOp = true;
    }

    Word lhs;
    lhs.raw = w;
    node->words.push_back(lhs);
    if (haveOp)
    {
        node->var = op;
        if (peek().type != Token::Word)
        {
            error = "syntax error in conditional expression: unexpected end of file";
            return false;
        }
        Word rhs;
        rhs.raw = next().text;
        node->words.push_back(rhs);
    }
    out = node;
    return true;
}

// ==========================================================================
// 审核视图: 把解释器 AST 按**执行顺序**压平成 pipelines
//
// 审核方(终端/策略)只需要"简单命令 + 重定向"; 复合命令会被展开成内部的命令,
// 所以 `for i in a b; do rm -rf /; done` 里的 rm 也会被审到(旧版只看到 `for` 这个词)。
// ==========================================================================
namespace
{
// 程序名/路径里出现这些字符 = 静态审不出值(和旧审核解析器的口径一致)
bool hasExpansion(const string &w)
{
    return w.find_first_of("$~*?`") != string::npos;
}

struct AuditWalker
{
    vector<AuditPipeline> &out;
    string &error;
    bool ok = true;
    AuditPipeline cur;

    explicit AuditWalker(vector<AuditPipeline> &o, string &e) : out(o), error(e) {}

    void flush()
    {
        if (!cur.commands.empty())
        {
            out.push_back(cur);
            cur.commands.clear();
        }
    }

    void addSimple(const SimpleCmd &c)
    {
        AuditCommand cmd;
        for (const Word &w : c.argv)
            cmd.argv.push_back(unquoteWord(w.raw));
        if (!cmd.argv.empty())
            cmd.programExpanded = hasExpansion(cmd.argv[0]);

        for (const Redirect &r : c.redirects)
        {
            // heredoc / here-string 的 target 是**正文/数据**, 不是路径
            if (r.op == "<<" || r.op == "<<-")
                continue;

            AuditRedirect ar;
            ar.op = r.op;
            ar.fdDup = (r.op == "2>&1");
            ar.read = (r.op == "<");
            if (r.op == "2>" || r.op == "2>>")
                ar.fd = 2;
            ar.target = unquoteWord(r.target.raw);

            if (ar.fdDup)
            {
                cmd.redirects.push_back(ar);   // 复制 fd, 后面按 fdDup 跳过路径检查
                continue;
            }
            if (hasExpansion(ar.target))
            {
                error = "cannot statically resolve redirection target (contains a variable or "
                        "wildcard): " + ar.target;
                ok = false;
                return;
            }
            cmd.redirects.push_back(ar);
        }
        cur.commands.push_back(cmd);
    }

    void visit(const NodePtr &n)
    {
        if (!n || !ok)
            return;
        switch (n->kind)
        {
        case Node::Simple:
            addSimple(n->cmd);
            break;

        case Node::Pipeline:
        {
            for (const NodePtr &st : n->stages)
            {
                if (st && st->kind == Node::Simple)
                    addSimple(st->cmd);
                else
                {
                    flush();
                    visit(st);
                    flush();
                }
            }
            flush();   // 管道是审核的最小单位, 结束就收一条
            break;
        }

        case Node::AndOr:
            for (const NodePtr &p : n->parts)
            {
                visit(p);
                flush();
            }
            break;

        case Node::Subshell:
        case Node::Group:
            flush();
            visit(n->body);
            flush();
            break;

        case Node::If:
            for (const NodePtr &c : n->conds)
            {
                flush();
                visit(c);
                flush();
            }
            for (const NodePtr &b : n->bodies)
            {
                flush();
                visit(b);
                flush();
            }
            if (n->elseBody)
            {
                flush();
                visit(n->elseBody);
                flush();
            }
            break;

        case Node::For:
        case Node::ForArith:
        case Node::Function:   // 函数体在定义时就审: 调用时可能看不见
            flush();
            visit(n->body);
            flush();
            break;

        case Node::While:
            flush();
            visit(n->cond);
            visit(n->body);
            flush();
            break;

        case Node::Case:
            flush();
            for (const NodePtr &b : n->bodies)
            {
                visit(b);
                flush();
            }
            break;

        case Node::Cond:    // [[ ]]: 没有外部命令
        case Node::Arith:   // (( )): 纯算术
            break;
        }
    }
};
}   // namespace

bool auditParse(const string &text, AuditLine &out, string &error)
{
    out = AuditLine();

    string lerr;
    vector<Token> tokens = lex(text, lerr);
    if (!lerr.empty())
    {
        error = lerr;
        return false;
    }
    for (const Token &t : tokens)
    {
        if (t.type == Token::Op && t.text == "&")
            out.background = true;
    }

    NodePtr root;
    Parser p(tokens);
    if (!p.parse(root, lerr))
    {
        error = lerr;
        return false;
    }

    AuditWalker w(out.pipelines, error);
    w.visit(root);
    w.flush();
    return w.ok;
}

} // namespace jzsh
