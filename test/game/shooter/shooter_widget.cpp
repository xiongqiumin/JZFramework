#include "shooter_widget.h"
#include "JZKeyEvent.h"
#include "JZRect.h"

#include <cstdlib>
#include <ctime>
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
    VK_A = 0x41, VK_D = 0x44, VK_W = 0x57, VK_S = 0x53,
    VK_R = 0x52,
};

static constexpr float kPi = 3.14159265f;

ShooterGame::ShooterGame(JZWidget* parent)
    : JZGameView(parent)
{
    std::srand((unsigned)::time(nullptr));
}

// ---------------------------------------------------------------------------
// 钩子: 重新开始 (show 时基类调一次; 按 R 也从这里进)
// ---------------------------------------------------------------------------
void ShooterGame::resetGame()
{
    int w = width(), h = height();
    m_playerX = w / 2;
    m_playerY = h - 60;
    m_playerCd = 0;
    m_spawnCd = 0;
    m_invuln = 0;
    m_score = 0;
    m_lives = 3;
    m_multi = 1;
    m_ring  = 0;
    m_over = false;
    m_bullets.clear();
    m_ebullets.clear();
    m_enemies.clear();
    m_pwups.clear();
    m_booms.clear();

    // 裁判输出的状态也一并复位 (R 重开后要能重新报 CLEAR / GAME OVER)
    m_lastReportScore = -1;
    m_gameOverReported = false;
    m_clearReported = false;

    // 星星背景
    m_stars.clear();
    int n = (w * h) / 4000;
    for (int i = 0; i < n; i++)
        m_stars.push_back({ std::rand() % w, std::rand() % h,
                           1 + std::rand() % 2, 1 + std::rand() % 2 });
}

// ---------------------------------------------------------------------------
// 钩子: 逻辑步 (基类的 JZFixedTimer 以固定步长驱动, 落后时连续补帧)
// ---------------------------------------------------------------------------
void ShooterGame::stepGame()
{
    // 背景星星滚动 (放逻辑里, 帧率无关)
    int w = width(), h = height();
    for (auto& s : m_stars) {
        s.y += s.spd;
        if (s.y > h) { s.y = 0; s.x = std::rand() % w; }
    }

    if (!m_over)
        updateGame();

    // 爆炸特效即便 game over 也继续播放完
    for (auto& b : m_booms) b.life--;
    m_booms.erase(std::remove_if(m_booms.begin(), m_booms.end(),
        [](const Boom& b) { return b.life <= 0; }), m_booms.end());

    reportState();
}

// ---------------------------------------------------------------------------
// 裁判输出
// ---------------------------------------------------------------------------
// 与 TS 版(test_jsshooter 的 reportState)同一套行格式, **只有 stdout 这一个出口**:
//   [shooter] score=.. lives=..                  分数一变就打一行(尺子据此取"最高分")
//   [shooter] alive score=.. lives=.. over=0|1   分数不变时每 2s 一行心跳
//   [shooter] CLEAR score=.. target=300          首次到线打一行
//   [shooter] GAME OVER score=..                 三条命打光时一次
//   [hit] 敌弹/撞机 ...                          每次挨打一行(带我的 x/y 与那颗弹/那架机的速度)
void ShooterGame::reportState()
{
    auto now = std::chrono::steady_clock::now();
    if (m_score != m_lastReportScore) {
        std::printf("[shooter] score=%d lives=%d\n", m_score, m_lives);
        m_lastReportScore = m_score;
        m_lastReportAt = now;
    } else if (std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastReportAt).count() >= 2000) {
        std::printf("[shooter] alive score=%d lives=%d over=%d\n", m_score, m_lives, m_over ? 1 : 0);
        m_lastReportAt = now;
    }
    if (!m_clearReported && m_score >= kTargetScore) {
        m_clearReported = true;
        std::printf("[shooter] CLEAR score=%d target=%d\n", m_score, kTargetScore);
    }
    if (m_over && !m_gameOverReported) {
        m_gameOverReported = true;
        std::printf("[shooter] GAME OVER score=%d\n", m_score);
    }
    // 刷新: 这个 exe 的 stdout 可能是管道(被脚本读), 不刷就攒在缓冲区里
    std::fflush(stdout);
}

