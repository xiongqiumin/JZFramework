#ifndef JZ_SHELL_PARSER_H_
#define JZ_SHELL_PARSER_H_

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// JZShell —— 受限 shell 语法解析器(只做静态分析, 不执行任何东西)
//
// 为什么要有它: JZVirtualTerminal::run() 最终是把整条命令交给 `sh -c` 跑的,
// 所以"能不能跑"必须在交给 shell 之前判完。以前是靠字符串扫描 + 各种特判
// (找段首 token、抠重定向目标、判断 & 是不是后台), 每加一种语法就多一处
// 打补丁的地方。这里改成正经的两层: 词法 -> 语法树, 审核层只遍历树。
//
// 支持的语法:
//   逻辑链  cmd && cmd || cmd ; cmd
//   管道    cmd | cmd
//   重定向  > >> < 、带 fd 前缀(2> / 2>>)、fd 复制(2>&1 / >&-)、合并(&>file)
//   赋值前缀 FOO=bar cmd
//   引号/转义  '...' "..." \x 、# 行注释
//
// ⚠ 只识别结构, 不做变量/通配符展开(那是 sh 的事)。凡是"静态看不出值"的
//   地方, 都通过 Command::programExpanded / 报错暴露给审核层去拒。
// ⚠ 明确不支持(解析阶段直接报错, 不让它混过去):
//   命令替换 $( ) 与反引号、进程替换 <( ) >( )、heredoc <<、子shell ( )、
//   brace group { }。后台 & 会被解析成 Line::background 交给调用方拒绝。
//   for / while / if 等复合语句: 词法上就是普通 word, 由调用方按程序名拒。
// ---------------------------------------------------------------------------
namespace JZShell
{

// 一条重定向。例: `2>>a.log`、`3<in.txt`、`2>&1`、`&>all.txt`
struct Redirect
{
    int fd = -1;              // 左侧 fd(-1 = 用默认值: > 是 1, < 是 0)
    std::string op;           // ">" ">>" "<" ">&" "<&" "&>"
    std::string target;       // 目标; fdDup 时是 "1" / "-" 这类
    bool fdDup = false;       // true = `&N` / `&-`: 复制 fd, 不是文件
    bool read = false;        // 是输入重定向(<), 要审读权限而不是写权限
};

// 管道里的一节
struct Command
{
    std::vector<std::string> assign;   // 前面的 VAR=val(只作用于这一条命令)
    std::vector<std::string> argv;     // argv[0] 是程序名(已去掉引号, 未做展开)
    std::vector<Redirect> redirects;
    bool programExpanded = false;      // 程序名里带 $ ~ * ?, 静态审不出是哪个程序

    // argv[0] 在原命令串里的位置([pos, pos+len))。
    // 用途: 上层"按短名审过之后"要把短名换成全路径 —— 只能照这个位置原地替换,
    // 不能拿名字去字符串里硬找(那样 `echo tsc` 里的 tsc 会被误换成全路径)。
    // programLen == 0 表示位置未知(不该发生, 兜底用: 上层见到 0 就不替换)。
    std::size_t programPos = 0;
    std::size_t programLen = 0;
};

struct Pipeline
{
    std::vector<Command> commands;     // 至少 1 条
};

// 一整行。ops[i] 是 pipelines[i] 后面跟的连接符("&&" / "||" / ";" / 空)
struct Line
{
    std::vector<Pipeline> pipelines;
    std::vector<std::string> ops;
    bool background = false;           // 出现了 `cmd &`

    bool empty() const { return pipelines.empty(); }
};

// 照 AST 改命令串: 把每条命令的"程序名"换成 resolver 给的文本(返回空串 = 这条不换)。
//
// 位置是 AST 自己记的(Command::programPos/programLen, 词法阶段记的词跨度), 不是拿
// 名字在串里硬找 —— 所以 `echo tsc` 里出现在**参数位置**的 tsc 不会被误换。
//
// ⚠ 是"按 AST 定位, 在原文上打补丁", 不是"从 AST 重新生成命令串": 注释、多余空格,
//   以及 allowUnsafe 下整段跳过的 $( ) / heredoc / 子shell 内容都不在 AST 里,
//   重建会把它们弄丢(命令被悄悄改掉)。打补丁则只动程序名那几个字符。
std::string rewritePrograms(const std::string &text, const Line &line,
                            const std::function<std::string(const Command &cmd)> &resolver);

// 解析失败时 error 是能直接给人/模型看的中文原因。
// allowUnsafe: true 时不再把 heredoc/命令替换/进程替换/子shell 当成错误,
//   而是把它们的"内容"整段跳过(子shell/$( ) 内部的命令不会被抽出, 这是已知取舍)。
//   供"全部不检查(Unrestricted)"档用: 仍要解析出结构来跑安全底线, 但信任整条命令。
bool parse(const std::string &text, Line &out, std::string &error, bool allowUnsafe = false);

}  // namespace JZShell

#endif // !JZ_SHELL_PARSER_H_
