// JZUiLoader —— C++ 侧的 UI 加载器(见 JZUiLoader.h 的说明)。
// 只做"生成控件": 解析/方言表全部复用基类 JZUiParser, 于是 JS 与 C++ 两个后端共用一份方言。
#include "JZUiLoader.h"

#include "JZWidget.h"
#include "JZWindow.h"
#include "JZMainWindow.h"
#include "JZDialog.h"
#include "JZLabel.h"
#include "JZPushButton.h"
#include "JZLineEdit.h"
#include "JZDateEdit.h"
#include "JZTextEdit.h"
#include "JZAbstractTextEdit.h"
#include "JZCheckbox.h"
#include "JZRadioButton.h"
#include "JZSwitch.h"
#include "JZComboBox.h"
#include "JZSpinBox.h"
#include "JZDoubleSpinBox.h"
#include "JZSlider.h"
#include "JZProgressBar.h"
#include "JZGroupBox.h"
#include "JZListWidget.h"
#include "JZTableWidget.h"
#include "JZTreeWidget.h"
#include "JZStackedWidget.h"
#include "JZTabWidget.h"
#include "JZSplitter.h"
#include "JZScrollArea.h"
#include "JZScrollBar.h"
#include "JZChart.h"
#include "JZImageLabel.h"
#include "JZMenuBar.h"
#include "JZMenu.h"
#include "JZAction.h"
#include "JZLayout.h"

#include <functional>

namespace {

// 样式串的生成已上收到 JZUiParser::styleToCss(三个后端共用一份口径, 见 JZUiParser.h)。

} // namespace

// ---------------------------------------------------------------------------
// 类型 -> 真控件
// ---------------------------------------------------------------------------
JZWidget* JZUiLoader::createWidget(const std::string& type)
{
    // 走同一张表(JZUiParser::classForType): 表里没有的类型直接当"未知控件类型"报错。
    const std::string cls = JZUiParser::classForType(type);
    if (cls.empty())
        return nullptr;

    if (cls == "JZWindow")        return new JZWindow();
    if (cls == "JZMainWindow")    return new JZMainWindow();
    if (cls == "JZDialog")        return new JZDialog();
    if (cls == "JZWidget")        return new JZWidget();
    if (cls == "JZLabel")         return new JZLabel();
    if (cls == "JZPushButton")    return new JZPushButton();
    if (cls == "JZLineEdit")      return new JZLineEdit();
    if (cls == "JZDateEdit")      return new JZDateEdit();   // 接口先落地(见 JZDateEdit.h)
    if (cls == "JZTextEdit")      return new JZTextEdit();
    if (cls == "JZCheckbox")      return new JZCheckbox();
    if (cls == "JZRadioButton")   return new JZRadioButton();
    if (cls == "JZSwitch")        return new JZSwitch();
    if (cls == "JZComboBox")      return new JZComboBox();
    if (cls == "JZSpinBox")       return new JZSpinBox();
    if (cls == "JZDoubleSpinBox") return new JZDoubleSpinBox();   // 小数步进框
    if (cls == "JZSlider")        return new JZSlider();
    if (cls == "JZProgressBar")   return new JZProgressBar();
    if (cls == "JZGroupBox")      return new JZGroupBox();
    if (cls == "JZListWidget")    return new JZListWidget();
    if (cls == "JZTableWidget")   return new JZTableWidget();
    if (cls == "JZTreeWidget")    return new JZTreeWidget();      // 树控件(JS 侧尚未绑定)
    if (cls == "JZStackedWidget") return new JZStackedWidget(nullptr);   // 它没有默认构造
    if (cls == "JZTabWidget")     return new JZTabWidget();
    if (cls == "JZSplitter")      return new JZSplitter();
    if (cls == "JZScrollArea")    return new JZScrollArea();
    if (cls == "JZScrollBar")     return new JZScrollBar();
    if (cls == "JZChart")         return new JZChart();
    return nullptr;   // 布局/菜单结构不走这里(它们在 buildLayout / buildMenuBar 里)
}

