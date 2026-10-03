#pragma once

#include "JZWidget.h"
#include "JZPainter.h"
#include "JZKeyEvent.h"
#include "JZTimer.h"
#include "JZFixedTimer.h"
#include "JZShowEvent.h"

#include <vector>
#include <set>
#include <chrono>
#include <string>

// 飞行射击游戏控件 (V2): 验证 GDI 绘制 + 性能。
//   - 敌人也会向玩家发射子弹 (enemy bullet)
//   - 击毁敌人有概率掉落升级道具: 多发(P) / 环射(R) / 回血(+)
//   - 升级后玩家可并行多发, 环射为整圈扩散
//   - 修复碰撞: 仅在敌人/敌弹真正命中玩家时扣血(带受击无敌帧), 敌人漏底不再扣血
//   - HUD 显示 FPS 与实体数量, 便于观察 GDI 性能
class GameWidget : public JZWidget
{
public:
    explicit GameWidget(JZWidget* parent = nullptr);
    ~GameWidget();

protected:
    void paintEvent(JZPainterEvent* event) override;
    void keyPressEvent(JZKeyEvent* event) override;
    void keyReleaseEvent(JZKeyEvent* event) override;
    void showEvent(JZShowEvent* event) override;

private:
    struct Enemy;           // 前向声明 (enemyFire 用引用参数)
    void onLogic();         // 逻辑 timer: 固定步长推进仿真
    void onLogicError(int missed); // 逻辑 timer 落后过多(补不过来)时回调
    void onRender();        // 渲染 timer: 触发重绘 + 测 FPS
    void updateStars();     // 背景星星滚动(放入逻辑, 避免随渲染帧率变化)
    void reset();           // 重新开始
    void firePlayer();      // 玩家开火 (合并多发 + 环射)
    void spawnEnemy();
    void enemyFire(Enemy& e);
    void hitPlayer();       // 玩家受击 (扣血 + 无敌帧 + 爆炸)
    void updateGame();
    void updateFps();

    // 绘制
    void drawBackground(JZPainter* p);
    void drawShip(JZPainter* p, int cx, int cy, const JZColor& c, bool scalePulse);
    void drawEnemy(JZPainter* p, int cx, int cy, int w, int h, int type);
    void drawBullets(JZPainter* p);
    void drawEnemyBullets(JZPainter* p);
    void drawPowerUps(JZPainter* p);
    void drawExplosions(JZPainter* p);
    void drawHud(JZPainter* p);
    void drawOverlay(JZPainter* p);
    void drawErrorOverlay(JZPainter* p);   // 逻辑落后过多: 停止游戏的错误遮罩

    struct Star    { int x, y, spd, sz; };
    struct Bullet  { int x, y, vx, vy; };          // 玩家子弹 (环射带 vx/vy)
    struct EBullet { int x, y, vx, vy; };           // 敌方子弹
    struct Enemy   { int x, y, w, h, hp, spd, type, fireCd; };
    struct PowerUp { int x, y, vy, type; };         // 0=多发 1=环射 2=回血
    struct Boom    { int x, y, life, max; };

    JZFixedTimer* m_logicTimer = nullptr;   // 逻辑定时器 (固定步长 + 自动补帧)
    JZTimer*      m_renderTimer = nullptr;   // 渲染定时器 (压测 FPS)

    int m_playerX = 0, m_playerY = 0;
    int m_playerCd = 0;      // 开火冷却 (tick)
    int m_spawnCd  = 0;      // 敌人生成冷却 (tick)
    int m_invuln   = 0;      // 受击无敌帧
    int m_score = 0;
    int m_lives = 3;
    int m_multi = 1;         // 并行弹数量 (1..5)
    int m_ring  = 0;         // 环射等级 (0 关闭, 每级 +4 弹)
    bool m_over = false;
    std::string m_errMsg;   // 非空时表示逻辑落后过多, 游戏已停止

    float m_fps = 0;
    float m_peakFps = 0;
    int   m_frameDraws = 0;   // 本帧绘制的图元(绘制请求)数
    int   m_peakDraws = 0;
    std::chrono::steady_clock::time_point m_lastTick;
    // 动画时钟: 用于驱动旋转/缩放等矩阵变换的演示 (与逻辑步长解耦, 渲染期推进)
    double m_animT = 0;
    std::chrono::steady_clock::time_point m_animTick;

    std::vector<Star>    m_stars;
    std::vector<Bullet>  m_bullets;
    std::vector<EBullet> m_ebullets;
    std::vector<Enemy>   m_enemies;
    std::vector<PowerUp> m_pwups;
    std::vector<Boom>    m_booms;

    std::set<uint32_t>   m_keys;   // 当前按下的虚拟键码

    static constexpr int kLogicMs   = 16;   // 逻辑步长 (~60Hz), 仿真速度稳定
    // 通关线: 打到这个分数算通关(一架敌机 +10 ⇒ 30 架)。与 TS 版(test_jsshooter)的
    // kTargetScore 及玩法侧的 game/shooter/config.json 的 targetScore 保持一致。
    static constexpr int kTargetScore = 300;
    static constexpr int kRenderMs  = 0;    // 渲染: 0 = 尽可能快, 用于压测 FPS 上限
    static constexpr int kPlayerSpd = 6;
    static constexpr int kBulletSpd = 11;
    static constexpr int kEBulletSpd= 4;
    static constexpr int kMaxEnemy  = 140;
};
