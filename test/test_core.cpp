#include "JZTest.h"
#include "JZStd.h"
#include "JZString.h"
#include "JZRegExp.h"
#include "JZByteArray.h"

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


int main(int argc,char *argv[])
{
    JZTest test;
    test_JZCore(&test);
    test.run(argc, argv);
    return 0;
}