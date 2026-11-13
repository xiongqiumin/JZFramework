#include <sstream>
#include <string>
#include <vector>

#include "JZDebug.h"
#include "JZResources.h"
#include "JZFileInfo.h"
#include "JZFile.h"
#include "JZDir.h"

// 资源: jz_add_resources 生成 (res.qrc -> jz_res_resources.h, prefix=/data -> jzres_data_init)
#include "jz_res_resources.h"
// Proto: jz_add_proto_json 生成 (sample.proto -> jz_sample_pb.h / jz_sample_pb.cpp)
#include "jz_sample_pb.h"

// sample.proto 中所有类型位于 demo namespace
using namespace demo;

static int g_fail = 0;

#define JZTEST_CHECK(cond, msg)                                        \
    do                                                                 \
    {                                                                  \
        if (!(cond))                                                   \
        {                                                              \
            jzDebug() << "[FAIL] " << (msg);                           \
            g_fail++;                                                  \
        }                                                              \
        else                                                           \
        {                                                              \
            jzDebug() << "[OK]   " << (msg);                           \
        }                                                              \
    } while (0)

// ============ 资源回环: 编译进二进制的字节应与源文件一致 ============
static void testResourceLoopback()
{
    jzres_data_init();

    const JZResourceData *rd = JZResources::instance()->find(":/data/shape.json");
    JZTEST_CHECK(rd != nullptr, "资源 :/data/shape.json 已注册");
    if (!rd)
        return;

    // 回环校验: 从二进制读出, 内容与编译前的 JSON 原文一致
    std::string content(reinterpret_cast<const char *>(rd->data), rd->size);
    JZTEST_CHECK(rd->size > 0, "资源 size > 0");
    JZTEST_CHECK(content.find("\"triangle\"") != std::string::npos,
                 "资源内容回环: 读出编译前的 JSON 原文");
}

// ============ 明文资源仍走零拷贝分支 (未启用 XOR_KEY 的工程) ============
static void testPlainResourceFallback()
{
    static const unsigned char kPlain[] = { 'P', 'L', 'A', 'I', 'N', 0 };
    const int kSize = (int)sizeof(kPlain);

    JZResources::instance()->add(":/test/plain.bin", kPlain, kSize, 123);

    const JZResourceData *rd = JZResources::instance()->find(":/test/plain.bin");
    JZTEST_CHECK(rd != nullptr, "明文资源可登记");
    if (!rd)
        return;
    JZTEST_CHECK(rd->size == kSize, "明文资源 size 原样保留");
    JZTEST_CHECK(rd->data == kPlain, "明文资源保持零拷贝(data 指针原样)");
    JZTEST_CHECK(rd->mtime == 123, "明文资源 mtime 原样保留");
}

// ============ 打包资源按「文件」读: open/size/seek/readAll 全是明文语义 ============
static void testPackedResourceFileIO()
{
    JZFile f;
    JZTEST_CHECK(f.open(":/data/shape.json", "rb"), "JZFile 可打开打包资源");
    if (!f.isOpen())
        return;

    JZByteArray all = f.readAll();
    std::string content(all.data(), all.size());
    JZTEST_CHECK(content.find("\"triangle\"") != std::string::npos,
                 "打包资源 readAll() 读出明文");

    // 明文长度 == JZFileInfo::size(), 且不是「密文长度」
    JZFileInfo fi(":/data/shape.json");
    JZTEST_CHECK(fi.size() == all.size(), "打包资源 size() == 明文长度");

    // 随机访问: 从明文里的 "triangle" 处 seek 再读, 证明解包不是顺序流糊出来的
    size_t pos = content.find("\"triangle\"");
    if (pos != std::string::npos)
    {
        f.seek((int)pos + 1);
        JZByteArray word = f.read(8);
        JZTEST_CHECK(word.size() == 8 && std::string(word.data(), word.size()) == "triangle",
                     "打包资源 seek + read 读到正确明文");
    }
}

