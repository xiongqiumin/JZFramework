#include <cstdlib>
#include <cstdio>
#include <sstream>
#include <vector>
#include <string>
#include <iomanip> 
#include "JZCommandLineParser.h"
#include "JZDebug.h"
#include "JZFileInfo.h"
#include "JZStd.h"

//JZCommandLineOption
JZCommandLineOption::JZCommandLineOption()
{
    required = false;
}

JZCommandLineOption::JZCommandLineOption(std::string name,std::string shortName,std::string desc,bool required)
{
    JZ_ASSERT_X(str_startsWith(name,"--"), "name should starts with --");
    JZ_ASSERT_X(shortName.empty() || str_startsWith(shortName,"-"), "short name should starts with -");

    this->name = name;
    this->shortName = shortName;
    this->description = desc;
    this->required = required;
}

//JZCommandLineArgument
JZCommandLineArgument::JZCommandLineArgument()
{
    required = true;
}

//JZCommandLineParser
JZCommandLineParser::CommandValue::CommandValue()
    : type(STRING), ptr(nullptr), parsed(false), required(false), shortName("")
{
}

JZCommandLineParser::JZCommandLineParser()
{
}

void JZCommandLineParser::setDescription(const std::string & description)
{
    m_description = description;
}

void JZCommandLineParser::addOption(const std::string& name, std::string* value, bool required)
{
    JZCommandLineOption opt = parseOptionName(name);
    opt.required = required;
    addOption(opt, value);
}

void JZCommandLineParser::addEnumOption(const std::string& name, std::string* value, const std::vector<std::string>& enumValues, bool required)
{
    JZCommandLineOption opt = parseOptionName(name);
    opt.required = required;
    addEnumOption(opt, value, enumValues);
}

void JZCommandLineParser::addOption(const std::string& name, int* value, bool required)
{
    JZCommandLineOption opt = parseOptionName(name);
    opt.required = required;
    addOption(opt, value);
}

void JZCommandLineParser::addOption(const std::string& name, double* value, bool required)
{
    JZCommandLineOption opt = parseOptionName(name);
    opt.required = required;
    addOption(opt, value);
}

void JZCommandLineParser::addOption(const std::string& name, bool* value, bool required)
{
    JZCommandLineOption opt = parseOptionName(name);
    opt.required = required;
    addOption(opt, value);
}

void JZCommandLineParser::addOption(const JZCommandLineOption& opt, std::string* value)
{
    CommandValue cv;
    cv.type = CommandValue::STRING;
    cv.ptr = value;
    cv.required = opt.required;
    cv.parsed = false;
    cv.name = opt.name;
    cv.shortName = opt.shortName;
    cv.description = opt.description;
    m_commands.push_back(cv);
}

void JZCommandLineParser::addEnumOption(const JZCommandLineOption& opt, std::string* value, const std::vector<std::string>& enumValues)
{
    JZ_ASSERT(enumValues.size() > 0);
    CommandValue cv;
    cv.type = CommandValue::ENUM;
    cv.ptr = value;
    cv.required = opt.required;
    cv.parsed = false;
    cv.name = opt.name;
    cv.shortName = opt.shortName;
    cv.description = opt.description;
    cv.enumValues = enumValues;
    m_commands.push_back(cv);
}

void JZCommandLineParser::addOption(const JZCommandLineOption& opt, int* value)
{
    CommandValue cv;
    cv.type = CommandValue::INT;
    cv.ptr = value;
    cv.required = opt.required;
    cv.parsed = false;
    cv.name = opt.name;
    cv.shortName = opt.shortName;
    m_commands.push_back(cv);
}

void JZCommandLineParser::addOption(const JZCommandLineOption& opt, double* value)
{
    CommandValue cv;
    cv.type = CommandValue::DOUBLE;
    cv.ptr = value;
    cv.required = opt.required;
    cv.parsed = false;
    cv.name = opt.name;
    cv.shortName = opt.shortName;
    m_commands.push_back(cv);
}

void JZCommandLineParser::addOption(const JZCommandLineOption& opt, bool* value)
{
    CommandValue cv;
    cv.type = CommandValue::BOOL;
    cv.ptr = value;
    cv.required = opt.required;
    cv.parsed = false;
    cv.name = opt.name;
    cv.shortName = opt.shortName;
    m_commands.push_back(cv);
}

