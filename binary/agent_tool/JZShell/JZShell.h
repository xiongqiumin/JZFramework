#ifndef JZ_SHELL_H_
#define JZ_SHELL_H_

#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "JZObject.h"
#include "JZShellPolicy.h"   // 审核引擎(白名单/区域/危险参数/安全底线)归本类所有

// ---------------------------------------------------------------------------
// JZShell —— 自己实现的 shell(解释器), 不依赖 bash, 也不需要 PTY
//
// 为什么要自己写: bash 不提供"程序可用的接口"——没有命令边界、拿不到每条命令的
// 退出码、cwd/env 是它自己的内部状态。包一层 PTY 只能补边界, 补不了"状态归谁"。
// 自己解释执行, 这些信息天然就是我们的。
//
// 分工(**审核在这里, 配策略的在外面**):
//   · 本类负责"解析 + 展开 + 执行 + 状态"(cwd/env/退出码), 并持有一份
//     JZShellPolicy(见 policy()) —— 白名单 / 区域 / 危险参数 / 安全底线都在那里;
//   · 宿主(JZVirtualTerminal / CLI)只负责把策略配好(工作区、权限档、程序映射),
//     再用 policy() 做**执行前**的审核 —— 它看到的是解析后的 argv, 比文本级更准。
//
// 支持的语法(明确子集, 遇到不支持的**直接报错并给替代写法**, 不静默):
//   简单命令 / 赋值前缀 FOO=1 cmd / 引号('...' "..." \\x) / # 注释
//   $VAR、${VAR}、${VAR:-默认}、$?、$1..$9、$#、$@ / ~ / 通配符 * ? [ ]
//   $( ) 命令替换(可嵌套) / $(( )) 整数算术 / (( )) 算术命令(返回 status)
//   管道 |(两边的每一段都可以是复合命令) / 逻辑 && || / `!` 前缀 / 串联 ;
//   重定向: > >> < 2> 2>> 2>&1 &> <<heredoc <<- <<<here-string
//     —— 既可挂在简单命令上, 也可挂在复合命令后面(`{ ...; } > f` / `fi 2>&1` / `done >> log`)
//   if/then/elif/else/fi、for ... in ...、for ((;;))、while、until ... do ... done
//   case ... in 模式) ... ;; esac(模式可带前导 `(`; 支持 | 与通配)
//   函数 f() { ... } 与调用($1..$#、local、return) / 子 shell ( ... ) / 分组 { ...; }
//   执行 #! 脚本文件(用本解释器跑, 不再交给系统 bash)
//   内建: cd pwd export unset echo printf exit true false test [ : local return
//
// 不支持(明确报错): 反引号 / 数组与关联数组 / [[ ]] / 进程替换 <( ) >( ) /
//   alias 定义 / source(可给替代写法) / & 后台(当语句分隔符, 仍是同步执行) / 作业控制
//   (参考实现是 bash, 但只做 POSIX + 上面这些常用扩展; 其它 shell 的方言一律不支持)
//
// ⚠ 执行是**同步**的: exec() 内部起进程并等到结束(waitForFinished)。给 agent 用时
//   要放到工作线程或后续改成异步; 目前适合 CLI / 测试 / 单线程脚本场景。
// ---------------------------------------------------------------------------