void ShooterGame::updateGame()
{
    int w = width(), h = height();
    if (m_invuln > 0) m_invuln--;

    // 玩家移动 (方向键 / WASD)
    if (isKeyDown(VK_LEFT_)  || isKeyDown(VK_A)) m_playerX -= kPlayerSpd;
    if (isKeyDown(VK_RIGHT_) || isKeyDown(VK_D)) m_playerX += kPlayerSpd;
    if (isKeyDown(VK_UP_)    || isKeyDown(VK_W)) m_playerY -= kPlayerSpd;
    if (isKeyDown(VK_DOWN_)  || isKeyDown(VK_S)) m_playerY += kPlayerSpd;
    m_playerX = std::max(16, std::min(w - 16, m_playerX));
    m_playerY = std::max(40, std::min(h - 20, m_playerY));

    // 开火
    if (m_playerCd > 0) m_playerCd--;
    if (isKeyDown(VK_SPACE) && m_playerCd == 0) {
        firePlayer();
        m_playerCd = 8;
    }

    // 玩家子弹移动
    for (auto& b : m_bullets) { b.x += b.vx; b.y += b.vy; }
    m_bullets.erase(std::remove_if(m_bullets.begin(), m_bullets.end(),
        [w, h](const Bullet& b) { return b.x < -20 || b.x > w + 20 || b.y < -20 || b.y > h + 20; }),
        m_bullets.end());
    if (m_bullets.size() > 600) m_bullets.erase(m_bullets.begin(), m_bullets.end() - 600);

    // 敌方子弹移动
    for (auto& b : m_ebullets) { b.x += b.vx; b.y += b.vy; }
    m_ebullets.erase(std::remove_if(m_ebullets.begin(), m_ebullets.end(),
        [w, h](const EBullet& b) { return b.x < -20 || b.x > w + 20 || b.y < -20 || b.y > h + 20; }),
        m_ebullets.end());

    // 生成敌人 (随分数加快, 上限封顶)
    if (m_spawnCd > 0) m_spawnCd--;
    int spawnGap = std::max(10, 46 - m_score / 40);
    if (m_spawnCd == 0 && (int)m_enemies.size() < kMaxEnemy) {
        spawnEnemy();
        m_spawnCd = spawnGap;
    }

    // 敌人下移 + 开火
    int eSpdBase = 2 + m_score / 160;
    for (auto& e : m_enemies) {
        e.y += e.spd + eSpdBase;
        if (e.fireCd > 0) e.fireCd--;
        if (e.fireCd == 0) { enemyFire(e); e.fireCd = 70 + std::rand() % 90; }
    }

    // 掉落道具下移
    for (auto& p : m_pwups) p.y += p.vy;
    m_pwups.erase(std::remove_if(m_pwups.begin(), m_pwups.end(),
        [h](const PowerUp& p) { return p.y > h + 20; }), m_pwups.end());

    // 玩家子弹 vs 敌人
    for (auto it = m_enemies.begin(); it != m_enemies.end();) {
        bool dead = false;
        for (auto bt = m_bullets.begin(); bt != m_bullets.end();) {
            if (std::abs(bt->x - it->x) < it->w / 2 + 3 &&
                std::abs(bt->y - it->y) < it->h / 2 + 6) {
                it->hp--;
                bt = m_bullets.erase(bt);
                if (it->hp <= 0) {
                    m_score += 10;
                    m_booms.push_back({ it->x, it->y, 18, 18 });
                    // 掉落道具 (约 22%)
                    if (std::rand() % 100 < 22) {
                        int t = std::rand() % 100;
                        int type = t < 40 ? 0 : (t < 75 ? 1 : 2);
                        m_pwups.push_back({ it->x, it->y, 2, type });
                    }
                    dead = true;
                }
                break;
            } else {
                ++bt;
            }
        }
        if (dead) it = m_enemies.erase(it);
        else      ++it;
    }

    // 敌方子弹 vs 玩家 (受击无敌时不计)
    for (auto it = m_ebullets.begin(); it != m_ebullets.end();) {
        if (m_invuln == 0 && std::abs(it->x - m_playerX) < 16 &&
            std::abs(it->y - m_playerY) < 18) {
            std::printf("[hit] enemy bullet x=%d y=%d vx=%d vy=%d meX=%d meY=%d\n",
                        it->x, it->y, it->vx, it->vy, m_playerX, m_playerY);
            hitPlayer();
            it = m_ebullets.erase(it);
        } else {
            ++it;
        }
    }

    // 敌人 vs 玩家 / 漏底
    for (auto it = m_enemies.begin(); it != m_enemies.end();) {
        bool gone = false;
        if (m_invuln == 0 &&
            std::abs(it->x - m_playerX) < it->w / 2 + 14 &&
            std::abs(it->y - m_playerY) < it->h / 2 + 14) {   // 撞机: 扣血
            std::printf("[hit] crash x=%d y=%d w=%d h=%d spd=%d meX=%d meY=%d\n",
                        it->x, it->y, it->w, it->h, it->spd + eSpdBase, m_playerX, m_playerY);
            hitPlayer();
            gone = true;
        } else if (it->y - it->h / 2 > h) {                   // 漏底: 仅移除, 不扣血
            gone = true;
        }
        if (gone) it = m_enemies.erase(it);
        else      ++it;
    }

    // 道具拾取
    for (auto it = m_pwups.begin(); it != m_pwups.end();) {
        if (std::abs(it->x - m_playerX) < 18 && std::abs(it->y - m_playerY) < 20) {
            if (it->type == 0)      m_multi = std::min(5, m_multi + 1);
            else if (it->type == 1) m_ring  = std::min(4, m_ring + 1);
            else                    m_lives = std::min(5, m_lives + 1);
            m_booms.push_back({ it->x, it->y, 12, 12 });
            it = m_pwups.erase(it);
        } else {
            ++it;
        }
    }
}

