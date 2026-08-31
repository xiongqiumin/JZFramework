#ifndef JZ_CODE_TOOLS_H_
#define JZ_CODE_TOOLS_H_

#include <string>
#include <vector>

#include "JZAgentTool.h"
#include "JZProcess.h"
#include "JZTimer.h"

// ------------------------------------------------------------------
// 工作区路径工具（网页端文件浏览复用同一套越界校验，避免两处规则不一致）
// ------------------------------------------------------------------
bool codeIsSubPath(const std::string &path, const std::string &root);

// 把用户传入的（相对/绝对）路径解析为工作区内的绝对路径，越界返回 false
bool codeResolvePath(const std::string &workspace, const std::string &input,
                     std::string &absPath, std::string &error);

// 列出工作区某目录的直接子项（目录名以 '/' 结尾），rel 为空表示根目录
bool codeListDir(const std::string &workspace, const std::string &rel,
                 std::vector<std::string> &out, std::string &error);

// 读取工作区内文本文件（utf8 输出）
bool codeReadFile(const std::string &workspace, const std::string &rel,
                  std::string &text, std::string &error);

// ------------------------------------------------------------------
// 所有代码工具的基类：
//   - 统一管理工作区根目录与路径越界校验
//   - 统一的结果回传（finish）
//   - 统一的调用/返回日志信号（sigToolLog），供网页端展示执行过程
// ------------------------------------------------------------------
class JZCodeTool : public JZAgentTool
{
public:
    JZCodeTool();
    virtual ~JZCodeTool();

    void setWorkspace(const std::string &path);
    const std::string &workspace() const;

    bool resolvePath(const std::string &input, std::string &absPath, std::string &error) const;

    static ParamSchema makeParam(const std::string &name, ParamType type, const std::string &desc);

jz_signals:
    // (类别: "call" / "result", 工具名, 文本)
    JZSignal<const std::string &, const std::string &, const std::string &> sigToolLog;

protected:
    void finish(const ToolCall &call, bool ok, const std::string &text);
    void emitLog(const std::string &type, const std::string &text);

    std::string m_workspace;
};

// ------------------------------------------------------------------
// list_dir: 列出目录内容
// ------------------------------------------------------------------
class JZListDirTool : public JZCodeTool
{
public:
    JZListDirTool();
    virtual void call(const ToolCall &params) override;
};

// ------------------------------------------------------------------
// read_file: 读取文件（可按行号范围）
// ------------------------------------------------------------------
class JZReadFileTool : public JZCodeTool
{
public:
    JZReadFileTool();
    virtual void call(const ToolCall &params) override;
};

// ------------------------------------------------------------------
// search_code: 在工作区内搜索文本 / 正则
// ------------------------------------------------------------------
class JZSearchCodeTool : public JZCodeTool
{
public:
    JZSearchCodeTool();
    virtual void call(const ToolCall &params) override;
};

// ------------------------------------------------------------------
// modify_code: 修改（或创建）文件
//   old_str 为空  -> 用 new_str 覆盖整个文件（新建/重写）
//   old_str 非空  -> 精确替换，默认要求唯一匹配
// ------------------------------------------------------------------
class JZModifyCodeTool : public JZCodeTool
{
public:
    JZModifyCodeTool();
    virtual void call(const ToolCall &params) override;
};

// ------------------------------------------------------------------
// build / run: 执行外部命令（编译、运行程序）
// 两者实现完全一致，只是默认命令、描述与超时不同
// ------------------------------------------------------------------
class JZCommandTool : public JZCodeTool
{
public:
    JZCommandTool();
    virtual ~JZCommandTool();

    void setup(const std::string &name, const std::string &desc,
               const std::string &defaultCommand, int timeoutMs);
    void setDefaultCommand(const std::string &cmd);
    const std::string &defaultCommand() const;

    virtual void call(const ToolCall &params) override;

    // 停止所有仍在运行的子进程（用户点了"停止"时调用）
    void stopAll();

protected:
    struct Task
    {
        std::string id;
        JZProcess *proc = nullptr;
        JZTimer *timer = nullptr;
        int64_t startTime = 0;
        bool timeout = false;
    };

    void onProcFinished(JZProcess *proc, const std::string &name, const std::string &id,
                        int exitCode, JZProcess::ExitStatus status);
    void removeTask(JZProcess *proc);

    std::string m_defaultCommand;
    int m_timeoutMs = 120000;
    std::vector<Task> m_tasks;
};

#endif // !JZ_CODE_TOOLS_H_
