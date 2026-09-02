#include "JZTest.h"
#include "JZApplication.h"
#include "JZProcess.h"
#include "JZByteArray.h"
#include "JZElapsedTimer.h"
#include "JZThread.h"
#include "JZStd.h"

#include <string>
#include <vector>
#include <functional>
#include <cctype>

// ============================================================
// JZProcess 独立测试 (test/core)
// 平台说明:
//   - Linux   使用 sh / echo / cat / sleep
//   - Windows 使用 cmd /c / sort / ping (echo 为 shell 内建, 需经 cmd)
// 两条平台的测试逻辑完全一致, 仅命令构造不同(见下方平台宏)。
// ============================================================

namespace {

#if defined(_WIN32)
    const char *kShell      = "cmd";
    const char *kShellFlag  = "/c";
    const char *kTmpDir     = "C:\\";
    const char *kStdinEcho  = "sort";   // Windows 无 cat, sort 同样回显 stdin
    // Windows 无 sleep, 用 ping 制造 ~60s 长时进程 (随后被 terminate)
    std::string longRunCmd() { return "ping -n 60 127.0.0.1 > nul"; }
#else
    const char *kShell      = "sh";
    const char *kShellFlag  = "-c";
    const char *kTmpDir     = "/tmp";
    const char *kStdinEcho  = "cat";
    std::string longRunCmd() { return "sleep 30"; }
#endif

std::vector<std::string> shellArgs(const std::string &cmd)
{
    return { kShellFlag, cmd };
}

// 在事件循环驱动下等待进程结束, 同时 pump 事件以保证信号(sigFinished 等)被投递
bool pumpUntilFinished(JZProcess &p, int timeout_ms = 10000)
{
    JZElapsedTimer t;
    t.start();
    while (p.state() != JZProcess::NotRunning && t.elapsed() < timeout_ms)
    {
        jzApp->processEvents();
        JZThread::msleep(5);
    }
    for (int i = 0; i < 50; i++)
        jzApp->processEvents();
    return p.state() == JZProcess::NotRunning;
}

std::string ba2str(const JZByteArray &ba)
{
    if (ba.size() == 0)
        return std::string();
    return std::string(ba.data(), ba.size());
}

std::string strTrim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

// 去除 CRLF 并 trim (Windows echo 末尾带空格, 需 trim 才能与期望值精确相等)
std::string normOut(const JZByteArray &ba)
{
    std::string s = ba2str(ba);
    str_replace(s, "\r", "");
    str_replace(s, "\n", "");
    return strTrim(s);
}

// ---------- 1. 基本启动 / 退出码 / 标准输出 ----------
bool process_runEcho()
{
    JZProcess p;
    JZ_TEST_TRUE(p.start(kShell, shellArgs("echo hello world")));
    JZ_TEST_EQ(p.state(), JZProcess::Running);

    bool finished = false;
    int finishedCode = -1;
    connect(&p.sigFinished, [&](int code, JZProcess::ExitStatus s) {
        finished = true;
        finishedCode = code;
        JZ_TEST_EQ(s, JZProcess::NormalExit);
    });

    JZ_TEST_TRUE(pumpUntilFinished(p));
    JZ_TEST_EQ(p.exitStatus(), JZProcess::NormalExit);
    JZ_TEST_EQ(p.exitCode(), 0);
    JZ_TEST_TRUE(finished);
    JZ_TEST_EQ(finishedCode, 0);

    JZ_TEST_EQ(normOut(p.readAllStandardOutput()), "hello world");
    return true;
}

// ---------- 2. 非 0 退出码 ----------
bool process_nonZeroExit()
{
    JZProcess p;
    JZ_TEST_TRUE(p.start(kShell, shellArgs("exit 7")));
    JZ_TEST_TRUE(pumpUntilFinished(p));
    JZ_TEST_EQ(p.exitStatus(), JZProcess::NormalExit); // 正常退出(只是 code 非 0)
    JZ_TEST_EQ(p.exitCode(), 7);
    return true;
}

// ---------- 3. 向子进程写 stdin 并回读 ----------
bool process_writeStdin()
{
    JZProcess p;
    p.setProcessChannelMode(JZProcess::MergedChannels);
    JZ_TEST_TRUE(p.start(kStdinEcho));

    int64_t written = p.write(str_toUtf8("hello process"));
    JZ_TEST_GE(written, 0);
    p.closeWriteChannel(); // 关闭写端, 子进程读到 EOF 后退出

    JZ_TEST_TRUE(pumpUntilFinished(p));
    JZ_TEST_EQ(normOut(p.readAllStandardOutput()), "hello process");
    return true;
}

// ---------- 4. 分离通道: stdout / stderr 各自独立 ----------
bool process_separateChannels()
{
    JZProcess p;
    p.setProcessChannelMode(JZProcess::SeparateChannels);
    // stdout 输出 "out-line", stderr 输出 "err-line"
    JZ_TEST_TRUE(p.start(kShell, shellArgs("echo out-line & echo err-line 1>&2")));
    JZ_TEST_TRUE(pumpUntilFinished(p));

    JZ_TEST_EQ(normOut(p.readAllStandardOutput()), "out-line");
    JZ_TEST_EQ(normOut(p.readAllStandardError()), "err-line");
    return true;
}

// ---------- 5. 合并通道: stderr 被合并进 stdout ----------
bool process_mergedChannels()
{
    JZProcess p;
    p.setProcessChannelMode(JZProcess::MergedChannels);
    JZ_TEST_TRUE(p.start(kShell, shellArgs("echo out-line & echo err-line 1>&2")));
    JZ_TEST_TRUE(pumpUntilFinished(p));

    std::string out = normOut(p.readAllStandardOutput());
    JZ_TEST_TRUE(str_contains(out, "out-line"));
    JZ_TEST_TRUE(str_contains(out, "err-line"));
    JZ_TEST_EQ(p.readAllStandardError().size(), 0); // 合并模式下 stderr 为空
    return true;
}

// ---------- 6. terminate / kill 运行中进程 ----------
bool process_terminate()
{
    JZProcess p;
    JZ_TEST_TRUE(p.start(kShell, shellArgs(longRunCmd())));
    JZ_TEST_EQ(p.state(), JZProcess::Running);

    p.terminate();
    // terminate 后等待其真正退出
    JZElapsedTimer t;
    t.start();
    while (p.state() == JZProcess::Running && t.elapsed() < 5000)
    {
        jzApp->processEvents();
        JZThread::msleep(5);
    }
    JZ_TEST_EQ(p.state(), JZProcess::NotRunning);
    JZ_TEST_EQ(p.exitStatus(), JZProcess::CrashExit); // 被信号/terminate 终止
    return true;
}

// ---------- 7. 启动不存在的程序应失败并触发 sigErrorOccurred ----------
bool process_notFound()
{
    JZProcess p;
    bool errorFired = false;
    connect(&p.sigErrorOccurred, [&](JZProcess::ProcessError e) {
        errorFired = true;
        JZ_TEST_EQ(e, JZProcess::FailedToStart);
    });

    JZ_TEST_FALSE(p.start("__this_program_does_not_exist_xyz__"));
    JZ_TEST_EQ(p.state(), JZProcess::NotRunning);
    JZ_TEST_TRUE(errorFired);
    return true;
}

// ---------- 8. 工作目录 / 参数 / 状态访问器 ----------
bool process_accessors()
{
    JZProcess p;
    p.setWorkingDirectory(kTmpDir);
    JZ_TEST_EQ(p.workingDirectory(), std::string(kTmpDir));
    p.setProcessChannelMode(JZProcess::MergedChannels);
    JZ_TEST_EQ(p.processChannelMode(), JZProcess::MergedChannels);

    JZ_TEST_TRUE(p.start(kShell, shellArgs("echo x")));
    JZ_TEST_EQ(p.program(), std::string(kShell));
    JZ_TEST_EQ(p.arguments().size(), 2u);
    JZ_TEST_EQ(p.arguments()[0], std::string(kShellFlag));
    JZ_TEST_TRUE(pumpUntilFinished(p));
    return true;
}

// ---------- 9. waitForFinished 同步阻塞接口 ----------
bool process_waitForFinishedSync()
{
    JZProcess p;
    JZ_TEST_TRUE(p.start(kShell, shellArgs("echo sync-test")));
    JZ_TEST_TRUE(p.waitForFinished(10000));
    JZ_TEST_EQ(p.state(), JZProcess::NotRunning);
    JZ_TEST_EQ(p.exitCode(), 0);
    JZ_TEST_EQ(normOut(p.readAllStandardOutput()), "sync-test");
    return true;
}

} // namespace

void test_JZProcess(JZTest *test)
{
    auto suite = std::make_shared<JZTestSuite>("JZProcessTest");
    suite->addTestCase("runEcho", process_runEcho);
    suite->addTestCase("nonZeroExit", process_nonZeroExit);
    suite->addTestCase("writeStdin", process_writeStdin);
    suite->addTestCase("separateChannels", process_separateChannels);
    suite->addTestCase("mergedChannels", process_mergedChannels);
    suite->addTestCase("terminate", process_terminate);
    suite->addTestCase("notFound", process_notFound);
    suite->addTestCase("accessors", process_accessors);
    suite->addTestCase("waitForFinishedSync", process_waitForFinishedSync);
    test->addSuite(suite);
}

int main(int argc, char *argv[])
{
    JZApplication app;

    JZTest test;
    test_JZProcess(&test);
    test.run(argc, argv);
    return 0;
}