// 语法树。放在类外是为了让 .cpp 里的解析器(匿名命名空间)也能用 —— 它是纯数据,
// 不暴露在公开接口里, 只通过 JZShell 的别名使用。
namespace JZShellAst
{
struct Word
{
    std::string raw;          // 原文(保留引号/转义, 展开时再处理)
};

struct Redirect
{
    int fd = -1;              // -1 = 按 op 取默认(> 1, < 0)
    std::string op;           // ">" ">>" "<" "2>" "2>>" "2>&1" "&>"
    Word target;
    // heredoc 用: 定界符被引号/反斜杠包过(`<<'EOF'`)时正文不展开变量与命令替换
    bool noExpand = false;
};

struct SimpleCmd
{
    std::vector<Word> assigns;      // FOO=bar 前缀
    // 数组赋值 `a=(w1 w2 ...)` / `a+=(...)`: 名字(带 '+' 前缀表示追加) + 各元素原文
    std::vector<std::pair<std::string, std::vector<Word>>> arrayAssigns;
    std::vector<Word> argv;         // 程序名 + 参数(未展开)
    std::vector<Redirect> redirects;
};

struct Node;
typedef std::shared_ptr<Node> NodePtr;

struct Node
{
    enum Kind
    {
        Simple = 0,     // cmd: 一条简单命令(赋值前缀 + argv + 重定向)
        Pipeline,       // stages: 每段是**任意**命令(简单命令, 或 if/for/while/{...}/(...) 这类复合命令)
        AndOr,          // parts + ops("&&" / "||")
        If,             // conds[i] / bodies[i] / elseBody
        For,            // var + words + body
        While,          // cond + body
        Function,       // var = 函数名, body = 函数体
        Subshell,       // body: ( ... ) —— 状态改动不泄漏
        Case,           // var = 主语, words[i] = 分支模式(用 | 分隔), bodies[i] = 分支体
        Arith,          // var = 算术表达式原文: (( expr )) —— 状态 = (expr != 0) ? 0 : 1
        ForArith,       // var = "init; cond; step" 原文, body = 循环体
        // [[ ... ]]: parts/ops 是 && || 链; var 是二元运算符; words 是操作数(1 或 2 个);
        // negate = `!`; cond = 括号分组里的子条件
        Cond,
        Group,          // body: { ...; } —— 在当前 shell 里执行(不像子 shell 那样隔离)
    };

    Kind kind = Simple;
    int line = 0;                         // 语句起始行($LINENO / DEBUG trap)
    bool negate = false;                  // 前面有 `!`: 退出码取反
    bool until = false;                   // While 节点: true = `until`(条件为假时继续)
    bool selectLoop = false;              // For 节点: true = `select`(从 stdin 取词)
    bool forNoIn = false;                 // For 节点: 没写 `in` —— 遍历 "$@"(bash 的行为)
    SimpleCmd cmd;                        // Simple
    std::vector<NodePtr> stages;          // Pipeline
    // 复合命令自己的重定向: `{ ...; } > f` / `if ...; fi 2>&1` / `while ...; done >> log`
    // (简单命令的重定向在 SimpleCmd::redirects 里, 由 runSimple 处理)
    std::vector<Redirect> redirects;
    std::vector<NodePtr> parts;           // AndOr
    std::vector<std::string> ops;         // AndOr
    std::vector<NodePtr> conds;           // If
    std::vector<NodePtr> bodies;          // If / Case
    NodePtr elseBody;                     // If
    std::string var;                      // For / Case / Arith
    std::vector<Word> words;              // For
    NodePtr cond;                         // While
    NodePtr body;                         // For / While
};
} // namespace JZShellAst

