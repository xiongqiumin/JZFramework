#include "JZByteArray.h"
#include "JZVariant.h"
#include "JZCircleBuffer.h"
#include "JZAllocator.h"
#include "JZTest.h"
#include "JZThread.h"
#include "math.h"
#include <cstring>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <map>
#include <string>

static void benchmark_audio_base64()
{
    // 16000Hz, 1秒, 16bit 单声道 PCM = 32000 字节
    const int sampleRate = 16000;
    const int durationSec = 1;
    const int bytesPerSample = 2; // 16bit
    const int channels = 1;
    const int totalBytes = sampleRate * durationSec * bytesPerSample * channels; // 32000

    printf("\n========== Audio Benchmark ==========\n");
    printf("Config: %dHz, %ds, %dbit, %dch => %d bytes\n",
           sampleRate, durationSec, bytesPerSample * 8, channels, totalBytes);

    // 生成模拟 PCM 数据 (正弦波)
    JZByteArray srcPcm;
    srcPcm.resize(totalBytes);
    for (int i = 0; i < sampleRate; ++i) {
        int16_t sample = (int16_t)(32767.0 * sin(2.0 * 3.1415926 * 440.0 * i / sampleRate));
        srcPcm[i * 2] = sample & 0xFF;
        srcPcm[i * 2 + 1] = (sample >> 8) & 0xFF;
    }

    // 自适应 benchmark: 目标总耗时 100ms, 迭代数自动推算 (含预热, 报稳态值)
    printf("---------- Adaptive Benchmark (JZBenchmark) ----------\n");
    JZBenchmark bm(/*targetMs=*/100);
    JZByteArray b64Data = srcPcm.toBase64(); // 复用编码数据
    JZByteArray dst, decoded, b64Result;
    dst.resize(totalBytes); // 确保目标缓冲区足够大

    JZ_BENCH(bm, "memcpy(32KB)", [&]() { memcpy(dst.data(), srcPcm.data(), totalBytes); });
    JZ_BENCH(bm, "toBase64(32KB)", [&]() { b64Result = srcPcm.toBase64(); });
    JZ_BENCH(bm, "fromBase64", [&]() { decoded = JZByteArray::fromBase64(b64Data); });

    // 慢操作演示: 单次很耗时, 总时长被限制在 [minSec, maxSec] 内 (默认 0.01s~2s)
    JZBenchmark slowBm(/*targetMs=*/100);
    JZ_BENCH(slowBm, "sleep_1ms", [&]() { JZThread::msleep(1); });
    printf("-----------------------------------------------------\n\n");
}

static void benchmark_base64_512()
{
    // 1 秒@16kHz: 共 16000 个 short (32000 字节), 按 512 short (1024 字节) 分块
    const int totalShorts = 16000;
    const int chunkShorts = 512;
    const int stepBytes   = chunkShorts * 2;   // 每块 1024 字节
    const int totalBytes  = totalShorts * 2;   // 32000 字节

    printf("\n========== Base64 Loop Benchmark (for i<16000 step 512, toBase64) ==========\n");

    // 生成 1 秒音频数据 (伪随机 short)
    JZByteArray src;
    src.resize(totalBytes);
    for (int i = 0; i < totalShorts; ++i) {
        short v = (short)((i * 31 + 7) & 0xFFFF);
        src[i * 2]     = (char)(v & 0xFF);
        src[i * 2 + 1] = (char)((v >> 8) & 0xFF);
    }

    JZByteArray dst, decoded;

    // 预编码各块, 供解码循环使用
    std::vector<JZByteArray> b64Chunks;
    b64Chunks.reserve(totalShorts / chunkShorts);
    for (int i = 0; i < totalShorts; i += chunkShorts)
        b64Chunks.push_back(src.mid(i * 2, stepBytes).toBase64());

    // 测试这两个循环本身: 每次对 512-short 块编/解码, 跑满约 1 秒
    JZBenchmark bm(/*targetMs=*/1000);
    JZ_BENCH(bm, "toBase64 loop(16000/512)", [&]() {
        for (int i = 0; i < totalShorts; i += chunkShorts) {
            dst = src.mid(i * 2, stepBytes).toBase64();
        }
    });
    JZ_BENCH(bm, "fromBase64 loop(16000/512)", [&]() {
        for (size_t k = 0; k < b64Chunks.size(); ++k) {
            decoded = JZByteArray::fromBase64(b64Chunks[k]);
        }
    });

    printf("============================================================================\n\n");
}

