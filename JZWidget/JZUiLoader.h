#ifndef JZ_UI_LOADER_H_
#define JZ_UI_LOADER_H_

#include "JZGlobal.h"
#include "JZUiParser.h"   // JZUiNode + 方言表(JZUiParser::classForType / mountMode / hasXxx ...)

#include <map>
#include <string>

class JZWidget;
class JZLayout;

// ---------------------------------------------------------------------------
// JZUiLoader —— **C++ 侧**的 UI 加载器(对应 JS 侧的 JZUiParser)。
//
// 分工: 解析部分**公用**(继承 JZUiParser: 同一棵 JZUiNode 树 + 同一份方言判别/属性白名单),
//       "生成控件"这段各自独立:
//         * JS 侧(JZUiParser::createUi): 把树生成脚本 -> engine->eval -> JS 侧的控件对象;
//         * C++ 侧(本类 build):            直接 new 真 JZWidget, 属性走真 setter, 挂载走同一套
//                                          UiMount 规则(见 JZUiParser::mountMode)。
//       ⇒ 同一份 .ui(xml), 两条后端都能建树; 方言要改只改 JZUiParser 里那几张表。
//
// 用法:
//   JZUiLoader loader;
//   if (!loader.loadFromFile(path)) { err = loader.error(); return; }
//   JZUiLoader::Result r;
//   if (!loader.build(r)) { err = loader.error(); return; }
//   r.root->show();
//   JZWidget* list = r.byId["listWidget"];      // 与 JS 的 { root, <id>: widget } 同形
//
// 所有权: 建出来的控件树以 root 为根(子控件都挂在 root 下), 删 root 即整棵树析构。
// 注意: 只有启用 JZWidget 的构建才编本文件(见 JZJSEngine.cmake 的 if(HAS_JZWidget))。
// ---------------------------------------------------------------------------
class JZFRAMEWORK_API JZUiLoader : public JZUiParser
{
public:
    struct Result
    {
        JZWidget* root = nullptr;                 // 顶层控件(通常 JZWindow / JZMainWindow)
        std::map<std::string, JZWidget*> byId;    // 带 name 的控件(含 root); 没 name 的不进表
    };

    // 在已解析(load/loadFromFile 成功)的树上建真控件。parent 非空时把 root 挂到它下面,
    // 通常传 nullptr 让 root 自己当窗口。失败返回 false, 原因见 error()。
    bool build(JZWidget* parent, Result& out);
    bool build(Result& out) { return build(nullptr, out); }

private:
    // 建一个控件(含属性/子节点/菜单栏), 失败返回 nullptr 并置 error
    JZWidget* buildWidget(const JZUiNode& node, JZWidget* owner, Result& out);
    // 建节点下的子控件/子布局(挂载规则见 UiMount)
    bool buildChildren(const JZUiNode& node, JZWidget* self, Result& out);
    // 建布局(并把它下面的子控件按 row/col 或顺序加进去)
    JZLayout* buildLayout(const JZUiNode& layoutNode, JZWidget* host, Result& out);
    // 菜单栏(<menubar> -> <menu> -> <action>/<separator>)
    bool buildMenuBar(const JZUiNode& menuBarNode, JZWidget* mainWindow);

    void applyProps(const JZUiNode& node, JZWidget* w);
    void applyLayoutProps(const JZUiNode& node, JZLayout* lay);

    JZWidget* createWidget(const std::string& type);   // 未知类型返回 nullptr
    JZLayout* createLayout(const std::string& type);
};

#endif // !JZ_UI_LOADER_H_
