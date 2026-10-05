#ifndef JZ_UI_PARSER_H_
#define JZ_UI_PARSER_H_

#include "JZGlobal.h"
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "json/json.h"

// ---------------------------------------------------------------------------
// JZUiParser —— UI 定义的**解析 + 方言**(Qt 风格 .ui/xml -> JZUiNode 树)。
//
// 为什么单独提出来: 这份方言要被**两个后端**共用 ——
//   * JS 侧 JZJSEngine/JZJSUiLoader: 把树生成 JS 脚本, 交给引擎 eval;
//   * C++ 侧 JZWidget/JZUiLoader:    把树直接 new 成真 JZWidget。
// 解析/方言在这里只有一份, 两边各写各的"生成控件", 就不会出现两份方言走偏的事。
// (2026-10-04 从 JZJSEngine/JZJSUiLoader.{h,cpp} 拆出来, 为的是让 C++ 侧能住在 JZWidget 里。)
// ---------------------------------------------------------------------------

// UI 定义节点：纯数据结构，描述一个控件(不含任何真实 JZWidget 实例)。
// - type    控件类名(对应后续 JZWidget 子类 / @jz/widget 的 type), 如 "Window"/"PushButton"/"Label"
// - id      可选, 生成 TS 时的成员名, 也对应运行时 widget 的 objectName
// - props   属性键值(原样保留 Json::Value, 保留类型: 字符串/数字/布尔/数组)
// - children 递归子节点(控件, 即 <widget>)
// - items   列表型子项(ComboBox / ListWidget 等不支持通用布局的控件, 用 addItem 添加)
struct JZFRAMEWORK_API JZUiNode
{
    std::string type;
    std::string id;
    Json::Value props;                    // objectValue(可能为空)
    std::vector<JZUiNode> children;
    std::vector<std::string> items;       // 文本项(对应 addItem(text))

    // 网格布局(GridLayout)里的位置: 来自 Qt 的 <item row="1" column="1">(转换器把 row/column
    // 直接写在 <widget> 上)。-1 = 未指定(非网格布局的子节点恒为 -1)。
    int row = -1;
    int col = -1;

    // 盒子布局(VBox/HBox)里的伸缩因子: 来自 <widget ... stretch="1">(即 addWidget(w, stretch))。
    // 0 = 不伸缩(按 sizeHint 摆) —— 与 Qt 的 <stretch> 同义, 也是方言里"让表格/文本区撑满"的写法。
    // 注: 网格布局用的是 setRowStretch/setColumnStretch(按行/列), 不走这里。
    int stretch = 0;

    bool isValid() const { return !type.empty(); }
};

// 子控件挂载方式(按父控件类型定)。两个后端共用同一套判别。
enum class UiMount
{
    Central,      // MainWindow: 单中央控件(setCentralWidget; 菜单栏走 addMenuBar)
    Stack,        // StackedWidget: 每页一个
    Splitter,     // Splitter: 每子项一个
    Tab,          // TabWidget: addTab(page, label)
    Scroll,       // ScrollArea: setWidget(单一内容控件)
    WindowAdd,    // Window / Dialog: child.setParentWidget(window)
    None,         // 非容器(ComboBox / ListWidget / TableWidget): 用 <item>/属性
    Parent,       // 通用容器(JZWidget / GroupBox / ...): child.setParentWidget(container)
};

// UI 定义解析器 + 方言表。
//
// XML 约定(对齐 Qt .ui, 但不做事件绑定, 只做属性定义):
//   <ui>
//     <class>DemoUi</class>            <!-- 可选, 生成 TS 类名(默认 "Ui") -->
//     <widget class="Window" name="main">
//       <property name="title">Demo</property>
//       <property name="geometry">100,100,400,300</property>  <!-- 4 个数 -> setGeometry -->
//       <widget class="Label" name="tip">
//         <property name="text">Count: 0</property>
//       </widget>
//     </widget>
//   </ui>
class JZFRAMEWORK_API JZUiParser
{
public:
    JZUiParser();

    // 从 XML 文本解析(类似 Qt uic 输入)。失败返回 false, 原因见 error()。
    bool load(const std::string& xmlText);
    // 从文件读取(XML 文本)并解析。
    bool loadFromFile(const std::string& path);

    const JZUiNode* root() const { return m_ok ? &m_root : nullptr; }
    std::string error() const { return m_error; }

    // 生成类名(来自 <class> 元素, 默认 "Ui")。
    std::string className() const { return m_className; }