JZCommandLineParser::CommandValue* JZCommandLineParser::getOptionValue(const std::string& name)
{
    for (int i = 0; i < (int)m_commands.size(); i++)
    {
        auto& cv = m_commands[i];
        if (cv.shortName == name || cv.name == name)
            return &cv;
    }
    return nullptr;
}

bool JZCommandLineParser::isOptionProvided(const std::string& name)
{
    auto cmd = getOptionValue(name);
    JZ_ASSERT(cmd);
    return cmd->parsed;
}

//argument
void JZCommandLineParser::addArgument(const std::string& name, std::string* value, bool required)
{
    JZCommandLineArgument cv;
    cv.name = name;
    cv.required = required;
    addArgument(cv, value);
}

void JZCommandLineParser::addArgument(const std::string& name, int* value, bool required)
{
    JZCommandLineArgument opt;
    opt.name = name;
    opt.required = required;
    addArgument(opt, value);
}

void JZCommandLineParser::addArgument(const std::string& name, double* value, bool required)
{
    JZCommandLineArgument opt;
    opt.name = name;
    opt.required = required;
    addArgument(opt, value);
}

void JZCommandLineParser::addArgument(const std::string& name, bool* value, bool required)
{
    JZCommandLineArgument opt;
    opt.name = name;
    opt.required = required;
    addArgument(opt, value);
}

void JZCommandLineParser::addArgument(const JZCommandLineArgument& opt, std::string* value)
{
    CommandValue cv;
    cv.type = CommandValue::STRING;
    cv.ptr = value;
    cv.required = opt.required;
    cv.parsed = false;
    cv.name = opt.name;
    addArgumentValue(cv);
}

void JZCommandLineParser::addArgument(const JZCommandLineArgument& opt, int* value)
{
    CommandValue cv;
    cv.type = CommandValue::INT;
    cv.ptr = value;
    cv.required = opt.required;
    cv.parsed = false;
    cv.name = opt.name;
    addArgumentValue(cv);
}

void JZCommandLineParser::addArgument(const JZCommandLineArgument& opt, double* value)
{
    CommandValue cv;
    cv.type = CommandValue::DOUBLE;
    cv.ptr = value;
    cv.required = opt.required;
    cv.parsed = false;
    cv.name = opt.name;
    addArgumentValue(cv);
}

void JZCommandLineParser::addArgument(const JZCommandLineArgument& opt, bool* value)
{
    CommandValue cv;
    cv.type = CommandValue::BOOL;
    cv.ptr = value;
    cv.required = opt.required;
    cv.parsed = false;
    cv.name = opt.name;
    addArgumentValue(cv);
}

void JZCommandLineParser::addArgumentValue(const CommandValue& cv)
{
    if (cv.required && m_posCommands.size() > 0 && !m_posCommands.back().required)
    {
        JZ_ASSERT(0);
    }
    m_posCommands.push_back(cv);
}

bool JZCommandLineParser::isArgumentProvided(const std::string& name) const
{
    for (const auto& arg : m_posCommands)
    {
        if (arg.name == name)
        {
            return arg.parsed;
        }
    }
    return false;
}

bool JZCommandLineParser::parse(const std::string& commandLine)
{
    std::vector<std::string> args;
    std::string currentArg;
    bool inQuotes = false;

    for (size_t i = 0; i < commandLine.length(); i++)
    {
        char c = commandLine[i];

        if (c == '"')
        {
            inQuotes = !inQuotes;
        }
        else if (c == ' ' && !inQuotes)
        {
            if (!currentArg.empty())
            {
                args.push_back(currentArg);
                currentArg.clear();
            }
        }
        else
        {
            currentArg += c;
        }
    }

    if (!currentArg.empty())
    {
        args.push_back(currentArg);
    }

    std::vector<char*> argvPtrs;
    for (auto& arg : args)
    {
        argvPtrs.push_back(&arg[0]);
    }

    return parse(argvPtrs.size(), argvPtrs.data());
}

