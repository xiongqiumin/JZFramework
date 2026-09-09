#ifndef JZ_CODE_AGENT_APPCONFIG_H_
#define JZ_CODE_AGENT_APPCONFIG_H_

#include <string>
#include <vector>

#include "JZCodeAgent.h"

// 应用配置（持久化到 exe 目录下的 code_agent.json）
struct AppConfig
{
    std::string modelUrl;
    std::string modelName;
    std::string modelToken;
    std::string workspace;
    // TS 环境目录（JS/TS 编译器所在，默认 E:/libs/ts_compiler）：
    // 应用时自动把该目录作为只读授信目录挂载，并按真实布局注入 node / tsc 可信程序。
    std::string tsEnvDir;
    // 授信目录：编译器/SDK 等安装目录，虚拟终端可只读访问、可在这里找可执行程序。
    // 不填则只能访问工作区，且没有可用的外部程序。每个目录可单独配置是否可写（默认只读）。
    std::vector<TrustedDir> trustedDirs;
    // 显式指定的可信程序（全路径），如 [{cmake, C:/.../cmake.exe}, {python, C:/.../python.exe}]。
    // 启动时会按本机默认补全缺失项（在 PATH 中探测 cmake/python/git 等并写入全路径）。
    std::vector<TrustedProgram> trustedPrograms;

    // 网络搜索：搜索接口模板(含 {q} 占位符)。空则用内置默认源（Bing RSS，format=rss）。
    std::string webSearchUrl;
    // HTTP 代理：对 http 与 https 目标均生效（https 走 CONNECT 隧道）。host 为空表示不启用。
    std::string proxyHost;
    int proxyPort = 0;
    std::string proxyUser;
    std::string proxyPassword;

    // 最近打开的工程（绝对路径，最近使用的排在最前，最多 kMaxRecentProjects 条）。
    // 启动时自动恢复第 0 条，即“默认打开上次的项目”。
    std::vector<std::string> recentProjects;
};

// 最近工程列表最大长度
enum { kMaxRecentProjects = 10 };

#endif // !JZ_CODE_AGENT_APPCONFIG_H_
