#include "JZTest.h"
#include "JZStd.h"
#include "JZString.h"
#include "JZRegExp.h"
#include "JZByteArray.h"
#include "JZElapsedTimer.h"
#include "JZApplication.h"
#include "JZTimer.h"
#include "JZObject.h"
#include "JZThread.h"
#include "test_thread.h"
#include "test_mutexpointer.h"
#include <cstdio>
#include <cstring>
#include "JZDebug.h"
#include "JZOptional.h"
#include "JZResult.h"
#include "math.h"
#include "JZDataStream.h"
#include <vector>

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
    std::string utf8 = str_local_to_utf8("test一下中文test");
    std::string local = str_utf8_to_local(utf8);
    JZ_TEST_EQ(local, "test一下中文test");

    std::wstring wstr = str_to_wstr("test一下中文test");
    std::string str = wstr_to_str(wstr);
    JZ_TEST_EQ(str, "test一下中文test");
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

// ---- JZDataStream 字节序正确性验证 ----
// 这些用例验证: 大小端都能正确序列化/反序列化, 且大端确实做了字节交换,
// 单字节类型不受字节序影响, 与主机字节序无关(任何平台都能跑通)。

static bool jzdatastream_endian_roundtrip()
{
    int32_t val = 0x12345678;

    // 小端写入再读回
    JZByteArray leBuf;
    JZDataStream wle(&leBuf, JZ::WriteOnly);
    wle << val;
    int32_t leBack = 0;
    JZDataStream rle(&leBuf, JZ::ReadOnly);
    rle >> leBack;
    JZ_TEST_EQ(leBack, val);

    // 大端写入再读回
    JZByteArray beBuf;
    JZDataStream wbe(&beBuf, JZ::WriteOnly);
    wbe.setByteOrder(JZDataStream::BigEndian);
    wbe << val;
    int32_t beBack = 0;
    JZDataStream rbe(&beBuf, JZ::ReadOnly);
    rbe.setByteOrder(JZDataStream::BigEndian);
    rbe >> beBack;
    JZ_TEST_EQ(beBack, val);

    // 小端与大端的字节序列互为逆序(证明大端确实做了字节交换)
    JZ_TEST_EQ(leBuf.size(), beBuf.size());
    for (int i = 0; i < (int)leBuf.size(); ++i)
        JZ_TEST_EQ((uint8_t)leBuf[i], (uint8_t)beBuf[beBuf.size() - 1 - i]);

    return true;
}

static bool jzdatastream_endian_cross_order()
{
    // 小端写入, 大端读出 => 得到字节反转后的值
    JZByteArray buf;
    JZDataStream w(&buf, JZ::WriteOnly);
    w << (int32_t)0x12345678;

    int32_t r = 0;
    JZDataStream rds(&buf, JZ::ReadOnly);
    rds.setByteOrder(JZDataStream::BigEndian);
    rds >> r;
    JZ_TEST_EQ(r, (int32_t)0x78563412);

    return true;
}

static bool jzdatastream_endian_byte_pattern()
{
    // 大端下, 0x11223344 的 4 个字节应为 11 22 33 44 (与主机字节序无关)
    JZByteArray beBuf;
    JZDataStream wbe(&beBuf, JZ::WriteOnly);
    wbe.setByteOrder(JZDataStream::BigEndian);
    wbe << (int32_t)0x11223344;
    JZ_TEST_EQ((uint8_t)beBuf[0], (uint8_t)0x11);
    JZ_TEST_EQ((uint8_t)beBuf[1], (uint8_t)0x22);
    JZ_TEST_EQ((uint8_t)beBuf[2], (uint8_t)0x33);
    JZ_TEST_EQ((uint8_t)beBuf[3], (uint8_t)0x44);

    // 小端下应为 44 33 22 11
    JZByteArray leBuf;
    JZDataStream wle(&leBuf, JZ::WriteOnly);
    wle << (int32_t)0x11223344;
    JZ_TEST_EQ((uint8_t)leBuf[0], (uint8_t)0x44);
    JZ_TEST_EQ((uint8_t)leBuf[1], (uint8_t)0x33);
    JZ_TEST_EQ((uint8_t)leBuf[2], (uint8_t)0x22);
    JZ_TEST_EQ((uint8_t)leBuf[3], (uint8_t)0x11);

    return true;
}

