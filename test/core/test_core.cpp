#include "JZTest.h"
#include "JZStd.h"
#include "JZString.h"
#include "JZRegExp.h"
#include "JZByteArray.h"
#include "JZElapsedTimer.h"
#include "JZApplication.h"
#include "JZThread.h"
#include "test_thread.h"
#include "test_mutexpointer.h"
#include <cstdio>
#include <cstring>
#include "JZDebug.h"
#include "JZOptional.h"
#include "JZResult.h"

bool jzstd_str_basic()
{
    JZ_TEST_TRUE(str_startsWith("hello world", "hello"));
    JZ_TEST_TRUE(str_endsWith("hello world", "world"));
    JZ_TEST_TRUE(str_contains("hello world", "lo"));
    JZ_TEST_FALSE(str_startsWith("hello", "world"));
    return true;
}

bool jzstd_str_case_insensitive()
{
    JZ_TEST_TRUE(str_startsWith("Hello World", "hello", false));
    JZ_TEST_TRUE(str_endsWith("Hello World", "WORLD", false));
    JZ_TEST_TRUE(str_contains("Hello World", "wo", false));
    return true;
}

bool jzstd_str_convert()
{
    JZ_TEST_EQ(str_toLower("HELLO"), "hello");
    JZ_TEST_EQ(str_toUpper("hello"), "HELLO");
    JZ_TEST_EQ(str_trimmed("  hello  "), "hello");
    return true;
}

bool jzstd_str_split_join()
{
    auto parts = str_split("a,b,c", ",");
    JZ_TEST_EQ(parts.size(), 3u);
    JZ_TEST_EQ(parts[0], "a");
    JZ_TEST_EQ(parts[1], "b");
    JZ_TEST_EQ(parts[2], "c");

    JZ_TEST_EQ(str_join(parts, "-"), "a-b-c");
    return true;
}

bool jzstd_str_index()
{
    JZ_TEST_EQ(str_indexOf("hello world", "world"), 6);
    JZ_TEST_EQ(str_indexOf("hello world", "xyz"), -1);
    JZ_TEST_EQ(str_lastIndexOf("ababa", "aba"), 2);
    return true;
}

bool jzstd_str_convert_numeric()
{
    bool ok = false;
    JZ_TEST_EQ(str_toInt("123", &ok), 123);
    JZ_TEST_TRUE(ok);
    JZ_TEST_EQ(str_toDouble("3.14", &ok), 3.14);
    JZ_TEST_TRUE(ok);
    return true;
}

bool jzstd_vec_basic()
{
    std::vector<int> vec = {1, 2, 3, 4, 5};
    JZ_TEST_EQ(vec_indexOf(vec, 3), 2);
    JZ_TEST_EQ(vec_indexOf(vec, 10), -1);
    JZ_TEST_TRUE(vec_contains(vec, 4));
    JZ_TEST_FALSE(vec_contains(vec, 6));
    return true;
}

bool jzstd_vec_modify()
{
    std::vector<int> vec = {1, 2, 3};
    vec_removeAt(vec, 1);
    JZ_TEST_EQ(vec.size(), 2u);
    JZ_TEST_EQ(vec[0], 1);
    JZ_TEST_EQ(vec[1], 3);
    return true;
}

bool jzstd_map_basic()
{
    std::map<std::string, int> m;
    m["key1"] = 1;
    m["key2"] = 2;
    JZ_TEST_TRUE(map_contains(m, std::string("key1")));
    JZ_TEST_FALSE(map_contains(m, std::string("key3")));
    JZ_TEST_EQ(map_value(m, std::string("key2"), 0), 2);
    JZ_TEST_EQ(map_value(m, std::string("key3"), 0), 0);
    return true;
}

bool jzstd_str_chop()
{
    std::string s = "hello";
    str_chop(s, 2);
    JZ_TEST_EQ(s, "hel");
    return true;
}