JZLayout* JZUiLoader::createLayout(const std::string& type)
{
    const std::string cls = JZUiParser::classForType(type);
    if (cls == "JZLayout")
        return new JZVBoxLayout();   // 基类是抽象的: 兜底按竖排(与"没写方向"的直觉一致)
    if (cls == "JZVBoxLayout") return new JZVBoxLayout();
    if (cls == "JZHBoxLayout") return new JZHBoxLayout();
    if (cls == "JZGridLayout") return new JZGridLayout();
    return nullptr;
}

// ---------------------------------------------------------------------------
// 属性(与 JS 侧白名单同一口径: 表里没有的属性一律丢弃, 不去调不存在的 setter)
// ---------------------------------------------------------------------------
void JZUiLoader::applyProps(const JZUiNode& node, JZWidget* w)
{
    if (!w)
        return;

    const Json::Value& props = node.props;
    if (!props.isObject())
        return;

    const std::string& t = node.type;
    for (Json::Value::const_iterator it = props.begin(); it != props.end(); ++it)
    {
        const std::string key = it.name();
        const Json::Value& val = *it;

        if (key == "geometry" && val.isArray() && val.size() == 4)
        {
            w->setGeometry(val[0].asInt(), val[1].asInt(), val[2].asInt(), val[3].asInt());
        }
        else if (key == "visible" && val.isBool())
        {
            w->setVisible(val.asBool());
        }
        else if (key == "styleSheet")
        {
            // 贴 Qt 的写法: 规则表("JZLabel { color:red }")与裸声明("color:red")都收,
            // 裸声明由 JZUiParser::styleSheetText 包成"类型#名字"规则(语义同 Qt 的控件级表)
            w->setStyleSheet(JZUiParser::styleSheetText(JZUiParser::propText(val),
                                                        JZUiParser::classForType(t), node.id));
        }
        else if (key == "style" && val.isObject())
        {
            // JSON 节点路径的样式对象(来自 JS 侧的数据树 / 程序化构造), 与上面等价
            w->setStyleSheet(JZUiParser::styleToCss(val));
        }
        else if (key == "text" && JZUiParser::hasText(t))
        {
            const std::string s = JZUiParser::propText(val);
            if (JZLabel* p = dynamic_cast<JZLabel*>(w))                        p->setText(s);
            else if (JZPushButton* p = dynamic_cast<JZPushButton*>(w))         p->setText(s);
            else if (JZCheckbox* p = dynamic_cast<JZCheckbox*>(w))             p->setText(s);
            else if (JZRadioButton* p = dynamic_cast<JZRadioButton*>(w))       p->setText(s);
            else if (JZAbstractTextEdit* p = dynamic_cast<JZAbstractTextEdit*>(w)) p->setText(s);
        }
        else if (key == "title" && JZUiParser::hasTitle(t))
        {
            const std::string s = JZUiParser::propText(val);
            if (JZGroupBox* p = dynamic_cast<JZGroupBox*>(w))   p->setTitle(s);
            else if (JZChart* p = dynamic_cast<JZChart*>(w))    p->setTitle(s);
            else if (JZWindow* p = dynamic_cast<JZWindow*>(w))  p->setTitle(s);
        }
        else if (key == "currentIndex" && JZUiParser::hasCurrentIndex(t) && val.isInt())
        {
            const int i = val.asInt();
            if (JZComboBox* p = dynamic_cast<JZComboBox*>(w))                 p->setCurrentIndex(i);
            else if (JZTabWidget* p = dynamic_cast<JZTabWidget*>(w))          p->setCurrentIndex(i);
            else if (JZStackedWidget* p = dynamic_cast<JZStackedWidget*>(w))  p->setCurrentIndex(i);
            else if (JZListWidget* p = dynamic_cast<JZListWidget*>(w))        p->setCurrentIndex(i);
        }
        else if (key == "tabPosition" && JZUiParser::hasTabPosition(t) && val.isString())
        {
            // Qt 的 .ui: <property name="tabPosition"><enum>QTabWidget::West</enum></property>
            // ⇒ props 里是字符串 "QTabWidget::West"(枚举前缀靠 parseTabPosition 剥掉)。
            const int pos = JZUiParser::parseTabPosition(val.asString());
            if (pos >= 0) {
                if (JZTabWidget* p = dynamic_cast<JZTabWidget*>(w))
                    p->setTabPosition((JZTabWidget::TabPosition)pos);
            }
        }
        else if (key == "checked" && JZUiParser::hasChecked(t) && val.isBool())
        {
            const bool on = val.asBool();
            if (JZCheckbox* p = dynamic_cast<JZCheckbox*>(w))            p->setChecked(on);
            else if (JZRadioButton* p = dynamic_cast<JZRadioButton*>(w)) p->setChecked(on);
        }
        else if (key == "readOnly" && JZUiParser::hasReadOnly(t) && val.isBool())
        {
            if (JZAbstractTextEdit* p = dynamic_cast<JZAbstractTextEdit*>(w))
                p->setReadOnly(val.asBool());
            else if (JZSpinBox* p = dynamic_cast<JZSpinBox*>(w))
                p->setReadOnly(val.asBool());
        }
        else if (key == "placeholderText" && JZUiParser::hasPlaceholder(t))
        {
            if (JZAbstractTextEdit* p = dynamic_cast<JZAbstractTextEdit*>(w))
                p->setPlaceholderText(JZUiParser::propText(val));
        }
        else if (key == "minimum" && JZUiParser::hasRange(t) && (val.isInt() || val.isDouble()))
        {
            // 先判小数框: JZDoubleSpinBox 派生自 JZSpinBox, 否则会被下面那支按整数收掉(小数截断)
            if (JZDoubleSpinBox* d = dynamic_cast<JZDoubleSpinBox*>(w))     d->setMinimum(val.asDouble());
            else if (JZSpinBox* p = dynamic_cast<JZSpinBox*>(w))            p->setMinimum(val.asInt());
        }
        else if (key == "maximum" && JZUiParser::hasRange(t) && (val.isInt() || val.isDouble()))
        {
            if (JZDoubleSpinBox* d = dynamic_cast<JZDoubleSpinBox*>(w))     d->setMaximum(val.asDouble());
            else if (JZSpinBox* p = dynamic_cast<JZSpinBox*>(w))            p->setMaximum(val.asInt());
        }
        // ---- 步进框通用: 值 / 后缀(整数框与小数框都有) ----
        else if (key == "value" && JZUiParser::hasRange(t) && (val.isInt() || val.isDouble()))
        {
            if (JZDoubleSpinBox* d = dynamic_cast<JZDoubleSpinBox*>(w))     d->setValue(val.asDouble());
            else if (JZSpinBox* p = dynamic_cast<JZSpinBox*>(w))            p->setValue(val.asInt());
        }
        else if (key == "suffix" && JZUiParser::hasRange(t))
        {
            if (JZSpinBox* p = dynamic_cast<JZSpinBox*>(w))                 p->setSuffix(JZUiParser::propText(val));
        }
        // ---- DoubleSpinBox 独有(Qt 同名; 见 JZUiParser::isDoubleSpin) ----
        else if (key == "singleStep" && JZUiParser::isDoubleSpin(t) && (val.isInt() || val.isDouble()))
        {
            if (JZDoubleSpinBox* d = dynamic_cast<JZDoubleSpinBox*>(w))     d->setSingleStep(val.asDouble());
        }
        else if (key == "decimals" && JZUiParser::isDoubleSpin(t) && val.isInt())
        {
            if (JZDoubleSpinBox* d = dynamic_cast<JZDoubleSpinBox*>(w))     d->setDecimals(val.asInt());
        }
        else if (key == "prefix" && JZUiParser::isDoubleSpin(t))
        {
            if (JZDoubleSpinBox* d = dynamic_cast<JZDoubleSpinBox*>(w))     d->setPrefix(JZUiParser::propText(val));
        }
        // ---- DateEdit 专属: 方言里日期是毫秒时间戳(与 JS 生成器同一口径, 见 JZJSUiLoader) ----
        else if (key == "dateMsecs" && JZUiParser::isDateEdit(t) && (val.isInt() || val.isDouble()))
        {
            if (JZDateEdit* d = dynamic_cast<JZDateEdit*>(w))
                d->setDate(JZDateTime::fromMSecsSinceEpoch((int64_t)val.asDouble()));
        }
        else if (key == "displayFormat" && JZUiParser::isDateEdit(t))
        {
            if (JZDateEdit* d = dynamic_cast<JZDateEdit*>(w))
                d->setDisplayFormat(JZUiParser::propText(val));
        }
        else if (key == "calendarPopup" && JZUiParser::isDateEdit(t) && val.isBool())
        {
            if (JZDateEdit* d = dynamic_cast<JZDateEdit*>(w))
                d->setCalendarPopup(val.asBool());
        }
        // 其余属性与 JS 侧同口径: 丢弃(避免调用不存在的 setter)
    }
}

