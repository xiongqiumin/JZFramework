#ifndef JZ_TEXT_IMAGE_MAP_H_
#define JZ_TEXT_IMAGE_MAP_H_

#include <map>
#include <string>

#include "JZImage.h"

// ==================== 文本内联图片注册表 (单例) ====================
//
// 设计: 图片不占用多字符标记, 而是占**一个私有域码点** (PUA, Plane 15/16:
// U+F0000..U+FFFFD)。这样文本流里它就是"一个普通字符":
//   - 前缀测宽 / 光标定位 / 退格删除 / 选区 全部天然把它当 1 个单元;
//   - 不需要 run 模型, 不需要"视觉下标", 不需要标记语法解析。
// 唯一需要特判的地方是**测宽与绘制**: 遇到本区间的码点要去本表查图片尺寸。
//
// 生命周期: 本表是进程级单例, 图片按 **key 幂等** 分配码点:
//   - 同一 key 重复 add 返回同一码点, 不会重复占位;
//   - 引用计数 refCount 记录"当前有多少份文本还在用这个码点"。
// 文本控件在 relayout 时扫描自身文本, 对本表登记/释放引用。
//
// 注意: 码点会在应用生命周期内**复用**, 因此 key 必须能唯一标识一张图
// (例如文件路径 / 截图 id), 不能只用序号, 否则不同内容会撞到同一码点。
class JZTextImageMap
{
public:
    // 私有使用区 (Plane 15) 起点。U+F0000..U+FFFFD 为补充私有使用区 A,
    // 普通字体必然没有字形, 因此不会被机器上任何字体"意外渲染出字"。
    static const char32_t kPuaBase = 0xF0000;
    static const char32_t kPuaEnd = 0xFFFFD;

    struct Entry {
        std::string key;        // 语义标识 (幂等分配的依据)
        JZImage     image;      // 图片像素 (可能为空: 只有尺寸的占位)
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
