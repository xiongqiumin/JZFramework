// JZSUiLoader - JS side UI code generator (parse + dialect moved to JZWidget/JZUiParser).
#include "JZJSUiLoader.h"

#include "JZJSEngine.h"
#include "JZXml/JZXml.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// Dialect helpers below keep the generator body unchanged: the tables themselves now live in
// JZUiParser (JZWidget/JZUiParser.cpp); these forwarders are internal to this file.
namespace {
bool uiIsLayout(const std::string& t) { return JZUiParser::isLayoutType(t); }
UiMount uiMountMode(const std::string& t) { return JZUiParser::mountMode(t); }
bool uiAcceptsLayout(const std::string& t) { return JZUiParser::acceptsLayout(t); }
bool uiIsItemHost(const std::string& t) { return JZUiParser::isItemHost(t); }
bool uiIsWindow(const std::string& t) { return JZUiParser::isWindowType(t); }
bool uiIsMenuStructure(const std::string& t) { return JZUiParser::isMenuStructure(t); }
std::string uiTabLabel(const JZUiNode& c) { return JZUiParser::tabLabel(c); }
bool uiHasText(const std::string& t) { return JZUiParser::hasText(t); }
bool uiHasTitle(const std::string& t) { return JZUiParser::hasTitle(t); }
bool uiHasCurrentIndex(const std::string& t) { return JZUiParser::hasCurrentIndex(t); }
bool uiHasTabPosition(const std::string& t) { return JZUiParser::hasTabPosition(t); }
bool uiHasChecked(const std::string& t) { return JZUiParser::hasChecked(t); }
bool uiHasReadOnly(const std::string& t) { return JZUiParser::hasReadOnly(t); }
bool uiHasPlaceholder(const std::string& t) { return JZUiParser::hasPlaceholder(t); }
bool uiHasRange(const std::string& t) { return JZUiParser::hasRange(t); }
bool uiIsDoubleSpin(const std::string& t) { return JZUiParser::isDoubleSpin(t); }
bool uiIsDateEdit(const std::string& t) { return JZUiParser::isDateEdit(t); }
int uiLayoutMarginIndex(const std::string& k) { return JZUiParser::layoutMarginIndex(k); }
std::string uiPropText(const Json::Value& v) { return JZUiParser::propText(v); }
const std::map<std::string, std::string>& uiClassMap() { return JZUiParser::typeMap(); }
}

JZJSUiLoader::JZJSUiLoader() {}
// 生成 @jz/widget 风格的 TypeScript 类(Qt uic 风格)
// ---------------------------------------------------------------------------

namespace {

// 把 JSON 值序列化成 TS 字面量。
std::string tsLiteral(const Json::Value& v)
{
    if (v.isNull())
        return "null";
    if (v.isBool())
        return v.asBool() ? "true" : "false";
    if (v.isInt())
        return std::to_string(v.asInt());
    if (v.isUInt())
        return std::to_string(v.asUInt());
    if (v.isInt64())
        return std::to_string(v.asInt64());
    if (v.isUInt64())
        return std::to_string(v.asUInt64());
    if (v.isDouble()) {
        double d = v.asDouble();
        if (d == std::floor(d) && !std::isinf(d) && !std::isnan(d)) {
            std::ostringstream oss;
            oss << (long long)d;
            return oss.str();
        }
        std::ostringstream oss;
        oss << std::setprecision(15) << d;
        return oss.str();
    }
    if (v.isString()) {
        std::string out = "\"";
        const std::string& s = v.asString();
        for (char c : s) {
            switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\x%02x", (unsigned char)c);
                    out += buf;
                } else {
                    out += c;
                }
            }
        }
        out += "\"";
        return out;
    }
    if (v.isArray()) {
        std::string out = "[";
        for (Json::ArrayIndex i = 0; i < v.size(); ++i) {
            if (i) out += ", ";
            out += tsLiteral(v[i]);
        }
        out += "]";
        return out;
    }
    if (v.isObject()) {
        std::string out = "{";
        bool first = true;
        for (auto it = v.begin(); it != v.end(); ++it) {
            if (!first) out += ", ";
            first = false;
            out += tsLiteral(it.name()) + ": " + tsLiteral(*it);
        }
        out += "}";
        return out;
    }
    return "null";
}