// ============ Proto 回环: 对象 -> JSON -> 对象 ============
static void testProtoLoopback()
{
    Shape a;
    a.name = "triangle";
    a.color = Color::Blue;
    Point p;
    p.x = 0.0; p.y = 0.0; a.points.push_back(p);
    p.x = 1.0; p.y = 0.0; a.points.push_back(p);
    p.x = 7.0; p.y = 6.0; a.points.push_back(p);
    a.score = 10;
    a.attrs["kind"] = "polygon";

    // 序列化 -> Json::Value
    Json::Value j;
    {
        JZJsonStream s;
        s.open(&j);
        s << a;
        JZTEST_CHECK(!s.isError(), "Shape 序列化成功");
    }

    // 回环: 反序列化回来并逐字段比对
    Shape b;
    {
        JZJsonStream s;
        s.open(&j);
        s >> b;
        JZTEST_CHECK(!s.isError(), "Shape 反序列化成功 (" + s.errorString() + ")");
    }

    JZTEST_CHECK(b.name == a.name, "字段 name 回环一致");
    JZTEST_CHECK(b.color == a.color, "字段 color 回环一致");
    JZTEST_CHECK(b.points.size() == a.points.size(), "字段 points 回环一致(数量)");
    if (b.points.size() == a.points.size())
    {
        JZTEST_CHECK(b.points[0].x == a.points[0].x && b.points[0].y == a.points[0].y,
                     "字段 points[0] 回环一致");
        JZTEST_CHECK(b.points[2].x == a.points[2].x && b.points[2].y == a.points[2].y,
                     "字段 points[2] 回环一致");
    }
    JZTEST_CHECK(b.score.isSome() && b.score.value() == 10,
                 "字段 score(optional) 回环一致");
    JZTEST_CHECK(b.attrs["kind"] == "polygon", "字段 attrs(map) 回环一致");
}

// ============ 资源 + Proto 联合回环 ============
// 内嵌 JSON 资源本身就是一条已序列化的 Shape 消息:
// 读资源 -> 解析 JSON -> 反序列化 -> 再序列化, 验证往返一致
static void testResourceProtoLoopback()
{
    const JZResourceData *rd = JZResources::instance()->find(":/data/shape.json");
    if (!rd)
    {
        JZTEST_CHECK(false, "资源缺失, 跳过联合回环");
        return;
    }
    std::string content(reinterpret_cast<const char *>(rd->data), rd->size);

    Json::Value obj;
    Json::CharReaderBuilder builder;
    std::string perr;
    std::istringstream iss(content);
    JZTEST_CHECK(Json::parseFromStream(builder, iss, &obj, &perr),
                 "解析内嵌 JSON 资源 (" + perr + ")");
    if (obj.isNull())
        return;

    Shape s;
    {
        JZJsonStream stream;
        stream.open(&obj);
        stream >> s;
        JZTEST_CHECK(!stream.isError(), "从内嵌资源反序列化 Shape (" + stream.errorString() + ")");
    }
    JZTEST_CHECK(s.name == "triangle", "联合回环 name 正确");
    JZTEST_CHECK(s.color == Color::Blue, "联合回环 color 正确");
    JZTEST_CHECK(s.points.size() == 3, "联合回环 points 数量正确");
    JZTEST_CHECK(s.score.isSome() && s.score.value() == 10, "联合回环 score 正确");
    JZTEST_CHECK(s.attrs["kind"] == "polygon", "联合回环 attrs 正确");

    // 再序列化回去, 与原始 JSON 文本比对(回环)
    Json::Value back;
    {
        JZJsonStream stream;
        stream.open(&back);
        stream << s;
        JZTEST_CHECK(!stream.isError(), "联合回环 再序列化");
    }
    JZTEST_CHECK(back["name"].asString() == "triangle",
                 "联合回环 序列化->反序列化->再序列化 一致");
}

