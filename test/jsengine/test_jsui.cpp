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

    jzDebug() << (g_fail == 0 ? "[ALL PASS]" : "[HAS FAIL]");
    return g_fail == 0 ? 0 : 1;
}
