#include <string>

#include "JZDebug.h"
#include "JZApplication.h"
#include "JZJSEngine.h"
#include "JZJSBind.h"
#include "JZJSUiLoader.h"
#include "JZJSWidgetModule.h"
#include "JZWidget.h"
#include "JZImage.h"
#include "JZJSType.h"
#include "JZJSTableWidget.h"
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
        return "<js-exception>";
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
        JZImage image = JZImage::create(20, 10, JZColor(0, 0, 0, 0));   // 栈对象, 借用给 JS(cppOwner=true)
        JZJSEngine engine;
        engine.init();
        JZJSWidgetModule widgetModule;
        widgetModule.initEngine(&engine);
        engine.setProperty(engine.globalObject(), "img",
                           toJsValueFromPtr(engine.context(), &image, true));

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

    // 8) JZTableWidget(指针版数据模型) + JS 镜像类 JZJSTableWidget 的所有权/失效自检。
    //    镜像类还没在 registWidgets 里注册(与 JZJSListWidget 一样待接线), 故直接 C++ 侧调 ****Js 方法。
    jzDebug() << "===== JZTableWidget 指针版数据模型 + JS 镜像自检 =====";
    {
        JZJSTableWidget* tbl = new JZJSTableWidget();
        tbl->setRowCount(2);
        tbl->setColumnCount(2);

        // 接管(/Transfer/): JS 创建的单元格交给表格, JS 侧那份翻转为"非拥有"
        jsbind::JSRawPointer<JZTableWidgetItem> a1(new JZTableWidgetItem("A1"), false);
        jsbind::JSRawPointer<JZTableWidgetItem> swA1 = tbl->setItemJs(0, 0, a1);
        check("setItemJs 接管: 表格持有 + JS 侧翻转为非拥有",
              tbl->item(0, 0) == a1.get() && a1.cppOwner() && swA1.get() == a1.get());
        check("item 记录行列号", a1.get()->row() == 0 && a1.get()->column() == 0);
        check("表格反查行列", tbl->row(a1.get()) == 0 && tbl->column(a1.get()) == 0);

        // 表格自建单元格(便捷重载) + 借用包装(cppOwner=true = 表格拥有, JS 只持句柄)
        jsbind::JSRawPointer<JZTableWidgetItem> swB2 = tbl->setItemJs(1, 1, std::string("B2"));
        check("setItemJs(文本) 新建单元格, 借用包装可用",
              swB2.get() != nullptr && swB2.get()->text() == "B2" && swB2.cppOwner());

        // 顶掉同格旧单元格: 旧 item 被删除, 其 JS 包装必须先失效(否则悬垂)
        JZTableWidgetItem* oldB2 = tbl->item(1, 1);
        tbl->setItemJs(1, 1, std::string("B2-x"));
        check("顶掉旧单元格后旧包装失效",
              swB2.isCleared() && tbl->item(1, 1) != oldB2 && tbl->itemText(1, 1) == "B2-x");

        // 摘除并删除: JS 侧包装失效
        tbl->removeItemJs(0, 0);
        check("removeItemJs 后 JS 侧包装失效", swA1.isCleared() && a1.isCleared() && tbl->item(0, 0) == nullptr);

        // 摘除并交回所有权(/TransferBack/): cppOwner=false, 包装析构时释放(等价 JS GC)
        JZTableWidgetItem* rawB = tbl->item(1, 1);
        jsbind::JSRawPointer<JZTableWidgetItem> taken = tbl->takeItemJs(1, 1);
        check("takeItemJs 交回所有权", taken.get() == rawB && !taken.cppOwner() && tbl->item(1, 1) == nullptr);
        taken = jsbind::JSRawPointer<JZTableWidgetItem>();   // 放掉 -> 该 item 被释放(等价 JS GC)

        // 插删行列: item 自身记录的行列号跟随
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

        // 整行删除: 该行 JS 包装失效
        jsbind::JSRawPointer<JZTableWidgetItem> swC1 = tbl->itemJs(2, 0);
        tbl->removeRowJs(2);
        check("removeRowJs 使该行 JS 包装失效", swC1.isCleared() && tbl->rowCount() == 2);

        // 整表清空: 全部包装失效
        tbl->setItemJs(1, 0, std::string("D1"));
        jsbind::JSRawPointer<JZTableWidgetItem> swD = tbl->itemJs(1, 0);
        tbl->clearContentsJs();
        check("clearContentsJs 后全部失效且清空",
              swD.isCleared() && tbl->item(1, 0) == nullptr && tbl->rowCount() == 2);

        delete tbl;
    }

    // 9) JZChart: JS 层暴露(JZJSWidgetModule::registWidgets 里 ClassBind 注册)。
    //    断言用字符串比较而不是 bool: eval 抛异常时 fromJSValue<bool> 仍可能为真(对象真值), 会假通过。
    jzDebug() << "===== JZChart JS 绑定演示 =====";
    {
        JZJSEngine engine;
        engine.init();
        JZJSWidgetModule widgetModule;
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
        engine.init();
        JZJSWidgetModule widgetModule;
        widgetModule.initEngine(&engine);

        // 包装的所有权在谁手上: 1 = C++ 拥有(JS 不删, /Transfer/), 0 = JS 拥有, -1 = 不是 jsbind 包装。
        // /Transfer/ /TransferBack/ 翻转的就是它; PointInfo 是共享的 ⇒ 从 JS 那份包装上读到的就是真实状态。
        auto cppOwnerOf = [](JSValueConst v) {
            JSClassID id = 0;
            auto* p = (jsbind::JSPointerBase*)JS_GetAnyOpaque(v, &id);
            return p ? (p->cppOwner() ? 1 : 0) : -1;
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

        // (a) addWidget: 所有权转移(/Transfer/) + 真的挂成子控件(父链 / 对象树 / 布局项三处一致)
        check("addWidget 前包装由 JS 拥有", cppOwnerOf(cVal.get()) == 0);
        evalStr(engine, "p1.addWidget(c); 'ok'");
        check("addWidget 后翻成 C++ 拥有(/Transfer/)", cppOwnerOf(cVal.get()) == 1);
        check("addWidget 后成为父控件的子控件(树+布局项)",
              c->parentWidget() == p1 && hasChild(p1, c) && !hasChild(p2, c)
              && p1->layout() != nullptr && static_cast<JZBoxLayout*>(p1->layout())->count() == 1);

        // (b) setParentWidget 换父: 同一个控件只属于一个父(旧父不再持有)
        check("setParentWidget 换父后父链指向新父",
              evalStr(engine, "c.setParentWidget(p2); c.parentWidget().objectName()") == "p2");
        check("换父后 C++ 对象树同步(旧父不再持有)",
              c->parentWidget() == p2 && !hasChild(p1, c) && hasChild(p2, c) && cppOwnerOf(cVal.get()) == 1);

        // (c) 脱离父树: 交回 JS 拥有; 之后再挂回又能进树
        check("setParentWidget(null) 脱离父树",
              evalStr(engine, "c.setParentWidget(null); (c.parentWidget() === null) ? 'null' : 'obj'") == "null");
        check("脱离后包装翻回 JS 拥有(/TransferBack/)",
              cppOwnerOf(cVal.get()) == 0 && c->parentWidget() == nullptr && !hasChild(p2, c));
        check("重新挂回父树后所有权与父链恢复",
              evalStr(engine, "p2.addWidget(c); (c.parentWidget().objectName() === 'p2') ? 'ok' : 'bad'") == "ok"
              && cppOwnerOf(cVal.get()) == 1 && hasChild(p2, c));

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
                      "rep + '/' + nul + '/' + kept") == "rejected/rejected/vbox");

        check("clearLayout 后 layout() 为 null、旧视图失效、可再设新布局",
              evalStr(engine,
                      "var view = w.layout();"       // 视图(缓存那份): clearLayout 后会被置失效
                      "w.clearLayout();"
                      "var cleared = (w.layout() === null) ? 'null' : 'obj';"
                      "var dead = 'usable';"
                      "try { view.addSpacing(1); } catch (e) { dead = 'released'; }"
                      "w.setLayout(new JZHBoxLayout());"
                      "cleared + '/' + dead + '/' + ((w.layout() instanceof JZHBoxLayout) ? 'hbox' : 'bad')")
              == "null/released/hbox");

        check("layout() 返回同一个包装(缓存, 引用相等)",
              evalStr(engine, "(w.layout() === w.layout()) ? 'same' : 'diff'") == "same");
        {
            JZJSValuePtr wVal = engine.eval("w");
            JZWidget* w = engine.fromJSValue<JZWidget*>(wVal.get());
            check("C++ 侧布局已是 HBox", dynamic_cast<JZHBoxLayout*>(w->layout()) != nullptr);

            JZJSValuePtr view = engine.eval("w.layout()");
            check("布局包装是借用视图(JS 不拥有)", cppOwnerOf(view.get()) == 1);
        }

        // 窗口默认不带布局(否则 JS 里 `new JZWindow(...); win.setLayout(v)` 就成了重复设置):
        // 想要默认纵向布局直接 addWidget —— JZWindow.addWidget 会按需补一个 VBox。
        check("窗口无布局时 addWidget 自动补一个 VBox",
              evalStr(engine,
                      "var nw = new JZWindow(120, 80, 'nw');"
                      "var none = (nw.layout() === null) ? 'null' : 'obj';"
                      "nw.addWidget(new JZPushButton());"
                      "none + '/' + ((nw.layout() instanceof JZVBoxLayout) ? 'vbox' : 'bad')") == "null/vbox");

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
        widgetModule.initEngine(&engine);

        auto hasChild = [](JZObject* parent, JZObject* child) {
            for (JZObject* o : parent->children())
                if (o == child)
                    return true;
            return false;
        };

        // 值类型: 实参真的写进底层对象(以前 createArgs 一律忽略实参)
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
        check("构造器实参: 控件 (text,parent) / (parent,w,h,text) / (title,parent) / (orientation,parent)",
              evalStr(engine,
                      "var hw = new JZWindow(300, 180, 'host');"
                      "var lb = new JZLabel('hello', hw);"
                      "var bt = new JZPushButton(hw, 90, 28, 'OK');"
                      "var rb = new JZRadioButton('r1', hw);"
                      "var gb = new JZGroupBox('grp', hw);"
                      "var sp = new JZSplitter(1, hw);"
                      "var vl = new JZVBoxLayout(3, 7);"
                      "((lb.text() === 'hello' && bt.text() === 'OK' && bt.width() === 90 && bt.height() === 28)"
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
    //     Splitter.addWidget —— 与 addWidget 同一套(所有权转移 + 登记保活)。
    //     验证: 挂进去 → JS 丢掉引用 → 强制 GC → 事件仍能派发到 JS(包装还活着 = 保活生效)。
    jzDebug() << "===== 容器挂载路径(所有权/保活)演示 =====";
    {
        JZJSEngine engine;
        engine.init();
        JZJSWidgetModule widgetModule;
        widgetModule.initEngine(&engine);

        auto cppOwnerOf = [](JSValueConst v) {
            JSClassID id = 0;
            auto* p = (jsbind::JSPointerBase*)JS_GetAnyOpaque(v, &id);
            return p ? (p->cppOwner() ? 1 : 0) : -1;
        };

        // 丢掉 JS 引用 + 强制 GC, 然后往控件上派发一次鼠标按下, 返回 JS 侧计数器
        auto revive = [&engine, &cppOwnerOf](JSValueConst childVal, const char* childName, const char* hitsName) {
            JZWidget* child = engine.fromJSValue<JZWidget*>(childVal);
            bool transferred = (cppOwnerOf(childVal) == 1);
            evalStr(engine, std::string(childName) + " = null; 'ok'");
            JS_RunGC(JS_GetRuntime(engine.context()));

            JZMouseEvent press(JZMouseEvent::Press, JZMouseEvent::LeftButton, JZPoint(3, 3), JZPoint(3, 3));
            jzApp->sendEvent(child, &press);

            std::string hits = evalStr(engine, std::string("String(globalThis.") + hitsName + ")");
            return transferred && hits == "1";
        };

        // ① JZStackedWidget.addWidget(test_jswidget 的"页"就是这条路径)
        evalStr(engine,
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
    }

    jzDebug() << (g_fail == 0 ? "[ALL PASS]" : "[HAS FAIL]");
    return g_fail == 0 ? 0 : 1;
}