    // 已知控件类型表(用于校验/提示, 可按需扩展)。空表表示不做类型校验。
    static const std::vector<std::string>& knownTypes();

    // 深度优先遍历(便于后续建 widget / 导出 / 校验 id 唯一性)。
    using VisitFn = std::function<void(const JZUiNode& node, const JZUiNode* parent)>;
    void traverse(const VisitFn& fn) const;

    // 收集所有 id 匹配的节点(允许重复 id)。
    std::vector<const JZUiNode*> nodesById(const std::string& id) const;

    // 把当前定义回写为 Json::Value(与内部树一致, 便于测试/往返校验)。
    void toJson(Json::Value& out) const;

    // ---- 方言表/判别(两个后端共用) ----
    static const std::map<std::string, std::string>& typeMap();   // "Label" -> "JZLabel"
    static std::string classForType(const std::string& type);     // 未知类型返回空串
    static bool isLayoutType(const std::string& t);
    static bool isWindowType(const std::string& t);
    static bool isMenuStructure(const std::string& t);
    static bool acceptsLayout(const std::string& t);              // 能不能 setLayout
    static bool isItemHost(const std::string& t);                 // 用 <item> 添内容的控件
    static UiMount mountMode(const std::string& t);
    static std::string tabLabel(const JZUiNode& child);
    static bool hasText(const std::string& t);
    static bool hasTitle(const std::string& t);
    static bool hasCurrentIndex(const std::string& t);
    static bool hasTabPosition(const std::string& t);             // TabWidget 专属: 标签条方向
    // .ui 里的枚举串 -> 序号: 认 "West" / "QTabWidget::West" / "QTabWidget::TabPosition::West"
    // (Qt 的 .ui 写的是最后一种)。认不出返回 -1; 序号与 JZTabWidget::TabPosition 同值。
    static int         parseTabPosition(const std::string& value);
    static std::string tabPositionName(int pos);                  // 0 -> "North", 越界 -> ""
    static bool hasChecked(const std::string& t);
    static bool hasReadOnly(const std::string& t);
    static bool hasPlaceholder(const std::string& t);
    static bool hasRange(const std::string& t);
    static bool isDoubleSpin(const std::string& t);   // 小数步进框(专属属性: singleStep/decimals/prefix)
    static bool isDateEdit(const std::string& t);     // 日期框(专属属性: dateMsecs/displayFormat/calendarPopup)
    static int layoutMarginIndex(const std::string& key);         // leftMargin.. -> 0..3; 不是则 -1
    static std::string propText(const Json::Value& v);            // 文本属性统一转字符串
    // .ui 里的 styleSheet 文本 -> 真正交给控件级 setStyleSheet 的 CSS。
    // 引擎的样式表解析器只吃"选择器 + 声明"的规则(控件级内联声明 setStyle 还没实现, 见
    // docs/jzstylesheet-design.md §5), 而 Qt 里 `setStyleSheet("color:red")` 是合法的裸声明 ——
    // 所以裸声明(不含 '{')在这里被包成 "类型名#objectName { ... }": 选择器正好指向该控件自己,
    // 语义与 Qt 的控件级表一致(只作用于本控件, 不外溢到兄弟节点)。
    // objectName 为空时无法安全限定作用域 —— 原样返回(解析会失败, 由调用方按需提示)。
    static std::string styleSheetText(const std::string& css, const std::string& className,
                                      const std::string& objectName);

    // 样式对象 -> CSS 串(三个后端共用; 与 JS 侧 uiLoader 的 __css 同口径):
    //   { backgroundColor: "#fff", borderWidth: 1 } -> "background-color:#fff;border-width:1;"
    // 注: 这是"JSON 节点里的 style 对象"; 写在 .ui(XML) 里的样式用 styleSheet 字符串属性(贴 Qt)。
    static std::string styleToCss(const Json::Value& style);

protected:
    // 给派生类(JZUiLoader / JZJSUiLoader)报错/读状态用 —— m_error 在基类里是 mutable 的。
    void setError(const std::string& msg) const { m_error = msg; }

    bool m_ok = false;
    mutable std::string m_error;
    std::string m_className = "Ui";
    JZUiNode m_root;

private:
    bool parseWidget(const class JZDomElement& el, JZUiNode& out, const std::string& path);
    void writeNode(const JZUiNode& node, Json::Value& out) const;
    void traverseNode(const JZUiNode& node, const JZUiNode* parent, const VisitFn& fn) const;
};

#endif // !JZ_UI_PARSER_H_