// 把任意 id 规整成合法 TS 标识符。
std::string sanitizeId(const std::string& id)
{
    std::string s;
    for (char c : id) {
        if (std::isalnum((unsigned char)c) || c == '_')
            s += c;
        else
            s += '_';
    }
    if (!s.empty() && std::isdigit((unsigned char)s[0]))
        s = "_" + s;
    return s;
}

// 子控件挂载方式: UiMount 枚举已提到头文件(JZJSUiLoader.h) —— C++ 侧 JZUiLoader 也要用它。
// 按父控件类型决定挂载方式:
//   Window/Dialog -> setParentWidget          MainWindow -> setCentralWidget(菜单栏 addMenuBar)
//   StackedWidget -> addWidget(每页一个)       Splitter   -> addWidget(每子项一个)
//   TabWidget     -> addTab(每页 + label)      ScrollArea -> setWidget(单一内容控件)
//   通用容器      -> setParentWidget          非容器(ComboBox/ListWidget/TableWidget) -> 不接受

bool uiIsLayout(const std::string& t);
UiMount uiMountMode(const std::string& t);
bool uiAcceptsLayout(const std::string& t);
bool uiIsItemHost(const std::string& t);
std::string uiTabLabel(const JZUiNode& child);

// 递归生成 setupUi() 方法体(平坦语句), 同时收集带 id 的字段名。
struct TsGen {
    std::string out;
    int counter = 0;
    std::set<std::string> idFields;

    // 生成菜单树(菜单栏 / 子菜单里的 Menu 节点): addMenu + addAction/addSeparator。
    // 定义放在 genNode **之前**(它要被 genNode 调用; 这里不留"声明 + 类内后置定义"那套)。
    void genMenuTs(const JZUiNode& menu, int depth, const std::string& parentExpr)
    {
        std::string indent(depth * 2, ' ');
        std::string m = "_m" + std::to_string(counter++);
        std::string title = menu.props.isMember("title")
            ? tsLiteral(menu.props["title"].asString()) : "\"\"";
        out += indent + "const " + m + " = " + parentExpr + ".addMenu(" + title + ");\n";
        for (const auto& c : menu.children) {
            if (c.type == "Action") {
                std::string a = "_a" + std::to_string(counter++);
                std::string text = c.props.isMember("text")
                    ? tsLiteral(c.props["text"].asString()) : "\"\"";
                out += indent + "const " + a + " = " + m + ".addAction(" + text + ");\n";
                if (c.props.isMember("shortcut"))
                    out += indent + a + ".setShortcut(" + tsLiteral(c.props["shortcut"].asString()) + ");\n";
                if (c.props.isMember("checkable"))
                    out += indent + a + ".setCheckable(" + tsLiteral(c.props["checkable"]) + ");\n";
                if (c.props.isMember("checked"))
                    out += indent + a + ".setChecked(" + tsLiteral(c.props["checked"]) + ");\n";
                if (c.props.isMember("enabled") && !c.props["enabled"].asBool())
                    out += indent + a + ".setEnabled(false);\n";
            } else if (c.type == "Separator") {
                out += indent + m + ".addSeparator();\n";
            } else if (c.type == "Menu") {
                genMenuTs(c, depth, m);
            }
        }
    }

