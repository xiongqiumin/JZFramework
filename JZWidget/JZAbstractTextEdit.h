#ifndef JZ_ABSTRACT_TEXT_EDIT_H_
#define JZ_ABSTRACT_TEXT_EDIT_H_

#include <map>
#include <string>
#include <vector>
#include "JZWidget.h"
#include "JZFont.h"
#include "JZImage.h"
#include "JZEmoji.h"
#include "JZString.h"
#include "JZSignal.h"
#include "JZTimer.h"

// ==================== 文本编辑控件公共实现 ====================
// 单行 (JZLineEdit) 与多行 (JZTextEdit) 文本编辑的全部逻辑集中在本类:
// 文本/光标/选区、行布局、鼠标与键盘编辑、IME 组字、剪贴板、绘制、
// 以及平台 IME 窗口 (候选条) 定位。两个派生类只提供各自的语义入口
// (password/maxLength/lineWrap/默认尺寸) 与信号含义, 不再承载实现。
//
// 单行/多行的差异被 m_singleLine 收敛为少量分支, 以"行模型"统一:
//   - 行布局: 单行恒为一行 (文本入口把换行替换成空格, 保证不变量);
//   - 垂直:   仅多行参与垂直滚动/上下移动; 单行的"首行 y"取控件高度
//             内垂直居中, 与旧 JZLineEdit 的 centeredTextTop 逐像素一致;
//   - 回车:   单行发 sigReturnPressed/sigEditingFinished, 多行插入 '\n';
//   - 上/下键: 多行移动光标, 单行交回 (event->ignore(), 让父级滚动)。
//   - 密码:   仅单行有; 所有测宽/绘制/点击定位统一走 displayString 掩码。
//
// 索引约定: 内部一律按**码点**索引 (JZString); 而 IME 给的下标 (GCS_CURSORPOS、
// 组字属性数组) 是 UTF-16 单元, 两者在非 BMP 字符上会错位, 换算见 JZImeEvent。
//
// 图文混排 (2026-09-10): 图片占**一个私有域码点** (U+F0000.., 见
// JZTextImageMap), 因此它就是普通字符 —— 光标/退格/选区/前缀测宽全部天然按
// 1 个单元处理, 不需要标记语法, 也不需要"视觉下标"这层换算。
// m_text 仍是唯一文本载体 (可持久化/剪贴板/AI 读取), 图片像素登记在
// JZTextImageMap 单例里, 按 key 幂等分配码点。
//
// 段落语义 (2026-09-10 追加): 光标以一个**段落 (JZTextSegment)** 为单位移动,
// 段落有两种语义:
//   - Text  : 连续可渲染文字的**同质游程** (同字体/同 emoji 回退族), 宽度按字体度量;
//   - Image : 一个私有域码点, 宽度取注册表, 是**不可分割的 1 格**。
// 段落的切分是"光标移动 + 渲染 + 测宽"三者的**共同口径** —— 三处都必须走
// enumerateSegments(), 否则会出现"光标跨不过图 / 点击命中偏 / emoji 宽度错"。
// 有了段落语义后, emoji 回退字体只需要在 Text 段落上按"该段是否有字形"选择,
// 不必在光标/命中层面再引入视觉下标。
//
// 与消息层的关系: 本类只提供事件入口 (keyPressEvent / imeEvent / focusXxxEvent),
// 由平台消息层把 WM_KEY* / WM_IME_* 翻译后投递进来。未接消息层时控件能正常
// 显示, 只是无法输入。

class JZUndoStack;

class JZAbstractTextEdit : public JZWidget
{
    JZ_OBJECT_DECLARE(JZAbstractTextEdit)

public:
    ~JZAbstractTextEdit() override;

    // ---- 文本 (UTF-8 std::string) ----
    void setText(const std::string& text);
    std::string text() const;

    void setPlaceholderText(const std::string& text);
    std::string placeholderText() const;

    void setReadOnly(bool readOnly);
    bool isReadOnly() const;

    // ---- 图文混排 (内联图片) ----
    // 图片在文本流里占**一个私有域码点** (U+F0000.., 见 JZTextImageMap)。
    // 于是它就是一个普通字符: 光标/退格/选区/前缀测宽全部天然按 1 个单元处理,
    // 无需特殊标记语法。像素与显示尺寸存在 JZTextImageMap 单例里, 按 key 幂等
    // 分配码点, 并由本控件在 relayout 时维护引用计数。
    //
    // appendImage: 在文本末尾追加一张图。
    // insertImage: 在光标处 (有选区则替换选区) 插入。
    // key 用于标识图片 (如文件路径 / 截图 id), 为空时自动生成唯一 key。
    // 插入失败 (图片为空且未给尺寸 / 只读) 时静默无操作。
    void appendImage(const JZImage& image, const std::string& key = std::string(),
                     int width = 0, int height = 0);
    void insertImage(const JZImage& image, const std::string& key = std::string(),
                     int width = 0, int height = 0);

