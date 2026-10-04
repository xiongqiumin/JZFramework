// JZUiParser - UI definition dialect + Qt-style ui XML parsing.
//
// Shared by BOTH backends: JS side (JZJSEngine/JZJSUiLoader: generates a JS script) and
// C++ side (JZWidget/JZUiLoader: news real JZWidgets). Only the "generate widgets" half
// is per-backend; parse + dialect tables live here so the two cannot drift apart.
//
// (Moved out of JZJSEngine/JZJSUiLoader.cpp on 2026-10-04 so the C++ builder can live in JZWidget.)
#include "JZUiParser.h"
#include "JZXml/JZXml.h"

#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>

JZUiParser::JZUiParser() {}

// 已知控件类型表：覆盖 JZWidget 现有可创建控件。loader 默认不强制校验,
// 仅作为参考清单; 扩展新控件时在此追加即可。
static const std::vector<std::string>& builtinKnownTypes()
{
    static const std::vector<std::string> types = {
        "Window", "MainWindow", "Dialog", "Canvas",
        "PushButton", "Label", "ImageLabel", "Chart",
        "LineEdit", "TextEdit", "AbstractTextEdit",
        "CheckBox", "RadioButton", "Switch", "ComboBox",
        "ListWidget", "TreeWidget", "TableWidget",
        "Slider", "ScrollBar", "ProgressBar", "SpinBox", "DoubleSpinBox",
        "GroupBox", "TabWidget", "StackedWidget", "ScrollArea",
        "MenuBar", "Menu",
        "Layout", "HBoxLayout", "VBoxLayout", "GridLayout",
    };
    return types;
}

const std::vector<std::string>& JZUiParser::knownTypes()
{
    return builtinKnownTypes();
}

// ---------------------------------------------------------------------------
// 文本工具
// ---------------------------------------------------------------------------

static std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// 尝试把整串解析为数值(整数优先, 否则 double)。成功返回 true。
static bool parseNumber(const std::string& s, Json::Value& out)
{
    if (s.empty())
        return false;
    const char* c = s.c_str();
    char* end = nullptr;
    errno = 0;
    double d = std::strtod(c, &end);
    if (end == c || *end != '\0')
        return false;                 // 非纯数值
    if (!std::isfinite(d))
        return false;
    if (s.find_first_of(".eE") == std::string::npos) {
        out = Json::Value((Json::Int64)std::strtoll(c, nullptr, 10));
    } else {
        out = Json::Value(d);
    }
    return true;
}

// XML 属性文本 -> Json::Value: 布尔 / 数字 / 数字数组 / 字符串。
static Json::Value xmlPropToJson(const std::string& raw)
{
    std::string s = trim(raw);
    if (s.empty())
        return Json::Value("");
    if (s == "true" || s == "false")
        return Json::Value(s == "true");

    Json::Value num;
    if (parseNumber(s, num))
        return num;

    // 逗号分隔的数字列表 -> 数组(>=2 个元素才视为数组)
    if (s.find(',') != std::string::npos) {
        Json::Value arr(Json::arrayValue);
        size_t start = 0;
        bool allNum = true;
        while (true) {
            size_t comma = s.find(',', start);
            std::string part = trim(s.substr(start, comma - start));
            Json::Value v;
            if (parseNumber(part, v))
                arr.append(v);
            else {
                allNum = false;
                break;
            }
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }
        if (allNum && arr.size() >= 2)
            return arr;
    }
    return Json::Value(s);
}

// ---------------------------------------------------------------------------
// XML 解析
// ---------------------------------------------------------------------------

