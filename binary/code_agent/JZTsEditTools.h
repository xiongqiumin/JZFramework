#ifndef JZ_TS_EDIT_TOOLS_H_
#define JZ_TS_EDIT_TOOLS_H_

#include <functional>
#include <memory>
#include <string>

#include "JZCodeTools.h"
#include "JZTsLangServer.h"

// 结构化 TS 编辑工具组：以「符号」为编辑单元的大模型主力改码通道。
//
// 定位方式与查询工具组一致：file + 符号名（scope 支持 "file" 或 "file#Class" 两级），
// 符号的行范围由 TSGO documentSymbol 保证语义精确；行级插入/替换/删除在本地文本上完成，
// rename 走 LSP textDocument/rename 实现全项目引用同步。
// 与 modify_file（old_str/new_str 精确文本匹配）互补：改类/函数优先用本组工具，
// 不受缩进、换行、上下文拼接错误的影响。
class JZTsEditTool : public JZCodeTool
{
public:
    enum class Kind
    {
        Summary,         // ts_summary：工程级总览（工作区文件+顶层符号；file/class 级由查询组覆盖）
        AddClass,        // ts_add_class：文件末尾插入类
        RenameClass,     // ts_rename_class：语义重命名（全项目引用）
        DeleteClass,     // ts_delete_class：删除整个类
        AddFunction,     // ts_add_function：顶层函数 / 类方法
        RenameFunction,  // ts_rename_function
        ReplaceFunction, // ts_replace_function：整体替换函数/方法声明
        DeleteFunction   // ts_delete_function
    };

    // 宿主用于写路径权限校验（可空：测试/无宿主环境跳过校验）。
    // 传 ICodeToolHost 而不是 JZCodeAgent —— 工具库里不认识 code_agent 这个具体类。
    JZTsEditTool(JZTsLangServer *server, Kind kind, ICodeToolHost *host = nullptr);
    virtual void call(const ToolCall &params) override;

private:
    // ---- 公共步骤 ----
    // 拉取文件归一化符号树（JZTsLangServer::request(Outline)，1-based 行号）
    void requestOutline(const std::string &file,
                        std::function<void(bool ok, const Json::Value &tree)> onTree);
    // scope 解析："a/b.ts" -> (a/b.ts, "")；"a/b.ts#Calc" -> (a/b.ts, Calc)
    static void parseScope(const std::string &scope, std::string &file, std::string &cls);
    DenyReason checkWrite(const std::string &abs);
    void finishEdit(const ToolCall &params, bool ok, const std::string &msg,
                    DenyReason deny = DenyReason::None);

    // ---- 各操作 ----
    void doSummary(const ToolCall &params, const Json::Value &p);
    void doAddClass(const ToolCall &params, const Json::Value &p);
    void doAddFunction(const ToolCall &params, const Json::Value &p);
    void doReplaceFunction(const ToolCall &params, const Json::Value &p);
    void doDelete(const ToolCall &params, const Json::Value &p, bool isClass);
    void doRename(const ToolCall &params, const Json::Value &p, bool isClass);

    // kind=project 摘要的异步链状态（逐文件 documentSymbol，避免 shared_ptr 自引用泄漏）
    struct SummaryState;
    void summaryStep(std::shared_ptr<SummaryState> st);

private:
    JZTsLangServer *m_server = nullptr;
    Kind m_kind;
};

#endif // !JZ_TS_EDIT_TOOLS_H_