    // 当前文本里出现的图片码点个数 (调试/测试用)
    int  inlineImageCount() const;

    // ---- 段落语义 (光标/渲染/测宽的共同口径) ----
    // 段落类型:
    //   Text  : 一段可渲染文字 (同质游程, 走字体度量);
    //   Image : 内联图片 (占一个私有域码点, 走 JZTextImageMap 的显示尺寸);
    //   Emoji : emoji 码点 (含 VS16/ZWJ/肤色等修饰符组合), 走字体回退族,
    //           且**整体不可分割** (光标一步跨过, 退格整段删除)。
    enum class SegmentType {
        Text,
        Image,
        Emoji,
    };

    // 一个段落。
    //   start/size 是 **UTF-8 字节下标 / 字节长度** —— 与 text()/setText() 同一
    //   坐标系统, 外部可直接拿它对 std::string 做 substr。
    //   不变量: 段落按 start 递增且**完整覆盖** [from, from+len) 字节区间,
    //           无空洞、无重叠; 每段 size >= 1 (不可为空段)。
    //   text / image / emoji 按 type 取用其一:
    //     Text  -> text  (该段的 UTF-8 文本)
    //     Image -> image (该段的图片像素; 可能为空 = 只有尺寸的占位)
    //     Emoji -> emoji (该段的 emoji 文本, 含修饰符, 如 "👍" / "❤️")
    struct Segment {
        SegmentType type = SegmentType::Text;
        int  start = 0;                 // utf8 index (起始字节下标)
        int  size = 0;                  // utf8 size  (字节长度)

        std::string text;               // type == Text
        JZImage     image;              // type == Image
        std::string emoji;              // type == Emoji

        bool isImage() const { return type == SegmentType::Image; }
        bool isEmoji() const { return type == SegmentType::Emoji; }
    };

    // 把 [from, from+len) 的**字节区间**文本切成段落序列。len < 0 表示到末尾。
    // 切分规则:
    //   - 图片码点 -> 独立 Image 段;
    //   - emoji 码点 (含其后的 VS16/ZWJ/肤色修饰符) -> 独立 Emoji 段;
    //   - 其余连续普通码点 -> 合并成一个 Text 段。
    std::vector<Segment> enumerateSegments(int from = 0, int len = -1) const;

    // UTF-8 字节下标 <-> 码点下标。
    // 段落用**字节**坐标 (与 text()/setText() 一致), 而 cursorPosition()/
    // setCursorPosition() 用**码点**坐标; 两者互转需要这两个换算。
    // 字节下标不是码点起点时, 向下取到该码点的起点。
    int  utf8ByteToCodepoint(int byteOffset) const;
    int  codepointToUtf8Byte(int cpIndex) const;

    // 光标位置所在的**段落序号** (0..段数), 落在段落起点则返回该段落;
    // 落在最后一段末尾返回段数 (即"末尾之后的插入点")。
    int segmentIndexOfCursor(int pos = -1) const;   // pos < 0 表示用当前光标

    // 按段落移动光标: dir = -1 上一个段落起点 / +1 下一个段落起点。
    // select = true 时扩展选区。返回是否发生了移动。
    bool moveCursorBySegment(int dir, bool select);
    // 跳到段落序号 idx 的起点 (idx 会被 clamp 到 [0, 段数])
    void moveCursorToSegment(int idx, bool select);

    // ---- 段落 -> 可绘制图像 (测宽与绘制共用的**唯一**入口) ----
    // Image 段取 JZTextImageMap 的图, Emoji 段取 JZEmojiImageMap 的图。
    // 返回 true 时 w/h 有效 (out 可能为空 = 只有尺寸的占位);
    // 返回 false 表示该段没有可用图像 (如图集未收录该 emoji), 调用方应降级。
    // 关键: xOfIndex 的宽度累加与 paintEvent 的绘制必须都走它, 否则光标会偏。
    bool segmentImage(const Segment& seg, const JZImage*& out,
                      int& w, int& h) const;
    // 图片/emoji 段的显示高度 (用于行高), 按字号推导
    int  inlineBoxHeight() const;

    // 单行内前 partCount 个部件的累计宽度 (相对行首, 不含 margin/scroll)。
    // 这是**唯一**允许计算"光标/命中点 x 坐标"的函数。
    int  xOfIndex(int line, int partCount) const;

    // ---- 光标与选区 ----
    int  cursorPosition() const;
    void setCursorPosition(int pos);
    bool hasSelection() const;
    void selectAll();
    void clearSelection();
    void setSelection(int start, int length);
    std::string selectedText() const;
    int  selectionStart() const;
    int  selectionLength() const;

