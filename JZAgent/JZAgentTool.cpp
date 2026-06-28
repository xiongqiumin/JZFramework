#include "JZAgentTool.h"
#include "JZAgent.h"

// ------------------------------
// 类型转换函数
// ------------------------------
std::string paramTypeToString(ParamType type)
{
    switch (type)
    {
        case STRING:
            return "string";
        case NUMBER:
            return "number";
        case BOOLEAN:
            return "boolean";
        case ARRAY:
            return "array";
        case OBJECT:
            return "object";
        case NULL_TYPE:
            return "null";
        default:
            return "string";
    }
}

ParamType stringToParamType(const std::string& typeStr)
{
    if (typeStr == "string")
        return STRING;
    if (typeStr == "number")
        return NUMBER;
    if (typeStr == "boolean")
        return BOOLEAN;
    if (typeStr == "array")
        return ARRAY;
    if (typeStr == "object")
        return OBJECT;
    if (typeStr == "null")
        return NULL_TYPE;
    return STRING;
}

// ------------------------------
// ParamSchema 序列化（JSON Schema 标准）
// ------------------------------
Json::Value ParamSchema::toJson() const
{
    Json::Value root;
    root["type"] = paramTypeToString(type);
    root["description"] = description;

    // 枚举
    if (enum_values.isSome())
    {
        Json::Value arr(Json::arrayValue);
        for (const auto& e : enum_values.value())
            arr.append(e);
        root["enum"] = arr;
    }

    // 字符串约束
    if (minLength.isSome())
        root["minLength"] = minLength.value();
    if (maxLength.isSome())
        root["maxLength"] = maxLength.value();
    if (pattern.isSome())
        root["pattern"] = pattern.value();
    if (format.isSome())
        root["format"] = format.value();

    // 数值约束
    if (minimum.isSome())
        root["minimum"] = minimum.value();
    if (maximum.isSome())
        root["maximum"] = maximum.value();
    if (exclusiveMinimum.isSome())
        root["exclusiveMinimum"] = exclusiveMinimum.value();
    if (exclusiveMaximum.isSome())
        root["exclusiveMaximum"] = exclusiveMaximum.value();

    // 默认值
    if (default_val.isSome())
        root["default"] = default_val.value();

    // 数组元素类型
    if (array_item_type.isSome())
    {
        Json::Value itemSchema;
        itemSchema["type"] = paramTypeToString(array_item_type.value());
        root["items"] = itemSchema;
    }

    // 对象子属性
    if (properties.isSome())
    {
        Json::Value props(Json::objectValue);
        for (const auto& p : properties.value())
        {
            props[p.name] = p.toJson();
        }
        root["properties"] = props;
    }

    return root;
}

// ------------------------------
// ParamSchema 反序列化
// ------------------------------
bool ParamSchema::fromJson(const Json::Value& root)
{
    if (!root.isObject())
        return false;

    description = root.get("description", "").asString();
    type = stringToParamType(root.get("type", "string").asString());

    // 基础约束
    if (root.isMember("default"))
        default_val = root["default"];
    if (root.isMember("minLength") && root["minLength"].isInt())
        minLength = root["minLength"].asInt();
    if (root.isMember("maxLength") && root["maxLength"].isInt())
        maxLength = root["maxLength"].asInt();
    if (root.isMember("minimum") && root["minimum"].isNumeric())
        minimum = root["minimum"].asDouble();
    if (root.isMember("maximum") && root["maximum"].isNumeric())
        maximum = root["maximum"].asDouble();
    if (root.isMember("exclusiveMinimum"))
        exclusiveMinimum = root["exclusiveMinimum"].asBool();
    if (root.isMember("exclusiveMaximum"))
        exclusiveMaximum = root["exclusiveMaximum"].asBool();
    if (root.isMember("format"))
        format = root["format"].asString();
    if (root.isMember("pattern"))
        pattern = root["pattern"].asString();

    // 枚举
    if (root.isMember("enum") && root["enum"].isArray())
    {
        std::vector<std::string> enums;
        for (const auto& e : root["enum"])
            enums.push_back(e.asString());
        enum_values = enums;
    }

    // 数组元素
    if (root.isMember("items") && root["items"].isMember("type"))
    {
        array_item_type = stringToParamType(root["items"]["type"].asString());
    }

    // 对象子属性
    if (root.isMember("properties") && root["properties"].isObject())
    {
        std::vector<ParamSchema> props;
        auto& propObj = root["properties"];
        for (const std::string& key : propObj.getMemberNames())
        {
            ParamSchema p;
            p.name = key;
            if (p.fromJson(propObj[key]))
            {
                props.push_back(p);
            }
        }
        properties = props;
    }

    return true;
}

