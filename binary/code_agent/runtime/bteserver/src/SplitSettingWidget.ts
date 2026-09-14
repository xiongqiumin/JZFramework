// ---------------------------------------------------------------------------
// SplitSettingWidget.js —— 对应 C++ BTEServer/SplitSettingWidget.h + SplitSettingWidget.cpp
//
// 分选设置页: 15 级(A..O) 放电容量/中值电压区间表 + 等分生成 + 分选模板存取 + 执行分选。
// (SplitResult / SplitResult::RecordInfo 也定义在 C++ 的 SplitSettingWidget.h 里, 一并导出)
//
// Qt -> JS: s << x -> push; 出参 SplitInfo::Range&/RecordInfo& 在 JS 里直接改对象的
//           字段(对象是引用, 天然等价于出参), 函数返回原来的 bool/int。
// ---------------------------------------------------------------------------

import { tr, emit, sprintf } from './BTEServer.js';
import { UiHelper, MAX_SPLIT_LEVEL } from './Common.js';
import { formatDateTime } from './datetime.js';
import { DataTrans } from './DataTrans.js';
import { BTEProject, SplitInfo, g_project } from './BTEProject.js';
// 迁移: 弹窗/文件对话框改用引擎的真实现(JZMessageBox / JZFileDialog, 都是全局);
// 还没绑定的输入对话框(QInputDialog)暂时留着 qtcompat 的替身。
import { QInputDialog, Qt } from './qtcompat.js';

// ---------------------------------------------------------------------------
// struct SplitResult
// ---------------------------------------------------------------------------
export class SplitResult
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // struct RecordInfo { ... };
    static get RecordInfo() { return SplitResult_RecordInfo; }

    constructor()
    {
        this.error = false;
        this.erroNum = 0;
        this.sumItem = 0;

        this.recordList = [];		//每条数据的结果
        this.indexs = [];           //分选档位,索引
    }
}

// struct SplitResult::RecordInfo
class SplitResult_RecordInfo
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor()
    {
        this.itemIndex = 0;
        this.Time = 0;
        this.Level = 0;
        this.DisChargeCap = 0;
        this.DisChargeEng = 0;
        this.MeanVoltage = 0;
        this.CcRatio = 0;        //恒流比(%)
        this.CellNo = "";
        this.CellPatch = "";
    }

    // QString LevelText();
    LevelText()
    {
        // 迁移: 原来走 qApp.translate(); 翻译层未移植, 直接给文本
        const name = (this.Level < 15) ? String.fromCharCode('A'.charCodeAt(0) + this.Level) : "落选";
        return name;
    }

    // QString TimeText();
    TimeText()
    {
        return DataTrans.fromTimeUnit(this.Time);
    }
}