    void setCursorBlinkPeriod(int ms);              // <=0 关闭闪烁
    void setTextMargins(int left, int top, int right, int bottom);  // 多行版本
    void setTextMargins(int left, int right);                       // 单行版本 (只改左右)

    // ---- 撤销 / 重做 (仿 Qt QUndoStack) ----
    // 合并策略: 相邻的用户编辑 (输入/删除/IME 上屏) 在 5 秒内合并为一步,
    // 超过 5 秒另起一步; 粘贴/剪切每次单独算一步, 不与相邻输入合并。
    void undo();
    void redo();
    bool canUndo() const;
    bool canRedo() const;
    void setUndoLimit(int limit);       // 0 = 不限制 (默认)
    int  undoLimit() const;
    void clearUndoHistory();            // 丢弃全部历史 (setText 内部已自动调用)

    // 可编辑 (非只读; 单行且非密码) 时需要输入法; 窗口据此关联/断开 IMC
    bool imeEnabled() const override;

    // 最小期望尺寸: 单行/多行文本框的下限, 供布局推导窗口最小尺寸
    JZSize minimumSizeHint() const override;

    // ---- AI 语义 ----
    std::string aiRole() const override { return "textbox"; }
    std::string aiText() const override { return m_text.toStdString(); }
    std::vector<JZAiAction> aiActions() const override { return {{"setText", "string 新文本"}}; }

jz_signals:
    JZSignal<const std::string&> sigTextChanged;       // 文本变化 (含 IME 上屏)
    JZSigVoid sigCursorPositionChanged;
    JZSigVoid sigReturnPressed;                        // 仅单行模式: 回车
    JZSigVoid sigEditingFinished;                      // 仅单行模式: 失焦 / 回车

protected:
    explicit JZAbstractTextEdit(JZWidget* parent, bool singleLine);

    // ---- 子类语义配置 (派生类公共 API 转调; 具体实现全在本类) ----
    void setPasswordMode(bool password);               // JZLineEdit::setPassword
    bool isPasswordMode() const { return m_password; }
    void setMaxLengthLimit(int n);                     // JZLineEdit::setMaxLength (0 = 不限)
    int  maxLengthLimit() const { return m_maxLength; }
    void setLineWrapMode(bool wrap);                   // JZTextEdit::setLineWrap
    bool isLineWrapMode() const { return m_lineWrap; }

    // ---- 事件入口: 编辑/输入逻辑全在基类, 派生类无需覆写 ----
    void paintEvent(JZPainterEvent* event) override;
    void resizeEvent(JZResizeEvent* event) override;
    void mousePressEvent(JZMouseEvent* event) override;
    void mouseReleaseEvent(JZMouseEvent* event) override;
    void mouseMoveEvent(JZMouseEvent* event) override;
    void keyPressEvent(JZKeyEvent* event) override;
    void imeEvent(JZImeEvent* event) override;
    void focusInEvent(JZEvent* event) override;
    void focusOutEvent(JZEvent* event) override;

    bool m_singleLine = false;    // true: JZLineEdit; false: JZTextEdit
    bool m_password = false;      // 仅单行; 多行恒 false
    bool m_readOnly = false;
    bool m_lineWrap = false;      // 仅多行 (resize 重建行布局用)
    int  m_maxLength = 0;         // 0 = 不限 (仅单行会设置)
    int  m_marginLeft = 6;
    int  m_marginTop = 4;
    int  m_marginRight = 6;
    int  m_marginBottom = 4;

private:
    class EditCommand;                    // 撤销命令, 定义在 .cpp

    // ---- 图文混排内部工具 ----
    // key 缺省时生成唯一的 "img_<n>"
    std::string makeImageKey(const std::string& key) const;
    // 在码点下标 pos 处插入一个图片码点 (不做选区处理, 由调用方负责)
    void insertImageAt(int pos, const JZImage& image, const std::string& key,
                       int width, int height);

    // ---- 部件长度模型 (图文混排的定位基础) ----
    // 图片占一个私有域码点 (JZTextImageMap::isImageCode), 因此"码点下标"就是
    // 光标/选区的唯一坐标: 不再需要视觉下标或 run 模型。
    // 唯一的特判是**测宽**: 普通码点走字体度量, 图片码点走注册表的显示宽。
    // 行的"部件"(part) = 一个码点 (普通字符或图片), 用 part 序号可直接索引 m_text。

    // ---- 文本修改统一入口 (全部通过撤销栈) ----
    // 把 [pos, pos+len) 替换为 after, 记为一步 (可合并)。isolated = true 时
    // 该命令不与前后命令合并 (粘贴用)。
    void replaceRangeWithUndo(int pos, int removeLen, const JZString& after,
                              bool isolated = false);
    void applyEditRangeRaw(int pos, int removeLen, const JZString& after);
    // 撤销/重做收尾: 设光标 + afterContentChanged() (重建行/发信号)
    void finishEditRange(int cursor, int anchor);

