#include "JZBenchmark.h"
#include "JZTest.h"
#include "JZStd.h"
#include "JZApplication.h"
#include "JZFile.h"
#include "JZDir.h"
#include "JZTimer.h"
#include "JZFixedTimer.h"
#include "JZThread.h"
#include "JZEventLoop.h"
#include "JZCrypto.h"
#include <atomic>
#include <vector>

// 128k 测试数据
static const int kIoTestSize = 128 * 1024;
static const char *kIoTestFile = "benchmark_io.tmp";
static const char *kIoTestDir = "benchmark_dir.tmp";

static JZByteArray makeIoTestData(int size)
{
    JZByteArray data;
    data.resize(size);
    for (int i = 0; i < size; i++)
        data[i] = (char)(i & 0xFF);
    return data;
}

// JZThread::msleep 基准
static void benchmark_thread_sleep()
{
    printf("\n========== JZThread::msleep Benchmark ==========\n");
    JZBenchmark bm(/*targetMs=*/100);
    bm.runAndPrint("JZThread::msleep(1ms) x100", []() {
        for (int i = 0; i < 100; i++)
            JZThread::msleep(1);
    });
    printf("-----------------------------------------------------\n\n");
}

// JZTimer 基准: 衡量 singleShot(0) 的实现开销(分配 / 派发)
static void benchmark_timer()
{
    printf("\n========== JZTimer Benchmark ==========\n");
    JZBenchmark bm(/*targetMs=*/100);

    // 批量 post + 纯事件排空: 衡量"一次 singleShot(0)"的实现开销(分配/派发)。
    // 0ms 走事件队列后这里应明显变快(不再建 JZSingleTimer、不进线程定时器表、
    // 也不需要 deleteLater 的第二次投递)。
    // ⚠ 别在这里塞 msleep 去"等它触发": msleep(1) 自身有 ~10ms 粒度(受时钟节拍限制),
    //   那样量到的是睡眠粒度, 不是 singleShot 的开销 —— 旧版本那样写过, 已去掉。
    bm.runAndPrint("JZTimer::singleShot(0) x10000 + drain", []() {
        std::atomic<int> fired{0};
        for (int i = 0; i < 10000; i++)
            JZTimer::singleShot(0, [&]() { fired++; });
        int guard = 0;
        while (fired.load() < 10000 && guard++ < 100000)
            jzApp->processEvents();
        JZ_TEST_TRUE(fired.load() == 10000);
    });

    // 带接收者版本(语义: 接收者已销毁则不跑)同样走事件队列
    {
        JZObject receiver;
        bm.runAndPrint("JZTimer::singleShot(0, obj) x10000 + drain", [&receiver]() {
            std::atomic<int> fired{0};
            for (int i = 0; i < 10000; i++)
                JZTimer::singleShot(0, &receiver, [&]() { fired++; });
            int guard = 0;
            while (fired.load() < 10000 && guard++ < 100000)
                jzApp->processEvents();
            JZ_TEST_TRUE(fired.load() == 10000);
        });
    }
    printf("-----------------------------------------------------\n\n");
}

// 文件打开基准
static void benchmark_file_open()
{
    printf("\n========== JZFile open/close Benchmark ==========\n");
    // 先创建一个文件供反复打开
    {
        JZFile f;
        JZ_TEST_TRUE(f.open(kIoTestFile, "wb"));
        f.close();
    }
    JZBenchmark bm(/*targetMs=*/100);
    bm.runAndPrint("JZFile open+close x1000", []() {
        for (int i = 0; i < 1000; i++)
        {
            JZFile file;
            JZ_TEST_TRUE(file.open(kIoTestFile, "r+b"));
            file.close();
        }
    });
    printf("-----------------------------------------------------\n\n");
}

