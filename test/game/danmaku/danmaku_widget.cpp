#include "danmaku_widget.h"
#include "JZKeyEvent.h"
#include "JZImage.h"
#include "JZFont.h"

#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <algorithm>

// Win32 虚拟键码 (JZKeyEvent::key 即平台 VK_*)
enum {
    VK_LEFT_  = 0x25,
    VK_UP_    = 0x26,
    VK_RIGHT_ = 0x27,
    VK_DOWN_  = 0x28,
    VK_SPACE  = 0x20,
    VK_RETURN = 0x0D,
    VK_SHIFT_ = 0x10,
    VK_A = 0x41, VK_D = 0x44, VK_W = 0x57, VK_S = 0x53,
    VK_R = 0x52,
};

static constexpr double kPi = 3.14159265358979323846;

// ---- 纹理生成: 离屏画进 JZImage, 再转成显示用 JZPixmap (预转 PARGB) ----
// 弹幕里同一张图一帧要贴几百次, 必须走 pixmap 路径 (drawPixmap 零逐像素开销)。
static JZPixmap makeGlowTexture(const JZColor& core, const JZColor& halo, int size)
{
    JZImage img = JZImage::create(size, size);
    {
        JZPainter p(img);
        const int r = size / 2;
        for (int i = 3; i >= 1; --i) {          // 三层递减 alpha 的柔光
            const double t = (double)i / 3.0;   // 1.0 = 最外圈
            const int rr = (int)(r * t);
            JZColor c = halo;
            c.setRgba(c.red(), c.green(), c.blue(), (uint8_t)(30 + 60 * (1.0 - t)));
            p.fillEllipse(r - rr, r - rr, rr * 2, rr * 2, c);
        }
        const int cr = size / 5;                // 亮核
        p.fillEllipse(r - cr, r - cr, cr * 2, cr * 2, core);
    }
    return JZPixmap::fromImage(img);
}

// ============================================================================
// StarBullet: 继承示范 —— 旋转星形玉。
//   * 只重写虚钩子 onDraw 换外观 (save/变换/restore 仍归基类 draw 统一管);
//   * 自带两个玩法字段 (spin/spinSpd): 这就是"继承带派生状态", 也是
//     JZScene 必须持指针而不能按值存 vector<JZSprite> 的原因 (按值会切片)。
// ============================================================================
class StarBullet : public JZSprite
{
public:
    double spin = 0;      // 自转角 (度; 玩法层每逻辑步推进)
    double spinSpd = 6;   // 度/逻辑步

protected:
    void onDraw(JZPainter* p) const override
    {
        // 进入时局部原点已落在锚点处 (本类用中心锚 (0.5,0.5) => 直接以 (0,0) 为中心)
        const double r = width * 0.5;
        const double ri = r * 0.38;
        p->save();
        p->rotate(spin);
        p->setPen(JZPen(color, 2));
        for (int i = 0; i < 4; i++) {
            const double a = i * 90.0 * kPi / 180.0;
            p->drawLine((int)(std::cos(a) * ri), (int)(std::sin(a) * ri),
                        (int)(std::cos(a) * r), (int)(std::sin(a) * r));
        }
        p->fillEllipse((int)(-ri), (int)(-ri), (int)(ri * 2), (int)(ri * 2), color);
        p->restore();
    }
};

DanmakuGame::DanmakuGame(JZWidget* parent)
    : JZGameView(parent)
{
    makeTextures();
    std::srand((unsigned)::time(nullptr));
}

