#ifndef JZ_PIXMAP_H_
#define JZ_PIXMAP_H_

#include <cstdint>
#include <memory>
#include <vector>

class JZImage;

// ==================== JZPixmap: 显示用位图 (对齐 Qt 的 QImage -> QPixmap) ====================
// JZImage 是"工程像素数据": RGBA8888、格式无关、可改/可编解码/可缩放, 但每个渲染
// 后端都得先把它转成自己的原生布局才能贴图 —— GDI+ 要的是 32bppPARGB
// (内存字节序 B,G,R,A, 且 R/G/B 已按 alpha 预乘)。
//
// JZPixmap 就是"已经转好、可以直接贴"的显示用位图:
//   - 转换只在 fromImage() 时做一次 (O(像素)), 之后每次绘制没有任何逐像素处理。
//     (对比: 绘制期用 ImageAttributes 颜色矩阵现换, 3840x2160 实测 425ms/帧 —— 
//      所以"每帧现转"这条路是不通的, 想快就只能预转 + 反复贴。)
//   - 像素缓冲由本对象 (shared_ptr) 持有, 后端绘制时零拷贝引用它, 因此不存在
//     "位图指向的缓冲先被释放" 的悬垂 (GDI+ 的 scan0 构造函数只记指针不复制)。
//   - 内容只读。要改像素请改 JZImage 后重新生成 pixmap。
//
// 用法: 同一张图要反复画 (图标/缩略图/全屏标注底图) 就预先转成 JZPixmap 并持有,
// 绘制时 JZPainter::drawPixmap(); 只画一两次的直接 JZPainter::drawImage(JZImage)
// 即可 (后端内部会做同样的转换并缓存, 但那条路径的缓存是按图懒建的)。
//
// 像素布局目前固定为 GDI+ 需要的 32bppPARGB; 将来接入别的后端时在这里扩充目标
// 格式 (新增枚举/工厂, 由该后端负责转换) 即可。
class JZPixmapData
{
public:
    int width() const { return m_w; }
    int height() const { return m_h; }
    bool isEmpty() const { return m_w <= 0 || m_h <= 0 || m_pixels.empty(); }

    // 32bppPARGB: 每像素 4 字节 = B, G, R, A (R/G/B 已预乘 alpha)
    const uint8_t* pixels() const { return m_pixels.empty() ? nullptr : m_pixels.data(); }

    // 接管一块 pixels (w*h*4 字节); 尺寸与缓冲不符时置为空。
    void setPixels(int w, int h, std::vector<uint8_t>&& pixels);

private:
    int m_w = 0;
    int m_h = 0;
    std::vector<uint8_t> m_pixels;
};

class JZPixmap
{
public:
    JZPixmap() = default;

    bool isNull() const;
    int width() const;
    int height() const;
    std::shared_ptr<JZPixmapData> data() const { return m_data; }

    // 由 JZImage 生成显示用位图 (一次性转换)。空图返回空 pixmap。
    static JZPixmap fromImage(const JZImage& img);

private:
    std::shared_ptr<JZPixmapData> m_data;
};

#endif // JZ_PIXMAP_H_
