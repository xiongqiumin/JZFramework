// sprite_bench: JZSprite/JZScene 容量基准 (无 ECS 现状实测)。
// 测量四件事: 创建(堆分配) / 更新(forEach 遍历) / 碰撞查询(forEachOverlap) /
// 绘制(离屏 image, 与窗口同一条 draw task 管线)。
// 输出纯 ASCII (PowerShell 重定向读日志只认 ASCII)。
#include "JZScene.h"
#include "JZSprite.h"
#include "JZImage.h"
#include "JZPainter.h"
#include "JZColor.h"

#include <chrono>
#include <cstdio>
#include <cstddef>

static double nowMs()
{
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static JZPixmap makeGlowTexture(const JZColor& core, const JZColor& halo, int size)
{
    JZImage img = JZImage::create(size, size);
    {
        JZPainter p(img);
        const int r = size / 2;
        for (int i = 3; i >= 1; --i) {
            const double t = (double)i / 3.0;
            const int rr = (int)(r * t);
            JZColor c = halo;
            c.setRgba(c.red(), c.green(), c.blue(), (uint8_t)(30 + 60 * (1.0 - t)));
            p.fillEllipse(r - rr, r - rr, rr * 2, rr * 2, c);
        }
        const int cr = size / 5;
        p.fillEllipse(r - cr, r - cr, cr * 2, cr * 2, core);
    }
    return JZPixmap::fromImage(img);
}

struct Row {
    int n = 0;
    double createMs = 0;   // 创建 N 个 (含 N 次堆分配)
    double updateMs = 0;   // 一次全量 forEach 更新
    double queryMs = 0;    // 一次 forEachOverlap (200x200 查询框)
    double drawMs = 0;     // 一帧绘制 (离屏 800x600)
    double perSpriteUs = 0;
};

// mode: 0=全纹理玉  1=全描边椭圆玉  2=混合(各半)
// 每项测 reps 次取**最小值** (抗机器噪声: 发热/后台负载只会让数字变差, 取 min 逼近"干净"值)
static void runBatch(int n, const JZPixmap& glow, int mode, Row& out)
{
    JZScene scene;
    out.n = n;

    double t0 = nowMs();
    for (int i = 0; i < n; i++) {
        auto& s = scene.create<JZSprite>();
        s.x = (i * 37) % 800;
        s.y = (i * 53) % 600;
        s.vx = 1.0;
        s.vy = 0.5;
        const bool tex = (mode == 0) || (mode == 2 && i % 2 == 0);
        if (tex) {
            s.texture = glow;   // shared_ptr 拷贝 = 引用计数 +1
            s.width = 22;
            s.height = 22;
        } else {
            s.shape = JZSprite::Ellipse;
            s.width = 14;
            s.height = 14;
            s.color = JZColor(255, 90, 180);
            s.borderColor = JZColor(255, 210, 245);
            s.borderWidth = 1;
        }
    }
    out.createMs = nowMs() - t0;

    // ---- 更新: forEach 全量遍历 + 积分 ----
    {
        double best = 1e30;
        for (int r = 0; r < 5; r++) {
            t0 = nowMs();
            scene.forEach([](JZSprite& s) { s.x += s.vx; s.y += s.vy; });
            const double ms = nowMs() - t0;
            if (ms < best) best = ms;
        }
        out.updateMs = best;
    }

    // ---- 碰撞查询: 一次 forEachOverlap ----
    {
        JZBody body = JZBody::fromCenter(400, 300, 200, 200);
        double best = 1e30;
        for (int r = 0; r < 10; r++) {
            t0 = nowMs();
            long hits = 0;
            scene.forEachOverlap(body, [&](JZSprite&) { hits++; });
            (void)hits;
            const double ms = nowMs() - t0;
            if (ms < best) best = ms;
        }
        out.queryMs = best;
    }

    // ---- 绘制: 离屏 image; 测 5 帧取最小 (离屏合成是常数项, min 屏蔽掉它的抖动) ----
    {
        JZImage img = JZImage::create(800, 600);
        double best = 1e30;
        for (int f = 0; f < 5; f++) {
            t0 = nowMs();
            {
                JZPainter p(img);
                scene.draw(&p);
            }
            const double ms = nowMs() - t0;
            if (ms < best) best = ms;
        }
        out.drawMs = best;
        out.perSpriteUs = out.drawMs * 1000.0 / n;
    }
}

int main()
{
    const JZPixmap glow = makeGlowTexture(JZColor(220, 250, 255), JZColor(60, 140, 255), 22);

    std::printf("sizeof(JZSprite) = %zu bytes (+ unique_ptr node ~ %zu bytes with malloc overhead)\n",
                sizeof(JZSprite), sizeof(JZSprite) + 16);

    // ---- 微探针放最前面 (干净的分配器/缓存状态; 也各测多次取 min) ----
    // 绕开 JZSprite 机制 (每精灵 save/restore => Push/Pop 任务), 直接推裸图元,
    // 分离"机制开销"与"图元开销"。
    std::printf("--- raw painter probes @10000 (no sprite machinery, min of 5) ---\n");
    {
        JZImage img = JZImage::create(800, 600);
        const int N = 10000;
        double best = 1e30;
        for (int f = 0; f < 5; f++) {
            const double t0 = nowMs();
            {
                JZPainter p(img);
                for (int i = 0; i < N; i++)
                    p.drawPixmap(glow, (i * 37) % 800, (i * 53) % 600, 22, 22);
            }
            const double ms = nowMs() - t0;
            if (ms < best) best = ms;
        }
        std::printf("raw drawPixmap: %8.2f ms  (%.3f us/call)\n", best, best * 1000.0 / N);

        best = 1e30;
        for (int f = 0; f < 5; f++) {
            const double t0 = nowMs();
            {
                JZPainter p(img);
                for (int i = 0; i < N; i++)
                    p.fillRect((i * 37) % 800, (i * 53) % 600, 14, 14, JZColor(255, 90, 180));
            }
            const double ms = nowMs() - t0;
            if (ms < best) best = ms;
        }
        std::printf("raw fillRect  : %8.2f ms  (%.3f us/call)\n", best, best * 1000.0 / N);

        best = 1e30;
        for (int f = 0; f < 5; f++) {
            const double t0 = nowMs();
            {
                JZPainter p(img);
                for (int i = 0; i < N; i++) {
                    p.save();
                    p.translate((i * 37) % 800, (i * 53) % 600);
                    p.restore();
                }
            }
            const double ms = nowMs() - t0;
            if (ms < best) best = ms;
        }
        std::printf("save/restore : %8.2f ms  (%.3f us/pair)\n", best, best * 1000.0 / N);
        std::fflush(stdout);
    }

    std::printf("%8s %11s %11s %11s %13s %13s\n",
                "N", "create(ms)", "update(ms)", "query(ms)", "draw(ms/frame)", "draw(us/sprite)");
    std::printf("--- mixed (half texture / half bordered-ellipse), min of 5 ---\n");
    const int ns[] = { 1000, 5000, 10000, 50000, 100000 };
    Row rows[5];
    for (int i = 0; i < 5; i++) {
        runBatch(ns[i], glow, 2, rows[i]);
        std::printf("%8d %11.2f %11.3f %11.3f %13.2f %13.3f\n",
                    rows[i].n, rows[i].createMs, rows[i].updateMs,
                    rows[i].queryMs, rows[i].drawMs, rows[i].perSpriteUs);
        std::fflush(stdout);
    }

    // 边际绘制成本: 10k -> 100k 的斜率 (消掉离屏每帧合成的常数项)
    const double slope = (rows[4].drawMs - rows[2].drawMs) * 1000.0 / (rows[4].n - rows[2].n);
    std::printf("marginal draw cost = %.3f us/sprite (10k->100k slope)\n", slope);
    std::printf("16.7ms/frame (60fps) budget -> max drawable sprites ~= %.0f\n",
                16700.0 / (slope > 0 ? slope : 1));
    std::printf("100k sprites memory ~= %.1f MB (sprites) + %.1f MB (ptr vector)\n",
                rows[4].n * (sizeof(JZSprite) + 16) / 1048576.0,
                rows[4].n * sizeof(void*) / 1048576.0);

    // ---- 成本拆分 @10k: 纹理玉 vs 描边椭圆玉 ----
    std::printf("--- cost split @10000 (min of 5) ---\n");
    const char* names[] = { "texture only ", "ellipse only ", "mixed        " };
    for (int m = 0; m < 3; m++) {
        Row r;
        runBatch(10000, glow, m, r);
        std::printf("%s draw = %8.2f ms/frame  (%.3f us/sprite)\n",
                    names[m], r.drawMs, r.perSpriteUs);
        std::fflush(stdout);
    }
    return 0;
}