// ToolCall
Json::Value ToolCall::toJson() const
{
    Json::Value tool_call;
    tool_call["id"] = id;
    tool_call["type"] = type;

    Json::Value func;
    func["name"] = function_name;
    func["arguments"] = function_args;
    tool_call["function"] = func;

    return tool_call;
}

bool ToolCall::fromJson(const Json::Value& value)
{
    if (!value.isObject())
        return false;

    // 1. 基础字段
    id = value.get("id", "").asString();
    type = value.get("type", "").asString();

    // 2. 解析 function
    const Json::Value& func = value["function"];
    if (func.isObject())
    {
        function_name = func.get("name", "").asString();
        function_args = func.get("arguments", "").asString();
    }
    else
    {
        function_name.clear();
        function_args.clear();
    }
    
    params = Json::Value();
    return true;
}

// ToolCallResult
ToolCallResult::ToolCallResult()
{
    isOk = false;
}


// ToolDefine
Json::Value ToolDefine::toJson() const
{
    Json::Value func;
    func["name"] = name;
    func["description"] = description;

    // 构建 parameters（严格符合 OpenAI 规范）
    Json::Value params;
    params["type"] = "object";

    // properties
    Json::Value props(Json::objectValue);
    for (const auto& p : parameters)
    {
        props[p.name] = p.toJson();
    }
    params["properties"] = props;

    // 必填参数列表
    if (!required_parameters.empty())
    {
        Json::Value reqArr(Json::arrayValue);
        for (const auto& r : required_parameters)
            reqArr.append(r);
        params["required"] = reqArr;
    }

    func["parameters"] = params;
    return func;
}

bool ToolDefine::fromJson(const Json::Value& func)
{
    name = func.get("name", "").asString();
    description = func.get("description", "").asString();

    // 读取 parameters
    if (func.isMember("parameters") && func["parameters"].isObject())
    {
        const Json::Value& params = func["parameters"];
        const Json::Value& props = params["properties"];

        parameters.clear();
        for (const std::string& key : props.getMemberNames())
        {
            ParamSchema p;
            p.name = key;
            if (!p.fromJson(props[key]))
                return false;

            parameters.push_back(p);
        }

        // 读取 required
        required_parameters.clear();
        if (params.isMember("required") && params["required"].isArray())
        {
            for (const auto& r : params["required"])
            {
                required_parameters.push_back(r.asString());
            }
        }
    }
    return true;
}

//JZAgentTool
JZAgentTool::JZAgentTool()
{
}

JZAgentTool::~JZAgentTool()
{
}

//JZAgentFunctionTool
JZAgentFunctionTool::JZAgentFunctionTool()
{
}

JZAgentFunctionTool::~JZAgentFunctionTool()
{
}

void JZAgentFunctionTool::call(JZAgent* agent, const ToolCall& params)
{
    ToolCallResult response = function(agent, params);
    agent->toolCallResult(response);
}

//JZAgentAsyncFunctionTool
JZAgentAsyncFunctionTool::JZAgentAsyncFunctionTool()
{
}

JZAgentAsyncFunctionTool::~JZAgentAsyncFunctionTool()
{
}

void JZAgentAsyncFunctionTool::call(JZAgent* agent, const ToolCall& params)
{
    function(agent, params);
}