#include <vector>
#include <regex>
#include "JZCrashReport.h"
#include "JZProcess.h"
#include "JZFileInfo.h"
#include "JZApplication.h"
#include "JZPlatform.h"
#include "JZDebug.h"

std::string JZCrashReportManager::makeReport(const JZCrashReport &report)
{
    char sigBuf[64];
    snprintf(sigBuf, sizeof(sigBuf), "Signal: %d", report.signal);

    std::string reportStr;
    reportStr += "==================== Crash Report ====================\n";
    if (!report.platform.empty())
    {
        reportStr += "Platform: " + report.platform + "\n";
    }
    reportStr += sigBuf;
    reportStr += "\n";
    if(!report.assert.empty())
    {
        reportStr += "Assert: ";
        reportStr += report.assert + "\n";
    }
    reportStr += "Error: ";
    reportStr += report.msg;
    reportStr += "\n\n";

    reportStr += "Backtrace:\n";
    JZOptional<std::string> stack_parsed = parseCrashReport(report.stack);
    if (stack_parsed.isSome())
    {
        reportStr += stack_parsed.value() + "\n";
    }
    else
    {
        reportStr += report.stack;  // fallback 到原始堆栈
    }
    reportStr += "==================================================\n";

    return reportStr;
}

std::pair<std::string, std::string> JZCrashReportManager::addr2line(const std::string& offset,
                                                                    const std::string& addr2line_path,
                                              const std::string& exe_path)
{
    try
    {
        JZProcess process;
        std::vector<std::string> args = {"-f", "-C", "-e", exe_path, offset};
        if (!process.start(addr2line_path, args))
        {
            return {"??", "??"};
        }

        process.waitForFinished();
        std::string output = str_fromLocal(process.readAllStandardOutput());
        
        // 按行分割输出
        std::vector<std::string> lines;
        std::string line;
        for (char c : output)
        {
            if (c == '\n')
            {
                if (!line.empty())
                {
                    lines.push_back(line);
                }
                line.clear();
            }
            else
            {
                line += c;
            }
        }
        if (!line.empty())
            lines.push_back(line);

        if (lines.size() >= 2)
        {
            return {lines[0], lines[1]};
        }
    }
    catch (...)
    {
        // 捕获所有异常，保证不崩溃
    }

    return {"??", "??"};
}

JZOptional<std::string> JZCrashReportManager::parseCrashReport(const std::string& crash_report)
{
    if (JZPlatform::platform() == "linux")
        return parseCrashReportLinux(crash_report);
    else
        return JZOptional<std::string>::None();
}

JZOptional<std::string> JZCrashReportManager::parseCrashReportLinux(const std::string& crash_report)
{
    std::vector<std::string> stackLines;
    std::regex pattern(R"(([^\s]+)\(\+0x([0-9a-fA-F]+)\)\s+\[0x([0-9a-fA-F]+)\])");
    int frameIndex = 0;

    std::string addr2line_path = "/usr/bin/addr2line";
    if (!JZFileInfo::exists(addr2line_path))
        return JZOptional<std::string>::None();

    std::string exe_path = jzApp->applicationFilePath();
    // 获取可执行文件名
    std::string exeBaseName = JZFileInfo(exe_path).fileName();

    // 按行分割崩溃日志
    std::vector<std::string> lines;
    std::string line;
    for (char c : crash_report)
    {
        if (c == '\n')
        {
            lines.push_back(line);
            line.clear();
        }
        else
        {
            line += c;
        }
    }
    if (!line.empty())
        lines.push_back(line);

    for (const auto& logLine : lines)
    {
        if (logLine == "SymbolizedStack:")
            break;

        std::smatch match;
        if (std::regex_search(logLine, match, pattern))
        {
            std::string binary = match[1].str();
            std::string offset = "0x" + match[2].str();
            std::string address = "0x" + match[3].str();

            std::string binaryBaseName = JZFileInfo(binary).fileName();
            std::string frameStr;

            if (binaryBaseName.find(exeBaseName) != std::string::npos)
            {
                // 匹配目标程序，调用 addr2line 解析
                auto add_ret = addr2line(offset, addr2line_path, exe_path);
                auto &funcName = add_ret.first;
                auto &location = add_ret.second;

                if (location != "??" && location.find(':') != std::string::npos)
                {
                    // 移除 (discriminator xxx) 冗余信息
                    std::regex discRegex(R"(\s*\(discriminator\s+\d+\))");
                    std::string fileLine = std::regex_replace(location, discRegex, "");
                    frameStr = "#" + std::to_string(frameIndex) + " " + address + " in " + funcName + " " + fileLine;
                }
                else
                {
                    frameStr = "#" + std::to_string(frameIndex) + " " + address + " in " + funcName + " (" + binary +
                               "+" + offset + ")";
                }
            }
            else
            {
                // 非目标程序栈帧
                frameStr = "#" + std::to_string(frameIndex) + " " + address + " in (" + binary + "+" + offset + ")";
            }

            stackLines.push_back(frameStr);
            frameIndex++;
        }
    }

    // 拼接所有栈帧
    std::string result;
    for (size_t i = 0; i < stackLines.size(); ++i)
    {
        if (i > 0)
            result += "\n";
        result += stackLines[i];
    }

    return result;
}