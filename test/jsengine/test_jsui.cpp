#include <string>

#include "JZDebug.h"
#include "JZApplication.h"
#include "JZJSEngine.h"
#include "JZJSBind.h"
#include "JZJSUiLoader.h"
#include "JZJSWidgetModule.h"
#include "JZWidget.h"
#include "JZMouseEvent.h"    // 右键菜单用例: 合成 Release + RightButton 事件
#include "JZImage.h"
#include "JZJSType.h"
#include "JZTableWidget.h"
#include "JZMenu.h"
#include "JZAction.h"
#include "quickjs/quickjs.h"

using namespace std;

static int g_fail = 0;
static void check(const string &name, bool ok)
{
    jzDebug() << (ok ? "[PASS]" : "[FAIL]") << name;
    if (!ok)
        g_fail++;
}

// 求值并取字符串, 供"把实际值拼成字符串再比较"的断言使用。
// 注意: 脚本抛错时 eval 返回的是 JS 异常值, 直接 fromJSValue<std::string> 会崩 —— 这里先挡掉。
static string evalStr(JZJSEngine& engine, const string& code)
{
    auto v = engine.eval(code);
    if (JS_IsException(v.get()) || JS_IsUndefined(v.get()))
    {
        jzDebug().noquote() << engine.runtimeError();
        return "<js-exception>";
    }
    return engine.fromJSValue<string>(v.get());
}

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
    setbuf(stdout, NULL);   // 测试输出不缓冲: 崩溃/断言时也能看到已跑过的用例

    JZApplication a;

    // 样例 Qt 风格 ui 文件(XML):
    // - <class> 指定生成 TS 类名 DemoUi;
    // - 根 widget Window 带 window_template_id(原样保留为属性);
    // - 两个子 widget Label / PushButton, 各带 name 供 JS 侧按成员名访问。
    const std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<ui version="4.0">
    <class>DemoUi</class>
    <widget class="Window" name="main">
        <property name="title">Demo</property>
        <property name="geometry">100,100,400,300</property>
        <property name="window_template_id">win_main_v1</property>
        <widget class="Label" name="tip">
            <property name="text">Count: 0</property>
            <property name="geometry">20,20,300,30</property>
        </widget>
        <widget class="PushButton" name="btn">
            <property name="text">Click me</property>
            <property name="geometry">20,70,120,40</property>
        </widget>
    </widget>