// 文件写基准 (128k)
static void benchmark_file_write()
{
    printf("\n========== JZFile write Benchmark ==========\n");
    JZByteArray data = makeIoTestData(kIoTestSize);
    JZBenchmark bm(/*targetMs=*/100);
    bm.runAndPrint("JZFile write 128k", [&]() {
        JZFile file;
        JZ_TEST_TRUE(file.open(kIoTestFile, "wb"));
        file.write(data);
        file.close();
    });
    printf("-----------------------------------------------------\n\n");
}

// 文件读基准 (128k)
static void benchmark_file_read()
{
    printf("\n========== JZFile read Benchmark ==========\n");
    JZByteArray data = makeIoTestData(kIoTestSize);
    {
        JZFile file;
        JZ_TEST_TRUE(file.open(kIoTestFile, "wb"));
        file.write(data);
        file.close();
    }

    JZBenchmark bm(/*targetMs=*/100);
    bm.runAndPrint("JZFile read 128k", []() {
        JZFile file;
        JZ_TEST_TRUE(file.open(kIoTestFile, "rb"));
        JZByteArray out = file.readAll();
        file.close();
        JZ_TEST_EQ(out.size(), kIoTestSize);
    });
    printf("-----------------------------------------------------\n\n");
}

// 文件打开/读写/seek 基准 (128k)
static void benchmark_file_seek()
{
    printf("\n========== JZFile seek/read/write Benchmark ==========\n");
    JZByteArray data = makeIoTestData(kIoTestSize);
    {
        JZFile file;
        JZ_TEST_TRUE(file.open(kIoTestFile, "wb"));
        file.write(data);
        file.close();
    }

    JZBenchmark bm(/*targetMs=*/100);
    bm.runAndPrint("JZFile seek+read+write 128k", []() {
        JZFile file;
        JZ_TEST_TRUE(file.open(kIoTestFile, "r+b"));
        JZByteArray block;
        block.resize(4096, 0);
        int pos = 0;
        while (pos < kIoTestSize)
        {
            file.seek(pos);
            file.write(block);
            file.seek(pos);
            JZByteArray rd = file.read(4096);
            pos += (int)rd.size();
        }
        file.close();
    });
    printf("-----------------------------------------------------\n\n");
}

// JZDir 基准
static void benchmark_dir()
{
    printf("\n========== JZDir Benchmark ==========\n");
    JZBenchmark bm(/*targetMs=*/100);
    bm.runAndPrint("JZDir mkdir+write20+list+remove", []() {
        JZDir dir(kIoTestDir);
        if (dir.exists())
            dir.removeRecursively();
        // ⚠ 原写法 dir.mkdir(kIoTestDir) 会拼成"<dir 自身>/<该名字>"的嵌套路径
        //   (JZDir::absoluteFilePath: 相对路径相对的是 dir 自己, 不是 CWD), 父级不存在
        //   ⇒ CreateDirectory 失败 ⇒ 断言抛出 ⇒ main 无 try/catch ⇒ 整进程 abort。
        //   这里改为建出 dir 自身(空路径 = absolutePath() = <cwd>/benchmark_dir.tmp)。
        JZ_TEST_TRUE(dir.mkpath(""));

        for (int i = 0; i < 20; i++)
        {
            JZFile file;
            std::string path = str_asprintf("%s/f_%d.tmp", kIoTestDir, i);
            JZ_TEST_TRUE(file.open(path.c_str(), "wb"));
            JZByteArray d;
            d.resize(256, 'x');
            file.write(d);
            file.close();
        }

        JZ_TEST_TRUE(dir.exists());
        std::vector<std::string> entries = dir.entryList(JZDir::Files | JZDir::Dirs | JZDir::NoDotAndDotDot);
        JZ_TEST_GE((int)entries.size(), 20);
        JZ_TEST_TRUE(dir.removeRecursively());
    });
    printf("-----------------------------------------------------\n\n");
}

