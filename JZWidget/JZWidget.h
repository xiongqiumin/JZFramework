#ifndef JZ_WIDGET_WIDGET_H_
#define JZ_WIDGET_WIDGET_H_

#include "JZObject.h"
#include "JZPoint.h"
#include "JZSize.h"
#include "JZRect.h"
#include "JZString.h"
#include "JZStyleSet.h"

#include <climits>
#include <cstdint>
#include <vector>
#include "json/json.h"
#include "JZLayout.h"
#include "JZKeyEvent.h"
#include "JZImeEvent.h"
#include "JZMouseEvent.h"
#include "JZPainterEvent.h"
#include "JZResizeEvent.h"
#include "JZShowEvent.h"
#include "JZCloseEvent.h"
#include "JZHideEvent.h"
#include "JZImage.h"

// AI 语义动作 (ai-native-design L1): invoke 工具按 name 分发
struct JZAiAction
{
    std::string name;     // "click" / "setValue" / "toggle" / "select" / "setText"
    std::string params;   // 参数说明 (v1 简化为人类可读描述)
};

// 控件光标形状 (等价 Qt Qt::CursorShape): 与 Windows 标准光标一一对应 (IDC_*)。
enum class JZCursorShape {
    Arrow,          // 默认箭头 (IDC_ARROW)
    IBeam,          // 文本输入 (IDC_IBEAM)
    PointingHand,   // 可点击 (IDC_HAND)
    SizeHor,        // 左右拖拽 (IDC_SIZEWE)
    SizeVer,        // 上下拖拽 (IDC_SIZENS)
    SizeAll,        // 四向拖拽 (IDC_SIZEALL)
    Cross,          // 十字 (IDC_CROSS)
    Forbidden,      // 禁止 (IDC_NO)
    Wait,           // 忙碌 (IDC_WAIT)
};

// 光标对象 (等价 Qt QCursor; v1 仅封装标准形状, 后续可扩展自定义位图)。
// 可隐式由 JZCursorShape 构造, 便于 setCursor(JZCursorShape::SizeHor) 直写。
class JZCursor
{
public:
    JZCursor(JZCursorShape shape = JZCursorShape::Arrow)
        : m_shape(shape)
    {
    }

    JZCursorShape shape() const { return m_shape; }

    bool operator==(const JZCursor& o) const { return m_shape == o.m_shape; }
    bool operator!=(const JZCursor& o) const { return !(*this == o); }

private:
    JZCursorShape m_shape;
};

class JZTimer;   // 悬浮提示延时收起 (见 .cpp)
class JZStyleSheetParser;   // 样式表解析器 (控件级 setStyleSheet 持有, 见 JZStyleSheetParser.h)

class JZWidget : public JZObject
{
    JZ_OBJECT_DECLARE(JZWidget)

public:
    explicit JZWidget(JZWidget* parent = nullptr);
    virtual ~JZWidget();

    void setParentWidget(JZWidget* parent);
    JZWidget* parentWidget() const;

    // 找到所属显示设备 (回溯到根控件, 由其提供渲染设备)
    class JZWindow* window();
    class JZWindow* window() const;

    // 位置
    JZPoint pos() const;
    void move(int x, int y);
    void move(const JZPoint& p);
    int x() const;
    int y() const;

    // 尺寸
    JZSize size() const;
    virtual void resize(int w, int h);   // 虚: 显示设备重写以同步原生窗口尺寸
    void resize(const JZSize& s);
    int width() const;
    int height() const;

    JZRect rect() const;

    JZPoint mapTo(JZWidget* parent, const JZPoint& rect) const;

    // 控件坐标 -> 屏幕逻辑坐标 (与 Qt 的 mapToGlobal 一致; popup/IME 等
    // 自定位窗口放置、合成鼠标事件 globalPos 对齐均用它)。无原生窗口时退化
    // 为控件树内累加坐标。
    JZPoint mapToGlobal(const JZPoint& rect) const;

    JZRect mapTo(JZWidget *parent, const JZRect& rect) const;
    JZRect mapToGlobal(const JZRect &rect) const;

    // 抓图 (等价 Qt::QWidget::grab): 抓取本控件当前显示内容为图像。
    // 对 JZWindow 即整个客户区, 对子控件为其在客户区内的那块矩形。
    // 返回 RGBA 图像, 尺寸为**物理像素** (逻辑尺寸 × DPI 缩放), 与 Qt 一致;
    // 窗口未创建 / 尺寸为空 / 抓图失败时返回 isNull()==true 的空图。
    // 实现走 PrintWindow (窗口被遮挡或最小化也能抓到自绘内容), 失败退化 BitBlt。
    JZImage grab();