void ShooterGame::firePlayer()
{
    int spd = kBulletSpd;
    // 并行多发 (水平展开)
    int n = m_multi;
    for (int i = 0; i < n; i++) {
        int off = (int)((i - (n - 1) / 2.0f) * 11);
        m_bullets.push_back({ m_playerX + off, m_playerY - 18, 0, -spd });
    }
    // 环射: 整圈扩散, 弹数随等级增加
    if (m_ring > 0) {
        int cnt = std::min(24, 8 + m_ring * 4);
        for (int i = 0; i < cnt; i++) {
            float a = 2.0f * kPi * i / cnt;
            m_bullets.push_back({ m_playerX, m_playerY,
                                  (int)(std::cos(a) * spd), (int)(std::sin(a) * spd) });
        }
    }
}

void ShooterGame::spawnEnemy()
{
    int w = width();
    int type = std::rand() % 100 < 30 ? 1 : 0;   // 30% 大块头
    int ew = type ? 46 : 30;
    int eh = type ? 40 : 26;
    m_enemies.push_back({ 20 + std::rand() % (w - 40), -eh,
                          ew, eh, type ? 3 : 1, type ? 0 : 1, type,
                          60 + std::rand() % 80 });
}

void ShooterGame::enemyFire(Enemy& e)
{
    // 朝玩家方向发射
    int dx = m_playerX - e.x;
    int dy = m_playerY - e.y;
    float len = std::sqrt((float)(dx * dx + dy * dy)) + 1e-3f;
    m_ebullets.push_back({ e.x, e.y + e.h / 2,
                           (int)(dx / len * kEBulletSpd),
                           (int)(dy / len * kEBulletSpd) });
}

void ShooterGame::hitPlayer()
{
    m_lives--;
    m_invuln = 90;   // ~2.5s 无敌
    m_booms.push_back({ m_playerX, m_playerY, 26, 26 });
    if (m_lives <= 0) { m_lives = 0; m_over = true; }
}

// ---------------------------------------------------------------------------
// 键盘: 基类已登记"按住"集合, 这里只加 R/回车 重开
// ---------------------------------------------------------------------------
void ShooterGame::keyPressEvent(JZKeyEvent* event)
{
    JZGameView::keyPressEvent(event);   // 基类维护 keysDown()
    uint32_t k = event->key();
    if (k == VK_R || k == VK_RETURN) {
        // game over / 逻辑落后停止后: 重开一局
        if (m_over || !logicActive()) {
            resetGame();
            restartLogic();
        }
    }
}

// ---------------------------------------------------------------------------
// 钩子: 渲染 (基类 paintEvent 里建好 painter 后调这里)
// ---------------------------------------------------------------------------
void ShooterGame::renderGame(JZPainter* p)
{
    JZFont font(16);
    p->setFont(font);

    m_frameDraws = 0;

    drawBackground(p);
    drawExplosions(p);

    // 敌人
    for (auto& e : m_enemies)
        drawEnemy(p, e.x, e.y, e.w, e.h, e.type);

    drawBullets(p);
    drawEnemyBullets(p);
    drawPowerUps(p);

    // 玩家 (受击无敌时: 隐藏式闪烁 —— 整帧隐藏; 可见帧额外叠加缩放脉冲, 验证矩阵变换)
    bool blink = m_invuln > 0 && ((m_invuln / 5) % 2 == 0);
    if (!m_over && !blink)
        drawShip(p, m_playerX, m_playerY, JZColor(120, 220, 255), m_invuln > 0);

    if (m_frameDraws > m_peakDraws) m_peakDraws = m_frameDraws;

    drawHud(p);
    if (m_over) drawOverlay(p);
    else if (!errorMessage().empty()) drawErrorOverlay(p);

    if (m_frameDraws > m_peakDraws) m_peakDraws = m_frameDraws;
}

