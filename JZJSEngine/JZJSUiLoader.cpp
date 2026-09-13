#include "JZJSUiLoader.h"
#include "JZJSEngine.h"
#include "JZXml/JZXml.h"

#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>

JZJSUiLoader::JZJSUiLoader() {}

// 已知控件类型表：覆盖 JZWidget 现有可创建控件。loader 默认不强制校验,
// 仅作为参考清单; 扩展新控件时在此追加即可。
static const std::vector<std::string>& builtinKnownTypes()
{
    static const std::vector<std::string> types = {
        "Window", "MainWindow", "Dialog", "Canvas",
        "PushButton", "Label", "ImageLabel",
        "LineEdit", "TextEdit", "AbstractTextEdit",
        "CheckBox", "RadioButton", "Switch", "ComboBox",
        "ListWidget", "TreeWidget", "TableWidget",
        "Slider", "ScrollBar", "ProgressBar", "SpinBox",
        "GroupBox", "TabWidget", "StackedWidget", "ScrollArea",
        "MenuBar", "Menu",
        "Layout", "HBoxLayout", "VBoxLayout", "GridLayout",
    };
    return types;
}

const std::vector<std::string>& JZJSUiLoader::knownTypes()
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

bool JZJSUiLoader::load(const std::string& xmlText)
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

bool JZJSUiLoader::parseWidget(const JZDomElement& el, JZUiNode& out, const std::string& path)
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
        }
        // 其它子元素(layout/等等)目前忽略
    }
    return true;
}

bool JZJSUiLoader::loadFromFile(const std::string& path)
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

void JZJSUiLoader::traverse(const VisitFn& fn) const
{
    if (m_ok)
        traverseNode(m_root, nullptr, fn);
}

void JZJSUiLoader::traverseNode(const JZUiNode& node, const JZUiNode* parent, const VisitFn& fn) const
{
    fn(node, parent);
    for (const auto& c : node.children)
        traverseNode(c, &node, fn);
}

std::vector<const JZUiNode*> JZJSUiLoader::nodesById(const std::string& id) const
{
    std::vector<const JZUiNode*> res;
    traverse([&](const JZUiNode& n, const JZUiNode*) {
        if (n.id == id)
            res.push_back(&n);
    });
    return res;
}

void JZJSUiLoader::toJson(Json::Value& out) const
{
    out = Json::Value(Json::objectValue);
    writeNode(m_root, out);
}

void JZJSUiLoader::writeNode(const JZUiNode& node, Json::Value& out) const
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
}

// ---------------------------------------------------------------------------
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

// 递归生成 setupUi() 方法体(平坦语句), 同时收集带 id 的字段名。
struct TsGen {
    std::string out;
    int counter = 0;
    std::set<std::string> idFields;

    // 生成该节点的创建 + 属性设置, 返回引用该节点的表达式(供父节点 append)。
    std::string genNode(const JZUiNode& node, int depth)
    {
        std::string indent(depth * 2, ' ');
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

        for (const auto& c : node.children) {
            std::string childExpr = genNode(c, depth);
            out += indent + lhs + ".append(" + childExpr + ");\n";
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

// UI 短类型名 -> 引擎已注册的 JS 构造器名(JZJSWidgetModule 注册的 C++ 类)。
// 仅覆盖已注册控件; 含未注册类型时 createUi 直接失败(见下方校验)。
const std::map<std::string, std::string>& uiClassMap()
{
    static const std::map<std::string, std::string> m = {
        { "Window",       "JZWindow" },
        { "MainWindow",   "JZWindow" },
        { "Dialog",       "JZWindow" },
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
        { "GroupBox",     "JZGroupBox" },
        { "Layout",       "JZLayout" },
        { "VBoxLayout",   "JZVBoxLayout" },
        { "HBoxLayout",   "JZHBoxLayout" },
        { "GridLayout",   "JZGridLayout" },
    };
    return m;
}

bool uiIsWindow(const std::string& t) {
    return t == "Window" || t == "MainWindow" || t == "Dialog";
}
bool uiIsLayout(const std::string& t) {
    return t == "Layout" || t == "VBoxLayout" || t == "HBoxLayout" || t == "GridLayout";
}
bool uiHasText(const std::string& t) {
    return t == "Label" || t == "ImageLabel" || t == "PushButton" ||
           t == "LineEdit" || t == "CheckBox" || t == "RadioButton";
}
bool uiHasTitle(const std::string& t) {
    return uiIsWindow(t) || t == "GroupBox";
}

// 生成脚本片段: 创建节点 + 属性 + 挂载子节点, 返回该节点的局部变量名。
struct UiScriptGen {
    std::string out;
    int counter = 0;

    std::string genNode(const JZUiNode& node, int depth)
    {
        std::string indent(depth * 2, ' ');
        std::string var = "__w" + std::to_string(counter++);
        out += indent + "var " + var + " = __mk(" + tsLiteral(node.type) + ");\n";
        // 保持引用, 避免无 id 的子控件被 JS GC 回收后底层 C++ 对象悬空
        out += indent + "__keep.push(" + var + ");\n";

        const Json::Value& props = node.props;
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
                    out += indent + var + ".setText(" + tsLiteral(val) + ");\n";
                } else if (key == "title" && uiHasTitle(node.type)) {
                    out += indent + var + ".setTitle(" + tsLiteral(val) + ");\n";
                } else if (key == "style" && val.isObject()) {
                    out += indent + var + ".setStyleSheet(__css(" + tsLiteral(val) + "));\n";
                }
                // 其它属性暂不处理(避免调用不存在的 setter 抛异常)
            }
        }

        for (const auto& c : node.children) {
            std::string childExpr = genNode(c, depth + 1);
            if (uiIsWindow(node.type) && !uiIsLayout(c.type))
                out += indent + var + ".addWidget(" + childExpr + ");\n";
            else if (uiIsLayout(c.type))
                out += indent + var + ".setLayout(" + childExpr + ");\n";
            else
                out += indent + childExpr + ".setParentWidget(" + var + ");\n";
        }

        if (!node.id.empty())
            out += indent + "__ui." + sanitizeId(node.id) + " = " + var + ";\n";
        return var;
    }
};

} // namespace

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

    // 脚本: IIFE 构造控件树并返回 { root, <id>: widget, __keep: [...] }
    std::string script = R"(
(function(){
  function __mk(t){ var C = __types[t]; if(!C) throw new Error("unknown widget type: " + t); return new C(); }
  function __css(s){ var o=""; for(var k in s){ if(!Object.prototype.hasOwnProperty.call(s,k)) continue; var v=s[k]; var css=k.replace(/[A-Z]/g, function(m){ return "-" + m.toLowerCase(); }); o += css + ":" + v + ";"; } return o; }
  var __ui = {};
  var __keep = [];
)";
    script += "  var __types = " + typesObj + ";\n";
    script += body;
    script += "  __ui.__keep = __keep;\n  return __ui;\n})()\n";

    JZJSValuePtr res = engine->eval(script);
    if (!engine->runtimeError().empty()) {
        m_error = engine->runtimeError();
        return JS_UNDEFINED;
    }
    return JS_DupValue(engine->context(), res.get());
}