void DanmakuGame::makeTextures()
{
    // 光弹: 青蓝柔光 + 白核 (环形弹共用 => 几百个精灵共享一张 pixmap)
    m_texGlow = makeGlowTexture(JZColor(220, 250, 255), JZColor(60, 140, 255), 22);

    // 玩家弹: 细长白梭
    {
        JZImage img = JZImage::create(6, 18);
        {
            JZPainter p(img);
            p.fillEllipse(0, 0, 6, 18, JZColor(160, 230, 255));
            p.fillEllipse(1, 5, 4, 8, JZColor::White);
        }
        m_texShot = JZPixmap::fromImage(img);
    }

    // 玩家座机 (朝上): 三角机身 + 后翼 + 尾焰 + 座舱
    {
        JZImage img = JZImage::create(48, 48);
        {
            JZPainter p(img);
            p.setPen(JZPen(JZColor(120, 220, 255), 2));
            p.drawLine(24, 4, 12, 40);
            p.drawLine(24, 4, 36, 40);
            p.drawLine(12, 40, 36, 40);
            p.drawLine(12, 40, 4, 30);
            p.drawLine(36, 40, 44, 30);
            p.setPen(JZPen(JZColor::Orange, 2));
            p.drawLine(20, 40, 20, 45);
            p.drawLine(28, 40, 28, 45);
            p.fillEllipse(21, 16, 6, 10, JZColor::Cyan);
        }
        m_texPlayer = JZPixmap::fromImage(img);
    }

    // Boss: 紫红圆角身躯 + 白眼 + 红瞳 + 嘴线
    {
        JZImage img = JZImage::create(128, 96);
        {
            JZPainter p(img);
            p.fillRoundRect(4, 4, 120, 88, 28, JZColor(90, 40, 110));
            p.setPen(JZPen(JZColor(220, 120, 255), 3));
            p.drawRoundRect(4, 4, 120, 88, 28);
            p.fillEllipse(28, 30, 24, 24, JZColor::White);
            p.fillEllipse(76, 30, 24, 24, JZColor::White);
            p.fillEllipse(36, 38, 8, 8, JZColor::Red);
            p.fillEllipse(84, 38, 8, 8, JZColor::Red);
            p.setPen(JZPen(JZColor(255, 180, 60), 2));
            p.drawLine(40, 72, 88, 72);
        }
        m_texBoss = JZPixmap::fromImage(img);
    }
}

// ---------------------------------------------------------------------------
// 钩子: 重新开始
// ---------------------------------------------------------------------------
void DanmakuGame::resetGame()
{
    int w = width(), h = height();
    m_px = w / 2.0;
    m_my = h - 80.0;
    m_bx = w / 2.0;
    m_by = 90.0;
    m_bossLevel = 0;
    m_bossMaxHp = 40;
    m_bossHp = m_bossMaxHp;
    m_score = 0;
    m_lives = 3;
    m_invuln = 0;
    m_fireCd = 0;
    m_pattern = 0;
    m_patTick = 200;
    m_subCd = 0;
    m_spiralA = 0;
    m_bossT = 0;
    m_over = false;
    m_focus = false;
    m_enemyBullets.clear();
    m_playerShots.clear();
    m_fx.clear();
    m_actors.clear();
    m_lastReportScore = -1;
    m_lastReportLives = -1;
}