bool jzstd_str_mid_left_right()
{
    std::string s = "abcdef";
    JZ_TEST_EQ(str_mid(s, 2, 3), "cde");
    JZ_TEST_EQ(str_mid(s, 2), "cdef");
    JZ_TEST_EQ(str_left(s, 3), "abc");
    JZ_TEST_EQ(str_right(s, 3), "def");
    return true;
}

bool jzstd_str_replace_remove()
{
    std::string s = "hello world";
    str_replace(s, "world", "universe");
    JZ_TEST_EQ(s, "hello universe");

    std::string s2 = "abcabc";
    str_remove(s2, "abc");
    JZ_TEST_EQ(s2, "");
    return true;
}

bool jzstd_str_prepend_append()
{
    std::string s = "world";
    str_prepend(s, "hello ");
    JZ_TEST_EQ(s, "hello world");

    str_append(s, "!");
    JZ_TEST_EQ(s, "hello world!");
    return true;
}

bool jzstd_str_normalized()
{
    std::string s = "  hello   world  ";
    JZ_TEST_EQ(str_normalized(s), "hello world");
    return true;
}

bool jzstd_str_asprintf()
{
    std::string s = str_asprintf("%s %d", "hello", 123);
    JZ_TEST_EQ(s, "hello 123");
    return true;
}

bool jzstd_str_index_case_insensitive()
{
    JZ_TEST_EQ(str_indexOf("Hello World", "world", 0, false), 6);
    JZ_TEST_EQ(str_lastIndexOf("AbcABC", "abc", -1, false), 3);
    return true;
}

bool jzstd_str_convert_numeric_base()
{
    bool ok = false;
    JZ_TEST_EQ(str_toInt("1010", &ok, 2), 10);
    JZ_TEST_TRUE(ok);
    JZ_TEST_EQ(str_toInt("FF", &ok, 16), 255);
    JZ_TEST_TRUE(ok);
    return true;
}

bool jzstd_str_encoding_convert()
{
    std::string utf8 = str_local_to_utf8("test");
    std::string local = str_utf8_to_local(utf8);
    JZ_TEST_EQ(local, "test");

    std::wstring wstr = str_to_wstr("test");
    std::string str = wstr_to_str(wstr);
    JZ_TEST_EQ(str, "test");
    return true;
}

bool jzstd_vec_pop_front()
{
    std::vector<int> vec = {1, 2, 3};
    vec_popFront(vec);
    JZ_TEST_EQ(vec.size(), 2u);
    JZ_TEST_EQ(vec[0], 2);
    JZ_TEST_EQ(vec[1], 3);
    return true;
}

bool jzstd_vec_remove_one_all()
{
    std::vector<int> vec = {1, 2, 2, 3, 2};
    vec_removeOne(vec, 2);
    JZ_TEST_EQ(vec.size(), 4u);
    JZ_TEST_EQ(vec[0], 1);
    JZ_TEST_EQ(vec[1], 2);

    std::vector<int> vec2 = {1, 2, 2, 3, 2};
    vec_removeAll(vec2, 2);
    JZ_TEST_EQ(vec2.size(), 2u);
    JZ_TEST_EQ(vec2[0], 1);
    JZ_TEST_EQ(vec2[1], 3);
    return true;
}

bool jzstd_vec_append_mid()
{
    std::vector<int> dst = {1, 2};
    std::vector<int> src = {3, 4, 5};
    vec_append(dst, src);
    JZ_TEST_EQ(dst.size(), 5u);
    JZ_TEST_EQ(dst[4], 5);

    std::vector<int> vec = {1, 2, 3, 4, 5};
    std::vector<int> mid = vec_mid(vec, 1, 3);
    JZ_TEST_EQ(mid.size(), 3u);
    JZ_TEST_EQ(mid[0], 2);
    JZ_TEST_EQ(mid[1], 3);
    JZ_TEST_EQ(mid[2], 4);
    return true;
}