void JZUiLoader::applyLayoutProps(const JZUiNode& node, JZLayout* lay)
{
    if (!lay)
        return;

    const Json::Value& props = node.props;
    if (!props.isObject())
        return;

    // 四边 margin: Qt 是四个独立属性, 引擎是一次性 setContentsMargins ⇒ 有任意一个就整组发
    // (缺的按 0), 与 JS 侧同一口径。
    int m[4] = {0, 0, 0, 0};
    bool hasMargin = false;

    for (Json::Value::const_iterator it = props.begin(); it != props.end(); ++it)
    {
        const std::string key = it.name();
        const Json::Value& val = *it;

        if (key == "spacing" && val.isInt())
        {
            lay->setSpacing(val.asInt());
        }
        else
        {
            const int idx = JZUiParser::layoutMarginIndex(key);
            if (idx >= 0 && val.isInt())
            {
                m[idx] = val.asInt();
                hasMargin = true;
            }
        }
    }

    if (hasMargin)
        lay->setContentsMargins(m[0], m[1], m[2], m[3]);
}

// ---------------------------------------------------------------------------
// 菜单栏: <menubar> -> <menu> -> <action>/<separator>(可嵌套子菜单)
// ---------------------------------------------------------------------------
bool JZUiLoader::buildMenuBar(const JZUiNode& menuBarNode, JZWidget* mainWindow)
{
    JZMainWindow* mw = dynamic_cast<JZMainWindow*>(mainWindow);
    if (!mw)
    {
        setError("JZUiLoader: <menubar> 的父控件不是 JZMainWindow");
        return false;
    }

    JZMenuBar* bar = new JZMenuBar();

    // 一个菜单节点的内容(Action/Separator/子 Menu); 递归填子菜单
    std::function<void(const JZUiNode&, JZMenu*)> fillMenu;
    fillMenu = [&](const JZUiNode& menuNode, JZMenu* menu)
    {
        for (size_t i = 0; i < menuNode.children.size(); ++i)
        {
            const JZUiNode& c = menuNode.children[i];
            const Json::Value& p = c.props;

            if (c.type == "Action")
            {
                const std::string text = p.isMember("text") ? JZUiParser::propText(p["text"]) : std::string();
                JZAction* a = menu->addAction(text);
                if (!a)
                    continue;
                if (p.isMember("shortcut"))
                    a->setShortcut(JZUiParser::propText(p["shortcut"]));
                if (p.isMember("checkable"))
                    a->setCheckable(p["checkable"].asBool());
                if (p.isMember("checked"))
                    a->setChecked(p["checked"].asBool());
                if (p.isMember("enabled"))
                    a->setEnabled(p["enabled"].asBool());
            }
            else if (c.type == "Separator")
            {
                menu->addSeparator();
            }
            else if (c.type == "Menu")
            {
                const std::string title = p.isMember("title") ? JZUiParser::propText(p["title"]) : std::string();
                JZMenu* sub = menu->addMenu(title);
                if (sub)
                    fillMenu(c, sub);
            }
        }
    };

    for (size_t i = 0; i < menuBarNode.children.size(); ++i)
    {
        const JZUiNode& m = menuBarNode.children[i];
        if (m.type != "Menu")
            continue;

        const std::string title = m.props.isMember("title")
            ? JZUiParser::propText(m.props["title"]) : std::string();
        JZMenu* menu = bar->addMenu(title);
        if (menu)
            fillMenu(m, menu);
    }

    mw->addMenuBar(bar);
    return true;
}