// ---------------------------------------------------------------------------
// 实现内部件（命名空间 jzsh）
//
// 词法(JZShellLex.cpp) / 语法(JZShellParser.cpp) / 主体(JZShell.cpp) 三个 TU
// 之间必须共享的东西：字符分类、Token、lex()、Parser 类体。
// 拆文件之前它们都在 JZShell.cpp 的匿名命名空间里；现在放在这份头里，
// 但**不属于**对外接口 —— 宿主只应该用下面 class JZShell 的公开成员。
// ---------------------------------------------------------------------------
namespace jzsh
{
using namespace std;
// 语法树类型定义在 JZShellAst 里(见上), 这里整体拉进 jzsh:
// 三个 TU(词法/语法/主体)里直接写 Node / NodePtr / SimpleCmd, 不必到处加 JZShellAst::。
using JZShellAst::Node;
using JZShellAst::NodePtr;
using JZShellAst::SimpleCmd;
using JZShellAst::Redirect;
using JZShellAst::Word;

// ---- 字符分类 / 赋值前缀（JZShellLex.cpp）----
bool isSpaceChar(char c);
bool isNameStart(char c);
bool isNameChar(char c);
bool isAssignmentWord(const string &w);

// ---- 词法（JZShellLex.cpp）----
struct Token
{
    enum Type
    {
        Word = 0,
        Op,
        Arith,   // `(( ... ))` 整段(内部不按 shell 运算符切)
        Heredoc, // heredoc 正文(`<< WORD` 之后整段收进来)
        Eof
    };
    // 显式构造函数: MSVC 在 out.push_back({ Token::Op, "2>" }) 这类调用里
    // 不接受用 initializer-list 隐式构造成员带默认初值的聚合体。给一条构造路径后,
    // 这些调用与 Token{...} / 默认构造都保持原语义。
    Token(Type t = Eof, const string &txt = string(), int ln = 0)
        : type(t), text(txt), line(ln) {}
    Type type = Eof;
    string text;
    int line = 0;      // 所在行(从 1 开始): $LINENO / DEBUG trap 要用
};

bool isOpStart(char c);
size_t findArithClose(const string &s, size_t i);
size_t consumeDollarParen(const string &s, size_t i, string &inner);
vector<Token> lex(const string &s, string &error);
// 去掉一个词里的引号与转义(不展开 $ / 通配): heredoc 定界符比对、审核视图共用
string unquoteWord(const string &w);

// ---- 语法分析（JZShellParser.cpp）----
class Parser
{
public:
    Parser(const vector<Token> &tokens) : m_t(tokens) {}

    bool parse(NodePtr &out, string &error);
    // 解析失败的行号(用于 `$0: line N: ...` 这种前缀); 成功时为 0
    int failLine() const { return m_failLine; }
    int failedLine() const;

private:
    const Token &peek() const { return m_t[m_pos]; }
    const Token &next() { return m_t[m_pos++]; }
    bool isWord(const char *w) const { return peek().type == Token::Word && peek().text == w; }
    bool isOp(const char *o) const { return peek().type == Token::Op && peek().text == o; }
    bool isArith() const { return peek().type == Token::Arith; }
    bool acceptOp(const char *o)
    {
        if (isOp(o))
        {
            m_pos++;
            return true;
        }
        return false;
    }
    // bash 的两条标准语法错误文本(见 bash 的 parse.y / y.tab.c 里 yyerror 的用法):
    //   syntax error near unexpected token `X'
    //   syntax error: unexpected end of file
    // 词法里换行是 ';', 报错要按 bash 说 `newline`。
    std::string syntaxNear() const
    {
        if (peek().type == Token::Eof)
            return "syntax error: unexpected end of file";
        string t = peek().text;
        if (t == ";" || t == "\n")
            t = "newline";
        if (t.empty())
            t = "newline";
        return "syntax error near unexpected token `" + t + "'";
    }
    // `unexpected EOF while looking for matching \`X'`
    static std::string syntaxUnclosed(const char *what)
    {
        return std::string("unexpected EOF while looking for matching `") + what + "'";
    }

    // 跳过换行 token: bash 允许 `cmd &&` / `cmd |` 后面换行再写下一段
    // (`[ -z "$x" ] &&\n{ ... }` 这种写法在 bash 测试里很常见)
    // ⚠ 词法里**换行就是 ';'**(lexer 不做 '\n' token), 所以这里连 ';' 一起跳;
    //   只在 && || | 之后调用, 代价是 `a && ; b` 这种写法我们不会报错(bash 会)。
    void skipNewlines()
    {
        while (isOp("\n") || isOp(";"))
            m_pos++;
    }
    bool expectWord(const char *w, string &error)
    {
        if (!isWord(w))
        {
            error = syntaxNear();   // `then`/`fi`/`done` 少了: bash 也是报 unexpected token
            return false;
        }
        m_pos++;
        return true;
    }