void ShooterGame::drawBackground(JZPainter* p)
{
    int w = width(), h = height();
    p->fillRect(0, 0, w, h, JZColor(10, 12, 30));

    // 星星向下滚动 (位置由 stepGame() 在逻辑步内更新, 此处只读)
    for (auto& s : m_stars) {
        p->fillRect(s.x, s.y, s.sz, s.sz, JZColor(190, 200, 255));
    }

    // 地面线
    p->setPen(JZPen(JZColor(40, 60, 90), 1));
    p->drawLine(0, h - 8, w, h - 8);

    m_frameDraws += 2 + (int)m_stars.size();   // 背景 + 星星 + 地面线
}

void ShooterGame::drawShip(JZPainter* p, int cx, int cy, const JZColor& c, bool scalePulse)
{
    m_frameDraws += 8;   // 7 条线 + 1 座舱 (save/restore 不计入)
    p->save();
    p->translate(cx, cy);              // 把局部原点移到飞船中心
    p->rotate(animTime() * 50.0);      // 持续旋转: 验证矩阵 rotate (屏幕顺时针为正)
    if (scalePulse) {                  // 受击期间可见帧: 额外缩放脉冲 (变换大小)
        double s = 1.0 + 0.6 * std::sin(animTime() * 28.0);
        p->scale(s, s);
    }
    // 以下几何以 (0,0) 为中心绘制
    p->setPen(JZPen(c, 2));
    p->drawLine(-15, 14, 0, -18);      // 机身 (三角)
    p->drawLine(0, -18, 15, 14);
    p->drawLine(15, 14, -15, 14);
    p->drawLine(-15, 14, -24, 4);      // 机翼
    p->drawLine(15, 14, 24, 4);
    int flame = std::rand() % 8 + 4;
    p->setPen(JZPen(JZColor::Orange, 2));
    p->drawLine(-5, 14, -5, 14 + flame);   // 尾焰
    p->drawLine(5, 14, 5, 14 + flame);
    p->setBrush(JZBrush(JZColor::Cyan));
    p->fillRect(-3, -8, 6, 9, JZColor::Cyan);  // 座舱
    p->restore();
}

void ShooterGame::drawEnemy(JZPainter* p, int cx, int cy, int w, int h, int type)
{
    m_frameDraws += 4;   // 机身填充 + 描边 + 两只眼 (save/restore 不计入)
    // 每敌相位略错开: 旋转 + 轻微缩放脉冲, 验证多实体下的矩阵变换
    double ang = animTime() * 25.0 + cx * 0.05;
    double sc  = 1.0 + 0.12 * std::sin(animTime() * 4.0 + cy * 0.03);
    p->save();
    p->translate(cx, cy);
    p->rotate(ang);
    p->scale(sc, sc);
    JZColor body = type ? JZColor(230, 90, 90) : JZColor(220, 70, 160);
    p->setBrush(JZBrush(body));
    p->fillRect(-w / 2, -h / 2, w, h, body);
    p->setPen(JZPen(JZColor(255, 220, 220), 1));
    p->drawRect(-w / 2, -h / 2, w, h);
    // 眼 (以中心为基准)
    p->setBrush(JZBrush(JZColor::White));
    p->fillRect(-w / 4 - 3, -4, 6, 6, JZColor::White);
    p->fillRect(w / 4 - 3, -4, 6, 6, JZColor::White);
    p->restore();
}

void ShooterGame::drawBullets(JZPainter* p)
{
    m_frameDraws += (int)m_bullets.size();
    p->setBrush(JZBrush(JZColor::Yellow));
    for (auto& b : m_bullets) {
        // 子弹朝向运动方向旋转 (环射时呈放射状), 验证大量实体的矩阵变换
        double ang = std::atan2((double)b.vy, (double)b.vx) * 180.0 / kPi + 90.0;
        p->save();
        p->translate(b.x, b.y);
        p->rotate(ang);
        p->fillRect(-2, -6, 4, 11, JZColor::Yellow);
        p->restore();
    }
}