    // 生成该节点的创建 + 属性设置, 返回引用该节点的表达式(供父节点 append)。
    std::string genNode(const JZUiNode& node, int depth)
    {
        std::string indent(depth * 2, ' ');
        if (node.type == "MenuBar") {
            // 菜单栏专属: 用 JZMenuBar, 内部用 addMenu/addAction/addSeparator 构造菜单树
            std::string lhs;
            if (!node.id.empty()) {
                lhs = "this." + sanitizeId(node.id);
                idFields.insert(sanitizeId(node.id));
                out += indent + lhs + " = new JZMenuBar();\n";
            } else {
                std::string var = "_w" + std::to_string(counter++);
                lhs = var;
                out += indent + "const " + var + " = new JZMenuBar();\n";
            }
            for (const auto& m : node.children)
                if (m.type == "Menu") genMenuTs(m, depth, lhs);
            return lhs;
        }
        std::string lhs;
        if (!node.id.empty()) {
            lhs = "this." + sanitizeId(node.id);
            idFields.insert(sanitizeId(node.id));
            out += indent + lhs + " = new Widget(" + tsLiteral(node.type) + ");\n";
        } else {
            std::string var = "_w" + std::to_string(counter++);
            lhs = var;
            out += indent + "const " + var + " = new Widget(" + tsLiteral(node.type) + ");\n";
        }

        if (!node.id.empty())
            out += indent + lhs + ".setId(" + tsLiteral(node.id) + ");\n";

        const Json::Value& props = node.props;
        if (props.isObject()) {
            for (auto it = props.begin(); it != props.end(); ++it) {
                const std::string key = it.name();
                const Json::Value& val = *it;
                if (key == "geometry" && val.isArray() && val.size() == 4) {
                    out += indent + lhs + ".setGeometry("
                        + tsLiteral(val[0]) + ", " + tsLiteral(val[1]) + ", "
                        + tsLiteral(val[2]) + ", " + tsLiteral(val[3]) + ");\n";
                } else if (key == "visible" && val.isBool()) {
                    out += indent + lhs + ".setVisible(" + tsLiteral(val) + ");\n";
                } else if (key == "style" && val.isObject()) {
                    out += indent + lhs + ".setStyle(" + tsLiteral(val) + ");\n";
                } else {
                    out += indent + lhs + ".setProperty("
                        + tsLiteral(key) + ", " + tsLiteral(val) + ");\n";
                }
            }
        }

        // 统计 MainWindow 的中央控件个数(只允许一个非菜单栏 widget)
        int centralCount = 0;

        for (const auto& c : node.children) {
            // ---- 布局子节点: 只有"通用布局宿主"才接受 setLayout ----
            if (uiIsLayout(c.type)) {
                if (!uiAcceptsLayout(node.type)) {
                    // MainWindow / StackedWidget / Splitter / TabWidget / ScrollArea / 非容器
                    // 都有固定内部布局, setLayout 不适用
                    out += indent + "// uiLoader: " + node.type
                        + " 不支持通用布局(不能用 <layout> 子节点)\n";
                    continue;
                }
                std::string childExpr = genNode(c, depth);
                out += indent + lhs + ".setLayout(" + childExpr + ");\n";
                continue;
            }

            // ---- widget 子节点: 按父控件类型决定挂载方式 ----
            std::string childExpr = genNode(c, depth);
            UiMount mode = uiMountMode(node.type);
            switch (mode) {
            case UiMount::Central:
                if (c.type == "MenuBar") {
                    out += indent + lhs + ".addMenuBar(" + childExpr + ");\n";
                } else if (centralCount++ > 0) {
                    out += indent + "// uiLoader: MainWindow 只能有一个中央控件(忽略 \"" + c.id + "\")\n";
                } else {
                    out += indent + lhs + ".setCentralWidget(" + childExpr + ");\n";
                }
                break;
            case UiMount::Stack:
                out += indent + lhs + ".addWidget(" + childExpr + ");\n";
                break;
            case UiMount::Splitter:
                out += indent + lhs + ".addWidget(" + childExpr + ");\n";
                break;
            case UiMount::Tab:
                out += indent + lhs + ".addTab(" + childExpr + ", "
                    + tsLiteral(uiTabLabel(c)) + ");\n";
                break;
            case UiMount::Scroll:
                out += indent + lhs + ".setWidget(" + childExpr + ");\n";
                break;
            case UiMount::WindowAdd:
                out += indent + lhs + ".addWidget(" + childExpr + ");\n";
                break;
            case UiMount::None:
                // 非容器(ComboBox / ListWidget / TableWidget)不接受 widget 子控件
                out += indent + "// uiLoader: " + node.type
                    + " 不是容器控件, 不能挂 widget 子控件(用 <item> 添加列表项)\n";
                break;
            case UiMount::Parent:
            default:
                out += indent + lhs + ".append(" + childExpr + ");\n";
                break;
            }
        }

        // 列表型控件(ComboBox / ListWidget): <item> 子项 -> addItem(text)
        if (uiIsItemHost(node.type)) {
            for (const auto& it : node.items)
                out += indent + lhs + ".addItem(" + tsLiteral(it) + ");\n";
        }

        return lhs;
    }
};

} // namespace

