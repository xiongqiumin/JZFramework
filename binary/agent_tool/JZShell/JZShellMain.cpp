// ---------------------------------------------------------------------------
// jz_shell —— JZShell(自研 shell)的独立演示/自检程序
//
// 用法:
//   jz_shell -c "pwd" -c "cd .." -c "pwd"        批量跑(同一个会话, 状态保留)
//   jz_shell --script build.sh                    跑一个脚本文件
//   jz_shell                                      交互: 逐行输入, exit/quit 退出
//   jz_shell --cwd <目录> --path <额外搜索目录>
//
// 不依赖 bash / PTY: 语法由 JZShell 自己解析执行, 外部命令(ls/grep/...)在 Windows
// 上默认从 Git 的 usr/bin、bin 里找。
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <fcntl.h>
#  include <io.h>
#else
#  include <unistd.h>
#endif

#include "JZApplication.h"
#include "JZDir.h"
#include "JZFileInfo.h"
#include "JZShell.h"

using namespace std;

namespace
{
struct Options
{
    vector<string> commands;
    vector<string> pathDirs;
    string script;
    vector<string> positional;   // `-c 命令 $0 $1 ...` 或脚本后面的参数
    string cwd;
    int timeoutMs = 15 * 60 * 1000;
    bool quiet = false;       // 只输出命令自己的 stdout/stderr, 不打横幅
    bool check = false;       // 只做语法检查(--check): 给"定位解析失败"用, 不执行
    bool posix = false;       // `-o posix`
    bool extdebug = false;    // `-O extdebug`
};

void usage()
{
    printf("用法: jz_shell [-c \"命令\"]... [--script <文件>] [--cwd <目录>]\n"
           "              [--path <额外程序目录>]... [--timeout <毫秒>] [-q|--quiet]\n"
           "              [--check](只做语法检查, 不执行)\n"
           "  不带 -c/--script 时进入交互模式(逐行执行, exit/quit 退出)\n"
           "  -q: 不打印 `$ cmd` / `[exit N] [cwd ...]` 横幅, 便于和 bash 输出比对\n");
}

// Windows 上外部命令(ls/grep/sed...)来自 Git for Windows; 不需要 bash 本身
void addGitToolDirs(JZShell &shell)
{
#ifdef _WIN32
    vector<string> roots;
    const char *env = getenv("JZ_GIT_ROOT");
    if (env && *env)
        roots.push_back(env);
    roots.push_back("C:/Program Files/Git");
    roots.push_back("C:/Program Files (x86)/Git");
    for (const string &r : roots)
    {
        string usrBin = r + "/usr/bin";
        if (JZFileInfo(usrBin).isDir())
        {
            shell.addPathDir(usrBin);
            shell.addPathDir(r + "/bin");
            return;
        }
    }
#else
    (void)shell;
#endif
}

// 从进程 stdin 读一行(不带换行)。**逐字节读、不预读** —— 用 iostream 的话它会把
// 后面几 KB 一起吞进缓冲区, 脚本里起的子命令就再也读不到那些数据了。
bool readStdLine(std::string &out)
{
    out.clear();
    bool any = false;
    while (true)
    {
        char c = 0;
#ifdef _WIN32
        int n = _read(0, &c, 1);
#else
        int n = (int)::read(0, &c, 1);
#endif
        if (n <= 0)
            return any;          // EOF / 读失败
        any = true;
        if (c == '\n')
            return true;
        if (c != '\r')
            out += c;
    }
}

void printResult(const string &cmd, const JZShell::Result &r, bool quiet)
{
    if (quiet)
    {
        if (!r.output.empty())
            fwrite(r.output.data(), 1, r.output.size(), stdout);
        if (!r.error.empty())
            fprintf(stderr, "%s\n", r.error.c_str());
        return;
    }
    printf("$ %s\n", cmd.c_str());
    if (!r.output.empty())
    {
        fwrite(r.output.data(), 1, r.output.size(), stdout);
        if (r.output.back() != '\n')
            printf("\n");
    }
    if (!r.error.empty())
        printf("[error] %s\n", r.error.c_str());
    printf("[exit %d] [cwd %s]\n\n", r.exitCode, r.cwd.c_str());
}

bool runOne(JZShell &shell, const string &cmd, int *failCount, bool quiet)
{
    JZShell::Result r = shell.exec(cmd);
    printResult(cmd, r, quiet);
    if (!r.ok && failCount)
        (*failCount)++;
    return r.ok;
}
} // namespace

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef _WIN32
    // 文本模式会把 \n 写成 \r\n, 与 bash 的输出逐字节比对时全错
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    // 输出被重定向/进管道时默认安静: `$ cmd` / `[exit N]` 横幅是给人看的,
    // bash 测试套件会把两者逐字节比对(它就是直接调 $THIS_SH 的), 有横幅必然对不上。
