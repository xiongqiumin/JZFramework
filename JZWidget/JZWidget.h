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
#include "JZDragEvent.h"
#include "JZImage.h"

// AI 语义动作 (ai-native-design L1): invoke 工具按 name 分发
struct JZFRAMEWORK_API JZAiAction
{
    std::string name;     // "click" / "setValue" / "toggle" / "select" / "setText"
    std::string params;   // 参数说明 (v1 简化为人类可读描述)
};

// 控件光标形状 (等价 Qt Qt::CursorShape): 与 Windows 标准光标一一对应 (IDC_*)。
enum class JZFRAMEWORK_API JZCursorShape {
    Arrow,          // 默认箭头 (IDC_ARROW)
    IBeam,          // 文本输入 (IDC_IBEAM)
    PointingHand,   // 可点击 (IDC_HAND)
    SizeHor,        // 左右拖拽 (IDC_SIZEWE)
    SizeVer,        // 上下拖拽 (IDC_SIZENS)
    SizeAll,        // 四向拖拽 (IDC_SIZEALL)
    SizeFDiag,      // 主对角线拖拽 (IDC_SIZENWSE, ↘↖)
    SizeBDiag,      // 副对角线拖拽 (IDC_SIZENESW, ↗↙)
    Cross,          // 十字 (IDC_CROSS)
    Forbidden,      // 禁止 (IDC_NO)
    Wait,           // 忙碌 (IDC_WAIT)
};

// 光标对象 (等价 Qt QCursor; v1 仅封装标准形状, 后续可扩展自定义位图)。
// 可隐式由 JZCursorShape 构造, 便于 setCursor(JZCursorShape::SizeHor) 直写。
class JZFRAMEWORK_API JZCursor
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
class JZMenu;    // 上下文菜单 (右键菜单; 见 setContextMenu / setContextMenuPolicy)

class JZFRAMEWORK_API JZWidget : public JZObject
{
    JZ_OBJECT_DECLARE(JZWidget)

public:
    explicit JZWidget(JZWidget* parent = nullptr);
    virtual ~JZWidget();

    void setParentWidget(JZWidget* parent);
    JZWidget* parentWidget() const;

    // 找到所属显示设备 (回溯到根控件, 由其提供渲染设备)
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

    JZPoint mapTo(JZWidget* parent, const JZPoint& pt) const;
    JZPoint mapToGlobal(const JZPoint& pt) const;

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

    // ---- 使能状态 (等价 Qt QWidget::setEnabled / isEnabled) ----
    // 语义(对齐 Qt):
    //   * isEnabled() 沿父链判定 —— 任一祖先被禁用, 自身也视为禁用 (禁用是可继承的);
    //   * 被禁用的控件**不接收**鼠标(含滚轮与右键菜单)、键盘、输入法事件: 事件在本控件
    //     被吞掉(既不派发也不向父控件冒泡, Qt 同款); 已持有焦点时立即失焦;
    //   * 绘制/显隐/尺寸等事件照常处理 (禁用的控件仍要画、仍参与布局);
    //   * 外观(灰化)由控件自己决定 —— 框架不动样式, 需要灰化的控件在 paintEvent 里查
    //     isEnabled() 自行换色 (与 Qt 的 disabled 调色板由样式层负责一致)。
    void setEnabled(bool enabled);
    bool isEnabled() const;

    // 不透明度: 用于刷新遍历时的"剔除"判断 (等价 LVGL lv_obj 的 opaque 标志)
    void setOpaque(int opaque);
    int opaque() const;

    // 命中测试透传 (AI 鼠标等全窗覆盖层): 自身不拦截鼠标事件 (hitTest 跳过),
    // 但子控件 (如覆盖层上的取消按钮) 正常参与命中
    void setPassThrough(bool on) { m_passThrough = on; }
    bool isPassThrough() const { return m_passThrough; }

