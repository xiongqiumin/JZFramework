#pragma once

#include "JZGameView.h"
#include "JZScene.h"
#include "JZSprite.h"
#include "JZBody.h"
#include "JZPixmap.h"
#include "JZColor.h"
#include "JZPainter.h"

#include <chrono>

// ============================================================================
// 弹幕游戏示例 (test/game/danmaku): JZSprite/JZScene/JZBody 的用法示范。
//   * 四个 scene 分层: 敌弹(z=0) / 玩家弹(z=1) / 角色(z=1) / 特效(z=2)
//   * JZSprite 两条外观路径都用到: 光弹/座机/Boss 走纹理 (drawPixmap),
//     瞄准玉走程序化椭圆 (fillEllipse + 描边)
//   * 弹幕 pattern 三连: 环形弹 / 瞄准弹 / 螺旋弹, 各 ~200 tick 后轮换
//   * 碰撞全走 JZBody(AABB): 玩家小判定点 vs 敌弹, 玩家弹 vs Boss
//   * Shift = 精准模式 (减速 + 显示判定点), 空格 = 射击, R = 重开
// ============================================================================
class DanmakuGame : public JZGameView
{
public:
    explicit DanmakuGame(JZWidget* parent = nullptr);

protected:
    void resetGame() override;
    void stepGame() override;
    void renderGame(JZPainter* p) override;
    void keyPressEvent(JZKeyEvent* event) override;
    void keyReleaseEvent(JZKeyEvent* event) override;

private:
    void makeTextures();      // 离屏 (JZPainter 画进 JZImage) 生成四张纹理
    void firePlayer();
    void spawnRing(int count, double speed);
    void spawnAimed(int count, double speed);
    void spawnSpiral();
    void explode(double x, double y, const JZColor& c, int n);
    void hitPlayer();
    void reportState();       // 裁判输出 (与 shooter 同一套风格)

    void drawHud(JZPainter* p);
    void drawOverlay(JZPainter* p);
    void drawErrorOverlay(JZPainter* p);

    JZScene m_enemyBullets;   // 敌弹
    JZScene m_playerShots;    // 玩家弹
    JZScene m_actors;         // 玩家 + Boss (每帧重建: 就两个, 演示"绘制队列"用法)
    JZScene m_fx;             // 爆炸/火花

    JZPixmap m_texPlayer;     // 座机 (48x48, 朝上)
    JZPixmap m_texBoss;       // Boss (128x96)
    JZPixmap m_texGlow;       // 光弹 (22x22, 环弹/螺旋弹共用一张图 = 共享纹理)
    JZPixmap m_texShot;       // 玩家弹 (6x18)

    double m_px = 0, m_my = 0;      // 玩家中心
    double m_bx = 0, m_by = 0;      // Boss 中心
    int m_bossHp = 0, m_bossMaxHp = 0, m_bossLevel = 0;
    int m_score = 0, m_lives = 3;
    int m_invuln = 0;               // 受击无敌帧
    int m_fireCd = 0;
    int m_pattern = 0;              // 0=环形 1=瞄准 2=螺旋
    int m_patTick = 0;              // 当前 pattern 剩余 tick
    int m_subCd = 0;                // pattern 内单批发射冷却
    int m_spiralA = 0;              // 螺旋臂角度
    int m_bossT = 0;                // Boss 走位时钟
    bool m_over = false;
    bool m_focus = false;           // Shift 精准模式

    // 裁判输出状态
    int m_lastReportScore = -1;
    int m_lastReportLives = -1;
    std::chrono::steady_clock::time_point m_lastReportAt;

    static constexpr int kMaxBullets = 900;   // 同屏敌弹上限 (防失控)
};
