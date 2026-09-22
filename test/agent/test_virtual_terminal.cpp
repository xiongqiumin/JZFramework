// JZVirtualTerminal 冒烟测试(不依赖 JZProcess 的那部分)
//
// 覆盖: 单终端 / 内建文件动作 / **区域审核**(越界必须被拒) / 审计回调 / 程序白名单 /
//       **并发运行**(同一终端同时跑多条命令 + 按 run_id 精确 cancel + cancelAll) /
//       **后台运行** / **进程树收尾**(停一条命令要连它的子孙一起收, 不留占端口的孤儿)。
// run() 形态: JZResult<std::string>(成功 = run_id, 失败 = 原因) —— 测试里统一走 runCmd() 包装。
// 期望输出: ALL PASS
//
// 注: JZVirtualTerminal 现在就是"一个终端"(无多会话), cwd/env/在跑进程都是它自己的状态;
//     一个终端可以同时跑多条命令(每条一个 JZProcess, 用 run_id 区分)。

#include <stdio.h>
#include <string>
#include <utility>

#include "JZApplication.h"
#include "JZDir.h"
#include "JZFile.h"
#include "JZFileInfo.h"
#include "JZTimer.h"
#include "JZVirtualTerminal.h"

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#endif

using namespace std;

static int g_fail = 0;