bool jzstd_map_insert_remove()
{
    std::map<std::string, int> m;
    map_insert(m, std::string("key1"), 1);
    JZ_TEST_TRUE(map_contains(m, std::string("key1")));

    map_remove(m, std::string("key1"));
    JZ_TEST_FALSE(map_contains(m, std::string("key1")));
    return true;
}

bool jzstd_map_keys_key()
{
    std::map<std::string, int> m;
    m["a"] = 1;
    m["b"] = 2;
    m["c"] = 3;

    auto keys = map_keys(m);
    JZ_TEST_EQ(keys.size(), 3u);
    JZ_TEST_TRUE(map_contains(m, keys[0]));

    JZ_TEST_EQ(map_key(m, 2, std::string("")), std::string("b"));
    JZ_TEST_EQ(map_key(m, 100, std::string("default")), std::string("default"));
    return true;
}

bool jzstd_jz_delete_all()
{
    std::vector<int*> vec;
    vec.push_back(new int(1));
    vec.push_back(new int(2));
    JZ_TEST_EQ(vec.size(), 2u);
    jzDeleteAll(vec);

    std::map<int, int*> m;
    m[1] = new int(10);
    m[2] = new int(20);
    JZ_TEST_EQ(m.size(), 2u);
    jzDeleteAll(m);
    return true;
}

bool jzstring_basic()
{
    JZString s("hello");
    JZ_TEST_EQ(s.size(), 5);
    JZ_TEST_FALSE(s.isEmpty());
    JZ_TEST_EQ(s.toStdString(), "hello");
    return true;
}

bool jzstring_compare()
{
    JZString a("apple"), b("banana"), c("apple");
    JZ_TEST_TRUE(a == c);
    JZ_TEST_TRUE(a != b);
    JZ_TEST_TRUE(a < b);
    JZ_TEST_TRUE(b > a);
    JZ_TEST_TRUE(a <= c);
    JZ_TEST_TRUE(b >= a);
    return true;
}

bool jzstring_concat()
{
    JZString s("hello");
    s += " world";
    JZ_TEST_EQ(s.toStdString(), "hello world");

    JZString s2 = s + "!";
    JZ_TEST_EQ(s2.toStdString(), "hello world!");

    JZString s3 = "Hi, " + s;
    JZ_TEST_EQ(s3.toStdString(), "Hi, hello world");
    return true;
}

bool jzstring_access()
{
    JZString s("test");
    JZ_TEST_EQ(s[0].toChar(), 't');
    JZ_TEST_EQ(s.at(1).toChar(), 'e');
    JZ_TEST_EQ(s.front().toChar(), 't');
    JZ_TEST_EQ(s.back().toChar(), 't');
    return true;
}

bool jzstring_find()
{
    JZString s("hello world");
    JZ_TEST_EQ(s.indexOf("world"), 6);
    JZ_TEST_EQ(s.indexOf("xyz"), -1);
    JZ_TEST_EQ(s.lastIndexOf("l"), 9);
    JZ_TEST_TRUE(s.contains("lo"));
    JZ_TEST_TRUE(s.startsWith("hello"));
    JZ_TEST_TRUE(s.endsWith("world"));
    return true;
}

bool jzstring_substring()
{
    JZString s("abcdef");
    JZ_TEST_EQ(s.mid(2, 3).toStdString(), "cde");
    JZ_TEST_EQ(s.mid(2).toStdString(), "cdef");
    JZ_TEST_EQ(s.left(3).toStdString(), "abc");
    JZ_TEST_EQ(s.right(3).toStdString(), "def");
    return true;
}

bool jzstring_modify()
{
    JZString s("hello");
    s.insert(2, "XX");
    JZ_TEST_EQ(s.toStdString(), "heXXllo");

    s.remove(2, 2);
    JZ_TEST_EQ(s.toStdString(), "hello");

    s.replace(0, 2, "Hi");
    JZ_TEST_EQ(s.toStdString(), "Hillo");
    return true;
}