bool JZUiParser::load(const std::string& xmlText)
{
    m_error.clear();
    m_ok = false;
    m_root = JZUiNode();
    m_className = "Ui";

    JZDomDocument doc;
    std::string err;
    if (!doc.setContent(xmlText, &err)) {
        m_error = "XML 解析失败: " + err;
        return false;
    }

    JZDomElement ui = doc.documentElement();
    if (ui.isNull() || ui.tagName() != "ui") {
        m_error = "根元素必须是 <ui>";
        return false;
    }

    // 生成类名: <class> 元素文本
    JZDomNode classNode = ui.namedItem("class");
    if (!classNode.isNull() && classNode.isElement()) {
        std::string cn = trim(classNode.toElement().text());
        if (!cn.empty())
            m_className = cn;
    }

    // 根 widget: <ui> 下第一个直接 <widget> 子元素
    JZDomNodeList children = ui.childNodes();
    JZDomElement rootWidget;
    for (int i = 0; i < children.count(); ++i) {
        JZDomNode n = children.item(i);
        if (n.isElement() && n.toElement().tagName() == "widget") {
            rootWidget = n.toElement();
            break;
        }
    }
    if (rootWidget.isNull()) {
        m_error = "<ui> 缺少根 <widget>";
        return false;
    }

    if (!parseWidget(rootWidget, m_root, "ui/widget"))
        return false;

    m_ok = true;
    return true;
}

// ---- 菜单结构解析: <menubar> -> <menu title> -> <action>/<separator>/<menu> ----
// 这些不是渲染树控件(JZMenu/JZAction 是 JZObject 数据模型), 用专属标签与 <widget>
// 同层级挂在窗口下; 构造时由 addMenu/addAction/addSeparator 建立, 不进 __types。
static bool parseActionNode(const JZDomElement& el, JZUiNode& out, std::string& err)
{
    out.type = "Action";
    out.props = Json::Value(Json::objectValue);
    std::string text = trim(el.attribute("text"));
    if (text.empty())
        text = trim(el.text());
    if (!text.empty())
        out.props["text"] = text;
    if (el.hasAttribute("shortcut"))
        out.props["shortcut"] = trim(el.attribute("shortcut"));
    if (el.hasAttribute("checkable"))
        out.props["checkable"] = (trim(el.attribute("checkable")) == "true");
    if (el.hasAttribute("checked"))
        out.props["checked"] = (trim(el.attribute("checked")) == "true");
    if (el.hasAttribute("enabled"))
        out.props["enabled"] = (trim(el.attribute("enabled")) == "true");
    return true;
}

static bool parseMenuNode(const JZDomElement& el, JZUiNode& out, std::string& err, const std::string& path)
{
    out.type = "Menu";
    out.props = Json::Value(Json::objectValue);
    if (el.hasAttribute("title"))
        out.props["title"] = trim(el.attribute("title"));
    std::string id = el.attribute("name");
    if (id.empty())
        id = el.attribute("id");
    out.id = id;

    JZDomNodeList kids = el.childNodes();
    for (int i = 0; i < kids.count(); ++i) {
        JZDomNode n = kids.item(i);
        if (!n.isElement())
            continue;
        JZDomElement e = n.toElement();
        std::string tag = e.tagName();
        if (tag == "action") {
            JZUiNode child;
            if (!parseActionNode(e, child, err))
                return false;
            out.children.push_back(std::move(child));
        } else if (tag == "separator") {
            JZUiNode child;
            child.type = "Separator";
            out.children.push_back(std::move(child));
        } else if (tag == "menu") {
            JZUiNode child;
            if (!parseMenuNode(e, child, err, path + "/menu"))
                return false;
            out.children.push_back(std::move(child));
        }
    }
    return true;
}

static bool parseMenubarNode(const JZDomElement& el, JZUiNode& out, std::string& err, const std::string& path)
{
    out.type = "MenuBar";
    out.props = Json::Value(Json::objectValue);
    std::string id = el.attribute("name");
    if (id.empty())
        id = el.attribute("id");
    out.id = id;

    JZDomNodeList kids = el.childNodes();
    for (int i = 0; i < kids.count(); ++i) {
        JZDomNode n = kids.item(i);
        if (!n.isElement())
            continue;
        JZDomElement e = n.toElement();
        if (e.tagName() == "menu") {
            JZUiNode child;
            if (!parseMenuNode(e, child, err, path + "/MenuBar"))
                return false;
            out.children.push_back(std::move(child));
        }
    }
    return true;
}

