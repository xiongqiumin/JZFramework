// JZUiCppGen —— .ui -> C++ 代码生成器(见头文件说明)。
// 实现要点: 每个"生成什么语句"的判断都走 JZUiParser 的同一套方言表(类映射 / 挂载方式 /
// tabLabel / 属性白名单 / margin 下标), 与 JZUiLoader(运行期建控件)保持同一口径。
#include "JZUiCppGen.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

namespace {

// 引擎类名 -> 头文件名(一个类一个头; 与 JZWidget 目录一致)
std::string headerForClass(const std::string& cls)
{
    static const std::map<std::string, std::string> m = {
        { "JZWidget",        "JZWidget.h" },
        { "JZWindow",        "JZWindow.h" },
        { "JZMainWindow",    "JZMainWindow.h" },
        { "JZDialog",        "JZDialog.h" },
        { "JZLabel",         "JZLabel.h" },
        { "JZImageLabel",    "JZImageLabel.h" },
        { "JZPushButton",    "JZPushButton.h" },
        { "JZLineEdit",      "JZLineEdit.h" },
        { "JZDateEdit",      "JZDateEdit.h" },
        { "JZTextEdit",      "JZTextEdit.h" },
        { "JZCheckbox",      "JZCheckbox.h" },
        { "JZRadioButton",   "JZRadioButton.h" },
        { "JZSwitch",        "JZSwitch.h" },
        { "JZComboBox",      "JZComboBox.h" },
        { "JZSpinBox",       "JZSpinBox.h" },
        { "JZDoubleSpinBox", "JZDoubleSpinBox.h" },
        { "JZSlider",        "JZSlider.h" },
        { "JZProgressBar",   "JZProgressBar.h" },
        { "JZGroupBox",      "JZGroupBox.h" },
        { "JZListWidget",    "JZListWidget.h" },
        { "JZTableWidget",   "JZTableWidget.h" },
        { "JZTreeWidget",    "JZTreeWidget.h" },
        { "JZStackedWidget", "JZStackedWidget.h" },
        { "JZTabWidget",     "JZTabWidget.h" },
        { "JZSplitter",      "JZSplitter.h" },
        { "JZScrollArea",    "JZScrollArea.h" },
        { "JZScrollBar",     "JZScrollBar.h" },
        { "JZChart",         "JZChart.h" },
        { "JZMenuBar",       "JZMenuBar.h" },
        { "JZMenu",          "JZMenu.h" },
        { "JZAction",        "JZAction.h" },
        { "JZLayout",        "JZLayout.h" },   // 布局族(VBox/HBox/Grid)都在这个头里
    };
    std::map<std::string, std::string>::const_iterator it = m.find(cls);
    return (it == m.end()) ? std::string() : it->second;
}

// 少数控件没有默认构造(与 JZUiLoader::createWidget 同口径: JZStackedWidget 必须给 parent)
std::string ctorArgsFor(const std::string& cls)
{
    if (cls == "JZStackedWidget")
        return "nullptr";
    return std::string();
}

// C++ 字符串字面量转义(保留 UTF-8 原字节; 生成的文件带 BOM, MSVC 按 UTF-8 读)
std::string esc(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); ++i)
    {
        const char c = s[i];
        switch (c)
        {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:   out += c;      break;
        }
    }
    return out;
}

// 整数 / 小数 / 布尔
std::string litInt(int v) { return std::to_string(v); }
std::string litBool(bool v) { return v ? "true" : "false"; }
std::string litDouble(double v)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%.15g", v);   // 0.5 -> "0.5", 1e6 -> "1000000"
    std::string s = buf;
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos
        && s.find("inf") == std::string::npos && s.find("nan") == std::string::npos)
        s += ".0";                            // 明确写成小数, 免得落到 int 重载
    return s;
}

} // namespace

