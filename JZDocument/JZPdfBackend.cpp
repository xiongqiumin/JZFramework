// MSVC: getenv 走 CRT 安全警告(C4996), 这里只为读一个可选环境变量, 直接关掉该警告
#ifdef _MSC_VER
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#endif

#include "JZDocument.h"

#include <stdlib.h>
#include <time.h>

#include "JZCore/JZDir.h"
#include "JZCore/JZFile.h"
#include "JZCore/JZFileInfo.h"
#include "JZCore/JZPlatform.h"
#include "JZCore/JZProcess.h"
#include "JZCore/JZStd.h"

// ============================================================================
// PDF 后端: 外部单二进制 CLI(jzpdf, PDFium 内核) + stdout JSON
//
// 主程序不链接 PDFium: 只通过 JZProcess 起进程、读 stdout、反序列化成 JZDocResult。
// 好处见 binary/jzpdf/main.cpp 头部注释(体积 / 崩溃隔离 / 后端可替换)。
// ============================================================================

namespace
{
// 定位后端 exe: 环境变量 → 当前 exe 同目录 → PATH
std::string findPdfCli()
{
    const char *env = getenv("JZDOC_PDF_CLI");
    if (env && *env)
        return env;

    std::string dir = JZPlatform::applicationDirPath();
    if (!dir.empty())
    {
#ifdef JZ_OS_WIN
        std::string exe = dir + "/jzpdf.exe";
#else
        std::string exe = dir + "/jzpdf";
#endif
        if (JZFileInfo::exists(exe))
            return exe;
    }
    return "jzpdf";   // 交给系统 PATH
}

std::string makeTempImageDir()
{
    static int counter = 0;
    counter++;
    std::string dir = JZDir::currentPath() + "/jzdoc_pdf_" +
                      str_asprintf("%d_%d", (int)time(nullptr), counter);
    JZDir d;
    if (!d.mkpath(dir))
        return std::string();
    return dir;
}

JZResult<JZDocResult> fail(const std::string &msg, const std::string &dir)
{
    if (!dir.empty())
    {
        JZDir d(dir);
        d.removeRecursively();
    }
    return JZResult<JZDocResult>::Error(msg);
}
}  // namespace

JZResult<JZDocResult> jzReadPdf(const std::string &path)
{
    if (!JZFileInfo::exists(path))
        return JZResult<JZDocResult>::Error("document not found: " + path);

    std::string cli = findPdfCli();
    std::string imageDir = makeTempImageDir();
    if (imageDir.empty())
        return JZResult<JZDocResult>::Error("cannot create temp dir for pdf images");

    // 结果写文件而不是走 stdout: JZProcess 的 stdout 靠定时器(事件循环)抽,
    // 调用方没在跑事件循环时, 大输出会把子进程堵在管道写 —— 直接落盘绕开这个坑。
    std::string resultPath = imageDir + "/result.json";

    std::vector<std::string> args;
    args.push_back("--image-dir");
    args.push_back(imageDir);
    args.push_back("--out");
    args.push_back(resultPath);
    args.push_back(path);

    JZProcess process;
    if (!process.start(cli, args))
        return fail("failed to start pdf backend '" + cli + "' (set JZDOC_PDF_CLI or put jzpdf next to the exe)", imageDir);

    if (!process.waitForFinished(120000))
    {
        process.killProcess();
        return fail("pdf backend timeout: " + path, imageDir);
    }

    JZFile outFile;
    std::string out;
    if (outFile.open(resultPath, "rb"))
    {
        JZByteArray bytes = outFile.readAll();
        outFile.close();
        out.assign(bytes.constData(), (size_t)bytes.size());
    }
    if (out.empty())
    {
        JZByteArray errBytes = process.readAllStandardError();
        std::string err(errBytes.constData(), (size_t)errBytes.size());
        return fail("pdf backend produced no output" + (err.empty() ? std::string() : ": " + err), imageDir);
    }

    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(out, root))
        return fail("cannot parse pdf backend json: " + reader.getFormattedErrorMessages(), imageDir);

    JZResult<JZDocResult> ret = JZDocResult::fromJson(root);
    if (!imageDir.empty())
    {
        JZDir d(imageDir);
        d.removeRecursively();
    }
    return ret;
}
