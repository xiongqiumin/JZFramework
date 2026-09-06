#include <string>

#include "JZDebug.h"
#include "JZApplication.h"
#include "JZJSUiLoader.h"

using namespace std;

// 递归打印定义树, 便于人眼查看每个节点的 type / id / props / children
static void dumpNode(const JZUiNode& node, int depth)
{
    string indent(depth * 2, ' ');
    jzDebug() << indent << "[type=" << node.type
              << " id=" << (node.id.empty() ? "(none)" : node.id) << "]";
    if (!node.props.isNull())
        jzDebug() << indent << "  props=" << node.props.toStyledString();
    for (const auto& c : node.children)
        dumpNode(c, depth + 1);
}

int main()
{
    JZApplication a;

    // 样例 UI 描述 JSON:
    // - 根节点 Window 带 window_template_id(目前放在 props 里, C++ 定义原样保留);
    // - 两个子节点 Label / PushButton, 各带 id 供 JS 侧 ui.w.<id> 查找。
    const std::string json = R"({
        "type": "Window",
        "id": "main",
        "props": {
            "title": "Demo",
            "geometry": [100, 100, 400, 300],
            "window_template_id": "win_main_v1"
        },
        "children": [
            {
                "type": "Label",
                "id": "tip",
                "props": { "text": "Count: 0", "geometry": [20, 20, 300, 30] }
            },
            {
                "type": "PushButton",
                "id": "btn",
                "props": { "text": "Click me", "geometry": [20, 70, 120, 40] }
            }
        ]
    })";

    JZJSUiLoader loader;
    if (!loader.load(json)) {
        jzDebug() << "[FAIL] 加载失败: " << loader.error();
        return 1;
    }

    // 1) 回写为 JSON(与输入结构一致), 看 toJson 产物
    jzDebug() << "===== UI 定义 (loader.toJson, 美化 JSON) =====";
    Json::Value out;
    loader.toJson(out);
    jzDebug() << out.toStyledString();

    // 2) 结构化 dump, 看节点层级
    jzDebug() << "===== UI 定义 (结构化 dump) =====";
    dumpNode(*loader.root(), 0);

    // 3) 演示按 id 查找(JS 侧会据此建 ui.w 索引)
    jzDebug() << "===== nodesById 演示 =====";
    jzDebug() << "nodesById('btn') 命中数: " << loader.nodesById("btn").size();
    jzDebug() << "nodesById('tip') 命中数: " << loader.nodesById("tip").size();

    return 0;
}
