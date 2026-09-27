#ifndef JZ_TEXT_IMAGE_MAP_H_
#define JZ_TEXT_IMAGE_MAP_H_
#include <map>
#include <string>

#include "JZImage.h"
#include "JZPixmap.h"

// ==================== 文本内联图片注册表 (单例) ====================
//
// 设计: 图片不占用多字符标记, 而是占**一个私有域码点** (PUA, Plane 15/16:
// U+F0000..U+FFFFD)。这样文本流里它就是"一个普通字符":
//   - 前缀测宽 / 光标定位 / 退格删除 / 选区 全部天然把它当 1 个单元;
//   - 不需要 run 模型, 不需要"视觉下标", 不需要标记语法解析。
// 唯一需要特判的地方是**测宽与绘制**: 遇到本区间的码点要去本表查图片尺寸。
//
// 存储形态: 表里存的**不是原图**, 而是"按显示尺寸处理好的一份"——
//   - 尺寸先被**上限** (setMaxDisplaySize, 默认 320x240) 等比收缩;
//   - 再按该尺寸缩放像素 (image) 并预转成显示用位图 (pixmap)。
// 于是文本控件那边"测宽 / 绘制 / 缓存"三个口径天然一致, 且一张截图无论多大,
// 在文本里的显示尺寸与内存占用都有上界 (不会撑爆行高, 也不会每帧重采样)。
//
// 生命周期: 本表是进程级单例, 图片按 **key 幂等** 分配码点:
//   - 同一 key 重复 add 返回同一码点, 不会重复占位;
//   - 引用计数 refCount 记录"当前有多少份文本还在用这个码点"。
// 文本控件在 relayout 时扫描自身文本, 对本表登记/释放引用。
//
// 注意: 码点会在应用生命周期内**复用**, 因此 key 必须能唯一标识一张图
// (例如文件路径 / 截图 id), 不能只用序号, 否则不同内容会撞到同一码点。
class JZFRAMEWORK_API JZTextImageMap
{
public:
    // 私有使用区 (Plane 15) 起点。U+F0000..U+FFFFD 为补充私有使用区 A,
    // 普通字体必然没有字形, 因此不会被机器上任何字体"意外渲染出字"。
    static const char32_t kPuaBase = 0xF0000;
    static const char32_t kPuaEnd = 0xFFFFD;

    struct Entry {
        std::string key;        // 语义标识 (幂等分配的依据)
        // 显示尺寸的像素 (超上限时已**等比缩小**; 可能为空: 只有尺寸的占位)。
        // 入库即缩放: 巨图不会以原始分辨率留在内存/不会每帧重采样。
        JZImage     image;
        // image 的显示用位图 (入库时预转成后端原生布局), 绘制时直接贴,
        // 不再走后端"按图懒转换"的缓存查找。占位条目 (无像素) 为空。
        JZPixmap    pixmap;
        int         width = 0;  // 显示宽 (逻辑像素)
        int         height = 0; // 显示高 (逻辑像素)
        int         refCount = 0; // 当前有多少份文本引用该码点
    };

    static JZTextImageMap& instance();

    // ---- 码点判定 ----
    static bool isImageCode(char32_t cp)
    {
        return cp >= kPuaBase && cp <= kPuaEnd;
    }

    // ---- 显示尺寸上限 (逻辑像素) ----
    // 超过上限的图片在 add() 时**等比缩小**后再入库, 于是:
    //   - 行高/布局不会被一张巨图撑爆 (文本里图片是"一个字符", 但不能让行变成半屏);
    //   - 入库的是缩小后的像素与 pixmap, 缓存内存与每次贴图的代价都有界。
    // w/h <= 0 表示该方向不限制。本表是进程级单例, 上限也是**进程级**的。
    static void setMaxDisplaySize(int w, int h);
    static int  maxDisplayWidth();
    static int  maxDisplayHeight();
    // 按上限等比收缩 (w/h 就地修改; 上限 <= 0 的方向不参与)
    static void clampToMaxDisplaySize(int& w, int& h);

    // ---- 登记 / 查询 ----
    // 按 key 幂等分配码点; 已存在则复用并刷新图片与尺寸 (refCount 由 retain 管)。
    // 失败 (image 无像素且未给尺寸) 返回 0。
    char32_t add(const std::string& key, const JZImage& image,
                 int width = 0, int height = 0);

    // 查找; 未登记的码点返回 nullptr
    const Entry* find(char32_t cp) const;

    // 便捷: 取显示尺寸; 未登记时给出兜底尺寸并返回 false
    bool sizeOf(char32_t cp, int& w, int& h) const;

    // ---- 引用计数 (文本控件在 relayout 时调用) ----
    void retain(char32_t cp);
    void release(char32_t cp);
    int  refCount(char32_t cp) const;

    // 丢弃所有 refCount == 0 的条目 (图片不再被任何文本引用)
    void collectUnused();

    // 全量清空 (测试/应用退出用)
    void clear();

    int count() const { return static_cast<int>(m_entries.size()); }

private:
    JZTextImageMap() = default;
    JZTextImageMap(const JZTextImageMap&) = delete;
    JZTextImageMap& operator=(const JZTextImageMap&) = delete;

    char32_t nextFreeCode() const;

    std::map<std::string, char32_t> m_byKey;   // key -> 码点 (幂等分配)
    std::map<char32_t, Entry>       m_entries; // 码点 -> 条目
    char32_t m_nextCandidate = kPuaBase;       // 下一个候选码点 (线性找空位)
};

#endif // JZ_TEXT_IMAGE_MAP_H_
