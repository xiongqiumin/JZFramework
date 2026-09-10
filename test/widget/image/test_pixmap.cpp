// JZPixmap 冒烟测试 (不依赖窗口系统, 控制台直接跑):
//   JZImage(RGBA8888) -> JZPixmap(32bppPARGB: B,G,R,A + 预乘) 的转换结果逐字节校验,
//   外加空图/尺寸/fromData 共享语义。
//
// 为什么要这么较真: 绘制期不做任何转换, 全部依赖这里转出来的字节 —— 通道序错了就是
// 红蓝互换, 预乘漏了就是半透明像素发白发灰, 都得在这里挡住。

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "JZImage.h"
#include "JZPixmap.h"

namespace
{

int g_failed = 0;

void check(bool ok, const std::string& what)
{
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what.c_str());
    if (!ok)
        ++g_failed;
}

// 从像素缓冲读某点 4 字节
std::string px4(const uint8_t* p)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "(%d,%d,%d,%d)", p[0], p[1], p[2], p[3]);
    return buf;
}

bool pixelEquals(const std::shared_ptr<JZPixmapData>& d, int x, int y,
                 int b, int g, int r, int a)
{
    if (!d || x < 0 || y < 0 || x >= d->width() || y >= d->height())
        return false;
    const uint8_t* p = d->pixels() + ((size_t)y * d->width() + x) * 4;
    return p[0] == b && p[1] == g && p[2] == r && p[3] == a;
}

} // namespace

int main()
{
    // ---- 空图 ----
    {
        JZPixmap empty;
        check(empty.isNull(), "默认构造为空 pixmap");
        check(empty.width() == 0 && empty.height() == 0, "空 pixmap 尺寸为 0");
        check(JZPixmap::fromImage(JZImage()).isNull(), "fromImage(空 JZImage) 为空");
    }

    // ---- RGBA8888 -> 32bppPARGB ----
    const int W = 4, H = 1;
    const uint8_t rgba[W * 4] = {
        255,   0,   0, 255,   // 不透明红
        255, 255, 255, 128,   // 白, a=128 (需预乘)
         10, 200,  30,   0,   // 全透明 (PARGB 必须全 0)
          0, 255,   0, 255    // 不透明绿
    };

    const JZImage img = JZImage::fromRGBA(rgba, W, H);
    check(!img.isNull(), "fromRGBA 造图非空");

    const JZPixmap pm = JZPixmap::fromImage(img);
    check(!pm.isNull(), "fromImage 非空");
    check(pm.width() == W && pm.height() == H, "pixmap 尺寸与原图一致");
    check(pm.data() && pm.data()->pixels() != nullptr, "pixmap 有像素缓冲");

    const std::shared_ptr<JZPixmapData> d = pm.data();
    if (d && d->pixels())
    {
        // 不透明像素: 只换序 (R/B 交换), alpha 保持 255
        check(pixelEquals(d, 0, 0, 0, 0, 255, 255),
              "不透明红 -> (B,G,R,A)=(0,0,255,255), 实际 " + px4(d->pixels()));
        check(pixelEquals(d, 3, 0, 0, 255, 0, 255),
              "不透明绿 -> (0,255,0,255), 实际 " + px4(d->pixels() + 3 * 4));
        // 半透明: R/G/B 都要乘 alpha/255 (255*128/255 = 128)
        check(pixelEquals(d, 1, 0, 128, 128, 128, 128),
              "白 a=128 -> (128,128,128,128) 预乘, 实际 " + px4(d->pixels() + 4));
        // 全透明: 全 0 (不能留下未预乘的杂色, 否则边缘会有黑/亮边)
        check(pixelEquals(d, 2, 0, 0, 0, 0, 0),
              "a=0 -> 全 0, 实际 " + px4(d->pixels() + 8));
    }

    // ---- JZPixmap 与 JZImage 各自独立: 源图数据可变, pixmap 是快照 ----
    {
        std::vector<uint8_t> buf((size_t)W * H * 4, 0);
        buf[0] = 255; buf[3] = 255;                 // 红
        std::shared_ptr<JZImageData> data = std::make_shared<JZImageData>();
        data->setPixels(W, H, buf.data());
        JZImage src = JZImage::fromData(data);
        check(src.data() == data, "JZImage::fromData 共享同一份数据 (不拷贝)");

        const JZPixmap snap = JZPixmap::fromImage(src);
        buf[2] = 255; buf[0] = 0;                   // 改原缓冲
        data->setPixels(W, H, buf.data());
        check(pixelEquals(snap.data(), 0, 0, 0, 0, 255, 255),
              "pixmap 是生成时的快照, 不受源图后续修改影响");
    }

    // ---- 多行图尺寸/步长 (行边界不能错位) ----
    {
        const int MW = 3, MH = 2;
        std::vector<uint8_t> buf((size_t)MW * MH * 4, 0);
        for (int i = 0; i < MW * MH; ++i) { buf[i * 4 + 0] = 255; buf[i * 4 + 3] = 255; }
        const JZImage many = JZImage::fromRGBA(buf.data(), MW, MH);
        const JZPixmap mp = JZPixmap::fromImage(many);
        check(mp.width() == MW && mp.height() == MH, "多行图尺寸");
        check(pixelEquals(mp.data(), MW - 1, MH - 1, 0, 0, 255, 255),
              "多行图最后一像素正确 (行步长无错位)");
    }

    std::printf("\n%s (%d failed)\n", g_failed == 0 ? "ALL PASS" : "FAILED", g_failed);
    return g_failed == 0 ? 0 : 1;
}