    // ---- 拖放接收开关 (等价 Qt QWidget::setAcceptDrops) ----
    // 打开后本控件参与拖放: 外部把数据拖到它上面时, 依次收到 dragEnter -> dragMove* ->
    // drop (拖出去则 dragLeave)。默认关闭 —— 未开 receipt 的控件对拖放完全透明。
    // 事件沿父链逐级尝试: 最深的控件不收就交给父控件, 于是"容器整体接受拖放"
    // 不必给每个子控件都开这一个开关 (Qt 同款语义)。
    // 拖着的过程中改这个值: 下一次 dragMove 重新挑目标时生效。
    void setAcceptDrops(bool on) { m_acceptDrops = on; }
    bool acceptDrops() const { return m_acceptDrops; }

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

    // ---- 上下文菜单 (右键菜单, 等价 Qt 的 contextMenuPolicy / customContextMenuRequested) ----
    // 触发时机: 右键**释放** (与 Windows WM_CONTEXTMENU / Qt 的默认时机一致) —— 放在
    //   mouseReleaseEvent 之后, 于是列表/表格等控件先按右键更新选中态, 再轮到菜单。
    // 处理权沿父链向上递交: 策略为 NoContextMenu (= 默认, "本控件没有菜单")时交给父控件,
    //   直到某个控件的策略不是 NoContextMenu (Qt 同款语义); PreventContextMenu 就地吞掉。
    //
    // 两种用法 (对应 Qt 的两种主要策略):
    //   CustomContextMenu  —— 发 sigCustomContextMenuRequested(x, y): x/y 是**发起者控件**的
    //                         局部坐标, 菜单由接收方自己弹 (menu->popup(window(), ...), 屏幕坐标);
    //   ActionsContextMenu —— 直接弹 setContextMenu() 挂上的那份菜单。
    enum ContextMenuPolicy {
        NoContextMenu,        // 0 (默认): 没有菜单, 交给父控件
        PreventContextMenu,   // 1: 吞掉请求, 到此为止
        DefaultContextMenu,   // 2: 预留给"由子类提供默认菜单"(当前等同 NoContextMenu)
        ActionsContextMenu,   // 3: 弹 setContextMenu() 挂的那份菜单
        CustomContextMenu,    // 4: 发 sigCustomContextMenuRequested(x, y)
    };
    void setContextMenuPolicy(ContextMenuPolicy policy) { m_contextMenuPolicy = policy; }
    ContextMenuPolicy contextMenuPolicy() const { return m_contextMenuPolicy; }

    // 挂一份右键菜单 (只有策略为 ActionsContextMenu 时由框架自动弹)。
    // **借用**: 菜单的存活由调用方负责 —— 与 JS 侧的 `new JZMenu()` 归 JS 拥有同一套路
    // (对照 setLayoutFromJs), 控件只持守卫: 菜单先没了自己变成空, 不会留下悬垂指针。
    void setContextMenu(JZMenu* menu);
    JZMenu* contextMenu() const;

    // 走一遍上下文菜单流程 (pos = 本控件局部坐标): 策略处理 + 向上递交。
    // 也是程序/AI 合成的触发入口 (`w->requestContextMenu(w->rect().center())`)。
    void requestContextMenu(const JZPoint& pos);

jz_signals:
    JZSignal<int, int> sigCustomContextMenuRequested;   // (x, y) 发起者控件的局部坐标

public:
    // 布局 (Qt 语义, 对齐 QWidget::setLayout):
    //   一个控件最多一个布局, 只在**还没有布局**时能设:
    //     * 已有布局时再 setLayout → 警告并忽略(Qt 同款行为: 新布局不被接管, 由调用方自行释放);
    //     * setLayout(nullptr) → 警告并忽略(Qt 是 "Cannot set layout to 0", null 不是清空手段)。
    //   清空/替换的正确姿势: 先 clearLayout()(等价 Qt 的 `delete w->layout()`, 布局析构会
    //   同步清掉本控件的布局指针), 再 setLayout(新布局)。
    //   装上后布局即成为**本控件的子对象**(JZObject 父子树): 随控件一起析构(布局不会活得比
    //   容器久), 布局先被销毁时经 ChildRemoved 把 m_layout 置空(见 childEvent), 永不留悬垂。
    void setLayout(JZLayout* layout);
    // 从 JS 侧设布局: 与 setLayout 同一套 Qt 语义, 但**本控件不显式 delete** —— 那份布局
    // 归它的 JS 对象(JS 对象回收时 delete)。同样会挂成本控件的子对象, 故控件先析构时由对象树
    // 回收它(JS 侧包装靠守卫识别"已经没了", 不会重复 delete)。
    // 理由: JS 侧的 `new JZVBoxLayout()` 是 JS 拥有的句柄(新模型: 谁创建谁持有), 控件再删一次
    // 就是双重释放; C++ 内部自建的布局走 setLayout(由本控件 delete)。
    virtual void setLayoutFromJs(JZLayout* layout);
    // "布局实际装在哪": 普通控件就是自己; 容器子类可覆写(如 JZGroupBox 装到内部内容区)。
    // 绑定层靠它判断 setLayoutFromJs 有没有被接受, 也用不着去猜子类的内部结构。
    virtual JZLayout* appliedLayout() const { return m_layout; }
    JZLayout* layout() const;

