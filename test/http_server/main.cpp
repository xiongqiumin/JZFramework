// ---------------------------------------------------------------------------
// 合并后的 HTTP 测试入口: 一条用例一个进程
//
//   用法: test_http <case>
//         test_http            (列出用例)
//
// 为什么合并: 每个小测试原来各是一个 exe, 全量构建要重复链接 JZFramework 9 次,
// 解决方案与 ctest 目标也各多一个。合并后仍然"一个用例一个进程"(ctest 粒度不变,
// 崩溃/超时只影响那一条), 只是共享同一个可执行。
//
// 约定: 用例函数不要自己 new JZApplication —— 这里统一建一个再调用。
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstring>

#include "JZApplication.h"

typedef int (*JZHttpTestFn)();

struct JZHttpTestCase
{
    const char *name;
    JZHttpTestFn fn;
};

// 各用例入口(原来的 int main(), 去掉了 JZApplication)
extern int test_static_files();
extern int test_http_all();
extern int test_compress();
extern int test_cookie();
extern int test_peer();
extern int test_route();
extern int test_routing();
extern int test_https();
extern int test_https_mtls();

static const JZHttpTestCase kCases[] = {
    { "static", test_static_files },
    { "sse_echo", test_http_all },
    { "compress", test_compress },
    { "cookie", test_cookie },
    { "peer", test_peer },
    { "route", test_route },
    { "routing", test_routing },
    { "https", test_https },
    { "https_mtls", test_https_mtls },
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

        JZApplication app;
        return kCases[i].fn();
    }

    printf("未知用例: %s\n", argv[1]);
    return 2;
}
