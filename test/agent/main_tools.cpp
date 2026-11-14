// ---------------------------------------------------------------------------
// 合并后的 agent 工具测试入口: 一条用例一个进程
//
//   用法: test_agent_tools <case>
//         test_agent_tools            (列出用例)
//
// 只收**自包含**的用例(临时工作区/本地 shell/进程内 JSON-RPC, 无模型、无网络)。
// 需要真实模型的仍独立: test_agent / test_agent_compress / test_image / test_mcpclient,
// 以及 fdb_v3_text / five_chess 两个跑批程序。
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstring>

#include "JZApplication.h"

typedef int (*JzAgentTestFn)(int argc, char *argv[]);

struct JzAgentTestCase
{
    const char *name;
    JzAgentTestFn fn;
};

extern int test_code_tools(int argc, char *argv[]);
extern int test_loopdetect(int argc, char *argv[]);
extern int test_terminal_tools(int argc, char *argv[]);
extern int test_agenttoolbridge(int argc, char *argv[]);

static const JzAgentTestCase kCases[] = {
    { "code_tools", test_code_tools },
    { "loopdetect", test_loopdetect },
    { "terminal_tools", test_terminal_tools },
    { "agenttoolbridge", test_agenttoolbridge },
};

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        printf("用法: %s <case>\n可用用例:\n", argv[0]);
        for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++)
            printf("  %s\n", kCases[i].name);
        return 2;
    }

    for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++)
    {
        if (strcmp(argv[1], kCases[i].name) != 0)
            continue;

        JZApplication app;              // 每个用例一个进程, 统一建
        return kCases[i].fn(argc - 1, argv + 1);
    }

    printf("未知用例: %s\n", argv[1]);
    return 2;
}
