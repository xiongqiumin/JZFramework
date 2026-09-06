#ifndef JZ_JS_UI_LOADER_H_
#define JZ_JS_UI_LOADER_H_

#include <functional>
#include <string>
#include <vector>

#include "json/json.h"

// UI 定义节点：纯数据结构，描述一个控件(不含任何真实 JZWidget 实例)。
// - type    控件类名, 对应后续 JZWidget 子类, 如 "Window"/"PushButton"/"Label"
// - id      可选, 对应后续 widget 的 objectName, 供 JS 侧 ui.w.<id> 查找
// - props   属性键值(text/geometry/visible/title ...) 原样保留 Json::Value(保留类型)
// - children 递归子节点
struct JZUiNode
{
    std::string type;
    std::string id;
    Json::Value props;                    // objectValue(可能为空)
    std::vector<JZUiNode> children;

    bool isValid() const { return !type.empty(); }
};

// JZJSUiLoader：输入 JSON(文本 / Json::Value / 文件)，生成 UI 定义树。
// 仅做结构解析与基本校验，不创建任何真实控件 —— 真实 widget 由后续步骤( JZWidget 侧 )
// 消费本定义构建。这样 JZJSEngine 模块无需链接 GUI 模块，保持纯数据层。
class JZJSUiLoader
{
public:
    JZJSUiLoader();

    // 从 JSON 文本解析。失败返回 false, 原因见 error()。
    bool load(const std::string& jsonText);
    // 从已解析的 Json::Value(通常为 object)构建。
    bool load(const Json::Value& root);
    // 从文件读取(JSON 文本)并解析。
    bool loadFromFile(const std::string& path);

    const JZUiNode* root() const { return m_ok ? &m_root : nullptr; }
    std::string error() const { return m_error; }

    // 已知控件类型表(用于校验/提示, 可按需扩展)。空表表示不做类型校验。
    static const std::vector<std::string>& knownTypes();

    // 深度优先遍历(便于后续建 widget / 导出 / 校验 id 唯一性)。
    using VisitFn = std::function<void(const JZUiNode& node, const JZUiNode* parent)>;
    void traverse(const VisitFn& fn) const;

    // 收集所有 id 匹配的节点(供 JS 侧建立 id -> node 索引; 允许重复 id)。
    std::vector<const JZUiNode*> nodesById(const std::string& id) const;

    // 把当前定义回写为 Json::Value(与输入结构一致, 便于测试/往返校验)。
    void toJson(Json::Value& out) const;

private:
    bool parseNode(const Json::Value& v, JZUiNode& out, const std::string& path);
    void writeNode(const JZUiNode& node, Json::Value& out) const;
    void traverseNode(const JZUiNode& node, const JZUiNode* parent, const VisitFn& fn) const;

    bool m_ok = false;
    std::string m_error;
    JZUiNode m_root;
};

#endif // !JZ_JS_UI_LOADER_H_
