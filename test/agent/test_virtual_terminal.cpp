// JZVirtualTerminal 冒烟测试(不依赖 JZProcess 的那部分)
//
// 覆盖: 单终端 / 内建文件动作 / **区域审核**(越界必须被拒) / 审计回调 / 程序白名单。
// 期望输出: ALL PASS
//
// 注: JZVirtualTerminal 现在就是"一个终端"(无多会话), cwd/env/在跑进程都是它自己的状态。

#include <stdio.h>
#include <string>

#include "JZApplication.h"
#include "JZDir.h"
#include "JZFile.h"
#include "JZFileInfo.h"
#include "JZTimer.h"
#include "JZVirtualTerminal.h"

using namespace std;

static int g_fail = 0;

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
        bool started = m_term.run("echo jz_terminal_ok", 20000,
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
        check(!m_term.run("some_unknown_prog --do-it", 20000,
                          [](bool, const string &, const string &) {}, error),
              "run 拒绝白名单外的程序");

        // 重定向目标在工作区外: 也要拒(zone 模型的写边界就靠这条)
        string outside = jzApp->applicationDirPath() + "/vt_outside.txt";
        JZFile::remove(outside);
        check(!m_term.run("echo x > " + outside, 20000,
                          [](bool, const string &, const string &) {}, error),
              "run 拒绝越界的重定向: " + error);
        check(!JZFileInfo(outside).exists(), "越界文件确实没被创建");

        // 单独一条设/清变量: 不跨命令保留, 必须**明确报错**(否则模型会以为设过了)
        check(!m_term.run("export JZ_X=1", 20000,
                          [](bool, const string &, const string &) {}, error),
              "run 拒绝单独一条 export");
        check(!m_term.run("JZ_X=1", 20000,
                          [](bool, const string &, const string &) {}, error),
              "run 拒绝单独一条赋值");
        check(!m_term.run("unset PATH", 20000,
                          [](bool, const string &, const string &) {}, error),
              "run 拒绝单独一条 unset");

        stepRunShell();
    }

    // 真正要验证的: 用 shell 的 coreutils 干活 —— 文件工具删掉之后模型靠的就是这些。
    // 顺带验证 `&&` 串联(整条交给 shell, 我们不解析)。
    void stepRunShell()
    {
        string error;
        bool started = m_term.run("ls -1 && mkdir -p deep/dir && ls -1", 20000,
                                  [this](bool ok, const string &out, const string &err)
                                  {
                                      check(ok, "run ls/mkdir 链式: " +
                                                (err.empty() ? string() : err));
                                      check(contains(out, "g.txt") && contains(out, "deep"),
                                            "shell 的 ls/mkdir 真的生效: " + out);
                                      check(JZFileInfo(m_ws + "/sub/deep/dir").isDir(),
                                            "mkdir -p 的目录真的建出来了");
                                      stepRunInlineEnv();
                                  }, error);
        check(started, "run shell coreutils: " + error);
        if (!started)
            stepRunInlineEnv();
    }

    // 内联写变量是**合法**的(在那一条命令内有效) —— 这就是"不做会话级 env"的替代写法
    void stepRunInlineEnv()
    {
        string error;
        bool started = m_term.run("export JZ_X=42 && echo val=$JZ_X", 20000,
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
        bool started = m_term.run("cd deep && pwd", 20000,
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
        bool started = m_term.run("cd deep", 20000,
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
        bool started = m_term.run("pwd", 20000,
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
            check(!m_term.run(c.cmd, 20000,
                              [](bool, const string &, const string &) {}, error),
                  string("拒绝 ") + c.what);
        }

        // 反过来: `&&` 不能被误当成后台 `&`。
        // 注意 check 的消息是**提前**拼好的, 所以只在失败时才带 error(否则会带上一次拒绝的旧文本)
        bool andOk = m_term.run("echo a && echo b", 20000,
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
        bool started = m_term.run("echo 'a $(whoami) b'", 20000,
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
        bool started = m_term.run("printf 'a\\nb\\nc\\n' | head -n 1", 20000,
                                  [this](bool ok, const string &out, const string &err)
                                  {
                                      check(ok, "run 管道 | 命令正常: " + err);
                                      check(contains(out, "a"),
                                            "管道两端都跑通(grep...|head 同形态): " + out);
                                      stepUnrestrictedFloor();
                                  }, error);
        check(started, "run 管道命令启动: " + error);
        if (!started)
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
            // 底线命中是同步拒(起进程之前), 回调不会被调到
            check(!m_term.run(cmd, 20000, [](bool, const string &, const string &) {}, error),
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
        bool started = m_term.run(cmd, 20000,
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
