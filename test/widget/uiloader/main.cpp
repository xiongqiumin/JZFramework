// ---------------------------------------------------------------------------
// test_uiloader —— C++ 侧 UI 加载器(JZUiLoader)的冒烟用例。
//
// 和 JS 侧用的是**同一份方言**: JS 走 JZJSUiLoader::createUi(生成脚本 -> eval),
// C++ 走 JZUiLoader::build(直接 new 真控件)。这里把同一段 ui xml 用 C++ 后端建出来,
// 校验几个关键点(控件类型/名字表/ComboBox 的 <item>/GridLayout 的行列/菜单栏), 然后直接返回 ——
// 不开事件循环, 所以能进 ctest; 想肉眼看界面加 --show。
// ---------------------------------------------------------------------------
#include "JZApplication.h"
#include "JZUiLoader.h"

#include "JZMainWindow.h"
#include "JZWidget.h"
#include "JZLabel.h"
#include "JZPushButton.h"
#include "JZLineEdit.h"
#include "JZComboBox.h"
#include "JZGroupBox.h"
#include "JZLayout.h"
#include "JZMenuBar.h"
#include "JZDateEdit.h"
#include "JZDateTime.h"
#include "JZSpinBox.h"
#include "JZDoubleSpinBox.h"
#include "JZTreeWidget.h"

#include <stdio.h>
#include <string>

// 方言与 test/jsengine/data 下的 .ui 一致: 顶层 MainWindow + menubar + 中央控件 + 布局 + 网格
static const char* kXml =
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
"<ui version=\"4.0\">\n"
"  <class>DemoUi</class>\n"
"  <widget class=\"MainWindow\" name=\"main\">\n"
"    <property name=\"geometry\">0,0,420,300</property>\n"
"    <property name=\"title\">JZUiLoader 冒烟</property>\n"
"    <menubar id=\"menuBar\">\n"
"      <menu title=\"文件\">\n"
"        <action text=\"退出\"/>\n"
"      </menu>\n"
"      <menu title=\"翻译\">\n"
"        <action text=\"中文\" checkable=\"true\" checked=\"true\"/>\n"
"        <action text=\"英语\" checkable=\"true\"/>\n"
"      </menu>\n"
"    </menubar>\n"
"    <widget class=\"Widget\" name=\"centralWidget\">\n"
"      <widget class=\"VBoxLayout\" name=\"verticalLayout\">\n"
"        <widget class=\"Label\" name=\"tip\">\n"
"          <property name=\"text\">C++ uiLoader 建出来的界面</property>\n"
"          <property name=\"styleSheet\">color:#334455; font-weight:bold;</property>\n"
"        </widget>\n"
"        <widget class=\"GroupBox\" name=\"box\">\n"
"          <property name=\"title\">设备</property>\n"
"          <widget class=\"GridLayout\" name=\"grid\">\n"
"            <property name=\"leftMargin\">4</property>\n"
"            <property name=\"rightMargin\">4</property>\n"
"            <property name=\"spacing\">6</property>\n"
"            <widget class=\"Label\" name=\"l1\" row=\"0\" column=\"0\">\n"
"              <property name=\"text\">设备号</property>\n"
"            </widget>\n"
"            <widget class=\"ComboBox\" name=\"cbDevice\" row=\"0\" column=\"1\">\n"
"              <item>127.0.0.1</item>\n"
"              <item>127.0.0.2</item>\n"
"            </widget>\n"
"            <widget class=\"Label\" name=\"l2\" row=\"1\" column=\"0\">\n"
"              <property name=\"text\">单元号</property>\n"
"            </widget>\n"
"            <widget class=\"LineEdit\" name=\"edUnit\" row=\"1\" column=\"1\"/>\n"
"            <widget class=\"Label\" name=\"l3\" row=\"2\" column=\"0\">\n"
"              <property name=\"text\">日期</property>\n"
"            </widget>\n"
"            <widget class=\"DateEdit\" name=\"edDate\" row=\"2\" column=\"1\">\n"
"              <property name=\"dateMsecs\">1791115200000</property>\n"
"              <property name=\"displayFormat\">yyyy-MM-dd</property>\n"
"              <property name=\"calendarPopup\">true</property>\n"
"            </widget>\n"
"            <widget class=\"Label\" name=\"l4\" row=\"3\" column=\"0\">\n"
"              <property name=\"text\">阈值</property>\n"
"            </widget>\n"
"            <widget class=\"DoubleSpinBox\" name=\"edThreshold\" row=\"3\" column=\"1\">\n"
"              <property name=\"minimum\">0.1</property>\n"
"              <property name=\"maximum\">9.9</property>\n"
"              <property name=\"value\">1.5</property>\n"
"              <property name=\"singleStep\">0.5</property>\n"
"              <property name=\"decimals\">2</property>\n"
"              <property name=\"suffix\"> mm</property>\n"
"            </widget>\n"
"          </widget>\n"
"        </widget>\n"
"        <widget class=\"PushButton\" name=\"btnOk\">\n"
"          <property name=\"text\">查询</property>\n"
"        </widget>\n"
"        <widget class=\"SpinBox\" name=\"spCount\">\n"
"          <property name=\"minimum\">2</property>\n"
"          <property name=\"maximum\">8</property>\n"
"          <property name=\"value\">5</property>\n"
"          <property name=\"suffix\"> 个</property>\n"
"        </widget>\n"
"        <widget class=\"TreeWidget\" name=\"treeDev\"/>\n"
"      </widget>\n"
"    </widget>\n"
"  </widget>\n"
"</ui>\n";

