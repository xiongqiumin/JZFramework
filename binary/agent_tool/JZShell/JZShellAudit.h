#ifndef JZ_SHELL_AUDIT_H_
#define JZ_SHELL_AUDIT_H_

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 审核视图: 由**解释器自己的解析器**产出的"执行前可审"的结构
//
// 为什么还要这一层: 审核方(终端/策略)要的是"按执行顺序排好的简单命令 + 重定向",
// 不关心 AST 形状。所以这里只是 jzsh::Parser 的结果再压平一层, **没有第二套词法/语法**。
//
// 口径(与解释器一致):
//   * 引号/转义已去掉, **不做 $ / 通配展开** —— 所以程序名里带 `$ ~ * ?` 时审不出是哪个
//     程序, 由 programExpanded 标出来, 上层拒绝;
//   * heredoc / here-string 的正文不当命令审(它就是数据), 直接从重定向里滤掉;
//   * 复合命令(if/for/while/case/{...}/(...))会被**展开**成内部的命令按执行顺序排进来 ——
//     比旧版更严(旧版只看到 `for` 这个词, 直接按程序名拒)。
// ---------------------------------------------------------------------------
namespace jzsh
{

// 一条重定向(op 是解释器词法里的写法: ">", ">>", "<", "2>", "2>>", "2>&1", "&>")
struct AuditRedirect
{
    int fd = -1;              // 左侧 fd(-1 = 用默认值: > 是 1, < 是 0)
    std::string op;
    std::string target;       // 已去引号、未展开
    bool fdDup = false;       // `2>&1`: 复制 fd, 不碰文件
    bool read = false;        // 输入重定向
};

// 管道里的一节
struct AuditCommand
{
    std::vector<std::string> argv;     // argv[0] 是程序名(已去引号, 未展开)
    std::vector<AuditRedirect> redirects;
    bool programExpanded = false;      // 程序名里带 $ ~ * ?: 静态审不出是哪个程序
};

struct AuditPipeline
{
    std::vector<AuditCommand> commands;   // 至少 1 条
};

// 一整行: pipelines 按**执行顺序**排列(逻辑链/复合命令都已展开)
struct AuditLine
{
    std::vector<AuditPipeline> pipelines;
    bool background = false;              // 出现了 `cmd &`
    bool empty() const { return pipelines.empty(); }
};

// 解析成审核视图。返回 false 时 error 是能直接给人/模型看的原因
// (目前只有一类: 重定向目标带 $ ~ * ?, 静态审不出读写哪个路径)。
bool auditParse(const std::string &text, AuditLine &out, std::string &error);

}   // namespace jzsh

#endif // !JZ_SHELL_AUDIT_H_