// 帮助/用法/报错这类"给人看的输出"统一走 JZLog(jzInfo): 控制台编码由 JZLog 按 stdout 的
// 实际状态决定(真控制台按控制台 CP 转字节, 管道/UTF-8 终端原样 UTF-8 —— 见 JZLog.cpp 里
// toConsoleBytes 的说明)。于是"整段裸 UTF-8 在 GBK 控制台乱码"与"只给某一行转 local 在
// UTF-8 终端乱码"两种毛病都不再存在: 整段输出走同一处, 编码只判一次。
// ⚠ noquote(): JZDebug 的 operator<<(std::string) 默认会给字符串加引号并转义;
// ⚠ fflush(stdout): JZLog 的控制台分支是 printf, 输出被重定向/走管道时是**块缓冲**。
//   这页文本很可能是"打完还继续跑"(如 test_agent 解析失败后继续用默认配置), 主动 flush
//   保证它立刻出去(换成 jzInfo 之前是逐行 std::endl, 本来就是即时的)。
//   末尾那个多出来的 '\n' 去掉: JZLog 输出时会自己补一个, 免得帮助结尾多一行空白。
static void printToLog(std::string text)
{
    if (!text.empty() && text[text.size() - 1] == '\n')
        text.erase(text.size() - 1);
    jzInfo().noquote() << text.c_str();
    fflush(stdout);
}

void JZCommandLineParser::reportError(const std::string& error)
{
    m_errorMsg = error;
    printToLog(usage() + "\n" + m_errorMsg);
    std::exit(1);
}

JZCommandLineOption JZCommandLineParser::parseOptionName(const std::string& name)
{
    JZCommandLineOption result;

    auto parts = str_split(name, ",", JZ::SkipEmptyParts);
    JZ_ASSERT(parts.size() > 0 && parts.size() <= 2);

    for (const auto& part : parts)
    {
        auto trimmed = str_trimmed(part);
        if (trimmed.size() > 2 && trimmed[0] == '-' && trimmed[1] == '-')
        {
            // 长选项
            result.name = trimmed;
        }
        else if (trimmed.size() > 1 && trimmed[0] == '-')
        {
            // 短选项
            JZ_ASSERT(trimmed.size() == 2);
            result.shortName = trimmed;
        }
        else
        {
            JZ_ASSERT(false);  // 非法格式
        }
    }

    return result;
}

bool JZCommandLineParser::valueToPosArg(const std::string& valueStr, int posArgIndex)
{
    if (posArgIndex >= m_posCommands.size())
    {
        reportError("too many positional arguments, Expected at most " + std::to_string(m_posCommands.size()) +
                    " positional argument(s)");
        return false;
    }

    auto cmd = &m_posCommands[posArgIndex];
    std::string error_pre = "positional argument #" + std::to_string(posArgIndex) + ": " + cmd->name; 
    switch (cmd->type)
    {
        case CommandValue::BOOL:
        {
            bool* b = static_cast<bool*>(cmd->ptr);
            if (valueStr == "true" || valueStr == "1")
            {
                *b = true;
            }
            else if (valueStr == "false" || valueStr == "0")
            {
                *b = false;
            }
            else
            {
                reportError(error_pre + " requires an double, got \"" + valueStr + "\"");
                return false;
            }
            break;
        }
        case CommandValue::STRING:
            *static_cast<std::string*>(cmd->ptr) = str_ensure_utf8(valueStr);
            break;

        case CommandValue::INT:
        {
            bool ok;
            int val = str_toInt(valueStr, &ok);
            if (!ok)
            {
                reportError(error_pre + " requires an int, got \"" + valueStr + "\"");
                return false;
            }
            *static_cast<int*>(cmd->ptr) = val;
            break;
        }

        case CommandValue::DOUBLE:
        {
            bool ok;
            double val = str_toDouble(valueStr, &ok);
            if (!ok)
            {
                reportError(error_pre + " requires an double, got \"" + valueStr + "\"");
                return false;
            }
            *static_cast<double*>(cmd->ptr) = val;
            break;
        }

        default:
            break;
    }

    cmd->parsed = true;
    return true;
}