bool JZJSUiLoader::generateTsClass(std::string& outTs,
                                   const std::string& className,
                                   const std::string& importPath) const
{
    if (!m_ok) {
        m_error = "未加载 UI 定义(请先 load)";
        return false;
    }

    TsGen g;
    std::string rootExpr = g.genNode(m_root, 2);   // depth 2 = setupUi 方法体内
    g.out += "    this.root = " + rootExpr + ";\n";

    std::string cls = className.empty() ? m_className : className;

    std::string code;
    code += "import { Widget } from '" + importPath + "';\n\n";
    code += "export class " + cls + " {\n";
    code += "  root: Widget;\n";
    for (const auto& f : g.idFields) {
        if (f == "root")
            continue;   // 避免重复声明
        code += "  " + f + ": Widget;\n";
    }
    code += "\n";
    code += "  constructor() {\n";
    code += "    this.setupUi();\n";
    code += "  }\n\n";
    code += "  setupUi(): void {\n";
    code += g.out;
    code += "  }\n";
    code += "}\n";

    outTs = code;
    return true;
}

std::string JZJSUiLoader::toTsClass(const std::string& className,
                                    const std::string& importPath) const
{
    std::string out;
    if (generateTsClass(out, className, importPath))
        return out;
    return std::string();
}

// ---------------------------------------------------------------------------
// 运行时构造: 生成可执行 JS 脚本并经 engine->eval 构造真实控件树
// ---------------------------------------------------------------------------

namespace {

// 生成脚本片段: 创建节点 + 属性 + 挂载子节点, 返回该节点的局部变量名。
struct UiScriptGen {
    std::string out;
    int counter = 0;

    // 生成菜单树(菜单栏 / 子菜单里的 Menu 节点): addMenu + addAction/addSeparator。
    // 定义前置(同 TsGen::genMenuTs): genNode 会调用它。
    void genMenuJs(const JZUiNode& menu, int depth, const std::string& parentExpr)
    {
        std::string indent(depth * 2, ' ');
        std::string m = "__w" + std::to_string(counter++);
        std::string title = menu.props.isMember("title")
            ? tsLiteral(menu.props["title"].asString()) : "\"\"";
        out += indent + "var " + m + " = " + parentExpr + ".addMenu(" + title + ");\n";
        for (const auto& c : menu.children) {
            if (c.type == "Action") {
                std::string a = "__w" + std::to_string(counter++);
                std::string text = c.props.isMember("text")
                    ? tsLiteral(c.props["text"].asString()) : "\"\"";
                out += indent + "var " + a + " = " + m + ".addAction(" + text + ");\n";
                if (c.props.isMember("shortcut"))
                    out += indent + a + ".setShortcut(" + tsLiteral(c.props["shortcut"].asString()) + ");\n";
                if (c.props.isMember("checkable"))
                    out += indent + a + ".setCheckable(" + tsLiteral(c.props["checkable"]) + ");\n";
                if (c.props.isMember("checked"))
                    out += indent + a + ".setChecked(" + tsLiteral(c.props["checked"]) + ");\n";
                if (c.props.isMember("enabled") && !c.props["enabled"].asBool())
                    out += indent + a + ".setEnabled(false);\n";
            } else if (c.type == "Separator") {
                out += indent + m + ".addSeparator();\n";
            } else if (c.type == "Menu") {
                genMenuJs(c, depth, m);
            }
        }
    }

