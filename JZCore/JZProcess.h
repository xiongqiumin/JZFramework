#ifndef JZPROCESS_H
#define JZPROCESS_H
#include "JZObject.h"
#include "JZByteArray.h"
#include "JZThread.h"
#include <vector>
#include <string>
#include <map>
#include <cstdint>   // intptr_t: 跨平台的"句柄"(Windows HANDLE / POSIX fd)

class JZProcessPrivate;

class JZFRAMEWORK_API JZProcess : public JZObject
{
    JZ_OBJECT_DECLARE(JZProcess)
public:
    enum ProcessState {
        NotRunning,
        Starting,
        Running
    };

    enum ProcessChannelMode {
        SeparateChannels,
        MergedChannels,
        ForwardedChannels
    };

    // 读哪一路(与 QProcess 同名同义): read()/readLine()/bytesAvailable() 都跟着它走
    enum ProcessChannel {
        StandardOutput,
        StandardError
    };

    // 子进程的 stdin 从哪来:
    //   ManagedInputChannel(默认) = 我们建管道, 由 write() 喂;
    //   InheritInputChannel       = 直接继承**本进程的** stdin(`$SH < script` 时, 脚本里
    //                               起的命令要接着读脚本剩下的内容, 正是这个语义)。
    enum InputChannelMode {
        ManagedInputChannel,
        InheritInputChannel
    };

    // 子进程的 stdout/stderr 往哪去:
    //   ManagedOutputChannel(默认) = 我们建管道, 由 readAllStandardOutput()/Error() 收;
    //   InheritOutputChannel       = 直接继承**本进程的** fd 1 / fd 2, 不建管道(与
    //                                InheritInputChannel 对称)。子进程自己 write() 出去,
    //                                两路的先后由内核保证 —— 这正是 bash 的做法, 不用我们把
    //                                两个缓冲区拼回一个字符串(那样顺序必然错)。
    //   注意: 继承模式下 readAllStandardOutput()/Error() 恒为空。
    enum OutputChannelMode {
        ManagedOutputChannel,
        InheritOutputChannel
    };

    enum ExitStatus {
        NormalExit,
        CrashExit
    };

    enum ProcessError {
        FailedToStart,
        Crashed,
        Timedout,
        WriteError,
        ReadError,
        UnknownError
    };

    // 启动一个分离进程: 不创建管道、不跟踪状态、不回收, 父进程退出后子进程独立存活。
    // 成功返回 true (仅表示创建成功, 不等待子进程结束)。
    static bool startDetached(const std::string &program,
                              const std::vector<std::string> &arguments = {},
                              const std::string &workingDirectory = {});

    explicit JZProcess(JZObject *parent = nullptr);
    ~JZProcess();

    bool start(const std::string &program, const std::vector<std::string> &arguments = {});

    // ---- 分离运行: 先 start(), 再 detach() 放手 ----
    // start() 已经按 setStandardXxx* 建好了 IO(没设的才是管道), detach() 只做三件事:
    //   停 timer、关进程句柄、把作业的 kill-on-close 摘掉(否则我们退出会把刚放手的进程带走)。
    // 之后这个对象**不再跟踪**它: waitForFinished/readAll/exitCode 全部无意义, 只有
    // processId() 还有效。⚠ 有托管管道时不允许 detach(那不是放手, 是剪管子)。
    bool detach();
    bool canDetach() const;
    bool isDetached() const { return m_detached; }
    // 等待进程结束。等待期间**会持续读管道** —— 否则子进程写满管道缓冲区就卡死
    // (以前只 wait 不读, 输出稍大就死锁)。msecs < 0 = 无限等(QProcess 的约定)。
    bool waitForFinished(int msecs = 30000);
    // start() 是同步的, 所以这里只是把"没起来"报出来; 保留接口是为了和 QProcess 对齐
    bool waitForStarted(int msecs = 30000);
    // 等到"当前读通道"有数据可读(期间持续收数据), 超时返回 false
    bool waitForReadyRead(int msecs = 30000);

    JZByteArray readAllStandardOutput();
    JZByteArray readAllStandardError();
    // ---- 增量读(与 QProcess 对齐) ----
    // maxSize <= 0 表示"有多少给多少"
    JZByteArray read(int64_t maxSize = 0);
    // 读到且包含第一个 '\n'; maxSize <= 0 表示不限
    JZByteArray readLine(int64_t maxSize = 0);
    // 当前读通道里已经缓存、还没被取走的字节数
    int64_t bytesAvailable() const;
    bool canReadLine() const;
    void setReadChannel(ProcessChannel channel);
    ProcessChannel readChannel() const;

    int64_t write(const JZByteArray &data);

    void terminate();
    void killProcess();

    void setWorkingDirectory(const std::string &dir);
    std::string workingDirectory() const;

    // 子进程的环境变量(覆盖/追加到本进程环境之上)。空 = 直接继承本进程环境。
    // 用途: shell 的 `export FOO=bar` / `FOO=bar cmd` —— 变量只存在于解释器的表里,
    // 不落到这里的话子进程根本看不见(实测 `export FOO=bar; env | grep FOO` 是空的)。
    void setEnvironmentVariables(const std::map<std::string, std::string> &vars);
    const std::map<std::string, std::string> &environmentVariables() const;

    // 上一次 start() 失败的原因（已按失败类型归类：工作目录不存在 / 找不到程序 / 拒绝访问 / 错误码 …）。
    // start() 成功时清空。⚠ 以前只回 bool，调用方只能猜，于是把"工作目录不存在"误报成"程序不存在"
    // （实测：run_js 在还没编译、build/ 尚未生成时启动，CreateProcess 回 ERROR_DIRECTORY 267）。
    std::string lastStartError() const;

