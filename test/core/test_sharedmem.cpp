#include "JZApplication.h"
#include "JZByteArray.h"
#include "JZProcess.h"
#include "JZSharedMemory.h"

#include <cstdio>
#include <cstring>
#include <string>

// ============================================================
// JZSharedMemory 独立测试 (test/core)
//   1) 同进程: create → 写 → 再 attach 读回(验证命名段 + size 反查)
//   2) 错误路径: 重复 create / attach 不存在的 key 都要失败
//   3) 跨进程: 父进程写, 用 JZProcess 起自己(`child` 模式)去 attach 读
// ============================================================

static int g_fail = 0;

static void check(bool ok, const std::string &msg)
{
    if (ok)
        printf("[ok]   %s\n", msg.c_str());
    else
    {
        printf("[fail] %s\n", msg.c_str());
        g_fail++;
    }
}

static int runChild(const std::string &key)
{
    // 子进程: 只 attach 读, 打印内容给父进程比对
    JZSharedMemory shm(key);
    if (!shm.attach())
    {
        printf("CHILD-ATTACH-FAILED: %s\n", shm.errorString().c_str());
        return 2;
    }
    printf("CHILD-DATA: %s\n", (const char *)shm.constData());
    printf("CHILD-SIZE: %d\n", shm.size());
    return 0;
}

int main(int argc, char **argv)
{
    JZApplication app;
    const std::string key = "jz_test_sharedmem";

    if (argc > 2 && std::string(argv[1]) == "child")
        return runChild(argv[2]);

    JZSharedMemory::remove(key);

    // ---- 1) 创建 + 写 ----
    JZSharedMemory a(key);
    check(a.create(4096), "create(4096): " + a.errorString());
    check(a.isAttached(), "create 后 isAttached");
    check(a.size() == 4096, "create 后 size == 4096, 实际 " + std::to_string(a.size()));
    const char *payload = "hello-shared-memory";
    if (a.data() != nullptr)
        memcpy(a.data(), payload, strlen(payload) + 1);
    else
        check(false, "create 后 data() 非空");

    // ---- 2) 同进程再 attach(另一个对象), 读回 ----
    {
        JZSharedMemory b(key);
        check(b.attach(), "attach 已存在的段: " + b.errorString());
        check(b.size() >= 4096, "attach 反查到的 size >= 4096, 实际 " + std::to_string(b.size()));
        check(b.constData() != nullptr &&
                  strcmp((const char *)b.constData(), payload) == 0,
              "attach 读到创建方写的内容");
        b.detach();
        check(!b.isAttached(), "detach 后 isAttached 为假");
    }

    // ---- 3) 错误路径 ----
    {
        JZSharedMemory dup(key);
        check(!dup.create(4096), "重复 create 应失败(段已存在)");
        check(!dup.errorString().empty(), "重复 create 有可读原因: " + dup.errorString());
    }
    {
        JZSharedMemory none("jz_test_sharedmem_不存在_" + std::to_string((long long)&app));
        check(!none.attach(), "attach 不存在的 key 应失败");
        check(!none.errorString().empty(), "attach 失败有可读原因: " + none.errorString());
    }

    // ---- 4) 跨进程: 起自己当子进程去 attach ----
    {
        // 用绝对路径, 否则 cwd 不在 PATH 里时 CreateProcess 找不到自己
        std::string exe = app.applicationFilePath();
        JZProcess p;
        if (p.start(exe, { "child", key }))
        {
            if (p.waitForFinished(15000))
            {
                JZByteArray raw = p.readAllStandardOutput();
                std::string out(raw.data(), (size_t)raw.size());
                check(out.find("CHILD-DATA: hello-shared-memory") != std::string::npos,
                      "跨进程 attach 读到父进程写的内容: " + out);
            }
            else
                check(false, "子进程 15s 未结束");
        }
        else
            check(false, "无法启动子进程: " + p.lastStartError());
    }

    // ---- 5) remove ----
    a.detach();
    check(!a.isAttached(), "父进程 detach 后 isAttached 为假");
    JZSharedMemory::remove(key);
    {
        JZSharedMemory gone(key);
        check(!gone.attach(), "remove 之后 attach 应失败");
    }

    printf("%s (fail=%d)\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