    bool parseList(NodePtr &out, const set<string> &stops, string &error);
    bool parseAndOr(NodePtr &out, const set<string> &stops, string &error);
    bool parsePipeline(NodePtr &out, string &error);
    bool parsePipelineInner(NodePtr &out, string &error);
    bool parseCompound(NodePtr &out, string &error);   // `{ ...; }`
    bool parseCommand(NodePtr &out, string &error);
    bool parseSimple(NodePtr &out, string &error);
    bool parseIf(NodePtr &out, string &error);
    bool parseFor(NodePtr &out, string &error);
    bool parseWhile(NodePtr &out, string &error);
    bool parseLoopBody(NodePtr &out, string &error);   // `; do ... done` 或 `; { ... }`
    bool parseFunction(NodePtr &out, string &error);
    bool parseSubshell(NodePtr &out, string &error);
    bool parseCase(NodePtr &out, string &error);

    // [[ ... ]] 条件表达式: or -> and -> not -> primary
    bool parseCond(NodePtr &out, string &error);
    bool parseCondOr(NodePtr &out, string &error);
    bool parseCondAnd(NodePtr &out, string &error);
    bool parseCondTerm(NodePtr &out, string &error);

    // 收集紧随其后的重定向(`> >> < 2> 2>> 2>&1 &> << <<- <<<`), 简单命令和复合命令共用。
    // 复合命令用它是为了支持 `{ ...; } > f` / `if ...; fi 2>&1` / `while ...; done >> log`。
    bool parseRedirects(vector<Redirect> &out, string &error);

    // `name ( )` 的形式(函数定义), 只看三个 token, 不改位置。
    // ⚠ 名字不要求是标识符: 非 POSIX 模式下 bash 允许 `fu%nc() { ... }`
    //   (bash tests 的 cprint 就靠这个), 所以这里只要求"一个词 + ( + )"。
    bool looksLikeFunctionDef() const
    {
        if (peek().type != Token::Word || peek().text.empty())
            return false;
        // 保留字不能当函数名: `for()` 是语法错误(bash: unexpected token `('),
        // 不能走函数定义那条路
        const string &nm = peek().text;
        if (nm == "for" || nm == "while" || nm == "until" || nm == "if" || nm == "case" ||
            nm == "do" || nm == "done" || nm == "then" || nm == "else" || nm == "elif" ||
            nm == "fi" || nm == "esac" || nm == "in" || nm == "select" || nm == "function")
            return false;
        return m_pos + 2 < m_t.size() &&
               m_t[m_pos + 1].type == Token::Op && m_t[m_pos + 1].text == "(" &&
               m_t[m_pos + 2].type == Token::Op && m_t[m_pos + 2].text == ")";
    }

    const vector<Token> &m_t;
    size_t m_pos = 0;
    int m_failLine = 0;   // 见 failLine()
    bool m_until = false;    // 下一个 while 是 `until`(条件取反)
};

} // namespace jzsh

class JZShell : public JZObject
{
public:
    struct Result
    {
        bool ok = false;          // exitCode == 0 且没有语法/启动错误
        int exitCode = -1;        // 命令退出码(内建命令由我们自己给)
        std::string output;      // 标准输出(2>&1 时含 stderr)
        std::string error;       // 语法/启动/不支持 的错误(不是命令自己的 stderr)
        std::string cwd;          // 执行完之后的工作目录
    };

    explicit JZShell(JZObject *parent = nullptr);
    ~JZShell() override;

    // 初始工作目录。空 = 继承当前进程 cwd。失败返回 false(目录不存在)。
    bool setCwd(const std::string &dir, std::string *error = nullptr);
    std::string cwd() const { return m_cwd; }