static bool jzdatastream_endian_float_double()
{
    double d = 3.141592653589793;
    JZByteArray leBuf, beBuf;

    JZDataStream wle(&leBuf, JZ::WriteOnly); wle << d;
    JZDataStream wbe(&beBuf, JZ::WriteOnly); wbe.setByteOrder(JZDataStream::BigEndian); wbe << d;

    // 大小端字节序列互为逆序
    JZ_TEST_EQ(leBuf.size(), beBuf.size());
    for (int i = 0; i < (int)leBuf.size(); ++i)
        JZ_TEST_EQ((uint8_t)leBuf[i], (uint8_t)beBuf[beBuf.size() - 1 - i]);

    // 同序读回应得到原值
    double dle = 0, dbe = 0;
    JZDataStream rle(&leBuf, JZ::ReadOnly); rle >> dle;
    JZDataStream rbe(&beBuf, JZ::ReadOnly); rbe.setByteOrder(JZDataStream::BigEndian); rbe >> dbe;
    JZ_TEST_EQ(dle, d);
    JZ_TEST_EQ(dbe, d);

    return true;
}

static bool jzdatastream_endian_uint8_noswap()
{
    // 单字节类型不受字节序影响
    JZByteArray leBuf, beBuf;
    JZDataStream wle(&leBuf, JZ::WriteOnly); wle << (uint8_t)0xAB;
    JZDataStream wbe(&beBuf, JZ::WriteOnly); wbe.setByteOrder(JZDataStream::BigEndian); wbe << (uint8_t)0xAB;
    JZ_TEST_EQ(leBuf.size(), 1);
    JZ_TEST_EQ(beBuf.size(), 1);
    JZ_TEST_EQ((uint8_t)leBuf[0], (uint8_t)0xAB);
    JZ_TEST_EQ((uint8_t)beBuf[0], (uint8_t)0xAB);

    uint8_t rle = 0, rbe = 0;
    JZDataStream rle2(&leBuf, JZ::ReadOnly); rle2 >> rle;
    JZDataStream rbe2(&beBuf, JZ::ReadOnly); rbe2.setByteOrder(JZDataStream::BigEndian); rbe2 >> rbe;
    JZ_TEST_EQ(rle, (uint8_t)0xAB);
    JZ_TEST_EQ(rbe, (uint8_t)0xAB);
    return true;
}

static bool jzdatastream_endian_vector()
{
    std::vector<uint32_t> v = {0x11223344u, 0x55667788u, 0x99AABBCCu};

    // 大端写入: 前 4 字节为 size 字段, 之后每个元素应为大端字节排列(与主机序无关)
    JZByteArray beBuf;
    JZDataStream wbe(&beBuf, JZ::WriteOnly);
    wbe.setByteOrder(JZDataStream::BigEndian);
    wbe << v;
    JZ_TEST_EQ((uint8_t)beBuf[4], (uint8_t)0x11);
    JZ_TEST_EQ((uint8_t)beBuf[5], (uint8_t)0x22);
    JZ_TEST_EQ((uint8_t)beBuf[6], (uint8_t)0x33);
    JZ_TEST_EQ((uint8_t)beBuf[7], (uint8_t)0x44);

    // 大端读回应得到原 vector
    std::vector<uint32_t> vr;
    JZDataStream rbe(&beBuf, JZ::ReadOnly);
    rbe.setByteOrder(JZDataStream::BigEndian);
    rbe >> vr;
    JZ_TEST_EQ(vr.size(), v.size());
    for (size_t i = 0; i < v.size(); ++i)
        JZ_TEST_EQ((uint32_t)vr[i], (uint32_t)v[i]);

    // 小端写入: 前 4 字节为 size 字段, 首元素字节应为 44 33 22 11
    JZByteArray leBuf;
    JZDataStream wle(&leBuf, JZ::WriteOnly);
    wle << v;
    JZ_TEST_EQ((uint8_t)leBuf[4], (uint8_t)0x44);
    JZ_TEST_EQ((uint8_t)leBuf[5], (uint8_t)0x33);
    JZ_TEST_EQ((uint8_t)leBuf[6], (uint8_t)0x22);
    JZ_TEST_EQ((uint8_t)leBuf[7], (uint8_t)0x11);

    std::vector<uint32_t> vrl;
    JZDataStream rle(&leBuf, JZ::ReadOnly);
    rle >> vrl;
    JZ_TEST_EQ(vrl.size(), v.size());
    for (size_t i = 0; i < v.size(); ++i)
        JZ_TEST_EQ((uint32_t)vrl[i], (uint32_t)v[i]);

    return true;
}

