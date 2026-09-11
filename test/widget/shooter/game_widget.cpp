#include "game_widget.h"
#include "JZColor.h"
#include "JZRect.h"
#include "JZDebug.h"

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

GameWidget::GameWidget(JZWidget* parent)
    : JZWidget(parent)
{
    m_logicTimer  = new JZFixedTimer(this);
    m_renderTimer = new JZTimer(this);
    connect(&m_logicTimer->sigTimeout, this, &GameWidget::onLogic);
    connect(&m_logicTimer->sigError,   this, &GameWidget::onLogicError);
    connect(&m_renderTimer->sigTimeout, this, &GameWidget::onRender);
    std::srand((unsigned)::time(nullptr));
}

GameWidget::~GameWidget()
{
    if (m_logicTimer)  { m_logicTimer->stop();  delete m_logicTimer;  m_logicTimer = nullptr; }
    if (m_renderTimer) { m_renderTimer->stop(); delete m_renderTimer; m_renderTimer = nullptr; }
}

void GameWidget::showEvent(JZShowEvent* event)
{
    JZWidget::showEvent(event);
    reset();                 // 此时 width()/height() 已就绪
    setFocus();              // 抢占焦点以接收键盘事件
    m_lastTick = std::chrono::steady_clock::now();
    m_animTick = m_lastTick;
    m_logicTimer->start(kLogicMs);    // 逻辑: 固定步长
    m_renderTimer->start(kRenderMs);  // 渲染: 尽可能快(压测 FPS)
}

void GameWidget::reset()
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
    m_errMsg.clear();
    m_bullets.clear();
    m_ebullets.clear();
    m_enemies.clear();
    m_pwups.clear();
    m_booms.clear();
    m_keys.clear();

    // 星星背景
    m_stars.clear();
    int n = (w * h) / 4000;
    for (int i = 0; i < n; i++)
        m_stars.push_back({ std::rand() % w, std::rand() % h,
                           1 + std::rand() % 2, 1 + std::rand() % 2 });
}

// ---------------- 游戏循环 ----------------
// 逻辑定时器: 固定步长推进仿真 (与渲染解耦, 仿真速度不随 FPS 变化)
// JZFixedTimer 以墙钟为基准, 唤醒时若落后会连续补帧; 落后过多(补不过来)则
// 触发 sigError -> onLogicError, 这里停止游戏并报错(不再追帧).
void GameWidget::onLogic()
{
    updateStars();                 // 背景滚动(放逻辑里, 帧率无关)
    if (!m_over)
        updateGame();

    // 爆炸特效即便 game over 也继续播放完
    for (auto& b : m_booms) b.life--;
    m_booms.erase(std::remove_if(m_booms.begin(), m_booms.end(),
        [](const Boom& b) { return b.life <= 0; }), m_booms.end());
}

// 逻辑落后过多(补不过来): 停止游戏并上报错误, 不再尝试追帧.
void GameWidget::onLogicError(int missed)
{
    jzWarning() << "GameWidget logic fell too far behind (" << missed
                << " frames missed), stopping game";
    if (m_logicTimer)
        m_logicTimer->stop();   // JZFixedTimer 在 emit sigError 前已 stop, 这里确保
    m_errMsg = "逻辑帧落后过多, 游戏停止 (落后 " + std::to_string(missed) + " 帧)";
    update();   // 立即重绘以显示错误遮罩
}

// 渲染定时器: 只负责重绘 + 测 FPS (尽可能快, 用于压测)
void GameWidget::onRender()
{
    auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - m_animTick).count();
    m_animTick = now;
    m_animT += dt;   // 推进动画时钟 (驱动旋转/缩放变换演示)
    updateFps();
    update();   // 触发重绘
}

void GameWidget::updateStars()
{
    int w = width(), h = height();
    for (auto& s : m_stars) {
        s.y += s.spd;
        if (s.y > h) { s.y = 0; s.x = std::rand() % w; }
    }
}

void GameWidget::updateFps()
{
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastTick).count();
    m_lastTick = now;
    if (ms > 0) {
        float f = 1000.0f / (float)ms;
        m_fps = m_fps == 0 ? f : (m_fps * 0.9f + f * 0.1f);
        if (f > m_peakFps) m_peakFps = f;
    }
}