</ui>)";

    JZJSUiLoader loader;
    if (!loader.load(xml)) {
        jzDebug() << "[FAIL] 加载失败: " << loader.error();
        return 1;
    }

    // 1) 回写为 JSON(内部树结构), 看 toJson 产物
    jzDebug() << "===== UI 定义 (loader.toJson) =====";
    Json::Value out;
    loader.toJson(out);
    jzDebug() << out.toStyledString();

    // 2) 结构化 dump, 看节点层级
    jzDebug() << "===== UI 定义 (结构化 dump) =====";
    dumpNode(*loader.root(), 0);

    // 3) 演示按 id 查找
    jzDebug() << "===== nodesById 演示 =====";
    jzDebug() << "nodesById('btn') 命中数: " << loader.nodesById("btn").size();
    jzDebug() << "nodesById('tip') 命中数: " << loader.nodesById("tip").size();
    jzDebug() << "className(): " << loader.className();

    // 4) 生成与 @jz/widget 构建器对齐的 TypeScript 类源码(Qt uic 风格)
    jzDebug() << "===== generateTsClass 演示 =====";
    std::string ts;
    // 不传 className, 使用 XML 中的 <class> DemoUi
    if (!loader.generateTsClass(ts)) {
        jzDebug() << "[FAIL] 生成 TS 失败: " << loader.error();
        return 1;
    }
    jzDebug() << ts;

    // 5) 运行时构造: 经 JZJSWidgetModule 注册真实控件类, createUi 在引擎中构造真实控件树并验证
    jzDebug() << "===== createUi 运行时构造演示 =====";
    {
        JZJSEngine engine;
        engine.init();
        JZJSWidgetModule widgetModule;
        engine.setLibraryDir({ std::string(JZ_JS_WRAPPER_DIR) });   // 控件类层(widget/dist/index.js)靠它定位
        widgetModule.initEngine(&engine);

        JSValue ui = loader.createUi(&engine);
        check("createUi 返回有效 JSValue", !JS_IsUndefined(ui));
        if (JS_IsUndefined(ui)) {
            jzDebug() << "[FAIL] createUi 失败: " << loader.error();
            return 1;
        }
        // 把构造结果挂到全局, 所有权转移(之后不再 free ui)
        engine.setProperty(engine.globalObject(), "ui", ui);

        // 校验: root 是 JZWindow, 子节点类型正确, 且属性已写入底层控件
        auto v = engine.eval(
            "ui.root instanceof JZWindow"
            " && ui.tip instanceof JZLabel"
            " && ui.btn instanceof JZPushButton"
            " && ui.btn.text() === 'Click me'"
            " && ui.root.title() === 'Demo'");
        check("createUi 控件树类型与属性正确", engine.fromJSValue<bool>(v.get()));

        // 校验 geometry 已生效(根窗口 400x300): 真实 JZRect 用 width()/height() 方法
        auto g = engine.eval("var r = ui.root.geometry(); r.width() === 400 && r.height() === 300;");
        check("createUi geometry 生效", engine.fromJSValue<bool>(g.get()));
    }

    // 6) 全局 uiLoader 命名空间: 只暴露 loadFile / load 两个工厂方法, 各自直接返回根 JZWindow 窗口
    jzDebug() << "===== uiLoader 全局命名空间演示 =====";
    {
        JZJSEngine engine;
        engine.init();
        JZJSWidgetModule widgetModule;
        engine.setLibraryDir({ std::string(JZ_JS_WRAPPER_DIR) });   // 控件类层(widget/dist/index.js)靠它定位
        widgetModule.initEngine(&engine);

        auto api = engine.eval(
            "typeof uiLoader === 'object'"
            " && typeof uiLoader.loadFile === 'function'"
            " && typeof uiLoader.load === 'function'");
        check("uiLoader 暴露 loadFile/load", engine.fromJSValue<bool>(api.get()));

        // loadFile(文件路径) 直接返回根窗口(ui 即 JZWindow, 带 id 子控件属性)
        // CMake 已把 timer_ui.xml 拷到构建目录
        auto f = engine.eval(
            "var ui = uiLoader.loadFile('timer_ui.xml');"
            "ui instanceof JZWindow"
            " && ui.tip instanceof JZLabel"
            " && ui.btn instanceof JZPushButton"
            " && ui.btn.text() === 'Start'"
            " && ui.title() === 'Timer'");
        check("uiLoader.loadFile 返回根窗口且子控件可访问", engine.fromJSValue<bool>(f.get()));

        // load(字符串) 直接返回根窗口(单行 XML, 避免嵌入 JS 字符串时出现换行)
        std::string xmlStr = R"(<?xml version="1.0" encoding="UTF-8"?><ui><widget class="Window" name="root"><property name="title">X</property><property name="geometry">0,0,100,100</property></widget></ui>)";
        auto s = engine.eval(
            "var ui2 = uiLoader.load('" + xmlStr + "');"
            "ui2 instanceof JZWindow && ui2.title() === 'X';");
        check("uiLoader.load 返回根窗口", engine.fromJSValue<bool>(s.get()));

        // 非法文件/文本应抛异常(而不是返回不可用的对象)
        auto bad = engine.eval("uiLoader.loadFile('__not_exist__.xml');");
        check("uiLoader.loadFile 找不到文件时抛异常", engine.runtimeError().size() > 0 || JS_IsException(bad.get()));
    }

    // 7) JS 侧画笔: 创建请求经 JZJSPaintScope 动态转发(见 JZJSPaintScope.h)
    //    屏幕: new JZPainter(widget) —— 对应 C++ `JZPainter p(this)`, 生命周期 = 本次绘制派发;
    //    离屏: new JZPainter(image)  —— 自持图层, end() 时把内容合成回 image。
    jzDebug() << "===== JS 画笔生命周期演示 =====";
    {
        // 值类型(JZImage)交给 JS 时**所有权归 JS**: 给一份堆拷贝, JS 对象回收时由终结器 delete。
        // (C++ 栈对象不能这么交出去 —— 那种"借给 JS 看"的视图必须用完 disarmOpaque。)
        JZImage* image = new JZImage(JZImage::create(20, 10, JZColor(0, 0, 0, 0)));
        JZJSEngine engine;
        engine.init();
        JZJSWidgetModule widgetModule;
        engine.setLibraryDir({ std::string(JZ_JS_WRAPPER_DIR) });   // 控件类层(widget/dist/index.js)靠它定位
        widgetModule.initEngine(&engine);
        engine.setProperty(engine.globalObject(), "img", engine.toJsValueFromPtr(image));

        auto outside = engine.eval(
            "var r = 'no-throw';"
            "try { new JZPainter(null); } catch (e) { r = 'throw'; }"
            "r");
        check("onPaint 外 new JZPainter 抛异常", engine.fromJSValue<std::string>(outside.get()) == "throw");

        auto inside = engine.eval(
            "var win = new JZWindow(200, 120, 'Paint');"
            "var kept = null;"
            "var drew = 0;"
            "win.onPaint = () => { kept = new JZPainter(win); kept.fillRect(0, 0, 10, 10, '#ff0000'); drew++; };"
            "1");
        check("onPaint 注册成功", engine.fromJSValue<int>(inside.get()) == 1);

        // 手动派发一次 paint: 走 JZWidget::event -> paintEvent -> JS onPaint
        JZWidget* painted = engine.fromJSValue<JZWidget*>(engine.eval("win").get());
        JZPainterEvent paintEvent;
        paintEvent.setRect(JZRect(0, 0, 200, 120));
        jzApp->sendEvent(painted, &paintEvent);

        auto after = engine.eval(
            "var state = 'usable';"
            "try { kept.fillRect(0, 0, 1, 1, '#000000'); } catch (e) { state = 'released'; }"
            "drew + ':' + state");
        check("onPaint 内画笔可绘制, 派发结束后失效",
              engine.fromJSValue<std::string>(after.get()) == "1:released");

        // 约束② 同一次绘制内每个控件只能有一份画笔(第二份直接抛错, 不是静默忽略)
        engine.eval(
            "var dupState = 'no-throw';"
            "win.onPaint = () => {"
            "  var p1 = new JZPainter(win);"
            "  try { var p2 = new JZPainter(win); } catch (e) { dupState = 'throw'; }"
            "  p1.fillRect(0, 0, 3, 3, '#00ffff');"
            "};"
            "1");
        jzApp->sendEvent(painted, &paintEvent);
        check("同一控件一次绘制内不允许第二份画笔",
              engine.fromJSValue<std::string>(engine.eval("dupState").get()) == "throw");

        // 同一次派发内先 p.end() 提前结束: 既立刻失效, 也不能让派发末尾的回收重复 delete
        engine.eval(
            "var earlyState = 'usable';"
            "win.onPaint = () => {"
            "  var p = new JZPainter(win);"
            "  p.fillRect(0, 0, 2, 2, '#0000ff');"
            "  p.end();"
            "  try { p.fillRect(0, 0, 1, 1, '#000000'); } catch (e) { earlyState = 'released'; }"
            "};"
            "1");
        jzApp->sendEvent(painted, &paintEvent);
        check("onPaint 内 p.end() 提前结束且重复回收安全",
              engine.fromJSValue<std::string>(engine.eval("earlyState").get()) == "released");

        // 离屏: 动态转发到 JZPainter(JZImage&); 不受绘制派发约束, end() 立即结束并让包装失效
        auto off = engine.eval(
            "var offState = 'ok';"
            "var op = new JZPainter(img);"
            "op.fillRect(0, 0, 20, 10, '#00ff00');"
            "op.end();"
            "try { op.fillRect(0, 0, 1, 1, '#000000'); offState = 'usable'; } catch (e) { offState = 'released'; }"
            "offState");
        check("离屏 new JZPainter(image) 可绘制, end() 后失效",
              engine.fromJSValue<std::string>(off.get()) == "released");

        // 离屏画笔不调 end(): 对象被 JS 回收后由作用域结清(不崩、不泄漏), 期间目标 image 由作用域保活
        auto reap = engine.eval(
            "var op2 = new JZPainter(img);"
            "op2.fillRect(0, 0, 5, 5, '#0000ff');"
            "op2 = null;"
            "var junk = []; for (var i = 0; i < 5000; i++) junk.push({ a: i, s: 'x' + i });"
            "junk = null;"
            "var op3 = new JZPainter(img); op3.fillRect(0, 0, 3, 3, '#ff00ff'); op3.end();"
            "'ok'");
        check("离屏画笔被回收后由作用域结清(未 end 也不崩)",
              engine.fromJSValue<std::string>(reap.get()) == "ok");
    }

    // 8) JZTableWidget(指针版数据模型)自检。
    //    注: 原来这里还测 JZJSTableWidget 镜像类(JS 侧 item 对象的所有权/失效), 那套已删除 ——
    //    item 对象没有注册成 JS 类, 镜像类从未接线, 属于死代码; 将来真要做 JS 侧 item 接口时,
    //    按"全局 C 函数 + 状态放 JZJSWidgetModule::WidgetInfo::ext"的路子重做。
    jzDebug() << "===== JZTableWidget 指针版数据模型自检 =====";
    {
        JZTableWidget* tbl = new JZTableWidget();
        tbl->setRowCount(2);
        tbl->setColumnCount(2);

        // 单元格落位: 表格持有 + 记录行列号
        tbl->setItemText(0, 0, "A1");
        JZTableWidgetItem* a1 = tbl->item(0, 0);
        check("setItemText 建格并落位", a1 != nullptr && a1->text() == "A1");
        check("item 记录行列号", a1->row() == 0 && a1->column() == 0);
        check("表格反查行列", tbl->row(a1) == 0 && tbl->column(a1) == 0);

        // 重复 setItemText: 同格已有单元格时**复用**它(只改文本, 不换 item)
        tbl->setItemText(1, 1, "B2");
        JZTableWidgetItem* oldB2 = tbl->item(1, 1);
        tbl->setItemText(1, 1, "B2-x");
        check("重复 setItemText 复用同一单元格",
              tbl->item(1, 1) == oldB2 && tbl->itemText(1, 1) == "B2-x");

        // 摘除并删除(Qt 惯用法: 摘除 + 自己 delete)
        delete tbl->takeItem(0, 0);
        check("takeItem 后该格为空", tbl->item(0, 0) == nullptr);

        // 插删行列: item 自身记录的行列号跟随(基类 reindex 维护)
        tbl->setRowCount(3);
        tbl->setItemText(2, 0, "C1");
        JZTableWidgetItem* c1 = tbl->item(2, 0);
        tbl->insertRow(0);
        check("insertRow 后行号顺延", c1->row() == 3 && tbl->item(3, 0) == c1);
        tbl->removeRow(0);
        check("removeRow 后行号还原", c1->row() == 2 && tbl->item(2, 0) == c1);
        tbl->insertColumn(0);
        check("insertColumn 后列号顺延", c1->column() == 1 && tbl->item(2, 1) == c1);
        tbl->removeColumn(0);
        check("removeColumn 后列号还原", c1->column() == 0 && tbl->item(2, 0) == c1);

        // 整行删除 / 整表清空
        tbl->removeRow(2);
        check("removeRow 删掉整行单元格", tbl->item(2, 0) == nullptr && tbl->rowCount() == 2);
        tbl->setItemText(1, 0, "D1");
        tbl->clearContents();
        check("clearContents 后清空", tbl->item(1, 0) == nullptr && tbl->rowCount() == 2);

        delete tbl;
    }

    // 9) JZChart: JS 层暴露(JZJSWidgetModule::registWidgets 里 ClassBind 注册)。
    //    断言用字符串比较而不是 bool: eval 抛异常时 fromJSValue<bool> 仍可能为真(对象真值), 会假通过。
    jzDebug() << "===== JZChart JS 绑定演示 =====";
    {
        JZJSEngine engine;
        engine.init();
        JZJSWidgetModule widgetModule;
        engine.setLibraryDir({ std::string(JZ_JS_WRAPPER_DIR) });   // 控件类层(widget/dist/index.js)靠它定位
        widgetModule.initEngine(&engine);

        check("JZChart 类与类型常量已注册",
              evalStr(engine,
                      "(typeof JZChart === 'function'"
                      " && JZChart.LineChart === 0 && JZChart.BarChart === 1 && JZChart.PieChart === 2)"
                      " ? 'ok' : 'bad'") == "ok");

        check("JS 创建的 JZChart 是 JZWidget 派生",
              evalStr(engine, "(new JZChart() instanceof JZWidget) ? 'ok' : 'bad'") == "ok");

        // 类型/标题/分类/序列 往返: 读回的值来自真实 C++ 对象(chartType/title/categories/series*)
        check("JZChart 类型/标题/分类/序列往返",
              evalStr(engine,
                      "var c = new JZChart();"
                      "c.setChartType(JZChart.BarChart);"
                      "c.setTitle('Sales');"
                      "c.setCategories(['A', 'B', 'C']);"
                      "var s0 = c.addSeries('S1', '#339933');"
                      "c.setSeriesData(s0, [12, 18, 15]);"
                      "c.setShowValue(true);"
                      "var col = c.seriesColor(0);"
                      "(c.chartType() === 1 && s0 === 0 && c.seriesCount() === 1"
                      " && c.title() === 'Sales' && c.categories().join(',') === 'A,B,C'"
                      " && c.seriesName(0) === 'S1' && c.seriesData(0).join(',') === '12,18,15'"
                      " && col.red() === 51 && col.green() === 153 && col.blue() === 51)"
                      " ? 'ok' : 'bad'") == "ok");

        // 空颜色字符串 -> 默认调色板: 第 0 条 Blue(0,0,255), 第 1 条 Red(255,0,0)
        check("JZChart addSeries 空颜色取默认调色板",
              evalStr(engine,
                      "var c = new JZChart();"
                      "c.addSeries('A', '');"
                      "c.addSeries('B', '');"
                      "(c.seriesColor(0).blue() === 255 && c.seriesColor(1).red() === 255) ? 'ok' : 'bad'") == "ok");

        // setData 二维数组 -> 多条序列(名称/配色自动生成)
        check("JZChart setData(二维数组) 建多条序列",
              evalStr(engine,
                      "var c = new JZChart();"
                      "c.setData([[1, 2], [3, 4], [5, 6]]);"
                      "(c.seriesCount() === 3 && c.seriesName(1) === 'Series 2'"
                      " && c.seriesData(2).join(',') === '5,6') ? 'ok' : 'bad'") == "ok");

        // 越界: 读取抛异常(基类 series(i) 不做边界检查, JS 层必须拦), 写入按 C++ 语义静默忽略
        check("JZChart 越界: 读抛异常, 写静默忽略",
              evalStr(engine,
                      "var c = new JZChart();"
                      "var read = 'no-throw';"
                      "try { c.seriesName(0); } catch (e) { read = 'throw'; }"
                      "var write = 'no-throw';"
                      "try { c.setSeriesData(5, [1, 2]); } catch (e) { write = 'throw'; }"
                      "read + '/' + write") == "throw/no-throw");

        // 数组形参的类型校验(不再是"传啥都当空数组")
        check("JZChart setSeriesData 非数组抛异常",
              evalStr(engine,
                      "var c = new JZChart();"
                      "var r = 'no-throw';"
                      "try { c.setSeriesData(0, 'not-array'); } catch (e) { r = 'throw'; }"
                      "r") == "throw");

        // uiLoader 支持 Chart 类型(class + 属性 title)
        std::string chartXml = R"(<?xml version="1.0" encoding="UTF-8"?><ui><widget class="Window" name="root"><property name="geometry">0,0,240,160</property><widget class="Chart" name="chart"><property name="title">Trend</property><property name="geometry">0,0,240,160</property></widget></widget></ui>)";
        check("uiLoader 支持 Chart 类型",
              evalStr(engine,
                      "var uiC = uiLoader.load('" + chartXml + "');"
                      "(uiC.chart instanceof JZChart && uiC.chart.title() === 'Trend') ? 'ok' : 'bad'") == "ok");
    }

    // 10) 控件状态管理: addWidget 的所有权转移 / 父链切换 / 脱离父树 / 布局替换 / JS 包装保活。
    //     —— 覆盖 JZJSWidgetModule 的控件登记(保活)与 JZJSWidget.cpp 的 setParentWidget / setLayout。
    //     注意: 每次从 JS 取对象都要先把包装留在 C++ 变量里, 免得"取指针"用的是临时的返回值。
    jzDebug() << "===== 控件状态管理(所有权/父链/布局/保活)演示 =====";
    {
        JZJSEngine engine;
        connect(&engine.sigLog, [](const std::string& log) { jzDebug() << log; });
        engine.init();
        JZJSWidgetModule widgetModule;
        engine.setLibraryDir({ std::string(JZ_JS_WRAPPER_DIR) });   // 控件类层(widget/dist/index.js)靠它定位
        widgetModule.initEngine(&engine);

        // 新模型(2026-09-18 定案): **不做所有权翻转**。控件族对象的唯一指针槽是"守卫"
        // (JS 对象 opaque 里的 JZObjectPointer*, 见 JZJSWidgetModule 的 op 槽):
        //   1 = 守卫有效(底层对象还活着) / 0 = 底层对象已删(守卫被 ~JZObject 置空) / -1 = 空槽
        //   -2 = 不是控件(布局/值类型等仍是引擎注册类, opaque 是裸指针, 不能当守卫解)
        // 谁负责 delete: JS 侧(JS 对象被回收时由守卫决定"还该不该删") — 父控件先删过的就只放守卫。
        const JSClassID opClass = (JSClassID)engine.widgetOpClassId();
        auto aliveOf = [opClass](JSValueConst v) {
            JSClassID id = 0;
            void* opaque = JS_GetAnyOpaque(v, &id);
            if (!opaque)
                return -1;
            if (id != opClass)
                return -2;
            return ((JZObjectPointer*)opaque)->object() ? 1 : 0;
        };
        auto hasChild = [](JZObject* parent, JZObject* child) {
            for (JZObject* o : parent->children())
                if (o == child)
                    return true;
            return false;
        };

        JZJSValuePtr p1Val = engine.eval("var p1 = new JZWindow(200, 100, 'p1'); p1.setObjectName('p1'); p1");
        JZJSValuePtr p2Val = engine.eval("var p2 = new JZWindow(200, 100, 'p2'); p2.setObjectName('p2'); p2");
        JZJSValuePtr cVal  = engine.eval("var c = new JZPushButton(); c.setObjectName('c'); c");
        JZWidget* p1 = engine.fromJSValue<JZWidget*>(p1Val.get());
        JZWidget* p2 = engine.fromJSValue<JZWidget*>(p2Val.get());
        JZWidget* c  = engine.fromJSValue<JZWidget*>(cVal.get());

        // (a) setParentWidget: 真的挂成子控件(父链 / 对象树 / 布局项三处一致); 所有权**不翻转**
        check("setParentWidget 前控件已建好(守卫有效)", aliveOf(cVal.get()) == 1);
        evalStr(engine, "c.setParentWidget(p1); 'ok'");
        check("setParentWidget 不翻转所有权(仍归 JS)", aliveOf(cVal.get()) == 1);
        check("setParentWidget 后成为父控件的子控件(树+布局项)",
              c->parentWidget() == p1 && hasChild(p1, c) && !hasChild(p2, c));

        // (b) setParentWidget 换父: 同一个控件只属于一个父(旧父不再持有)
        check("setParentWidget 换父后父链指向新父",
              evalStr(engine, "c.setParentWidget(p2); c.parentWidget().objectName()") == "p2");
        check("换父后 C++ 对象树同步(旧父不再持有)",
              c->parentWidget() == p2 && !hasChild(p1, c) && hasChild(p2, c) && aliveOf(cVal.get()) == 1);

        // (c) 脱离父树: 交回 JS 拥有; 之后再挂回又能进树
        check("setParentWidget(null) 脱离父树",
              evalStr(engine, "c.setParentWidget(null); (c.parentWidget() === null) ? 'null' : 'obj'") == "null");
        check("脱离父树不改变所有权(控件仍活着, 只是没父了)",
              aliveOf(cVal.get()) == 1 && c->parentWidget() == nullptr && !hasChild(p2, c));
        check("重新挂回父树后父链恢复",
              evalStr(engine, "c.setParentWidget(p2); (c.parentWidget().objectName() === 'p2') ? 'ok' : 'bad'") == "ok"
              && aliveOf(cVal.get()) == 1 && hasChild(p2, c));

        // (d) setLayout 的 Qt 语义: 一个控件只能有一个布局, 且只能设一次;
        //     清空只能 clearLayout()(等价 Qt 的 `delete w->layout()`), 之后才能设新的。
        check("setLayout: 重复设置被拒绝、null 被拒绝(原布局不受影响)",
              evalStr(engine,
                      "var w = new JZWindow(240, 160, 'lw');"
                      "w.setLayout(new JZVBoxLayout());"
                      "var rep = 'accepted';"
                      "try { w.setLayout(new JZHBoxLayout()); } catch (e) { rep = 'rejected'; }"
                      "var nul = 'accepted';"
                      "try { w.setLayout(null); } catch (e) { nul = 'rejected'; }"
                      "var kept = (w.layout() instanceof JZVBoxLayout) ? 'vbox' : 'bad';"   // 原布局没被换也没被删
                      "console.log(rep + '/' + nul + '/' + kept);"
                      "rep + '/' + nul + '/' + kept") == "rejected/rejected/vbox");

        // 注: JZLayout 现在也继承 JZObject(2026-09-18) -> 布局的 JS 句柄带**守卫**, 被 C++ 删掉后
        // 再使用会抛"对象已被销毁", 不再是悬垂裸指针(见下面 "布局句柄有守卫" 那条)。
        check("clearLayout 后 layout() 为 null、可再设新布局",
              evalStr(engine,
                      "w.clearLayout();"
                      "var cleared = (w.layout() === null) ? 'null' : 'obj';"
                      "w.setLayout(new JZHBoxLayout());"
                      "cleared + '/' + ((w.layout() instanceof JZHBoxLayout) ? 'hbox' : 'bad')")
              == "null/hbox");

        check("layout() 返回同一个包装(缓存, 引用相等)",
              evalStr(engine, "(w.layout() === w.layout()) ? 'same' : 'diff'") == "same");
        {
            JZJSValuePtr wVal = engine.eval("w");
            JZWidget* w = engine.fromJSValue<JZWidget*>(wVal.get());
            check("C++ 侧布局已是 HBox", dynamic_cast<JZHBoxLayout*>(w->layout()) != nullptr);

            JZJSValuePtr view = engine.eval("w.layout()");
            check("布局的 opaque 不是控件族那个 op 槽(它走自己类的守卫)", aliveOf(view.get()) == -2);

            // 布局继承 JZObject 的直接收益: C++ 侧把布局删掉, JS 那份句柄自动失效(以前是裸指针 -> UAF)
            JZJSValuePtr lvVal = engine.eval("var lv = new JZVBoxLayout(4, 4); lv;");
            JZLayout* lvRaw = engine.fromJSValue<JZLayout*>(lvVal.get());
            check("布局: JS 句柄能取回底层对象", lvRaw != nullptr && lvRaw->spacing() == 4);
            delete lvRaw;   // C++ 侧收走(模拟"布局被 C++ 删掉"的路径)
            check("布局句柄有守卫: C++ 删掉后 JS 再用 -> 抛异常(不 UAF)",
                  evalStr(engine, "var gr='ok'; try { lv.count(); } catch (e) { gr='throw'; } gr;") == "throw");
        }

        // (e) 保活: JS 丢掉子控件引用 + 强制 GC 后包装仍必须活着(否则 m_jsvalue 悬垂),
        //     事件照旧能派发到 JS。没有保活时这一段会失效/崩 —— 正是本轮修的问题。
        evalStr(engine,
                "var kw = new JZWindow(200, 120, 'keep');"
                "var kc = new JZWidget();"
                "kc.setFixedSize(40, 20);"
                "globalThis.__hits = 0;"
                "kc.onMousePress = function (e) { globalThis.__hits++; };"
                "kw.addWidget(kc);"
                "globalThis.__win = kw;"     // 顶层窗口留给 JS 拥有; 子控件之后只由 C++ 对象树持有
                "'ok'");
        // 只取裸 C++ 指针(取完就把 JS 包装放掉): 之后 kc 的包装上就没有任何 JS 侧引用了,
        // 只剩"模块保活"那一份 —— 保活失效则包装被回收, 下面的 m_jsvalue 就是悬垂的。
        JZWidget* kc = engine.fromJSValue<JZWidget*>(engine.eval("kc").get());
        evalStr(engine, "kc = null; 'ok'");                    // JS 全局也放手
        JS_RunGC(JS_GetRuntime(engine.context()));             // 强制回收一轮: 没保活的话包装就没了
        check("保活: GC 后包装未失效且仍能派发",
              kc != nullptr && kc->parentWidget() != nullptr);
        {
            JZMouseEvent press(JZMouseEvent::Press, JZMouseEvent::LeftButton,
                               JZPoint(5, 5), JZPoint(5, 5));
            jzApp->sendEvent(kc, &press);
        }
        check("保活: GC 后子控件事件仍派发到 JS",
              evalStr(engine, "String(globalThis.__hits)") == "1");
    }

    // 11) JS 构造器(C 版本): 实参形态分派 + 构造后登记控件表(有父控件就同步保活)。
    //     —— 覆盖 js_JZWidget_ctor 系列 / jzRegistWidgetJsConstructors(JZJSWidget.cpp)。
    jzDebug() << "===== JS 构造器(实参形态/登记/保活)演示 =====";
    {
        JZJSEngine engine;
        engine.init();
        JZJSWidgetModule widgetModule;
        engine.setLibraryDir({ std::string(JZ_JS_WRAPPER_DIR) });   // 控件类层(widget/dist/index.js)靠它定位
        widgetModule.initEngine(&engine);

        auto hasChild = [](JZObject* parent, JZObject* child) {
            for (JZObject* o : parent->children())
                if (o == child)
                    return true;
            return false;
        };

        // 值类型: 实参真的写进底层对象(以前 create 一律忽略实参)
        check("构造器实参: JZWindow(w,h,title) / JZPoint / JZRect / JZSize / JZColor",
              evalStr(engine,
                      "var cw = new JZWindow(320, 200, 'CTor');"
                      "var cp = new JZPoint(3, 4);"
                      "var cs = new JZSize(5, 6);"
                      "var cr = new JZRect(1, 2, 30, 40);"
                      "var cc = new JZColor(255, 0, 0);"
                      "((cw.title() === 'CTor' && cw.width() === 320 && cw.height() === 200)"
                      " && cp.x() === 3 && cp.y() === 4"
                      " && cs.width() === 5 && cs.height() === 6"
                      " && cr.x() === 1 && cr.y() === 2 && cr.width() === 30 && cr.height() === 40"
                      " && cc.red() === 255 && cc.green() === 0 && cc.blue() === 0) ? 'ok' : 'bad'") == "ok");

        // 控件: 带文本/带尺寸/带方向的形态
        check("构造器实参: 控件 (text,parent) / (title,parent) / (orientation,parent)",
              evalStr(engine,
                      "var hw = new JZWindow(300, 180, 'host');"
                      "var lb = new JZLabel('hello', hw);"
                      "var bt = new JZPushButton('OK', hw);"
                      "var rb = new JZRadioButton('r1', hw);"
                      "var gb = new JZGroupBox('grp', hw);"
                      "var sp = new JZSplitter(1, hw);"
                      "var vl = new JZVBoxLayout(3, 7);"
                      "((lb.text() === 'hello' && bt.text() === 'OK')"
                      " && rb.text() === 'r1' && gb.title() === 'grp' && vl.spacing() === 7) ? 'ok' : 'bad'") == "ok");

        // 构造时就带 parent: 父链/对象树立刻成立(C++ 侧核对), 且登记进模块的控件表
        {
            JZWidget* hw = engine.fromJSValue<JZWidget*>(engine.eval("hw").get());
            JZWidget* lb = engine.fromJSValue<JZWidget*>(engine.eval("lb").get());
            check("构造时带 parent: 父链即时生效且进了对象树",
                  lb->parentWidget() == hw && hasChild(hw, lb));
        }

        // 实参不合法要报错(以前是"静默忽略")
        check("构造器实参不合法时抛错",
              evalStr(engine,
                      "var e1 = 'no-throw';"
                      "try { new JZWindow(100); } catch (e) { e1 = 'throw'; }"
                      "var e2 = 'no-throw';"
                      "try { new JZRect(1, 2, 3); } catch (e) { e2 = 'throw'; }"
                      "e1 + '/' + e2") == "throw/throw");

        // 构造带父的控件也保活(走 registerCreatedWidget 这条登记入口):
        // JS 丢掉引用 + GC 之后, 事件仍要能派发到 JS。
        evalStr(engine,
                "var kw2 = new JZWindow(200, 120, 'keep2');"
                "var kc2 = new JZWidget(kw2);"            // ← 构造直接带父
                "kc2.setFixedSize(30, 20);"
                "globalThis.__hits2 = 0;"
                "kc2.onMousePress = function (e) { globalThis.__hits2++; };"
                "globalThis.__win2 = kw2;"
                "'ok'");
        JZWidget* kc2 = engine.fromJSValue<JZWidget*>(engine.eval("kc2").get());
        evalStr(engine, "kc2 = null; 'ok'");
        JS_RunGC(JS_GetRuntime(engine.context()));
        {
            JZMouseEvent press(JZMouseEvent::Press, JZMouseEvent::LeftButton, JZPoint(5, 5), JZPoint(5, 5));
            jzApp->sendEvent(kc2, &press);
        }
        check("构造带父的控件同样保活(GC 后事件仍派发到 JS)",
              evalStr(engine, "String(globalThis.__hits2)") == "1");
    }

    // 12) 容器专用挂载路径: StackedWidget.addWidget / TabWidget.addTab / ScrollArea.setWidget /
    //     Splitter.addWidget —— 与 addWidget 同一套(JS 侧登记 + 保活, 见 tree.ts)。
    //     验证: 挂进去 → JS 丢掉引用 → 强制 GC → 事件仍能派发到 JS(包装还活着 = 保活生效)。
    jzDebug() << "===== 容器挂载路径(登记/保活)演示 =====";
    {
        JZJSEngine engine;
        connect(&engine.sigLog, [](const std::string& log) { jzDebug() << log; });
        engine.init();
        JZJSWidgetModule widgetModule;
        engine.setLibraryDir({ std::string(JZ_JS_WRAPPER_DIR) });   // 控件类层(widget/dist/index.js)靠它定位
        widgetModule.initEngine(&engine);

        const JSClassID opClass = (JSClassID)engine.widgetOpClassId();
        auto aliveOf = [opClass](JSValueConst v) {
            JSClassID id = 0;
            void* opaque = JS_GetAnyOpaque(v, &id);
            if (!opaque || id != opClass)
                return -1;
            return ((JZObjectPointer*)opaque)->object() ? 1 : 0;
        };

        // 丢掉 JS 引用 + 强制 GC, 然后往控件上派发一次鼠标按下, 返回 JS 侧计数器。
        // 保活来自 JS 侧对象树(tree.ts: 父/容器持子控件的强引用), 所以 GC 后包装与事件都得还在。
        auto revive = [&engine, &aliveOf](JSValueConst childVal, const char* childName, const char* hitsName) {
            JZWidget* child = engine.fromJSValue<JZWidget*>(childVal);
            bool mounted = (aliveOf(childVal) == 1);
            evalStr(engine, std::string(childName) + " = null; 'ok'");
            JS_RunGC(JS_GetRuntime(engine.context()));

            JZMouseEvent press(JZMouseEvent::Press, JZMouseEvent::LeftButton, JZPoint(3, 3), JZPoint(3, 3));
            jzApp->sendEvent(child, &press);

            std::string hits = evalStr(engine, std::string("String(globalThis.") + hitsName + ")");
            return mounted && hits == "1";
        };

        // ① JZStackedWidget.addWidget(test_jswidget 的"页"就是这条路径)
        auto eval_result = evalStr(engine,
                "var w1 = new JZWindow(200, 120, 'w1');"
                "var st = new JZStackedWidget();"
                "var pg = new JZWidget();"
                "pg.setFixedSize(20, 10);"
                "globalThis.__h1 = 0;"
                "pg.onMousePress = function () { globalThis.__h1++; };"
                "st.addWidget(pg);"
                "w1.addWidget(st);"
                "globalThis.__w1 = w1;"
                "'ok'");
        {
            JZJSValuePtr pgVal = engine.eval("pg");
            check("JZStackedWidget.addWidget: 转移所有权 + GC 后保活", revive(pgVal.get(), "pg", "__h1"));
        }

        // ② JZTabWidget.addTab(页面会被挂到内部页面容器上, 但所有权归标签控件)
        evalStr(engine,
                "var w2 = new JZWindow(240, 140, 'w2');"
                "var tw = new JZTabWidget();"
                "var tp = new JZWidget();"
                "globalThis.__h2 = 0;"
                "tp.onMousePress = function () { globalThis.__h2++; };"
                "tw.addTab(tp, 'A');"
                "w2.addWidget(tw);"
                "globalThis.__w2 = w2;"
                "'ok'");
        {
            JZJSValuePtr tpVal = engine.eval("tp");
            check("JZTabWidget.addTab: 转移所有权 + GC 后保活", revive(tpVal.get(), "tp", "__h2"));
        }

        // ③ JZScrollArea.setWidget(内容控件由滚动区持有, 内部再挂到 viewport 下)
        evalStr(engine,
                "var w3 = new JZWindow(240, 160, 'w3');"
                "var sa = new JZScrollArea();"
                "var ct = new JZWidget();"
                "globalThis.__h3 = 0;"
                "ct.onMousePress = function () { globalThis.__h3++; };"
                "sa.setWidget(ct);"
                "w3.addWidget(sa);"
                "globalThis.__w3 = w3;"
                "'ok'");
        {
            JZJSValuePtr ctVal = engine.eval("ct");
            check("JZScrollArea.setWidget: 转移所有权 + GC 后保活", revive(ctVal.get(), "ct", "__h3"));
        }

        // ④ JZSplitter.addWidget / insertWidget
        evalStr(engine,
                "var w4 = new JZWindow(260, 160, 'w4');"
                "var sp = new JZSplitter(1);"
                "var pn = new JZWidget();"
                "globalThis.__h4 = 0;"
                "pn.onMousePress = function () { globalThis.__h4++; };"
                "sp.addWidget(pn);"
                "w4.addWidget(sp);"
                "globalThis.__w4 = w4;"
                "'ok'");
        {
            JZJSValuePtr pnVal = engine.eval("pn");
            check("JZSplitter.addWidget: 转移所有权 + GC 后保活", revive(pnVal.get(), "pn", "__h4"));
        }

        // ④b 回吐子控件的 getter: 由 JS 侧对象树回答(C++ 只有裸指针, 变不回同一个 JS 对象) ——
        //     拿回的就是你当初 addXxx 传进去的那份包装(`===` 成立), 下标与 C++ 存放顺序一致。
        check("容器 getter: widget(i)/currentWidget()/widget() 拿回同一份包装",
              evalStr(engine,
                      "var sc = new JZStackedWidget();"
                      "var p0 = new JZWidget(), p1 = new JZWidget();"
                      "var i0 = sc.addWidget(p0); sc.addWidget(p1);"
                      "var ins = new JZWidget(); sc.insertWidget(1, ins);"
                      "sc.setCurrentIndex(2);"
                      "var ok1 = (i0 === 0 && sc.count() === 3 && sc.widget(0) === p0"
                      "           && sc.widget(1) === ins && sc.widget(2) === p1);"
                      "var ok2 = (sc.currentWidget() === p1);"
                      "sc.removeWidget(1);"
                      "var ok3 = (sc.count() === 2 && sc.widget(1) === p1);"
                      "var ta = new JZTabWidget();"
                      "var t0 = new JZWidget(); ta.addTab(t0, 'A');"
                      "var ok4 = (ta.widget(0) === t0 && ta.tabText(0) === 'A'"
                      "           && ta.currentWidget() === ta.widget(ta.currentIndex())"
                      "           && ta.indexOf(t0) === 0);"
                      "var sa = new JZScrollArea();"
                      "var ct = new JZWidget(); sa.setWidget(ct);"
                      "var ok5 = (sa.widget() === ct);"
                      "var sp = new JZSplitter();"
                      "var s0 = new JZWidget(), s1 = new JZWidget();"
                      "sp.addWidget(s0); sp.addWidget(s1);"
                      "var ok6 = (sp.widget(0) === s0 && sp.widget(1) === s1 && sp.count() === 2);"
                      "String(ok1 && ok2 && ok3 && ok4 && ok5 && ok6)")
                      == "true");

        // ⑤ onPaint / onMousePress 这类回调: JS 侧写法不变(`w.onPaint = fn`), 但回调**存在模块的槽里**
        //    (见 JZJSWidgetModule::WidgetInfo::callbacks) —— 与包装对象的身份/生命周期解耦:
        //    丢 JS 变量、包装被 GC、getter 每次拿到哪份包装, 都不影响事件转发。
        evalStr(engine,
                "var w5 = new JZWindow(240, 160, 'w5');"
                "var sa5 = new JZScrollArea();"
                "var cw = new JZWidget();"
                "globalThis.__p5 = 0;"
                "cw.onPaint = function () { globalThis.__p5++; };"
                "sa5.setWidget(cw);"
                "w5.addWidget(sa5);"
                "globalThis.__w5 = w5;"
                // 新模型: 父子/容器关系由 JS 便利层登记(@jz/widget 的 tree.ts), 所以 children() 拿回的
                // 就是你当初传进去的那份包装(`===` 成立, 保活也靠这份强引用)。
                "globalThis.__sameWidget = (sa5.children()[0] === cw) ? 'same' : 'diff';"
                "'ok'");
        {
            JZWidget* cw = engine.fromJSValue<JZWidget*>(engine.eval("cw").get());
            evalStr(engine, "cw = null; 'ok'");            // JS 变量放手
            JS_RunGC(JS_GetRuntime(engine.context()));     // 强制回收: 没保活则包装(onPaint)就没了
            {
                JZPainterEvent pe;
                pe.setRect(JZRect(0, 0, 20, 20));
                jzApp->sendEvent(cw, &pe);
            }
            check("setWidget 内容的 onPaint: 丢引用+GC 后仍派发到 JS",
                  evalStr(engine, "String(globalThis.__p5)") == "1");
            // JS 侧登记的关系: children()/parent() 拿回的是**同一份**包装(不是现造的), `===` 成立。
            check("JS 侧登记的关系拿回同一份包装(=== 成立)",
                  evalStr(engine, "globalThis.__sameWidget") == "same");

            // ⑥ 回调就是实例上的普通属性(派发时现读): 读回同一个函数; 赋非函数**不校验**(等到派发时
            //    按"这一项没有回调"处理, 回退 C++ 默认实现); 清空写 null。
            evalStr(engine,
                    "globalThis.__p6 = 0;"
                    "globalThis.__readback = 'bad';"
                    "globalThis.__own = 'bad';"
                    "globalThis.__typeerr = 'no-throw';"
                    "var sa6 = new JZScrollArea();"
                    "var c6 = new JZWidget();"
                    "sa6.setWidget(c6);"
                    "var f6 = function () { globalThis.__p6++; };"
                    "c6.onPaint = f6;"
                    "globalThis.__readback = (c6.onPaint === f6) ? 'same-fn' : 'diff-fn';"
                    "globalThis.__own = Object.prototype.hasOwnProperty.call(c6, 'onPaint') ? 'own' : 'proto';"
                    "try { c6.onPaint = 123; } catch (e) { globalThis.__typeerr = 'throw'; }"
                    "c6.onPaint = f6;"   // 上面赋了非函数: 这里放回函数, 后面还要用
                    "'ok'");
            check("onPaint 读回同一个函数(就是实例属性)",
                  evalStr(engine, "globalThis.__readback") == "same-fn");
            check("onPaint 是实例上的普通属性(不是原型访问器)",
                  evalStr(engine, "globalThis.__own") == "own");
            check("onPaint 赋非函数不抛(派发时按没回调处理)",
                  evalStr(engine, "globalThis.__typeerr") == "no-throw");

            // 回调照旧生效(存在 C 侧, 与包装无关)
            JZWidget* c6w = engine.fromJSValue<JZWidget*>(engine.eval("c6").get());
            {
                JZPainterEvent pe6;
                pe6.setRect(JZRect(0, 0, 20, 20));
                jzApp->sendEvent(c6w, &pe6);
            }
            check("C 侧槽里的 onPaint 正常派发", evalStr(engine, "String(globalThis.__p6)") == "1");

            // null 清空: 之后不再转发(事件走原生实现)。
            // 这里用 show 而不是 paint 验证 —— 原生 paint 路径会 `JZPainter p(this)`, 需要控件已在
            // 真实绘制流程里(有 layer); 裸 sendEvent 一个 paint 必然崩, 与"回调存在哪"无关。
            evalStr(engine,
                    "globalThis.__s6 = 0;"
                    "c6.onShow = function () { globalThis.__s6++; };"
                    "'ok'");
            {
                JZShowEvent se6;
                jzApp->sendEvent(c6w, &se6);
            }
            check("onShow 生效(C 侧槽)", evalStr(engine, "String(globalThis.__s6)") == "1");
            evalStr(engine, "c6.onShow = null; 'ok'");
            {
                JZShowEvent se6;
                jzApp->sendEvent(c6w, &se6);
            }
            check("onShow = null 清空回调(不再派发)", evalStr(engine, "String(globalThis.__s6)") == "1");
            check("清空后 getter 返回 null", evalStr(engine, "String(c6.onShow)") == "null");

            // ⑦ 属性系统: `w.setProperty(name, value)` / `property(name)` —— **纯 JS 侧存**
            //    (@jz/widget 的 props.ts 用 WeakMap 挂在 JZWidget 原型上), 不落 C++:
            //    值原样(同一身份)、函数/控件实例都能挂、控件回收时属性跟着走。
            evalStr(engine,
                    "c6.setProperty('answer', 42);"
                    "c6.setProperty('pi', 3.5);"
                    "c6.setProperty('label', 'hi');"
                    "c6.setProperty('on', true);"
                    "c6.setProperty('list', [1, 'a', false]);"
                    "c6.setProperty('obj', { n: 7 });"
                    "c6.setProperty('fn', function () { return 1; });"
                    "'ok'");

            check("property: 标量读回(仍是原生类型)",
                  evalStr(engine, "String(c6.property('answer')) + '|' + String(c6.property('pi')) + '|' + String(c6.property('on')) + '|' + c6.property('label')")
                      == "42|3.5|true|hi");
            check("property: JS 值原样读回(同一身份 + 内容对)",
                  evalStr(engine, "var l2 = c6.property('list'); String(l2 === c6.property('list')) + '|' + String(l2[1]) + '|' + String(c6.property('obj').n) + '|' + String(c6.property('fn')())")
                      == "true|a|7|1");
            check("setProperty: 控件实例也能挂(同一身份)",
                  evalStr(engine, "try { c6.setProperty('w', c6); String(c6.property('w') === c6); } catch (e) { 'throw'; }") == "true");
            check("property: 未设置的属性 -> null", evalStr(engine, "String(c6.property('nope'))") == "null");

            // 存 JS 侧的硬证据: C++ 的属性表里查不到这些名字(引擎自己的宿主句柄 js_value 不算)
            check("setProperty 不落 C++ 侧(底层属性表里没有)",
                  !c6w->property("answer").isValid() && !c6w->property("list").isValid() && !c6w->property("obj").isValid());

            // null / undefined = 移除该属性
            evalStr(engine, "c6.setProperty('label', null); c6.setProperty('obj', undefined); 'ok'");
            check("setProperty: 传 null/undefined 移除属性",
                  evalStr(engine, "String(c6.property('label')) + '|' + String(c6.property('obj'))") == "null|null");

            // ⑧ 对话框绑定(registDialogs): 类/全局对象与接口在即可。
            //    不在这里调 exec(): 它会真开模态循环, 无桌面会话下没有可交互的窗口(桌面环境里另测)。
            // 信号是**属性**(onFinished 等): 未设置时读回 null, 赋值后才是函数 —— 这里先赋一个再查类型
            check("JZDialog: 已注册(可 new + result/accept/reject; onFinished 是可写属性)",
                  evalStr(engine,
                          "var d8 = new JZDialog();"
                          "d8.onFinished = function (r) {};"
                          "d8.result() + '|' + (typeof d8.exec) + '|' + (typeof d8.accept) + '|' + (typeof d8.onFinished)")
                      == "0|function|function|function");
            check("JZMessageBox: 静态接口在",
                  evalStr(engine, "(typeof JZMessageBox.information) + '|' + (typeof JZMessageBox.warning) + '|' + (typeof JZMessageBox.question)")
                      == "function|function|function");
            check("JZFileDialog: 4 个静态接口都在",
                  evalStr(engine, "[JZFileDialog.getOpenFileName, JZFileDialog.getOpenFileNames, JZFileDialog.getSaveFileName, JZFileDialog.getExistingDirectory].every(function (f) { return typeof f === 'function'; })")
                      == "true");
            check("deleteLater: 已绑定(嵌套模态里删控件的正规做法)",
                  evalStr(engine, "typeof d8.deleteLater") == "function");

            // ⑨ 信号属性化(defSignalProperty): 与事件槽同一条规则(都是**实例上的普通属性**, 派发时
            //    现读) —— 赋值设 / 覆盖上一次 / 赋 null 清空 / 赋非函数不校验(派发时按"没回调"处理)。
            //    用 JZSpinBox::setValue 触发, 验证"信号接到了最新那个回调上"。
            check("信号是属性: 覆盖式 + null 清空 + 赋非函数不抛",
                  evalStr(engine,
                          "(function(){ var sp = new JZSpinBox(); sp.setRange(0, 100); var log = '';"
                          " sp.onValueChanged = function (v) { log += 'A' + v; };"
                          " sp.onValueChanged = function (v) { log += 'B' + v; };"   // 覆盖: 只该跑 B
                          " sp.setValue(7);"
                          " sp.onValueChanged = null;"                              // 清空: 之后再 emit 不调
                          " sp.setValue(8);"
                          " var thrown = 'none';"
                          " try { sp.onValueChanged = 1; } catch (e) { thrown = 'throw'; }"
                          " return log + '|' + thrown + '|' + String(sp.onValueChanged); })()")
                      // 读回就是你赋进去的那个值(1) —— 新模型下回调槽就是实例上的普通属性, 不校验赋值
                      == "B7|none|1");

            // 探针: 绑定类能不能被 JS 继承 —— bteserver 的 UI 类写的是 `class XxxDialog extends QDialog`
            // 这种风格, 若将来去掉 Qt 适配层改用真实绑定, 这一步必须先成立。
            // 这里把结论拼进断言名(始终 PASS, 只为把诊断打出来)。
            check("JZSettings: 首次运行自建文件 + set/get 往返 + 分组",
                  evalStr(engine,
                          "var st8 = new JZSettings();"
                          "var ok8 = st8.open('jzsettings_probe.ini');"
                          "st8.setValue('k', 'v');"
                          "st8.setInt('n', 7);"
                          "st8.beginGroup('g'); st8.setValue('x', 'y'); var gv8 = st8.getValue('x'); st8.endGroup();"
                          "var h8 = st8.hasValue('k');"
                          "st8.sync();"
                          "ok8 + '|' + st8.getValue('k') + '|' + st8.getInt('n') + '|' + gv8 + '|' + h8")
                      == "true|v|7|y|true");

            // 值类型的等值: JS/TS 没有运算符重载 ⇒ 值类型(JZPoint/JZSize/JZRect/JZColor/JZFont)
            // 一律提供 equal(other)(C++ 侧 operator==); `===` 比的是包装身份, 两份独立构造的值恒不等。
            check("值类型 equal(): 比内容不比包装 ===",
                  evalStr(engine,
                          "var p1 = new JZPoint(1, 2), p2 = new JZPoint(1, 2), p3 = new JZPoint(1, 3);"
                          "var s1 = new JZSize(3, 4), s2 = new JZSize(3, 4);"
                          "var r1 = new JZRect(1, 2, 3, 4), r2 = new JZRect(1, 2, 3, 4), r3 = new JZRect(1, 2, 3, 5);"
                          "var c1 = new JZColor(255, 0, 0), c2 = new JZColor(255, 0, 0), c3 = new JZColor(255, 0, 1);"
                          "var f1 = new JZFont('A', 12), f2 = new JZFont('A', 12), f3 = new JZFont('A', 13);"
                          "var T = function (v) { return v ? 'T' : 'F'; };"
                          "var w = new JZWidget(); w.setGeometry(1, 2, 30, 40);"
                          "T(p1.equal(p2)) + T(p1.equal(p3)) + T(p1 !== p2)"
                          " + T(s1.equal(s2))"
                          " + T(r1.equal(r2)) + T(r1.equal(r3))"
                          " + T(c1.equal(c2)) + T(c1.equal(c3))"
                          " + T(f1.equal(f2)) + T(f1.equal(f3))"
                          " + T(w.geometry().equal(new JZRect(1, 2, 30, 40)))")
                      == "TFTTTFTFTFT");

            // 句柄指向同一个 C++ 对象吗? 新模型里**没有引擎侧身份缓存**(2026-09-17 随包装层删掉了):
            // 身份由"谁登记"决定 —— JS 侧登记的关系(setParent / addWidget / ...)拿回的就是你当初传进去
            // 的那份包装(=== 成立); 而"现造包装"的 getter(值类型, 如 w.geometry())每次都是新对象,
            // 比内容要用 equal()(见上一条)。这里验证前一半 + 跨父不串。
            check("句柄身份: JS 侧登记的关系 === 成立(没有引擎侧身份缓存)",
                evalStr(engine,
                    "var pa = new JZWidget(), pb = new JZWidget();"
                    "var ca = new JZWidget(), cb = new JZWidget();"
                    "ca.setParentWidget(pa);"
                    "cb.setParentWidget(pb);"
                    "String(pa.children()[0] === ca)"                       // 父拿回子: 同一份
                    " + '|' + String(ca.parentWidget() === pa)"              // 子拿回父: 同一份
                    " + '|' + ((pa.children()[0] === pb.children()[0]) ? 'same' : 'diff')")   // 跨父不串
                == "true|true|diff");

            // JS 侧**不暴露** item 对象(JZTableWidgetItem/JZListWidgetItem 没注册成 JS 类; 那套
            // 所有权/失效逻辑从未接线, 属死代码 —— 见 JZJSWidgetModule::registViews 的说明)。
            // 这里验证"值级"接口(文本/计数/当前项/行列数) + 下面的**视图镜像**(dataview);
            // 控件本身的能力由第 8 节的 C++ 用例覆盖。
            check("JZTableWidget(值级接口): setItemText/itemText + 行列数 + 当前格",
                  evalStr(engine,
                          "var tw8 = new JZTableWidget();"
                          "tw8.setRowCount(2); tw8.setColumnCount(2);"
                          "tw8.setItemText(0, 0, 'cell');"
                          "var s1 = tw8.rowCount() + '|' + tw8.columnCount() + '|' + tw8.itemText(0, 0);"
                          "tw8.setCurrentCell(1, 1);"
                          "s1 + '|' + tw8.currentRow() + '|' + tw8.currentColumn()")
                      == "2|2|cell|1|1");

            check("JZListWidget(值级接口): appendItem/appendItems/count/itemText/currentIndex",
                  evalStr(engine,
                          "var lw8 = new JZListWidget();"
                          "lw8.appendItem('a');"
                          "lw8.appendItems(['b', 'c']);"
                          "var s4 = lw8.count() + '|' + lw8.itemText(0) + '|' + lw8.itemText(2);"
                          "lw8.setCurrentIndex(1);"
                          "s4 + '|' + lw8.currentIndex()")
                      == "3|a|c|1");

            // ---- dataview: JS 侧"视图镜像"与控件双向绑定(见 @jz/widget 的 dataview.ts) ----
            // 视图侧增删 -> 控件立刻同步; 控件侧改数据 -> 视图自动重读(靠 onDataChanged 回灌)。
            check("dataview(表格): 视图增删改 <-> 控件同步",
                  evalStr(engine,
                          "var tw9 = new JZTableWidget();"
                          "var dv9 = tw9.dataView();"
                          "dv9.push(['A', 1]);"                 // 数字也收(按文本存)
                          "dv9.push(['B', 2]);"
                          "var s = tw9.rowCount() + '|' + tw9.columnCount() + '|' + tw9.itemText(1, 0) + '|' + tw9.itemText(0, 1);"
                          "dv9[1] = ['B2', 22];"                 // 下标赋值 = 换整行
                          "s += '|' + tw9.itemText(1, 0) + '|' + tw9.itemText(1, 1) + '|' + dv9[1][0];"
                          "dv9.splice(0, 1);"                    // 数组习惯删一行
                          "s += '|' + tw9.rowCount() + '|' + tw9.itemText(0, 0) + '|' + dv9.length;"
                          "dv9.columns(['名称', '值']);"         // 列(表头)
                          "s + '|' + dv9.columns()[1]")
                      == "2|2|B|1|B2|22|B2|1|B2|1|值");

            check("dataview(表格): 控件侧改数据 -> 视图自动回灌 + 值级快照",
                  evalStr(engine,
                          "tw9.setItemText(0, 0, 'C');"          // C++ 侧改文本
                          "var a = dv9[0][0];"
                          "tw9.setRowCount(3);"                  // C++ 侧改行数
                          "var b = dv9.length;"
                          "var snap = dv9.rows();"
                          "snap[0][0] = 'X';"                    // 改快照不脏镜像
                          "a + '|' + b + '|' + dv9[0][0] + '|' + snap.length")
                      == "C|3|C|3");

            check("dataview(列表): push/remove/长度 + 控件侧同步",
                  evalStr(engine,
                          "var lw9 = new JZListWidget();"
                          "var lv9 = lw9.dataView();"
                          "lv9.push('a'); lv9.push('b'); lv9.push('c');"
                          "var s = lw9.count() + '|' + lw9.itemText(2) + '|' + lv9[1];"
                          "lv9.remove(0);"
                          "s += '|' + lw9.count() + '|' + lw9.itemText(0) + '|' + lv9[0];"
                          "lw9.appendItem('d');"                 // 控件侧加 -> 视图回灌
                          "s + '|' + lv9.length + '|' + lv9[2]")
                      == "3|c|b|2|b|b|3|d");

            check("dataview: 同一控件只有一份视图 + 取到的是数组习惯的视图对象",
                  evalStr(engine,
                          "var same = (tw9.dataView() === dv9) ? 'same' : 'diff';"
                          "var it = [];"
                          "for (var r of dv9) it.push(r[0]);"
                          "same + '|' + it.join(',')")
                      == "same|C,,");

            check("list.itemData/setItemData: 业务数据槽原样往返(没有 item 句柄)",
                  evalStr(engine,
                          "var lw10 = new JZListWidget();"
                          "lw10.appendItem('dev');"
                          "lw10.appendItem('dev2');"
                          "var payload = { netId: 7, id: 'A1' };"
                          "lw10.setItemData(0, payload);"
                          "lw10.setItemData(1, ['x', 'y']);"
                          "var d0 = lw10.itemData(0);"
                          "(d0 === payload) + '|' + d0.netId + '|' + d0.id + '|' + lw10.itemData(1)[1]")
                      == "true|7|A1|y");

            // 菜单: **句柄式**为主(uiLoader 生成的 setupUi() 与 app 都按这个写):
            //   addAction/addMenu/actionAt/menuAt -> JZAction/JZMenu 句柄; 句柄上挂 onTriggered。
            // 另有下标式的便捷形式(setActionXxx / isActionChecked / onActionTriggered 下标回调)。
            // ⚠ 句柄是"借用"(对象由菜单持有) —— JS 侧由 @jz/widget 的 menus.ts 保活。
            check("JZMenu: addAction 返回 JZAction 句柄 + 下标式状态读写",
                  evalStr(engine,
                          "var mn = new JZMenu();"
                          "var a0 = mn.addAction('a'); var a1 = mn.addAction('b');"
                          "a1.setCheckable(true); a1.setChecked(true); a1.setEnabled(false);"
                          "globalThis.__menu = mn; globalThis.__menuHit = -1; globalThis.__a0Hit = 0;"
                          "mn.onActionTriggered = function (i) { globalThis.__menuHit = i; };"
                          "globalThis.__a0 = a0;"
                          "(typeof a0.text) + '|' + a0.text() + '|' + mn.actionCount() + '|' + String(a1.isChecked()) + '|' + mn.isActionChecked(1)")
                      == "function|a|2|true|true");
            evalStr(engine, "globalThis.__a0.onTriggered = function () { globalThis.__a0Hit = 1; }; 'ok'");
            {
                JZJSValuePtr mv = engine.eval("globalThis.__menu");
                JZMenu* menu = engine.fromJSValue<JZMenu*>(mv.get());
                check("JZMenu: JS 侧拿到的是真菜单对象", menu != nullptr);
                if (menu)
                {
                    menu->actionAt(0)->trigger();              // 句柄式回调(onTriggered)
                    menu->actionAt(1)->setEnabled(true);      // disabled 的 trigger 会被 JZAction 忽略
                    menu->actionAt(1)->trigger();              // 下标式回调(onActionTriggered)
                    jzApp->processEvents();
                }
                check("JZMenu: 句柄上的 onTriggered 生效",
                      evalStr(engine, "String(globalThis.__a0Hit)") == "1");
                check("JZMenu: 下标式 onActionTriggered 收到下标 1",
                      evalStr(engine, "String(globalThis.__menuHit)") == "1");
            }

            // 视图是**值级**的, item 类更没有暴露给 JS(用户明确要求):
            check("dataview: 不暴露 item 类, 单元格类型受限",
                  evalStr(engine,
                          "var t1 = (typeof JZTableWidgetItem === 'undefined') ? 'noTableItem' : 'hasTableItem';"
                          "var t2 = (typeof JZListWidgetItem === 'undefined') ? 'noListItem' : 'hasListItem';"
                          "var bad = 'ok';"
                          "try { dv9.push([{ k: 1 }]); } catch (e) { bad = 'throw'; }"
                          "t1 + '/' + t2 + '/' + bad")
                      == "noTableItem/noListItem/throw");

            check("继承绑定类探针 => " + evalStr(engine,
                      "(function () {"
                      "  try {"
                      "    class D8 extends JZDialog { constructor() { super(); } }"
                      "    var o = new D8();"
                      "    return 'ok(D=' + (o instanceof D8) + ',JZ=' + (o instanceof JZDialog) + ',accept=' + (typeof o.accept) + ')';"
                      "  } catch (e) { return 'throw: ' + e.message; }"
                      "})()"),
                  true);
        }
    }

    // 15) 右键菜单(contextMenuPolicy + onCustomContextMenuRequested): 与 Qt 同名同语义 ——
    //     策略挂在控件上; 右键**释放**时沿父链向上找第一个"管这件事"的控件, 给它自己的局部坐标。
    jzDebug() << "===== 右键菜单(策略 / 父链递交 / 坐标换算)演示 =====";
    {
        JZJSEngine engine;
        engine.init();
        JZJSWidgetModule widgetModule;
        engine.setLibraryDir({ std::string(JZ_JS_WRAPPER_DIR) });   // 控件类层(widget/dist/index.js)靠它定位
        widgetModule.initEngine(&engine);

        check("右键菜单: 策略常量挂在 JZWidget 构造器上",
              evalStr(engine,
                      "(typeof JZWidget === 'function'"
                      " && JZWidget.NoContextMenu === 0 && JZWidget.PreventContextMenu === 1"
                      " && JZWidget.DefaultContextMenu === 2 && JZWidget.ActionsContextMenu === 3"
                      " && JZWidget.CustomContextMenu === 4) ? 'ok' : 'bad'")
                  == "ok");

        check("右键菜单: CustomContextMenu 回调收到本控件局部坐标",
              evalStr(engine,
                      "var cw = new JZWidget();"
                      "globalThis.__ctx = '';"
                      "cw.setContextMenuPolicy(JZWidget.CustomContextMenu);"
                      "cw.onCustomContextMenuRequested = function (x, y) { globalThis.__ctx = x + ',' + y; };"
                      "cw.requestContextMenu(7, 9);"
                      "globalThis.__ctx + '|' + cw.contextMenuPolicy()")
                  == "7,9|4");

        check("右键菜单: 默认策略沿父链递交(坐标换算) + PreventContextMenu 就地吞掉",
              evalStr(engine,
                      "var pp = new JZWidget(); pp.setGeometry(0, 0, 200, 100);"
                      "var ch = new JZWidget(); ch.setGeometry(10, 20, 50, 30); ch.setParentWidget(pp);"
                      "globalThis.__up = '';"
                      "pp.setContextMenuPolicy(JZWidget.CustomContextMenu);"
                      "pp.onCustomContextMenuRequested = function (x, y) { globalThis.__up = x + ',' + y; };"
                      "ch.requestContextMenu(5, 6);"                 // 子的局部 -> 父里是 15,26
                      "var s = globalThis.__up;"
                      "ch.setContextMenuPolicy(JZWidget.PreventContextMenu);"
                      "globalThis.__up = '';"
                      "ch.requestContextMenu(1, 2);"                 // 被吞掉: 父不再收到
                      "s + '|' + globalThis.__up + '|' + String(new JZWidget().contextMenu() === null)")
                  == "15,26||true");

        check("右键菜单: ActionsContextMenu 借用菜单(setContextMenu 往返; 无窗口则不弹)",
              evalStr(engine,
                      "var aw = new JZWidget();"
                      "var cm = new JZMenu(); cm.addAction('copy');"
                      "aw.setContextMenu(cm);"
                      "aw.setContextMenuPolicy(JZWidget.ActionsContextMenu);"
                      "aw.requestContextMenu(0, 0);"                 // 没有所属窗口 -> 不弹(不阻塞)
                      "var got = aw.contextMenu();"
                      "String(got === cm) + '|' + got.actionCount()")
                  == "true|1");

        // 上面都是从 JS 直接调 requestContextMenu; 这里补"真鼠标事件"那条线: WM_RBUTTONUP 分派
        // 到控件后才会走到上下文菜单流程(见 JZWidget::event)。
        JZWidget host;
        host.setContextMenuPolicy(JZWidget::CustomContextMenu);
        int gotX = -1, gotY = -1;
        connect(&host.sigCustomContextMenuRequested, [&](int x, int y) { gotX = x; gotY = y; });
        JZMouseEvent rightUp(JZMouseEvent::Release, JZMouseEvent::RightButton, JZPoint(0, 0), JZPoint(3, 4));
        jzApp->sendEvent(&host, &rightUp);
        check("右键菜单: 鼠标右键 Release 触发(JZWidget::event 那条线)", gotX == 3 && gotY == 4);

        gotX = -1;
        JZMouseEvent leftUp(JZMouseEvent::Release, JZMouseEvent::LeftButton, JZPoint(0, 0), JZPoint(3, 4));
        jzApp->sendEvent(&host, &leftUp);
        check("右键菜单: 左键 Release 不触发", gotX == -1);
    }

    jzDebug() << (g_fail == 0 ? "[ALL PASS]" : "[HAS FAIL]");
    return g_fail == 0 ? 0 : 1;
}