static bool jzdatastream_endian_vector_bool()
{
    // std::vector<bool> 是位压缩特化, 必须逐元素序列化(走显式特化, 不走批量裸写)。
    std::vector<bool> v = {true, false, true, true, false, true, false, false};

    // 小端写入: 每个 bool 占 1 字节(0/1), 头部为 size
    JZByteArray leBuf;
    JZDataStream wle(&leBuf, JZ::WriteOnly);
    wle << v;
    JZ_TEST_EQ((int32_t)leBuf.size(), (int32_t)(4 + v.size()));   // 4(size) + 8(bool)
    JZ_TEST_EQ((uint8_t)leBuf[4], (uint8_t)1);   // v[0]=true
    JZ_TEST_EQ((uint8_t)leBuf[5], (uint8_t)0);   // v[1]=false

    std::vector<bool> vrl;
    JZDataStream rle(&leBuf, JZ::ReadOnly);
    rle >> vrl;
    JZ_TEST_EQ(vrl.size(), v.size());
    for (size_t i = 0; i < v.size(); ++i)
        JZ_TEST_TRUE((bool)vrl[i] == (bool)v[i]);

    // 大端写入: bool 单字节无字节序问题, 但 size 字段(4 字节)仍受字节序影响,
    // 故大小端缓冲整体不会逐字节相同, 只验证元素部分(跳过 4 字节 size)一致。
    JZByteArray beBuf;
    JZDataStream wbe(&beBuf, JZ::WriteOnly);
    wbe.setByteOrder(JZDataStream::BigEndian);
    wbe << v;
    JZ_TEST_EQ(beBuf.size(), leBuf.size());
    for (int i = 4; i < (int)leBuf.size(); ++i)   // 跳过 size 字段
        JZ_TEST_EQ((uint8_t)beBuf[i], (uint8_t)leBuf[i]);

    std::vector<bool> vrb;
    JZDataStream rbe(&beBuf, JZ::ReadOnly);
    rbe.setByteOrder(JZDataStream::BigEndian);
    rbe >> vrb;
    JZ_TEST_EQ(vrb.size(), v.size());
    for (size_t i = 0; i < v.size(); ++i)
        JZ_TEST_TRUE((bool)vrb[i] == (bool)v[i]);

    return true;
}

bool jztimer_singleShot()
{
    jzInfo() << "jztimer_singleShot begin";

    JZObject root;
    JZTimer timer(&root);
    timer.setSingleShot(true);

    int fired = 0;
    connect(&timer.sigTimeout, [&fired]() { fired += 1; });

    timer.start(100);

    // 等待足够时间让单次定时器触发一次
    JZTest::exec(500);

    jzInfo() << "jztimer_singleShot fired = " << fired;
    JZ_TEST_EQ(fired, 1);
    return true;
}

bool jztimer_repeat()
{
    jzInfo() << "jztimer_repeat begin";

    JZObject root;
    JZTimer timer(&root);
    timer.setSingleShot(false);

    int fired = 0;
    connect(&timer.sigTimeout, [&fired]() { fired += 1; });

    timer.start(100);

    // 等待约 1 秒，预期触发多次
    JZTest::exec(1000);
    timer.stop();

    jzInfo() << "jztimer_repeat fired = " << fired;
    // 误差范围内：至少应触发 5 次（1s / 100ms）
    JZ_TEST_TRUE(fired >= 5);
    return true;
}

bool jztimer_stop()
{
    jzInfo() << "jztimer_stop begin";

    JZObject root;
    JZTimer timer(&root);

    int fired = 0;
    connect(&timer.sigTimeout, [&fired]() { fired += 1; });

    timer.start(100);
    JZTest::exec(200);
    timer.stop();

    int before = fired;
    // 停止后等待，不应再触发
    JZTest::exec(500);

    jzInfo() << "jztimer_stop before = " << before
             << ", after = " << fired;
    JZ_TEST_EQ(fired, before);
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

    auto dataStreamSuite = std::make_shared<JZTestSuite>("JZDataStream");
    test->addSuite(dataStreamSuite);
    dataStreamSuite->addTestCase("endian_roundtrip", jzdatastream_endian_roundtrip);
    dataStreamSuite->addTestCase("endian_cross_order", jzdatastream_endian_cross_order);
    dataStreamSuite->addTestCase("endian_byte_pattern", jzdatastream_endian_byte_pattern);
    dataStreamSuite->addTestCase("endian_float_double", jzdatastream_endian_float_double);
    dataStreamSuite->addTestCase("endian_uint8_noswap", jzdatastream_endian_uint8_noswap);
    dataStreamSuite->addTestCase("endian_vector", jzdatastream_endian_vector);
    dataStreamSuite->addTestCase("endian_vector_bool", jzdatastream_endian_vector_bool);

    auto timerSuite = std::make_shared<JZTestSuite>("JZTimer");
    test->addSuite(timerSuite);
    timerSuite->addTestCase("singleShot", jztimer_singleShot);
    timerSuite->addTestCase("repeat", jztimer_repeat);
    timerSuite->addTestCase("stop", jztimer_stop);
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
    JZApplication app;

    JZTest test;
    
    jzDebug() << jzVersion();
    smoke_debug_optional_result();
    test_JZCore(&test);
    test_JZThread(&test);
    test_JZMutexPointer(&test);
    test.run(argc, argv);
    return 0;
}