#pragma once

#include "JZGameView.h"
#include "JZColor.h"
#include "JZPainter.h"

#include <chrono>
#include <string>
#include <vector>

// 飞行射击示例 (test/game/shooter): JZGameView 的用法示范。
//   - 基类管"两个 timer"(逻辑固定步长 + 渲染)与键盘登记, 本类只写玩法钩子:
//     resetGame / stepGame / renderGame, 以及 R 重开
//   - 敌人会朝玩家发射子弹; 击毁敌机概率掉落 多发(P) / 环射(R) / 回血(+) 道具
//   - 受击无敌帧; 敌人漏底不扣血; HUD 显示 FPS / 每帧绘制请求数 / 实体数
//   - 与 test/widget/shooter (自己直写两个 timer 的版本) 是同一套玩法, 对照看基类省了什么
class ShooterGame : public JZGameView
{
public:
    explicit ShooterGame(JZWidget* parent = nullptr);

protected:
    void resetGame() override;
    void stepGame() override;
    void renderGame(JZPainter* p) override;
    void keyPressEvent(JZKeyEvent* event) override;

private:
    struct Enemy;   // 前向声明 (enemyFire 用引用参数)
    void firePlayer();
    void spawnEnemy();
    void enemyFire(Enemy& e);
    void hitPlayer();
    void updateGame();
    void reportState();   // 裁判输出 (与 TS 版 test_jsshooter 同一套行格式, 见 .cpp)

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
    void drawErrorOverlay(JZPainter* p);

    struct Star    { int x, y, spd, sz; };
    struct Bullet  { int x, y, vx, vy; };    // 玩家子弹 (环射带 vx/vy)
    struct EBullet { int x, y, vx, vy; };    // 敌方子弹
    struct Enemy   { int x, y, w, h, hp, spd, type, fireCd; };
    struct PowerUp { int x, y, vy, type; };  // 0=多发 1=环射 2=回血
    struct Boom    { int x, y, life, max; };

    int m_playerX = 0, m_playerY = 0;
    int m_playerCd = 0;      // 开火冷却 (逻辑步)
    int m_spawnCd  = 0;      // 敌人生成冷却 (逻辑步)
    int m_invuln   = 0;      // 受击无敌帧
    int m_score = 0;
    int m_lives = 3;
    int m_multi = 1;         // 并行弹数量 (1..5)
    int m_ring  = 0;         // 环射等级 (0 关闭, 每级 +4 弹)
    bool m_over = false;

    // 裁判输出(reportState)的状态: 分数一变就打一行 + 分数不变时的心跳节流 + 两个"只打一次"
    int m_lastReportScore = -1;      // 上一次打印时的分数(-1 = 还没打过)
    std::chrono::steady_clock::time_point m_lastReportAt;
    bool m_gameOverReported = false;
    bool m_clearReported = false;

    int m_frameDraws = 0;   // 本帧绘制的图元(绘制请求)数
    int m_peakDraws = 0;

    std::vector<Star>    m_stars;
    std::vector<Bullet>  m_bullets;
    std::vector<EBullet> m_ebullets;
    std::vector<Enemy>   m_enemies;
    std::vector<PowerUp> m_pwups;
    std::vector<Boom>    m_booms;

    // 通关线: 打到这个分数算通关(一架敌机 +10 => 30 架)。与 TS 版(test_jsshooter)的
    // kTargetScore 及玩法侧 game/shooter/config.json 的 targetScore 保持一致。
    static constexpr int kTargetScore = 300;
    static constexpr int kPlayerSpd = 6;
    static constexpr int kBulletSpd = 11;
    static constexpr int kEBulletSpd = 4;
    static constexpr int kMaxEnemy  = 140;
};
