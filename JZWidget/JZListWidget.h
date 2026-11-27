#ifndef JZ_LIST_WIDGET_H_
#define JZ_LIST_WIDGET_H_
#include "JZAbstractViewWidget.h"
#include "JZSignal.h"
#include "JZImage.h"
#include "JZVariant.h"
#include "JZPainter.h"
#include "JZRect.h"
#include "JZPoint.h"
#include "JZColor.h"
#include <vector>
#include <string>
#include <memory>
#include <functional>

class JZListWidget;

// 绘制一项时由列表填好的上下文 (项自己 paint 时读它)。
struct JZFRAMEWORK_API JZListItemPaintInfo
{
    int  index = -1;        // 项索引
    bool selected = false;  // 是否当前项
    bool hovered = false;   // 预留: 悬停态
    bool focused = false;   // 预留: 焦点态
};

// 列表项数据 + 可选的自绘/自测。
// 所有权: 由 JZListWidget 持有(unique_ptr 存于 m_items), 外部拿到的是**借用指针** ——
// 地址只要不被 take/clear 就一直稳定(增删其它项不会搬移本项), 列表析构后失效。
// 改值用 setText/setIcon/setData: 会通知所属列表重绘。
//
// 自绘项 (变高条目, 如聊天气泡/日志卡):
//   子类重写 heightForWidth() 返回本行高度, 重写 paint() 画自己;
//   列表打开 setDynamicItemHeight(true) 后走"逐项测量"模式。
class JZFRAMEWORK_API JZListWidgetItem
{
public:
    JZListWidgetItem() {}
    explicit JZListWidgetItem(const std::string& text) : m_text(text) {}
    // 虚析构: 列表用基类指针持有, 子类的成员必须能正确析构
    virtual ~JZListWidgetItem() = default;

    // 所属列表(由 append/insert 自动设置; 未加入列表或被 takeItem 摘除后为 nullptr)
    JZListWidget* widget() const { return m_widget; }

    const std::string& text() const { return m_text; }
    const JZImage& icon() const { return m_icon; }
    const JZVariant& data() const { return m_data; }

    void setText(const std::string& t);
    void setIcon(const JZImage& i);
    void setData(const JZVariant& d);

    // ---- 自绘项接口 (不重写 = 旧行为: 图标 + 单行文本) ----
    // 本行在给定内容宽度下的高度:
    //   >= 0 : 本项自定高度 (需要换行的项在这里做测量并缓存);
    //   <  0 : 不参与逐项测量, 用列表的 setItemHeight() 定高 (默认实现返回 -1)。
    virtual int heightForWidth(int width);

    // 画自己。rect = 本项在视口内的矩形 (已扣掉内容内边距, 未扣项间距)。
    // 选中态从 info.selected 读: 默认实现会画选中底色 + 白字, 自绘项要高亮就自己画。
    virtual void paint(JZPainter& p, const JZRect& rect, const JZListItemPaintInfo& info);

    // 项内交互: 返回 true = 已消费, 列表不再做"点击选中/方向键移动"等默认处理。
    // rect 与 paint 的 rect 同坐标系; 需要焦点/拖拽时自己在 mousePress 里 setFocus()。
    virtual bool mousePress(const JZRect& rect, JZMouseEvent* e);
    virtual bool mouseMove(const JZRect& rect, JZMouseEvent* e);
    virtual bool mouseRelease(const JZRect& rect, JZMouseEvent* e);
    // 当前项收到按键 (Ctrl+C 之类); 返回 true = 已消费
    virtual bool keyPress(JZKeyEvent* e);

    // 内容变了(流式追加/折叠/选区变化...): 通知列表只重测这一项并重绘。
    // 自绘项改完自己的状态必须调它, 否则列表不会重排。
    void notifyChanged();

private:
    friend class JZListWidget;      // 列表负责设置 m_widget 并直接读写内部状态

    JZListWidget* m_widget = nullptr;
    JZImage m_icon;
    std::string m_text;
    JZVariant m_data;
};