    // ownerVar: 本节点所在**布局**的宿主控件(非布局节点传空)。
    // 为什么需要: 布局的 addWidget **只登记 item、不建立父子关系**, 所以"布局里的控件"要两句 ——
    // lay.addWidget(child) + child.setParentWidget(ownerWidget)。
    std::string genNode(const JZUiNode& node, int depth, const std::string& ownerVar = "")
    {
        std::string indent(depth * 2, ' ');
        if (node.type == "MenuBar") {
            // 菜单栏专属: __mk 出 JZMenuBar, 内部用 addMenu/addAction/addSeparator 构造菜单树
            std::string var = "__w" + std::to_string(counter++);
            out += indent + "var " + var + " = __mk(" + tsLiteral(node.type) + ");\n";
            if (!node.id.empty())
                out += indent + "__ui." + sanitizeId(node.id) + " = " + var + ";\n";
            for (const auto& m : node.children)
                if (m.type == "Menu") genMenuJs(m, depth + 1, var);
            return var;
        }
        std::string var = "__w" + std::to_string(counter++);
        out += indent + "var " + var + " = __mk(" + tsLiteral(node.type) + ");\n";

        const Json::Value& props = node.props;
        bool marginsDone = false;   // 四边 margin 只发一次 setContentsMargins(见下)
        if (props.isObject()) {
            for (auto it = props.begin(); it != props.end(); ++it) {
                std::string key = it.name();
                const Json::Value& val = *it;
                if (key == "geometry" && val.isArray() && val.size() == 4) {
                    out += indent + var + ".setGeometry("
                        + tsLiteral(val[0]) + ", " + tsLiteral(val[1]) + ", "
                        + tsLiteral(val[2]) + ", " + tsLiteral(val[3]) + ");\n";
                } else if (key == "visible" && val.isBool()) {
                    out += indent + var + ".setVisible(" + tsLiteral(val) + ");\n";
                } else if (key == "text" && uiHasText(node.type)) {
                    out += indent + var + ".setText(" + tsLiteral(uiPropText(val)) + ");\n";
                } else if (key == "title" && uiHasTitle(node.type)) {
                    out += indent + var + ".setTitle(" + tsLiteral(uiPropText(val)) + ");\n";
                } else if (key == "styleSheet") {
                    // 贴 Qt 的写法: 规则表与裸声明都收(裸声明包成规则; 与 C++ 侧同口径)
                    out += indent + var + ".setStyleSheet("
                        + tsLiteral(JZUiParser::styleSheetText(uiPropText(val),
                                                               uiClassMap().at(node.type), node.id))
                        + ");\n";
                } else if (key == "style" && val.isObject()) {
                    out += indent + var + ".setStyleSheet(__css(" + tsLiteral(val) + "));\n";
                }
                // 以下这批是后补的常用属性: 一律"属性名 + 控件类型白名单 + 值类型"三重把关。
                // 值类型按 xmlPropToJson 的推断结果卡(纯文本 "0" 会变成整数、"true" 会变成布尔),
                // 所以 placeholderText 既要收字符串、也要收纯数字文本("0" 这种)。
                else if (key == "currentIndex" && uiHasCurrentIndex(node.type) && val.isInt()) {
                    out += indent + var + ".setCurrentIndex(" + tsLiteral(val) + ");\n";
                } else if (key == "tabPosition" && uiHasTabPosition(node.type) && val.isString()) {
                    // Qt 的 <enum>QTabWidget::West</enum> -> 生成 JZTabWidget.West(常量在 JS 侧已挂到构造器上)
                    const int pos = JZUiParser::parseTabPosition(val.asString());
                    const std::string name = JZUiParser::tabPositionName(pos);
                    if (!name.empty())
                        out += indent + var + ".setTabPosition(JZTabWidget." + name + ");\n";
                } else if (key == "checked" && uiHasChecked(node.type) && val.isBool()) {
                    out += indent + var + ".setChecked(" + tsLiteral(val) + ");\n";
                } else if (key == "readOnly" && uiHasReadOnly(node.type) && val.isBool()) {
                    out += indent + var + ".setReadOnly(" + tsLiteral(val) + ");\n";
                } else if (key == "placeholderText" && uiHasPlaceholder(node.type)
                           && (val.isString() || val.isInt())) {
                    out += indent + var + ".setPlaceholderText(" + tsLiteral(uiPropText(val)) + ");\n";
                } else if (key == "minimum" && uiHasRange(node.type) && (val.isInt() || val.isDouble())) {
                    out += indent + var + ".setMinimum(" + tsLiteral(val) + ");\n";
                } else if (key == "maximum" && uiHasRange(node.type) && (val.isInt() || val.isDouble())) {
                    out += indent + var + ".setMaximum(" + tsLiteral(val) + ");\n";
                }
                // 步进框通用: 值 / 后缀(整数框与小数框都有这两个 setter)
                else if (key == "value" && uiHasRange(node.type) && (val.isInt() || val.isDouble())) {
                    out += indent + var + ".setValue(" + tsLiteral(val) + ");\n";
                } else if (key == "suffix" && uiHasRange(node.type)) {
                    out += indent + var + ".setSuffix(" + tsLiteral(uiPropText(val)) + ");\n";
                }
                // 小数步进框专属
                else if (key == "singleStep" && uiIsDoubleSpin(node.type) && (val.isInt() || val.isDouble())) {
                    out += indent + var + ".setSingleStep(" + tsLiteral(val) + ");\n";
                } else if (key == "decimals" && uiIsDoubleSpin(node.type) && val.isInt()) {
                    out += indent + var + ".setDecimals(" + tsLiteral(val) + ");\n";
                } else if (key == "prefix" && uiIsDoubleSpin(node.type)) {
                    out += indent + var + ".setPrefix(" + tsLiteral(uiPropText(val)) + ");\n";
                }
                // 日期框: 方言里的日期就是**毫秒时间戳**(Qt 的 <date><year>..</year></date> 结构在转换
                // 阶段折算成它), 这样两个后端共用一份口径, 各自不用再写日期解析。
                else if (key == "dateMsecs" && uiIsDateEdit(node.type) && (val.isInt() || val.isDouble())) {
                    out += indent + var + ".setDateMsecs(" + tsLiteral(val) + ");\n";
                } else if (key == "displayFormat" && uiIsDateEdit(node.type)) {
                    out += indent + var + ".setDisplayFormat(" + tsLiteral(uiPropText(val)) + ");\n";
                } else if (key == "calendarPopup" && uiIsDateEdit(node.type) && val.isBool()) {
                    out += indent + var + ".setCalendarPopup(" + tsLiteral(val) + ");\n";
                }
                // 布局的间距/边距: Qt 的 <property name="spacing"> / leftMargin... 这四个。
                // 不处理的话网格排出来是"贴在一起"的 —— 引擎布局默认 gap=0, 而 Qt 用的是 style 默认
                // 间距(Windows 上 6 左右), 所以转换后的界面普遍比原版"紧"。
                else if (uiIsLayout(node.type) && key == "spacing" && val.isInt()) {
                    out += indent + var + ".setSpacing(" + tsLiteral(val) + ");\n";
                } else if (uiIsLayout(node.type) && uiLayoutMarginIndex(key) >= 0) {
                    // 四边 margin: Qt 分四个独立属性, 引擎是一次性 setContentsMargins(l,t,r,b) ⇒
                    // 遇到第一个就把整组发掉(缺的按 0), 其余三个不再重复发。
                    if (!marginsDone) {
                        marginsDone = true;
                        int m[4] = {0, 0, 0, 0};
                        const char* mn[4] = {"leftMargin", "topMargin", "rightMargin", "bottomMargin"};
                        for (int k = 0; k < 4; k++)
                            if (props.isMember(mn[k]) && props[mn[k]].isInt())
                                m[k] = props[mn[k]].asInt();
                        out += indent + var + ".setContentsMargins(" + std::to_string(m[0]) + ", "
                            + std::to_string(m[1]) + ", " + std::to_string(m[2]) + ", "
                            + std::to_string(m[3]) + ");\n";
                    }
                }
                // 其余属性仍然不处理(避免调用不存在的 setter 抛异常) —— 引擎侧没绑的能用清单见
                // 各 .ui 转换出来的 report.txt("被忽略的属性"一节)。
            }
        }

        // 统计 MainWindow 的中央控件个数(只允许一个非菜单栏 widget)
        int centralCount = 0;

        for (const auto& c : node.children) {
            // ---- 布局子节点: 只有"通用布局宿主"才接受 setLayout ----
            if (uiIsLayout(c.type)) {
                if (uiIsLayout(node.type)) {
                    // 布局套布局: 引擎的布局没有 addLayout, 显式报错比静默生成坏代码好
                    out += indent + "throw new Error(\"uiLoader: 暂不支持布局嵌套布局(父 "
                        + node.type + " \"" + node.id + "\" > 子 \"" + c.id
                        + "\"), 请在转换阶段把内层布局拍平\");\n";
                    continue;
                }
                if (!uiAcceptsLayout(node.type)) {
                    // MainWindow / StackedWidget / Splitter / TabWidget / ScrollArea / 非容器
                    // 都有固定内部布局, setLayout 不适用 —— 直接报错比生成坏代码好。
                    out += indent + "throw new Error(\"uiLoader: " + node.type
                        + " 不支持通用布局(不能用 <layout> 子节点)\");\n";
                    continue;
                }
                std::string childExpr = genNode(c, depth + 1, var);   // 布局的宿主 = 本控件
                out += indent + var + ".setLayout(" + childExpr + ");\n";
                continue;
            }

            // ---- widget 子节点: 按父控件类型决定挂载方式 ----
            std::string childExpr = genNode(c, depth + 1);

            if (uiIsLayout(node.type)) {
                // 父是布局: 先登记进布局, 再把父子关系立起来 ——
                // **不能**用 setParentWidget(布局): 布局不是控件, 那里会抛"需要 JZWidget"。
                // 布局 addWidget 的真实形参: 箱式 (控件, stretch); 网格 (控件, row, col)。
                if (node.type == "GridLayout") {
                    if (c.row < 0 || c.col < 0) {
                        // 网格必须给位置: 按声明顺序静默竖排会得到一个"错得看不出来"的界面
                        // (历史数据页的 QGridLayout 以前就是这样被降级成 VBoxLayout 的)。
                        out += indent + "throw new Error(\"uiLoader: GridLayout \\\"" + node.id
                            + "\\\" 的子控件 \\\"" + c.id + "\\\" 缺 row/column 属性\");\n";
                        continue;
                    }
                    out += indent + var + ".addWidget(" + childExpr + ", "
                        + std::to_string(c.row) + ", " + std::to_string(c.col) + ");\n";
                } else {
                    // 伸缩因子来自 <widget ... stretch="1">(Qt 的 <stretch>; 与 C++ 侧同口径)
                    out += indent + var + ".addWidget(" + childExpr + ", "
                        + std::to_string(c.stretch) + ");\n";
                }
                if (!ownerVar.empty())
                    out += indent + childExpr + ".setParentWidget(" + ownerVar + ");\n";
                continue;
            }

            UiMount mode = uiMountMode(node.type);
            switch (mode) {
            case UiMount::Central:
                // MainWindow: 菜单栏走 addMenuBar; 其余(至多一个)widget 作中央控件
                if (c.type == "MenuBar") {
                    out += indent + var + ".addMenuBar(" + childExpr + ");\n";
                } else {
                    if (centralCount++ > 0) {
                        // MainWindow 只接受一个中央控件; 多余的(QToolBar/QStatusBar 这类引擎没有对应能力的)
                        // **只留注释报告, 不生成语句** —— 与 TsGen 同一口径。
                        // (以前这里生成 `throw new Error("... "id" ...")`: 引号没转义 -> 生成脚本语法错,
                        //  整个 UI 都建不出来; 见 ui2jz 的转换报告里 QToolBar -> Widget 那一条。)
                        out += indent + "// uiLoader: MainWindow 只接受一个中央控件, 多余子控件 \""
                            + c.id + "\" 已忽略\n";
                    } else {
                        out += indent + var + ".setCentralWidget(" + childExpr + ");\n";
                    }
                }
                break;
            case UiMount::Stack:
                // StackedWidget: 每页一个, 固定内部布局, 走 addWidget
                out += indent + var + ".addWidget(" + childExpr + ");\n";
                break;
            case UiMount::Splitter:
                // Splitter: 每个子控件一个, 走 addWidget
                out += indent + var + ".addWidget(" + childExpr + ");\n";
                break;
            case UiMount::Tab:
                // TabWidget: 每页一个, 标签取子控件 title/id
                out += indent + var + ".addTab(" + childExpr + ", "
                    + tsLiteral(uiTabLabel(c)) + ");\n";
                break;
            case UiMount::Scroll:
                // ScrollArea: 单一内容控件, 走 setWidget
                out += indent + var + ".setWidget(" + childExpr + ");\n";
                break;
            case UiMount::WindowAdd:
                // Window / Dialog: 没有内置布局, 子控件用 setParentWidget 挂到窗口下(绝对定位, 不自动补 VBox)
                out += indent + childExpr + ".setParentWidget(" + var + ");\n";
                break;
            case UiMount::None:
                // 非容器(ComboBox / ListWidget / TableWidget)不接受 widget 子控件,
                // 列表内容用 <item> 子元素(addItem), 其它数据走属性。
                out += indent + "throw new Error(\"uiLoader: " + node.type
                    + " 不是容器控件, 不能挂 widget 子控件(用 <item> 添加列表项)\");\n";
                break;
            case UiMount::Parent:
            default:
                // 通用容器(JZWidget / GroupBox / ...): setParentWidget
                out += indent + childExpr + ".setParentWidget(" + var + ");\n";
                break;
            }
        }

        // 列表型控件(ComboBox / ListWidget): <item> 子项 -> addItem(text)
        if (uiIsItemHost(node.type)) {
            for (const auto& it : node.items)
                out += indent + var + ".addItem(" + tsLiteral(it) + ");\n";
        }

        // currentIndex 必须在**子节点挂载之后**再设一次:
        // 上面按属性顺序先调了 setCurrentIndex(1), 那时容器里还没有页/项, 索引会被夹成 -1,
        // 之后挂载也不会自动补回来(实测 tabWidget.currentIndex() 回读 -1)。
        if (props.isObject() && uiHasCurrentIndex(node.type)
            && props.isMember("currentIndex") && props["currentIndex"].isInt()) {
            out += indent + var + ".setCurrentIndex("
                + tsLiteral(props["currentIndex"]) + ");\n";
        }

        if (!node.id.empty())
            out += indent + "__ui." + sanitizeId(node.id) + " = " + var + ";\n";
        return var;
    }
};

}  // namespace