    // 尺寸约束
    void setFixedSize(int w, int h);
    void setFixedWidth(int w);
    void setFixedHeight(int h);

    void setMinimumSize(int w, int h);
    void setMinimumWidth(int w);
    void setMinimumHeight(int h);

    void setMaximumSize(int w, int h);
    void setMaximumWidth(int w);
    void setMaximumHeight(int h);

    int minimumWidth() const;
    int minimumHeight() const;
    int maximumWidth() const;
    int maximumHeight() const;

    // 尺寸策略 (参考 Qt QSizePolicy::Policy)
    enum SizePolicy {
        Fixed,            // 严格使用 sizeHint, 不增不减
        Minimum,          // 可用空间不足时保持最小, 富余时可增长
        Maximum,          // 可用空间不足时可收缩, 富余时保持 sizeHint
        Preferred,        // 可增可缩, 偏好 sizeHint
        Expanding,        // 可增可缩, 且倾向占据更多空间
        MinimumExpanding, // 倾向占据更多空间, 但不小于最小
        Ignored           // 完全忽略 sizeHint, 尽量填满可用空间
    };
    SizePolicy horizontalPolicy() const;
    SizePolicy verticalPolicy() const;
    void setSizePolicy(SizePolicy h, SizePolicy v);
    void setHorizontalPolicy(SizePolicy p);
    void setVerticalPolicy(SizePolicy p);

    // 期望尺寸 (参考 Qt QWidget::sizeHint); 默认回退到当前尺寸, 容器/控件可重写
    virtual JZSize sizeHint() const;
    // 最小期望尺寸 (参考 Qt QWidget::minimumSizeHint): 默认取布局的 minimumSizeHint
    // (无布局返回 0×0)。minimumWidth/Height 会取 max(显式设置, minimumSizeHint),
    // 使带布局的窗口/容器自动拥有贴合内容的缩放下限, 无需显式 setMinimumSize。
    virtual JZSize minimumSizeHint() const;
    // 几何
    JZRect geometry() const;
    void setGeometry(const JZRect& r);
    virtual void setGeometry(int x, int y, int w, int h);

    // 可见性
    void setVisible(bool visible);
    bool isVisible() const;

    void show();
    void hide();

    // 不透明度: 用于刷新遍历时的"剔除"判断 (等价 LVGL lv_obj 的 opaque 标志)
    void setOpaque(int opaque);
    int opaque() const;

    // 命中测试透传 (AI 鼠标等全窗覆盖层): 自身不拦截鼠标事件 (hitTest 跳过),
    // 但子控件 (如覆盖层上的取消按钮) 正常参与命中
    void setPassThrough(bool on) { m_passThrough = on; }
    bool isPassThrough() const { return m_passThrough; }

    // 子控件裁剪 (Qt WA_ClipChildren 语义): 开启后本控件所有子孙的绘制被裁剪到
    // 本控件的显示区域 (用于滚动区 viewport 把越界的内容控件裁掉)。
    // 绘制裁剪实现见 JZPainter::clippedToAncestors。
    void setClipChildren(bool on)
    {
        if (m_clipChildren == on)
            return;
        m_clipChildren = on;
        update();
    }
    bool clipChildren() const { return m_clipChildren; }

    // ---- 鼠标光标 (等价 Qt QWidget::cursor) ----
    // setCursor: 悬停本控件 (含未自行设置光标的子控件) 时显示该光标, 调用后
    //   立即生效 (窗口按当前悬停位置刷新); unsetCursor 撤销自身设置恢复继承;
    // cursor(): 生效光标 —— 自身已设置返回自身, 否则沿父链取最近设置者的,
    //   全部未设置则为默认箭头。
    void setCursor(const JZCursor& cursor);
    void unsetCursor();
    JZCursor cursor() const;

    // ---- 悬浮提示 (等价 Qt QWidget::setToolTip) ----
    // 鼠标悬停本控件时, 自动在控件上方弹出多行小面板 (文本用 '\n' 换行)。
    // 逻辑集中在 JZWidget, 各控件只需 setTooltip 即可, 不必各自维护弹层。
    void setTooltip(const JZString& tip);
    const JZString& tooltip() const;

    // 布局
    void setLayout(JZLayout* layout);
    JZLayout* layout() const;

    // 控件样式 (背景/边框/圆角/padding)。paintEvent 内经 JZStylePainter 主动绘制;
    // 后续由样式表 (JZStyleSheet) 解析填充。
    JZStyleSet& style() { return m_style; }
    const JZStyleSet& style() const { return m_style; }

