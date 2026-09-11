#ifndef JZ_EMOJI_IMAGE_MAP_H_
#define JZ_EMOJI_IMAGE_MAP_H_

#include <map>
#include <string>

#include "JZImage.h"
#include "JZPixmap.h"

// ==================== Emoji 位图注册表 (单例) ====================
//
// 为什么不用字体渲染 emoji:
//   Windows 的 Segoe UI Emoji 走 COLR/CPAL (矢量分层), **GDI+ 不支持彩色**,
//   只能画出单色轮廓; 而彩色位图字体 (Noto 的 CBDT / Apple 的 sbix) 系统里没有。
//   因此这里改用**内置 PNG 图集** (Twemoji 72x72, 见 assets/emoji/), 通过
//   JZResources 打包进程序, 按需解码 + 缓存, 走与内联图片完全相同的
//   "码点 -> JZImage -> drawImage" 路径。
//
// 命名约定 (与 Twemoji 一致, **不需要映射表**):
//   emoji 文本 -> 各码点小写十六进制, 用 '-' 连接, 作为资源名。
//      👍          U+1F44D                  -> "1f44d"
//      ❤️          U+2764 U+FE0F            -> "2764-fe0f"
//      🇨🇳         U+1F1E8 U+1F1F3          -> "1f1e8-1f1f3"
//      👨‍👩‍👧        U+1F468 200D 1F469 200D 1F467 -> "1f468-200d-1f469-200d-1f467"
//
// 查找策略: 先用**完整序列**找, 找不到再退回**首个码点** (例如带变体选择符
// 的组合找不到时, 至少能画基字符)。都没有则返回 nullptr, 调用方降级为占位。
//
// 线程/生命周期: 进程级单例, 只增不减 (emoji 数量有限, 与文档无关);
// 首次查找时才解码对应 PNG, 因此不用的 emoji 不占内存。
//
// 两级缓存: m_cache 存**解码后的原图** (JZImage, 72x72), m_pixmaps 存按显示尺寸
// 处理好的**显示用位图** (JZPixmap)。文本绘制走后者 (见 findPixmap)。
class JZEmojiImageMap
{
public:
    static JZEmojiImageMap& instance();

    // emoji 文本 (UTF-8, 可能含 ZWJ/VS16/肤色等修饰符) -> 原图。
    // 未命中返回 nullptr (调用方自行降级, 例如画占位框或退回字体绘制)。
    const JZImage* find(const std::string& emoji);

    // 由码点序列直接查找 (与 find 等价, 避免调用方自己拼 UTF-8)
    const JZImage* findByCodepoints(const std::vector<char32_t>& cps);

    // ---- 显示用位图 (按显示尺寸缓存) ----
    // 原图是图集里的 72x72, 而文本里通常只显示到 ~18px。若每帧现缩现转,
    // 一屏几十个 emoji 就是几十次重采样 —— 所以这里按 (emoji, 尺寸) 缓存
    // "已缩放 + 已转成后端原生布局"的位图, 绘制时直接贴。
    //   boxSize: 目标显示边长 (逻辑像素, 正方形); <= 0 时取原图边长。
    // 未收录该 emoji 时返回 nullptr (调用方降级为字体绘制/占位)。
    const JZPixmap* findPixmap(const std::string& emoji, int boxSize);
    const JZPixmap* findPixmap(const std::vector<char32_t>& cps, int boxSize);

    // 内置资源名 (如 "1f44d"); 便于测试与调试
    static std::string resourceKey(const std::vector<char32_t>& cps);
    static std::string resourceKey(const std::string& emojiUtf8);

    // 清空缓存 (测试用)
    void clear();

    int cachedCount() const { return static_cast<int>(m_cache.size()); }
    int pixmapCount() const { return static_cast<int>(m_pixmaps.size()); }

private:
    JZEmojiImageMap() = default;
    JZEmojiImageMap(const JZEmojiImageMap&) = delete;
    JZEmojiImageMap& operator=(const JZEmojiImageMap&) = delete;

    // 实际加载: 先完整序列, 再退化到首码点
    const JZImage* load(const std::vector<char32_t>& cps);

    struct CacheEntry {
        JZImage image;      // 可能为 isNull() (表示"确认没有这张图")
        bool    resolved = false;
    };
    std::map<std::string, CacheEntry> m_cache;

    // "<资源名>@<显示边长>" -> 显示用位图 (同一个 emoji 在不同字号下各占一条)
    std::map<std::string, JZPixmap> m_pixmaps;
};

#endif // JZ_EMOJI_IMAGE_MAP_H_