bool JZCommandLineParser::parse(int argc, char** argv)
{
    JZ_ASSERT(argc >= 1);

    JZFileInfo info(argv[0]);
    m_programName = info.fileName();

    if(argc == 2)
    {
        std::string arg = argv[1];
        if (arg == "-h" || arg == "--help")
        {
            printHelp();
            std::exit(0);
        }
    }

    m_errorMsg.clear();
    
    bool inPositionalMode = false;
    int posArgIndex = 0;

    // 重置所有参数的解析状态
    for (auto& cmd : m_commands)
        cmd.parsed = false;

    // ==============================
    // 【系统标准】从 i=1 开始解析，跳过 argv[0] 程序名
    // ==============================
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];

        // ==============================
        // 位置参数模式：后续所有参数都视为普通参数，不再解析选项
        // ==============================
        if (inPositionalMode)
        {
            if (!valueToPosArg(arg, posArgIndex))
                return false;
            posArgIndex++;
            continue;
        }

        // ==============================
        // 不是以 - 开头 → 进入位置参数模式
        // ==============================
        if (arg.empty() || arg[0] != '-')
        {
            inPositionalMode = true;
            if (!valueToPosArg(arg,posArgIndex))
                return false;
            posArgIndex++;
            continue;
        }

        // ==============================
        // 非法短参数：如 "-"
        // ==============================
        if (arg.size() < 2)
        {
            reportError("Invalid argument: " + arg);
            return false;
        }

        std::string opt_name;
        std::string valueStr;
        bool hasEqual = false;
        size_t eqPos = arg.find('=');

        // ==============================
        // 解析 = 格式：--key=val 或 -k=val
        // ==============================
        if (eqPos != std::string::npos)
        {
            hasEqual = true;
            opt_name = arg.substr(0, eqPos);
            valueStr = arg.substr(eqPos+1);
        }
        else
        {
            opt_name = arg;
        }

        // ==============================
        // 查找参数
        // ==============================
        CommandValue* cmd = getOptionValue(opt_name);

        if (!cmd)
        {
            reportError("unrecognized arguments: " + opt_name);
            return false;
        }

        // ==============================
        // BOOL 类型
        // ==============================
        if (cmd->type == CommandValue::BOOL)
        {
            bool* bptr = static_cast<bool*>(cmd->ptr);
            *bptr = true;

            if (hasEqual)
                *bptr = (valueStr == "true");
            else if (i + 1 < argc)
            {
                std::string nextArg = argv[i + 1];
                if (nextArg == "true" || nextArg == "false")
                {
                    *bptr = (nextArg == "true");
                    ++i;
                }
            }

            cmd->parsed = true;
        }
        // ==============================
        // 字符串 / 整数 / 浮点数
        // ==============================
        else
        {
            if (!hasEqual)
            {
                if (i + 1 >= argc)
                {
                    reportError("missing value for option " + opt_name);
                    return false;
                }
                valueStr = argv[++i];
            }

            switch (cmd->type)
            {
                case CommandValue::STRING:
                    *static_cast<std::string*>(cmd->ptr) = str_ensure_utf8(valueStr);
                    break;

                case CommandValue::ENUM:
                {
                    bool matched = false;
                    std::string utf8Val = str_ensure_utf8(valueStr);
                    for (const auto& e : cmd->enumValues)
                    {
                        if (e == utf8Val)
                        {
                            matched = true;
                            break;
                        }
                    }
                    if (!matched)
                    {
                        std::string allowed;
                        for (size_t k = 0; k < cmd->enumValues.size(); k++)
                        {
                            if (k) allowed += ", ";
                            allowed += cmd->enumValues[k];
                        }
                        reportError("option " + opt_name + " must be one of {" + allowed + "}, got \"" + valueStr + "\"");
                        return false;
                    }
                    *static_cast<std::string*>(cmd->ptr) = utf8Val;
                    break;
                }

                case CommandValue::INT:
                {
                    bool ok;
                    int val = str_toInt(valueStr, &ok);
                    if (!ok)
                    {
                        reportError("option " + opt_name + " requires an integer, got \"" + valueStr + "\"");
                        return false;
                    }
                    *static_cast<int*>(cmd->ptr) = val;
                    break;
                }

                case CommandValue::DOUBLE:
                {
                    bool ok;
                    double val = str_toDouble(valueStr, &ok);
                    if (!ok)
                    {
                        reportError("option " + opt_name + " requires an double, got \"" + valueStr + "\"");
                        return false;
                    }
                    *static_cast<double*>(cmd->ptr) = val;
                    break;
                }

                default:
                    break;
            }

            cmd->parsed = true;
        }
    }

    // ==============================
    // 检查必选参数
    // ==============================
    for (const auto& cv : m_commands)
    {
        if (cv.required && !cv.parsed)
        {
            reportError("missing required option: " + optionName(cv));
            return false;
        }
    }

    for (int i = 0; i < (int)m_posCommands.size(); i++)
    {
        auto& cv = m_posCommands[i];
        if (cv.required && !cv.parsed)
        {
            auto errorMsg = "missing required positional argument #" + std::to_string(i) + ": " + cv.name;
            reportError(errorMsg);
            return false;
        }
    }

    return true;
}

