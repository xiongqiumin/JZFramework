#include "JZBenchmark.h"
#include "JZTest.h"
#include "JZStd.h"
#include "JZApplication.h"
#include "JZFile.h"
#include "JZDir.h"
#include "JZTimer.h"
#include "JZThread.h"
#include "JZCrypto.h"
#include <atomic>

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

// JZTimer 基准: 测量 singleShot(0) 经消息循环触发的调度延迟
static void benchmark_timer()
{
    printf("\n========== JZTimer Benchmark ==========\n");
    JZBenchmark bm(/*targetMs=*/100);
    bm.runAndPrint("JZTimer::singleShot(0) fire x100", []() {
        for (int i = 0; i < 100; i++)
        {
            std::atomic<bool> fired{false};
            JZTimer::singleShot(0, [&]() { fired = true; });
            for (int k = 0; k < 100 && !fired.load(); k++)
            {
                jzApp->processEvents();
                JZThread::msleep(1);
            }
            JZ_TEST_TRUE(fired.load());
        }
    });
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
        JZ_TEST_TRUE(dir.mkdir(kIoTestDir));

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
    printf("\n========== JZCrypto::md5 Benchmark ==========\n");
    JZByteArray data = makeIoTestData(kIoTestSize);
    JZBenchmark bm(/*targetMs=*/100);
    bm.runAndPrint("JZCrypto::md5 128k", [&]() {
        JZByteArray hash = JZCrypto::md5(data);
        JZ_TEST_EQ(hash.size(), 16);
    });
    printf("-----------------------------------------------------\n\n");
}

void test_benchmark_io()
{
    benchmark_thread_sleep();
    benchmark_timer();
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
