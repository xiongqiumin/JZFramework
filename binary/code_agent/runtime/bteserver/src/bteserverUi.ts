// ---------------------------------------------------------------------------
// bteserverUi —— BTEServer 主窗口的**真** UI 定义
//                (替换原先由 qtcompat 顶着的 Ui_BTEServerClass 替身)
//
// 形态对齐 Qt 的 uic: 一份 UI 定义 -> 一个"带命名子控件"的窗口对象。
// 这里用引擎自带的 uiLoader: 输入 Qt .ui 风格 XML, 运行期直接造**真控件树**;
// 返回值"既是 JZWindow, 又能通过 ui.<id> 访问子控件"(见 JZJSWidgetModule::uiLoaderResultToWindow)
// —— 所以调用方照 Qt 的 setupUi 习惯写 this.ui.listWidget 就行。
//
// 与 Qt 原版的差异(逐条都标了原因):
//   * 控件类名 = 真 JZ 类去掉 `JZ` 前缀: ListWidget / TableWidget / TabWidget / TextEdit / ...
//     (已知类型清单见 JZJSUiLoader::knownTypes());
//   * 只定义**结构与属性**, 不做事件绑定 —— 原 C++ 由 ui_*.h 自动 connect 的 on_xxx_clicked /
//     on_xxx_triggered 在 JS 侧改为显式 `ui.btnX.push((checked) => this.on_btnX_clicked())`;
//   * `actionChn` / `actionEng` 是菜单动作, 引擎还没有 JZAction 的绑定, 先用最小替身(见 makeAction);
//   * `retranslateUi` 是 uic 生成的成员方法, XML 路径下不需要, 这里留个空实现对齐原调用点;
//   * 仓库里没有原 .ui 文件, 尺寸/文本按 C++ 里的用法反推, 目标是"结构对 + 能跑起来", 细节后续再调。
// ---------------------------------------------------------------------------

declare const uiLoader: any;

import { SplitSettingWidget } from './SplitSettingWidget.js';
import { SplitResultWidget } from './SplitResultWidget.js';

// UI XML —— name 即 `this.ui.<name>`; 类名不带 JZ 前缀(loader 规则)。
const BTE_UI_XML = `
<ui>
  <class>Ui_BTEServerClass</class>
  <widget class="Window" name="main">
    <property name="title">BTEServer</property>
    <property name="geometry">80,60,1280,800</property>

    <!-- 顶部按钮条 -->
    <widget class="PushButton" name="btnShowNormal"><property name="text">实时</property></widget>
    <widget class="PushButton" name="btnShowList"><property name="text">工步</property></widget>
    <widget class="PushButton" name="btnShowTest"><property name="text">测试</property></widget>
    <widget class="PushButton" name="btnShowSplitSetting"><property name="text">分选设置</property></widget>
    <widget class="PushButton" name="btnSplitQuery"><property name="text">分选查询</property></widget>
    <widget class="PushButton" name="btnClearAlarm"><property name="text">清除报警</property></widget>
    <widget class="PushButton" name="btnNear"><property name="text">近端</property></widget>
    <widget class="PushButton" name="btnNone"><property name="text">取消</property></widget>

    <!-- 服务 / 收发(原先靠 uic 命名约定自动连槽, 代码里不显式引用 ⇒ 之前漏掉的就是这批;
         文案按槽的实现推断 —— 原 .ui 不在仓库, 以之为准要再核) -->
    <widget class="PushButton" name="btnResetServer"><property name="text">重置服务</property></widget>
    <widget class="PushButton" name="btnClearRecv"><property name="text">清空接收</property></widget>
    <widget class="PushButton" name="btnClearSend"><property name="text">清空发送</property></widget>

    <!-- 报文显示开关 -->
    <widget class="CheckBox" name="boxShowRT"><property name="text">实时帧</property></widget>
    <widget class="CheckBox" name="boxShowDF"><property name="text">数据帧</property></widget>
    <widget class="CheckBox" name="boxRecordPack"><property name="text">记录报文</property></widget>

    <!-- 页容器: 实时 / 工步 / 测试 / 分选 / 历史 -->
    <widget class="StackedWidget" name="stackedWidget">
      <widget class="Widget" name="pageNormal"/>
      <widget class="Widget" name="pageList"/>
      <widget class="Widget" name="pageTest"/>
      <widget class="Widget" name="pageSplit"/>
      <widget class="Widget" name="pageHistory"/>
    </widget>

    <!-- 设备与通道卡片 -->
    <widget class="ComboBox" name="boxDevice"/>
    <widget class="ListWidget" name="listWidget"/>
    <widget class="Widget" name="itemWidgets"/>

    <!-- 表格 -->
    <widget class="TableWidget" name="tableList"/>
    <widget class="TableWidget" name="tableQuery"/>
    <widget class="TableWidget" name="tableSplitLevel"/>
    <widget class="TableWidget" name="tableSplitList"/>

    <!-- 查询条件 -->
    <widget class="LineEdit" name="lineCellNum"/>
    <widget class="LineEdit" name="lineCellPatch"/>
    <widget class="LineEdit" name="lineCreator"/>
    <widget class="LineEdit" name="lineNearDay"/>
    <widget class="LineEdit" name="lineQueryChannel"/>
    <widget class="LineEdit" name="lineQueryUnit"/>
    <widget class="LineEdit" name="dateFrom"/>
    <widget class="LineEdit" name="dateTo"/>

    <!-- 查询 / 删除(历史页) -->
    <widget class="PushButton" name="btnQuery"><property name="text">查询</property></widget>
    <widget class="PushButton" name="btnExportAll"><property name="text">导出全部</property></widget>
    <widget class="PushButton" name="btnDeleteSelect"><property name="text">删除选中</property></widget>
    <widget class="PushButton" name="btnDeleteWeek"><property name="text">删除一周前</property></widget>
    <widget class="PushButton" name="btnDeleteMonth"><property name="text">删除一月前</property></widget>
    <widget class="PushButton" name="btnDeleteYear"><property name="text">删除一年前</property></widget>
    <widget class="PushButton" name="btnDeleteDevice"><property name="text">删除设备记录</property></widget>

    <!-- 分选 -->
    <widget class="PushButton" name="btnSaveSplit"><property name="text">保存分选</property></widget>
    <widget class="PushButton" name="btnCopySplitList"><property name="text">复制列表</property></widget>

    <!-- 分选: 饼图是真 JZChart; 另外两个自定义控件(SplitSettingWidget/SplitResultWidget)
         不写进 XML —— 它们现在是真控件类, 由 createUi() 里 new 出来挂到分选页上。 -->
    <widget class="Chart" name="splitPie"/>

    <!-- 日志 / 收发 -->
    <widget class="TextEdit" name="textLog"/>
    <widget class="TextEdit" name="textSend"/>
    <widget class="TextEdit" name="textRecv"/>

    <!-- 标签页(实时页里再分"数据/记录"等) -->
    <widget class="TabWidget" name="tabWidget"/>
    <widget class="TabWidget" name="tabWidgetSplit"/>
  </widget>
</ui>
`;