// md5 基准 (128k)
static void benchmark_md5()
{
    printf("\n========== JZCrypto::hash(MD5) Benchmark ==========\n");
    JZByteArray data = makeIoTestData(kIoTestSize);
    JZBenchmark bm(/*targetMs=*/100);
    bm.runAndPrint("JZCrypto::hash(MD5) 128k", [&]() {
        JZByteArray hash = JZCrypto::hash(JZCrypto::HashAlgorithm::MD5, data);
        JZ_TEST_EQ(hash.size(), 16);
    });
    printf("-----------------------------------------------------\n\n");
}

// JZFixedTimer 基准
//
// 它和 JZTimer 的关键差别是**自带一条计时线程**(Windows 上走高精度可等待定时器), 于是
// 有两件值得量的事:
//   · 创建成本 —— 每个实例一条线程(建线程 + join) + 定时器 + 事件, 决定能不能随手 new;
//   · 实际拍间隔 —— 决定 60Hz 这种固定步长能不能真的跑准(与 JZTimer 的唤醒粒度对比)。
static void benchmark_fixed_timer()
{
    printf("\n========== JZFixedTimer Benchmark ==========\n");
    JZBenchmark bm(/*targetMs=*/100);

    // 构造 + 析构: 本类最贵的操作(建线程 + join)。
    bm.runAndPrint("JZFixedTimer ctor+dtor x1", []() {
        JZFixedTimer t;
    });

    // 启动 + 停止: 只改计时线程的等待周期(SetWaitableTimer + SetEvent), 不碰消息循环。
    bm.runAndPrint("JZFixedTimer ctor+start+stop+dtor x1", []() {
        JZFixedTimer t;
        t.start(16);
        t.stop();
    });

    // 实际拍间隔: 起一个 16ms 的 JZFixedTimer, 泵事件循环直到跑够 N 拍, 量真实每拍耗时。
    // ⚠ 这里刻意**忙轮询**(不 msleep): msleep(1) 自身就有 ~10ms 粒度, 会把测量完全盖住;
    //   忙轮询量的是"计时线程到点 -> 投递 -> 主线程派发"整条链的实际延迟。
    {
        const int kTicks = 60;
        const int kIntervalMs = 16;
        JZFixedTimer t;
        int ticks = 0;
        connect(&t.sigTimeout, [&ticks]() { ticks++; });

        JZElapsedTimer el;
        el.start();
        t.start(kIntervalMs);
        while (ticks < kTicks)
            jzApp->processEvents();       // 忙轮询: 只为把"唤醒 + 投递"的延迟量出来
        int64_t ns = el.nsecsElapsed();
        t.stop();

        printf("  [bench] JZFixedTimer 实际拍间隔 (%d 拍, 目标 %dms; 忙轮询)\n", kTicks, kIntervalMs);
        printf("    total : %s\n", JZBenchmark::formatNs(ns).c_str());
        printf("    avg   : %s\n", JZBenchmark::formatNs(ns / kTicks).c_str());
        printf("    frameCount : %lld (含补帧)\n", (long long)t.frameCount());
    }

    // 唤醒精度: 用 JZEventLoop 跑**真实事件循环**(不是忙轮询 —— 走的是 JZWin32Thread::pumpOnce
    // 的"处理 -> 算 gap -> 阻塞等消息"), 逐档各跑 kRunMs, 看该步长实际触发多少次、每次相对
    // "理想时刻"偏移多少。
    // ⚠ 逐档**串行**跑, 不是同时跑: 同时跑会让它们挤在主线程一个派发队列里互相排队, 后处理到的
    //   tick 里 now 已经偏晚, 于是 tick() 判定"落后"而补帧 —— 量到的就变成"互相排队"而不是单档精度。
    // ⚠ maxCatchUp 特意放大到 1000: 只关心触发情况, 不想让它因落后过多而中途 stop 打断统计。
    // ⚠ 理想时刻按本档的启动时刻 t0 起算(不是统一 0)。
    {
        const int kIntervals[] = { 1, 3, 7, 13, 17 };
        const int kCount = (int)(sizeof(kIntervals) / sizeof(kIntervals[0]));
        const int kRunMs = 5000;
        const int64_t kWindowNs = (int64_t)kRunMs * 1000000LL;

        printf("  [bench] 唤醒精度 (JZEventLoop 每档跑 %d ms; 偏移 = 实际触发时刻 - 理想时刻, us)\n",
               kRunMs);
        printf("    步长ms   期望次数   实际次数   平均偏移   最小偏移   最大偏移   极差     补帧次数\n");

        for (int idx = 0; idx < kCount; idx++)
        {
            const int interval = kIntervals[idx];
            JZFixedTimer timer;
            std::vector<int64_t> shots;              // 每次触发的时刻(ns, 相对 el.start())
            shots.reserve(kRunMs / interval + 64);

            JZElapsedTimer el;
            connect(&timer.sigTimeout, [&shots, &el]() { shots.push_back(el.nsecsElapsed()); });

            JZEventLoop loop;
            // quit() 之后 pumpOnce 还阻塞在 GetMessageW 里, 再投一个空事件把它叫醒, 免得白等一圈 gap。
            JZTimer::singleShot(kRunMs, [&loop]() {
                loop.quit();
                runOnObject(&loop, []() {});
            });

            el.start();
            int64_t t0 = el.nsecsElapsed();
            timer.start(interval, 1000);
            loop.exec();
            timer.stop();

            size_t n = 0;                            // 只统计窗口内的样本
            while (n < shots.size() && shots[n] <= kWindowNs)
                n++;

            double sum = 0;
            int64_t mn = 0, mx = 0;
            int catchUps = 0;                        // 相邻两次调用挨得太近 = 同一次 tick 里的补帧
            for (size_t j = 0; j < n; j++)
            {
                int64_t ideal = t0 + (int64_t)(j + 1) * interval * 1000000LL;
                int64_t offUs = (shots[j] - ideal) / 1000;   // ns -> us
                sum += (double)offUs;
                if (j == 0 || offUs < mn) mn = offUs;
                if (j == 0 || offUs > mx) mx = offUs;
                if (j > 0 && shots[j] - shots[j - 1] < (int64_t)interval * 1000000LL / 2)
                    catchUps++;
            }
            printf("    %-8d %-10d %-10d %-10.1f %-10lld %-10lld %-8lld %d\n",
                   interval, kRunMs / interval, (int)n,
                   n ? sum / (double)n : 0.0,
                   (long long)mn, (long long)mx, (long long)(mx - mn), catchUps);

            // 还原"每次 tick 实际发射了几帧": 时间戳几乎相同(间隔远小于一个步长)的连续几帧
            // 就是同一次 tick 补出来的。这一行直接回答"会不会一次调用发两帧"。
            int hist[8] = { 0 };
            int run = 0, ticks = 0;
            const int64_t halfNs = (int64_t)interval * 1000000LL / 2;
            for (size_t j = 0; j < n; j++)
            {
                if (j == 0 || shots[j] - shots[j - 1] > halfNs)
                {
                    if (run > 0) hist[run < 7 ? run : 7]++;
                    run = 1;
                    ticks++;
                }
                else
                    run++;
            }
            if (run > 0) hist[run < 7 ? run : 7]++;

            printf("             其中 tick 调用 %d 次; 每次发射: 1帧 %d / 2帧 %d / 3帧 %d / >=4帧 %d\n",
                   ticks, hist[1], hist[2], hist[3],
                   hist[4] + hist[5] + hist[6] + hist[7]);
        }
    }
    printf("-----------------------------------------------------\n\n");
}

void test_benchmark_io()
{
    benchmark_thread_sleep();
    benchmark_timer();
    benchmark_fixed_timer();
    benchmark_file_open();
    benchmark_file_write();
    benchmark_file_read();
    benchmark_file_seek();
    benchmark_dir();
    benchmark_md5();
}

int main(int argc, char *argv[])
{
    JZApplication app;
    test_benchmark_io();
    return 0;
}