bool JZCommandLineParser::parseOptions(const std::string& options)
{
    std::string command_line = "parseOptions " + options;
    if(!parse(command_line))
    {
        return false;
    }

    return true;
}

std::string JZCommandLineParser::errorString() const
{
    return m_errorMsg;
}

std::string JZCommandLineParser::usage() const
{
    std::string usage = "usage: " + m_programName;
    if (m_commands.size() != 0)
        usage += " [options]";

    for (const auto& pos : m_posCommands)
    {
        std::string name;
        if (pos.required)
            name = " <" + pos.name + ">";  // 必选
        else
            name = " [" + pos.name + "]";  // 可选

        usage += name;
    }
    return usage;
}

std::string JZCommandLineParser::optionName(const CommandValue& cv) const
{
    std::string result;
    if (!cv.shortName.empty())
    {
        result += cv.shortName;
        if (!cv.name.empty())
            result += ", ";
    }
    if (!cv.name.empty())
        result += cv.name;
    
    return result;
}

// 核心实现
void JZCommandLineParser::printHelp() const
{
    auto value_str = [](const CommandValue& cv) -> std::string
    {
        std::string result = " (";

        // 输出类型
        switch (cv.type)
        {
            case CommandValue::STRING:
                result += "string";
                break;
            case CommandValue::INT:
                result += "int";
                break;
            case CommandValue::DOUBLE:
                result += "float";
                break;
            case CommandValue::BOOL:
                result += "bool";
                break;
            case CommandValue::ENUM:
            {
                result += "enum{";
                for (size_t k = 0; k < cv.enumValues.size(); k++)
                {
                    if (k) result += ", ";
                    result += cv.enumValues[k];
                }
                result += "}";
                break;
            }
        }

        // 非必填才输出默认值
        if (!cv.required)
        {
            result += ", default = ";
            switch (cv.type)
            {
                case CommandValue::STRING:
                    result += "\"" + *static_cast<std::string*>(cv.ptr) + "\"";
                    break;
                case CommandValue::ENUM:
                    result += "\"" + *static_cast<std::string*>(cv.ptr) + "\"";
                    break;
                case CommandValue::INT:
                    result += std::to_string(*static_cast<int*>(cv.ptr));
                    break;
                case CommandValue::DOUBLE:
                    result += std::to_string(*static_cast<double*>(cv.ptr));
                    break;
                case CommandValue::BOOL:
                    result += (*static_cast<bool*>(cv.ptr) ? "true" : "false");
                    break;
            }
        }

        result += ")";
        return result;
    };

    // 整段攒进 oss, 最后一次交给 JZLog 输出(见 printToLog: 编码判定在 JZLog 里统一做)
    std::ostringstream oss;

    if(!m_description.empty())
    {
        // 直接给 UTF-8 —— JZLog 会按控制台/管道状态决定要不要转字节。
        // (以前这里单独多转了一次 local: 在 UTF-8 终端/管道里, 整页帮助里就这行是乱码)
        oss << m_programName << " - " << m_description << "\n\n";
    }

    oss << usage() << "\n\n";

    if (m_commands.size() > 0)
    {
        oss << "options:\n";

        // 先遍历一遍，找到最长的参数名，用于对齐
        size_t maxNameLen = 0;
        for (const auto& cmd : m_commands)
        {
            std::string name = optionName(cmd);
            maxNameLen = std::max(maxNameLen, name.size());
        }

        oss << "  " << std::left << std::setw(maxNameLen) << "-h, --help"
            << ": show this help message and exit\n";

        for (const auto& cv : m_commands)
        {
            std::string name = optionName(cv);

            // 输出参数名，固定宽度右对齐冒号
            oss << "  " << std::left << std::setw(maxNameLen) << name << ": ";

            // 输出描述
            if (cv.description.empty())
                oss << cv.name;
            else
                oss << cv.description;

            oss << value_str(cv) << "\n";
        }
    }

    if (m_posCommands.size() > 0)
    {
        if (m_commands.size() > 0)
            oss << "\n";

        oss << "position arguments:\n";
        for (const auto& cv : m_posCommands)
        {
            std::string name;
            if (cv.required)
                name = "  <" + cv.name + ">";  // 必选
            else
                name = "  [" + cv.name + "]";  // 可选

            oss << name << ": ";

            // 输出描述
            if (cv.description.empty())
                oss << cv.name;
            else
                oss << cv.description;

            oss << value_str(cv) << "\n";
        }
    }

    printToLog(oss.str());
}