// 虚拟列表(owner-draw): 单列, 仅绘制当前可见区域, 通过 JZPainter 直接画到画布上,
// 不创建/复用任何行控件。网格/图片墙不是列表的形态 —— 需要时用 JZScrollArea + 自己的
// 内容控件(或另做一个视图控件), 别往列表里塞列布局。
//
// 两种高度模式:
//   - 定高(默认): 所有行 = itemHeight(), 老路径, 不分配任何高度缓存;
//   - 逐项测量: setDynamicItemHeight(true), 每行问 item->heightForWidth(内容宽),
//     高度可不同 —— 聊天/日志/时间线这类"内容多长行就多高"的场景。
//     该模式下 setSpacing()/setContentPadding() 生效。
class JZFRAMEWORK_API JZListWidget : public JZAbstractViewWidget
{
    JZ_OBJECT_DECLARE(JZListWidget)

public:
    // 自定义绘图函数: painter 为画布, rect 为该项在视口中的矩形, item 为数据。
    // 设了就优先于 item->paint() (整体换画法的快捷方式; 逐项不同的画法请重写 item->paint)。
    using PaintFn = std::function<void(JZPainter& painter, const JZRect& rect, JZListWidgetItem* item)>;

    explicit JZListWidget(JZWidget* parent = nullptr);
    ~JZListWidget();

    // ---- 数据模型 (接口形态与 Qt QListWidget 对齐) ----
    // 项对象由调用方创建并交给列表**接管所有权**(/Transfer/); 列表不返回句柄,
    // 需要时用 item(row) 取回。新增接口一律返回 void —— 与 Qt 的 addItem/insertItem 一致。
    // item()/currentItem() 是 const 方法却返回非 const 指针: 与 Qt 的 QListWidget::item(row) const
    // 一致 —— "写项属性"是列表数据的正常用法, 改值由 item 自己通知列表重绘。
    void appendItem(const std::string& text);                   // 列表新建一项  (Qt: addItem(QString))
    void appendItem(JZListWidgetItem* item);                    // 接管, 须 new 分配 (Qt: addItem(item))
    void appendItems(const std::vector<std::string>& texts);    //              (Qt: addItems)
    void insertItem(int index, const std::string& text);        //              (Qt: insertItem(row, QString))
    void insertItem(int index, JZListWidgetItem* item);         // 接管          (Qt: insertItem(row, item))

    // 摘除: 交出所有权, 由调用方释放。Qt 没有 removeItem —— 要删除就是 `delete takeItem(row)`。
    JZListWidgetItem* takeItem(int index);                      // 越界返回 nullptr

    void clear();

    // 数据变了(增删项 / 文本 / 图标 / data)就发一次 sigDataChanged ——
    // 外部观察者(JS 的 `onDataChanged`、AI 工具)靠它感知"数据被改过了"。
    // 任何改数据的入口(含 JS 绑定层自己发起的那些)都会发, 不做"谁发起"的区分。
    // 只发信号, 不做重绘/尺寸刷新(调用点各自负责, 保持原有行为不变)。
    void notifyDataChanged();

    // 查询
    int count() const;
    JZListWidgetItem* item(int index) const;                    // 越界返回 nullptr
    int row(const JZListWidgetItem* item) const;                // 未找到返回 -1 (Qt: row(item))
    std::string itemText(int index) const;                      // 便捷值访问(等价 item(i)->text())
    JZVariant itemData(int index) const;

    // ---- 视图 ----
    void setItemHeight(int h);          // 定高模式的行高
    int itemHeight() const;

    // 期望尺寸: 宽度贴合最宽项 (文本 + 图标), 高度按项数 * 行高 (至多 8 行,
    // 再多交给滚动)。虚拟列表只有自己知道该多大 —— 基类默认 0×0 会让它在
    // 固定项 (stretch=0) 的布局里被压成 0。
    JZSize sizeHint() const override;

    // 设置自定义绘图函数; 不设置则使用 item->paint()
    void setItemPaint(PaintFn fn);

    // ---- 逐项测量 (变高) ----
    // 开: 每行高度由 item->heightForWidth(内容宽) 决定, contentPadding/spacing 生效;
    // 关(默认): 全部走定高老路径, 行为与之前完全一致。
    void setDynamicItemHeight(bool on);
    bool dynamicItemHeight() const;

    // 项间距(px, 只在逐项测量模式生效): 相邻两项之间的竖向间隙
    void setSpacing(int px);
    int spacing() const;
    // 内容内边距(px, 只在逐项测量模式生效): 项的外框内缩
    void setContentPadding(int left, int top, int right, int bottom);
    int contentPaddingLeft() const;
    int contentPaddingTop() const;
    int contentPaddingRight() const;
    int contentPaddingBottom() const;

    // 内容变高时是否保持贴底(聊天/日志): 只在"变化前已经贴底"时生效,
    // 用户上滚看历史时不会被新内容拽走。配合 atBottom() 用。
    void setStickToBottom(bool on);
    bool stickToBottom() const;
    bool atBottom() const;