static void benchmark_variant()
{
    printf("\n========== JVariant Benchmark ==========\n");

    // 构造各类型 variant
    JZVariant intVar(12345);
    JZVariant dblVar(3.1415926);
    JZVariant strVar(std::string("hello variant benchmark"));
    JZVariantList list;
    for (int i = 0; i < 16; ++i)
        list.push_back(JZVariant(i));
    JZVariant listVar(list);
    JZVariant customVar = JZVariant::fromValue<int>(777);

    // 结果汇出, 避免编译器把调用优化掉
    int sinkInt = 0;
    double sinkDbl = 0.0;
    size_t strSink = 0;
    size_t listSink = 0;

    JZBenchmark bm(/*targetMs=*/100);
    JZ_BENCH(bm, "construct(int)", [&]() { JZVariant v(12345); });
    JZ_BENCH(bm, "construct(string)", [&]() { JZVariant v(std::string("benchmark")); });
    JZ_BENCH(bm, "toInt", [&]() { sinkInt += intVar.toInt(); });
    JZ_BENCH(bm, "toDouble", [&]() { sinkDbl += dblVar.toDouble(); });
    JZ_BENCH(bm, "toString", [&]() { strSink += strVar.toString().size(); });
    JZ_BENCH(bm, "toList", [&]() { listSink += listVar.toList().size(); });
    JZ_BENCH(bm, "fromValue<int>", [&]() { JZVariant v = JZVariant::fromValue<int>(777); });
    JZ_BENCH(bm, "value<int>", [&]() { sinkInt += customVar.value<int>(); });
    JZ_BENCH(bm, "copy", [&]() { JZVariant v(intVar); });
    JZ_BENCH(bm, "operator=", [&]() { JZVariant v; v = intVar; });

    printf("  [sink] int=%d dbl=%.6f strSize=%zu listSize=%zu\n",
           sinkInt, sinkDbl, strSink, listSink);
    printf("-----------------------------------------------------\n\n");
}

static void benchmark_bytearray_ops()
{
    printf("\n========== JZByteArray Ops Benchmark ==========\n");

    JZByteArray src;
    src.resize(4096, 'A');
    JZByteArray sink;

    JZBenchmark bm(/*targetMs=*/100);
    JZ_BENCH(bm, "append(4KB)", [&]() { JZByteArray b; b.append(src.data(), 4096); });
    JZ_BENCH(bm, "mid(2KB@1KB)", [&]() { sink = src.mid(1024, 2048); });
    JZ_BENCH(bm, "remove(2KB@1KB)", [&]() {
        JZByteArray b(src);
        b.remove(1024, 2048);
        sink = b;
    });
    JZ_BENCH(bm, "left(2KB)", [&]() { sink = src.left(2048); });
    JZ_BENCH(bm, "resize+fill(4KB)", [&]() { JZByteArray b; b.resize(4096, 'x'); });
    JZ_BENCH(bm, "toHex(4KB)", [&]() { sink = src.toHex(); });
    JZ_BENCH(bm, "copy(4KB)", [&]() { JZByteArray b(src); });
    JZ_BENCH(bm, "operator[]", [&]() {
        volatile char c = 0;
        for (int i = 0; i < 4096; ++i)
            c ^= (char)src[i];
        (void)c;
    });

    printf("-----------------------------------------------------\n\n");
}

static void benchmark_circlebuffer()
{
    printf("\n========== JZCircleBuffer Benchmark ==========\n");

    const int block = 1024;
    char wbuf[1024];
    char rbuf[1024];
    memset(wbuf, 0xAB, sizeof(wbuf));

    JZCircleBuffer fixedBuf(4096, /*enableDynamic=*/false);
    JZCircleBuffer dynBuf(4096, /*enableDynamic=*/true);
    JZCircleBuffer growBuf(1024, /*enableDynamic=*/true);

    JZBenchmark bm(/*targetMs=*/100);
    JZ_BENCH(bm, "write+read(1KB,fixed)", [&]() {
        if (fixedBuf.write(wbuf, block))
            (void)fixedBuf.read(rbuf, block);
    });
    JZ_BENCH(bm, "write+read(1KB,dynamic)", [&]() {
        if (dynBuf.write(wbuf, block))
            (void)dynBuf.read(rbuf, block);
    });
    JZ_BENCH(bm, "write grow(2KB,1KB cap)", [&]() {
        growBuf.write(wbuf, 2048);
        growBuf.clear();
    });

    printf("-----------------------------------------------------\n\n");
}