#ifdef _WIN32
    const bool piped = (_isatty(_fileno(stdout)) == 0);
#else
    const bool piped = (isatty(fileno(stdout)) == 0);
#endif

    Options opt;
    for (int i = 1; i < argc; i++)
    {
        string a = argv[i];
        auto next = [&](string &out) -> bool
        {
            if (i + 1 >= argc)
                return false;
            out = argv[++i];
            return true;
        };
        if (a == "-c" || a == "--command")
        {
            string v;
            if (!next(v))
            {
                usage();
                return 2;
            }
            opt.commands.push_back(v);
        }
        else if (a == "--script")
        {
            if (!next(opt.script))
            {
                usage();
                return 2;
            }
        }
        else if (a == "--cwd")
        {
            if (!next(opt.cwd))
            {
                usage();
                return 2;
            }
        }
        else if (a == "--path")
        {
            string v;
            if (!next(v))
            {
                usage();
                return 2;
            }
            opt.pathDirs.push_back(v);
        }
        else if (a == "--timeout")
        {
            string v;
            if (!next(v))
            {
                usage();
                return 2;
            }
            opt.timeoutMs = atoi(v.c_str());
        }
        else if (a == "-q" || a == "--quiet")
        {
            opt.quiet = true;
        }
        else if (a == "--check")
        {
            opt.check = true;
        }
        else if (a == "-o" || a == "-O" || a == "--option")
        {
            // bash 的 `-o posix` / `-O extglob` 写法(测试套件里会这么调)
            string v;
            if (!next(v))
            {
                usage();
                return 2;
            }
            if (v == "posix")
                opt.posix = true;
            else if (v == "extdebug")
                opt.extdebug = true;
        }
        else if (a == "-h" || a == "--help")
        {
            usage();
            return 0;
        }
        else if (a.size() > 2 && a[0] == '-' && a[1] != '-')
        {
            // 合并的短选项: bash 允许 `-ce` / `-ec` / `-xec`(测试套件里就是这么调的)。
            // 只看有没有 'c'(后面跟命令串), 其它开关(-e/-x/-u/-v/-n)接受不实现。
            bool needCmd = false;
            for (size_t k = 1; k < a.size(); k++)
            {
                if (a[k] == 'c')
                    needCmd = true;
            }
            if (needCmd)
            {
                string v;
                if (!next(v))
                {
                    usage();
                    return 2;
                }
                opt.commands.push_back(v);
            }
        }
        else if (a[0] != '-')
        {
            // `jz_shell ./x.sh` —— 与 bash 一致: 第一个非选项参数就是脚本文件。
            // 这样测试套件里的 `$THIS_SH ./xxx.tests` 可以直接指向本程序(不必再套一层包装脚本)。
            // `-c 命令 名字 参数...`: 命令之后那些是 $0 和位置参数(bash 的写法)。
            if (!opt.commands.empty())
                opt.positional.push_back(a);
            else if (opt.script.empty())
                opt.script = a;
            else
                opt.positional.push_back(a);
        }
        else
        {
            printf("[error] 不认识的参数: %s\n", a.c_str());
            usage();
            return 2;
        }
    }

    if (piped)
        opt.quiet = true;   // 被重定向/进管道: 不打横幅(见上面 piped 的说明)

    JZApplication app;   // JZProcess 用到 JZObject 的 timer 基础设施
    JZShell shell;
    shell.setCommandTimeoutMs(opt.timeoutMs);
    addGitToolDirs(shell);
    for (const string &d : opt.pathDirs)
        shell.addPathDir(d);

    // 错误信息的前缀形态与 bash 对齐(见 JZShell::setErrorMode*):
    //   `-c` 模式: `$0: -c: line N: ...`;  脚本模式: `<脚本>: line N: ...`
    if (!opt.commands.empty())
    {
        shell.setErrorModeCommand();
        if (!opt.positional.empty())
            shell.setArgs(opt.positional);
    }
    else if (!opt.script.empty())
    {
        shell.setErrorModeScript(opt.script);
        // `$0` = 脚本路径(bash 的行为); 后面还能跟脚本自己的参数
        vector<string> args;
        args.push_back(opt.script);
        for (const string &p : opt.positional)
            args.push_back(p);
        shell.setArgs(args);
    }
    if (opt.posix)
        shell.setPosix(true);
    if (opt.extdebug)
        shell.setExtdebug(true);

    // 命令行程序直接把输出写到自己的 fd 1/2(bash 的做法): 子进程也继承这两个 fd,
    // 这样 stdout/stderr 的交错顺序由内核保证, 而不是"整块拼起来"。
    // 需要拿回字符串的宿主(虚拟终端/agent 工具)用 JZShell 时不要开这个。
    shell.setDirectOutput(true);

    string err;
    if (!opt.cwd.empty() && !shell.setCwd(opt.cwd, &err))
    {
        printf("[error] 工作目录无效: %s\n", err.c_str());
        return 2;
    }

    int fail = 0;

    // ---- 脚本 ----
    if (!opt.script.empty())
    {
        ifstream f(opt.script, ios::binary);
        if (!f)
        {
            printf("[error] 打不开脚本: %s\n", opt.script.c_str());
            return 2;
        }
        string text((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
        if (opt.check)   // --check: 只解析, 不执行(定位解析失败时用, 不会真的跑命令)
        {
            string err;
            if (!shell.parseOnly(text, &err))
            {
                fprintf(stderr, "%s\n", err.c_str());
                return 1;
            }
            return 0;
        }
        JZShell::Result r = shell.exec(text);
        printResult(opt.script, r, opt.quiet);
        // 退出码要**原样**返回: 脚本里 `exit 14` ⇒ 进程状态 14(bash 的行为)。
        // 以前一律压成 1, `exit 142 | exit 14` 这类用例的返回值就对不上。
        return r.ok ? 0 : (r.exitCode > 0 ? (r.exitCode & 0xff) : 1);
    }

    // ---- 批量 ----
    if (!opt.commands.empty())
    {
        for (const string &c : opt.commands)
            runOne(shell, c, &fail, opt.quiet);
        return fail == 0 ? 0 : 1;
    }

    // ---- 从标准输入读一整段当脚本跑(bash 的行为: 没有参数时读 stdin) ----
    // `$THIS_SH <<EOF` / `echo 'cmd' | $THIS_SH` 都靠这条; 只在非终端时走,
    // 否则交互模式下 `cin >> line` 会一直等到 EOF(管道给的 stdin 不一定关)。
    if (piped)
    {
        // ⚠ 必须**逐行**读(而且不能预读): 脚本里起的命令要能接着读剩下的 stdin。
        //   一次性 slurp 的话, `read x` 这样的子命令拿到的是 EOF(input-test 就是这个)。
        //   行累积成一条完整语句再执行 —— 判据是"解析只差 EOF"。
        shell.setInheritStdin(true);
        string buf;
        string line;
        while (readStdLine(line))
        {
            buf += line;
            buf += '\n';

            string perr;
            bool incomplete = false;
            if (!shell.parseOnly(buf, &perr))
            {
                if (perr.find("unexpected end of file") != string::npos ||
                    perr.find("unexpected EOF while looking for matching") != string::npos)
                    incomplete = true;   // 结构还没写完(if/while/引号/heredoc), 继续攒
            }
            if (incomplete)
                continue;

            JZShell::Result r = shell.exec(buf);
            printResult("<stdin>", r, opt.quiet);
            if (!r.ok)
                fail++;
            buf.clear();
        }
        if (!buf.empty())
        {
            JZShell::Result r = shell.exec(buf);
            printResult("<stdin>", r, opt.quiet);
            if (!r.ok)
                fail++;
        }
        return fail == 0 ? 0 : 1;
    }

    // ---- 交互 ----
    printf("[jz_shell] cwd=%s  (自研 shell, 不依赖 bash/PTY)\n", shell.cwd().c_str());
    printf("输入命令回车执行, exit/quit 退出\n\n");
    string line;
    while (true)
    {
        printf("> ");
        fflush(stdout);
        if (!getline(cin, line))
            break;
        if (line == "exit" || line == "quit")
            break;
        if (line.empty())
            continue;
        runOne(shell, line, &fail, opt.quiet);
    }
    return fail == 0 ? 0 : 1;
}