    // 环境变量(叠加在本进程环境之上; exec 时**只**用于命令替换/PATH 解析,
    // 子进程的环境暂时沿用本进程的 —— JZProcess 还没有 setEnvironment)。
    void setEnvVar(const std::string &name, const std::string &value);
    void unsetEnvVar(const std::string &name);
    std::string envVar(const std::string &name) const;
    std::map<std::string, std::string> envAll() const;

    // 额外的程序搜索目录(前插, 优先于 PATH)。Windows 上通常加 Git 的 usr/bin。
    void addPathDir(const std::string &dir);
    std::vector<std::string> pathDirs() const { return m_pathDirs; }

    // 短程序名 -> 可执行文件全路径(显式映射, 优先于目录搜索)。
    // 上层(终端/宿主)把"打包进来的编译器"这类东西登记成短名, 命令里照常写 `tsc -p .`。
    void addProgramPath(const std::string &name, const std::string &fullPath);
    std::string programPath(const std::string &name) const;

    // ---- 审核引擎 ----
    // 白名单 / 区域 / 危险参数 / 安全底线都在这份 policy 里(实现见 JZShellPolicy.h)。
    // 宿主要审核就写 `shell->policy().checkWrite(path)`; 终端的同名接口只是转发过来。
    // ⚠ 配置(setConfig/addProgram)请在开跑之前做完: policy 不加锁, 运行期按只读用。
    JZShellPolicy &policy() { return m_policy; }
    const JZShellPolicy &policy() const { return m_policy; }

    // 单条外部命令的超时(毫秒, <=0 = 不限)。默认 15 分钟。
    void setCommandTimeoutMs(int ms) { m_timeoutMs = ms; }
    int commandTimeoutMs() const { return m_timeoutMs; }

    // 执行一整行(可以是 ; && || | if/for/while 的组合)。同步, 阻塞到结束。
    Result exec(const std::string &line);

    // 协作式取消: 置位后, 正在等待的外部命令会被 kill, exec 以 130 返回。
    // 只能从**另一个线程**调(exec 是阻塞的); 同线程里想停只能等它自己结束。
    // ⚠ 必须是原子量: exec 线程在读(100ms 分片), 宿主线程在写(取消/析构)。
    void requestCancel() { m_cancelRequested.store(true); }
    bool cancelRequested() const { return m_cancelRequested.load(); }

    // 只做语法检查(不执行), 给上层做"先审再跑"或自检用。
    bool parseOnly(const std::string &line, std::string *error) const;

    int lastStatus() const { return m_lastStatus; }

    // 错误信息前缀, 与 bash 的两种形态对应:
    //   -c 模式:   `$0: -c: line N: <msg>`   ($0 = `-c 命令名` 里那个名字)
    //   脚本模式:  `<脚本路径>: line N: <msg>`
    // 都不设 = 不加前缀(纯库用法保持原样)。
    void setErrorModeCommand() { m_errMode = 1; }
    void setErrorModeScript(const std::string &path) { m_errMode = 2; m_errScript = path; }
    // 位置参数($0 在 [0])。-c 模式对应 `-c 命令 $0 $1 ...` 里那几个额外参数。
    void setArgs(const std::vector<std::string> &args) { m_params = args; }
    // `set -o posix` / `jz_shell -o posix`: 收窄 tilde 展开等
    void setPosix(bool on) { m_posix = on; }
    bool posix() const { return m_posix; }
    // `shopt -s extdebug` / `jz_shell -O extdebug`
    void setExtdebug(bool on) { m_extdebug = on; }
    // 脚本来自 stdin 时置 true: 里面起的命令继承我们的 stdin(见 m_inheritStdin)
    void setInheritStdin(bool on) { m_inheritStdin = on; }
    // 直写模式(bash 的做法): 结果不再攒成字符串, 而是每到语句边界就写进 fd 1 / fd 2;
    // 外部命令也直接继承我们的 fd。这样父子进程的 stdout/stderr 顺序由内核保证,
    // 而不是"整块 stdout + 整块 stderr"拼起来。命令行程序(jz_shell)用它;
    // 需要拿回文本的宿主(虚拟终端/agent 工具)不要开。
    void setDirectOutput(bool on) { m_directOutput = on; }
    bool directOutput() const { return m_directOutput; }

private:
    typedef JZShellAst::Word Word;
    typedef JZShellAst::Redirect Redirect;
    typedef JZShellAst::SimpleCmd SimpleCmd;
    typedef JZShellAst::Node Node;
    typedef JZShellAst::NodePtr NodePtr;