// ---------------------------------------------------------------------------
// 钩子: 逻辑步 (固定步长; 弹幕密度与帧率解耦)
// ---------------------------------------------------------------------------
void DanmakuGame::stepGame()
{
    int w = width(), h = height();

    // ---- 特效 (game over 也继续放完) ----
    m_fx.forEach([](JZSprite& s) {
        s.x += s.vx;
        s.y += s.vy;
        s.vx *= 0.92;
        s.vy *= 0.92;
        s.scaleX += 0.10;
        s.scaleY += 0.10;
        s.alpha -= 0.055;
    });
    m_fx.removeIf([](const JZSprite& s) { return s.alpha <= 0.02; });

    if (m_over) {
        reportState();
        return;
    }

    // ---- 玩家 ----
    const double spd = m_focus ? 2.4 : 5.5;   // 精准模式减速 (判定点更值钱)
    if (isKeyDown(VK_LEFT_)  || isKeyDown(VK_A)) m_px -= spd;
    if (isKeyDown(VK_RIGHT_) || isKeyDown(VK_D)) m_px += spd;
    if (isKeyDown(VK_UP_)    || isKeyDown(VK_W)) m_my -= spd;
    if (isKeyDown(VK_DOWN_)  || isKeyDown(VK_S)) m_my += spd;
    m_px = std::max(20.0, std::min(w - 20.0, m_px));
    m_my = std::max(30.0, std::min(h - 20.0, m_my));

    if (m_invuln > 0) m_invuln--;
    if (m_fireCd > 0) m_fireCd--;
    if (isKeyDown(VK_SPACE) && m_fireCd == 0) {
        firePlayer();
        m_fireCd = 5;
    }

    // ---- Boss 走位 (横向正弦 + 轻微浮动) ----
    m_bossT++;
    m_bx = w / 2.0 + std::sin(m_bossT * 0.012) * (w * 0.30);
    m_by = 90.0 + std::sin(m_bossT * 0.007) * 24.0;

    // ---- 弹幕 pattern 轮换: 0=环形 1=瞄准 2=螺旋, 每个 ~200 tick ----
    if (m_patTick > 0) m_patTick--;
    if (m_patTick == 0) {
        m_pattern = (m_pattern + 1) % 3;
        m_patTick = 200;
        m_subCd = 0;
        m_spiralA = 0;
    }
    if (m_subCd > 0) m_subCd--;
    if (m_subCd == 0) {
        if (m_pattern == 0) { spawnRing(18, 2.3); m_subCd = 26; }
        else if (m_pattern == 1) { spawnAimed(5, 3.0); m_subCd = 14; }
        else { spawnSpiral(); m_subCd = 2; }
    }

    // ---- 敌弹移动 + 出界清理 (vx/vy 由玩法层在生成时给, 引擎只负责存) ----
    // 派生状态 (StarBullet::spin) 也在这里推进: dynamic_cast 是"继承 + 系统遍历"
    // 的常见代价; 若它成为热点, 就把 spin 上移为基类字段或改组件化。
    m_enemyBullets.forEach([&](JZSprite& s) {
        s.x += s.vx;
        s.y += s.vy;
        if (StarBullet* st = dynamic_cast<StarBullet*>(&s))
            st->spin += st->spinSpd;
    });
    m_enemyBullets.removeIf([&](const JZSprite& s) {
        return s.alpha <= 0.0 || s.x < -40 || s.x > w + 40 || s.y < -40 || s.y > h + 40;
    });

    // ---- 玩家弹移动 ----
    m_playerShots.forEach([](JZSprite& s) { s.y += s.vy; });
    m_playerShots.removeIf([](const JZSprite& s) { return s.alpha <= 0.0 || s.y < -30; });

    // ---- 碰撞: 玩家弹 vs Boss (AABB) ----
    JZBody bossBody = JZBody::fromCenter(m_bx, m_by, 110, 80);
    m_playerShots.forEachOverlap(bossBody, [&](JZSprite& s) {
        s.alpha = 0;                       // 标记删除 (循环结束后统一清)
        m_bossHp--;
        explode(s.x, s.y, JZColor(255, 200, 120), 2);
    });
    m_playerShots.removeIf([](const JZSprite& s) { return s.alpha <= 0.0; });
    if (m_bossHp <= 0) {
        m_score += 500;
        explode(m_bx, m_by, JZColor(255, 120, 220), 18);
        m_bossLevel++;
        m_bossMaxHp = 40 + m_bossLevel * 25;
        m_bossHp = m_bossMaxHp;
        std::printf("[danmaku] BOSS DOWN level=%d score=%d\n", m_bossLevel, m_score);
        std::fflush(stdout);
    }

    // ---- 碰撞: 敌弹 vs 玩家 (小判定点; 无敌帧内只清弹不扣血) ----
    JZBody playerBody = JZBody::fromCenter(m_px, m_my, 7, 7);
    if (m_invuln == 0) {
        m_enemyBullets.forEachOverlap(playerBody, [&](JZSprite& s) {
            s.alpha = 0;
            if (m_invuln == 0)              // 一批弹同时命中只算一次
                hitPlayer();
        });
        m_enemyBullets.removeIf([](const JZSprite& s) { return s.alpha <= 0.0; });
    }

    reportState();
}