// ---------------------------------------------------------------------------
// 布局: 建布局 + 把它的子控件按格子/顺序加进去
// ---------------------------------------------------------------------------
JZLayout* JZUiLoader::buildLayout(const JZUiNode& layoutNode, JZWidget* host, Result& out)
{
    JZLayout* lay = createLayout(layoutNode.type);
    if (!lay)
    {
        setError("JZUiLoader: 未知布局类型 " + layoutNode.type
                 + " (\"" + layoutNode.id + "\")");
        return nullptr;
    }

    if (!layoutNode.id.empty())
        lay->setObjectName(layoutNode.id);
    applyLayoutProps(layoutNode, lay);

    const bool grid = (layoutNode.type == "GridLayout");
    for (size_t i = 0; i < layoutNode.children.size(); ++i)
    {
        const JZUiNode& c = layoutNode.children[i];

        if (JZUiParser::isLayoutType(c.type))
        {
            // 与 JS 侧同口径: 引擎布局没有 addLayout, 显式报错比静默排错好
            setError("JZUiLoader: 暂不支持布局嵌套布局(父 " + layoutNode.type + " \""
                     + layoutNode.id + "\" > 子 \"" + c.id + "\"), 请在转换阶段把内层布局拍平");
            delete lay;
            return nullptr;
        }

        JZWidget* child = buildWidget(c, nullptr, out);
        if (!child)
        {
            delete lay;
            return nullptr;
        }

        if (grid)
        {
            if (c.row < 0 || c.col < 0)
            {
                setError("JZUiLoader: GridLayout \"" + layoutNode.id + "\" 的子控件 \""
                         + c.id + "\" 缺 row/column 属性");
                delete lay;
                return nullptr;
            }
            static_cast<JZGridLayout*>(lay)->addWidget(child, c.row, c.col);
        }
        else
        {
            // 伸缩因子来自 <widget ... stretch="1">(Qt 的 <stretch>); 缺省 0 = 按 sizeHint 摆
            static_cast<JZBoxLayout*>(lay)->addWidget(child, c.stretch);
        }

        // 布局不是控件, 父子关系挂到布局的宿主控件上(与 JS 侧同一口径)
        if (host)
            child->setParentWidget(host);
    }

    return lay;
}