bool JZUiParser::parseWidget(const JZDomElement& el, JZUiNode& out, const std::string& path)
{
    std::string type = el.attribute("class");
    if (type.empty())
        type = el.attribute("type");
    if (type.empty()) {
        m_error = path + ": <widget> 缺少 class 属性";
        return false;
    }
    out.type = type;

    // id: 优先 name(Qt 习惯), 兼容 id
    std::string id = el.attribute("name");
    if (id.empty())
        id = el.attribute("id");
    out.id = id;

    out.props = Json::Value(Json::objectValue);

    // 网格布局里的位置(Qt 的 <item row="1" column="1"> 由转换器写到 <widget> 上):
    // 只有父是 GridLayout 时才用得上, 其它布局忽略。兼容 column / col 两种写法。
    if (el.hasAttribute("row"))
        out.row = atoi(el.attribute("row").c_str());
    if (el.hasAttribute("column"))
        out.col = atoi(el.attribute("column").c_str());
    else if (el.hasAttribute("col"))
        out.col = atoi(el.attribute("col").c_str());

    // 盒子布局里的伸缩因子(Qt 的 <stretch>): <widget class="TableWidget" name="t" stretch="1"/>
    if (el.hasAttribute("stretch"))
        out.stretch = atoi(el.attribute("stretch").c_str());

    JZDomNodeList kids = el.childNodes();
    for (int i = 0; i < kids.count(); ++i) {
        JZDomNode n = kids.item(i);
        if (!n.isElement())
            continue;
        JZDomElement e = n.toElement();
        std::string tag = e.tagName();

        if (tag == "property") {
            std::string pname = e.attribute("name");
            if (pname.empty()) {
                m_error = path + ": <property> 缺少 name 属性";
                return false;
            }
            std::string val;
            if (e.hasAttribute("value"))
                val = e.attribute("value");
            else
                val = e.text();
            out.props[pname] = xmlPropToJson(val);
        } else if (tag == "widget") {
            JZUiNode child;
            if (!parseWidget(e, child, path + "/" + type))
                return false;
            out.children.push_back(std::move(child));
        } else if (tag == "menubar") {
            // 菜单栏: 专属标签, 与 <widget> 同层级挂在窗口下; 内部用 <menu>/<action>/<separator>
            JZUiNode child;
            if (!parseMenubarNode(e, child, m_error, path + "/MenuBar"))
                return false;
            out.children.push_back(std::move(child));
        } else if (tag == "item") {
            // 列表型子项(ComboBox / ListWidget 用 addItem 添加):
            //   <item>foo</item> / <item text="foo"/> / <item><property name="text">foo</property></item>
            std::string text;
            if (e.hasAttribute("text")) {
                text = trim(e.attribute("text"));
            } else {
                bool found = false;
                JZDomNodeList ik = e.childNodes();
                for (int k = 0; k < ik.count(); ++k) {
                    JZDomNode nn = ik.item(k);
                    if (nn.isElement() && nn.toElement().tagName() == "property"
                        && nn.toElement().attribute("name") == "text") {
                        text = trim(nn.toElement().text());
                        found = true;
                        break;
                    }
                }
                if (!found)
                    text = trim(e.text());
            }
            out.items.push_back(text);
        }
        // 其它子元素(layout/等等)目前忽略
    }
    return true;
}

bool JZUiParser::loadFromFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        m_ok = false;
        m_error = "无法打开文件: " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return load(ss.str());
}

// ---------------------------------------------------------------------------
// 遍历 / 查询 / 序列化
// ---------------------------------------------------------------------------

void JZUiParser::traverse(const VisitFn& fn) const
{
    if (m_ok)
        traverseNode(m_root, nullptr, fn);
}

void JZUiParser::traverseNode(const JZUiNode& node, const JZUiNode* parent, const VisitFn& fn) const
{
    fn(node, parent);
    for (const auto& c : node.children)
        traverseNode(c, &node, fn);
}

