#ifndef JZ_COMMANDLINEPARSER_H
#define JZ_COMMANDLINEPARSER_H
#include "JZGlobal.h"
#include <iostream>
#include <map>
#include <string>

class JZFRAMEWORK_API JZCommandLineOption
{
public:
    JZCommandLineOption();
    JZCommandLineOption(std::string name,std::string shortName,std::string desc,bool required = false);
    
    std::string name;
    std::string shortName;
    std::string description;
    bool required;
};

class JZFRAMEWORK_API JZCommandLineArgument
{
public:
    JZCommandLineArgument();

    std::string name;
    std::string description;
    bool required;
};

class JZFRAMEWORK_API JZCommandLineParser
{
public:
    JZCommandLineParser();

    void setDescription(const std::string& description);

    // name supports single "-t" or "--time", or combined "-t,--time"
    void addOption(const std::string& name, std::string* value, bool required = false);
    void addEnumOption(const std::string& name, std::string* value, const std::vector<std::string>& enumValues, bool required = false);
    void addOption(const std::string& name, int* value, bool required = false);
    void addOption(const std::string& name, double* value, bool required = false);
    void addOption(const std::string& name, bool* value, bool required = false);
    void addOption(const JZCommandLineOption& opt, std::string* value);
    void addEnumOption(const JZCommandLineOption& opt, std::string* value, const std::vector<std::string>& enumValues);
    void addOption(const JZCommandLineOption& opt, int* value);
    void addOption(const JZCommandLineOption& opt, double* value);
    void addOption(const JZCommandLineOption& opt, bool* value);
    bool isOptionProvided(const std::string& name);

    void addArgument(const std::string& name, std::string* value, bool required = true);
    void addArgument(const std::string& name, int* value, bool required = true);
    void addArgument(const std::string& name, double* value, bool required = true);
    void addArgument(const std::string& name, bool* value, bool required = true);
    void addArgument(const JZCommandLineArgument& opt, std::string* value);
    void addArgument(const JZCommandLineArgument& opt, int* value);
    void addArgument(const JZCommandLineArgument& opt, double* value);
    void addArgument(const JZCommandLineArgument& opt, bool* value);
    bool isArgumentProvided(const std::string& name) const;

    bool parse(int argc, char** argv);
    bool parse(const std::string& commandLine);
    bool parseOptions(const std::string& options);

    std::string errorString() const;
    void printHelp() const;

private:
    struct CommandValue
    {
        enum Type
        {
            STRING,
            INT,
            DOUBLE,
            BOOL,
            ENUM
        };

        Type type;
        void* ptr;
        bool parsed;
        bool required;
        std::string name;
        std::string shortName;
        std::string description;
        std::vector<std::string> enumValues;   // 仅 ENUM 类型有效: 允许取值的字符串集合

        CommandValue();
    };

    std::string usage() const;
    JZCommandLineOption parseOptionName(const std::string& name);
    CommandValue* getOptionValue(const std::string& name);
    bool valueToPosArg(const std::string& str, int posArgIndex);
    void reportError(const std::string& error);
    std::string optionName(const CommandValue& value) const;
    void addArgumentValue(const CommandValue& value);

    std::string m_description;
    std::string m_programName;
    std::vector<CommandValue> m_commands;
    std::vector<CommandValue> m_posCommands;
    std::string m_errorMsg;
};

#endif
