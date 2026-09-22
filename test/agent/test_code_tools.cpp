// 代码工具类的冒烟测试: search_file(按文件名找文件) / search_context(搜文本·正则)
//
// 为什么要单测这两个名字: 以前只有一个 "search_file", 做的却是"搜内容" —— 名字与行为不符,
// 模型想"按文件名找文件"只能拿 file_filter 硬凑。现在拆开:
//   search_file    按文件名 / 相对路径找文件(通配符), 只回路径
//   search_context 搜文本 / 正则, 回 "路径:行号:内容"
// 这一层是模型真正看到的东西(回包文案、跳过说明、上限提示), 所以单独测:
//
// 覆盖:
//   工具名与注册名一致(改名没漏) / 通配符与"文件名或路径任一命中" / 大小写不敏感 / 递归
//   跳过生成目录(build)并在回包里说明 / max_results 上限提示 / 无匹配时的统计
//   search_context 的文本·正则·file_filter·非法正则报错 / 参数缺失报错
// 期望输出: ALL PASS
//
// 注: 两个工具都是**同步**回包(finish 里直接 emit), 所以不需要事件循环与轮询。

#include <stdio.h>

#include <fstream>
#include <string>

#include "JZApplication.h"
#include "JZCodeTools.h"
#include "JZDir.h"
#include "json/json.h"

using namespace std;

static int g_fail = 0;

static void check(bool ok, const string &what)
{
    printf("%s %s\n", ok ? "[ok]  " : "[fail]", what.c_str());
    if (!ok)
        g_fail++;
}

static bool contains(const string &text, const string &needle)
{
    return text.find(needle) != string::npos;
}

// 宿主: 工具只需要"终端配置(目录边界)"与"读文件"两件事, 这里给最小实现
class TestHost : public ICodeToolHost
{
public:
    JZVirtualTerminalConfig cfg;

    const JZVirtualTerminalConfig &toolTerminalConfig() const override { return cfg; }

    bool toolReadFile(const std::string &absPath, std::string &text, std::string &error) override
    {
        std::ifstream in(absPath.c_str(), std::ios::binary);
        if (!in.is_open())
        {
            error = "读不了文件: " + absPath;
            return false;
        }
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        text = content;
        return true;
    }
};

// 一次调用的回包
struct LastResult
{
    int count = 0;
    bool ok = false;
    string text;
};