// ---------------------------------------------------------------------------
// 发射 / 弹幕 pattern
// ---------------------------------------------------------------------------
void DanmakuGame::firePlayer()
{
    auto& s = m_playerShots.create<JZSprite>();
    s.texture = m_texShot;
    s.width = 6;
    s.height = 18;
    s.anchorX = 0.5;
    s.anchorY = 0.5;
    s.z = 1;
    s.x = m_px - 10;
    s.y = m_my - 16;
    s.vy = -13;

    auto& s2 = m_playerShots.create<JZSprite>();
    s2 = s;                                 // 同一份配置, 只改 x
    s2.x = m_px + 10;
}

// 环形弹: 从 Boss 中心整圈扩散 (纹理玉)
void DanmakuGame::spawnRing(int count, double speed)
{
    if ((int)m_enemyBullets.size() >= kMaxBullets)
        return;
    const double base = (double)(std::rand() % 3600) / 10.0;
    for (int i = 0; i < count; i++) {
        const double a = (base + 360.0 * i / count) * kPi / 180.0;
        auto& s = m_enemyBullets.create<JZSprite>();
        s.texture = m_texGlow;
        s.width = 22;
        s.height = 22;
        s.anchorX = 0.5;
        s.anchorY = 0.5;
        s.z = 0;
        s.x = m_bx + std::cos(a) * 30.0;
        s.y = m_by + std::sin(a) * 30.0;
        s.vx = std::cos(a) * speed;
        s.vy = std::sin(a) * speed;
    }
}

// 瞄准弹: 朝玩家方向的扇形 (程序化椭圆玉 + 描边)
void DanmakuGame::spawnAimed(int count, double speed)
{
    if ((int)m_enemyBullets.size() >= kMaxBullets)
        return;
    const double base = std::atan2(m_my - m_by, m_px - m_bx);
    for (int i = 0; i < count; i++) {
        const double a = base + (i - (count - 1) / 2.0) * 0.13;
        auto& s = m_enemyBullets.create<JZSprite>();
        s.shape = JZSprite::Ellipse;
        s.width = 14;
        s.height = 14;
        s.anchorX = 0.5;
        s.anchorY = 0.5;
        s.color = JZColor(255, 90, 180);
        s.borderColor = JZColor(255, 210, 245);
        s.borderWidth = 1;
        s.z = 0;
        s.x = m_bx;
        s.y = m_by;
        s.vx = std::cos(a) * speed;
        s.vy = std::sin(a) * speed;
    }
}

// 螺旋弹: 双臂匀速旋转 (派生类 StarBullet: 自转星形玉)
void DanmakuGame::spawnSpiral()
{
    if ((int)m_enemyBullets.size() >= kMaxBullets)
        return;
    for (int k = 0; k < 2; k++) {
        const double a = (m_spiralA + k * 180) * kPi / 180.0;
        auto& s = m_enemyBullets.create<StarBullet>();
        s.shape = JZSprite::Ellipse;
        s.width = 18;
        s.height = 18;
        s.anchorX = 0.5;
        s.anchorY = 0.5;
        s.color = JZColor(140, 220, 255);
        s.borderColor = JZColor::White;
        s.borderWidth = 1;
        s.z = 0;
        s.x = m_bx + std::cos(a) * 26.0;
        s.y = m_by + std::sin(a) * 26.0;
        s.vx = std::cos(a) * 2.6;
        s.vy = std::sin(a) * 2.6;
        s.spin = (double)m_spiralA;
        s.spinSpd = 7;
    }
    m_spiralA += 23;
    if (m_spiralA >= 360) m_spiralA -= 360;
}