std::vector<const JZUiNode*> JZUiParser::nodesById(const std::string& id) const
{
    std::vector<const JZUiNode*> res;
    traverse([&](const JZUiNode& n, const JZUiNode*) {
        if (n.id == id)
            res.push_back(&n);
    });
    return res;
}

void JZUiParser::toJson(Json::Value& out) const
{
    out = Json::Value(Json::objectValue);
    writeNode(m_root, out);
}

void JZUiParser::writeNode(const JZUiNode& node, Json::Value& out) const
{
    out["type"] = node.type;
    if (!node.id.empty())
        out["id"] = node.id;
    if (!node.props.isNull())
        out["props"] = node.props;
    if (!node.children.empty()) {
        Json::Value arr(Json::arrayValue);
        for (const auto& c : node.children) {
            Json::Value item(Json::objectValue);
            writeNode(c, item);
            arr.append(item);
        }
        out["children"] = arr;
    }
    if (!node.items.empty()) {
        Json::Value arr(Json::arrayValue);
        for (const auto& it : node.items)
            arr.append(it);
        out["items"] = arr;
    }
}

// ---------------------------------------------------------------------------
// 生成 @jz/widget 风格的 TypeScript 类(Qt uic 风格)
// ---------------------------------------------------------------------------


// ---------------- dialect tables ----------------
namespace {

// UI 短类型名 -> 引擎已注册的 JS 构造器名(JZJSWidgetModule 注册的 C++ 类)。
// 仅覆盖已注册控件; 含未注册类型时 createUi 直接失败(见下方校验)。
const std::map<std::string, std::string>& uiClassMap()
{
    static const std::map<std::string, std::string> m = {
        { "Window",       "JZWindow" },
        { "MainWindow",   "JZMainWindow" },
        // Qt 的 QDialog -> JZDialog(有 exec/accept/reject; JZDialog 派生自 JZWindow, 所以
        // setTitle/setGeometry 这些照样有)。原来映射到 JZWindow, 转换出来的对话框没有模态能力。
        { "Dialog",       "JZDialog" },
        { "Canvas",       "JZCanvas" },
        { "Label",        "JZLabel" },
        { "ImageLabel",   "JZLabel" },
        { "PushButton",   "JZPushButton" },
        { "LineEdit",     "JZLineEdit" },
        { "CheckBox",     "JZCheckbox" },
        { "RadioButton",  "JZRadioButton" },
        { "ComboBox",     "JZComboBox" },
        { "Slider",       "JZSlider" },
        { "ProgressBar",  "JZProgressBar" },
        { "SpinBox",      "JZSpinBox" },
        // 小数步进框(对齐 Qt QDoubleSpinBox; 见 JZWidget/JZDoubleSpinBox.h)。⚠ 与 DateEdit 同理:
        // JS 侧还没有 JZDoubleSpinBox 绑定 ⇒ 这条映射目前只有 C++ 后端用得上。转换器现在把
        // QDoubleSpinBox 降级成 SpinBox(小数被截断, 见各 .ui.report.txt), 所以现有 JS 的 .ui 里
        // 不出现 DoubleSpinBox; 真出现会明确报"未知控件类型"。
        { "DoubleSpinBox", "JZDoubleSpinBox" },
        { "GroupBox",     "JZGroupBox" },
        // 日期编辑框(接口先落地, 实现留空; 见 JZWidget/JZDateEdit.h)。
        // ⚠ JS 侧还没有 JZDateEdit 绑定 ⇒ 这条映射目前只有 C++ 后端用得上; JS 侧的 .ui 里
        //    出现 DateEdit 会明确报"未知控件类型"(转换器现在是把 QDateEdit 降级成 LineEdit 的)。
        { "DateEdit",     "JZDateEdit" },
        { "Chart",        "JZChart" },
        { "Layout",       "JZLayout" },
        { "VBoxLayout",   "JZVBoxLayout" },
        { "HBoxLayout",   "JZHBoxLayout" },
        { "GridLayout",   "JZGridLayout" },
        // ---- 补齐(2026-09-14): 原表只覆盖了一部分已注册控件, 缺的会让 createUi 直接失败
        //      ("未知控件类型: xxx(引擎未注册该类)"), 而它们在 JZJSWidgetModule 里其实都注册了。
        //      注1: 只有**树里实际用到**的类型才会写进生成脚本, 所以给全没有副作用(未用到的映射是惰性的);
        //      注2: 映射目标必须是真正注册过的类, 否则用到时会 undefined —— 下表均为已注册控件。
        { "Widget",        "JZWidget" },
        { "Switch",        "JZSwitch" },
        { "ListWidget",    "JZListWidget" },
        { "TableWidget",   "JZTableWidget" },
        { "StackedWidget", "JZStackedWidget" },
        { "TabWidget",     "JZTabWidget" },
        { "TextEdit",      "JZTextEdit" },
        { "ScrollArea",    "JZScrollArea" },
        { "ScrollBar",     "JZScrollBar" },
        { "Splitter",      "JZSplitter" },
        { "MenuBar",       "JZMenuBar" },
        { "Menu",          "JZMenu" },
        { "Action",        "JZAction" },
        // 树控件: C++ 侧有 JZTreeWidget(JZUiLoader::createWidget 已接), 但 JS 侧**尚未绑定**这个类,
        // 所以下面的映射目前只有 C++ 后端用得上(JS 的 .ui 里出现 TreeWidget 会明确失败)。
        // 现状: 转换器把 QTreeWidget 降级成 TableWidget, 现有 JS 的 .ui 里不出现 TreeWidget。
        { "TreeWidget",   "JZTreeWidget" },
    };
    return m;
}

bool uiIsWindow(const std::string& t) {
    return t == "Window" || t == "MainWindow" || t == "Dialog";
}
bool uiIsLayout(const std::string& t) {
    return t == "Layout" || t == "VBoxLayout" || t == "HBoxLayout" || t == "GridLayout";
}
// 布局 margin 属性名 -> 下标(0=左 1=上 2=右 3=下); 不是 margin 返回 -1。
// Qt 是四个独立属性, 引擎是一次性 setContentsMargins(l,t,r,b), 见 genNode 里的整组发送。
int uiLayoutMarginIndex(const std::string& key) {
    if (key == "leftMargin") return 0;
    if (key == "topMargin") return 1;
    if (key == "rightMargin") return 2;
    if (key == "bottomMargin") return 3;
    return -1;
}
bool uiHasText(const std::string& t) {
    return t == "Label" || t == "ImageLabel" || t == "PushButton" ||
           t == "LineEdit" || t == "CheckBox" || t == "RadioButton" ||
           t == "DateEdit";   // DateEdit 派生自 JZLineEdit, setText 可用
}
bool uiHasTitle(const std::string& t) {
    return uiIsWindow(t) || t == "GroupBox" || t == "Chart";
}
// 下面几个白名单, 逐个对应 JZJSWidgetModule 里**真实绑定了 setter** 的控件类型。
// 没绑定的一律不放行(否则生成的脚本一跑就抛"方法不存在"), 所以白名单必须与那边保持一致:
//   setCurrentIndex: ComboBox / TabWidget / StackedWidget / ListWidget
//   setChecked:      CheckBox / RadioButton
//   setReadOnly:     LineEdit / TextEdit / SpinBox
//   setPlaceholderText: LineEdit / TextEdit
//   setMinimum/setMaximum: 目前只有 SpinBox
bool uiHasCurrentIndex(const std::string& t) {
    return t == "ComboBox" || t == "TabWidget" ||
           t == "StackedWidget" || t == "ListWidget";
}
// 注: ComboBox 也支持 setCurrentIndex(已并入上面的白名单), 这样 <property name="currentIndex">
//     能在生成脚本里正确落到 addItem 之后(项齐了再选, 否则索引被夹成 -1)。
bool uiHasChecked(const std::string& t) {
    return t == "CheckBox" || t == "RadioButton";
}
bool uiHasReadOnly(const std::string& t) {
    return t == "LineEdit" || t == "TextEdit" || t == "SpinBox"
        || t == "DoubleSpinBox" || t == "DateEdit";
}
bool uiHasPlaceholder(const std::string& t) {
    return t == "LineEdit" || t == "TextEdit";
}
bool uiHasRange(const std::string& t) {
    return t == "SpinBox" || t == "DoubleSpinBox";
}
// 小数步进框 / 日期框的**专属属性**: 这两类的 setter 在 C++ loader 与 JS 生成器两端都已就位
// (见 JZUiLoader::applyProps 与 JZJSUiLoader 的 genNode), 所以按类型名判即可 —— 用途与上面几张
// 白名单一样: 决定某个属性该不该落到真控件/生成脚本上。
bool uiIsDoubleSpin(const std::string& t) { return t == "DoubleSpinBox"; }
bool uiIsDateEdit(const std::string& t) {
    return t == "DateEdit";
}
// 注: DoubleSpinBox 独有的属性(value/singleStep/decimals/prefix/suffix)不在这里开白名单 ——
//     它们在 JZUiLoader::applyProps 里直接按 `dynamic_cast<JZDoubleSpinBox*>` 落, 因为 JS 侧
//     根本没有这个类(不存在"调用不存在的 setter"的风险), 也就没法和 JS 生成器同口径一说。

// 子控件挂载方式: 按**父控件类型**决定一个 <widget> 子节点如何挂上去。
// 通用容器(JZWidget / GroupBox 等)用 setParentWidget; 窗口用 addWidget;
// 下面这些控件有**专属**接口, 不能用通用布局/setParentWidget:
//   MainWindow    -> 单中央控件 setCentralWidget(菜单栏用 addMenuBar)
// 注: UiMount 的枚举定义已上移到 TsGen 之前(两个 generator 都要在文件前部看到它)。


UiMount uiMountMode(const std::string& t)
{
    if (t == "MainWindow")    return UiMount::Central;
    if (t == "StackedWidget") return UiMount::Stack;
    if (t == "Splitter")      return UiMount::Splitter;
    if (t == "TabWidget")     return UiMount::Tab;
    if (t == "ScrollArea")    return UiMount::Scroll;
    if (t == "ComboBox" || t == "ListWidget" || t == "TableWidget" || t == "TreeWidget")
        return UiMount::None;   // 视图类控件: 子节点不往它身上挂(数据由代码填)
    if (uiIsWindow(t))        return UiMount::WindowAdd;   // Window / Dialog(不含 MainWindow)
    return UiMount::Parent;                           // JZWidget / GroupBox / ...
}

// 接受 <layout> 子控件的父控件: 只有"通用布局宿主"(Window/Dialog/Widget/GroupBox)。
// MainWindow/Stacked/Splitter/Tab/Scroll/非容器都不接受 setLayout(各自有固定内部布局)。
bool uiAcceptsLayout(const std::string& t)
{
    UiMount m = uiMountMode(t);
    return m == UiMount::Parent || m == UiMount::WindowAdd;
}

// 列表型控件(用 <item> 子元素而非 widget 子控件): ComboBox / ListWidget
bool uiIsItemHost(const std::string& t)
{
    return t == "ComboBox" || t == "ListWidget";
}

// 菜单结构节点(Menu/Action/Separator)由 addMenu/addAction/addSeparator 构造,
// 不是经 __mk 创建的控件类, 不进 __types, 也不参与"未知类型"校验。
bool uiIsMenuStructure(const std::string& t)
{
    return t == "Menu" || t == "Action" || t == "Separator";
}

// TabWidget.addTab 的标签: 取子控件的 title 属性, 否则 id, 否则空串
std::string uiTabLabel(const JZUiNode& child)
{
    if (child.props.isObject() && child.props.isMember("title")
        && child.props["title"].isString())
        return child.props["title"].asString();
    return child.id;
}

// 文本类属性(text/title/placeholderText)要**强制按字符串**取值:
// xmlPropToJson 会把纯数字文本("1000")推断成整数, 若直接字面量化就成了 setText(1000),
// 而 setText/setTitle 只收 string —— 会抛"参数类型"错误。这里统一转成字符串。
std::string uiPropText(const Json::Value& v) {
    if (v.isString())
        return v.asString();
    if (v.isInt())
        return std::to_string(v.asInt());
    if (v.isDouble())
        return std::to_string(v.asDouble());
    if (v.isBool())
        return v.asBool() ? std::string("true") : std::string("false");
    return std::string();
}
}  // namespace

