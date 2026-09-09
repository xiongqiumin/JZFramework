#include "JZCompress.h"
#include "JZFile.h"
#include "JZDir.h"
#include "JZProcess.h"
#include "JZPlatform.h"
#include "JZDebug.h"
#include "json/json.h"

#include <chrono>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

/*
 * jzupdater (源码 binary/updater): OS_NORMAL 桌面平台独立更新器。
 *
 * 由 JZOTADriverNormal::reboot() 在应用退出前启动, 用法:
 *   jzupdater <job.json> [--shadow]
 *
 * job.json (由驱动写入 <appDir>/ota/job.json):
 *   {
 *     "package": "<升级包 zip 完整路径>",   // JZCompress 打包
 *     "appDir":  "<应用安装目录>",
 *     "appFile": "<应用可执行文件完整路径>",
 *     "restart": true                        // 更新完成后是否重启应用
 *   }
 *
 * 流程:
 *   0. 若运行的是已安装的 jzupdater 且升级包含它自己: 先把自己复制到
 *      <appDir>/ota/jzupdater_tmp, 以 --shadow 启动临时副本后退出;
 *      临时副本与 jzupdater.exe 不共享映像, 解压时即可覆盖它自身
 *   1. 等待主程序退出(exe 文件锁释放)
 *   2. 解压升级包覆盖安装目录
 *   3. 包根目录若含 meta.json, 安装到 <appDir>/ota/meta.json
 *      (内置版本信息, JZOTA::readCurrentMeta 桌面平台回退读取)
 *   4. 清理升级包与任务文件, 按需重启应用
 *
 * 临时副本运行中无法删除自身, 残留的 jzupdater_tmp 由下次更新启动时清理。
 */

// 等待主程序退出: 主程序运行中 exe 以写方式打开会失败, 以此判定文件锁释放。
static bool waitAppExit(const std::string &appFile, int timeoutMs)
{
    for (int waited = 0; waited < timeoutMs; waited += 200)
    {
        FILE *fp = fopen(appFile.c_str(), "rb+");
        if (fp)
        {
            fclose(fp);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

static bool copyFile(const std::string &src, const std::string &dst)
{
    JZFile in;
    if (!in.open(src, "rb"))
        return false;
    JZByteArray data = in.readAll();
    in.close();

    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
        return false;
    out.write(data.data(), data.size());
    return out.good();
}

static bool iequals(const std::string &a, const std::string &b)
{
#ifdef _WIN32
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
            return false;
    }
    return true;
#else
    return a == b;
#endif
}

// 当前是否为"已安装到 appDir 的 jzupdater"在运行(临时副本/手工副本均不匹配)
static bool isInstalledUpdater(const std::string &appDir)
{
    std::string installed = appDir + "/jzupdater";
#ifdef _WIN32
    installed += ".exe";
#endif
    return iequals(JZPlatform::applicationFilePath(), installed);
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        jzError() << "usage: jzupdater <job.json>";
        return 1;
    }

    // 读取任务文件
    JZFile jobFile;
    if (!jobFile.open(argv[1], "rb"))
    {
        jzError() << "JZUpdater: open job failed" << argv[1];
        return 1;
    }
    JZByteArray data = jobFile.readAll();
    jobFile.close();
    std::string text((const char *)data.data(), (size_t)data.size());

    Json::Value job;
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string errs;
    if (text.empty() || !reader->parse(text.data(), text.data() + text.size(), &job, &errs) ||
        !job.isObject())
    {
        jzError() << "JZUpdater: parse job failed:" << errs;
        return 1;
    }

    std::string package = job.isMember("package") ? job["package"].asString() : "";
    std::string appDir  = job.isMember("appDir")  ? job["appDir"].asString()  : "";
    std::string appFile = job.isMember("appFile") ? job["appFile"].asString() : "";
    bool restart        = job.isMember("restart") ? job["restart"].asBool() : true;

    if (package.empty() || appDir.empty())
    {
        jzError() << "JZUpdater: invalid job, package/appDir required";
        return 1;
    }

    std::string jobPath = argv[1];
    bool shadow = (argc >= 3) && (std::string(argv[2]) == "--shadow");

    // 自更新: 运行中的 exe 无法被覆盖。若当前是已安装的 jzupdater 且升级包
    // 可能包含它自己, 先把自己复制到 <otaDir>/jzupdater_tmp, 由临时副本
    // (与 jzupdater.exe 不共享映像)执行解压覆盖, 自身即可被安全替换。
    std::string otaDirPath = appDir + "/ota";
    std::string tmpUpdater = otaDirPath + "/jzupdater_tmp";
#ifdef _WIN32
    tmpUpdater += ".exe";
#endif
    if (!shadow && isInstalledUpdater(appDir))
    {
        JZDir dir;
        dir.mkpath(otaDirPath);
        JZFile::remove(tmpUpdater);   // 清理上次残留(忽略失败)

        if (copyFile(JZPlatform::applicationFilePath(), tmpUpdater) &&
            JZProcess::startDetached(tmpUpdater, { jobPath, "--shadow" }))
        {
            jzDebug() << "JZUpdater: shadow copy started" << tmpUpdater;
            return 0;
        }
        // 复制/拉起失败: 回退原地更新(仅当升级包不含 jzupdater 自身时可成功)
        jzWarning() << "JZUpdater: start shadow copy failed, continue in-place";
    }

    // 等待主程序退出(释放 exe 文件锁); 超时也继续尝试, 由解压失败兜底
    if (!appFile.empty() && !waitAppExit(appFile, 10000))
        jzWarning() << "JZUpdater: wait app exit timeout" << appFile;

    // 解压升级包覆盖安装目录
    JZUnZip zip;
    auto openRes = zip.open(package);
    if (openRes.isError())
    {
        jzError() << "JZUpdater: open package failed:" << openRes.errorMessage();
        return 1;
    }

    zip.setProgressCallback([](int64_t cur, int64_t total) {
        jzDebug() << "JZUpdater: unzip" << cur << "/" << total;
        return true;
    });
    auto unzipRes = zip.unzip(appDir);
    if (unzipRes.isError())
    {
        jzError() << "JZUpdater: unzip failed:" << unzipRes.errorMessage();
        return 1;
    }

    // 安装内置版本信息: 包根目录 meta.json -> <appDir>/ota/meta.json
    if (zip.contains("meta.json"))
    {
        JZDir dir;
        dir.mkpath(appDir + "/ota");
        auto metaRes = zip.extractToFile("meta.json", appDir + "/ota/meta.json");
        if (metaRes.isError())
            jzWarning() << "JZUpdater: install meta failed:" << metaRes.errorMessage();
        else
            jzDebug() << "JZUpdater: meta installed" << appDir + "/ota/meta.json";
    }

    // 清理升级包与任务文件
    JZFile::remove(package);
    JZFile::remove(argv[1]);

    // 临时副本尽力清理自身(Linux 可删运行中的映像; Windows 由下次更新启动时清理)
    if (shadow)
        JZFile::remove(tmpUpdater);

    // 重启应用
    if (restart && !appFile.empty())
    {
        if (!JZProcess::startDetached(appFile, {}, appDir))
        {
            jzError() << "JZUpdater: restart app failed" << appFile;
            return 1;
        }
    }

    jzDebug() << "JZUpdater: done";
    return 0;
}