static int g_fail = 0;

static void check(const char* what, bool ok)
{
    printf("[%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok)
        ++g_fail;
}

int app_main(int argc, char* argv[])
{
    JZApplication app;

    bool show = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--show")
            show = true;

    JZUiLoader loader;
    if (!loader.load(kXml))
    {
        printf("[FAIL] 解析失败: %s\n", loader.error().c_str());
        return 1;
    }

    JZUiLoader::Result r;
    if (!loader.build(r))
    {
        printf("[FAIL] 建控件失败: %s\n", loader.error().c_str());
        return 1;
    }

    // ---- 与 JS 的 { root, <id>: widget } 同形: 按名字取控件 ----
    JZMainWindow* mw = dynamic_cast<JZMainWindow*>(r.root);
    check("root 是 JZMainWindow", mw != nullptr);
    check("byId 有 tip/box/btnOk/cbDevice/edUnit",
          r.byId.count("tip") && r.byId.count("box") && r.byId.count("btnOk")
          && r.byId.count("cbDevice") && r.byId.count("edUnit"));

    JZLabel* tip = dynamic_cast<JZLabel*>(r.byId["tip"]);
    check("tip 是 JZLabel 且文本对", tip && tip->text() == "C++ uiLoader 建出来的界面");
    // .ui 里写的是裸声明(Qt 的 setStyleSheet("color:red") 写法); 引擎只吃"选择器+声明"的规则,
    // 所以由 JZUiParser::styleSheetText 包成 "类型#名字 { ... }" —— 语义同 Qt 的控件级表。
    check("tip 的 styleSheet 裸声明被包成规则落到控件上",
          tip && tip->styleSheet() == "JZLabel#tip { color:#334455; font-weight:bold; }");

    JZPushButton* btn = dynamic_cast<JZPushButton*>(r.byId["btnOk"]);
    check("btnOk 是 JZPushButton 且文本对", btn && btn->text() == "查询");

    JZLineEdit* ed = dynamic_cast<JZLineEdit*>(r.byId["edUnit"]);
    check("edUnit 是 JZLineEdit", ed != nullptr);

    JZComboBox* cb = dynamic_cast<JZComboBox*>(r.byId["cbDevice"]);
    check("cbDevice 是 JZComboBox 且有 2 个 <item>", cb && cb->count() == 2);
    check("cbDevice 第一项是 127.0.0.1", cb && cb->itemText(0) == "127.0.0.1");

    // 网格: 2 列 4 行(位置来自 row/column 属性)
    JZGroupBox* box = dynamic_cast<JZGroupBox*>(r.byId["box"]);
    JZGridLayout* grid = box ? dynamic_cast<JZGridLayout*>(box->layout()) : nullptr;
    check("box 里的 grid 是 JZGridLayout", grid != nullptr);
    check("grid 有 8 个子控件 / 4 行 / 2 列",
          grid && grid->count() == 8 && grid->rowCount() == 4 && grid->columnCount() == 2);
    check("grid spacing=6(属性落到了真布局上)", grid && grid->spacing() == 6);

    // 小数步进框: 范围/值/步长/精度/后缀 全部落到控件上 + 小数信号
    JZDoubleSpinBox* edTh = dynamic_cast<JZDoubleSpinBox*>(r.byId["edThreshold"]);
    check("edThreshold 是 JZDoubleSpinBox", edTh != nullptr);
    if (edTh)
    {
        check("edThreshold 范围是小数(0.1..9.9)",
              edTh->minimum() == 0.1 && edTh->maximum() == 9.9);
        check("edThreshold value=1.5 / singleStep=0.5 / decimals=2",
              edTh->value() == 1.5 && edTh->singleStep() == 0.5 && edTh->decimals() == 2);
        // 注意: 方言解析会把属性文本两端空白 trim 掉(xmlPropToJson), 所以 .ui 里写 "<property name="suffix"> mm</property>"
        // 落到控件上是 "mm" —— 需要前导空格的后缀只能从代码里 setSuffix(" mm")。
        check("edThreshold suffix=\"mm\"(方言 trim 掉前导空格)", edTh->suffix() == "mm");

        // 关键: 值信号必须是**小数**那个(double), 不能被基类的 int 版本截掉
        double got = 0.0;
        connect(&edTh->sigValueChanged, [&got](double v) { got = v; });
        edTh->setValue(2.25);
        check("edThreshold 改值发的是 double 信号(2.25)",
              got == 2.25 && edTh->value() == 2.25);

        // 内部步进也走小数(鼠标点按钮/滚轮/上下键都是这条路)
        edTh->setSingleStep(0.25);
        edTh->setValue(edTh->value() + edTh->singleStep());
        check("edThreshold 小数步进(2.25+0.25=2.5, 没被截成整数)", edTh->value() == 2.5);

        // 夹取用小数比较
        edTh->setValue(100.0);
        check("edThreshold 超上限被夹到 9.9", edTh->value() == 9.9);
    }

    // 整数步进框: 老口径没被这次重构改坏(minimum/maximum 照旧生效)
    JZSpinBox* sp = dynamic_cast<JZSpinBox*>(r.byId["spCount"]);
    check("spCount 是 JZSpinBox", sp != nullptr);
    if (sp)
    {
        check("spCount minimum=2 / maximum=8(属性照旧)", sp->minimum() == 2 && sp->maximum() == 8);
        check("spCount value=5 / suffix=\"个\"(新落的属性; 前导空格被方言 trim)",
              sp->value() == 5 && sp->suffix() == "个");
        int gotInt = -1;
        connect(&sp->sigValueChanged, [&gotInt](int v) { gotInt = v; });
        sp->setValue(6);
        check("spCount 改值发 int 信号(6)", gotInt == 6 && sp->value() == 6);
        sp->setValue(99);
        check("spCount 超上限夹到 8", sp->value() == 8);
    }

    // 树控件: 方言新入表(JS 侧尚未绑定, 所以这里只验 C++ 能建出来)
    check("treeDev 是 JZTreeWidget", dynamic_cast<JZTreeWidget*>(r.byId["treeDev"]) != nullptr);

    // 日期框: 方言属性(dateMsecs/displayFormat/calendarPopup) + 日期往返
    JZDateEdit* edDate = dynamic_cast<JZDateEdit*>(r.byId["edDate"]);
    check("edDate 是 JZDateEdit", edDate != nullptr);
    if (edDate)
    {
        check("edDate 从 dateMsecs 属性拿到了日期(2026-10-04)",
              edDate->date().year() == 2026 && edDate->date().month() == 10 && edDate->date().day() == 4);
        check("edDate displayFormat 落到控件上", edDate->displayFormat() == "yyyy-MM-dd");
        check("edDate calendarPopup 落到控件上(只记状态, 见 .h 的 TODO)", edDate->calendarPopup());
        check("edDate 文本按格式渲染 = 2026-10-04", edDate->text() == "2026-10-04");

        edDate->setDate(JZDateTime(2026, 10, 4));
        const JZDateTime d = edDate->date();
        check("edDate 日期往返(year/month/day)",
              d.year() == 2026 && d.month() == 10 && d.day() == 4);
    }

    // 菜单栏: <menubar> 下两个菜单
    check("MainWindow 有菜单栏且 2 个菜单",
          mw && mw->menuBar() && mw->menuBar()->menuCount() == 2);

    printf("[uiloader] 控件总数(byId)=%d, 失败项=%d\n", (int)r.byId.size(), g_fail);

    if (show && r.root)
    {
        r.root->show();
        app.exec();
    }

    delete r.root;
    return g_fail == 0 ? 0 : 1;
}

int main(int argc, char* argv[])
{
    return app_main(argc, argv);
}