// 枚举当前所有进程的 (pid, 父 pid) —— 用来验证"停一条命令有没有把它的子孙一起收掉"。
// 只测这个用途, 所以尽量简单: Windows 走 Toolhelp32, Linux 扫 /proc/<pid>/stat。
static vector<pair<unsigned long, unsigned long>> listProcesses()
{
    vector<pair<unsigned long, unsigned long>> out;
#ifdef _WIN32
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return out;
    PROCESSENTRY32 pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe))
    {
        do
        {
            out.push_back(make_pair((unsigned long)pe.th32ProcessID, (unsigned long)pe.th32ParentProcessID));
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
#else
    DIR *d = ::opendir("/proc");
    if (!d)
        return out;
    while (dirent *e = ::readdir(d))
    {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        unsigned long pid = ::strtoul(e->d_name, nullptr, 10);
        string path = string("/proc/") + e->d_name + "/stat";
        FILE *f = ::fopen(path.c_str(), "r");
        if (!f)
            continue;
        char buf[512] = { 0 };
        size_t n = ::fread(buf, 1, sizeof(buf) - 1, f);
        ::fclose(f);
        buf[n] = 0;
        char *r = ::strrchr(buf, ')');   // comm 可能含空格/括号, 取最后一个 ')'
        unsigned long ppid = 0;
        char state = 0;
        if (r)
            ::sscanf(r + 1, " %c %lu", &state, &ppid);
        out.push_back(make_pair(pid, ppid));
    }
    ::closedir(d);
#endif
    return out;
}

// rootPid 的子孙进程个数(不递归只看一层是不够的: git-bash 会再套一层 bash)
static int descendantCount(unsigned long rootPid)
{
    vector<pair<unsigned long, unsigned long>> all = listProcesses();
    vector<unsigned long> parents;
    parents.push_back(rootPid);

    int count = 0;
    bool grew = true;
    while (grew)
    {
        grew = false;
        for (size_t i = 0; i < all.size(); i++)
        {
            unsigned long pid = all[i].first;
            unsigned long ppid = all[i].second;
            bool known = false;
            for (size_t k = 0; k < parents.size() && !known; k++)
                known = (parents[k] == pid);
            if (known)
                continue;
            for (size_t k = 0; k < parents.size(); k++)
            {
                if (parents[k] == ppid)
                {
                    parents.push_back(pid);
                    count++;
                    grew = true;
                    break;
                }
            }
        }
    }
    return count;
}

static void check(bool ok, const string &what)
{
    printf("%s %s\n", ok ? "[ok]  " : "[fail]", what.c_str());
    if (!ok)
        g_fail++;
}

static bool contains(const string &text, const string &needle)
{
    return text.find(needle) != string::npos;
}

// 只为验证"短名 -> 全路径"的替换结果: expandProgramPaths 是 protected, 派生类才够得着
class ExpandProbe : public JZVirtualTerminal
{
public:
    string expand(const string &command)
    {
        JZShell::Line line;
        string err;
        if (!JZShell::parse(command, line, err))
            return string();
        return expandProgramPaths(line, command);
    }
};

static int g_auditCount = 0;
static string g_lastAuditAction;

class TerminalTest
{
public:
    int run()
    {
        // 用一个自己的临时工作区, 免得污染仓库
        m_ws = jzApp->applicationDirPath() + "/vt_test_ws";
        JZDir(m_ws).removeRecursively();
        JZDir().mkpath(m_ws);

        JZVirtualTerminalConfig cfg;
        cfg.zone.workspace = m_ws;
        m_term.setTerminalConfig(cfg);   // setTerminalConfig 会把 cwd 拉到工作区根
        m_term.setAuditCallback([](const string &action, const JZVirtualTerminal::Verdict &v, const string &)
        {
            g_auditCount++;
            g_lastAuditAction = action + (v.allowed() ? ":allow" : ":deny");
        });

        // 注意: run() 用的 echo **不手动 addProgram** —— 基础 shell 命令必须在默认表里。

        printf("     workspace=%s\n\n", m_ws.c_str());

        // 兜底: 回调没来也不能把测试挂死(异步步骤一旦断了, exec() 会永不返回)
        JZTimer watchdog;
        connect(&watchdog.sigTimeout, [this]()
                {
                    g_fail++;
                    printf("[fail] 超时: 20 秒还没跑完\n");
                    jzApp->quit();
                });
        watchdog.start(20000);

        stepWrite();
        return jzApp->exec();
    }

private:
    void stepWrite()
    {
        string error;
        // 父目录不存在应该自动建出来
        check(m_term.writeFile("a/b.txt", "hello\nworld\n", false, error),
              "write 相对路径(自动建父目录): " + error);
        check(JZFileInfo(m_ws + "/a/b.txt").isFile(), "文件确实落盘了");
        stepRead();
    }

    void stepRead()
    {
        string text, error;
        check(m_term.readFile("a/b.txt", text, error), "read 相对路径: " + error);
        check(contains(text, "hello"), "内容正确");
        stepList();
    }

    void stepList()
    {
        vector<string> names;
        string error;
        check(m_term.listDir("a", names, error), "ls: " + error);
        check(names.size() == 1 && names[0] == "b.txt", "ls 列出 b.txt");

        names.clear();
        check(m_term.listDir(".", names, error), "ls .");
        check(names.size() == 1 && names[0] == "a/", "ls . 里目录带 '/'");
        stepEdit();
    }

    void stepEdit()
    {
        string error;
        check(m_term.editFile("a/b.txt", "world", "there", false, error), "edit 唯一匹配: " + error);

        string text;
        m_term.readFile("a/b.txt", text, error);
        check(contains(text, "there") && !contains(text, "world"), "edit 生效");

        // "l" 出现多次 ⇒ 不唯一, 必须拒绝
        check(!m_term.editFile("a/b.txt", "l", "L", false, error), "edit 匹配多处被拒");
        stepCpMv();
    }

    void stepCpMv()
    {
        string error;
        check(m_term.makeDir("c/d", error), "mkdir -p: " + error);
        check(m_term.copyPath("a/b.txt", "c/d/e.txt", error), "cp: " + error);

        string text;
        check(m_term.readFile("c/d/e.txt", text, error) && contains(text, "there"), "cp 内容一致");

        check(m_term.movePath("c/d/e.txt", "c/f.txt", error), "mv: " + error);
        check(!m_term.readFile("c/d/e.txt", text, error), "源已不在");
        check(m_term.readFile("c/f.txt", text, error), "目标在");
        stepRm();
    }

    void stepRm()
    {
        string error;
        check(m_term.removePath("c/f.txt", false, error), "rm 文件: " + error);

        error.clear();
        check(m_term.removePath("c/d", false, error), "rmdir 空目录: " + error);

        error.clear();
        check(!m_term.removePath("a", false, error), "rm 非空目录(未 recursive)被拒");

        error.clear();
        check(m_term.removePath("a", true, error), "rm -r");
        check(!JZFileInfo::exists(m_ws + "/a"), "目录真的没了");
        stepEscape();
    }

    // ---- 重点: 区域审核 ----
    void stepEscape()
    {
        string text, error;

        // 相对路径用 ".." 逃出工作区根: strict 解析会直接拒(防止越界)
        check(!m_term.readFile("../../../../Windows/win.ini", text, error),
              "相对路径越界(..逃出根)被拒");
        // 绝对路径在工作区外: 区外默认**只读**(设计如此 —— 编译要读 /usr/include),
        // 所以"读"是允许的; 真正被拦的是"写/删区外"(见下)。这条区分是区域模型的本意,
        // 别和"相对路径越界"混为一谈。
        check(m_term.readFile("C:/Windows/System32/drivers/etc/hosts", text, error),
              "读区外文件允许(只读, 设计如此): " + error);
        // 写一样被拦
        check(!m_term.writeFile("C:/Windows/_vt_probe.txt", "x", false, error),
              "越界写被拒");
        // 删也一样
        check(!m_term.removePath("C:/Windows/notepad.exe", false, error),
              "越界删被拒");

        JZVirtualTerminal::Verdict v = m_term.checkRead("C:/Windows/System32/drivers/etc/hosts");
        check(v.allowed(), "checkRead: 区外=只读允许(写/删才拦, 不是 PathOutside)");

        v = m_term.checkWrite("C:/Windows/x.txt");
        check(!v.allowed(), "checkWrite 越界被拒");
        stepCd();
    }

    void stepCd()
    {
        string error;
        // ws 的父目录在区域外
        check(!m_term.changeDir("..", error), "cd .. 到工作区外被拒");

        check(m_term.makeDir("sub", error), "建 sub");
        check(m_term.changeDir("sub", error), "cd sub: " + error);

        string cwd;
        check(m_term.printDir(cwd) && cwd == m_ws + "/sub", "cwd 已经变了");

        // 相对路径现在应该基于新 cwd
        check(m_term.writeFile("g.txt", "x", false, error), "以新 cwd 为基准写文件");
        check(JZFileInfo(m_ws + "/sub/g.txt").isFile(), "文件落在 sub 下");
        stepExec();
    }

    void stepExec()
    {
        check(m_term.checkExec("git").allowed(), "git 在白名单且默认放行");
        check(!m_term.checkExec("reg").allowed(), "reg(SystemAdmin)永不放行");

        JZVirtualTerminal::Verdict v = m_term.checkExec("npm");
        check(!v.allowed() && v.reason == DenyReason::EnvChange, "npm 默认拒绝(装东西)");

        v = m_term.checkExec("git", { "-C", "C:/x" });
        check(!v.allowed(), "git -C 参数级黑名单命中");

        v = m_term.checkExec("git", { "status" });
        check(v.allowed(), "git status 放行");

        check(!m_term.checkExec("some_unknown_prog").allowed(), "白名单外的程序被拒");

        // 基础 shell 命令必须**默认**放行 —— 文件工具删掉之后, 模型就靠这些干活。
        // 这里刻意不手动 addProgram: 那会掩盖"默认表缺项"这个真正的毛病。
        check(m_term.checkExec("ls").allowed(), "ls 默认放行(基础 shell 类)");
        check(m_term.checkExec("cat").allowed(), "cat 默认放行");
        check(m_term.checkExec("mkdir").allowed(), "mkdir 默认放行");
        check(m_term.checkExec("rm").allowed(), "rm 默认放行");
        check(m_term.checkExec("grep").allowed(), "grep 默认放行");
        check(m_term.checkExec("head").allowed(), "head 默认放行(基础 shell 类)");

        // ---- 程序映射(运行时可配, 动态登记) ----
        // 场景: tsc / code_runtime 这类"随部署走、不在 PATH 里"的程序, 默认表里没有;
        // 经 JZVirtualTerminalConfig::programPaths 登记后应能审过、能查到全路径。
        {
            JZVirtualTerminalConfig saved = m_term.terminalConfig();
            check(!m_term.checkExec("my_tool").allowed(), "登记前: my_tool 不在白名单(被拒)");

            JZVirtualTerminalConfig cfg = saved;
            cfg.programPaths["my_tool"] = "C:/tools/my_tool.exe";
            m_term.setTerminalConfig(cfg);

            check(m_term.checkExec("my_tool").allowed(), "登记后: my_tool 进白名单(命令名可审过)");
            check(m_term.programPath("my_tool") == "C:/tools/my_tool.exe", "登记后: 能查到可执行文件全路径");

            // 映射只认短名字: 目录**不**进授信目录, 写全路径仍按路径审核(区外拒)
            bool hasDir = false;
            for (const TrustedDir &d : m_term.terminalConfig().zone.trustedDirs)
                if (d.path.find("C:/tools") != string::npos)
                    hasDir = true;
            check(!hasDir, "登记后: 可执行文件所在目录不进授信目录(只认短名字)");
            check(!m_term.checkExec("C:/tools/my_tool.exe").allowed(),
                  "登记后: 写全路径走路径审核(区外被拒)");

            // 真正交给 shell 的那条命令: 审过之后把短名原地换成全路径(不再注入 PATH)
            ExpandProbe probe;
            JZVirtualTerminalConfig pcfg = probe.terminalConfig();
            pcfg.programPaths["my_tool"] = "C:/tools/my_tool.exe";
            probe.setTerminalConfig(pcfg);
            check(probe.expand("my_tool -p .") == "C:/tools/my_tool.exe -p .",
                  "执行串: 短名换成全路径");
            check(probe.expand("cd src && my_tool -p .") == "cd src && C:/tools/my_tool.exe -p .",
                  "执行串: && 后面的短名也换");
            check(probe.expand("ls | my_tool") == "ls | C:/tools/my_tool.exe",
                  "执行串: 管道里的短名也换");
            check(probe.expand("echo my_tool") == "echo my_tool",
                  "执行串: 参数位置上的同名不换(按位置替换, 不是字符串硬找)");
            check(probe.expand("ls") == "ls", "执行串: 没配映射的原样不动");

            m_term.setTerminalConfig(saved);   // 还原, 不影响后续步骤
        }
        stepRun();
    }

    // run() 现在返回 JZResult<std::string>(成功 = run_id, 失败 = 原因)。测试里统一走这个包装,
    // 保持"started + error"的原写法 —— 否则几十处断言都得改成 JZResult 判定。
    bool runCmd(const string &command, int timeoutMs, const JZVirtualTerminal::RunCallback &cb, string &error,
                string *runId = nullptr,
                const std::function<void(const string &, const string &)> &onChunk = nullptr,
                bool background = false,
                JZVirtualTerminal::OutputFilter filter = JZVirtualTerminal::OutputFilter::Default)
    {
        JZResult<string> r = m_term.run(command, timeoutMs, cb, onChunk, background, filter);
        if (r.isError())
        {
            error = r.errorMessage();
            return false;
        }
        if (runId)
            *runId = r.value();
        return true;
    }

    // ---- run(): 真起进程(异步) ----
    // 这是"模型全走终端命令"的立足点: 没有它, 文件工具一删模型就什么都做不了。
    void stepRun()
    {
        // 命令是整条交给这个 shell 的。Windows 上必须是 Git Bash —— 找不到就**报错**,
        // 不退回 cmd(退回会让同一句话两边行为不一样, 更难查)。
        printf("     shell=%s\n", m_term.shellProgram().c_str());
        check(!m_term.shellProgram().empty(),
              "探测到可用的 shell: " + m_term.shellProgram());

        string error;
        bool started = runCmd("echo jz_terminal_ok", 20000,
                                  [this](bool ok, const string &out, const string &err)
                                  {
                                      check(ok, "run echo 正常结束" +
                                                (err.empty() ? string() : ": " + err));
                                      check(contains(out, "jz_terminal_ok"),
                                            "run 拿到 stdout: " + out);
                                      stepRunDeny();
                                  }, error);
        check(started, "run 启动命令: " + error);
        if (!started)
            stepRunDeny();          // 别把后面几步挂住
    }

    void stepRunDeny()
    {
        string error;
        // 白名单外的程序: 必须在**起进程之前**就被拒
        check(!runCmd("some_unknown_prog --do-it", 20000,
                          [](bool, const string &, const string &) {}, error),
              "run 拒绝白名单外的程序");

        // 重定向目标在工作区外: 也要拒(zone 模型的写边界就靠这条)
        string outside = jzApp->applicationDirPath() + "/vt_outside.txt";
        JZFile::remove(outside);
        check(!runCmd("echo x > " + outside, 20000,
                          [](bool, const string &, const string &) {}, error),
              "run 拒绝越界的重定向: " + error);
        check(!JZFileInfo(outside).exists(), "越界文件确实没被创建");

        // 单独一条设/清变量: 不跨命令保留, 必须**明确报错**(否则模型会以为设过了)
        check(!runCmd("export JZ_X=1", 20000,
                          [](bool, const string &, const string &) {}, error),
              "run 拒绝单独一条 export");
        check(!runCmd("JZ_X=1", 20000,
                          [](bool, const string &, const string &) {}, error),
              "run 拒绝单独一条赋值");
        check(!runCmd("unset PATH", 20000,
                          [](bool, const string &, const string &) {}, error),
              "run 拒绝单独一条 unset");

        stepRunShell();
    }

    // 真正要验证的: 用 shell 的 coreutils 干活 —— 文件工具删掉之后模型靠的就是这些。
    // 顺带验证 `&&` 串联(整条交给 shell, 我们不解析)。
    void stepRunShell()
    {
        string error;
        bool started = runCmd("ls -1 && mkdir -p deep/dir && ls -1", 20000,
                                  [this](bool ok, const string &out, const string &err)
                                  {
                                      check(ok, "run ls/mkdir 链式: " +
                                                (err.empty() ? string() : err));
                                      check(contains(out, "g.txt") && contains(out, "deep"),
                                            "shell 的 ls/mkdir 真的生效: " + out);
                                      check(JZFileInfo(m_ws + "/sub/deep/dir").isDir(),
                                            "mkdir -p 的目录真的建出来了");
                                      stepRunCond();
                                  }, error);
        check(started, "run shell coreutils: " + error);
        if (!started)
            stepRunCond();
    }

    // `[[ ]]`(bash 条件表达式): 审核面把它当普通命令 —— 靠的是白名单里有 "[[",
    // 与用哪个 shell 无关; 但 **只有 bash 有**这个内建, 回退到 dash/busybox 时运行期是 127。
    // 所以分开断言: "过审"必须成立(任何 shell), "真跑出分支"只在探测到 bash 时要求。
    void stepRunCond()
    {
        string error;
        const bool isBash = m_term.shellProgram().find("bash") != string::npos;
        bool started = runCmd("[[ -n \"x\" ]] && echo jz_cond_ok", 20000,
                                  [this, isBash](bool ok, const string &out, const string &err)
                                  {
                                      if (isBash)
                                      {
                                          check(ok, "[[ ]] 正常结束: " + err);
                                          check(contains(out, "jz_cond_ok"),
                                                "[[ ]] 条件成立真的进了分支: " + out);
                                      }
                                      stepRunInlineEnv();
                                  }, error);
        check(started, "[[ ]] 过审(白名单里有 [[): " + error);
        if (!started)
            stepRunInlineEnv();
    }

    // 内联写变量是**合法**的(在那一条命令内有效) —— 这就是"不做会话级 env"的替代写法
    void stepRunInlineEnv()
    {
        string error;
        bool started = runCmd("export JZ_X=42 && echo val=$JZ_X", 20000,
                                  [this](bool ok, const string &out, const string &err)
                                  {
                                      check(ok, "内联 export 那条命令本身正常: " + err);
                                      check(contains(out, "val=42"),
                                            "变量在那条命令内部生效: " + out);
                                      stepRunCdChain();
                                  }, error);
        check(started, "run 内联 export: " + error);
        if (!started)
            stepRunCdChain();
    }

    // 链式 `cd build && cmd`: 交给 shell —— 那一条命令内 cd 有效, 与真 shell 语义一致
    void stepRunCdChain()
    {
        string error;
        bool started = runCmd("cd deep && pwd", 20000,
                                  [this](bool ok, const string &out, const string &err)
                                  {
                                      check(ok, "链式 cd 那条命令本身正常: " + err);
                                      check(contains(out, "deep"),
                                            "cd 在该条命令内生效: " + out);

                                      // ⭐ 但链式 cd **不**改终端 cwd —— 这是**文档化的行为**, 不是漏。
                                      // 每条命令是一个新进程, cd 只活在那个 bash 里(它跑完就没了)。
                                      // 想要持久就单独发一条 `cd <目录>`(见下一步)。
                                      JZVirtualTerminal::SessionInfo info = m_term.info();
                                      check(!contains(info.cwd, "deep"),
                                            "链式 cd 不改终端 cwd(下一条命令仍在原目录): " +
                                                info.cwd);
                                      stepRunCdPersist();
                                  }, error);
        check(started, "run 链式 cd(现在 cd 必须在白名单里): " + error);
        if (!started)
            stepRunCdPersist();
    }

    // 单独一条 `cd <目录>`: 现在**直接拒绝**(设计变了 —— 每条命令都是新 shell, 单独 cd
    // 什么也留不下; 真要跨命令待在某目录, 用文件工具的 changeDir 或命令自带 `-C` 参数)。
    void stepRunCdPersist()
    {
        string error;
        // run("cd deep"): 同步拒(起进程之前), 回调不会被调到, started=false
        bool started = runCmd("cd deep", 20000,
                                  [](bool, const string &, const string &) {}, error);
        check(!started, "run 拒绝单独一条 cd(应改用 `cd x && cmd` 或工具 -C 参数): " + error);
        check(contains(error, "单独的 cd"), "拒绝信息点明正确写法");

        // 被拒的 cd 当然没改终端 cwd —— 它仍然是最初 changeDir 持久化的 sub
        JZVirtualTerminal::SessionInfo info = m_term.info();
        check(contains(info.cwd, "sub"), "被拒的 cd 没动终端 cwd(仍是 sub): " + info.cwd);
        stepRunPwd();
    }

    // 终端 cwd 反映的就是 changeDir 持久化的那个(单独 cd 不持久)
    void stepRunPwd()
    {
        string error;
        bool started = runCmd("pwd", 20000,
                                  [this](bool ok, const string &out, const string &err)
                                  {
                                      check(ok, "pwd 正常: " + err);
                                      check(contains(out, "sub"),
                                            "pwd 反映 changeDir 持久化的 cwd(sub): " + out);
                                      stepRunAuditDeny();
                                  }, error);
        check(started, "run pwd: " + error);
        if (!started)
            stepRunAuditDeny();
    }

    // 审查: "脱离进程管理"与"审不下去"的写法一律**先拒后起**
    void stepRunAuditDeny()
    {
        string error;
        struct Case
        {
            const char *cmd;
            const char *what;
        };
        const Case cases[] = {
            { "nohup cmake ..",         "nohup(脱离管理)" },
            { "setsid cmake ..",        "setsid(脱离管理)" },
            { "cmake .. &",             "结尾的后台 `&`" },
            { "echo hi & echo bye",     "中间的后台 `&`" },
            { "echo $(whoami)",         "$( ) 命令替换" },
            { "echo `whoami`",          "反引号命令替换" },
            { "cat <<EOF",              "heredoc(<<)" },
            { "diff <(ls) <(ls)",       "进程替换 <( )" },
            { "echo \"a $(whoami) b\"", "双引号里的 $( ) (仍会展开)" },
        };

        for (const Case &c : cases)
        {
            check(!runCmd(c.cmd, 20000,
                              [](bool, const string &, const string &) {}, error),
                  string("拒绝 ") + c.what);
        }

        // 反过来: `&&` 不能被误当成后台 `&`。
        // 注意 check 的消息是**提前**拼好的, 所以只在失败时才带 error(否则会带上一次拒绝的旧文本)
        bool andOk = runCmd("echo a && echo b", 20000,
                                [this](bool ok, const string &out, const string &err)
                                {
                                    check(ok, "&& 那条命令本身正常: " + err);
                                    check(contains(out, "a") && contains(out, "b"),
                                          "&& 两边都跑了: " + out);
                                    stepRunSingleQuoteLiteral();
                                }, error);
        check(andOk, "run 允许 &&" + (andOk ? string() : ": " + error));
        if (!andOk)
            stepRunSingleQuoteLiteral();
    }

    // 单引号里的是**字面量**(真 shell 也不展开) —— 上面 $( ) 那条规则不能误伤它
    void stepRunSingleQuoteLiteral()
    {
        string error;
        bool started = runCmd("echo 'a $(whoami) b'", 20000,
                                  [this](bool ok, const string &out, const string &err)
                                  {
                                      check(ok, "单引号里的 $( ) 只是字面量: " + err);
                                      check(contains(out, "$(whoami)"),
                                            "原样输出, 没被展开: " + out);
                                      stepRunPipe();
                                  }, error);
        check(started, "run 单引号字面量: " + error);
        if (!started)
            stepRunPipe();
    }

    // 管道 `|`: 拆段逐段审, 每段程序在白名单就放行(与 `grep ... | head` 同形态)。
    // 之前有人怀疑 `|` 会触发 "arguments must be a valid JSON string" 或命令被拒 —— 这里坐实不会。
    void stepRunPipe()
    {
        string error;
        bool started = runCmd("printf 'a\\nb\\nc\\n' | head -n 1", 20000,
                                  [this](bool ok, const string &out, const string &err)
                                  {
                                      check(ok, "run 管道 | 命令正常: " + err);
                                      check(contains(out, "a"),
                                            "管道两端都跑通(grep...|head 同形态): " + out);
                                      stepRunConcurrent();
                                  }, error);
        check(started, "run 管道命令启动: " + error);
        if (!started)
            stepRunConcurrent();
    }

    // ⭐ 并发: 同一个终端**同时**跑多条命令。以前第二条会被"这个终端还有命令在跑"直接拒掉,
    //    现在两条各跑各的(都 sleep 1s, 所以确实是同时在跑, 不是排队), 各自的输出/回调/run_id 不串。
    void stepRunConcurrent()
    {
        string error;
        m_left = 2;   // 两条(起失败的那条也会同步回调, 计数照样对得上)
        bool startedA = runCmd("echo conc_a && sleep 1 && echo conc_a_done", 20000,
                                   [this](bool ok, const string &out, const string &err)
                                   {
                                       check(ok, "并发 A 正常结束: " + err);
                                       m_outA = out;
                                       if (--m_left == 0)
                                           stepRunConcurrentCheck();
                                   }, error, &m_ridA);
        bool startedB = runCmd("echo conc_b && sleep 1 && echo conc_b_done", 20000,
                                   [this](bool ok, const string &out, const string &err)
                                   {
                                       check(ok, "并发 B 正常结束: " + err);
                                       m_outB = out;
                                       if (--m_left == 0)
                                           stepRunConcurrentCheck();
                                   }, error, &m_ridB);

        check(startedA && startedB, "同一终端可以同时起两条命令: " + error);
        check(!m_ridA.empty() && !m_ridB.empty() && m_ridA != m_ridB,
              "两条运行各自一个 run_id: " + m_ridA + " / " + m_ridB);
        check(m_term.info().runningCount == 2, "状态里是 2 条在跑");
        check(m_term.activeRunIds().size() == 2, "activeRunIds() 列出两条在跑");

        if (m_left <= 0)
            stepRunConcurrentCheck();
    }

    void stepRunConcurrentCheck()
    {
        if (m_concDone)
            return;
        m_concDone = true;

        check(contains(m_outA, "conc_a_done"), "A 的输出归 A: " + m_outA);
        check(contains(m_outB, "conc_b_done"), "B 的输出归 B: " + m_outB);
        check(!contains(m_outA, "conc_b") && !contains(m_outB, "conc_a"),
              "两条输出没有串到对方身上");
        check(m_term.info().runningCount == 0, "跑完就都摘掉了(0 条在跑)");

        // 运行记录也各留各的(terminal_query 按 run_id 取)
        JZVirtualTerminal::RunRecordInfo ra, rb;
        check(m_term.runSnapshot(m_ridA, ra) && contains(ra.command, "conc_a"),
              "run_id A 的记录是 A 那条命令");
        check(m_term.runSnapshot(m_ridB, rb) && contains(rb.command, "conc_b"),
              "run_id B 的记录是 B 那条命令");

        stepRunCancelOne();
    }

    // 按 run_id 精确停: 停掉一条长任务, 同一终端里另一条照跑、照收尾。
    void stepRunCancelOne()
    {
        string error;
        m_left = 2;
        bool startedLong = runCmd("sleep 30", 60000,
                                      [this](bool ok, const string &out, const string &err)
                                      {
                                          check(!ok, "被 cancel 的那条以失败收尾: " + err);
                                          m_cancelOut = out;
                                          if (--m_left == 0)
                                              stepRunCancelCheck();
                                      }, error, &m_ridLong);
        bool startedPeer = runCmd("echo cancel_peer_ok", 20000,
                                      [this](bool ok, const string &out, const string &err)
                                      {
                                          check(ok, "同终端里另一条不受影响: " + err);
                                          m_peerOut = out;
                                          if (--m_left == 0)
                                              stepRunCancelCheck();
                                      }, error, &m_ridPeer);

        check(startedLong && startedPeer, "起一长一短两条并发命令: " + error);
        check(m_term.cancel(m_ridLong), "cancel(run_id) 停掉指定的那条");
        check(m_term.info().runningCount <= 1, "被停的那条很快摘掉(最多剩另一条)");

        if (m_left <= 0)
            stepRunCancelCheck();
    }

    void stepRunCancelCheck()
    {
        if (m_cancelDone)
            return;
        m_cancelDone = true;

        check(contains(m_peerOut, "cancel_peer_ok"), "另一条正常跑完: " + m_peerOut);
        check(m_term.info().runningCount == 0, "两条都收尾干净(0 条在跑)");

        stepRunCancelAll();
    }

    // cancelAll(): 一次停掉本终端所有在跑的命令(返回停掉几条, 各自照常收尾)。
    void stepRunCancelAll()
    {
        string error;
        m_left = 2;
        m_killed = 0;
        auto cb = [this](bool ok, const string &, const string &err)
        {
            check(!ok, "cancelAll 停掉的那条以失败收尾: " + err);
            m_killed++;
            if (--m_left == 0)
                stepRunCancelAllCheck();
        };
        bool startedA = runCmd("sleep 30", 60000, cb, error);
        bool startedB = runCmd("sleep 30", 60000, cb, error);
        check(startedA && startedB, "起两条长任务: " + error);

        check(m_term.cancelAll() == 2, "cancelAll() 返回停掉的条数(2)");

        // ⚠ "还剩下几条在跑" 只能在收尾闸里断言: kill 是同步收尾的, 上面这一行就可能把整条链
        //    (包括下一步新起的命令)全跑完, 那时再看 runningCount 已经不是这一阶段的状态了。
        if (m_left <= 0)
            stepRunCancelAllCheck();
    }

    void stepRunCancelAllCheck()
    {
        if (m_cancelAllDone)
            return;
        m_cancelAllDone = true;

        check(m_killed == 2, "被停的两条各自都收到了收尾回调: " + to_string(m_killed));
        check(m_term.info().runningCount == 0, "cancelAll 之后一条都不剩");

        stepRunBackground();
    }

    // ⭐ 后台运行 runBackground(): 就是"起服务"这类**自己不退出**的长程任务的跑法。
    //   命令还是普通命令(同样过审、同样由本终端起进程), 区别只在: 启动成功就返回 run_id,
    //   不等它结束、也不设超时 —— 以前只能前台干等, 撞满 timeout_ms 还会把服务杀掉。
    void stepRunBackground()
    {
        m_bgCalledBack = false;
        JZResult<string> started = m_term.runBackground(
            "echo bg_start && sleep 30 && echo bg_end",
            [this](bool, const string &, const string &) { m_bgCalledBack = true; });
        check(!started.isError(), "runBackground 启动成功" +
              (started.isError() ? ": " + started.errorMessage() : string()));
        if (started.isError())
        {
            stepUnrestrictedFloor();
            return;
        }
        m_ridBg = started.value();

        // 启动这一步只回 run_id: 命令还在跑(收尾回调没来, 输出也没给回)
        check(!m_ridBg.empty(), "后台运行也有 run_id: " + m_ridBg);
        check(!m_bgCalledBack, "runBackground 启动即返回(没等命令结束)");

        JZVirtualTerminal::RunRecordInfo info;
        check(m_term.runSnapshot(m_ridBg, info), "能按 run_id 查到这条后台运行");
        check(info.background, "记录里标了它是后台运行");
        check(info.status == "running", "查到的状态是 running: " + info.status);
        check(info.pid > 0, "记录里带根 pid: " + to_string(info.pid));

        // 等一拍再查: 后台任务照样在收输出(日志查得到), 而且没有超时来杀它
        JZTimer::singleShot(1200, [this]() { stepRunBackgroundCheck(); });
    }

    void stepRunBackgroundCheck()
    {
        JZVirtualTerminal::RunRecordInfo info;
        check(m_term.runSnapshot(m_ridBg, info), "1.2 秒后仍能查到这条后台运行");
        check(info.status == "running", "后台任务不会被超时杀掉(仍在 running): " + info.status);
        bool hasStart = false;
        for (const JZVirtualTerminal::RunLogLine &l : info.log)
        {
            if (contains(l.text, "bg_start"))
                hasStart = true;
        }
        check(hasStart, "后台任务的输出照样进运行记录(terminal_query 能查到)");
        check(!m_bgCalledBack, "还在跑, 收尾回调没来");

        check(m_term.cancel(m_ridBg), "cancel(run_id) 能停掉后台任务");
        check(m_term.runSnapshot(m_ridBg, info) && info.status == "done",
              "停掉后状态变 done(结果留在记录里可查): " + info.status);

        stepRunTreeKill();
    }

    // ---- 进程树收尾: 停一条命令要连它的**子孙**一起收 ----
    //   执行面是 `bash -c "<命令>"`, 真正的服务/长任务都在 bash 底下(git-bash 实测还会再
    //   套一层 bash: bash -> bash -> sleep)。只杀直接子进程的话, 它们会变成孤儿进程继续
    //   占端口 —— 这就是"起了服务收不干净"的根因。这里用真实进程树断言收尾是否彻底。
    void stepRunTreeKill()
    {
        JZResult<string> started = m_term.runBackground("echo tree_probe && sleep 60",
                                                       [](bool, const string &, const string &) {});
        check(!started.isError(), "进程树: 后台起一条会把子进程挂住的命令" +
                                      (started.isError() ? ": " + started.errorMessage() : string()));
        if (started.isError())
        {
            stepUnrestrictedFloor();
            return;
        }
        m_treeRid = started.value();

        JZVirtualTerminal::RunRecordInfo info;
        check(m_term.runSnapshot(m_treeRid, info) && info.pid > 0, "进程树: 拿到根进程 pid");
        m_treeRoot = (unsigned long)info.pid;

        // 先等它把子孙起出来(否则这个测试什么也没验证), 再停
        JZTimer::singleShot(800, [this]()
                            {
                                m_treeDescBefore = descendantCount(m_treeRoot);
                                check(m_treeDescBefore > 0,
                                      "进程树: 跑起来之后确实有子孙进程(" +
                                          to_string(m_treeDescBefore) + " 个)");
                                check(m_term.cancel(m_treeRid), "进程树: cancel(run_id) 停掉这条");
                                JZTimer::singleShot(800, [this]() { stepRunTreeKillCheck(); });
                            });
    }

    void stepRunTreeKillCheck()
    {
        int left = descendantCount(m_treeRoot);
        check(left == 0, "进程树: 停掉之后子孙一个不剩(跑着时 " + to_string(m_treeDescBefore) +
                             " 个, 现在 " + to_string(left) + " 个)");
        stepUnrestrictedFloor();
    }

    // ⭐ Unrestricted(全部不检查)档: 白名单/区域/危险参数全跳过, 但"安全底线"仍在
    //   —— 特别危险的指令 + 删除工作区外的文件, 即便信任档也永远拒且不真执行。
    void stepUnrestrictedFloor()
    {
        JZVirtualTerminalConfig cfg = m_term.terminalConfig();
        cfg.level = AccessLevel::Unrestricted;
        m_term.setTerminalConfig(cfg);

        string error;
        auto deny = [&](const string &cmd, const string &what)
        {
            // 底线命中是同步拒(起进程之前), 回调不会被调到。
            // 单独用一份 error: 否则最后那条 check 的消息里会挂着上一次拒绝的原因, 看着像没过。
            string why;
            check(!runCmd(cmd, 20000, [](bool, const string &, const string &) {}, why),
                  "Unrestricted 仍拒绝 " + what);
        };

        // 1) 删除工作区外的文件(含根目录) —— 永远拒, 且不真执行
        string outside = jzApp->applicationDirPath() + "/vt_outside_rm.txt";
        JZFile::remove(outside);
        deny("rm -rf " + outside, "删除工作区外的文件");
        deny("rmdir " + outside, "删除工作区外的目录");
        deny("rm -rf /", "删除根目录");

        // 2) 特别危险的指令
        deny("shutdown", "关机");
        deny("mkfs.ext4 /dev/sda1", "格式化磁盘");
        deny("dd if=/dev/zero of=/dev/sda", "往块设备写");

        // 3) 放行面: 工作区内命令 + heredoc(Unrestricted 下仍要能解析 heredoc)
        string cmd = "echo unrestricted_floor_ok && cat > " + m_ws +
                     "/_h.txt <<'EOF'\nhello heredoc\nEOF";
        bool started = runCmd(cmd, 20000,
                                  [this](bool ok, const string &out, const string &err)
                                  {
                                      check(ok, "Unrestricted 放行工作区内命令/heredoc: " + err);
                                      check(contains(out, "unrestricted_floor_ok"),
                                            "普通命令照常执行: " + out);
                                      check(JZFileInfo(m_ws + "/_h.txt").isFile(),
                                            "heredoc 在 Unrestricted 下也写进去了");
                                      stepAudit();
                                  }, error);
        check(started, "run Unrestricted 放行命令: " + error);
        if (!started)
            stepAudit();
    }

    void stepAudit()
    {
        check(g_auditCount > 0, "审计回调被调用过");
        check(contains(g_lastAuditAction, ":deny") || contains(g_lastAuditAction, ":allow"),
              "审计里带动作与结论: " + g_lastAuditAction);

        JZDir(m_ws).removeRecursively();
        jzApp->quit();
    }

    JZVirtualTerminal m_term;
    string m_ws;

    // ---- 并发步骤用 ----
    int m_left = 0;                  // 还没回调的条数(到 0 才继续下一步)
    bool m_concDone = false;         // 各步的一次性闸: 同步回调 + 尾部兜底可能都触发下一步,
    bool m_cancelDone = false;       // 不加闸就会把后面的步骤跑两遍
    bool m_cancelAllDone = false;
    int m_killed = 0;                // cancelAll 收到收尾回调的条数
    string m_ridA, m_ridB;           // 并发两条的 run_id
    string m_ridLong, m_ridPeer;     // cancel 步骤的 run_id
    string m_ridBg;                  // 后台运行步骤的 run_id
    bool m_bgCalledBack = false;     // 后台那条有没有来过收尾回调
    string m_outA, m_outB;
    string m_cancelOut, m_peerOut;

    // ---- 进程树收尾步骤用 ----
    string m_treeRid;                // 那棵树对应的 run_id
    unsigned long m_treeRoot = 0;    // 根进程 pid
    int m_treeDescBefore = 0;        // 停掉之前数到的子孙进程个数
};

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    setvbuf(stdout, nullptr, _IONBF, 0);   // 崩了也能看到跑到哪
    printf("[step] main\n");

    JZApplication app;

    TerminalTest test;
    int ret = test.run();

    printf("\n%s (fail=%d)\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
    return g_fail == 0 ? ret : 1;
}