JSValue JZJSUiLoader::createUi(JZJSEngine* engine) const
{
    if (!m_ok) {
        m_error = "未加载 UI 定义(请先 load)";
        return JS_UNDEFINED;
    }
    if (!engine) {
        m_error = "engine 为空";
        return JS_UNDEFINED;
    }

    // 校验所有控件类型均已注册(未知类型直接失败, 避免 eval 时抛异常);
    // 同时收集实际用到的类型, 仅把这些写进 __types, 避免引用到引擎中未注册的控件类
    // (如树里没有 Canvas, 就不该在脚本里出现 "Canvas": JZCanvas, 否则未定义会抛异常)。
    const auto& clsMap = uiClassMap();
    bool unknown = false;
    std::string badType;
    std::set<std::string> usedTypes;
    traverse([&](const JZUiNode& n, const JZUiNode*) {
        if (uiIsMenuStructure(n.type))
            return;
        if (unknown) return;
        auto it = clsMap.find(n.type);
        if (it == clsMap.end()) {
            unknown = true;
            badType = n.type;
        } else {
            usedTypes.insert(n.type);
        }
    });
    if (unknown) {
        m_error = "未知控件类型: " + badType + "(引擎未注册该类)";
        return JS_UNDEFINED;
    }

    // __types 字面量: 仅含实际用到的类型 { "Window": JZWindow, ... }
    std::string typesObj = "{";
    bool first = true;
    for (const auto& t : usedTypes) {
        if (!first) typesObj += ", ";
        first = false;
        typesObj += tsLiteral(t) + ": " + clsMap.at(t);
    }
    typesObj += "}";

    UiScriptGen g;
    std::string rootExpr = g.genNode(m_root, 2);
    std::string body = g.out + "  __ui.root = " + rootExpr + ";\n";

    // 脚本: IIFE 构造控件树并返回 { root, <id>: widget }
    std::string script = R"(
(function(){
  function __mk(t){ var C = __types[t]; if(!C) throw new Error("unknown widget type: " + t); return new C(); }
  function __css(s){ var o=""; for(var k in s){ if(!Object.prototype.hasOwnProperty.call(s,k)) continue; var v=s[k]; var css=k.replace(/[A-Z]/g, function(m){ return "-" + m.toLowerCase(); }); o += css + ":" + v + ";"; } return o; }
  var __ui = {};
)";
    script += "  var __types = " + typesObj + ";\n";
    script += body;
    script += "  return __ui;\n})()\n";

    // 排查用: 设了环境变量就把生成的脚本原样打出来(报错里的 "eval:12:25" 指的就是它的行:列)。
    if (::getenv("JZ_DUMP_UI_SCRIPT"))
    {
        std::printf("===== createUi 生成的脚本 =====\n");
        int line = 0;
        std::string cur;
        for (size_t i = 0; i <= script.size(); ++i) {
            if (i == script.size() || script[i] == '\n') {
                std::printf("%4d: %s\n", ++line, cur.c_str());
                cur.clear();
            } else {
                cur += script[i];
            }
        }
        std::printf("===== 脚本结束 =====\n");
    }

    JZJSValuePtr res = engine->eval(script);
    if (!engine->runtimeError().empty()) {
        m_error = engine->runtimeError();
        return JS_UNDEFINED;
    }
    return JS_DupValue(engine->context(), res.get());
}