static void benchmark_alloc()
{
    printf("\n========== JZAllocator Benchmark ==========\n");

    // 别名避免在 JZ_BENCH 宏参数里出现 '<int, ...>' 逗号 (宏按逗号拆分参数)
    using JZIntVec = std::vector<int, JZAllocator<int>>;

    JZBenchmark bm(/*targetMs=*/100);

    JZ_BENCH(bm, "JZAllocator malloc+free(1KB)", [&]() {
        JZAllocator<char> a;
        char *p = a.allocate(1024);
        a.deallocate(p, 1024);
    });
    JZ_BENCH(bm, "std malloc+free(1KB)", [&]() {
        char *p = (char *)std::malloc(1024);
        std::free(p);
    });
    JZ_BENCH(bm, "vector<int,JZAllocator> push 1K", [&]() {
        JZIntVec v;
        v.reserve(1024);
        for (int i = 0; i < 1024; ++i)
            v.push_back(i);
    });
    JZ_BENCH(bm, "vector<int,std::alloc> push 1K", [&]() {
        std::vector<int> v;
        v.reserve(1024);
        for (int i = 0; i < 1024; ++i)
            v.push_back(i);
    });

    printf("-----------------------------------------------------\n\n");
}

static void benchmark_stl()
{
    printf("\n========== std::vector / std::string / std::map Benchmark ==========\n");

    const int n = 1024;
    std::vector<int> base;
    base.reserve(n);
    for (int i = 0; i < n; ++i)
        base.push_back(i * 3 + 1);

    std::string baseStr;
    baseStr.reserve(4096);
    for (int i = 0; i < 256; ++i)
        baseStr += "abcdefgh"; // 2048 字符

    std::map<int, int> baseMap;
    for (int i = 0; i < n; ++i)
        baseMap[i] = i * 2;

    // 别名避免在 JZ_BENCH 宏参数里出现 '<int, int>' 逗号
    using MapT = std::map<int, int>;

    size_t sink = 0;
    int intSink = 0;

    JZBenchmark bm(/*targetMs=*/100);

    // ---- std::vector ----
    JZ_BENCH(bm, "vector push_back 1K", [&]() {
        std::vector<int> v;
        v.reserve(n);
        for (int i = 0; i < n; ++i)
            v.push_back(i);
    });
    JZ_BENCH(bm, "vector copy 1K", [&]() { std::vector<int> v(base); });
    JZ_BENCH(bm, "vector append(range)", [&]() {
        std::vector<int> v;
        v.insert(v.end(), base.begin(), base.end());
    });
    JZ_BENCH(bm, "vector random access 1K", [&]() {
        int c = 0;
        for (int i = 0; i < n; ++i)
            c += base[(i * 7) % n];
        intSink += c;
    });
    JZ_BENCH(bm, "vector find 1K", [&]() {
        intSink += (int)(std::find(base.begin(), base.end(), 777) != base.end());
    });

    // ---- std::string ----
    JZ_BENCH(bm, "string concat 256x8", [&]() {
        std::string s;
        s.reserve(4096);
        for (int i = 0; i < 256; ++i)
            s += "abcdefgh";
    });
    JZ_BENCH(bm, "string copy 2KB", [&]() { std::string s(baseStr); });
    JZ_BENCH(bm, "string find", [&]() {
        sink += baseStr.find("cdef");
    });
    JZ_BENCH(bm, "string substr(1KB)", [&]() {
        std::string s = baseStr.substr(512, 1024);
        sink += s.size();
    });

    // ---- std::map ----
    JZ_BENCH(bm, "map insert 1K", [&]() {
        MapT m;
        for (int i = 0; i < n; ++i)
            m[i] = i * 2;
    });
    JZ_BENCH(bm, "map copy 1K", [&]() { MapT m(baseMap); });
    JZ_BENCH(bm, "map lookup 1K", [&]() {
        int c = 0;
        for (int i = 0; i < n; ++i)
            c += baseMap.find(i)->second;
        intSink += c;
    });

    printf("  [sink] strFind=%zu intSink=%d\n", sink, intSink);
    printf("-----------------------------------------------------\n\n");
}

void test_benchmark()
{
    benchmark_audio_base64();
    benchmark_base64_512();
    benchmark_variant();
    benchmark_bytearray_ops();
    benchmark_circlebuffer();
    benchmark_alloc();
    benchmark_stl();
}