    // 清空当前布局: 把指针置空(C++ 自建的那份顺便 delete; 归 JS 的那份只解绑, 仍作为子对象
    // 挂在控件上, 由对象树随控件回收)。Qt 没有这个 API(那边直接 `delete w->layout()`),
    // 给显式入口是为了 JS 侧 —— JS 删不掉 C++ 对象。
    // 布局不拥有子控件: 子控件留在对象树上、保持最后一次几何, 只是不再被布局管理。
    // 虚: 容器子类(如 JZGroupBox)的布局装在内部内容区, 要转发。
    virtual void clearLayout();

    // 自布局: 把自身控件按 JZSelfLayout 描述(锚点 + 偏移)摆放在父控件内。
    // 父控件 resize 时框架自动重摆(见 JZWidget::resize); setSelfLayout 时也会立即按当前父尺寸摆一次。
    void setSelfLayout(JZSelfLayout* layout);
    JZSelfLayout* selfLayout() const { return m_selfLayout; }

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
    // 本控件**消费**哪些样式属性。声明驱动解析: 未声明的属性不会被写入本控件的
    // JZStyleSet (写了也不生效)。基类 = 盒子层 (背景/边框/圆角/padding);
    // 需要消费更多属性的控件重写并把基类的结果带上。
    //
    // 之所以由控件自己声明: 属性最终由绘制代码消费, 只有绘制方知道自己读什么
    // (将来的自定义列表子项 / 自定义绘制更是如此)。漏声明会立刻表现为"样式没效果",
    // 比"元数据与实现漂移"更容易发现。
    //
    // 声明的名字必须在 JZStyleSet::propertyTable() 内 (未知名等于永不生效)。
    virtual std::vector<std::string> styleProps() const;

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

    // 输入法 (IME) 位置反馈: TSF 类输入法 (QQ 拼音 / 微软拼音 等) 会通过
    // WM_IME_REQUEST (IMR_COMPOSITIONWINDOW / IMR_CANDIDATEWINDOW) 向窗口索取
    // "组字区 / 候选窗"位置。焦点控件把当前输入行矩形 (lineRect) 与候选窗左上角
    // 锚点 (candPos) 以**屏幕逻辑坐标**回写, 窗口据此应答; 返回 false 表示不提供
    // (窗口回落到 ImmSetCompositionWindow / ImmSetCandidateWindow 已设的值)。
    virtual bool imeWindowRect(JZRect& lineRect, JZPoint& candPos) const {
        (void)lineRect; (void)candPos;
        return false;
    }

    // IMR_QUERYCHARPOSITION 应答: TSF 输入法 (QQ 拼音等) 通过 WM_IME_REQUEST 的
    // 该请求索取"组字串第 charPos 个字符"的屏幕位置, 并据此摆放候选窗 —— 不处理时
    // 候选框会退化到控件底部的默认位置。各参数均为**屏幕逻辑坐标**:
    //   pt         — 该字符左上角
    //   lineHeight — 该行行高
    //   docRect    — 可编辑区矩形
    // 返回 false 表示不提供 (窗口交回默认处理)。默认不实现。
    virtual bool imeQueryCharPosition(int charPos, JZPoint& pt, int& outLineHeight,
                                      JZRect& docRect) const {
        (void)charPos; (void)pt; (void)outLineHeight; (void)docRect;
        return false;
    }

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

