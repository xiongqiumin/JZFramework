// JZEmulator 冒烟测试 —— 在**真窗口**里把 mGBA 跑起来，跑够帧数后自检 + 存一帧 BMP。
//
// 这一份验的是**控件层**那条链：JZTimer 驱动的 onTick -> 模拟器步进 -> BGRA(X)->RGBA
// -> JZImage -> paintEvent 里 drawImage 缩放贴图。模拟器核心本身（DLL 加载 / ROM /
// 按键生效）已另有验证，见 JZEmulator/JZEmulatorCore.h 的文件头。
//
// 用法（在构建目录下，或用 ctest -R gba）:
//   test_emulator [dll目录] [rom路径] [--frames N] [--ms N]
// 默认: dll 目录 = 构建时写入的 JZ_MGBA_DIR，rom = E:/work/simulator/rom.gba
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "JZApplication.h"
#include "JZTimer.h"
#include "JZWindow.h"

#include "JZEmulatorView.h"

#ifndef JZ_EMULATOR_DLL_DIR_DEFAULT
#  define JZ_EMULATOR_DLL_DIR_DEFAULT "E:/work/simulator/host/build"
#endif

static int g_fail = 0;
static void check(bool ok, const char* msg)
{
    printf("  [%s] %s\n", ok ? "ok" : "FAIL", msg);
    if (!ok)
        ++g_fail;
}

// BGRA(每像素 4 字节) -> 32bpp BMP（top-down）
static bool write_bmp(const char* path, const unsigned char* bgra, int w, int h)
{
    const int row = w * 4, imgSize = row * h, hdrSize = 54;
    unsigned char hdr[hdrSize];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    *(int*)(hdr + 2) = hdrSize + imgSize;
    *(int*)(hdr + 10) = hdrSize;
    *(int*)(hdr + 14) = 40;
    *(int*)(hdr + 18) = w;
    *(int*)(hdr + 22) = -h;
    *(short*)(hdr + 26) = 1;
    *(short*)(hdr + 28) = 32;
    *(int*)(hdr + 34) = imgSize;
    FILE* f = fopen(path, "wb");
    if (!f)
        return false;
    fwrite(hdr, 1, sizeof(hdr), f);
    fwrite(bgra, 1, (size_t)imgSize, f);
    fclose(f);
    return true;
}

int app_main(int argc, char* argv[])
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    std::string dllDir = JZ_EMULATOR_DLL_DIR_DEFAULT;
    std::string rom = "E:/work/simulator/rom.gba";
    int targetFrames = 120;
    int tickMs = 16;

    int pos = 1;
    if (pos < argc && argv[pos][0] != '-')
        dllDir = argv[pos++];
    if (pos < argc && argv[pos][0] != '-')
        rom = argv[pos++];
    for (; pos < argc; ++pos)
    {
        if (!strcmp(argv[pos], "--frames") && pos + 1 < argc)
            targetFrames = atoi(argv[++pos]);
        else if (!strcmp(argv[pos], "--ms") && pos + 1 < argc)
            tickMs = atoi(argv[++pos]);
    }

    JZApplication app;

    JZWindow window;
    window.resize(240, 160);       // 1:1 —— 画面就是 GBA 原生尺寸（控件不做放大）
    window.setTitle("JZEmulator smoke test");

    JZEmulatorView* view = new JZEmulatorView(&window);   // parent = 窗口 ⇒ 归它所有
    view->setGeometry(0, 0, 240, 160);
    view->show();                                // showEvent 里会 setFocus（抢键盘）

    if (!view->openRom(dllDir, rom))
    {
        printf("FAIL openRom: %s\n", view->lastError().c_str());
        return 1;
    }
    printf("[emu] dll=%s\n[emu] rom=%s\n", dllDir.c_str(), rom.c_str());
    view->start(tickMs);
    window.show();

    // 轮询等它跑够帧数：自检 + 存一帧 + 收工
    JZTimer* watch = new JZTimer(&window);
    bool stopped = false;
    bool printedFirst = false;
    connect(&watch->sigTimeout, [&]() {
        if (stopped)
            return;
        const int n = view->frameCount();
        if (!printedFirst)
        {
            printedFirst = true;
            printf("[emu] 首拍 frameCount=%d（定时器 %dms/拍）\n", n, tickMs);
        }
        if (n < targetFrames)
            return;
        stopped = true;
        watch->stop();

        printf("== 自检 (跑了 %d 拍) ==\n", n);
        check(n >= targetFrames, "定时器在推进（onTick 每拍跑一帧）");
        check(view->isRomOpen(), "ROM 仍处于打开状态");

        int w = 0, h = 0;
        const uint8_t* px = view->frame(&w, &h);
        check(px != nullptr, "frame() 能取到像素");
        check(w == 240 && h == 160, "帧尺寸是 GBA 的 240x160");

        if (px && w > 0 && h > 0)
        {
            // 画面不该是一块纯色（那说明没渲染出内容/转换写坏）
            const size_t pixels = (size_t)w * h;
            int distinct = 0;
            for (size_t i = 1; i < pixels; ++i)
            {
                if (px[i * 4] != px[0] || px[i * 4 + 1] != px[1] || px[i * 4 + 2] != px[2])
                    ++distinct;
            }
            printf("  (帧内与首像素不同的像素数 = %d / %d)\n", distinct, (int)pixels);
            check(distinct > (int)pixels / 100, "画面有实际内容（非纯色）");
            if (write_bmp("emulator_frame.bmp", px, w, h))
                printf("  [ok] 已存 emulator_frame.bmp\n");
        }

        // 抓控件自己的图：这条才是 paintEvent 那条绘制链（BGRA->RGBA -> 最近邻整数倍放大
        // -> 直拷贴图）。上面验的是模拟器给的原始帧，不经过绘制。
        {
            JZImage shot = view->grab();
            check(!shot.isNull(), "控件截图非空（JZWidget::grab）");
            if (!shot.isNull())
            {
                const int sw = shot.width(), sh = shot.height();
                const int total = sw * sh;
                const std::shared_ptr<JZImageData> data = shot.data();
                const uint8_t* sp = data ? data->pixels() : nullptr;
                int nonBlack = 0;
                if (sp)
                {
                    for (int i = 0; i < total; ++i)
                        if (sp[i * 4] || sp[i * 4 + 1] || sp[i * 4 + 2])
                            ++nonBlack;
                }
                printf("  (控件截图 %dx%d 物理像素, 非黑像素 %d/%d)\n", sw, sh, nonBlack, total);
                check(nonBlack > total / 10, "控件上真的画出了画面（非黑占比 > 10%）");
                if (shot.save("emulator_view.png"))
                    printf("  [ok] 已存 emulator_view.png（控件上实际显示的内容，1:1）\n");
            }
        }

        printf("[emu] %s\n", g_fail == 0 ? "PASS" : "FAIL");
        window.close();       // 关掉唯一顶层窗口 -> 应用退出
    });
    watch->start(100);

    const int rc = app.exec();
    printf("[emu] exit=%d fail=%d\n", rc, g_fail);
    return g_fail == 0 ? 0 : 1;
}

int main(int argc, char* argv[])
{
    return app_main(argc, argv);
}