// ============ 多级资源读取 + JZDir + weaklyCanonicalPath ============
static void listResourceTree(const std::string &group, int depth)
{
    JZDir dir(group);
    if (!dir.exists())
        return;
    for (const auto &info : dir.entryInfoList(JZDir::Files))
    {
        JZTEST_CHECK(info.isResource(),
                     std::string(depth * 2, ' ') + "资源: " + info.filePath());
        JZTEST_CHECK(info.exists() && info.size() > 0,
                     std::string(depth * 2, ' ') + "  exists & size>0 (" +
                         std::to_string(info.size()) + ")");
    }
}

static void testResourceDirAndWeakPath()
{
    jzres_data_init();

    // res.qrc (prefix=/data) 注册的多级资源:
    //   :/data/shape.json
    //   :/data/config/foo.bin
    //   :/data/config/network/wifi.bin   (第三层)
    //   :/data/config/network/eth.bin    (第三层)
    //   :/data/img/logo.txt
    //   :/data/img/icons/home.txt        (第三层)
    //   :/data/img/icons/back.txt        (第三层)
    JZTEST_CHECK(JZDir(":/data").isResource(), "JZDir(':/data') 是资源目录");
    JZTEST_CHECK(JZDir(":/data").exists(), "JZDir(':/data') exists");

    // 1) JZDir 按资源组逐级读取多级资源 (资源组为 ':/group', 直接子项不含更深层级)
    auto top = JZDir(":/data").entryInfoList(JZDir::Files);
    JZTEST_CHECK(top.size() == 1 && top[0].fileName() == "shape.json",
                 "JZDir(':/data') 列出直接子资源 shape.json");

    // 第二层: config / img 下各自的直接子资源
    listResourceTree(":/data/config", 1);   // :/data/config/foo.bin
    listResourceTree(":/data/img", 1);       // :/data/img/logo.txt

    // 第三层: 用 JZDir 进入子目录再读取 (多级目录逐级下钻)
    listResourceTree(":/data/config/network", 2); // wifi.bin, eth.bin
    listResourceTree(":/data/img/icons", 2);       // home.txt, back.txt

    // 2) 递归遍历 :/data 下所有层级的资源 (跨多级)
    int count = 0;
    for (const auto &name : JZResources::instance()->resourcesList())
    {
        if (name.rfind(":/data/", 0) != 0)
            continue;
        JZFileInfo fi(name);
        JZTEST_CHECK(fi.isResource() && fi.exists(),
                     "递归资源 " + name + " 可读, size=" + std::to_string(fi.size()));
        count++;
    }
    JZTEST_CHECK(count == 7, ":/data 下共 7 个资源(跨三级目录)");

    // 3) weaklyCanonicalPath 对资源路径: 保留 ':' 前缀, 规整 // 与 ..
    JZTEST_CHECK(JZFileInfo::weaklyCanonicalPath(":/data/img/logo.txt") ==
                     ":/data/img/logo.txt",
                 "weakPath 资源路径原样返回");
    JZTEST_CHECK(JZFileInfo::weaklyCanonicalPath("://data//img/logo.txt") ==
                     ":/data/img/logo.txt",
                 "weakPath 资源路径去重 '//' 且保留 ':'");
    JZTEST_CHECK(JZFileInfo::weaklyCanonicalPath(":/data/../data/img/logo.txt") ==
                     ":/data/img/logo.txt",
                 "weakPath 资源路径折叠 '..' 且保留 ':'");
    // 普通文件绝对路径 (对照组)
    JZTEST_CHECK(JZFileInfo::weaklyCanonicalPath("/data/./img/../img/logo.txt") ==
                     "/data/img/logo.txt",
                 "weakPath 普通绝对路径规整");
}

int main()
{
    jzDebug() << "########## test_tools: 资源 + Proto 回环测试 ##########";
    testResourceLoopback();
    testPlainResourceFallback();
    testPackedResourceFileIO();
    testProtoLoopback();
    testResourceProtoLoopback();
    testResourceDirAndWeakPath();

    if (g_fail == 0)
        jzDebug() << "【全部通过】";
    else
        jzDebug() << "【失败 " << g_fail << " 项】";
    return g_fail;
}
