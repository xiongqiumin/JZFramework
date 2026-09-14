#include <string>
#include <vector>

#include "JZDebug.h"
#include "JZApplication.h"
#include "JZJSEngine.h"
#include "JZJSWidgetModule.h"
#include "JZJSUiLoader.h"
#include "JZWidget.h"
#include "JZMenuBar.h"
#include "JZMenu.h"
#include "quickjs/quickjs.h"

using namespace std;

static int g_fail = 0;
static void check(const string& name, bool ok)
{
    jzDebug() << (ok ? "[PASS]" : "[FAIL]") << name;
    if (!ok)
        g_fail++;
}

// 求值并取字符串; 脚本抛错时返回 "<js-exception>" 而不是崩。
static string evalStr(JZJSEngine& engine, const string& code)
{
    auto v = engine.eval(code);
    if (JS_IsException(v.get()) || JS_IsUndefined(v.get()))
        return "<js-exception>";
    return engine.fromJSValue<string>(v.get());
}

int main(int argc, char** argv)
{
    setbuf(stdout, NULL);   // 测试输出不缓冲: 崩溃/断言时也能看到已跑过的用例

    if (argc < 2) {
        jzDebug() << "[FAIL] 用法: test_load_ui <ui_xml 路径>";
        return 1;
    }
    string path = argv[1];

    JZApplication a;

    // 1) 加载 + 解析 UI 描述
    JZJSUiLoader loader;
    if (!loader.loadFromFile(path)) {
        jzDebug() << "[FAIL] 加载失败: " << loader.error();
        return 1;
    }

    jzDebug() << "===== 加载: " << path << " =====";
    Json::Value out;
    loader.toJson(out);
    jzDebug() << out.toStyledString();

    // 是否含菜单栏(用于决定后续菜单相关断言)
    bool hasMenubar = false;
    loader.traverse([&](const JZUiNode& n, const JZUiNode*) {
        if (n.type == "MenuBar")
            hasMenubar = true;
    });

    // 2) 运行时构造: 经 JZJSWidgetModule 注册真实控件类, createUi 在引擎中构造真实控件树
    JZJSEngine engine;
    engine.init();
    JZJSWidgetModule widgetModule;
    widgetModule.initEngine(&engine);

    JSValue ui = loader.createUi(&engine);
    check("createUi 返回有效 JSValue", !JS_IsUndefined(ui));
    if (JS_IsUndefined(ui)) {
        jzDebug() << "[FAIL] createUi 失败: " << loader.error();
        return 1;
    }
    engine.setProperty(engine.globalObject(), "ui", ui);

    // 3) 根控件类型正确
    string rootKind = evalStr(engine,
        "ui.root instanceof JZMainWindow ? 'mainwin'"
        " : (ui.root instanceof JZWindow ? 'window' : 'other')");
    check("根控件是 JZWindow/JZMainWindow", rootKind == "mainwin" || rootKind == "window");

    // 4) 菜单栏结构校验(仅当 UI 含 <menubar>)
    if (hasMenubar) {
        check("菜单栏已挂载 (root.menuBar() instanceof JZMenuBar)",
              evalStr(engine, "(ui.root.menuBar() instanceof JZMenuBar) ? 'ok' : 'bad'") == "ok");
        int menuCount = engine.fromJSValue<int>(engine.eval("ui.root.menuBar().menuCount()").get());
        check("菜单栏至少含一个菜单", menuCount > 0);
        int actionCount = engine.fromJSValue<int>(
            engine.eval("ui.root.menuBar().menuAt(0).actionCount()").get());
        check("第一个菜单至少含一个动作", actionCount > 0);
        // 子菜单(最近)应作为第二个菜单的嵌套出现: 文件菜单含一个子菜单
        int subMenus = engine.fromJSValue<int>(
            engine.eval("ui.root.menuBar().menuAt(0).menuCount()").get());
        check("文件菜单含一个子菜单(最近)", subMenus == 1);
    }

    // 5) 无运行时异常
    check("createUi 无运行时异常", engine.runtimeError().empty());

    jzDebug() << (g_fail == 0 ? "[ALL PASS]" : "[HAS FAIL]");
    return g_fail == 0 ? 0 : 1;
}
