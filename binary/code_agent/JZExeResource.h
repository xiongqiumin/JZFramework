#ifndef JZ_EXE_RESOURCE_H_
#define JZ_EXE_RESOURCE_H_

#include <string>

// 通用 PE 资源修改工具。
// 把"替换 exe 图标" generalization 为对任意资源(type/name)的读写，并额外提供
// 图标与"调试开关"两个具体能力的便捷封装。全部基于 Win32 资源 API
// （BeginUpdateResource / UpdateResource / EndUpdateResource），因此本文件只 include
// <windows.h>，不 include JZAgentTool.h（避免把 JZAgent 依赖带进这个纯 Win32 工具）。
//
// 用途：导出工程时把内嵌 JZEngineRuntime 的 code_runtime.exe 改名、换图标，并写入一个
//       "调试开关"资源（约定 type="JZRT_CFG" name="DEBUG"，内容为 JSON {"debug":bool}），
//       让连接后的 code_runtime 看起来像独立程序，且能被运行时自身读取以决定是否开启调试器。
class JZExeResource
{
public:
    // 通用：把任意字节 data 写入 exe 资源（type/name 为资源类型/名称字符串，语言中性）。
    // 成功返回 true；失败返回 false，error 说明原因。
    static bool setRawResource(const std::string &exePath,
                               const std::string &type, const std::string &name,
                               const std::string &data, std::string &error);

    // 通用：读回资源字节；不存在返回 false（error 说明原因）。
    static bool getRawResource(const std::string &exePath,
                               const std::string &type, const std::string &name,
                               std::string &out, std::string &error);

    // 图标：保持原 JZExeIcon 的能力
    static bool replaceIcon(const std::string &exePath, const std::string &icoPath, std::string &error);
    static bool hasIcon(const std::string &exePath);

    // 调试开关资源（约定 type="JZRT_CFG" name="DEBUG"，内容为 JSON {"debug":bool}）
    static bool setDebugFlag(const std::string &exePath, bool debug, std::string &error);
    // 读取调试开关；资源不存在时返回 true 且 debug=false（即缺省非调试）。
    static bool getDebugFlag(const std::string &exePath, bool &debug, std::string &error);

    static const char *kDebugType; // "JZRT_CFG"
    static const char *kDebugName; // "DEBUG"
};

#endif // !JZ_EXE_RESOURCE_H_