    // ---- 拖放事件 (等价 Qt 的 dragEnterEvent / dragMoveEvent / dragLeaveEvent / dropEvent) ----
    // 仅 setAcceptDrops(true) 的控件收到。四个 handler 的默认实现都是"不接"
    // (基类实现一律 ignore), 故通常只要重写用到的那一个:
    //   class ImageView : public JZWidget {
    //       ImageView() { setAcceptDrops(true); }
    //   protected:
    //       void dragEnterEvent(JZDragEnterEvent* e) override {
    //           if (e->mimeData()->hasImage() || e->mimeData()->hasUrls())
    //               e->acceptProposedAction();     // 接 := 这次拖放由本控件负责
    //       }
    //       void dropEvent(JZDropEvent* e) override {
    //           JZImage img = e->mimeData()->image();     // 位图或第一个图片文件
    //           if (img.isNull()) return;
    //           m_image = img; update();
    //           e->acceptProposedAction();
    //       }
    //   };
    // dragMoveEvent: 想按位置区分落点 (如拖到列表的哪一项) 才重写, 同样要 accept;
    // dragLeaveEvent: 拖走了, 复原拖着时画的那些提示 (描边 / 占位高亮)。
    virtual void dragEnterEvent(JZDragEnterEvent* event);
    virtual void dragMoveEvent(JZDragMoveEvent* event);
    virtual void dragLeaveEvent(JZDragLeaveEvent* event);
    virtual void dropEvent(JZDropEvent* event);

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

    // 子对象增删 (JZObject::childEvent 重写): 布局是本控件的子对象(见 setLayout),
    // 它被销毁 / 被 reparent 走时同步把 m_layout 置空 —— m_layout 与布局的实际生死
    // 只有这一个同步点, 故外部先一步销毁布局(JS 的 GC、`delete w->layout()`)也不留悬垂。
    void childEvent(JZChildEvent* event) override;

    JZLayout* m_layout = nullptr;
    bool m_layoutFromJs = false;            // true: 布局归 JS 拥有(setLayoutFromJs) -> 本控件不 delete
    void setLayoutImpl(JZLayout* layout, bool fromJs);
    JZSelfLayout* m_selfLayout = nullptr;   // 控件自身挂的自布局(父 resize 时重摆)

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
    bool m_enabled = true;         // 自身使能位 (祖先禁用经 isEnabled 沿父链体现)
    bool m_passThrough = false;
    bool m_clipChildren = false;
    bool m_acceptDrops = false;    // 是否接收拖放 (setAcceptDrops; 见 dragEnterEvent)
    int m_opaque = 255;    // 是否向身后的兄弟控件贡献"不透明覆盖"(重绘剔除; 0=透明覆盖层)

    bool m_hasCursor = false;      // 是否经 setCursor 显式设置过光标
    JZCursor m_cursor;             // 显式设置的光标 (m_hasCursor 为 true 时生效)

    // ---- 布局内部登记 (由 JZLayout 统一管理, 业务代码勿直接调用) ----
    friend class JZLayout;
    void _layoutAttach(JZLayout* layout);
    void _layoutDetach(JZLayout* layout);
    // 布局被销毁 / 换了宿主时由 JZLayout 回调: 本控件持有的正是它就把指针清空
    // (对应 Qt "布局析构时同步把所属控件的布局指针置空", 保证 delete layout / clearLayout 不留悬垂)。
    void _clearLayoutPointer(JZLayout* layout);
    std::vector<JZLayout*> m_layoutOwners;   // 引用本控件的布局 (析构时自动摘除)

    // ---- 右键菜单内部实现 (见 ContextMenuPolicy) ----
    void popupContextMenu(const JZPoint& pos);   // 弹 m_contextMenu(ActionsContextMenu 走到这里)

    ContextMenuPolicy m_contextMenuPolicy = NoContextMenu;
    JZObjectPointer   m_contextMenu;     // 借用: 菜单活着就用, 没了就自然失效

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