// ---------------------------------------------------------------------------
// 方言表/判别: 给 C++ 侧 JZUiLoader(JZUiLoader.h)复用
// ---------------------------------------------------------------------------
// 这里只是把上面那几张内部表(uiClassMap / uiMountMode / uiHasXxx / ...)透出来 ——
// 于是"JS 生成脚本"和"C++ 直接 new 控件"两个后端用的是**同一份方言判别**。
std::string JZUiParser::classForType(const std::string& type)
{
    const std::map<std::string, std::string>& m = uiClassMap();
    std::map<std::string, std::string>::const_iterator it = m.find(type);
    return (it == m.end()) ? std::string() : it->second;
}

bool JZUiParser::isLayoutType(const std::string& t) { return uiIsLayout(t); }
bool JZUiParser::isWindowType(const std::string& t) { return uiIsWindow(t); }
bool JZUiParser::isMenuStructure(const std::string& t) { return uiIsMenuStructure(t); }
bool JZUiParser::acceptsLayout(const std::string& t) { return uiAcceptsLayout(t); }
bool JZUiParser::isItemHost(const std::string& t) { return uiIsItemHost(t); }
UiMount JZUiParser::mountMode(const std::string& t) { return uiMountMode(t); }
std::string JZUiParser::tabLabel(const JZUiNode& child) { return uiTabLabel(child); }
bool JZUiParser::hasText(const std::string& t) { return uiHasText(t); }
bool JZUiParser::hasTitle(const std::string& t) { return uiHasTitle(t); }
bool JZUiParser::hasCurrentIndex(const std::string& t) { return uiHasCurrentIndex(t); }
bool JZUiParser::hasChecked(const std::string& t) { return uiHasChecked(t); }
bool JZUiParser::hasReadOnly(const std::string& t) { return uiHasReadOnly(t); }
bool JZUiParser::hasPlaceholder(const std::string& t) { return uiHasPlaceholder(t); }
bool JZUiParser::hasRange(const std::string& t) { return uiHasRange(t); }
bool JZUiParser::isDoubleSpin(const std::string& t) { return uiIsDoubleSpin(t); }
bool JZUiParser::isDateEdit(const std::string& t) { return uiIsDateEdit(t); }