// ---------------------------------------------------------------------------
// 子控件挂载(规则见 UiMount / JZUiParser::mountMode)
// ---------------------------------------------------------------------------
bool JZUiLoader::buildChildren(const JZUiNode& node, JZWidget* self, Result& out)
{
    if (!self)
        return true;

    // 1) 布局子节点 -> setLayout(只有"通用布局宿主"接受)
    for (size_t i = 0; i < node.children.size(); ++i)
    {
        const JZUiNode& c = node.children[i];
        if (!JZUiParser::isLayoutType(c.type))
            continue;

        if (!JZUiParser::acceptsLayout(node.type))
        {
            setError("JZUiLoader: " + node.type + " 不支持通用布局(不能用 <layout> 子节点)");
            return false;
        }

        JZLayout* lay = buildLayout(c, self, out);
        if (!lay)
            return false;
        self->setLayout(lay);
    }

    // 2) 控件子节点 -> 按父类型的挂载方式挂
    const UiMount mode = JZUiParser::mountMode(node.type);
    int centralCount = 0;

    for (size_t i = 0; i < node.children.size(); ++i)
    {
        const JZUiNode& c = node.children[i];

        if (JZUiParser::isLayoutType(c.type))
            continue;   // 上面处理过

        if (c.type == "MenuBar")
        {
            if (!buildMenuBar(c, self))
                return false;
            continue;
        }

        JZWidget* child = buildWidget(c, nullptr, out);
        if (!child)
            return false;

        switch (mode)
        {
        case UiMount::Central:
        {
            JZMainWindow* mw = dynamic_cast<JZMainWindow*>(self);
            if (mw && centralCount++ == 0)
                mw->setCentralWidget(child);
            // 多余的子控件: 与 JS 侧同口径, 忽略(引擎的 MainWindow 只接受一个中央控件)
            break;
        }
        case UiMount::Stack:
            if (JZStackedWidget* s = dynamic_cast<JZStackedWidget*>(self))
                s->addWidget(child);
            break;
        case UiMount::Splitter:
            if (JZSplitter* s = dynamic_cast<JZSplitter*>(self))
                s->addWidget(child);
            break;
        case UiMount::Tab:
            if (JZTabWidget* s = dynamic_cast<JZTabWidget*>(self))
                s->addTab(child, JZUiParser::tabLabel(c));
            break;
        case UiMount::Scroll:
            if (JZScrollArea* s = dynamic_cast<JZScrollArea*>(self))
            {
                s->setWidget(child);
                // 方言里的 ScrollArea 子控件 = Qt 的 widgetResizable=true(原版 .ui 就这么设的):
                // 内容控件由滚动区按 sizeHint/最小尺寸定尺寸, 内容比视口大就出滚动条。
                s->setWidgetResizable(true);
            }
            break;
        case UiMount::WindowAdd:
        case UiMount::Parent:
            child->setParentWidget(self);
            break;
        case UiMount::None:
            setError("JZUiLoader: " + node.type + " 不是容器控件, 不能挂 widget 子控件"
                     "(列表内容用 <item>, 其它数据走属性)");
            delete child;
            return false;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// 一个控件节点: 建 -> 属性 -> <item> -> 子节点
// ---------------------------------------------------------------------------
JZWidget* JZUiLoader::buildWidget(const JZUiNode& node, JZWidget* owner, Result& out)
{
    if (JZUiParser::isLayoutType(node.type) || JZUiParser::isMenuStructure(node.type))
    {
        setError("JZUiLoader: 节点 " + node.type + " (\"" + node.id + "\") 不是控件节点");
        return nullptr;
    }

    JZWidget* w = createWidget(node.type);
    if (!w)
    {
        setError("JZUiLoader: 未知控件类型 " + node.type + " (\"" + node.id + "\")");
        return nullptr;
    }

    if (!node.id.empty())
    {
        w->setObjectName(node.id);       // 与 JS 侧同名: 名字既是对象名, 也进 Result::byId
        out.byId[node.id] = w;
    }

    applyProps(node, w);

    if (owner)
        w->setParentWidget(owner);

    // <item> 子项(ComboBox / ListWidget)
    if (JZUiParser::isItemHost(node.type) && !node.items.empty())
    {
        for (size_t i = 0; i < node.items.size(); ++i)
        {
            if (JZComboBox* cb = dynamic_cast<JZComboBox*>(w))
                cb->addItem(node.items[i]);
            else if (JZListWidget* lw = dynamic_cast<JZListWidget*>(w))
                lw->appendItem(node.items[i]);
        }

        // 项齐了再选一次 currentIndex(与 JS 侧同口径: 否则索引会被夹成 -1)
        const Json::Value& props = node.props;
        if (props.isObject() && props.isMember("currentIndex") && props["currentIndex"].isInt())
        {
            const int idx = props["currentIndex"].asInt();
            if (JZComboBox* cb = dynamic_cast<JZComboBox*>(w))
                cb->setCurrentIndex(idx);
            else if (JZListWidget* lw = dynamic_cast<JZListWidget*>(w))
                lw->setCurrentIndex(idx);
        }
    }

    if (!buildChildren(node, w, out))
    {
        delete w;   // 半成品不留(子控件挂在它下面, 一起析构)
        return nullptr;
    }

    return w;
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------
bool JZUiLoader::build(JZWidget* parent, Result& out)
{
    out.root = nullptr;
    out.byId.clear();

    const JZUiNode* rootNode = root();
    if (!rootNode)
    {
        setError("JZUiLoader: 还没有 load(或 load 失败) —— 先 load/loadFromFile");
        return false;
    }

    JZWidget* rootWidget = buildWidget(*rootNode, parent, out);
    if (!rootWidget)
        return false;

    out.root = rootWidget;
    return true;
}