// 菜单动作的最小替身(Qt 的 QAction): 引擎还没绑定 JZAction。
// 只实现 BTEServer 用到的那几个口子: text / setChecked / isChecked / setCheckable / triggered。
function makeAction(text: string, checked: boolean): any
{
    // 同一个数组挂两个名字: Qt 写法 triggered / 本移植约定 sigTriggered —— 哪边 push 都能被触发
    const trig: any[] = [];
    const a: any = {
        _text: text,
        _checked: !!checked,
        _checkable: true,
        triggered: trig,                  // Qt 的 QAction::triggered(bool)
        sigTriggered: trig,               // 本移植的信号约定(sigX.push(cb))
        setText(v: any) { a._text = String(v); },
        text() { return a._text; },
        setCheckable(v: any) { a._checkable = !!v; },
        isCheckable() { return a._checkable; },
        setChecked(v: any)
        {
            a._checked = !!v;
            a.triggered.forEach((f: any) => f(a._checked));
        },
        isChecked() { return a._checked; },
        trigger() { a.setChecked(!a._checked); },
    };
    return a;
}

// 造主窗口: 返回的对象**本身是 JZWindow**, 同时带全部命名子控件(ui.<id>)。
export function createUi(): any
{
    const ui = uiLoader.load(BTE_UI_XML);
    if (!ui)
        throw new Error("bteserverUi: uiLoader.load 返回空(UI XML 解析或建控件失败, 见引擎日志)");

    // 菜单动作(不是控件, 不能写进 XML)
    ui.actionChn = makeAction("中文", true);
    ui.actionEng = makeAction("English", false);

    // uic 的 retranslateUi(): XML 路径下文本已给定, 留空实现对齐原调用点
    ui.retranslateUi = function (_w: any) { };

    // ---- 分选页的两个自定义控件: 真控件类, 实例化后挂到分选页上 ----
    const splitSetting = new SplitSettingWidget(ui.pageSplit);
    const splitResult = new SplitResultWidget(ui.pageSplit);
    ui.splitSettingWidget = splitSetting;
    ui.splitResult = splitResult;

    // 自定义控件是"组合"(持真控件 root) —— 布局/显示都走各自的 root
    const splitLay = new JZVBoxLayout();
    splitLay.addWidget(splitResult.root, 0);
    splitLay.addWidget(splitSetting.root, 1);
    ui.pageSplit.setLayout(splitLay);

    // ---- 分选饼图: ui.splitPie 就是真 JZChart(引擎原生支持饼图) ----
    // 原来那个包第三方 Nightcharts 的 SplitPieChart 类已删除(全仓没人 import 它);
    // 它对外只有两个方法, 这里按真图表 API 重写(调用点见 BTEServer.clearSplit/onSplitFinish)。
    const pie = ui.splitPie;
    pie.setChartType((JZChart as any).PieChart);   // 类型常量挂在构造器上(与 C++ 枚举同值)
    pie.setShowLegend(true);

    // void clear();
    pie.clear = function ()
    {
        pie.clearSeries();
        pie.setCategories([]);
        pie.update();
    };

    // void setResult(SplitResult result);
    // 原实现: 把 indexs[i].length / sumItem * 100 当成百分比喂给 Nightcharts ——
    // 这里直接喂**各档位的数量**(图表自己算比例/百分比), 扇区名走 categories。
    pie.setResult = function (result)
    {
        pie.clearSeries();

        const names = [];
        const values = [];
        for (let i = 0; i < result.indexs.length; i++)
        {
            if (result.indexs[i].length !== 0)
            {
                names.push((i < 15) ? String.fromCharCode('A'.charCodeAt(0) + i) : "落选");
                values.push(result.indexs[i].length);
            }
        }

        if (values.length > 0)
            pie.setData([values]);
        pie.setCategories(names);
        pie.update();
    };

    return ui;
}
