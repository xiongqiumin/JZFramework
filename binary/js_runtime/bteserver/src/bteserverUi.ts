// ---------------------------------------------------------------------------
// bteserverUi —— BTEServer 主窗口的 UI 定义
//
// XML 的来源: `ui/BTEServer.ui` —— 引擎方言的界面 XML(原版 Qt Designer 的 BTEServer.ui 经
// `tools/ui2jz.py` 转出; 该转换器已不在仓库里, 所以**这份 .ui 就是现在的源头**)。
// 运行期由 `loadUiFile()`(见 uiFile.ts)加载这一份, 不再有"内联字符串"副本;
// 改界面就改 `ui/BTEServer.ui`(加载时会按 build/ui -> ../src/ui 顺序找它)。
//
// createUi() 返回的对象**本身是窗口**(MainWindow -> JZMainWindow), 同时通过 `ui.<id>` 暴露全部
// 命名控件 —— 调用方照 Qt 的 setupUi 习惯写 `this.ui.listWidget` 就行。与 uic 的差异:
//   * 不做事件绑定: 原 ui_*.h 自动 connect 的 on_xxx_clicked / on_xxx_triggered 在 JS 侧改为
//     显式接线(见 BTEServer.connectUi 的 wire());
//   * **菜单动作没有 id**: 方言里 `<action>` 只有 text/shortcut/checkable/checked/enabled
//     (Qt 的 `name` 没进 XML) —— 所以拿不到 `ui.actionChn`, 只能按文本在菜单树里找(见 findAction);
//   * 两个自定义控件(SplitSettingWidget / SplitResultWidget)在 XML 里是**同名占位 Widget**,
//     真控件在这里 new 出来塞进占位, 并把 `ui.<id>` 换成那个组合对象(BTEServer 侧要它的信号数组);
//   * retranslateUi 留空实现(XML 里文本已给定)。
// ---------------------------------------------------------------------------


import { SplitSettingWidget } from './SplitSettingWidget.js';
import { SplitResultWidget } from './SplitResultWidget.js';
import { loadUiFile } from './uiFile.js';

// 按文本在菜单树里找一个 action —— 见文件头"菜单动作没有 id"。
function findAction(menuBar: any, text: string): any
{
    if (!menuBar || typeof menuBar.menuCount !== 'function')
        return null;
    for (let i = 0; i < menuBar.menuCount(); i++)
    {
        const m = menuBar.menuAt(i);
        if (!m || typeof m.actionCount !== 'function')
            continue;
        for (let j = 0; j < m.actionCount(); j++)
        {
            const a = m.actionAt(j);
            if (a && typeof a.text === 'function' && a.text() === text)
                return a;
        }
    }
    return null;
}

// 把"组合式"自定义控件(child, 真控件在 child.root)塞进 XML 里那个同名占位 Widget。
function fillPlaceholder(holder: any, child: any): void
{
    if (!holder || !child || !child.root)
        return;
    const lay = new JZVBoxLayout();
    lay.addWidget(child.root, 1);
    holder.setLayout(lay);
}

// 造主窗口: 返回的对象**本身是窗口**, 同时带全部命名子控件(ui.<id>)。
export function createUi(): any
{
    const ui = loadUiFile('BTEServer.ui');
    if (!ui)
        throw new Error("bteserverUi: loadUiFile 返回空(ui/BTEServer.ui 解析或建控件失败, 见引擎日志)");

    // 菜单动作: 引擎已绑定真 JZAction(JZMenu/JZMenuBar 都在), 按文本取回后挂成 ui.<id>。
    ui.actionChn = findAction(ui.menuBar, "中文");
    ui.actionEng = findAction(ui.menuBar, "英语");
    if (!ui.actionChn || !ui.actionEng)
        console.log("[bteserverUi] 菜单里没找到 中文/英语 动作 —— 语言切换会空转");

    // 文件→退出 / 帮助→关于: 同样按文本取回(Qt 的 ui.actionExit / ui.actionAbout 在方言里没有 name)
    // ⚠ 取回来必须在 connectUi() 里接 onTriggered 才有反应 —— 以前没接, "文件→退出"点了没反应。
    ui.actionExit = findAction(ui.menuBar, "退出");
    ui.actionAbout = findAction(ui.menuBar, "关于");
    if (!ui.actionExit)
        console.log("[bteserverUi] 菜单里没找到 退出 动作 —— 文件→退出 会没反应");

    // uic 的 retranslateUi(): XML 路径下文本已给定, 留空实现对齐原调用点
    ui.retranslateUi = function (_w: any) { };

    // ---- 分选页的两个自定义控件: XML 里留了同名占位 Widget, 这里 new 真控件塞进去 ----
    const holderSetting = ui.splitSettingWidget;   // XML 里的占位(Widget)
    const holderResult = ui.splitResult;
    const splitSetting = new SplitSettingWidget(holderSetting);
    const splitResult = new SplitResultWidget(holderResult);
    fillPlaceholder(holderSetting, splitSetting);
    fillPlaceholder(holderResult, splitResult);
    // 覆盖成"组合对象": BTEServer 侧用的是它的 sigPrepareData / sigSplitFinish 数组
    ui.splitSettingWidget = splitSetting;
    ui.splitResult = splitResult;

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
