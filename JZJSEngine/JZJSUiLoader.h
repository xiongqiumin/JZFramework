#ifndef JZ_JS_UI_LOADER_H_
#define JZ_JS_UI_LOADER_H_

#include <functional>
#include <string>
#include <vector>

#include "json/json.h"
#include "quickjs/quickjs.h"

class JZJSEngine;

// UI 定义节点：纯数据结构，描述一个控件(不含任何真实 JZWidget 实例)。
// - type    控件类名(对应后续 JZWidget 子类 / @jz/widget 的 type), 如 "Window"/"PushButton"/"Label"
// - id      可选, 生成 TS 时的成员名, 也对应运行时 widget 的 objectName
// - props   属性键值(原样保留 Json::Value, 保留类型: 字符串/数字/布尔/数组)
// - children 递归子节点(控件, 即 <widget>)
// - items   列表型子项(ComboBox / ListWidget 等不支持通用布局的控件, 用 addItem 添加)
struct JZUiNode
{
    std::string type;
    std::string id;
    Json::Value props;                    // objectValue(可能为空)
    std::vector<JZUiNode> children;
    std::vector<std::string> items;       // 文本项(对应 addItem(text))

    bool isValid() const { return !type.empty(); }
};

// JZJSUiLoader：输入 Qt 风格 ui 文件(XML), 解析为 UI 定义树, 并生成可消费的
// TypeScript 类(对齐 @jz/widget 构建器)。
// 仅做结构解析与基本校验，不创建任何真实控件 —— 真实 widget 由后续步骤( JZWidget 侧 )
// 消费本定义构建。这样 JZJSEngine 模块无需链接 GUI 模块，保持纯数据层。
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
//       <widget class="PushButton" name="btn">
//         <property name="text">Click me</property>
//       </widget>
//     </widget>
//   </ui>
//
// 生成的 TS 类(Qt uic 风格):
//   - 每个带 id 的节点对应一个公开字段(类型 Widget, 名称取 id); 另固定 root 字段;
//   - 生成 setupUi() 方法(平坦地创建并设置属性、挂载父子关系), 由构造函数调用。
class JZJSUiLoader
{
public:
    JZJSUiLoader();

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

    // 生成与 @jz/widget 控件构建器对齐的 TypeScript 类源码(Qt uic 风格)。
    // className 覆盖 XML 中的 <class>; importPath 为 'widget' 模块导入路径(默认 "widget")。
    // 未加载(load 失败)时返回 false, 原因见 error()。
    // 注意: 重复 id 会产生同名字段(非法 TS), 调用方需保证 id 唯一。
    bool generateTsClass(std::string& outTs,
                         const std::string& className = "",
                         const std::string& importPath = "widget") const;

    // generateTsClass 的便捷版: 成功返回生成源码, 失败返回空串(原因见 error())。
    std::string toTsClass(const std::string& className = "",
                          const std::string& importPath = "widget") const;

    // 基于已加载的 UI 定义, 在给定引擎中构造真实控件树并直接返回构造好的 JSValue。
    // 等价于 generateTsClass 的运行时版: 生成可执行的 JS(直接 new 各 JZWidget 子类、设置属性、
    // 挂载父子关系), 经 engine->eval 执行, 返回形如 { root, <id>: widget } 的 JS 对象。
    // 失败(未 load / 含未知控件类型 / eval 抛异常)返回 JS_UNDEFINED, 原因见 error()。
    // 注意: 调用前引擎须已由 JZJSWidgetModule::initEngine 注册全部控件类(new JZWindow 之类可用)。
    JSValue createUi(JZJSEngine* engine) const;

private:
    bool parseWidget(const class JZDomElement& el, JZUiNode& out, const std::string& path);
    void writeNode(const JZUiNode& node, Json::Value& out) const;
    void traverseNode(const JZUiNode& node, const JZUiNode* parent, const VisitFn& fn) const;

    bool m_ok = false;
    mutable std::string m_error;
    std::string m_className = "Ui";
    JZUiNode m_root;
};

#endif // !JZ_JS_UI_LOADER_H_
