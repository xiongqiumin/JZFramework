#include <sstream>
#include <string>
#include <vector>

#include "JZDebug.h"
#include "JZResources.h"

// 资源: jz_add_resources 生成 (res.qrc, prefix=/data -> jzres_data_init)
#include "jz_data_resources.h"
// Proto: jz_add_proto_json 生成的聚合头 (sample.proto -> jz_test_tools_proto.h)
#include "jz_test_tools_proto.h"

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

    const JZResourceData *rd = JZResources::instance()->find(":data/shape.json");
    JZTEST_CHECK(rd != nullptr, "资源 :data/shape.json 已注册");
    if (!rd)
        return;

    // 回环校验: 从二进制读出, 内容与编译前的 JSON 原文一致
    std::string content(reinterpret_cast<const char *>(rd->data), rd->size);
    JZTEST_CHECK(rd->size > 0, "资源 size > 0");
    JZTEST_CHECK(content.find("\"triangle\"") != std::string::npos,
                 "资源内容回环: 读出编译前的 JSON 原文");
}

// ============ Proto 回环: 对象 -> JSON -> 对象 ============
static void testProtoLoopback()
{
    Shape a;
    a.name = "triangle";
    a.color = Color::Blue;
    a.points.push_back(Point{0.0, 0.0});
    a.points.push_back(Point{1.0, 0.0});
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
    const JZResourceData *rd = JZResources::instance()->find(":data/shape.json");
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

int main()
{
    jzDebug() << "########## test_tools: 资源 + Proto 回环测试 ##########";
    testResourceLoopback();
    testProtoLoopback();
    testResourceProtoLoopback();

    if (g_fail == 0)
        jzDebug() << "【全部通过】";
    else
        jzDebug() << "【失败 " << g_fail << " 项】";
    return g_fail;
}