    // 行首/行尾码点下标 (辅助: 取行范围)
    void lineRange(int line, int& flatStart, int& flatEnd) const;
    // 行高 = max(baseH, 本行最大图片高 + 2)
    int  lineHeightOfParts(int flatStart, int flatEnd, int baseH) const;

    // 段落序列的起点**码点下标**列表 (含末尾哨兵 = 末尾码点下标), 供光标按段移动。
    // 段落起点 = 每个 segment 的起点的码点下标, 最后追加末尾哨兵作为"末尾插入点"。
    std::vector<int> segmentStarts(int from = 0, int len = -1) const;

    // 一行布局结果 (文本变化后由 relayout() 重建)
    struct LineLayout {
        int start = 0;    // 行首码点索引
        int length = 0;   // 本行字符数 (不含 '\n')
        int y = 0;        // 行顶部 y (逻辑像素, 未扣滚动; 单行恒 0)
        int height = 0;   // 本行行高 (含图片撑高; 无图片时 == lineHeight())
    };

    void relayout();                          // 同步行布局 + 图片引用计数
    void syncImageRefs();                     // 扫描 m_text, 维护注册表引用计数
    int  lineOfIndex(int idx) const;          // 参数为码点下标
    void locateIndex(int idx, int& line, int& column) const;  // 码点下标 -> (行, 行内下标)
    int  indexFromLineColumn(int line, int column) const;     // (行, 行内下标) -> 码点下标
    int  columnFromX(int line, int x) const;
    int  indexFromXY(int x, int y) const;
    int  lineHeight() const;
    int  lineHeightOf(int line) const;        // 该行实际行高 (含图片撑高)
    int  contentTop() const;                      // 首行文本区顶 y (含滚动 / 单行居中)
    JZString displayString(const JZString& s) const;  // 密码模式: 每码点变 '*'

    int  selStart() const;
    int  selEnd() const;
    void deleteSelection();
    void insertText(const JZString& s, bool isolated = false);
    void moveCursor(int pos, bool select);
    void moveCursorVertical(int deltaLine, bool select);
    // 左右键的"一步": 图片就是一个码点, 因此等价于 ±1
    int  stepVisual(int dir) const;
    void selectWordAt(int pos);
    void refreshDesiredColumn();
    void ensureCursorVisible();
    void resetCursorBlink();
    void onCursorTimer();
    void afterContentChanged();                   // 增删内容后的统一收尾
    void copyToClipboard();
    void cutToClipboard();
    void pasteFromClipboard();
    // 组字串绘制: 反显底色 -> 文字 -> 下划线 (顺序不能乱, 底色会盖字)
    void drawComposition(class JZPainter& p, int x, int textTop, int visTop, int visH,
                         const JZString& dispComp);

    // 平台相关: 实现在 .cpp, 非 Win32 平台为空实现
    void updateIMEWindowPos();
    void cancelComposition();
    void completeComposition();                   // 结束组字并把内容落屏

    JZString m_text;
    JZString m_placeholder;
    std::vector<LineLayout> m_lines;

    // 图片像素/尺寸不放在控件里: 统一登记到进程级单例 JZTextImageMap,
    // m_text 里只留一个私有域码点。relayout() 会扫描文本并维护引用计数。
    int m_nextImageKey = 1;               // 未给 key 时自动生成唯一 key 用
    // 上次扫描时各图片码点的出现次数 (与 m_text 做 diff, 决定 retain/release)
    std::map<char32_t, int> m_imageRefs;

    int  m_cursor = 0;                            // 插入点, 也是选区的活动端
    int  m_anchor = 0;                            // 选区锚点; == m_cursor 表示无选区
    int  m_scrollX = 0;                           // 水平滚动 (逻辑像素)
    int  m_scrollY = 0;                           // 垂直滚动 (仅多行)
    int  m_desiredColumn = 0;                     // 上下移动光标时保持的目标列
    int  m_blinkPeriod = 500;
    bool m_cursorOn = false;
    bool m_dragging = false;
    JZTimer* m_cursorTimer = nullptr;

    long long m_lastClickMs = 0;                  // 双击检测 (不依赖消息层)
    int  m_lastClickX = 0;

    // 撤销/重做历史 (仿 QUndoStack); 编辑入口统一走 replaceRangeWithUndo
    JZUndoStack* m_undoStack = nullptr;

    // 组字 (composition) 状态
    bool m_composing = false;
    JZString m_comp;                              // 组字串 (未上屏)
    std::vector<unsigned char> m_compAttr;        // 每码点属性 (JZImeAttr_*)
    int  m_compCursor = 0;                        // 光标在组字串中的码点位置
};

#endif // JZ_ABSTRACT_TEXT_EDIT_H_