void ShooterGame::drawEnemyBullets(JZPainter* p)
{
    m_frameDraws += (int)m_ebullets.size();
    p->setBrush(JZBrush(JZColor(255, 120, 60)));
    for (auto& b : m_ebullets) {
        double ang = std::atan2((double)b.vy, (double)b.vx) * 180.0 / kPi + 90.0;
        p->save();
        p->translate(b.x, b.y);
        p->rotate(ang);
        p->fillRect(-3, -3, 6, 6, JZColor(255, 120, 60));
        p->restore();
    }
}

void ShooterGame::drawPowerUps(JZPainter* p)
{
    m_frameDraws += 3 * (int)m_pwups.size();   // 填充 + 描边 + 文字
    for (auto& u : m_pwups) {
        JZColor c = u.type == 0 ? JZColor(80, 160, 255)
                  : u.type == 1 ? JZColor(120, 230, 120)
                                : JZColor(255, 220, 90);
        p->setBrush(JZBrush(c));
        p->fillRect(u.x - 9, u.y - 9, 18, 18, c);
        p->setPen(JZPen(JZColor::Black, 1));
        p->drawRect(u.x - 9, u.y - 9, 18, 18);
        p->setPen(JZPen(JZColor::Black));
        const char* t = u.type == 0 ? "P" : u.type == 1 ? "R" : "+";
        p->drawText(u.x - 5, u.y - 6, t);
    }
}

void ShooterGame::drawExplosions(JZPainter* p)
{
    m_frameDraws += 2 * (int)m_booms.size();   // 每个爆炸 2 段弧
    for (auto& b : m_booms) {
        int r = b.max - b.life + 4;
        int a = (int)(200 * b.life / (float)b.max);
        p->setPen(JZPen(JZColor(255, 180, 60, (uint8_t)a), 2));
        p->drawArc(b.x, b.y, r, 0, 3600);
        p->setPen(JZPen(JZColor(255, 240, 120, (uint8_t)(a * 0.6f)), 2));
        p->drawArc(b.x, b.y, r * 2 / 3, 0, 3600);
    }
}

void ShooterGame::drawHud(JZPainter* p)
{
    m_frameDraws += 3 + m_lives;   // 2 行文字 + 生命图标 + 提示行
    char buf[96];
    std::snprintf(buf, sizeof(buf), "SCORE:%d/%d  LIVES:%d  x%d  RING:%d",
                  m_score, kTargetScore, m_lives, m_multi, m_ring);
    p->setPen(JZPen(JZColor::White));
    p->drawText(12, 14, buf);

    // 性能信息: FPS(峰值) / 每帧绘制请求数(峰值) / 实体数
    std::snprintf(buf, sizeof(buf),
                  "FPS:%.0f(peak %.0f)  DRAW:%d(peak %d)  ENM:%d  BUL:%d  EBUL:%d",
                  fps(), peakFps(), m_frameDraws, m_peakDraws,
                  (int)m_enemies.size(), (int)m_bullets.size(), (int)m_ebullets.size());
    p->setPen(JZPen(JZColor(150, 200, 150)));
    p->drawText(12, 36, buf);

    // 生命小图标
    p->setBrush(JZBrush(JZColor(120, 220, 255)));
    for (int i = 0; i < m_lives; i++)
        p->fillRect(width() - 24 - i * 22, 10, 14, 10, JZColor(120, 220, 255));

    p->setPen(JZPen(JZColor(150, 150, 170)));
    p->drawText(12, height() - 24,
        "Arrows / WASD move   Space shoot   R restart");
}

void ShooterGame::drawOverlay(JZPainter* p)
{
    m_frameDraws += 4;   // 遮罩 + 3 行文字
    int w = width(), h = height();
    p->fillRect(0, 0, w, h, JZColor(0, 0, 0, 150));
    p->setPen(JZPen(JZColor::Red, 3));
    p->setFont(JZFont(48));
    p->drawText(w / 2 - 150, h / 2 - 30, "GAME OVER");
    p->setPen(JZPen(JZColor::White));
    p->setFont(JZFont(20));
    p->drawText(w / 2 - 110, h / 2 + 20,
        ("score: " + std::to_string(m_score)).c_str());
    p->drawText(w / 2 - 110, h / 2 + 50, "press R / Enter to restart");
}

void ShooterGame::drawErrorOverlay(JZPainter* p)
{
    int w = width(), h = height();
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