    struct StageResult
    {
        int status = 0;
        std::string out;
        std::string err;
        bool failed = false;      // 语法/启动/不支持 —— 不是普通非零退出
        std::string failReason;
    };

    // ---- 展开 / 查找 ----
    // 非 const: `$( )` 要在展开阶段真的跑一遍内层命令
    // splitFields=false 用于赋值右侧(FOO=$(...) / FOO=a b 都不分字段)
    // globbing=false 用于 `case` 的模式: 只做变量/引号展开, **不做路径展开**
    // (`case x in *)` 里的 `*` 是通配模式, 展开成当前目录文件名就永远匹配不上了)
    // tildeAssign: 这个词是**赋值词**(`NAME=~/x` / `export NAME=~/x`) —— 只有这种
    // 上下文里 `~` 才在 `=` 和每个 `:` 之后展开; 普通参数(`echo foo=bar:~`)不展开。
    std::vector<std::string> expandWord(const Word &w, std::string *error,
                                       bool splitFields = true, bool globbing = true,
                                       bool tildeAssign = false);
    std::string expandString(const std::string &s);    // 用于 target
    std::string expandPattern(const std::string &s);   // 用于 case 的模式
    // `${...}` 的求值(不含 `${` `}`): 支持 a[i] / a[@] / a[*] / ${#a} / ${#a[@]}。
    // 返回 false = 不认识这种写法, 调用方按"普通变量名"处理。
    bool expandBraced(const std::string &expr, std::vector<std::string> &out);
    // 变量取值: ? $ # @ * / 位置参数 / 环境 / 数组首元素。展开与 ${...} 求值共用一份规则。
    // ⚠ 不是 const: bash 的展开本身有副作用(${a[i++]} / $((x=1)) / 动态变量),
    //   而且 typeset -i 的数组元素要按算术求值(arithEval 非 const)。
    std::string varValue(const std::string &name);
    // bash 的"动态变量"($BASHPID / $SECONDS / $EPOCHSECONDS / $EPOCHREALTIME /
    // $BASH_ARGV0 / $BASH_COMMAND): 取值时现场算, 赋值时改的是**基准**而不是值。
    // found=false 表示这不是动态变量, 调用方按普通变量继续查。
    std::string dynamicVar(const std::string &name, bool &found) const;
    // 给变量赋值。所有"持久赋值"的路径都走这里: 动态变量就地改基准, 其余落 m_env。
    void storeVar(const std::string &name, const std::string &value);
    // $0: BASH_ARGV0 赋过值就用它, 否则用位置参数里的第 0 个
    std::string argv0() const;
    // 错误前缀; m_errMode==0 时返回空串。
    //   parseError=true : `$0: -c: line N: `(bash 的语法错误带 -c 标记)
    //   parseError=false: `$0: line N: `(运行期错误, 例如 command not found)
    //   脚本模式下两者都是 `<脚本>: line N: `
    std::string errorPrefix(int line, bool parseError = true) const;
    // 数组元素的读取值: `typeset -i` 的数组按算术求值, 其余原样返回
    std::string arrayElemValue(const std::string &base, const std::string &raw);
    // heredoc 正文展开(定界符没加引号时): `$var` `${...}` `$( )` `$(( ))` 与反引号,
    // 以及 `\$` `\`` `\\` 三种反斜杠转义; 引号在正文里是**普通字符**(bash 的规则)。
    std::string expandHeredocBody(const std::string &body);
    // 跑一整段文本并只取它的 stdout(去掉尾部换行), 期间的状态改动全部回滚 = 子 shell 语义
    bool captureCommand(const std::string &text, std::string &out, std::string *error);
    // $(( )) 整数算术求值
    bool arithEval(const std::string &expr, long long &out, std::string *error);
    // (( )) / for (( )) 用: 允许赋值与自增自减(i=0、i++、i+=2、j=1,k=2), 写回变量表
    bool arithRun(const std::string &expr, long long &out, std::string *error);
    std::string resolveProgram(const std::string &name) const;
    std::string absolutePath(const std::string &p) const;