// 爆炸火花: 程序化椭圆, 扩散 + 淡出 (演示 alpha/scale 字段)
void DanmakuGame::explode(double x, double y, const JZColor& c, int n)
{
    for (int i = 0; i < n; i++) {
        const double a = 2.0 * kPi * std::rand() / RAND_MAX;
        const double v = 0.8 + (std::rand() % 30) / 10.0;
        auto& s = m_fx.create<JZSprite>();
        s.shape = JZSprite::Ellipse;
        s.x = x;
        s.y = y;
        s.width = 6 + std::rand() % 6;
        s.height = s.width;
        s.color = c;
        s.borderColor = JZColor::White;
        s.borderWidth = 1;
        s.alpha = 0.9;
        s.z = 2;
        s.vx = std::cos(a) * v;
        s.vy = std::sin(a) * v;
    }
}

void DanmakuGame::hitPlayer()
{
    m_lives--;
    m_invuln = 120;   // ~2s 无敌
    explode(m_px, m_my, JZColor(120, 220, 255), 12);
    if (m_lives <= 0) {
        m_lives = 0;
        m_over = true;
    }
}

// ---------------------------------------------------------------------------
// 键盘: 基类登记按住集合; 这里加 Shift 精准模式与 R 重开
// ---------------------------------------------------------------------------
void DanmakuGame::keyPressEvent(JZKeyEvent* event)
{
    JZGameView::keyPressEvent(event);
    const uint32_t k = event->key();
    if (k == VK_SHIFT_)
        m_focus = true;
    if (k == VK_R || k == VK_RETURN) {
        if (m_over || !logicActive()) {
            resetGame();
            restartLogic();
        }
    }
}

void DanmakuGame::keyReleaseEvent(JZKeyEvent* event)
{
    JZGameView::keyReleaseEvent(event);
    if (event->key() == VK_SHIFT_)
        m_focus = false;
}

// ---------------------------------------------------------------------------
// 裁判输出 (与 shooter 同一套风格: 分数/命数一变一行)
// ---------------------------------------------------------------------------
void DanmakuGame::reportState()
{
    auto now = std::chrono::steady_clock::now();
    if (m_score != m_lastReportScore || m_lives != m_lastReportLives) {
        std::printf("[danmaku] score=%d lives=%d\n", m_score, m_lives);
        m_lastReportScore = m_score;
        m_lastReportLives = m_lives;
        m_lastReportAt = now;
    } else if (std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastReportAt).count() >= 2000) {
        std::printf("[danmaku] alive score=%d lives=%d over=%d\n", m_score, m_lives, m_over ? 1 : 0);
        m_lastReportAt = now;
    }
    if (m_over) {
        std::printf("[danmaku] GAME OVER score=%d\n", m_score);
    }
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// 钩子: 渲染 (基类建好 painter 后调这里)
// ---------------------------------------------------------------------------
void DanmakuGame::renderGame(JZPainter* p)
{
    const int w = width(), h = height();
    p->fillRect(0, 0, w, h, JZColor(8, 10, 24));

    // 简易星野 (纯装饰, 位置随手撒)
    for (int i = 0; i < 60; i++) {
        const int sx = (i * 137 + 29) % w;
        const int sy = (i * 89 + 13) % h;
        p->fillRect(sx, sy, 1, 1, JZColor(70, 80, 120));
    }

    // 四层 scene 依次画 (z 在各自 scene 内排)
    m_enemyBullets.draw(p);
    m_playerShots.draw(p);

    // actors 每帧重建 (绘制队列用法): Boss + 玩家
    m_actors.clear();
    {
        auto& boss = m_actors.create<JZSprite>();
        boss.texture = m_texBoss;
        boss.width = 128;
        boss.height = 96;
        boss.anchorX = 0.5;
        boss.anchorY = 0.5;
        boss.x = m_bx;
        boss.y = m_by;
        boss.z = 1;
    }
    if (!m_over) {
        auto& pl = m_actors.create<JZSprite>();
        pl.texture = m_texPlayer;
        pl.width = 48;
        pl.height = 48;
        pl.anchorX = 0.5;
        pl.anchorY = 0.5;
        pl.x = m_px;
        pl.y = m_my;
        pl.z = 1;
        // 受击无敌时隐藏式闪烁 (整帧隐藏)
        pl.visible = !(m_invuln > 0 && ((m_invuln / 5) % 2 == 0));
    }
    m_actors.draw(p);

    m_fx.draw(p);

    // 判定点 (精准模式下显示: 弹幕游戏的"真正的你")
    if (m_focus && !m_over) {
        p->fillEllipse((int)m_px - 3, (int)m_my - 3, 6, 6, JZColor::Cyan);
        p->setPen(JZPen(JZColor(255, 255, 255, 160), 1));
        p->drawEllipse((int)m_px - 7, (int)m_my - 7, 14, 14);
    }

    drawHud(p);
    if (m_over) drawOverlay(p);
    else if (!errorMessage().empty()) drawErrorOverlay(p);
}

