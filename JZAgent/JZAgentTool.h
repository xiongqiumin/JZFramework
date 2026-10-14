#ifndef JZ_AGENT_TOOL_H_
#define JZ_AGENT_TOOL_H_
#include <memory>
#include <functional>
#include "json/json.h"
#include "JZOptional.h"
#include "JZObject.h"
#include "JZSignal.h"

// 1. 参数类型枚举
//    用 enum class + 限定名, 不让 STRING/NUMBER/BOOLEAN/ARRAY/OBJECT 这些通用名
//    作为枚举数泄漏到全局命名空间 —— BOOLEAN 会与 <winnt.h> 的
//    `typedef unsigned char BOOLEAN` 冲突 (C2365), 且包含顺序无关, 无法靠调整 include 规避。
enum class ParamType
{
    String,   // 字符串
    Number,   // 数字（整数/浮点数）
    Boolean,  // 布尔值
    Array,    // 数组
    Object,   // 对象
    Null      // null类型
};

// 2. 参数 Schema 定义（符合 JSON Schema）
struct JZFRAMEWORK_API ParamSchema
{
    Json::Value toJson() const;
    bool fromJson(const Json::Value &root);

    std::string name;         // 参数名
    std::string description;  // 参数描述
    ParamType type;           // 参数类型

    // 约束条件
    JZOptional<Json::Value> default_val;
    JZOptional<int> minLength;
    JZOptional<int> maxLength;
    JZOptional<double> minimum;
    JZOptional<double> maximum;
    JZOptional<bool> exclusiveMinimum;
    JZOptional<bool> exclusiveMaximum;
    JZOptional<ParamType> array_item_type;
    JZOptional<std::vector<ParamSchema>> properties;
    JZOptional<std::vector<std::string>> enum_values;
    JZOptional<std::string> format;
    JZOptional<std::string> pattern;
};

// 3. 工具调用请求（OpenAI 标准结构）
struct JZFRAMEWORK_API ToolCall
{
    Json::Value toJson() const;
    bool fromJson(const Json::Value &value);

    std::string id;
    std::string type;
    std::string function_name;
    std::string function_args;

    //function_args 解析结果
    Json::Value params;
};

struct JZFRAMEWORK_API ToolCallResult
{
    ToolCallResult();

    std::string id;
    bool isOk;
    std::string result;
};

// 4. 工具定义（OpenAI 标准结构）
struct JZFRAMEWORK_API ToolDefine
{
    Json::Value toJson() const;
    bool fromJson(const Json::Value &root);

    std::string name;
    std::string description;
    std::vector<ParamSchema> parameters;
    std::vector<std::string> required_parameters;
};

class JZAgent;

// 5. 工具基类
class JZFRAMEWORK_API JZAgentTool : public JZObject
{
public:
    JZAgentTool();
    virtual ~JZAgentTool();

    const std::string &toolName() const;
    const ToolDefine &toolDefine() const;
    void setToolDefine(const ToolDefine &def);

    // 统一调用接口：agent 在需要执行工具时传入自身与已解析的参数
    virtual void call(const ToolCall &params) = 0;
    virtual void cancel(const std::string &id);

    // 轮询类工具: 用**同样的参数**反复调用本身就是正常用法(如 terminal_query 查后台任务
    // 进度、terminal_list 看会话), 不该被循环检测当成"原地打转"打掉。返回 true 的工具
    // 走放宽阈值(见 JZLoopConfig::pollRepeat / pollSameResult)。
    virtual bool isPollTool() const { return false; }

jz_signals:
    JZSignal<ToolCallResult> sigResult;

protected:
    ToolDefine m_toolDefine;
};

#endif  // !JZ_AGENT_TOOL_H_