    // ---- 执行 ----
    StageResult runNode(const NodePtr &n);
    // 管道: 每一段可以是简单命令, 也可以是复合命令(if/for/while/{ }/( ))
    StageResult runPipeline(const std::vector<NodePtr> &stages);
    StageResult runSimple(const SimpleCmd &cmd, const std::string &stdinText);
    // 全是"外部命令"的管道走**真管道**(JZProcess::setStandardOutputProcess):
    // 上游边跑边写, 下游边读边跑, 数据不进内存。`yes | head` 这类只有真管道才不卡。
    // 不能走(含内建/函数/别名/重定向/赋值/复合命令)时返回 false, 调用方退回字符串管道。
    bool tryRunExternalPipe(const std::vector<NodePtr> &stages, StageResult &r);
    // 复合命令上的重定向(`fi > f` / `done 2>&1` / `} < in`): 输入在跑之前准备好,
    // 输出在跑完之后落盘 —— 与 runSimple 里那套语义一致(见 runSimple 第 4/8 步)。
    bool prepareNodeInput(const NodePtr &n, StageResult &r);
    void flushNodeOutputs(const NodePtr &n, StageResult &r);
    StageResult runBuiltin(const std::string &name, const std::vector<std::string> &args,
                           const std::string &stdinText, bool *handled);
    StageResult runFunction(const std::string &name, const std::vector<std::string> &args);
    // 执行一个 shell 脚本文件(用本解释器跑, 不依赖系统 bash)
    StageResult runScriptFile(const std::string &path, const std::vector<std::string> &argv);
    StageResult runExternal(const std::string &program, const std::vector<std::string> &args,
                            const std::string &stdinText, bool merged, bool inheritOutput = false);
    // [[ ... ]] 求值(短路 && ||)
    bool condEval(const NodePtr &n, bool &out, std::string &error);
    bool applyRedirects(const SimpleCmd &cmd, const std::string &outText,
                        const std::string &errText, std::string *keptOut, std::string *keptErr);

    std::string m_cwd;
    std::map<std::string, std::string> m_env;      // 覆盖(叠在进程环境上)
    // 数组变量: `a=(x y)` 之后 a[0]=x ...。`$a` / `${a}` 取 a[0]; 与 m_env 互斥同名。
    std::map<std::string, std::vector<std::string>> m_arrays;
    std::vector<std::string> m_pathDirs;           // 额外搜索目录
    std::map<std::string, std::string> m_programPaths;   // 短名 -> 全路径(见 addProgramPath)
    JZShellPolicy m_policy;                        // 审核引擎(见 policy())
    std::map<std::string, std::string> m_aliases;  // alias 名 -> 展开文本(见 runSimple 的展开)
    std::vector<std::string> m_dirStack;           // pushd/popd 的目录栈(dirs 展示)
    int m_timeoutMs = 15 * 60 * 1000;
    int m_lastStatus = 0;
    bool m_exiting = false;
    int m_exitCode = 0;
    int m_depth = 0;                                // 递归深度保护
    // 管道里"上一段的 stdout", 等复合命令段内部的**第一条**要读输入的命令消费掉。
    // 本工程是同步字符串管道(`echo a | ...`), 没有真 fd, 只能这样近似传递。
    std::string m_stageIn;
    std::atomic<bool> m_cancelRequested{ false };    // 见 requestCancel()