    // 背景色(默认白) / 无项时的居中提示(空串 = 不画)
    void setBackgroundColor(const JZColor& c);
    void setEmptyText(const std::string& text);

    // ---- 几何查询 (两种高度模式都可用, 坐标为内容坐标 / 视口坐标各见注释) ----
    int itemTop(int index) const;        // 内容坐标下的项顶部; 越界返回 -1
    int itemHeightAt(int index) const;   // 项高度; 越界返回 -1
    int indexAt(const JZPoint& contentPos) const;   // 内容坐标 -> 项索引 (-1 = 不在任何项上)
    // 项在视口内的矩形(含当前滚动偏移); 越界/不可见也返回几何
    bool itemRect(int index, JZRect& out) const;

    // 单项内容变化: 只重测这一项 + 重算其后各项顶部 + 重绘 (流式追加走这里)
    void itemChanged(JZListWidgetItem* item);

    // ---- 选择 ----
    int currentIndex() const;
    void setCurrentIndex(int index);
    JZListWidgetItem* currentItem() const;                     // 无选中返回 nullptr
    void setCurrentItem(JZListWidgetItem* item, bool emitSignal = true);

jz_signals:
    // 当前项变化; **-1 = 无选中** (摘除当前项 / clear() 时发一次)。
    // 摘除当前项后不会自动把选择挪到邻项(Qt 语义), 要保持选中由调用方自己 setCurrentIndex。
    JZSigInt currentIndexChanged;
    JZSigInt sigActivated;   // 激活 (点击项 / Enter), 下拉弹层收编用
    JZSigVoid sigDataChanged;   // 数据(增删项/文本/图标/data)变化 —— 外部观察者(JS onDataChanged)用

public:
    // ---- AI 语义 ----
    std::string aiRole() const override { return "list"; }
    std::string aiText() const override { return itemText(m_current); }
    std::vector<std::string> aiItems() const override
    {
        std::vector<std::string> v;
        for (const auto& it : m_items)
            v.push_back(it->text());
        return v;
    }
    std::vector<JZAiAction> aiActions() const override { return {{"select", "number 项索引"}}; }

protected:
    void showEvent(JZShowEvent* e) override;
    void paintEvent(JZPainterEvent* event) override;
    void resizeEvent(JZResizeEvent* event) override;
    void mousePressEvent(JZMouseEvent* event) override;
    void mouseMoveEvent(JZMouseEvent* event) override;
    void mouseReleaseEvent(JZMouseEvent* event) override;
    void keyPressEvent(JZKeyEvent* event) override;

    void updateContentSize() override;
    // 追加/插入的统一实现: 建立反向引用 -> 落位 -> 刷新尺寸与重绘。
    JZListWidgetItem* insertItemAt(int index, std::unique_ptr<JZListWidgetItem> item);

    // ---- 逐项测量的内部布局 ----
    void ensureLayout() const;                  // 惰性重算 m_heights / m_tops
    void invalidateLayout(int fromIndex = 0);   // fromIndex<0 = 全部; >=0 = 从该项起
    int  contentWidthForItems() const;          // 项可用宽 = 视口宽 - 左右内边距
    void paintItemAt(JZPainter& p, int index, const JZRect& rect);  // 分发到 PaintFn / item->paint

    // 用 unique_ptr 存放: 保证 item 地址稳定(值存 vector 一旦扩容搬移, 外部拿到的借用指针
    // 会静默悬垂), 同时所有权/删除点唯一。
    std::vector<std::unique_ptr<JZListWidgetItem>> m_items;

    int m_itemHeight = 28;     // 定高模式行高
    int m_current = -1;

    PaintFn m_paintFn;

    // ---- 逐项测量模式 ----
    bool m_dynamic = false;
    int  m_spacing = 0;
    int  m_padL = 0, m_padT = 0, m_padR = 0, m_padB = 0;
    bool m_stickBottom = false;
    mutable std::vector<int> m_heights;   // 每项高度
    mutable std::vector<int> m_tops;      // 每项顶部, size = count+1 (含间距的前缀和)
    mutable int  m_layoutWidth = -1;      // 上次测量用的内容宽
    mutable int  m_dirtyFrom = 0;         // 从该项起高度失效 (0 = 全部)
    mutable bool m_layoutValid = false;
    int  m_mouseItem = -1;                // 正在被拖拽的项 (按下时认领)

    JZColor m_bgColor = JZColor(0xFF, 0xFF, 0xFF);
    std::string m_emptyText;
};

#endif
