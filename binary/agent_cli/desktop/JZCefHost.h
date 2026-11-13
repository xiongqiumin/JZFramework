#ifndef JZ_CEF_HOST_H_
#define JZ_CEF_HOST_H_

#include <string>

// ---------------------------------------------------------------------------
// 假的 CEF / 占位壳(2026-11-13)
//
// desktop 不再用 JZWidget 自绘窗口(MainWindow/ChatView/... 整组删除), 改成"一个浏览器壳
// 加载 agent_cli 的本地 HTTP 页面"。但仓库目前没有 CEF / WebView2 集成, 所以这一版只把
// **接口形状**摆好: initialize / createWindow / runMessageLoop / shutdown 全部 no-op,
// 只在 stderr 打一行说明。真浏览器内核接进来时替换本类的实现即可, main.cpp 不用改。
//
// 为什么页面必须走 HTTP: SPA 靠 fetch + EventSource 打 /api/*, 用 file:// 加载会
// origin=null 撞 CORS —— 这一条不因是否接真 CEF 而变。
//
// ⚠ isAvailable() 永远 false: 调用方可据此退化(提示用系统浏览器打开同一个 URL)。
// ---------------------------------------------------------------------------
class JZCefHost
{
public:
    bool initialize(int argc, char **argv);
    bool createWindow(const std::string &url);
    int runMessageLoop();
    void shutdown();

    bool isAvailable() const { return false; }
    const std::string &url() const { return m_url; }

private:
    std::string m_url;
};

#endif // !JZ_CEF_HOST_H_
