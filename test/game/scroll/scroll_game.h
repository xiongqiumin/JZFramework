#pragma once

#include "JZGameView2D.h"
#include "JZScene.h"
#include "JZSprite.h"
#include "JZPixmap.h"

#include <string>

// ============================================================================
// 横版滚屏示例 (test/game/scroll): JZGameView2D + JZCamera2D 的用法示范
//
//   * 世界 3600x540, 视口 960x540 —— 只有横向滚屏
//   * 三层绘制顺序就是本类的三个钩子:
//       renderBackground  屏幕坐标: 夜空 + 远景山(视差 0.25) + 地砖(factor 1)
//       renderWorld       世界坐标: 金币(剔除绘制) + 角色 + 终点旗
//       renderHud         屏幕坐标: 相机位置/可见数/fps + 进度条
//   * 相机: 跟随角色前方 140px, followRate=8 (平滑), 到世界右边界自动钳住
//   * 剔除: m_coins.drawVisible(p, visibleWorld()) —— 屏幕外的金币根本不进绘制流
//
// 验证 (--auto): 角色一直右跑, 相机最终应被钳在 worldW - viewW/2 = 3120,
// 期间周期性打印 `[scroll] ...` 行, 结束时打印 DONE 并退出。
// ============================================================================
class ScrollGame : public JZGameView2D
{
public:
    static constexpr int kViewW = 960;
    static constexpr int kViewH = 540;

    static constexpr double kWorldW = 3600.0;
    static constexpr double kWorldH = 540.0;
    static constexpr double kGroundY = 420.0;   // 地平线 (地面贴图从这里往下铺)
    static constexpr double kPlayerY = 380.0;   // 角色中心 y
    static constexpr double kCoinCount = 56.0;

    explicit ScrollGame(JZWidget* parent = nullptr);

    // 自动模式: 不需要键盘, 跑到世界尽头后打印结果并退出 (用于无头验证)
    void setAutoMode(bool on) { m_auto = on; }

    // 第 240 步抓一张 PNG (JZWidget::grab, 被遮挡也能抓到自绘内容)。
    // 给"agent 自己看画面"用: 不用人盯窗口也能确认滚屏真的画出来了。
    void setShotPath(const std::string& path) { m_shotPath = path; }

protected:
    void resetGame() override;
    void stepGame() override;
    void renderBackground(JZPainter* p) override;
    void renderWorld(JZPainter* p) override;
    void renderHud(JZPainter* p) override;
    void keyPressEvent(JZKeyEvent* event) override;

private:
    void makeTextures();
    void spawnCoins();
    void drawGround(JZPainter* p, const JZBody& view);
    void report();
    void finish();

    JZScene  m_coins;        // 世界坐标里的金币精灵 (剔除绘制)
    JZPixmap m_texHills;     // 远景山 (256x540, 上半透明)

    double m_playerX = 120.0;
    double m_speed = 6.0;
    int    m_step = 0;
    int    m_nextReport = 0;
    int    m_drawn = 0;      // 本帧实际进入绘制流的金币数
    int    m_total = 0;      // 场上金币总数

    bool   m_auto = false;
    bool   m_done = false;

    std::string m_shotPath;      // 非空 = 中途抓一张图
    bool   m_shotDone = false;
};
