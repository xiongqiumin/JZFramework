// ---------------------------------------------------------------------------
// 合并后的 JS 引擎测试入口: 一条用例一个进程
//
//   用法: test_jsengine <case> [case 自己的参数...]
//         test_jsengine            (列出用例)
//
// test_jscu 仍然是独立目标: 它要多编 JZJSComputerUseModule / JZComputerUse /
// JZScreenShot 并链接 jz_virtual_device + OpenCV, 不该把这套重依赖加给其它用例。
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstring>

#include "JZApplication.h"

typedef int (*JzTestFn)(int argc, char *argv[]);

struct JzTestCase
{
    const char *name;
    JzTestFn fn;
};

extern int test_jsengine(int argc, char *argv[]);
extern int test_jsnet(int argc, char *argv[]);
extern int test_jssqlite(int argc, char *argv[]);
extern int test_jsmodbus(int argc, char *argv[]);
extern int test_jsworker(int argc, char *argv[]);

#ifdef JZ_TEST_HAS_WIDGET
extern int test_jsui(int argc, char *argv[]);
extern int test_load_ui(int argc, char *argv[]);
#endif

static const JzTestCase kCases[] = {
    { "engine", test_jsengine },
    { "net", test_jsnet },
    { "sqlite", test_jssqlite },
    { "modbus", test_jsmodbus },
    { "worker", test_jsworker },
#ifdef JZ_TEST_HAS_WIDGET
    { "ui", test_jsui },
    { "load_ui", test_load_ui },
#endif
};

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        printf("用法: %s <case> [args...]\n可用用例:\n", argv[0]);
        for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++)
            printf("  %s\n", kCases[i].name);
        return 2;
    }

    for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++)
    {
        if (strcmp(argv[1], kCases[i].name) != 0)
            continue;

        JZApplication app;              // 每个用例一个进程, 统一建
        // 把用例名留在 argv[0]: 用例里沿用"程序名 + 参数"的读法(argv[1] 起才是参数)
        return kCases[i].fn(argc - 1, argv + 1);
    }

    printf("未知用例: %s\n", argv[1]);
    return 2;
}