    // ---- 样式表 (CSS 子集, 见 docs/jzstylesheet-design.md) ----
    // 本阶段只落"选择器素材 + 文本 -> 规则"的解析链路, 暂不做规则匹配 (resolve) 与生效:
    //   setStyleClass: 空格分隔的类名串 (同 HTML class 属性), 供将来的类选择器匹配;
    //   setStyleSheet: 控件级样式表 (Qt 语义: 作用于本控件), 解析结果由本控件持有;
    //                  返回 false 表示 CSS 有语法错误, 原因见 styleSheetError();
    //   全局样式表见 JZApplication::setStyleSheet。
    void setStyleClass(const std::string& cls);
    const std::string& styleClass() const;

    bool setStyleSheet(const std::string& css);
    std::string styleSheet() const;
    std::string styleSheetError() const;
    // 解析结果 (未设置过样式表则返回 nullptr)
    const JZStyleSheetParser* styleSheetParser() const;

    // ---- 样式解析 (resolve) ----
    // UA 默认外观: 各控件重写返回自己的硬编码默认值 (换肤色之前的外观基线)。
    // 每次重算样式时以此为起点, 再叠加样式表规则, 保证"未写样式的属性"回落到默认外观。
    virtual JZStyleSet uaStyle() const;

    // 样式失效: 清掉本控件与其子树 (控件级样式表才影响子孙) 的解析缓存并重绘。
    // 类变化 / 换表 / 控件级 setStyleSheet 后调用。
    void styleChanged();

    // 已解析样式 (内部结构, 绘制用): 首次访问时按当前样式表解析并缓存
    const JZStyleSet& resolvedStyle();

    // 重排:
    //   updateLayout()   只重排自身子控件(布局应用 / resize 内部使用, 不向上传播);
    //   updateGeometry() 在本控件尺寸特征(策略/约束/布局内容)变化后, 向上级联重排
    //                    (父布局持有本控件作为 item, 必须重排; 再继续向上直到根)。
    void updateLayout();
    void updateGeometry();

    // 焦点
    void setFocus();
    bool hasFocus() const;

    // 本控件当前是否需要输入法 (IME): 默认 false —— 焦点落在按钮/列表等非文本
    // 控件上时, 窗口会据此断开输入上下文, 避免"输入了但没有任何控件显示组字"。
    // 文本类控件 (JZLineEdit / JZTextEdit) 重写为可编辑时返回 true。
    virtual bool imeEnabled() const { return false; }

    // 重绘
    void update();
    void invalidate(const JZRect &rc);

    // ---- AI 语义接口 (对齐 Android AccessibilityNodeInfo 形态, ai-native-design L1) ----
    // 各控件按需重写 (每个 3~8 行); dumpSemanticTree 统一序列化成无障碍树
    virtual std::string aiRole() const;   // className 槽位: "window"/"button"/"textbox"...
                                          // (默认: 类名去 JZ 前缀小写)
    virtual std::string aiText() const;   // text 槽位: 可见文本 (按钮/输入框/标签; 默认空)
    virtual std::string aiName() const;   // contentDesc 槽位: 人类可读名 (默认 objectName, 空则 role)
    virtual bool aiChecked() const;       // checked 槽位 (checkbox/switch/radio)
    virtual bool aiRangeValue(int& value, int& min, int& max) const;  // slider/progress 的 RangeInfo
    virtual std::vector<std::string> aiItems() const;   // combo/list/tab 的条目摘要 (扩展字段)
    virtual std::vector<JZAiAction> aiActions() const;  // 支持的动作 (click/setValue/setText/select)

    // 无障碍树导出 (AccessibilityNodeInfo 风格):
    //   {className, viewId?, text?, contentDesc, boundsInScreen, visible,
    //    clickable, checkable?, checked?, editable, scrollable, range?, items?, actions?, children[]}
    // boundsInScreen 为屏幕逻辑坐标 (跨窗口/弹层也能对准); 只导出可见控件; maxDepth<0 不限深
    Json::Value dumpSemanticTree(int maxDepth = -1) const;

    // 事件分发入口 (JZObject::event 重写, Qt 风格 public):
    // 按事件类型分派到 paintEvent/mouseXxxEvent/keyXxxEvent/imeEvent/focusXxxEvent。
    virtual bool event(JZEvent* event) override;