JZUiCppGen::JZUiCppGen(const JZUiNode& root, const std::string& uiClass)
    : m_root(root)
    , m_uiClass(uiClass.empty() ? std::string("GeneratedUi") : uiClass)
{
}

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------
void JZUiCppGen::line(const std::string& s)
{
    m_body += "        " + s + "\n";
}

std::string JZUiCppGen::newVar()
{
    return "w" + std::to_string(m_varCounter++);
}

std::string JZUiCppGen::memberName(const std::string& id) const
{
    std::string out;
    out.reserve(id.size());
    for (size_t i = 0; i < id.size(); ++i)
    {
        const unsigned char c = (unsigned char)id[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        out += ok ? (char)c : '_';
    }
    if (out.empty())
        out = "w";
    if (out[0] >= '0' && out[0] <= '9')
        out = "id_" + out;
    return out;
}

// ---------------------------------------------------------------------------
// 预扫: 成员表 + 头文件 + 结构校验(未知类型 / 布局嵌套 / 非容器挂子控件)
// ---------------------------------------------------------------------------
void JZUiCppGen::collectMembers(const JZUiNode& node)
{
    if (!m_error.empty())
        return;

    if (JZUiParser::isLayoutType(node.type))
    {
        m_headers.push_back("JZLayout.h");
        if (!node.id.empty())
        {
            const std::string lmem = memberName(node.id);
            if (m_members.find("* " + lmem + " = nullptr;") == std::string::npos)
                m_members += "    JZLayout* " + lmem + " = nullptr;\n";
        }

        // 布局下面的控件照样要收(成员 + 头文件); 只拦"布局嵌套布局"(引擎不支持)
        for (size_t i = 0; i < node.children.size(); ++i)
        {
            const JZUiNode& c = node.children[i];
            if (JZUiParser::isLayoutType(c.type))
            {
                setError("暂不支持布局嵌套布局(父 " + node.type + " \"" + node.id
                         + "\" > 子 \"" + c.id + "\"), 请在转换阶段把内层布局拍平");
                return;
            }
            collectMembers(c);
            if (!m_error.empty())
                return;
        }
        return;
    }

    const std::string cls = JZUiParser::classForType(node.type);
    if (cls.empty())
    {
        setError("未知控件类型: " + node.type + (node.id.empty() ? "" : " (\"" + node.id + "\")"));
        return;
    }

    const std::string h = headerForClass(cls);
    if (h.empty())
    {
        setError("生成器没有 " + cls + " 的头文件映射(补 JZUiCppGen.cpp 的 headerForClass)");
        return;
    }
    m_headers.push_back(h);

    if (!node.id.empty())
    {
        const std::string mem = memberName(node.id);
        if (m_members.find("* " + mem + " = nullptr;") == std::string::npos)
            m_members += "    " + cls + "* " + mem + " = nullptr;\n";
    }

    // 结构校验(与 JZUiLoader 同口径): 非容器不能挂控件子节点; 布局不能嵌布局
    const UiMount mode = JZUiParser::mountMode(node.type);
    for (size_t i = 0; i < node.children.size(); ++i)
    {
        const JZUiNode& c = node.children[i];

        if (JZUiParser::isLayoutType(c.type))
        {
            for (size_t k = 0; k < c.children.size(); ++k)
                if (JZUiParser::isLayoutType(c.children[k].type))
                {
                    setError("暂不支持布局嵌套布局(父 " + c.type + " \"" + c.id
                             + "\" > 子 \"" + c.children[k].id + "\"), 请在转换阶段把内层布局拍平");
                    return;
                }
            if (!JZUiParser::acceptsLayout(node.type))
            {
                setError(node.type + " 不支持通用布局(不能用 <layout> 子节点)");
                return;
            }
            collectMembers(c);   // 布局节点: 收集它下面的控件
            continue;
        }

        if (c.type == "MenuBar")
            continue;            // 菜单栏用专属构建(见 genMenuBar)

        if (mode == UiMount::None)
        {
            setError(node.type + " 不是容器控件, 不能挂 widget 子控件"
                     "(列表内容用 <item>, 其它数据走属性)");
            return;
        }

        collectMembers(c);
    }
}

// ---------------------------------------------------------------------------
// 属性
// ---------------------------------------------------------------------------
void JZUiCppGen::genProps(const JZUiNode& node, const std::string& var)
{
    const Json::Value& props = node.props;
    if (!props.isObject())
        return;

    const std::string& t = node.type;
    const std::string cls = JZUiParser::classForType(t);   // 给 styleSheet 的"类型#名字"规则用
    const bool doubleSpin = JZUiParser::isDoubleSpin(t);
    const bool itemHost = JZUiParser::isItemHost(t);   // 它的 currentIndex 要等 <item> 加完再发

    for (Json::Value::const_iterator it = props.begin(); it != props.end(); ++it)
    {
        const std::string key = it.name();
        const Json::Value& val = *it;

        if (key == "geometry" && val.isArray() && val.size() == 4)
        {
            line(var + "->setGeometry(" + litInt(val[0].asInt()) + ", " + litInt(val[1].asInt())
                 + ", " + litInt(val[2].asInt()) + ", " + litInt(val[3].asInt()) + ");");
        }
        else if (key == "visible" && val.isBool())
        {
            line(var + "->setVisible(" + litBool(val.asBool()) + ");");
        }
        else if (key == "styleSheet")
        {
            // 贴 Qt 的写法: 规则表与裸声明都收(裸声明在此处就包成规则, 与 JZUiLoader 一致)
            line(var + "->setStyleSheet(\"" + esc(JZUiParser::styleSheetText(
                     JZUiParser::propText(val), cls, node.id)) + "\");");
        }
        else if (key == "style" && val.isObject())
        {
            // JSON 节点路径的样式对象(与 JZUiLoader 同口径)
            line(var + "->setStyleSheet(\"" + esc(JZUiParser::styleToCss(val)) + "\");");
        }
        else if (key == "text" && JZUiParser::hasText(t))
        {
            line(var + "->setText(\"" + esc(JZUiParser::propText(val)) + "\");");
        }
        else if (key == "title" && JZUiParser::hasTitle(t))
        {
            line(var + "->setTitle(\"" + esc(JZUiParser::propText(val)) + "\");");
        }
        else if (key == "currentIndex" && JZUiParser::hasCurrentIndex(t) && val.isInt() && !itemHost)
        {
            line(var + "->setCurrentIndex(" + litInt(val.asInt()) + ");");
        }
        else if (key == "tabPosition" && JZUiParser::hasTabPosition(t) && val.isString())
        {
            // Qt 的 <enum>QTabWidget::West</enum> -> props 里是字符串; 生成 JZTabWidget::West
            const int pos = JZUiParser::parseTabPosition(val.asString());
            const std::string name = JZUiParser::tabPositionName(pos);
            if (!name.empty())
                line(var + "->setTabPosition(JZTabWidget::" + name + ");");
        }
        else if (key == "checked" && JZUiParser::hasChecked(t) && val.isBool())
        {
            line(var + "->setChecked(" + litBool(val.asBool()) + ");");
        }
        else if (key == "readOnly" && JZUiParser::hasReadOnly(t) && val.isBool())
        {
            line(var + "->setReadOnly(" + litBool(val.asBool()) + ");");
        }
        else if (key == "placeholderText" && JZUiParser::hasPlaceholder(t))
        {
            line(var + "->setPlaceholderText(\"" + esc(JZUiParser::propText(val)) + "\");");
        }
        else if (key == "minimum" && JZUiParser::hasRange(t) && (val.isInt() || val.isDouble()))
        {
            line(var + "->setMinimum(" + (doubleSpin ? litDouble(val.asDouble()) : litInt(val.asInt())) + ");");
        }
        else if (key == "maximum" && JZUiParser::hasRange(t) && (val.isInt() || val.isDouble()))
        {
            line(var + "->setMaximum(" + (doubleSpin ? litDouble(val.asDouble()) : litInt(val.asInt())) + ");");
        }
        else if (key == "value" && JZUiParser::hasRange(t) && (val.isInt() || val.isDouble()))
        {
            line(var + "->setValue(" + (doubleSpin ? litDouble(val.asDouble()) : litInt(val.asInt())) + ");");
        }
        else if (key == "suffix" && JZUiParser::hasRange(t))
        {
            line(var + "->setSuffix(\"" + esc(JZUiParser::propText(val)) + "\");");
        }
        else if (key == "specialValueText" && JZUiParser::hasRange(t))
        {
            line(var + "->setSpecialValueText(\"" + esc(JZUiParser::propText(val)) + "\");");
        }
        else if (key == "singleStep" && doubleSpin && (val.isInt() || val.isDouble()))
        {
            line(var + "->setSingleStep(" + litDouble(val.asDouble()) + ");");
        }
        else if (key == "decimals" && doubleSpin && val.isInt())
        {
            line(var + "->setDecimals(" + litInt(val.asInt()) + ");");
        }
        else if (key == "prefix" && doubleSpin)
        {
            line(var + "->setPrefix(\"" + esc(JZUiParser::propText(val)) + "\");");
        }
        else if (key == "dateMsecs" && JZUiParser::isDateEdit(t) && (val.isInt() || val.isDouble()))
        {
            line(var + "->setDate(JZDateTime::fromMSecsSinceEpoch("
                 + std::to_string((long long)val.asDouble()) + "LL));");
        }
        else if (key == "displayFormat" && JZUiParser::isDateEdit(t))
        {
            line(var + "->setDisplayFormat(\"" + esc(JZUiParser::propText(val)) + "\");");
        }
        else if (key == "calendarPopup" && JZUiParser::isDateEdit(t) && val.isBool())
        {
            line(var + "->setCalendarPopup(" + litBool(val.asBool()) + ");");
        }
        // 其余属性与 JZUiLoader 同口径: 丢弃(避免调用不存在的 setter)
    }
}

void JZUiCppGen::genLayoutProps(const JZUiNode& node, const std::string& layVar)
{
    const Json::Value& props = node.props;
    if (!props.isObject())
        return;

    int m[4] = { 0, 0, 0, 0 };
    bool hasMargin = false;

    for (Json::Value::const_iterator it = props.begin(); it != props.end(); ++it)
    {
        const std::string key = it.name();
        const Json::Value& val = *it;

        if (key == "spacing" && val.isInt())
        {
            line(layVar + "->setSpacing(" + litInt(val.asInt()) + ");");
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
        line(layVar + "->setContentsMargins(" + litInt(m[0]) + ", " + litInt(m[1])
             + ", " + litInt(m[2]) + ", " + litInt(m[3]) + ");");
}

// ---------------------------------------------------------------------------
// 布局
// ---------------------------------------------------------------------------
std::string JZUiCppGen::genLayout(const JZUiNode& node, const std::string& hostVar)
{
    std::string cls = JZUiParser::classForType(node.type);
    if (cls == "JZLayout")
        cls = "JZVBoxLayout";      // 基类抽象: 兜底按竖排(与 JZUiLoader::createLayout 同)

    const std::string var = newVar();
    line("JZLayout* " + var + " = new " + cls + "();");
    if (!node.id.empty())
        line(var + "->setObjectName(\"" + esc(node.id) + "\");");
    genLayoutProps(node, var);

    const bool grid = (node.type == "GridLayout");
    for (size_t i = 0; i < node.children.size(); ++i)
    {
        const JZUiNode& c = node.children[i];

        if (JZUiParser::isLayoutType(c.type))
        {
            setError("暂不支持布局嵌套布局(父 " + node.type + " \"" + node.id
                     + "\" > 子 \"" + c.id + "\"), 请在转换阶段把内层布局拍平");
            return var;
        }

        const std::string cv = genWidget(c);
        if (!m_error.empty())
            return var;

        if (grid)
        {
            line("static_cast<JZGridLayout*>(" + var + ")->addWidget(" + cv + ", "
                 + litInt(c.row) + ", " + litInt(c.col) + ");");
        }
        else
        {
            // 伸缩因子来自 <widget ... stretch="1">(Qt 的 <stretch>); 缺省 0 = 按 sizeHint 摆
            line("static_cast<JZBoxLayout*>(" + var + ")->addWidget(" + cv + ", "
                 + litInt(c.stretch) + ");");
        }

        // 布局不是控件: 父子关系挂到布局的宿主控件上(与 JZUiLoader::buildLayout 同)
        if (!hostVar.empty())
            line(cv + "->setParentWidget(" + hostVar + ");");
    }

    return var;
}

// ---------------------------------------------------------------------------
// 菜单栏(<menubar> -> <menu> -> <action>/<separator>, 可嵌套子菜单)
// ---------------------------------------------------------------------------
void JZUiCppGen::genMenuBar(const JZUiNode& menuBarNode, const std::string& windowVar)
{
    m_headers.push_back("JZMenuBar.h");
    m_headers.push_back("JZMenu.h");
    m_headers.push_back("JZAction.h");

    const std::string bar = newVar();
    line("JZMenuBar* " + bar + " = new JZMenuBar();");

    std::function<void(const JZUiNode&, const std::string&)> fillMenu =
        [&](const JZUiNode& menuNode, const std::string& menuVar)
    {
        for (size_t i = 0; i < menuNode.children.size(); ++i)
        {
            const JZUiNode& c = menuNode.children[i];
            const Json::Value& p = c.props;

            if (c.type == "Action")
            {
                const std::string text = p.isMember("text") ? JZUiParser::propText(p["text"]) : std::string();
                const std::string act = newVar();
                line("JZAction* " + act + " = " + menuVar + "->addAction(\"" + esc(text) + "\");");
                if (p.isMember("shortcut"))
                    line(act + "->setShortcut(\"" + esc(JZUiParser::propText(p["shortcut"])) + "\");");
                if (p.isMember("checkable"))
                    line(act + "->setCheckable(" + litBool(p["checkable"].asBool()) + ");");
                if (p.isMember("checked"))
                    line(act + "->setChecked(" + litBool(p["checked"].asBool()) + ");");
                if (p.isMember("enabled"))
                    line(act + "->setEnabled(" + litBool(p["enabled"].asBool()) + ");");
            }
            else if (c.type == "Separator")
            {
                line(menuVar + "->addSeparator();");
            }
            else if (c.type == "Menu")
            {
                const std::string title = p.isMember("title") ? JZUiParser::propText(p["title"]) : std::string();
                const std::string sub = newVar();
                line("JZMenu* " + sub + " = " + menuVar + "->addMenu(\"" + esc(title) + "\");");
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
        const std::string menu = newVar();
        line("JZMenu* " + menu + " = " + bar + "->addMenu(\"" + esc(title) + "\");");
        fillMenu(m, menu);
    }

    line(windowVar + "->addMenuBar(" + bar + ");");
}

// ---------------------------------------------------------------------------
// 子节点(先布局, 再控件 —— 与 JZUiLoader::buildChildren 同序)
// ---------------------------------------------------------------------------
void JZUiCppGen::genChildren(const JZUiNode& node, const std::string& selfVar)
{
    if (!m_error.empty())
        return;

    // 1) 布局
    for (size_t i = 0; i < node.children.size(); ++i)
    {
        const JZUiNode& c = node.children[i];
        if (!JZUiParser::isLayoutType(c.type))
            continue;

        if (!JZUiParser::acceptsLayout(node.type))
        {
            setError(node.type + " 不支持通用布局(不能用 <layout> 子节点)");
            return;
        }

        const std::string lay = genLayout(c, selfVar);
        if (!m_error.empty())
            return;
        line(selfVar + "->setLayout(" + lay + ");");
    }

    // 2) 控件(挂载方式按父类型)
    const UiMount mode = JZUiParser::mountMode(node.type);
    int centralCount = 0;

    for (size_t i = 0; i < node.children.size(); ++i)
    {
        const JZUiNode& c = node.children[i];

        if (JZUiParser::isLayoutType(c.type))
            continue;   // 上面处理过

        if (c.type == "MenuBar")
        {
            genMenuBar(c, selfVar);
            continue;
        }

        const std::string cv = genWidget(c);
        if (!m_error.empty())
            return;

        if (mode == UiMount::Central)
        {
            // 多余的中央控件与 JZUiLoader 同口径(引擎的 MainWindow 只接受一个): 忽略
            if (centralCount++ == 0)
                line(selfVar + "->setCentralWidget(" + cv + ");");
        }
        else if (mode == UiMount::Stack || mode == UiMount::Splitter)
        {
            line(selfVar + "->addWidget(" + cv + ");");
        }
        else if (mode == UiMount::Tab)
        {
            line(selfVar + "->addTab(" + cv + ", \"" + esc(JZUiParser::tabLabel(c)) + "\");");
        }
        else if (mode == UiMount::Scroll)
        {
            line(selfVar + "->setWidget(" + cv + ");");
            // 方言里的 ScrollArea 子控件 = Qt 的 widgetResizable=true(原版 bteserver 的 .ui 就这么设的):
            // 内容控件由滚动区按 sizeHint/最小尺寸定尺寸, 内容比视口大就出滚动条(不是把内容压小)。
            // 注: JS 侧生成器(JZJSUiLoader)还没发这条 —— 那边缺 setWidgetResizable 绑定。
            line(selfVar + "->setWidgetResizable(true);");
        }
        else if (mode == UiMount::WindowAdd || mode == UiMount::Parent)
        {
            line(cv + "->setParentWidget(" + selfVar + ");");
        }
        else // UiMount::None
        {
            setError(node.type + " 不是容器控件, 不能挂 widget 子控件(列表内容用 <item>)");
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// 一个控件节点
// ---------------------------------------------------------------------------
std::string JZUiCppGen::genWidget(const JZUiNode& node)
{
    const std::string cls = JZUiParser::classForType(node.type);
    if (cls.empty())
    {
        setError("未知控件类型: " + node.type + (node.id.empty() ? "" : " (\"" + node.id + "\")"));
        return std::string();
    }

    const std::string var = newVar();
    line(cls + "* " + var + " = new " + cls + "(" + ctorArgsFor(cls) + ");");

    if (!node.id.empty())
    {
        line(var + "->setObjectName(\"" + esc(node.id) + "\");");
        line("this->" + memberName(node.id) + " = " + var + ";");
        line("byId[\"" + esc(node.id) + "\"] = " + var + ";");
    }

    genProps(node, var);
    if (!m_error.empty())
        return var;

    // <item> 子项(ComboBox / ListWidget): 加完项再选 currentIndex(与 loader 同口径)
    if (JZUiParser::isItemHost(node.type) && !node.items.empty())
    {
        for (size_t i = 0; i < node.items.size(); ++i)
        {
            if (cls == "JZComboBox")
                line(var + "->addItem(\"" + esc(node.items[i]) + "\");");
            else if (cls == "JZListWidget")
                line(var + "->appendItem(\"" + esc(node.items[i]) + "\");");
        }

        const Json::Value& props = node.props;
        if (props.isObject() && props.isMember("currentIndex") && props["currentIndex"].isInt())
            line(var + "->setCurrentIndex(" + litInt(props["currentIndex"].asInt()) + ");");
    }

    genChildren(node, var);
    return var;
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------
// 同一份 .ui 里两个控件同名时, 生成代码会出现两个同名对象: `this->name` 与 `byId["name"]`
// 被后一个静默覆盖 —— 代码里看不出问题, 只有界面上"多出一份"才发现(bteserver 的 bottomRow 就
// 踩过这个坑)。这里统一改名: 第二个起叫 name_2 / name_3 ..., 并记一条警告 —— 代码里对该控件的
// 引用要跟着改, 所以是"警告 + 改名"而不是静默放过, 但也不至于让整个构建挂掉。
void JZUiCppGen::dedupNames(JZUiNode& node, std::set<std::string>& used)
{
    if (!node.id.empty())
    {
        if (used.find(node.id) != used.end())
        {
            const std::string original = node.id;
            std::string candidate;
            for (int n = 2; ; ++n)
            {
                candidate = original + "_" + std::to_string(n);
                if (used.find(candidate) == used.end())
                    break;
            }
            node.id = candidate;   // 成员名/byId/setObjectName 都跟着用新名

            m_warnings.push_back("控件重名: \"" + original + "\" 重复出现, 后一个已改名为 \""
                                 + candidate + "\"; 代码里引用 \"" + original + "\" 的地方请一并确认");
        }
        used.insert(node.id);
    }

    for (size_t i = 0; i < node.children.size(); ++i)
        dedupNames(node.children[i], used);
}

std::string JZUiCppGen::generate()
{
    // 先处理重名(只改内部副本 m_root, 调用方拿到的树不动)
    {
        std::set<std::string> used;
        dedupNames(m_root, used);
    }

    if (m_error.empty())
        collectMembers(m_root);
    if (!m_error.empty())
        return std::string();

    const std::string rootVar = genWidget(m_root);
    if (!m_error.empty())
        return std::string();
    line("root = " + rootVar + ";");

    // 头文件: 去重 + 排序(稳定输出, 便于 diff)
    std::sort(m_headers.begin(), m_headers.end());
    m_headers.erase(std::unique(m_headers.begin(), m_headers.end()), m_headers.end());

    std::string guard = "JZ_UI_" + m_uiClass + "_H";
    for (size_t i = 0; i < guard.size(); ++i)
        guard[i] = (char)std::toupper((unsigned char)guard[i]);

    std::ostringstream out;
    out << "// ============================================================================\n"
        << "// 本文件由 jz_uic 从 .ui 文件自动生成(见 CMake 函数 jz_add_ui) —— 请勿手改。\n"
        << "// 改界面请改 .ui 文件, 重新构建即会重新生成;\n"
        << "// 手写逻辑请写在宿主 .cpp 里(这个类只负责建控件 + 命名成员表)。\n"
        << "// ============================================================================\n"
        << "#ifndef " << guard << "\n"
        << "#define " << guard << "\n\n"
        << "#include <map>\n"
        << "#include <string>\n"
        << "#include \"JZWidget.h\"\n";
    for (size_t i = 0; i < m_headers.size(); ++i)
    {
        if (m_headers[i] == "JZWidget.h")
            continue;
        out << "#include \"" << m_headers[i] << "\"\n";
    }

    out << "\nnamespace Ui {\n\n"
        << "class " << m_uiClass << "\n"
        << "{\n"
        << "public:\n"
        << "    JZWidget* root = nullptr;   // 顶层控件(通常是 JZWindow / JZMainWindow)\n"
        << m_members
        << "    // 名字 -> 控件(与 JZUiLoader::Result::byId 同义; 没写 name 的控件不进表)\n"
        << "    std::map<std::string, JZWidget*> byId;\n\n"
        << "    void setupUi()\n"
        << "    {\n"
        << m_body
        << "    }\n"
        << "};\n\n"
        << "} // namespace Ui\n\n"
        << "#endif // " << guard << "\n";

    return out.str();
}
