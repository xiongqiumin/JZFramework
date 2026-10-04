// ---------------------------------------------------------------------------
// jz_uic —— 从 .ui 文件(uiLoader 方言)生成 C++ 头文件(uic 风格)。
//
//   用法: jz_uic <in.ui> -o <out.h> [--class <UiClass>]
//
//   生成物: namespace Ui { class <UiClass> { ...成员 + root + byId...; void setupUi(); }; }
//   解析/方言判别复用 JZUiParser(与运行期 JZUiLoader、JS 侧生成器同源)。
//   构建期调用见 CMake 函数 jz_add_ui()(JZFramework.cmake)。
//
// 退出码: 0 成功; 1 失败(原因打到 stderr)。
// ---------------------------------------------------------------------------
#include "JZUiLoader.h"
#include "JZUiCppGen.h"

#include <stdio.h>
#include <fstream>
#include <string>

namespace {

// 路径 -> "BTEServer"(去目录与扩展名; 支持 .ui 与 .ui.xml 两种写法)
std::string baseName(const std::string& path)
{
    size_t slash = path.find_last_of("/\\");
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);

    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos)
        name = name.substr(0, dot);
    if (name.size() > 3 && name.compare(name.size() - 3, 3, ".ui") == 0)   // foo.ui.xml -> foo
        name = name.substr(0, name.size() - 3);

    return name;
}

} // namespace

int main(int argc, char** argv)
{
    std::string in, out, uiClass;

    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "-o" && i + 1 < argc)
            out = argv[++i];
        else if (a == "--class" && i + 1 < argc)
            uiClass = argv[++i];
        else if (!a.empty() && a[0] != '-' && in.empty())
            in = a;
    }

    if (in.empty() || out.empty())
    {
        fprintf(stderr, "用法: jz_uic <in.ui> -o <out.h> [--class <UiClass>]\n");
        return 1;
    }

    if (uiClass.empty())
        uiClass = baseName(in) + "Ui";

    JZUiLoader parser;   // 只借它的解析(不 build、不 new 控件 —— 生成期不需要 GUI)
    if (!parser.loadFromFile(in))
    {
        fprintf(stderr, "jz_uic: 解析失败 %s: %s\n", in.c_str(), parser.error().c_str());
        return 1;
    }

    const JZUiNode* root = parser.root();
    if (!root)
    {
        fprintf(stderr, "jz_uic: %s 里没有根 <widget>\n", in.c_str());
        return 1;
    }

    JZUiCppGen gen(*root, uiClass);
    const std::string code = gen.generate();
    if (code.empty())
    {
        fprintf(stderr, "jz_uic: 生成失败 %s: %s\n", in.c_str(),
                gen.error().empty() ? "(未知原因)" : gen.error().c_str());
        return 1;
    }

    const std::vector<std::string>& warns = gen.warnings();
    for (size_t i = 0; i < warns.size(); ++i)
        fprintf(stderr, "[jz_uic] 警告(%s): %s\n", in.c_str(), warns[i].c_str());

    // 带 UTF-8 BOM 写出: 与仓库里其它源码同一口径, MSVC 才会按 UTF-8 读中文字面量
    // (否则 "查询" 这类会变成乱码/警告)。没 BOM 时 GCC/Clang 也照读 UTF-8, 无副作用。
    std::ofstream f(out.c_str(), std::ios::binary | std::ios::trunc);
    if (!f)
    {
        fprintf(stderr, "jz_uic: 打不开输出文件 %s\n", out.c_str());
        return 1;
    }
    f.write("\xEF\xBB\xBF", 3);
    f.write(code.data(), (std::streamsize)code.size());
    f.close();
    if (!f)
    {
        fprintf(stderr, "jz_uic: 写输出文件失败 %s\n", out.c_str());
        return 1;
    }

    printf("[jz_uic] %s -> %s (class Ui::%s, %d 字节)\n",
           in.c_str(), out.c_str(), uiClass.c_str(), (int)code.size());
    return 0;
}