std::string JZUiParser::styleSheetText(const std::string& css, const std::string& className,
                                       const std::string& objectName)
{
    if (css.empty() || css.find('{') != std::string::npos)
        return css;                       // 已经是规则表 / 空: 原样用

    std::string cls = className.empty() ? std::string("*") : className;
    if (objectName.empty())
        return css;                       // 没名字没法限定到"这一个"控件, 交给上层处理

    return cls + "#" + objectName + " { " + css + " }";
}

std::string JZUiParser::styleToCss(const Json::Value& style)
{
    std::string out;
    if (!style.isObject())
        return out;

    // 驼峰 -> kebab: backgroundColor -> background-color
    for (Json::Value::const_iterator it = style.begin(); it != style.end(); ++it)
    {
        const std::string k = it.name();
        std::string key;
        for (size_t i = 0; i < k.size(); ++i)
        {
            if (k[i] >= 'A' && k[i] <= 'Z')
            {
                key += '-';
                key += (char)(k[i] - 'A' + 'a');
            }
            else
            {
                key += k[i];
            }
        }
        out += key + ":" + propText(*it) + ";";
    }
    return out;
}
int JZUiParser::layoutMarginIndex(const std::string& key) { return uiLayoutMarginIndex(key); }
std::string JZUiParser::propText(const Json::Value& v) { return uiPropText(v); }

const std::map<std::string, std::string>& JZUiParser::typeMap()
{
    return uiClassMap();
}