    // ---- 每一路的标准 IO 目标(与 QProcess 的 setStandardXxxFile 对齐) ----
    // 一路只有一个"槽": 管道 / 文件 / 外部句柄 / 丢弃 / 转发我们的。后设的覆盖先设的。
    // 只有 **Pipe**(默认) 才有管道: 设成其它目标后, 该路的 readAllXxx() 恒为空,
    // 数据由目标自己接(文件、控制台、宿主给的句柄)。
    struct ChannelTarget
    {
        enum Kind
        {
            Pipe = 0,   // 默认: 我们建管道并收集(可 readAll / waitForReadyRead)
            File,       // 写/读到文件(不存在则建; Truncate 清空, Append 追加)
            Handle,     // 直接用调用方给的句柄(Windows: HANDLE, POSIX: fd)
            Null,       // 丢弃(Windows: NUL, POSIX: /dev/null)
            Forward,    // 转发给我们自己的 fd 0/1/2(等于 Qt 的 ForwardedChannel)
        };
        Kind kind = Pipe;
        std::string file;
        bool append = false;
        std::intptr_t handle = 0;
    };
    enum FileOpenMode
    {
        Truncate = 0,   // `> file`
        Append = 1,     // `>> file`
    };

    void setStandardInputFile(const std::string &fileName);
    void setStandardOutputFile(const std::string &fileName, FileOpenMode mode = Truncate);
    void setStandardErrorFile(const std::string &fileName, FileOpenMode mode = Truncate);
    // ⚠ 句柄是**借用**: JZProcess 不会关闭它(谁开的谁关)。要传给子进程必须可继承:
    //   Windows 上我们启动时会替你补 HANDLE_FLAG_INHERIT; POSIX 上是 fd。
    void setStandardInputHandle(std::intptr_t handle);
    void setStandardOutputHandle(std::intptr_t handle);
    void setStandardErrorHandle(std::intptr_t handle);
    void setStandardInputNull();
    void setStandardOutputNull();
    void setStandardErrorNull();

    const ChannelTarget &standardOutputTarget() const { return m_stdoutChan; }
    const ChannelTarget &standardErrorTarget() const { return m_stderrChan; }
    const ChannelTarget &standardInputTarget() const { return m_stdinChan; }

    void setProcessChannelMode(ProcessChannelMode mode);
    ProcessChannelMode processChannelMode() const;

    void setInputChannelMode(InputChannelMode mode);
    InputChannelMode inputChannelMode() const;

    void setOutputChannelMode(OutputChannelMode mode);
    OutputChannelMode outputChannelMode() const;

    // 把本进程的标准输出直接接到**另一个正在运行的进程**的标准输入上(与 QProcess 同名同义)。
    // 两个进程各自独立运行、真正并发(A 写管道, B 从管道读), 数据不经过我们内存。
    // 用法(顺序很重要, 与 QProcess 一样: 先起下游, 再起上游):
    //     JZProcess consumer, producer;
    //     consumer.start("wc", {"-c"});
    //     producer.setStandardOutputProcess(&consumer);
    //     producer.start("cat", {"big.txt"});
    // 接了之后: 本进程不再有 stdout 管道(readAllStandardOutput 恒为空), 下游的 stdin
    // 也不能再 write() —— 上游退出时下游会收到 EOF。
    void setStandardOutputProcess(JZProcess *destination);
    JZProcess *standardOutputProcess() const;

    ProcessState state() const;
    int exitCode() const;
    ExitStatus exitStatus() const;

    // 真实操作系统进程号(Linux = fork 出来的 pid; Windows = CreateProcess 的 dwProcessId)。
    // 未启动 / 已回收返回 -1。JZVirtualTerminal 用它把"运行记录"和真实进程对应起来。
    int processId() const;

    std::string program() const;
    std::vector<std::string> arguments() const;

    void closeWriteChannel();

jz_signals:
    JZSignal<> sigStarted;
    JZSignal<int, JZProcess::ExitStatus> sigFinished;
    JZSignal<ProcessError> sigErrorOccurred;
    JZSignal<> sigReadyReadStandardOutput;
    JZSignal<> sigReadyReadStandardError;

private:
    friend JZProcessPrivate;

    virtual void timerEvent(JZTimerEvent *event) override;
    void finished(int, JZProcess::ExitStatus);

    JZProcessPrivate *d_ptr;
    int m_timerId;
    std::string m_startError;   // 见 lastStartError()
    ProcessChannel m_readChannel = StandardOutput;   // read()/readLine()/bytesAvailable() 看哪一路
    JZProcess *m_stdoutTarget = nullptr;             // 见 setStandardOutputProcess()
    InputChannelMode m_inputMode = ManagedInputChannel;   // 见 setInputChannelMode()
    std::map<std::string, std::string> m_envVars;         // 见 setEnvironmentVariables()
    OutputChannelMode m_outputMode = ManagedOutputChannel;   // 见 setOutputChannelMode()
    std::string m_program;                                  // start() 那次传进来的(program() 用)
    std::vector<std::string> m_arguments;                   // 同上
    ChannelTarget m_stdinChan;                              // 见 setStandardInputFile/Handle/Null
    ChannelTarget m_stdoutChan;                             // 见 setStandardOutputFile/Handle/Null
    ChannelTarget m_stderrChan;                             // 见 setStandardErrorFile/Handle/Null
    bool m_detached = false;                                // 见 detach()
};

#endif