    // 预定义属性
    virtual void setProperty(const std::string& name, const JZVariant& value) override;
    virtual JZVariant property(const std::string& name) const override;

protected:
    virtual void showEvent(JZShowEvent* event);
    virtual void hideEvent(JZHideEvent* event);
    // 注意: 没有 closeEvent —— 关闭是"窗口"的概念 (普通控件不可关闭), 在 JZWindow。
    virtual void resizeEvent(JZResizeEvent* event);
    virtual void paintEvent(JZPainterEvent* event);

    // 鼠标事件 (坐标为逻辑像素, localPos 相对本控件)。命中检测由
    // JZDisplayDevice 完成, ImGui 等渲染后端不参与交互。
    virtual void mousePressEvent(JZMouseEvent* event);
    virtual void mouseReleaseEvent(JZMouseEvent* event);
    virtual void mouseMoveEvent(JZMouseEvent* event);
    virtual void wheelEvent(JZMouseEvent* event);

    // 悬停态 (JZWindow 命中控件变化时分发, 同 Qt 的 enterEvent/leaveEvent):
    // 进入/离开本控件 (含其子树) 时触发。默认空; 悬浮提示等据此显示/收起。
    virtual void enterEvent(JZEvent* event);
    virtual void leaveEvent(JZEvent* event);

    // 鼠标抓取: 抓取后所有鼠标事件 (含窗口客户区外) 直发本控件,
    // 直到 releaseMouse 或窗口隐藏。用于拖拽 (滑块/分隔条) 避免光标
    // 冲出控件命中带时丢失 mouseMove/mouseRelease。等价于 Qt::grabMouse。
    void grabMouse();
    void releaseMouse();
    bool hasMouseGrab() const;

    // 键盘事件: key 为平台虚拟键码, text 为该键产生的可打印文本 (UTF-8)。
    // IME 上屏的内容**不走**这里, 走 imeEvent() 的 ResultStr。
    virtual void keyPressEvent(JZKeyEvent* event);
    virtual void keyReleaseEvent(JZKeyEvent* event);

    // 输入法事件: 组字 / 上屏 / 候选窗定位。控件收到后自绘组字串,
    // 并在需要时把光标位置回写给 IME (由控件自己调用平台 IMM 接口)。
    virtual void imeEvent(JZImeEvent* event);

    // 焦点进出: 文本控件在此开关光标闪烁, 并接上 / 断开输入上下文。
    virtual void focusInEvent(JZEvent* event);
    virtual void focusOutEvent(JZEvent* event);

    JZLayout* m_layout = nullptr;

    JZStyleSet m_style;

    // ---- 样式表素材 (见上方 setStyleClass/setStyleSheet) ----
    std::string m_styleClass;
    std::string m_styleSheetText;             // 原始 CSS 文本 (styleSheet() 原样返回)
    JZStyleSheetParser* m_styleSheet = nullptr;   // 懒创建: 首次 setStyleSheet 时 new

    // 解析缓存: m_style 是否已按当前 (表版本 + 类 + 自身表) 解析过
    bool m_styleResolved = false;

    int m_minW;
    int m_minH;
    int m_maxW;
    int m_maxH;

    SizePolicy m_horPolicy;
    SizePolicy m_verPolicy;

    int m_x;
    int m_y;
    int m_width;
    int m_height;

    bool m_visible;
    bool m_passThrough = false;
    bool m_clipChildren = false;
    int m_opaque = 255;    // 是否向身后的兄弟控件贡献"不透明覆盖"(重绘剔除; 0=透明覆盖层)

    bool m_hasCursor = false;      // 是否经 setCursor 显式设置过光标
    JZCursor m_cursor;             // 显式设置的光标 (m_hasCursor 为 true 时生效)

    // ---- 布局内部登记 (由 JZLayout 统一管理, 业务代码勿直接调用) ----
    friend class JZLayout;
    void _layoutAttach(JZLayout* layout);
    void _layoutDetach(JZLayout* layout);
    std::vector<JZLayout*> m_layoutOwners;   // 引用本控件的布局 (析构时自动摘除)

    // ---- 悬浮提示内部实现 (setTooltip 设置文本; enterEvent 起延时, 到时显示; leaveEvent 收起) ----
    void showTooltip();
    void hideTooltip();
    void onTooltipShow();     // 悬停延时到: 弹出 (区别于立即响应)
    void updateTooltip();
    enum { kTooltipShowDelay = 500 };   // 悬停多久才弹出 (ms)

    JZString   m_tooltipText;     // 空 = 不显示悬浮提示
    JZWidget*  m_tooltipPopup = nullptr;   // 懒创建: 悬停时弹出在控件上方的自绘面板
    JZTimer*   m_tooltipTimer = nullptr;   // 进入后的一次性"显示延时"定时器 (leave 取消)
};

#endif