class CodeToolTest
{
public:
    int run()
    {
        // 自己的临时工作区(建在 exe 目录下), 免得污染仓库
        m_ws = jzApp->applicationDirPath() + "/codetools_ws";
        JZDir(m_ws).removeRecursively();
        JZDir().mkpath(m_ws);

        m_host.cfg.zone.workspace = m_ws;

        makeFile("src/a.ts", "const a = 1; // NEEDLE_A\n");
        makeFile("src/sub/b.ts", "// NEEDLE_B\nconst b = 2;\n");
        makeFile("other/c.ts", "// needle_c(小写)\n");
        // 类型声明(真实工程里的布局: jz_types/<包>/types/*.d.ts)
        makeFile("jz_types/net/types/http.d.ts", "export function createServer(): void;\n");
        makeFile("jz_types/core/types/bytes.d.ts", "export class ByteReader {}\n");
        // 生成目录里的同名文件: 默认必须被跳过, 且要在回包里说明
        makeFile("build/gen/skip.d.ts", "export const generated: number;\n");

        printf("     workspace=%s\n\n", m_ws.c_str());

        m_file.setHost(&m_host);
        m_context.setHost(&m_host);
        connect(&m_file.sigResult, [this](const ToolCallResult &r) { record(m_fileRes, r); });
        connect(&m_context.sigResult, [this](const ToolCallResult &r) { record(m_contextRes, r); });

        check(m_file.toolName() == "search_file", "工具名 = search_file (找文件)");
        check(m_context.toolName() == "search_context", "工具名 = search_context (搜内容, 原 search_file 改名)");

        testFindByName();
        testFindByPathPattern();
        testFindScopeAndOptions();
        testFindEdges();
        testSearchContent();
        testSearchEdges();
        testTextClassifyEdges();
        testSkippedCountOnlyCountsScanned();

        printf("\n%s: fail=%d\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
        return g_fail;
    }

private:
    static void record(LastResult &out, const ToolCallResult &r)
    {
        out.count++;
        out.ok = r.isOk;
        out.text = r.result;
    }

    void makeFile(const string &rel, const string &content)
    {
        const string abs = m_ws + "/" + rel;
        const size_t slash = abs.find_last_of('/');
        if (slash != string::npos)
            JZDir().mkpath(abs.substr(0, slash));      // 父目录不存在就建出来
        std::ofstream out(abs.c_str(), std::ios::binary);
        out.write(content.c_str(), (std::streamsize)content.size());
        out.close();
    }

    static ToolCall makeCall(const string &argsJson)
    {
        ToolCall tc;
        tc.id = "call";
        tc.type = "function";
        tc.function_args = argsJson;
        Json::Reader reader;
        if (!reader.parse(argsJson, tc.params))
            tc.params = Json::Value(Json::objectValue);
        return tc;
    }

    void runFile(const string &argsJson)
    {
        m_fileRes = LastResult();
        m_file.call(makeCall(argsJson));
    }

    void runContext(const string &argsJson)
    {
        m_contextRes = LastResult();
        m_context.call(makeCall(argsJson));
    }

    // ---- search_file ----

    void testFindByName()
    {
        printf("search_file: 按文件名\n");
        runFile("{\"pattern\":\"*.d.ts\"}");
        check(m_fileRes.ok && m_fileRes.count == 1, "调用成功且只回一次包");
        check(contains(m_fileRes.text, "jz_types/net/types/http.d.ts"), "命中 jz_types/net/types/http.d.ts");
        check(contains(m_fileRes.text, "jz_types/core/types/bytes.d.ts"), "命中整棵树里的同类文件(递归)");
        check(contains(m_fileRes.text, "共 2 项"), "只回 jz_types 下那两个, 不含其它扩展名");
        check(m_fileRes.text.find("build/gen/skip.d.ts") == string::npos, "生成目录 build 里的文件没被扫到");
        check(contains(m_fileRes.text, "已跳过这些生成/隐藏目录") && contains(m_fileRes.text, "build"),
              "回包里说明了被跳过的目录(build)");

        runFile("{\"pattern\":\"b.ts\"}");
        check(contains(m_fileRes.text, "src/sub/b.ts"), "按精确文件名命中(含子目录)");

        runFile("{\"pattern\":\"*.D.TS\"}");
        check(contains(m_fileRes.text, "jz_types/net/types/http.d.ts"), "默认不区分大小写(*.D.TS 也能命中)");
        runFile("{\"pattern\":\"*.D.TS\",\"case_sensitive\":true}");
        check(contains(m_fileRes.text, "没有匹配"), "case_sensitive=true 时不命中");
    }

    void testFindByPathPattern()
    {
        printf("search_file: 按路径通配\n");
        runFile("{\"pattern\":\"jz_types/*/types/*.d.ts\"}");
        check(m_fileRes.ok && contains(m_fileRes.text, "共 2 项"),
              "路径式通配命中 2 个(靠相对路径匹配, 不是文件名)");
        check(contains(m_fileRes.text, "jz_types/net/types/http.d.ts") &&
                  contains(m_fileRes.text, "jz_types/core/types/bytes.d.ts"),
              "两个包目录都在结果里");

        runFile("{\"pattern\":\"src,jz_types/core\"}");
        check(contains(m_fileRes.text, "src/a.ts") || contains(m_fileRes.text, "src/"),
              "逗号分隔多个模式的第一个生效");
        check(contains(m_fileRes.text, "jz_types/core"), "逗号分隔多个模式的第二个也生效");
    }

    void testFindScopeAndOptions()
    {
        printf("search_file: path 与上限\n");
        runFile("{\"pattern\":\"*.ts\",\"path\":\"src\"}");
        check(m_fileRes.ok, "指定 path=src 能跑");
        check(contains(m_fileRes.text, "src/a.ts") && contains(m_fileRes.text, "src/sub/b.ts"),
              "path 之下递归命中两个 .ts");
        check(m_fileRes.text.find("other/c.ts") == string::npos, "path 之外的文件没被扫到");

        runFile("{\"pattern\":\"*.ts\",\"path\":\"src/sub/b.ts\"}");
        check(m_fileRes.ok && contains(m_fileRes.text, "src/sub/b.ts"), "path 直接给一个文件时命中它就回它");

        runFile("{\"pattern\":\"*.ts\",\"path\":\"src/a.ts\",\"file_filter\":\"x\"}");
        check(m_fileRes.ok, "path 给文件时不再受 file_filter 影响(那是 search_context 的参数)");

        runFile("{\"pattern\":\"*.ts\",\"max_results\":1}");
        check(contains(m_fileRes.text, "已达 max_results 上限"), "max_results 触顶有明确提示");

        // 真去生成目录里找: 跳过只作用于"递归的子目录", 直接指过去应该能扫到
        runFile("{\"pattern\":\"skip.d.ts\",\"path\":\"build\"}");
        check(m_fileRes.ok && contains(m_fileRes.text, "build/gen/skip.d.ts"),
              "直接把 path 指到生成目录时能扫到里面的文件");
    }

    void testFindEdges()
    {
        printf("search_file: 边界\n");
        runFile("{\"pattern\":\"\"}");
        check(!m_fileRes.ok && contains(m_fileRes.text, "pattern 不能为空"), "空 pattern 明确报错");

        runFile("{\"pattern\":\"nope*.zzz\"}");
        check(m_fileRes.ok && contains(m_fileRes.text, "没有匹配"), "没有匹配时回统计信息, 不是错误");
        check(contains(m_fileRes.text, "已扫描"), "无匹配回包带扫描量");

        runFile("{\"pattern\":\"*.ts\",\"path\":\"not_exist_dir\"}");
        check(!m_fileRes.ok && contains(m_fileRes.text, "路径不存在"), "path 不存在明确报错");

        runFile("{\"pattern\":\"c.ts\",\"path\":\"other/c.ts\"}");
        check(m_fileRes.ok, "path 直接给文件且命中");

        runFile("{\"pattern\":\"x.ts\",\"path\":\"other/c.ts\"}");
        check(!m_fileRes.ok && contains(m_fileRes.text, "不匹配"), "path 给文件但不匹配时明确报错(不静默回空)");
    }

    // ---- search_context ----

    void testSearchContent()
    {
        printf("search_context: 搜内容\n");
        runContext("{\"pattern\":\"NEEDLE_A\"}");
        check(m_contextRes.ok && m_contextRes.count == 1, "调用成功且只回一次包");
        check(contains(m_contextRes.text, "src/a.ts:1:"), "回 \"相对路径:行号:内容\"");

        runContext("{\"pattern\":\"needle_b\"}");
        check(contains(m_contextRes.text, "src/sub/b.ts:1:"), "默认不区分大小写");

        runContext("{\"pattern\":\"needle_b\",\"case_sensitive\":true}");
        check(contains(m_contextRes.text, "没有找到匹配"), "case_sensitive=true 时不命中");

        runContext("{\"pattern\":\"export\",\"path\":\"jz_types\",\"regex\":true}");
        check(contains(m_contextRes.text, "jz_types/net/types/http.d.ts:1:") &&
                  contains(m_contextRes.text, "jz_types/core/types/bytes.d.ts:1:"),
              "regex=true + path=jz_types 命中两份声明");

        runContext("{\"pattern\":\"NEEDLE_[AB]\",\"regex\":true}");
        check(contains(m_contextRes.text, "共 2 处"), "正则命中两处");

        runContext("{\"pattern\":\"const\",\"path\":\"src\",\"file_filter\":\"*.ts\"}");
        check(contains(m_contextRes.text, "src/a.ts:1:"), "file_filter 生效");
        runContext("{\"pattern\":\"const\",\"path\":\"src\",\"file_filter\":\"*.cpp\"}");
        check(contains(m_contextRes.text, "没有找到匹配"), "file_filter 不匹配时不命中");
    }

    void testSearchEdges()
    {
        printf("search_context: 边界\n");
        runContext("{\"pattern\":\"(\",\"regex\":true}");
        check(!m_contextRes.ok && contains(m_contextRes.text, "正则表达式无效"), "非法正则明确报错, 不静默变成没匹配");

        runContext("{\"pattern\":\"\"}");
        check(!m_contextRes.ok && contains(m_contextRes.text, "pattern 不能为空"), "空 pattern 明确报错");

        runContext("{\"pattern\":\"generated\",\"path\":\"build/gen/skip.d.ts\"}");
        check(m_contextRes.ok && contains(m_contextRes.text, "build/gen/skip.d.ts:1:"),
              "path 直接指文件时可以搜生成目录里的文件");
    }

    // ---- 文本/二进制判定与"跳过"统计 ----

    // isTextFile 只采样前 16KB 判 UTF-8 合法性, 而采样边界**可能正好切在一个多字节字符中间** ——
    // 那时不能把文件判成二进制(否则 read_file / modify_file / search_* 都会拒绝一份正常源码)。
    // 构造: 纯"中"(3 字节/字) 8000 字 = 24000 字节 ⇒ 第 16384 字节正落在某个"中"的中间。
    void testTextClassifyEdges()
    {
        printf("isTextFile: UTF-8 采样边界\n");
        string cjk;
        for (int i = 0; i < 8000; i++)
            cjk += "\xE4\xB8\xAD";                 // "中" 的 UTF-8 三字节
        const string rel = "src/zh_big.txt";
        makeFile(rel, cjk + "\nNEEDLE_ZH\n");

        check(isTextFile(m_ws + "/" + rel),
              "16384 采样边界切在多字节字符中间时仍判为文本(不是二进制)");

        runContext("{\"pattern\":\"NEEDLE_ZH\",\"path\":\"" + rel + "\"}");
        check(m_contextRes.ok && contains(m_contextRes.text, "NEEDLE_ZH"),
              "边界切在中文中间的文件也能被 search_context 搜到");
    }

    // "另有 N 个文件被跳过"是给模型看的**信号**(漏了哪些), 所以只该统计"本来要搜、但没搜成"的文件;
    // 被 file_filter 排除掉的(模型自己指定的过滤)不该算进来, 否则回包会谎报漏搜。
    void testSkippedCountOnlyCountsScanned()
    {
        printf("search_context: 跳过统计只算本要搜的文件\n");
        makeFile("src/huge.dat", string(3 * 1024 * 1024, 'x'));   // 3MB, 超过 2MB 单文件上限

        runContext("{\"pattern\":\"NEEDLE_A\",\"path\":\"src\",\"file_filter\":\"*.ts\"}");
        check(m_contextRes.ok && contains(m_contextRes.text, "src/a.ts:1:"), "过滤 *.ts 后仍能搜到");
        check(m_contextRes.text.find("被跳过") == string::npos,
              "被 file_filter 排除的大文件不该算进\"跳过\"统计(不然模型以为漏搜了相关文件)");
    }

    string m_ws;
    TestHost m_host;
    JZSearchFileTool m_file;
    JZSearchContextTool m_context;
    LastResult m_fileRes;
    LastResult m_contextRes;
};

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    JZApplication app;
    CodeToolTest test;
    return test.run();
}
