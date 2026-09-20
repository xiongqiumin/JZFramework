// agent_cli —— 通过 HTTP 暴露任务提交的 agent
//
// 与 agent_node 的区别:
//   agent_node 把本机能力包成 MCP server, 供远端 JZAgent 来调;
//   agent_cli 自己就跑一个 JZAgent(直接驱动本地 agent_tool 工具), 通过 HTTP
//   (POST /task) 对外暴露 —— 外部提交任务 + 验证脚本门控循环。
//
// 配置全部放 config.json(默认 exe 同目录, 可用 -c 指定); 命令行只负责:
//   -c 指定配置文件 / --init 生成默认配置 / --check 校验并打印生效配置 / -h 帮助。
//
// 日志默认不打印到控制台(避免污染), 可配置 logFile 落盘排查。

#include <stdio.h>
#include <stdlib.h>

#include <fstream>
#include <sstream>
#include <string>

#include "JZAgentCli.h"
#include "JZApplication.h"
#include "JZCommandLineParser.h"
#include "JZDir.h"
#include "JZFileInfo.h"
#include "JZJson.h"
#include "JZLog.h"
#include "JZPlatform.h"
#include "JZString.h"
#include "JZDebug.h"

using namespace std;

// ===========================================================================
// 默认配置(--init 写出的文本, 同步于 binary/agent_cli/config.json)
//   层次化: 顶层只放标识, 其余按 JZCliConfig 的主题归入子对象 —— llm / workspace /
//   tools / runtime / http。子对象内省掉前缀(如 http.bind, 而不是 http.httpBind)。
//   旧配置(全部平铺在顶层, 如 "httpBind")依然可读: 见 loadConfig 里的回落查找。
// ===========================================================================
static const char *kDefaultConfigJson = R"json({
    "agentName":            "agent_cli",
    "version":              "1.0.0",
    "logFile":              "",

    "llm": {
        "url":              "",
        "name":             "",
        "token":            "",
        "netTimeoutMs":     120000,
        "maxRounds":        100,
        "maxToken":         100000
    },

    "terminal": {
        "rootDir":          "",
        "trustedDirs":      [],
        "shellProgram":     "",
        "programs":         {},
        "recycleSec":       900,
        "allowTerminal":    true,
        "allowFileTools":   true,
        "allowPackageManager": false,
        "allowNetwork":     false,
        "unrestricted":     false
    },

    "http": {
        "bind":             "127.0.0.1",
        "port":             8080,
        "token":            "",
        "dir":              ""
    }
}
)json";

static string pathJoin(const string &dir, const string &name)
{
    if (dir.empty())
        return name;
    char last = dir.back();
    if (last == '/' || last == '\\')
        return dir + name;
    return dir + "/" + name;
}

// "D:/sdk:rw" -> {path:"D:/sdk", writable:true}(盘符冒号不能当分隔符, 只看末尾 ro/rw)
static bool parseTrusted(const string &text, TrustedDir &out)
{
    out.writable = false;
    out.path = text;
    size_t pos = text.rfind(':');
    if (pos != string::npos && pos + 1 < text.size())
    {
        string suffix = str_toLower(text.substr(pos + 1));
        if (suffix == "ro" || suffix == "rw")
        {
            out.writable = (suffix == "rw");
            out.path = text.substr(0, pos);
        }
    }
    return !out.path.empty();
}