void DanmakuGame::drawHud(JZPainter* p)
{
    char buf[128];

    std::snprintf(buf, sizeof(buf), "SCORE:%d  LIVES:%d  BOSS LV:%d",
                  m_score, m_lives, m_bossLevel);
    p->setPen(JZPen(JZColor::White));
    p->drawText(12, 14, buf);

    // Boss 血条 (顶部居中)
    const int bw = 240, bh = 10;
    const int bx = (width() - bw) / 2, by = 12;
    p->fillRect(bx, by, bw, bh, JZColor(40, 20, 50));
    const int fw = (int)((double)bw * m_bossHp / (m_bossMaxHp > 0 ? m_bossMaxHp : 1));
    p->fillRect(bx, by, fw, bh, JZColor(230, 80, 200));
    p->setPen(JZPen(JZColor(220, 120, 255), 1));
    p->drawRect(bx, by, bw, bh);

    // 性能/规模: FPS + 敌弹数 (弹幕密度一眼可见)
    std::snprintf(buf, sizeof(buf), "FPS:%.0f  BULLETS:%d  SHOTS:%d  FX:%d",
                  fps(), (int)m_enemyBullets.size(),
                  (int)m_playerShots.size(), (int)m_fx.size());
    p->setPen(JZPen(JZColor(150, 200, 150)));
    p->drawText(12, 36, buf);

    p->setPen(JZPen(JZColor(150, 150, 170)));
    p->drawText(12, height() - 24,
        "Arrows/WASD move  Space fire  Shift focus(slow+hitbox)  R restart");
}

void DanmakuGame::drawOverlay(JZPainter* p)
{
    const int w = width(), h = height();
    p->fillRect(0, 0, w, h, JZColor(0, 0, 0, 150));
    p->setPen(JZPen(JZColor::Red, 3));
    p->setFont(JZFont(48));
    p->drawText(w / 2 - 150, h / 2 - 30, "GAME OVER");
    p->setPen(JZPen(JZColor::White));
    p->setFont(JZFont(20));
    p->drawText(w / 2 - 110, h / 2 + 20, ("score: " + std::to_string(m_score)).c_str());
    p->drawText(w / 2 - 110, h / 2 + 50, "press R / Enter to restart");
}

void DanmakuGame::drawErrorOverlay(JZPainter* p)
{
    const int w = width(), h = height();
    p->fillRect(0, 0, w, h, JZColor(0, 0, 0, 170));
    p->setPen(JZPen(JZColor::Red, 3));
    p->setFont(JZFont(40));
    p->drawText(w / 2 - 150, h / 2 - 40, "GAME STOPPED");
    p->setPen(JZPen(JZColor(255, 180, 180)));
    p->setFont(JZFont(18));
    p->drawText(w / 2 - 180, h / 2 + 10, errorMessage().c_str());
    p->setPen(JZPen(JZColor::White));
    p->setFont(JZFont(18));
    p->drawText(w / 2 - 130, h / 2 + 50, "press R / Enter to restart");
}
