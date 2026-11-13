#ifndef JZ_CODE_AGENT_SCREENSHOT_H_
#define JZ_CODE_AGENT_SCREENSHOT_H_

#include <string>

#include "JZByteArray.h"
#include "JZImage.h"
#include "JZRect.h"

class JZWidget;

// 截图工具类: 抓屏/抓控件 -> 等比缩放 -> PNG 编码 -> data URL(供多模态消息发送) / 落盘,
// 以及"全屏标注截图窗口"(captureInteractive)。
// 全部为静态接口, 不持有状态; 图像统一为 RGBA8888 像素(与 JZWidget/JZScreen 一致)。
class JZScreenShot
{
public:
    // 随消息发给大模型时的长边上限(像素): 4K 整屏原图会让请求体过大, 默认等比缩到
    // 长边不超过该值(只缩不放); UI/文字截图在 1280 下仍然清晰可读。
    static const int kMaxDim = 1280;

    // ---- 抓取 ----
    static JZImage capturePrimaryScreen();       // 主显示器整屏
    static JZImage captureScreen(int index);     // 指定显示器整屏(越界返回空图)
    static JZImage captureWidget(JZWidget* w);   // 控件/窗口当前显示内容(被遮挡也能抓)

    // 抓整个虚拟桌面 = 所有显示器拼成一张(**物理像素**)图。
    //   - deviceScale: 目标缩放 (= 显示这张图的窗口的 device scale)。拼图统一到
    //     该缩放: 画布 = 逻辑桌面尺寸 * deviceScale, 单屏/各屏同缩放时每屏 1:1
    //     不重采样; 传 <=0 视为 1 (按逻辑像素拼, 会损失高 DPI 细节)。
    //   - logicalRect 非空时回填该虚拟桌面的**逻辑矩形** (图像尺寸 / deviceScale),
    //     与窗口屏幕坐标一致 —— 全屏窗口靠它摆到与桌面完全重合的位置。
    static JZImage captureVirtualDesktop(JZRect* logicalRect = nullptr,
                                         float deviceScale = 1.0f);

    // ---- 交互式截图(QQ 风格: 全屏蒙版 + 区域选择) ----
    // 先隐藏 owner 窗口, 再抓整个虚拟桌面(owner 不会出现在图里), 然后恢复 owner。
    // 弹出的全屏无边框窗口覆盖整张桌面: 全屏蒙版 + 拖拽框选(可移动 / 八向缩放,
    //   选区被"挖空"显示清晰原图, 带边框与控制点), 选区内可继续用 矩形/椭圆/画笔
    //   标注(1/2/3 切换工具, Ctrl+Z 撤销)。Enter 或 [完成] 接受, Esc 或 [取消] 放弃。
    // 确认返回**裁剪后**的选区图像(**物理像素** = 虚拟桌面逻辑尺寸 * 设备缩放,
    // 即与屏幕原始像素 1:1, 不损失清晰度), 标注一并烧入; 未框选时返回整屏。
    // 取消 / 抓屏失败 / owner 不可见时返回空图。owner 仅作模态归属(可空)。
    static JZImage captureInteractive(JZWidget* owner = nullptr);

    // ---- 变换 ----
    // 等比缩放到长边不超过 maxDim(不放大); maxDim<=0 或无需缩放时返回原图。
    static JZImage fit(const JZImage& img, int maxDim = kMaxDim);
    // 等比缩放到能放进 maxW×maxH 的框内(不放大); 用于输入框里的预览缩略图。
    static JZImage fitTo(const JZImage& img, int maxW, int maxH);

    // ---- 编码 / 解码 ----
    static JZByteArray toPng(const JZImage& img);
    // 缩放 + PNG + base64 拼成 "data:image/png;base64,..."(多模态消息的 image_url)
    static std::string toDataUrl(const JZImage& img, int maxDim = kMaxDim);
    // "data:image/...;base64,..." 或纯 base64 -> 图像(失败返回空图)
    static JZImage decodeDataUrl(const std::string& dataUrl);

    // PNG 落盘: dir 不存在会自动创建; name 为空时按时间戳命名(不含扩展名)。
    // 返回完整文件路径; 图像为空 / 编码或写盘失败返回空串。
    static std::string save(const JZImage& img, const std::string& dir,
                            const std::string& name = std::string());
};

#endif // !JZ_CODE_AGENT_SCREENSHOT_H_