static bool writeDefaultConfig(const string &path, string &error)
{
    if (JZFileInfo::exists(path))
    {
        error = "配置文件已存在: " + path;
        return false;
    }
    ofstream ofs(path, ios::binary);
    if (!ofs)
    {
        error = "无法写入配置文件: " + path;
        return false;
    }
    ofs << kDefaultConfigJson;
    if (!ofs)
    {
        error = "写入配置文件失败: " + path;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 分层配置的取值: 优先 "<group>.<name>"(现格式), 回落顶层 "<legacy>"(旧平铺格式)
//   例: http.bind / httpBind,  llm.url / modelUrl。legacy 为空表示没有旧写法。
//   命中才写回 out, 所以缺省值沿用 JZCliConfig 里的默认。
// ---------------------------------------------------------------------------
static const Json::Value *findConfigMember(const Json::Value &root, const string &group,
                                          const string &name, const string &legacy)
{
    if (!group.empty() && root.isMember(group) && root[group].isObject() &&
        root[group].isMember(name))
        return &root[group][name];

    if (!legacy.empty() && root.isMember(legacy))
        return &root[legacy];

    return nullptr;
}

static void readConfigString(const Json::Value &root, const string &group, const string &name,
                             const string &legacy, string &out)
{
    const Json::Value *v = findConfigMember(root, group, name, legacy);
    if (v && v->isString())
        out = v->asString();
}

static void readConfigInt(const Json::Value &root, const string &group, const string &name,
                          const string &legacy, int &out)
{
    const Json::Value *v = findConfigMember(root, group, name, legacy);
    if (v && v->isNumeric())
        out = v->asInt();
}

static void readConfigBool(const Json::Value &root, const string &group, const string &name,
                           const string &legacy, bool &out)
{
    const Json::Value *v = findConfigMember(root, group, name, legacy);
    if (v && v->isBool())
        out = v->asBool();
}

static bool loadConfig(const string &path, JZCliConfig &config, string &error)
{
    ifstream ifs(path, ios::binary);
    if (!ifs)
    {
        error = "配置文件不存在或无法打开: " + path;
        return false;
    }
    stringstream ss;
    ss << ifs.rdbuf();
    string text = ss.str();

    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(text, root))
    {
        error = "配置文件 JSON 解析失败: " + reader.getFormattedErrorMessages();
        return false;
    }
    if (!root.isObject())
    {
        error = "配置文件根必须是 JSON 对象";
        return false;
    }

    // 标识(顶层)
    JZJsonStream js(&root);
    js.readMemberDefault("agentName", config.agentName, config.agentName);
    js.readMemberDefault("version", config.version, config.version);

    // llm
    readConfigString(root, "llm", "url", "modelUrl", config.modelUrl);
    readConfigString(root, "llm", "name", "modelName", config.modelName);
    readConfigString(root, "llm", "token", "modelToken", config.modelToken);
    readConfigInt(root, "llm", "netTimeoutMs", "netTimeout", config.netTimeout);
    readConfigInt(root, "llm", "maxRounds", "maxRounds", config.maxRounds);
    readConfigInt(root, "llm", "maxToken", "maxToken", config.maxToken);

    // terminal(工作区 / shell / 会话回收 / 工具与命令策略: 都是本地终端这一摊)
    readConfigString(root, "terminal", "rootDir", "rootDir", config.rootDir);
    readConfigString(root, "terminal", "shellProgram", "shellProgram", config.shellProgram);
    readConfigInt(root, "terminal", "recycleSec", "recycleSec", config.recycleSec);
    readConfigBool(root, "terminal", "allowTerminal", "allowTerminal", config.allowTerminal);
    readConfigBool(root, "terminal", "allowFileTools", "allowFileTools", config.allowFileTools);
    readConfigBool(root, "terminal", "allowPackageManager", "allowPackageManager", config.allowPackageManager);
    readConfigBool(root, "terminal", "allowNetwork", "allowNetwork", config.allowNetwork);
    readConfigBool(root, "terminal", "unrestricted", "unrestricted", config.unrestricted);

    // 程序映射: 程序名 -> 可执行文件全路径, 如 {"tsc": "D:/app/ts_compiler/tsc/tsc.exe"}
    //   登记后: 短名字进白名单(能过审) + 执行时把短名换成这条全路径(能找到程序)。
    //   只认短名字(写全路径走路径审核); 没登记的程序 agent 就用不了(不做自动探测)。
    const Json::Value *progs = findConfigMember(root, "terminal", "programs", "programs");
    if (progs && progs->isObject())
    {
        for (Json::Value::const_iterator it = progs->begin(); it != progs->end(); ++it)
        {
            string name = str_toLower(it.key().asString());
            string path = (*it).isString() ? (*it).asString() : string();
            if (!name.empty() && !path.empty())
                config.programPaths[name] = path;
        }
    }

    // 日志(全局项, 不归终端)
    readConfigString(root, "", "logFile", "logFile", config.logFile);

    // http
    readConfigString(root, "http", "bind", "httpBind", config.httpBind);
    readConfigInt(root, "http", "port", "httpPort", config.httpPort);
    readConfigString(root, "http", "token", "httpToken", config.httpToken);
    readConfigString(root, "http", "dir", "httpDir", config.httpDir);

    const Json::Value *dirs = findConfigMember(root, "terminal", "trustedDirs", "trustedDirs");
    const Json::Value &arr = dirs ? *dirs : Json::Value::nullRef;
    if (!arr.isNull())
    {
        if (!arr.isArray())
        {
            error = "trustedDirs 必须是数组";
            return false;
        }
        for (Json::ArrayIndex i = 0; i < arr.size(); ++i)
        {
            const Json::Value &v = arr[i];
            TrustedDir dir;
            if (v.isString())
            {
                if (!parseTrusted(v.asString(), dir))
                {
                    error = "trustedDirs[" + to_string(i) + "] 目录为空";
                    return false;
                }
                config.trustedDirs.push_back(dir);
            }
            else if (v.isObject())
            {
                JZJsonStream ds(&v);
                if (!ds.readMember("path", dir.path) || dir.path.empty())
                {
                    error = "trustedDirs[" + to_string(i) + "].path 缺失或为空";
                    return false;
                }
                ds.readMemberDefault("writable", dir.writable, false);
                config.trustedDirs.push_back(dir);
            }
            else
            {
                error = "trustedDirs[" + to_string(i) + "] 必须是字符串或对象";
                return false;
            }
        }
    }
    return true;
}

static void printConfig(const string &configPath, const JZCliConfig &config)
{
    string trusted;
    for (size_t i = 0; i < config.trustedDirs.size(); i++)
    {
        if (i)
            trusted += ", ";
        trusted += config.trustedDirs[i].path + (config.trustedDirs[i].writable ? "(rw)" : "(ro)");
    }
    // 打到 stderr: stdout 是 ACP 通道, 不能污染
    fprintf(stderr, "配置文件: %s\n", configPath.c_str());
    fprintf(stderr, "  代理名    %s v%s\n", config.agentName.c_str(), config.version.c_str());
    fprintf(stderr, "  模型      %s @ %s\n",
            config.modelName.empty() ? "(未配置)" : config.modelName.c_str(),
            config.modelUrl.empty() ? "(未配置)" : config.modelUrl.c_str());
    fprintf(stderr, "  工作区    %s\n",
            (config.rootDir.empty() ? string(JZDir::currentPath() + "  (配置为空, 取进程目录)") : config.rootDir).c_str());
    fprintf(stderr, "  授信目录  %s\n", trusted.empty() ? "(无)" : trusted.c_str());
    fprintf(stderr, "  工具      终端=%s  文件=%s\n",
            config.allowTerminal ? "开" : "关", config.allowFileTools ? "开" : "关");
    fprintf(stderr, "  命令策略  包管理=%s  联网=%s  权限=%s\n",
            config.allowPackageManager ? "开" : "关",
            config.allowNetwork ? "开" : "关",
            config.unrestricted ? "全部不检查" : "按区域分档");
    string programs;
    for (const auto &kv : config.programPaths)
    {
        if (!programs.empty())
            programs += ", ";
        programs += kv.first + "=" + kv.second;
    }
    fprintf(stderr, "  shell     %s\n", config.shellProgram.empty() ? "自动探测" : config.shellProgram.c_str());
    fprintf(stderr, "  程序映射  %s\n",
            programs.empty() ? "(空: agent 只能用授信目录里的程序)" : programs.c_str());
    fprintf(stderr, "  日志文件  %s\n", config.logFile.empty() ? "(不打印到控制台)" : config.logFile.c_str());
}

int main(int argc, char *argv[])
{
    JZCommandLineParser parser;
    parser.setDescription("agent_cli - HTTP task agent (POST /task)\n");

    string cmdConfig;
    parser.addOption(JZCommandLineOption("--config", "-c",
        "配置文件路径(默认 exe 同目录 config.json)"), &cmdConfig);

    bool cmdInit = false;
    parser.addOption(JZCommandLineOption("--init", "",
        "生成默认 config.json 后退出(不启动)"), &cmdInit);

    bool cmdCheck = false;
    parser.addOption(JZCommandLineOption("--check", "",
        "校验配置并打印生效配置后退出(不启动)"), &cmdCheck);

    if (!parser.parse(argc, argv))
        return 1;

    string configPath = cmdConfig.empty()
        ? pathJoin(JZPlatform::applicationDirPath(), "config.json")
        : cmdConfig;

    if (cmdInit)
    {
        string err;
        if (!writeDefaultConfig(configPath, err))
        {
            fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        fprintf(stderr, "已生成默认配置文件: %s\n", configPath.c_str());
        return 0;
    }

    JZCliConfig config;
    string err;
    if (!loadConfig(configPath, config, err))
    {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    if (cmdCheck)
    {
        printConfig(configPath, config);
        return 0;
    }

    JZApplication app;

    JZAgentCli cli;
    if (!cli.init(config, err))
    {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    jzDebug("agent_cli 就绪(HTTP): http://%s:%d  token=%s",
            config.httpBind.c_str(), config.httpPort,
            config.httpToken.empty() ? "(无)" : "(已启用)");
    cli.startHttp();

    return app.exec();
}