bool jzstring_case()
{
    JZString s("Hello World");
    JZ_TEST_EQ(s.toUpper().toStdString(), "HELLO WORLD");
    JZ_TEST_EQ(s.toLower().toStdString(), "hello world");
    JZ_TEST_EQ(JZString("  test  ").trimmed().toStdString(), "test");
    return true;
}

bool jzstring_convert()
{
    JZ_TEST_EQ(JZString::number(123).toStdString(), "123");
    JZ_TEST_EQ(JZString::number(3.14).toStdString(), "3.14");

    bool ok = false;
    JZ_TEST_EQ(JZString("456").toInt(&ok), 456);
    JZ_TEST_TRUE(ok);
    JZ_TEST_EQ(JZString("2.5").toDouble(&ok), 2.5);
    JZ_TEST_TRUE(ok);
    return true;
}

bool jzstring_split()
{
    JZString s("a,b,c,d");
    auto parts = s.split(",");
    JZ_TEST_EQ(parts.size(), 4u);
    JZ_TEST_EQ(parts[0].toStdString(), "a");
    JZ_TEST_EQ(parts[3].toStdString(), "d");
    return true;
}

bool jzstring_from_std()
{
    std::string stdStr = "test string";
    JZString jzStr = JZString::fromStdString(stdStr);
    JZ_TEST_EQ(jzStr.toStdString(), stdStr);
    return true;
}

bool jzbytearray_basic()
{
    JZByteArray ba;
    JZ_TEST_TRUE(ba.isEmpty());
    JZ_TEST_EQ(ba.size(), 0);
    return true;
}

bool jzbytearray_append()
{
    JZByteArray ba;
    ba.push_back('h');
    ba.push_back('e');
    ba.push_back('l');
    JZ_TEST_EQ(ba.size(), 3);
    JZ_TEST_EQ(ba[0], 'h');
    JZ_TEST_EQ(ba[1], 'e');
    JZ_TEST_EQ(ba[2], 'l');
    return true;
}

bool jzbytearray_mid()
{
    const char* data = "abcdef";
    JZByteArray ba(data, 6);
    JZByteArray mid = ba.mid(2, 3);
    JZ_TEST_EQ(mid.size(), 3);
    JZ_TEST_EQ(mid[0], 'c');
    JZ_TEST_EQ(mid[1], 'd');
    JZ_TEST_EQ(mid[2], 'e');
    return true;
}

bool jzbytearray_resize()
{
    JZByteArray ba;
    ba.resize(5, 'x');
    JZ_TEST_EQ(ba.size(), 5);
    for (int i = 0; i < 5; ++i) {
        JZ_TEST_EQ(ba[i], 'x');
    }
    return true;
}

bool jzbytearray_clear()
{
    JZByteArray ba("test", 4);
    ba.clear();
    JZ_TEST_TRUE(ba.isEmpty());
    JZ_TEST_EQ(ba.size(), 0);
    return true;
}

