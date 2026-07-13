#include "JZApplication.h"
#include "JZDebug.h"
#include "JZTimer.h"
#include "JZCo.h"

#include "co_test_common.h"    // CO_TEST_ASSERT (失败即 exit(1))

// 各模块单独实现, 暴露独立的 test_coXxx() 入口
int test_coBasic();
int test_coTcp();
int test_coUdp();
int test_coWebSocket();
int test_coHttp();

int main()
{
    JZApplication a;
    jzco_init();

    // 看门狗: 任一测试若挂死(如 accept 永远等不到连接), 超时强制退出避免无限挂起
    JZTimer::singleShot(30000, []() {
        jzDebug() << "[FATAL] 协程综合测试超时(疑似挂死), 强制退出";
        std::exit(1);
    });

    jzDebug() << "########## 协程综合测试: 逐个运行 基础库 / TCP / UDP / WebSocket / HTTP ##########";

    test_coBasic();
    jzDebug() << "【OK】协程基础库 通过";

    test_coTcp();
    jzDebug() << "【OK】协程 TCP 通过";

    test_coUdp();
    jzDebug() << "【OK】协程 UDP 通过";

    test_coWebSocket();
    jzDebug() << "【OK】协程 WebSocket 通过";

    test_coHttp();
    jzDebug() << "【OK】协程 HTTP 通过";

    jzDebug() << "====== 所有协程测试通过 (基础库 + 网络 TCP/UDP/WebSocket/HTTP) ======";
    return 0;
}