    // ---- 函数 ----
    struct LocalSave
    {
        std::string name;
        bool had = false;
        std::string value;
    };
    std::map<std::string, NodePtr> m_functions;    // 函数名 -> 函数体
    std::vector<std::string> m_params;             // $0 $1 $2 ...(函数/脚本位置参数)
    std::vector<LocalSave> m_locals;               // local 声明的保存栈
    bool m_returning = false;                      // 正在执行 return
    int m_returnCode = 0;

    // ---- 动态变量(见 dynamicVar) ----
    int m_bashPid = 0;               // $BASHPID: 子 shell / $( ) 里递增, 真进程没变
    long long m_startMsec = 0;       // 构造时刻(steady clock, 毫秒) —— $SECONDS 的零点
    long long m_secondsBase = 0;     // SECONDS 被赋值时记下的基准
    long long m_epochBase = 0;       // EPOCHSECONDS 的偏移(秒)
    long long m_epochRealBase = 0;   // EPOCHREALTIME 的偏移(微秒)
    std::string m_argv0;             // BASH_ARGV0(空 = 沿用 $0)
    std::string m_currentCmd;        // BASH_COMMAND: 正在执行的简单命令原文(未展开)
    int m_lineno = 1;                // $LINENO: 当前正在执行的语句行
    bool m_posix = false;            // `set -o posix`: 收窄 tilde 展开的位置
    // break/continue: 记层数, 每层循环消耗一层(>1 就继续往上传)
    int m_breakLevel = 0;
    int m_continueLevel = 0;
    // 脚本是从 stdin 读进来的时候: 里面起的命令要继承我们的 stdin,
    // 才能接着读脚本剩下的内容(bash 的行为, 见 input-test)
    bool m_inheritStdin = false;
    // 直写模式: 输出按语句边界直接写 fd, 外部命令继承我们的 fd(见 setDirectOutput)
    bool m_directOutput = false;
    // >0 表示当前输出会被当成**字符串**用(`$( )` / 管道 / 重定向), 这时不能直写、不能继承
    int m_captureLevel = 0;
    // 把 r 里攒的文本立刻写出去并清空(只在直写模式且不在捕获上下文里时生效)
    void flushDirect(StageResult &r);
    // `read` 的待消费输入: 管道/heredoc 传进来的数据可能有多行, 一次 read 只吃一行,
    // 剩下的留在这里给下一次 read(否则 `while read x; do ... done` 只跑一轮)
    std::string m_inBuf;
    bool m_pipefail = false;   // `set -o pipefail`: 管道退出码取最后一个非 0
    bool m_lastpipe = false;   // `shopt -s lastpipe`: 管道最后一段在当前 shell 里跑
    // `declare -i` 标记过的变量: 赋值时按算术求值(`tot+=$n` 才是加, 不是字符串拼接)
    std::set<std::string> m_intVars;
    int m_errMode = 0;               // 0=不加前缀 1=-c 模式 2=脚本模式(见 setErrorMode*)
    std::string m_errScript;         // 脚本模式下的脚本路径(前缀用)
    // ---- DEBUG trap(`trap '...' DEBUG`) ----
    std::string m_trapDebug;         // 动作原文(空 = 没装)
    bool m_inTrap = false;           // 正在跑 trap 动作: 防递归
    bool m_extdebug = false;         // shopt -s extdebug: trap 返回 2 就跳过该命令
    std::vector<std::string> m_funcStack;   // 函数调用栈(内层在末尾)
    // 跑一次 DEBUG trap 动作; targetLine = 即将执行的那条命令的行号
    StageResult runDebugTrap(int targetLine);
    // 把 m_funcStack 同步成 $FUNCNAME 数组(FUNCNAME[0] 是当前函数, 栈底是 main)
    void syncFuncName();
};

#endif // !JZ_SHELL_H_
