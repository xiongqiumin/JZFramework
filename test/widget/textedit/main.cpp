// 图文混排 (私有域码点占位) 回归测试
//
// 设计 (2026-09-10): 图片在文本流里占**一个私有域码点** (U+F0000..),
// 由进程级单例 JZTextImageMap 按 key 幂等分配, 并维护引用计数。
// 因为图片就是普通字符, 所以:
//   - 视觉长度 == 码点个数 (图片算 1);
//   - 方向键一步跨过整张图; 退格/Delete 整段删掉该码点;
//   - 光标可以停在图片前后, 但不会"停在图片内部"(不存在内部)。
#include "JZTest.h"
#include "JZApplication.h"
#include "JZTextEdit.h"
#include "JZTextImageMap.h"
#include "JZEmojiImageMap.h"
#include "JZImage.h"
#include "JZKeyEvent.h"
#include "JZString.h"
#include "JZUtf8Iterator.h"

#include <memory>
#include <string>
#include <vector>

namespace {

JZImage solidImage(int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (int i = 0; i < w * h; ++i) {
        px[static_cast<size_t>(i) * 4 + 0] = r;
        px[static_cast<size_t>(i) * 4 + 1] = g;
        px[static_cast<size_t>(i) * 4 + 2] = b;
        px[static_cast<size_t>(i) * 4 + 3] = 255;
    }
    return JZImage::fromRGBA(px.data(), w, h);
}

// 构造 emoji 的 UTF-8 字节序列。
// 注意: **不要**在源码里直接写 "\U0001F44D" —— MSVC 对非 UTF-8(BOM) 源文件里
// 的 \U 转义处理不一致, 可能被静默丢弃。这里显式按字节拼, 与文件编码无关。
std::string utf8Emoji(char32_t cp)
{
    std::string out;
    JZUtf8Iterator::encode(cp, out);
    return out;
}
const std::string kThumb = utf8Emoji(0x1F44D);        // 👍 (4 字节)
const std::string kHeart = utf8Emoji(0x2764) + utf8Emoji(0xFE0F);  // ❤️ (3+3 字节)

// keyPressEvent 是 protected (正常由消息层投递), 测试里用子类开一个口子
class TestTextEdit : public JZTextEdit
{
public:
    explicit TestTextEdit(JZWidget* parent = nullptr) : JZTextEdit(parent) {}

    void sendKey(uint32_t key)
    {
        JZKeyEvent ev(key, 0);
        keyPressEvent(&ev);
    }

    // 在光标处插入一段文字 (走与真实输入相同的链路)
    void insertTextForTest(const std::string& s)
    {
        JZKeyEvent ev(0, 0, s);
        keyPressEvent(&ev);
    }