void test_JZCore(JZTest *test)
{
    auto stdSuite = std::make_shared<JZTestSuite>("JZStd");
    test->addSuite(stdSuite);
    stdSuite->addTestCase("str_basic", jzstd_str_basic);
    stdSuite->addTestCase("str_case_insensitive", jzstd_str_case_insensitive);
    stdSuite->addTestCase("str_convert", jzstd_str_convert);
    stdSuite->addTestCase("str_split_join", jzstd_str_split_join);
    stdSuite->addTestCase("str_index", jzstd_str_index);
    stdSuite->addTestCase("str_convert_numeric", jzstd_str_convert_numeric);
    stdSuite->addTestCase("str_chop", jzstd_str_chop);
    stdSuite->addTestCase("str_mid_left_right", jzstd_str_mid_left_right);
    stdSuite->addTestCase("str_replace_remove", jzstd_str_replace_remove);
    stdSuite->addTestCase("str_prepend_append", jzstd_str_prepend_append);
    stdSuite->addTestCase("str_normalized", jzstd_str_normalized);
    stdSuite->addTestCase("str_asprintf", jzstd_str_asprintf);
    stdSuite->addTestCase("str_index_case_insensitive", jzstd_str_index_case_insensitive);
    stdSuite->addTestCase("str_convert_numeric_base", jzstd_str_convert_numeric_base);
    stdSuite->addTestCase("str_encoding_convert", jzstd_str_encoding_convert);
    stdSuite->addTestCase("vec_basic", jzstd_vec_basic);
    stdSuite->addTestCase("vec_modify", jzstd_vec_modify);
    stdSuite->addTestCase("vec_pop_front", jzstd_vec_pop_front);
    stdSuite->addTestCase("vec_remove_one_all", jzstd_vec_remove_one_all);
    stdSuite->addTestCase("vec_append_mid", jzstd_vec_append_mid);
    stdSuite->addTestCase("map_basic", jzstd_map_basic);
    stdSuite->addTestCase("map_insert_remove", jzstd_map_insert_remove);
    stdSuite->addTestCase("map_keys_key", jzstd_map_keys_key);
    stdSuite->addTestCase("jz_delete_all", jzstd_jz_delete_all);

    auto strSuite = std::make_shared<JZTestSuite>("JZString");
    test->addSuite(strSuite);
    strSuite->addTestCase("basic", jzstring_basic);
    strSuite->addTestCase("compare", jzstring_compare);
    strSuite->addTestCase("concat", jzstring_concat);
    strSuite->addTestCase("access", jzstring_access);
    strSuite->addTestCase("find", jzstring_find);
    strSuite->addTestCase("substring", jzstring_substring);
    strSuite->addTestCase("modify", jzstring_modify);
    strSuite->addTestCase("case", jzstring_case);
    strSuite->addTestCase("convert", jzstring_convert);
    strSuite->addTestCase("split", jzstring_split);
    strSuite->addTestCase("from_std", jzstring_from_std);    

    auto bytearraySuite = std::make_shared<JZTestSuite>("JZByteArray");
    test->addSuite(bytearraySuite);
    bytearraySuite->addTestCase("basic", jzbytearray_basic);
    bytearraySuite->addTestCase("append", jzbytearray_append);
    bytearraySuite->addTestCase("mid", jzbytearray_mid);
    bytearraySuite->addTestCase("resize", jzbytearray_resize);
    bytearraySuite->addTestCase("clear", jzbytearray_clear);
}


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

static void smoke_debug_optional_result()
{
    // 验证 JZDebug 对 JZOptional / JZResult 的流式输出 (含 void 特化)
    JZOptional<int> oi = JZOptional<int>::Some(42);
    JZOptional<int> on = JZOptional<int>::None();
    JZOptional<void> ov = JZOptional<void>::Some();
    JZOptional<void> onv = JZOptional<void>::None();

    JZResult<int> ri = JZResult<int>::Ok(7);
    JZResult<int> re = JZResult<int>::Error(3, "boom");
    JZResult<void> rv = JZResult<void>::Ok();
    JZResult<void> rev = JZResult<void>::Error(5, "fail");

    jzInfo() << "smoke JZOptional<int> Some:" << oi;
    jzInfo() << "smoke JZOptional<int> None:" << on;
    jzInfo() << "smoke JZOptional<void> Some:" << ov;
    jzInfo() << "smoke JZOptional<void> None:" << onv;
    jzInfo() << "smoke JZResult<int> Ok:" << ri;
    jzInfo() << "smoke JZResult<int> Err:" << re;
    jzInfo() << "smoke JZResult<void> Ok:" << rv;
    jzInfo() << "smoke JZResult<void> Err:" << rev;
}

int main(int argc,char *argv[])
{
    smoke_debug_optional_result();
    benchmark_audio_base64();
    benchmark_base64_512();

    JZApplication app;

    JZTest test;
    test_JZCore(&test);
    test_JZThread(&test);
    test_JZMutexPointer(&test);
    test.run(argc, argv);
    return 0;
}