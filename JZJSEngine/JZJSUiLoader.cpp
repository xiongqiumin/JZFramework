#include "JZJSUiLoader.h"

#include <fstream>
#include <memory>
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

bool JZJSUiLoader::load(const std::string& jsonText)
{
    Json::Value root;
    Json::CharReaderBuilder builder;
    JSONCPP_STRING errs;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    bool ok = reader->parse(jsonText.data(), jsonText.data() + jsonText.size(), &root, &errs);
    if (!ok) {
        m_ok = false;
        m_error = "JSON 解析失败: " + errs;
        return false;
    }
    return load(root);
}

bool JZJSUiLoader::load(const Json::Value& root)
{
    m_error.clear();
    m_ok = false;
    m_root = JZUiNode();

    if (!root.isObject()) {
        m_error = "根节点必须是 JSON 对象";
        return false;
    }
    if (!parseNode(root, m_root, "root")) {
        return false;
    }
    m_ok = true;
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

bool JZJSUiLoader::parseNode(const Json::Value& v, JZUiNode& out, const std::string& path)
{
    if (!v.isObject()) {
        m_error = path + ": 节点必须是对象";
        return false;
    }

    // type 必填
    const Json::Value& type = v["type"];
    if (!type.isString() || type.asString().empty()) {
        m_error = path + ": 缺少字符串 type 字段";
        return false;
    }
    out.type = type.asString();

    // id 可选
    const Json::Value& id = v["id"];
    if (!id.isNull()) {
        if (!id.isString()) {
            m_error = path + ": id 必须是字符串";
            return false;
        }
        out.id = id.asString();
    }

    // props 可选
    const Json::Value& props = v["props"];
    if (!props.isNull()) {
        if (!props.isObject()) {
            m_error = path + ": props 必须是对象";
            return false;
        }
        out.props = props;
    } else {
        out.props = Json::Value(Json::objectValue);
    }

    // children 可选
    const Json::Value& children = v["children"];
    if (!children.isNull()) {
        if (!children.isArray()) {
            m_error = path + ": children 必须是数组";
            return false;
        }
        out.children.reserve(children.size());
        for (Json::ArrayIndex i = 0; i < children.size(); ++i) {
            JZUiNode child;
            std::string cpath = path + ".children[" + std::to_string(i) + "]";
            if (!parseNode(children[i], child, cpath))
                return false;
            out.children.push_back(std::move(child));
        }
    }
    return true;
}

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