// ---------------------------------------------------------------------------
// class SplitSettingWidget : public QWidget
// ---------------------------------------------------------------------------
// 迁移: 本类是"持有真控件的复合控件"(组合, **不是**继承) —— 内部 6 个控件都是真 JZ 控件。
// 为什么不用 `extends JZWidget`: 引擎注册的控件构造器是 C 函数, 它 `return` 自己的对象,
// 于是 `new SubClass()` 拿到的是**普通 JZWidget**(子类方法全丢, 实测 constructor.name === "JZWidget",
// instanceof 为 false)。所以自定义控件一律用组合: 本类持一个真控件 this.root 当"本体"。
export class SplitSettingWidget
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // SplitSettingWidget(QWidget *parent = Q_NULLPTR);
    constructor(parent)
    {
        // : QWidget(parent) —— 真控件本体(组合): 子控件都挂在它下面, 布局也设在它上面
        this.root = new JZWidget(parent);

        this.recordList = [];   // QVector<BTERecordPtr> recordList; 分选数据
        this.result = new SplitResult();   // SplitResult result; 分选结果
        this.m_type = 0;        // int m_type;  分选类型

        // std::array<SplitInfo::Range, MAX_SPLIT_LEVEL> m_range[2];
        this.m_range = [new Array(MAX_SPLIT_LEVEL), new Array(MAX_SPLIT_LEVEL)];
        for (let i = 0; i < 2; i++)
            for (let j = 0; j < MAX_SPLIT_LEVEL; j++)
                this.m_range[i][j] = new SplitInfo.Range();

        // 信号
        this.sigPrepareData = [];   // signals: void sigPrepareData();
        this.sigSplitFinish = [];   // signals: void sigSplitFinish();

        // ---- 真 UI(原 Ui_SplitSettingWidgetClass + setupUi 的替身) ----
        // 6 个控件直接 new 真 JZ 类, 用真布局排起来, 不再依赖 qtcompat 的 uic 替身。
        const lay = new JZVBoxLayout();

        this.ui = {};
        this.ui.tableSplitSelect = new JZTableWidget(this.root);
        this.ui.tableWidgetSplit = new JZTableWidget(this.root);
        this.ui.boxSplitType = new JZComboBox(this.root);
        this.ui.spinSplitMin = new JZSpinBox(this.root);
        this.ui.spinSplitMax = new JZSpinBox(this.root);
        this.ui.spinSplitNum = new JZSpinBox(this.root);
        // 原 uic 生成的 setupUi(): 这里控件都在上面建好了, 留空实现对齐名字
        this.ui.setupUi = function (_w) { };

        lay.addWidget(this.ui.boxSplitType, 0);
        lay.addWidget(this.ui.spinSplitMin, 0);
        lay.addWidget(this.ui.spinSplitMax, 0);
        lay.addWidget(this.ui.spinSplitNum, 0);
        lay.addWidget(this.ui.tableSplitSelect, 1);
        lay.addWidget(this.ui.tableWidgetSplit, 2);
        this.root.setLayout(lay);

        //tableSplit
        const strListSelect = [];
        strListSelect.push(tr("序号"), tr("名称"));
        UiHelper.SetTable(this.ui.tableSplitSelect, strListSelect);

        //
        const strListSplit = [];
        strListSplit.push(tr("等级"), tr("放电容量"), tr("放电中值电压"));
        this.ui.tableWidgetSplit.setColumnCount(strListSplit.length);
        this.ui.tableWidgetSplit.setHorizontalHeaderLabels(strListSplit);

        // Qt 的 setSelectionMode/setSelectionBehavior/verticalHeader().hide() 在真 JZTableWidget
        // 上没有对应接口(非必须的显示行为) —— 保留调用点说明, 不再调假接口。
        // (原来是: setSelectionMode(SingleSelection) / setSelectionBehavior(SelectRows) / verticalHeader().hide())

        this.ui.tableWidgetSplit.setRowCount(15);
        for (let i = 0; i < 15; i++)
        {
            // 单元格项仍用 qtcompat 的 QTableWidgetItem(它的 role 语义 port 里到处在用);
            // 表格本体已是真 JZTableWidget, 桥的 setItem 会写穿到真单元格。
            const item = new QTableWidgetItem(String.fromCharCode('A'.charCodeAt(0) + i));
            const item1 = new QTableWidgetItem();
            const item2 = new QTableWidgetItem();
            item.setFlags(item.flags() & (~Qt.ItemIsEditable));
            this.ui.tableWidgetSplit.setItem(i, 0, item);
            this.ui.tableWidgetSplit.setItem(i, 1, item1);
            this.ui.tableWidgetSplit.setItem(i, 2, item2);
        }

        this.ui.boxSplitType.addItem(tr("放电容量"));
        this.ui.boxSplitType.addItem(tr("放电中值电压"));
        this.ui.boxSplitType.addItem(tr("放电容量及中值电压"));
    }

    // ~SplitSettingWidget();
    dispose()
    {
    }

    // void init();
    init()
    {
        this.updateSplitTable();
    }

    // std::array<SplitInfo::Range, MAX_SPLIT_LEVEL> range();
    range()
    {
        const idx = this.ui.boxSplitType.currentIndex();
        return this.m_range[idx];
    }

    // void on_btnSplitEqual_clicked();
    on_btnSplitEqual_clicked()
    {
        const min = this.ui.spinSplitMin.value();
        const max = this.ui.spinSplitMax.value();
        const count = this.ui.spinSplitNum.value();
        if (min >= max)
        {
            JZMessageBox.information(this, "", tr("分选条件设置错误"));
            return;
        }

        const split_type = this.ui.boxSplitType.currentIndex();
        const types = [];
        if (split_type === 2)
            types.push(0, 1);
        else
            types.push(split_type);

        for (let type_idx = 0; type_idx < types.length; type_idx++)
        {
            const idx = types[type_idx] + 1;
            for (let i = 0; i < 15; i++)
                this.ui.tableWidgetSplit.item(i, idx).setText("");

            const gap = (max - min) / count;
            let e = max;
            let s = max - gap;
            for (let i = 0; i < count; i++)
            {
                if (i === count - 1)
                    s = min;

                const text = sprintf("%0.2f , %0.2f", s, e);
                this.ui.tableWidgetSplit.item(i, idx).setText(text);

                s -= gap;
                e -= gap;
            }
        }
    }

    // void on_btnSplit_clicked();
    on_btnSplit_clicked()
    {
        if (!this.uiToRange())
            return;

        emit(this.sigPrepareData);   // emit sigPrepareData();

        this.result.error = false;
        this.result.erroNum = -1;
        this.result.indexs = [];          // result.indexs.clear();
        for (let i = 0; i < 16; i++)
            this.result.indexs.push([]);  // result.indexs.resize(16);
        this.result.recordList = [];      // result.recordList.clear();
        this.result.sumItem = this.recordList.length;

        for (let i = 0; i < this.recordList.length; i++)
        {
            const info = new SplitResult.RecordInfo();
            const level = this.splitRecord(this.recordList[i], info);
            if (level === -1)
            {
                this.result.erroNum = i;
                this.result.error = true;
                break;
            }
            this.result.indexs[level].push(i);   // result.indexs[level].push_back(i);
            this.result.recordList.push(info);
        }

        emit(this.sigSplitFinish);   // emit sigSplitFinish();
    }

    // void on_btnSaveSplit_clicked();
    on_btnSaveSplit_clicked()
    {
        if (!this.uiToRange())
            return;

        // TODO(界面层): 引擎还没绑定输入对话框(JZInputDialog) —— 暂时仍用 qtcompat 的替身(返回空串=取消)
        const name = QInputDialog.getText(this, "", tr("请输入名称"));
        if (name === "")
            return;

        const info = new SplitInfo();
        info.name = name;
        info.rangeList[0] = this.m_range[0];
        info.rangeList[1] = this.m_range[1];

        const list = g_project.splitInfoList();
        list.push(info);   // list.push_back(info);
        g_project.setSplitInfoList(list);

        this.updateSplitTable();
    }

    // void on_btnDeleteSplit_clicked();
    on_btnDeleteSplit_clicked()
    {
        const idx = this.ui.tableSplitSelect.currentRow();
        if (idx < 0)
            return;

        const list = g_project.splitInfoList();
        list.splice(idx, 1);   // list.remove(idx);
        g_project.setSplitInfoList(list);

        this.updateSplitTable();
    }

    // void on_tableSplitSelect_itemDoubleClicked(QTableWidgetItem *item);
    on_tableSplitSelect_itemDoubleClicked(item)
    {
        const idx = this.ui.tableSplitSelect.currentRow();
        if (idx < 0)
            return;

        const list = g_project.splitInfoList();
        this.rangeToUi(list[idx].rangeList);
    }

    // bool parseRange(QString text, SplitInfo::Range &range);
    //   出参在 JS 里直接改 range 对象的字段(对象是引用)
    parseRange(text, range)
    {
        text = text.trim();   // text.trimmed();  (C++ 未接收返回值, 这里保持同样"无效"语义)
        if (text === "")
        {
            range.vaild = false;
            return true;
        }

        const list = text.split(",");
        if (list.length !== 2)
            return false;

        const low = Number(list[0]);
        const upper = Number(list[1]);
        const ok1 = list[0].trim() !== "" && !isNaN(low);
        const ok2 = list[1].trim() !== "" && !isNaN(upper);
        if (!ok1 || !ok2)
            return false;

        range.vaild = true;
        range.lower = low;
        range.upper = upper;
        return true;
    }

    // void rangeToUi(std::array<SplitInfo::Range, MAX_SPLIT_LEVEL> *ranges);
    rangeToUi(ranges)
    {
        const count = this.ui.tableWidgetSplit.rowCount();
        for (let idx = 0; idx < 2; idx++)
        {
            for (let i = 0; i < count; i++)
            {
                if (ranges[idx][i].vaild)
                {
                    const text = sprintf("%0.2f,%0.2f", ranges[idx][i].lower, ranges[idx][i].upper);
                    this.ui.tableWidgetSplit.item(i, idx + 1).setText(text);
                }
                else
                    this.ui.tableWidgetSplit.item(i, idx + 1).setText("");
            }
        }
    }

    // bool rangeVaild(int index, int type);
    rangeVaild(index, type)
    {
        if (type !== 2)
        {
            if (this.m_range[type][index].vaild)
                return true;
        }
        else
        {
            if (this.m_range[0][index].vaild && this.m_range[1][index].vaild)
                return true;
        }

        return false;
    }

    // bool uiToRange();
    uiToRange()
    {
        const type = this.ui.boxSplitType.currentIndex();
        const count = this.ui.tableWidgetSplit.rowCount();

        for (let idx = 0; idx < 2; idx++)
        {
            for (let i = 0; i < count; i++)
            {
                if (!this.parseRange(this.ui.tableWidgetSplit.item(i, idx + 1).text(), this.m_range[idx][i]))
                {
                    // .arg(%1,%2) -> sprintf(%d,%d)
                    const tips = sprintf(tr("第行%d,第列%d 数据范围错误,请检查"), i + 1, idx + 2);
                    JZMessageBox.information(this, "", tips);
                    return false;
                }
            }
        }

        let vaild_num = 0;
        for (let i = 0; i < this.m_range[type].length; i++)
        {
            if (this.rangeVaild(i, type))
                vaild_num++;
        }

        if (vaild_num === 0)
        {
            JZMessageBox.information(this, "", tr("至少要有一组分选数据"));
            return false;
        }

        return true;
    }

    // void updateSplitTable();
    updateSplitTable()
    {
        const list = g_project.splitInfoList();
        this.ui.tableSplitSelect.clearContents();
        this.ui.tableSplitSelect.setRowCount(list.length);
        for (let i = 0; i < list.length; i++)
        {
            const itemNo = new QTableWidgetItem(String(i + 1));
            const itemName = new QTableWidgetItem(list[i].name);

            this.ui.tableSplitSelect.setItem(i, 0, itemNo);
            this.ui.tableSplitSelect.setItem(i, 1, itemName);
        }
    }

    // int splitRecord(BTERecordPtr record, SplitResult::RecordInfo &info);  //分选
    //   info 为出参: JS 里直接改传入对象的字段
    splitRecord(record, info)
    {
        const type = this.ui.boxSplitType.currentIndex();
        let split_ret = 15; //落选
        if (record.info.dataCache.beginDis >= 0)
        {
            info.DisChargeCap = record.info.dataCache.endDisCap;
            info.DisChargeEng = record.info.dataCache.endDisEng;
            info.Time = record.info.dataCache.endDisTestTime;
            info.MeanVoltage = record.info.dataCache.midDisVoltage;
            info.CcRatio = record.info.dataCache.ccRatio;

            //查找计算结果
            for (let i = 0; i < this.m_range[0].length; i++)
            {
                if (!this.rangeVaild(i, type))
                    continue;

                if ((type === 0 && info.DisChargeCap >= this.m_range[0][i].lower && info.DisChargeCap <= this.m_range[0][i].upper)
                    || (type === 1 && info.MeanVoltage >= this.m_range[1][i].lower && info.MeanVoltage <= this.m_range[1][i].upper)
                    || (type === 2 && info.DisChargeCap >= this.m_range[0][i].lower && info.DisChargeCap <= this.m_range[0][i].upper
                        && info.MeanVoltage >= this.m_range[1][i].lower && info.MeanVoltage <= this.m_range[1][i].upper))
                {
                    split_ret = i;
                    break;
                }
            }
        }
        else
        {
            info.DisChargeCap = 0;
            info.DisChargeEng = 0;
            info.Time = 0;
            info.MeanVoltage = 0;
            info.CcRatio = 0;
        }
        info.CellNo = record.info.stepInfo.cellNo;
        info.CellPatch = record.info.stepInfo.cellBatch;
        info.Level = split_ret;
        info.itemIndex = record.info.itemIndex;

        return split_ret;
    }
}

// ---------------------------------------------------------------------------
// void ExportSplitResult(QWidget *w, QVector<QStringList> text);   (C++ 定义在 SplitDialog.cpp)
// ---------------------------------------------------------------------------
export function ExportSplitResult(w, text)
{
    const basename = "Split_" + formatDateTime(new Date(), "yyyyMMdd_hhmmss") + ".xlsx";
    // 真文件对话框(引擎的 JZFileDialog): 取消/失败返回 null(比原来的空串更好判)
    const filename = JZFileDialog.getSaveFileName(w, "", basename, "*.xlsx");
    if (!filename)
        return;

    const workbook = workbook_new(filename);
    const sheet_detail = workbook_add_worksheet(workbook, "Sheet1");
    for (let i = 0; i < text.length; i++)
    {
        for (let j = 0; j < text[i].length; j++)
            worksheet_write_string(sheet_detail, i, j, text[i][j], null);
    }
    workbook_close(workbook);
}
