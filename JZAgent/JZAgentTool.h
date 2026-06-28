#ifndef JZ_AGENT_TOOL_H_
#define JZ_AGENT_TOOL_H_

#include <memory>
#include <functional>
#include "json/json.h"
#include "JZOptional.h"

// 1. 参数类型枚举
enum ParamType
{
    STRING,    // 字符串
    NUMBER,    // 数字（整数/浮点数）
    BOOLEAN,   // 布尔值
    ARRAY,     // 数组
    OBJECT,    // 对象
    NULL_TYPE  // null类型
};

// 2. 参数 Schema 定义（符合 JSON Schema）
struct ParamSchema
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

// 3. 工具定义（OpenAI 标准结构）
struct ToolCall
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

struct ToolCallResult
{
    ToolCallResult();

    std::string id;
    bool isOk;
    std::string result;
};

class JZAgentTool;
typedef std::shared_ptr<JZAgentTool> JZAgentToolPtr;

struct ToolDefine
{
    Json::Value toJson() const;
    bool fromJson(const Json::Value &root);

    std::string name;
    std::string description;
    std::vector<ParamSchema> parameters;
    std::vector<std::string> required_parameters;

    JZAgentToolPtr impl;
};

//JZAgentTool
class JZAgent;
class JZAgentTool
{
public:
    JZAgentTool();
    virtual ~JZAgentTool();

    virtual void call(JZAgent *agent, const ToolCall &params) = 0;
};

//JZAgentFunctionTool
class JZAgentFunctionTool : public JZAgentTool
{
public:
    JZAgentFunctionTool();
    virtual ~JZAgentFunctionTool();

    virtual void call(JZAgent *agent, const ToolCall &params) override;

    std::function<ToolCallResult(JZAgent *agent, const ToolCall &params)> function;
};

//JZAgentAsyncFunctionTool
class JZAgentAsyncFunctionTool : public JZAgentTool
{
public:
    JZAgentAsyncFunctionTool();
    virtual ~JZAgentAsyncFunctionTool();

    virtual void call(JZAgent *agent, const ToolCall &params) override;

    std::function<void(JZAgent *agent, const ToolCall &params)> function;
};

#endif  // !JZ_AGENT_TOOL_H_
