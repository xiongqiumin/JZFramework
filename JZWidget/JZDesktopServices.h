#ifndef JZ_DESKTOP_SERVICES_H_
#define JZ_DESKTOP_SERVICES_H_

#include <string>

// 对齐 Qt::QDesktopServices: 用系统默认程序打开 URL / 本地文件 / 目录。
// 典型用法: 启动后自动打开本地网页 (http://127.0.0.1:port)。
class JZDesktopServices
{
public:
    // 用系统关联程序打开 URL (http/https)、本地文件 (file:// 或绝对路径)、目录。
    // 成功返回 true; 失败 (无关联程序 / 路径非法) 返回 false。
    // 注: 仅发起打开请求, 不等待目标程序退出。
    static bool openUrl(const std::string& url);
};

#endif // !JZ_DESKTOP_SERVICES_H_