    // 暴露 protected/private 的测宽与取图入口, 供测试断言
    int xOfIndexForTest(int line, int partCount) const
    {
        return xOfIndex(line, partCount);
    }
    bool segmentImageForTest(const JZAbstractTextEdit::Segment& seg,
                             const JZImage*& out, int& w, int& h) const
    {
        return segmentImage(seg, out, w, h);
    }
};

// ---- 1. 纯文本行为不回退 ----
bool plain_text_unchanged()
{
    TestTextEdit e;
    e.setText("abcdef");
    JZ_TEST_EQ(std::string("abcdef"), e.text());
    JZ_TEST_EQ(6, e.cursorPosition());

    e.sendKey(JZKey::Key_Backspace);            // 末尾退格删 1 码点
    JZ_TEST_EQ(std::string("abcde"), e.text());
    JZ_TEST_EQ(5, e.cursorPosition());

    e.setCursorPosition(0);
    e.sendKey(JZKey::Key_Delete);               // 行首 Delete 删 1 码点
    JZ_TEST_EQ(std::string("bcde"), e.text());
    return true;
}

// ---- 2. 插入图片: 文本里出现一个私有域码点 ----
bool append_image_adds_one_codepoint()
{
    TestTextEdit e;
    e.setText("hi");

    const JZImage img = solidImage(8, 8, 255, 0, 0);
    e.appendImage(img, "key_a", 10, 10);

    // 视觉长度 = "hi" (2) + 图片 (1) = 3
    JZ_TEST_EQ(3, e.cursorPosition());
    JZ_TEST_EQ(1, e.inlineImageCount());

    // 文本里确实有一个私有域码点
    const JZString t = JZString::fromStdString(e.text());
    JZ_TEST_EQ(3, t.size());
    JZ_TEST_TRUE(JZTextImageMap::isImageCode(t[2].toUnicode()));
    JZ_TEST_TRUE(t[0].toUnicode() == 'h');
    return true;
}

// ---- 3. 图片只占 1 个下标: 左右键一步跨过 ----
bool image_counts_as_one_unit()
{
    TestTextEdit e;
    e.setText("ab");
    e.appendImage(solidImage(8, 8, 0, 255, 0), "key_b", 12, 12);

    // 视觉: a b [img] -> 总长 3
    JZ_TEST_EQ(3, e.cursorPosition());

    e.sendKey(JZKey::Key_Left);                 // 跨过整张图 -> 2
    JZ_TEST_EQ(2, e.cursorPosition());

    e.sendKey(JZKey::Key_Left);                 // 普通字符之间 -> 1
    JZ_TEST_EQ(1, e.cursorPosition());

    e.sendKey(JZKey::Key_Right);                // 再回来 -> 2
    JZ_TEST_EQ(2, e.cursorPosition());
    return true;
}

// ---- 4. 退格整段删掉图片码点 ----
bool backspace_deletes_image()
{
    TestTextEdit e;
    e.setText("ab");
    e.appendImage(solidImage(8, 8, 0, 0, 255), "key_c", 12, 12);
    JZ_TEST_EQ(1, e.inlineImageCount());

    e.sendKey(JZKey::Key_Backspace);            // 一次退格 -> 图片整个消失

    JZ_TEST_EQ(std::string("ab"), e.text());
    JZ_TEST_EQ(0, e.inlineImageCount());
    JZ_TEST_EQ(2, e.cursorPosition());

    e.sendKey(JZKey::Key_Backspace);            // 再退才是普通字符
    JZ_TEST_EQ(std::string("a"), e.text());
    return true;
}

// ---- 5. Delete 整段删掉图片码点 ----
bool delete_removes_image()
{
    TestTextEdit e;
    e.setText("ab");
    e.appendImage(solidImage(8, 8, 0, 255, 255), "key_d", 12, 12);

    e.setCursorPosition(2);                     // 图片之前
    JZ_TEST_EQ(2, e.cursorPosition());
    e.sendKey(JZKey::Key_Delete);

    JZ_TEST_EQ(std::string("ab"), e.text());
    JZ_TEST_EQ(0, e.inlineImageCount());
    return true;
}

// ---- 6. 撤销 / 重做把图片当一步整体还原 ----
bool undo_redo_image()
{
    TestTextEdit e;
    e.setText("ab");
    e.appendImage(solidImage(8, 8, 255, 255, 0), "key_e", 12, 12);
    JZ_TEST_EQ(1, e.inlineImageCount());

    e.sendKey(JZKey::Key_Backspace);            // 删掉整张图
    JZ_TEST_EQ(std::string("ab"), e.text());
    JZ_TEST_EQ(0, e.inlineImageCount());

    e.undo();                                   // 撤销 -> 图回来
    JZ_TEST_EQ(std::string("ab"), e.text().substr(0, 2));
    JZ_TEST_EQ(1, e.inlineImageCount());

    e.redo();                                   // 重做 -> 图再消失
    JZ_TEST_EQ(std::string("ab"), e.text());
    JZ_TEST_EQ(0, e.inlineImageCount());
    return true;
}

// ---- 7. 光标可停在图片前后 (每个下标都合法且往返一致) ----
bool cursor_positions_roundtrip()
{
    TestTextEdit e;
    e.setText("ab");
    e.appendImage(solidImage(8, 8, 10, 10, 10), "key_f", 12, 12);

    const int total = e.cursorPosition();
    JZ_TEST_EQ(3, total);

    for (int v = 0; v <= total; ++v) {
        e.setCursorPosition(v);
        JZ_TEST_EQ(v, e.cursorPosition());
    }

    e.setCursorPosition(9999);                  // 越界 -> clamp 到末尾
    JZ_TEST_EQ(total, e.cursorPosition());
    return true;
}

// ---- 8. toPlainText: 图片码点 -> "[图片]" ----
bool plain_text_replaces_image()
{
    TestTextEdit e;
    e.setText("see ");
    e.appendImage(solidImage(8, 8, 0, 0, 0), "key_g", 10, 10);

    const std::string plain = e.toPlainText();
    JZ_TEST_EQ(std::string("see [图片]"), plain);   // 图片恰好一个

    // 含码点的原文本长度 = "see " + 1
    JZ_TEST_EQ(5, static_cast<int>(JZString::fromStdString(e.toMarkedText()).size()));
    return true;
}

// ---- 9. 同一 key 幂等: 二次插入复用同一码点 ----
bool same_key_is_idempotent()
{
    TestTextEdit e;
    e.setText("x");

    const JZImage img = solidImage(8, 8, 1, 2, 3);
    e.appendImage(img, "shared_key", 10, 10);
    const JZString t1 = JZString::fromStdString(e.text());
    const char32_t cp1 = t1[1].toUnicode();

    e.appendImage(img, "shared_key", 10, 10);   // 同 key 再插
    const JZString t2 = JZString::fromStdString(e.text());
    JZ_TEST_EQ(3, t2.size());
    // 两处引用的是**同一**码点
    JZ_TEST_EQ(true, t2[1].toUnicode() == cp1);
    JZ_TEST_EQ(true, t2[2].toUnicode() == cp1);
    JZ_TEST_EQ(2, e.inlineImageCount());
    return true;
}

// ---- 10. 中间插入图片: 前后文本不丢 ----
bool insert_image_in_middle()
{
    TestTextEdit e;
    e.setText("abcd");
    e.setCursorPosition(2);
    e.insertImage(solidImage(8, 8, 200, 100, 50), "key_h", 12, 12);

    // 插入后文本结构: a b [img] c d, 光标停在图片之后 (下标 3)
    JZ_TEST_EQ(3, e.cursorPosition());
    const JZString t = JZString::fromStdString(e.text());
    JZ_TEST_EQ(5, t.size());
    JZ_TEST_TRUE(t[0].toUnicode() == 'a');
    JZ_TEST_TRUE(t[1].toUnicode() == 'b');
    JZ_TEST_TRUE(JZTextImageMap::isImageCode(t[2].toUnicode()));
    JZ_TEST_TRUE(t[3].toUnicode() == 'c');
    JZ_TEST_TRUE(t[4].toUnicode() == 'd');

    // 光标停在图片之后 (下标 3), 退格整段删图
    e.setCursorPosition(3);
    e.sendKey(JZKey::Key_Backspace);
    JZ_TEST_EQ(std::string("abcd"), e.text());
    return true;
}

// ---- 11. 引用计数: 文本删空后图片可被回收 ----
bool refcount_released_after_removal()
{
    JZTextImageMap& map = JZTextImageMap::instance();

    {
        TestTextEdit e;
        e.setText("z");
        e.appendImage(solidImage(8, 8, 5, 5, 5), "ref_key", 10, 10);

        const char32_t cp = JZString::fromStdString(e.text())[1].toUnicode();
        JZ_TEST_EQ(1, map.refCount(cp));

        // 删掉图片 -> 引用计数归零
        e.setCursorPosition(2);
        e.sendKey(JZKey::Key_Backspace);
        JZ_TEST_EQ(0, map.refCount(cp));
    }
    return true;
}

// ---- 12. 段落切分: 图片单独成段, 连续文字合并为一段 (字节坐标) ----
bool segments_split_text_and_image()
{
    TestTextEdit e;
    e.setText("ab");
    e.appendImage(solidImage(8, 8, 1, 1, 1), "seg_key", 12, 12);   // ab[img]
    e.setCursorPosition(3);
    e.insertTextForTest("cd");                                     // ab[img]cd

    const std::vector<JZAbstractTextEdit::Segment> segs = e.enumerateSegments();
    JZ_TEST_EQ(3, static_cast<int>(segs.size()));

    // [0] 文字 "ab"  (字节 [0,2))
    JZ_TEST_EQ(true, segs[0].type == JZAbstractTextEdit::SegmentType::Text);
    JZ_TEST_EQ(0, segs[0].start);
    JZ_TEST_EQ(2, segs[0].size);
    JZ_TEST_EQ(std::string("ab"), segs[0].text);

    // [1] 图片 (不可分割, 占 4 字节: Plane 15 私有域码点)
    JZ_TEST_EQ(true, segs[1].isImage());
    JZ_TEST_EQ(2, segs[1].start);
    JZ_TEST_EQ(4, segs[1].size);
    JZ_TEST_EQ(false, segs[1].image.isNull());

    // [2] 文字 "cd"  (字节 [6,8))
    JZ_TEST_EQ(true, segs[2].type == JZAbstractTextEdit::SegmentType::Text);
    JZ_TEST_EQ(6, segs[2].start);
    JZ_TEST_EQ(2, segs[2].size);
    JZ_TEST_EQ(std::string("cd"), segs[2].text);

    // 完整覆盖: 首尾相接, 无空洞无重叠
    JZ_TEST_EQ(0, segs.front().start);
    for (size_t k = 1; k < segs.size(); ++k)
        JZ_TEST_EQ(segs[k - 1].start + segs[k - 1].size, segs[k].start);
    JZ_TEST_EQ(8, segs.back().start + segs.back().size);
    return true;
}

// ---- 12b. emoji 单独成段, 且修饰符并入同一段 ----
bool segments_split_emoji()
{
    TestTextEdit e;
    // "a👍b ❤️" —— 👍 4 字节;  ❤️ = U+2764 U+FE0F (3+3 字节)
    e.setText(std::string("a") + kThumb + "b " + kHeart);

    const std::vector<JZAbstractTextEdit::Segment> segs = e.enumerateSegments();

    // 期望: [Text "a"] [Emoji "👍"] [Text "b "] [Emoji "❤️"]
    JZ_TEST_EQ(4, static_cast<int>(segs.size()));

    JZ_TEST_EQ(true, segs[0].type == JZAbstractTextEdit::SegmentType::Text);
    JZ_TEST_EQ(std::string("a"), segs[0].text);

    JZ_TEST_EQ(true, segs[1].isEmoji());
    JZ_TEST_EQ(4, segs[1].size);                       // 👍 单码点 4 字节
    JZ_TEST_EQ(kThumb, segs[1].emoji);

    JZ_TEST_EQ(true, segs[2].type == JZAbstractTextEdit::SegmentType::Text);
    JZ_TEST_EQ(std::string("b "), segs[2].text);

    // ❤️: U+2764 (3 字节) + VS16 U+FE0F (3 字节) 必须并入**同一** emoji 段
    JZ_TEST_EQ(true, segs[3].isEmoji());
    JZ_TEST_EQ(6, segs[3].size);
    JZ_TEST_EQ(kHeart, segs[3].emoji);
    return true;
}

// ---- 12c. 字节坐标与码点坐标互转 ----
bool byte_codepoint_mapping()
{
    TestTextEdit e;
    e.setText("a" + kThumb + "bc");   // a(1B) 👍(4B) b(1B) c(1B)

    // 字节 -> 码点
    JZ_TEST_EQ(0, e.utf8ByteToCodepoint(0));   // 'a'
    JZ_TEST_EQ(1, e.utf8ByteToCodepoint(1));   // 👍
    JZ_TEST_EQ(2, e.utf8ByteToCodepoint(5));   // 'b'
    JZ_TEST_EQ(3, e.utf8ByteToCodepoint(6));   // 'c'
    JZ_TEST_EQ(4, e.utf8ByteToCodepoint(7));   // 末尾

    // 码点 -> 字节
    JZ_TEST_EQ(0, e.codepointToUtf8Byte(0));
    JZ_TEST_EQ(1, e.codepointToUtf8Byte(1));
    JZ_TEST_EQ(5, e.codepointToUtf8Byte(2));
    JZ_TEST_EQ(6, e.codepointToUtf8Byte(3));
    JZ_TEST_EQ(7, e.codepointToUtf8Byte(4));
    return true;
}

// ---- 13. 光标按段落移动: 图片整格跨过 ----
bool move_cursor_by_segment()
{
    TestTextEdit e;
    e.setText("ab");
    e.appendImage(solidImage(8, 8, 2, 2, 2), "seg_key2", 12, 12);  // ab[img]
    e.setCursorPosition(3);
    e.insertTextForTest("cd");                                     // ab[img]cd

    // 停靠点: 0(a) 2(图) 3(c) 5(末尾)
    e.setCursorPosition(5);

    JZ_TEST_TRUE(e.moveCursorBySegment(-1, false));   // -> 3 (cd 起点)
    JZ_TEST_EQ(3, e.cursorPosition());

    JZ_TEST_TRUE(e.moveCursorBySegment(-1, false));   // -> 2 (图片起点, 一步跨过 "ab")
    JZ_TEST_EQ(2, e.cursorPosition());

    JZ_TEST_TRUE(e.moveCursorBySegment(-1, false));   // -> 0 (ab 起点)
    JZ_TEST_EQ(0, e.cursorPosition());

    JZ_TEST_TRUE(!e.moveCursorBySegment(-1, false));  // 已在最前, 不再移动
    JZ_TEST_EQ(0, e.cursorPosition());

    // 向右: 图片一步跨到 3 (而不是停在图片"里面" —— 图片无内部)
    JZ_TEST_TRUE(e.moveCursorBySegment(1, false));    // -> 2
    JZ_TEST_EQ(2, e.cursorPosition());
    JZ_TEST_TRUE(e.moveCursorBySegment(1, false));    // -> 3
    JZ_TEST_EQ(3, e.cursorPosition());
    JZ_TEST_TRUE(e.moveCursorBySegment(1, false));    // -> 5
    JZ_TEST_EQ(5, e.cursorPosition());
    JZ_TEST_TRUE(!e.moveCursorBySegment(1, false));   // 已在末尾
    return true;
}

// ---- 14. 段落序号查询与按序号跳转 ----
bool segment_index_and_jump()
{
    TestTextEdit e;
    e.setText("xy");
    e.appendImage(solidImage(8, 8, 3, 3, 3), "seg_key3", 12, 12);  // xy[img]

    JZ_TEST_EQ(0, e.segmentIndexOfCursor(0));    // "xy" 段
    JZ_TEST_EQ(0, e.segmentIndexOfCursor(1));    // 段内
    JZ_TEST_EQ(1, e.segmentIndexOfCursor(2));    // 图片段
    JZ_TEST_EQ(2, e.segmentIndexOfCursor(3));    // 末尾插入点

    e.moveCursorToSegment(0, false);
    JZ_TEST_EQ(0, e.cursorPosition());
    e.moveCursorToSegment(1, false);
    JZ_TEST_EQ(2, e.cursorPosition());
    e.moveCursorToSegment(2, false);
    JZ_TEST_EQ(3, e.cursorPosition());
    e.moveCursorToSegment(99, false);            // 越界 -> clamp 到末尾
    JZ_TEST_EQ(3, e.cursorPosition());
    return true;
}

// ---- 15. 纯文本: 整段合并为 1 个 Text 段 ----
bool plain_text_single_segment()
{
    TestTextEdit e;
    e.setText("hello world");
    const std::vector<JZAbstractTextEdit::Segment> segs = e.enumerateSegments();
    JZ_TEST_EQ(1, static_cast<int>(segs.size()));
    JZ_TEST_EQ(11, segs[0].size);
    JZ_TEST_EQ(0, segs[0].start);
    JZ_TEST_EQ(true, segs[0].type == JZAbstractTextEdit::SegmentType::Text);

    // 空文本: 无段落, 但仍有 1 个停靠点 (末尾插入点)
    TestTextEdit e2;
    e2.setText("");
    JZ_TEST_EQ(0, static_cast<int>(e2.enumerateSegments().size()));
    return true;
}

// ---- 16. emoji 在光标语义上整体跨过 (一步一个 emoji, 不拆代理对) ----
bool emoji_cursor_whole_segment()
{
    TestTextEdit e;
    e.setText("a" + kThumb + "b");   // a 👍 b -> 码点: 0,1,2

    // 停靠点: 0(a) 1(👍) 2(b) 3(末尾)
    e.setCursorPosition(3);

    JZ_TEST_TRUE(e.moveCursorBySegment(-1, false));   // -> 2 (b)
    JZ_TEST_EQ(2, e.cursorPosition());
    JZ_TEST_TRUE(e.moveCursorBySegment(-1, false));   // -> 1 (👍, 一步跨过整个 emoji)
    JZ_TEST_EQ(1, e.cursorPosition());
    JZ_TEST_TRUE(e.moveCursorBySegment(-1, false));   // -> 0 (a)
    JZ_TEST_EQ(0, e.cursorPosition());
    JZ_TEST_TRUE(!e.moveCursorBySegment(-1, false));

    // 段落序号: 3 个段落 + 末尾哨兵
    JZ_TEST_EQ(0, e.segmentIndexOfCursor(0));
    JZ_TEST_EQ(1, e.segmentIndexOfCursor(1));
    JZ_TEST_EQ(2, e.segmentIndexOfCursor(2));
    JZ_TEST_EQ(3, e.segmentIndexOfCursor(3));
    return true;
}

// ---- 19. emoji 位图: 内置资源能查到并解码 ----
bool emoji_image_available()
{
    // 👍 U+1F44D 一定在常用集里
    const JZImage* img = JZEmojiImageMap::instance().find(kThumb);
    JZ_TEST_TRUE(img != nullptr);
    if (!img)
        return true;                       // 断言已失败, 不继续解引用
    JZ_TEST_TRUE(!img->isNull());
    JZ_TEST_TRUE(img->width() > 0);
    JZ_TEST_TRUE(img->height() > 0);

    // ❤️ (U+2764 U+FE0F) 也应有独立资源
    const JZImage* heart = JZEmojiImageMap::instance().find(kHeart);
    JZ_TEST_TRUE(heart != nullptr && !heart->isNull());
    return true;
}

// ---- 20. emoji 段能产出可绘制图像 (宽度 > 0) ----
bool emoji_segment_has_image()
{
    TestTextEdit e;
    e.setText("a" + kThumb + "b");

    const std::vector<JZAbstractTextEdit::Segment> segs = e.enumerateSegments();
    JZ_TEST_EQ(3, static_cast<int>(segs.size()));
    JZ_TEST_EQ(true, segs[1].isEmoji());

    const JZImage* img = nullptr;
    int w = 0, h = 0;
    const bool ok = e.segmentImageForTest(segs[1], img, w, h);
    JZ_TEST_TRUE(ok);
    JZ_TEST_TRUE(img != nullptr && !img->isNull());
    JZ_TEST_GT(w, 0);
    JZ_TEST_GT(h, 0);
    return true;
}

// ---- 21. emoji 计入行宽, 且大于纯文本情况 ----
bool emoji_contributes_to_width()
{
    TestTextEdit a;
    a.setText("x");

    TestTextEdit b;
    b.setText("x" + kThumb);

    // 含 emoji 的行宽必须更大
    JZ_TEST_GT(b.xOfIndexForTest(0, 2), a.xOfIndexForTest(0, 1));

    // emoji 一格宽 == 其显示宽
    const int wEmoji = b.xOfIndexForTest(0, 2) - b.xOfIndexForTest(0, 1);
    JZ_TEST_GT(wEmoji, 0);
    return true;
}

// ---- 22. 未收录的 emoji: 不崩, 且退回字体绘制 (宽度按字体度量) ----
bool unknown_emoji_is_safe()
{
    TestTextEdit e;
    // U+1FAE8 (🫨) 大概率不在常用集里
    e.setText("a" + utf8Emoji(0x1FAE8) + "b");

    const std::vector<JZAbstractTextEdit::Segment> segs = e.enumerateSegments();
    JZ_TEST_EQ(3, static_cast<int>(segs.size()));
    JZ_TEST_EQ(true, segs[1].isEmoji());

    // 图集里没有 -> segmentImage 返回 false, 但布局与光标必须仍然可用
    const JZImage* img = nullptr;
    int w = 0, h = 0;
    const bool ok = e.segmentImageForTest(segs[1], img, w, h);
    JZ_TEST_FALSE(ok);

    e.setCursorPosition(3);
    JZ_TEST_TRUE(e.moveCursorBySegment(-1, false));   // 一步跨过未知 emoji
    JZ_TEST_EQ(2, e.cursorPosition());
    return true;
}

static void test_textedit(JZTest* test)
{
    auto suite = std::make_shared<JZTestSuite>("JZTextEditInlineImage");
    test->addSuite(suite);
    suite->addTestCase("plain_text_unchanged",         plain_text_unchanged);
    suite->addTestCase("append_image_adds_one_codepoint", append_image_adds_one_codepoint);
    suite->addTestCase("image_counts_as_one_unit",     image_counts_as_one_unit);
    suite->addTestCase("backspace_deletes_image",      backspace_deletes_image);
    suite->addTestCase("delete_removes_image",         delete_removes_image);
    suite->addTestCase("undo_redo_image",              undo_redo_image);
    suite->addTestCase("cursor_positions_roundtrip",   cursor_positions_roundtrip);
    suite->addTestCase("plain_text_replaces_image",    plain_text_replaces_image);
    suite->addTestCase("same_key_is_idempotent",       same_key_is_idempotent);
    suite->addTestCase("insert_image_in_middle",       insert_image_in_middle);
    suite->addTestCase("refcount_released_after_removal", refcount_released_after_removal);
    suite->addTestCase("segments_split_text_and_image", segments_split_text_and_image);
    suite->addTestCase("segments_split_emoji",         segments_split_emoji);
    suite->addTestCase("byte_codepoint_mapping",       byte_codepoint_mapping);
    suite->addTestCase("move_cursor_by_segment",       move_cursor_by_segment);
    suite->addTestCase("segment_index_and_jump",       segment_index_and_jump);
    suite->addTestCase("plain_text_single_segment",    plain_text_single_segment);
    suite->addTestCase("emoji_cursor_whole_segment",   emoji_cursor_whole_segment);
    suite->addTestCase("emoji_image_available",        emoji_image_available);
    suite->addTestCase("emoji_segment_has_image",      emoji_segment_has_image);
    suite->addTestCase("emoji_contributes_to_width",   emoji_contributes_to_width);
    suite->addTestCase("unknown_emoji_is_safe",        unknown_emoji_is_safe);
}

} // namespace

int main(int argc, char* argv[])
{
    JZApplication app;

    JZTest test;
    test_textedit(&test);
    return test.run(argc, argv);
}