void GameWidget::updateGame()
{
    int w = width(), h = height();
    if (m_invuln > 0) m_invuln--;

    // 玩家移动
    if (m_keys.count(VK_LEFT_)  || m_keys.count(VK_A)) m_playerX -= kPlayerSpd;
    if (m_keys.count(VK_RIGHT_) || m_keys.count(VK_D)) m_playerX += kPlayerSpd;
    if (m_keys.count(VK_UP_)    || m_keys.count(VK_W)) m_playerY -= kPlayerSpd;
    if (m_keys.count(VK_DOWN_)  || m_keys.count(VK_S)) m_playerY += kPlayerSpd;
    m_playerX = std::max(16, std::min(w - 16, m_playerX));
    m_playerY = std::max(40, std::min(h - 20, m_playerY));

    // 开火
    if (m_playerCd > 0) m_playerCd--;
    if (m_keys.count(VK_SPACE) && m_playerCd == 0) {
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

void GameWidget::firePlayer()
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

void GameWidget::spawnEnemy()
{
    int w = width();
    int type = std::rand() % 100 < 30 ? 1 : 0;   // 30% 大块头
    int ew = type ? 46 : 30;
    int eh = type ? 40 : 26;
    m_enemies.push_back({ 20 + std::rand() % (w - 40), -eh,
                          ew, eh, type ? 3 : 1, type ? 0 : 1, type,
                          60 + std::rand() % 80 });
}

void GameWidget::enemyFire(Enemy& e)
{
    // 朝玩家方向发射
    int dx = m_playerX - e.x;
    int dy = m_playerY - e.y;
    float len = std::sqrt((float)(dx * dx + dy * dy)) + 1e-3f;
    m_ebullets.push_back({ e.x, e.y + e.h / 2,
                           (int)(dx / len * kEBulletSpd),
                           (int)(dy / len * kEBulletSpd) });
}

void GameWidget::hitPlayer()
{
    m_lives--;
    m_invuln = 90;   // ~2.5s 无敌
    m_booms.push_back({ m_playerX, m_playerY, 26, 26 });
    if (m_lives <= 0) { m_lives = 0; m_over = true; }
}

// ---------------- 键盘 ----------------
void GameWidget::keyPressEvent(JZKeyEvent* event)
{
    uint32_t k = event->key();
    m_keys.insert(k);
    if (k == VK_R || k == VK_RETURN) {
        bool needRecover = m_over || (m_logicTimer && !m_logicTimer->isActive());
        if (needRecover) {
            reset();
            if (m_logicTimer && !m_logicTimer->isActive())
                m_logicTimer->start(kLogicMs);   // 从落后停止状态恢复
        }
    }
    event->accept();
}

void GameWidget::keyReleaseEvent(JZKeyEvent* event)
{
    m_keys.erase(event->key());
    event->accept();
}

// ---------------- 绘制 ----------------
void GameWidget::paintEvent(JZPainterEvent* event)
{
    JZPainter* p = event->painter();
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
    else if (!m_errMsg.empty()) drawErrorOverlay(p);

    if (m_frameDraws > m_peakDraws) m_peakDraws = m_frameDraws;
}

void GameWidget::drawBackground(JZPainter* p)
{
    int w = width(), h = height();
    p->fillRect(0, 0, w, h, JZColor(10, 12, 30));

    // 星星向下滚动 (位置由 updateStars() 在逻辑步内更新, 此处只读)
    for (auto& s : m_stars) {
        p->fillRect(s.x, s.y, s.sz, s.sz, JZColor(190, 200, 255));
    }

    // 地面线
    p->setPen(JZPen(JZColor(40, 60, 90), 1));
    p->drawLine(0, h - 8, w, h - 8);

    m_frameDraws += 2 + (int)m_stars.size();   // 背景 + 星星 + 地面线
}

void GameWidget::drawShip(JZPainter* p, int cx, int cy, const JZColor& c, bool scalePulse)
{
    m_frameDraws += 8;   // 7 条线 + 1 座舱 (save/restore 不计入)
    p->save();
    p->translate(cx, cy);              // 把局部原点移到飞船中心
    p->rotate(m_animT * 50.0);         // 持续旋转: 验证矩阵 rotate (屏幕顺时针为正)
    if (scalePulse) {                  // 受击期间可见帧: 额外缩放脉冲 (变换大小)
        double s = 1.0f + 0.6f * std::sin(m_animT * 28.0f);
        p->scale(s, s);
    }
    // 以下几何以 (0,0) 为中心绘制
    p->setPen(JZPen(c, 2));
    p->drawLine(-15, 14, 0, -18);      // 机身 (三角)
    p->drawLine(0, -18, 15, 14);
    p->drawLine(15, 14, -15, 14);
    p->drawLine(-15, 14, -24, 4);     // 机翼
    p->drawLine(15, 14, 24, 4);
    int flame = std::rand() % 8 + 4;
    p->setPen(JZPen(JZColor::Orange, 2));
    p->drawLine(-5, 14, -5, 14 + flame);   // 尾焰
    p->drawLine(5, 14, 5, 14 + flame);
    p->setBrush(JZBrush(JZColor::Cyan));
    p->fillRect(-3, -8, 6, 9, JZColor::Cyan);  // 座舱
    p->restore();
}

void GameWidget::drawEnemy(JZPainter* p, int cx, int cy, int w, int h, int type)
{
    m_frameDraws += 4;   // 机身填充 + 描边 + 两只眼 (save/restore 不计入)
    // 每敌相位略错开: 旋转 + 轻微缩放脉冲, 验证多实体下的矩阵变换
    double ang = m_animT * 25.0f + cx * 0.05f;
    double sc  = 1.0f + 0.12f * std::sin(m_animT * 4.0f + cy * 0.03f);
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

void GameWidget::drawBullets(JZPainter* p)
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

void GameWidget::drawEnemyBullets(JZPainter* p)
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

void GameWidget::drawPowerUps(JZPainter* p)
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

void GameWidget::drawExplosions(JZPainter* p)
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

void GameWidget::drawHud(JZPainter* p)
{
    m_frameDraws += 3 + m_lives;   // 2 行文字 + 生命图标 + 提示行
    char buf[96];
    std::snprintf(buf, sizeof(buf), "SCORE:%d  LIVES:%d  x%d  RING:%d",
                  m_score, m_lives, m_multi, m_ring);
    p->setPen(JZPen(JZColor::White));
    p->drawText(12, 14, buf);

    // 性能信息: FPS(峰值) / 每帧绘制请求数(峰值) / 实体数
    std::snprintf(buf, sizeof(buf),
                  "FPS:%.0f(peak %.0f)  DRAW:%d(peak %d)  ENM:%d  BUL:%d  EBUL:%d",
                  m_fps, m_peakFps, m_frameDraws, m_peakDraws,
                  (int)m_enemies.size(), (int)m_bullets.size(), (int)m_ebullets.size());
    p->setPen(JZPen(JZColor(150, 200, 150)));
    p->drawText(12, 36, buf);

    // 生命小图标
    p->setBrush(JZBrush(JZColor(120, 220, 255)));
    for (int i = 0; i < m_lives; i++)
        p->fillRect(width() - 24 - i * 22, 10, 14, 10, JZColor(120, 220, 255));

    p->setPen(JZPen(JZColor(150, 150, 170)));
    p->drawText(12, height() - 24,
        "← → / A D 移动   空格 射击   R 重开");
}

void GameWidget::drawOverlay(JZPainter* p)
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
        ("分数: " + std::to_string(m_score)).c_str());
    p->drawText(w / 2 - 110, h / 2 + 50, "按 R / 回车 重新开始");
}

void GameWidget::drawErrorOverlay(JZPainter* p)
{
    int w = width(), h = height();
    p->fillRect(0, 0, w, h, JZColor(0, 0, 0, 170));
    p->setPen(JZPen(JZColor::Red, 3));
    p->setFont(JZFont(40));
    p->drawText(w / 2 - 150, h / 2 - 40, "GAME STOPPED");
    p->setPen(JZPen(JZColor(255, 180, 180)));
    p->setFont(JZFont(18));
    p->drawText(w / 2 - 180, h / 2 + 10, m_errMsg.c_str());
    p->setPen(JZPen(JZColor::White));
    p->setFont(JZFont(18));
    p->drawText(w / 2 - 130, h / 2 + 50, "按 R / 回车 重新运行");
}
