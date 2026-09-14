// ---------------------------------------------------------------------------
// BTEServer.js —— 对应 C++ BTEServer/BTEServer.h + BTEServer.cpp  (2488 行 C++)
//
// 主窗口: 网络事件分发、实时/记录数据落表、工步下发串行队列、报警、分选、查询导出。
//
// **本文件按 C++ 源行顺序分片翻译**, 分片在文件末尾用 "---- 分片 N ----" 分隔,
// 每次续写在上一个分片之后 append, 函数顺序与 C++ 完全一致。
//
// Qt -> JS 约定(与 common/ 各文件一致):
//   s << x -> s.write(x);  s >> x -> x = s.read();
//   信号槽 -> 回调数组字段 + 自由函数 emit(slots, ...args);
//   QMap -> Map; QVector -> 数组; QSet -> Set; QByteArray -> Uint8Array;
//   QDateTime -> Date; QPoint/QRect/QTableWidget/QGridLayout 等 GUI 类
//   按原样保留调用(先不管类是否存在); tr("...") -> 原字符串;
//   C 的 QString::asprintf / sprintf -> 自写 format(fmt, ...args)。
// ---------------------------------------------------------------------------

import { fs, path, process } from 'core/dist/index.js';
import { NetDataManager } from './netDataManager.js';
import { NetPackVariant } from './netPackDefine.js';
import { UiHelper, MAX_SUB_DEVICE, MAX_CHANNEL_NUM, MAX_DEVICE_ITEM, MAX_SPLIT_LEVEL } from './Common.js';
import { formatDateTime, secondsBetween, daysBetween, isBefore, isAfter, addDays, asDate } from './datetime.js';
import { DataTrans } from './DataTrans.js';
import {
    PackDefine, BTS_HEAD_LENGTH, BTS_SEND_HEAD_LENGTH, BTS_RECV_HEAD_LENGTH, BTS_TAIL_LENGTH,
    INVAILD_SUB_ID,
    BTS_CMD_SET_RTC, BTS_CMD_SET_RTC_RESP,
    BGS66_CMD_SET_RT_INTERVAL, BGS66_CMD_SET_RT_INTERVAL_RESP,
    BTS_CMD_SET_STEP76_Ex, BTS_CMD_SET_STEP_EX_RESP76,
    BTS_CMD_START70, BTS_CMD_START_RESP70,
    BTS_CMD_STOP, BTS_CMD_STOP_RESP,
    BTS_CMD_RESUME, BTS_CMD_RESUME_RESP,
    BTS_CMD_RESUME_CONFIRM, BTS_CMD_RESUME_CONFIRM_RESP,
    BTS_CMD_STEP_JUMP, BTS_CMD_STEP_JUMP_RESP,
    BTS_CMD_SEND_PARALLEL, BTS_CMD_SEND_PARALLEL_RESP,
    BTS_CMD_SEND_RTDATA76, BTS_CMD_SEND_DFDATA76, BTS_CMD_SEND_DFDATA_RESP76,
    BTS_CMD_RESUME_QUERY, BTS_CMD_RESUME_QUERY_RESP, BTS_CMD_CONNECT, BTS_CMD_CONNECT_RESP, BTS_CMD_HEART_BEAT, BTS_CMD_LED_ON, BTS_CMD_LED_ON_RESP, BTS_CMD_SEND_INFO, BTS_CMD_SEND_INFO_RESP, STEP_CHARGE, STEP_DISCHARGE, STEP_TYPE_NULL, STEP_TYPE_STOP, WRITE_DEVICE_IP } from './PackDefine.js';
import { AlarmPlayer } from './AlarmPlayer.js';
// 原 ServerThread.js(QThread 包装 NetServer)已删除: JS 无真线程, 直接用 NetServer;
// 线程包装去掉后端口启动方法也由 init(port) 变为 startServer(port), 见构造与 closeEvent。
import { NetServer } from './netServer.js';
import { BTEProject, DeviceData, SplitInfo, g_project, setGProject, ParallelInfo, STATUS_OK } from './BTEProject.js';
import { BTERecord, BTERecordInfo, BTEStepInfo } from './BTERecordDataBase.js';
import { BTEItemWidget, Item_Normal, Item_Small } from './BTEItemWidget.js';
import { SplitResult, ExportSplitResult } from './SplitSettingWidget.js';
import { StepDataDialog } from './StepDataDialog.js';
import {
    stru_BTS_CMD_SEND_RTDATA76, stru_BTS_CMD_SEND_DFDATA76, stru_BTS_CMD_SEND_DFDATA_RESP76,
    stru_BTS_CMD_SEND_PARALLEL, stru_BTS_CMD_STEP_REQUEST, stru_BTS_CMD_SET_STEP76_Ex,
    stru_BTS_CMD_SEND_INFO,
} from './structs.js';
import { HelpDialog } from './HelpDialog.js';
import { QApplication, QFile, QFileDialog, QInputDialog, QMessageBox, QPoint, QRect, QSettings, Qt, qRgb } from './qtcompat.js';
import { createUi } from './bteserverUi.js';   // 主窗口的真 UI(uic 的 Ui_BTEServerClass 替身)
import { SplitDialog } from './SplitDialog.js';
import { StepInfoDialog } from './StepInfoDialog.js';
import { StepJumpDialog } from './StepJumpDialog.js';
import { StepSettingDialog } from './StepSettingDialog.js';
import { stru_BGS66_CMD_SET_RT_INTERVAL, stru_BTS_CMD_LED_ON, stru_BTS_CMD_RESUME, stru_BTS_CMD_RESUME_CONFIRM, stru_BTS_CMD_RESUME_RESP, stru_BTS_CMD_SEND_PARALLEL_CANCEL, stru_BTS_CMD_SET_RTC, stru_BTS_CMD_START70, stru_BTS_CMD_START_RESP70, stru_BTS_CMD_STEP_JUMP, stru_BTS_CMD_STEP_JUMP_RESP, stru_BTS_CMD_STOP, stru_BTS_CMD_STOP_RESP } from './structs.js';
import { switchAppLang } from './main.js';

// #define HIDE_TEST_PAGE 1
const HIDE_TEST_PAGE = 1;

// ---- 实时数据列表(tableList)列序号 ----
export const LST_CHANNEL = 0;       // 通道号
export const LST_CELL_NO = 1;       // 电池编号
export const LST_STEP = 2;          // 工步
export const LST_LOOP = 3;          // 循环
export const LST_STATUS = 4;        // 工作状态
export const LST_TEMPATURE = 5;     // 温度
export const LST_VOLT = 6;          // 电压
export const LST_CURRENT = 7;       // 电流
export const LST_TIME = 8;          // 时间
export const LST_CHARGE_CAP = 9;    // 实充容量
export const LST_DISCHARGE_CAP = 10; // 实放容量
export const LST_CHARGE_ENG = 11;   // 实充能量
export const LST_DISCHARGE_ENG = 12; // 实放能量
export const LST_END_CHARGE_CAP = 13; // 终止充电容量
export const LST_END_DIS_CAP = 14;  // 终止容量

// ---------------------------------------------------------------------------
// struct DeviceInfo
// ---------------------------------------------------------------------------
export class DeviceInfo
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // enum { Status_Complete, Status_WaitStart, Status_Run, Status_WaitStop,
    //        Status_Stop, Status_WaitJump, Status_Error, Status_OffLine, };
    static get Status_Complete() { return 0; }
    static get Status_WaitStart() { return 1; }
    static get Status_Run() { return 2; }
    static get Status_WaitStop() { return 3; }
    static get Status_Stop() { return 4; }
    static get Status_WaitJump() { return 5; }
    static get Status_Error() { return 6; }
    static get Status_OffLine() { return 7; }

    // DeviceInfo();
    constructor()
    {
        this.netId = -1;
        this.DeviceStatus = new Array(MAX_DEVICE_ITEM).fill(0);    // 正在运行
        this.onLineStatus = new Array(MAX_DEVICE_ITEM).fill(false); // 实时状态是否已经同步
        this.alarmFlag = new Array(MAX_DEVICE_ITEM).fill(false);    // 每个item的报警标志(实时帧维护):实时帧报错时置位,通道恢复正常后复位;不随"清除报警"变化
        this.alarmClearFlag = new Array(MAX_DEVICE_ITEM).fill(false); // 每个item的报警确认标志:点击清除后置位(已确认,不再报警);重新下发工步/接续后复位

        this.DeviceID = 0;            // 设备标识号，1个字节,从硬件来
        this.DeviceString = "";
        this.data = null;             // DeviceData *data;
        this.heartTime = null;        // 心跳时间(Date; null = 未收到过心跳)

        this.stepSendWait = false;    // 等待中位机返回
        this.stepSendTime = null;     // 发包时间(Date; null = 未发过)
        this.stepPackList = [];       // QVector<StepRequestPtr> stepPackList; 工步序列

        this.netId = -1;
        this.DeviceStatus.fill(DeviceInfo.Status_Complete);
        this.onLineStatus.fill(false);
        this.alarmFlag.fill(false);
        this.alarmClearFlag.fill(false);

        this.stepSendWait = false;
        this.stepSendTime = new Date();   // 原 QDateTime::currentDateTime()
    }
}

// ---------------------------------------------------------------------------
// class BTEServer : public QMainWindow
// ---------------------------------------------------------------------------
export class BTEServer        // 继承自 QMainWindow: JS 侧先只保留类本身
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明

    // ---- 窗口级转发 ----
    // BTEServer 不再是 QMainWindow 的子类, 而是"持有真窗口(this.ui)的控制器":
    // uiLoader 造出来的对象本身就是 JZWindow, 窗口操作转给它。这样调用点(main.ts 的
    // w.show() / w.close())不用改名字。
    show() { this.ui.show(); }
    close() { this.ui.close(); }
    setTitle(t) { this.ui.setTitle(t); }
    setGeometry(x, y, w, h) { this.ui.setGeometry(x, y, w, h); }

    // 注意: C++ 里 QT 的 on_xxx_clicked / on_xxx_triggered 是"自动连接槽",
    //       由 ui_*.h 的 setupUi 按名字自动 connect; JS 侧保留同名方法, 由调用方显式绑。

    // BTEServer(QWidget *parent = Q_NULLPTR);
    constructor(parent)
    {
        // ---- 成员默认构造(对应 BTEServer.h 的成员声明) ----
        this.m_guid = new Uint8Array(16);
        this.m_respMap = new Map();
        this.m_alarm = null;
        this.m_alarmOn = false;
        this.m_server = new NetServer(null);   // 原 new ServerThread()

        // 定时器句柄: 0 = 未启动。原 C++ 是 QTimer m_autoTimer/m_heartTimer/m_stepTimer +
        // connect(&timer, &QTimer::timeout, slot); JS 侧直接用 setInterval, 句柄存这里(见 startTimers)。
        this.m_autoTimer = 0;
        this.m_heartTimer = 0;
        this.m_stepTimer = 0;

        this.m_downPoint = new QPoint(0, 0);
        this.m_curPoint = new QPoint(0, 0);
        this.m_selectWidget = null;
        this.m_mouseDown = false;
        this.m_waitResume = new Set();
        this.m_itemList = [];
        this.m_stepDataDialog = null;
        this.m_devices = new Map();
        this.m_project = new BTEProject();
        this.m_queryRecordList = [];
        this.m_splitResult = new SplitResult();
        this.m_actLang = null;
        this.m_dispIndex = 0;
        this.m_manager = new NetDataManager();

        // 真 UI(uic 的 Ui_BTEServerClass 替身): uiLoader.load(XML) 直接造真控件树,
        // 返回对象本身是 JZWindow, 且 ui.<name> 就是那些子控件(见 bteserverUi.ts)。
        this.ui = createUi();
        this.m_actLang = new QActionGroup(this);
        this.m_actLang.addAction(this.ui.actionChn);
        this.m_actLang.addAction(this.ui.actionEng);

        // 配置: JZSettings 接口对齐 QSettings, 但取值用 getInt(原来 set.value() 返回 QVariant)
        const set = new JZSettings();
        set.open(jzApp.applicationDirPath() + "/option.ini");
        const lang = set.getInt("lang");
        if (lang === 1)
        {
            this.ui.actionEng.setChecked(true);
        }

        setGProject(this.m_project);   // g_project = &m_project;
        this.m_project.init();
        this.loadProject();
        this.m_guid = new Uint8Array(16);
        for (let i = 0; i < 16; i++)
            this.m_guid[i] = 0x40 + i;

        this.initTable();

        this.m_respMap.set(BTS_CMD_SET_RTC_RESP, BTS_CMD_SET_RTC);
        this.m_respMap.set(BGS66_CMD_SET_RT_INTERVAL_RESP, BGS66_CMD_SET_RT_INTERVAL);
        this.m_respMap.set(BTS_CMD_SET_STEP_EX_RESP76, BTS_CMD_SET_STEP76_Ex);
        this.m_respMap.set(BTS_CMD_START_RESP70, BTS_CMD_START70);
        this.m_respMap.set(BTS_CMD_RESUME_RESP, BTS_CMD_RESUME);
        this.m_respMap.set(BTS_CMD_STOP_RESP, BTS_CMD_STOP);
        this.m_respMap.set(BTS_CMD_STEP_JUMP_RESP, BTS_CMD_STEP_JUMP);
        this.m_respMap.set(BTS_CMD_SEND_PARALLEL_RESP, BTS_CMD_SEND_PARALLEL);
        this.m_respMap.set(BTS_CMD_RESUME_CONFIRM_RESP, BTS_CMD_RESUME_CONFIRM);

        this.m_stepDataDialog = new StepDataDialog(this);

        // 创建连接
        this.connectUi();   // 原 connectSlotsByName 的那一层(见 connectUi())

        this.m_server.newConnect.push((netId) => this.onNewConnect(netId));        // Qt::QueuedConnection
        this.m_server.disConnect.push((netId) => this.onDisConnect(netId));        // Qt::QueuedConnection
        this.m_server.newDataRecv.push((netId, ptr) => this.onDataRecv(netId, ptr)); // Qt::QueuedConnection

        this.ui.splitSettingWidget.sigPrepareData.push(() => this.onSplitPrepareData());
        this.ui.splitSettingWidget.sigSplitFinish.push(() => this.onSplitFinish());
        // 定时器不再有 sigTimeout 数组: 由 startTimers() 里的 setInterval 直接调槽(见下方)

        this.m_stepDataDialog.sigChannelChanged.push((index) => this.onStepDataChannelChanged(index));

        this.ui.listWidget.setContextMenuPolicy(Qt.CustomContextMenu);
        this.ui.listWidget.sigCustomContextMenuRequested.push((pt) => this.onListItemMenu(pt));

        // 设置item
        const layout = new QGridLayout();
        for (let i = 0; i < MAX_SUB_DEVICE * MAX_CHANNEL_NUM; i++)
        {
            const row = (i / 8) | 0;
            const col = i % 8;

            const item = new BTEItemWidget();
            layout.addWidget(item, row, col);
            item.setVisible(false);
            item.setChannelText(sprintf("%d - %d", row + 1, col + 1));
            item.setItemIndex(i);

            // C++ 在槽里用 sender() 取发信号的卡片, JS 侧由回调把 item 传进去
            item.sigCustomContextMenuRequested.push((pt) => this.onBTEItemMenu(pt, item));
            item.sigToggled.push((check) => this.onBTEItemChecked(check, item));   // C++ 槽里用 sender() 取卡片

            this.m_itemList.push(item);   // m_itemList.push_back(item);
        }
        layout.setColumnStretch(8, 1);
        layout.setRowStretch(8, 1);
        layout.setMargin(3);
        layout.setSpacing(3);
        this.ui.itemWidgets.setLayout(layout);

        this.initShortCuts();
        //
        this.ui.tabWidget.setCurrentIndex(0);
        this.ui.tabWidgetSplit.setCurrentIndex(0);

        if (HIDE_TEST_PAGE)
        {
            this.ui.btnShowTest.hide();
        }
        else
        {
            // LogManager.instance().setLogLevel(LogLevel.LOG_DEBUG);
            //   原 LogManager.js 已删除, 日志级别改由引擎 console 接管
        }

        this.on_tabWidget_currentChanged(0);
        this.m_selectWidget = new QWidget(this);
        this.m_selectWidget.setStyleSheet("border: 2px solid rgb(255, 255, 127);");
        this.m_selectWidget.hide();

        // 报警音(收到通道故障后循环播放,直到用户手动清除)
        this.m_alarm = new AlarmPlayer();
        this.m_alarmOn = false;

        // 启动服务
        this.m_dispIndex = 0;
        this.m_mouseDown = false;

        this.initDeviceList();

        // this.m_server.start();          // 原 QThread::start(), JS 无线程可起
        this.m_server.startServer(8001);  // 原 m_server.init(8001)

        this.startTimers();   // 原 m_heartTimer.start(1000) + m_autoTimer.start(60*1000) + m_stepTimer.start(100)

        console.log("start program");
    }

    // 成员: 见 BTEServer.h 的 private 段(构造里用到, 这里集中声明以保持与 C++ 一致)
    //   m_guid[16] / m_respMap / m_alarm / m_alarmOn / m_server / m_autoTimer /
    //   m_heartTimer / m_stepTimer / m_downPoint / m_curPoint / m_selectWidget /
    //   m_mouseDown / m_waitResume / m_itemList / m_stepDataDialog / m_devices /
    //   m_project / m_queryRecordList / m_splitResult / m_actLang / m_dispIndex /
    //   m_manager / ui
    //   (m_autoTimer/m_heartTimer/m_stepTimer 现为 setInterval 句柄, 不是 QTimer 对象)

    // ---------------------------------------------------------------------------
    // 定时器(JS 原生)
    //
    // C++:   connect(&m_heartTimer, &QTimer::timeout, this, &BTEServer::onHeartTime);
    //        m_heartTimer.start(1000);
    // JS :   setInterval(() => this.onHeartTime(), 1000) —— 句柄存成员, 0 表示未启动。
    //        Qt 的"定时器是对象、信号槽解耦"在这里没必要: 每个定时器只有一个槽。
    // ---------------------------------------------------------------------------
    startTimers()
    {
        this.stopTimers();

        this.m_heartTimer = setInterval(() => this.onHeartTime(), 1000);
        this.m_autoTimer = setInterval(() => this.onAutoTime(), 60 * 1000);
        this.m_stepTimer = setInterval(() => this.onStepTime(), 100);
    }

    stopTimers()
    {
        if (this.m_heartTimer) { clearInterval(this.m_heartTimer); this.m_heartTimer = 0; }
        if (this.m_autoTimer) { clearInterval(this.m_autoTimer); this.m_autoTimer = 0; }
        if (this.m_stepTimer) { clearInterval(this.m_stepTimer); this.m_stepTimer = 0; }
    }

    // ~BTEServer();
    dispose()
    {
        console.log("exit program");

        this.stopTimers();
        this.stopAlarm();
        this.m_alarm = null;      // delete m_alarm;

        this.saveProject();
    }

    // void loadProject();
    loadProject()
    {
        this.m_project.load();
    }

    // void saveProject();
    saveProject()
    {
        this.m_project.save();
    }

    // void CrashExit();
    CrashExit()
    {
        // 保存
        this.m_project.save();

        // 关闭服务
        this.m_server.stopServer();   // 原 m_server.deinit()
    }

    // void initTable();
    initTable()
    {
        // tableQuery
        const strListHeader = [];
        strListHeader.push(tr("记录号"), tr("通道号"), tr("分选"), tr("启动时间"), tr("结束时间"), tr("记录条数"), tr("电池编号"),
            tr("批号/电池型号"), tr("创建者"), tr("备注"));
        UiHelper.SetTable(this.ui.tableQuery, strListHeader);

        this.ui.tableQuery.setContextMenuPolicy(Qt.CustomContextMenu);
        this.ui.tableQuery.sigCustomContextMenuRequested.push((pos) => this.onTableQueryMenu(pos));

        // tableList
        const strListList = [];
        strListList.push(tr("通道号"), tr("电池编号"), tr("工步"), tr("循环"), tr("工作状态"), tr("温度"), tr("电压"), tr("电流"),
            tr("时间"), tr("实充容量"), tr("实放容量"), tr("实充能量"), tr("实放能量"), tr("终止充电容量"), tr("终止容量"));
        UiHelper.SetTable(this.ui.tableList, strListList);

        this.ui.tableList.setRowCount(MAX_DEVICE_ITEM);
        for (let i = 0; i < MAX_DEVICE_ITEM; i++)
        {
            const itemNo = new QTableWidgetItem();
            itemNo.setText(sprintf("%02d-%02d", ((i / 8) | 0) + 1, i % 8 + 1));
            this.ui.tableList.setItem(i, 0, itemNo);
            for (let j = 1; j < strListList.length; j++)
                this.ui.tableList.setItem(i, j, new QTableWidgetItem());
        }

        // tableSplitLevel
        const strListSplitLevel = [];
        strListSplitLevel.push(tr("级别"), tr("电池个数"), tr("点灯"));
        UiHelper.SetTable(this.ui.tableSplitLevel, strListSplitLevel);

        // tableSplitList
        const strListSplitList = [];
        strListSplitList.push(tr("序号"), tr("单元-通道"), tr("放电容量(Ah)"), tr("放电时间（H:M:S:MS）"),
            tr("中值电压（V）"), tr("恒流比（%）"), tr("级别"), tr("电池编号"));
        UiHelper.SetTable(this.ui.tableSplitList, strListSplitList);
    }

    // virtual void showEvent(QShowEvent *event) override;
    showEvent(event)
    {
        QMainWindow.prototype.showEvent.call(this, event);
    }

    // virtual void closeEvent(QCloseEvent *event) override;
    closeEvent(event)
    {
        let can_close = true;

        for (const it of this.m_devices.values())
        {
            for (let i = 0; i < it.DeviceStatus.length; i++)
            {
                if (it.DeviceStatus[i] !== DeviceInfo.Status_Complete
                    && it.DeviceStatus[i] !== DeviceInfo.Status_Stop)
                {
                    can_close = false;
                    break;
                }
            }
        }

        if (!can_close)
        {
            event.ignore();
            QMessageBox.information(this, "", tr("正在运行，请先停止所有设备通道再退出"));
            return;
        }

        this.stopTimers();            // 原 QTimer 随对象析构自动停; JS 的 setInterval 必须显式清
        this.m_server.stopServer();   // 原 m_server.deinit()
        QMainWindow.prototype.closeEvent.call(this, event);
    }

    // virtual void paintEvent(QPaintEvent *event) override;
    paintEvent(event)
    {
        QMainWindow.prototype.paintEvent.call(this, event);
    }

    // virtual void mousePressEvent(QMouseEvent *event) override;
    mousePressEvent(event)
    {
        if (event.button() === Qt.LeftButton && this.ui.stackedWidget.currentIndex() === 0
            && this.ui.stackedWidget.geometry().contains(event.pos()))
        {
            this.m_mouseDown = true;
            this.m_downPoint = event.pos();
        }
    }

    // virtual void mouseMoveEvent(QMouseEvent *event) override;
    mouseMoveEvent(event)
    {
        this.m_curPoint = event.pos();
        if (this.m_mouseDown && this.m_curPoint.minus(this.m_downPoint).manhattanLength() > 40)
        {
            this.m_selectWidget.setGeometry(new QRect(this.m_downPoint, this.m_curPoint).normalized());
            this.m_selectWidget.show();
            this.m_selectWidget.raise();
            this.update();
        }
    }

    // virtual void mouseReleaseEvent(QMouseEvent *event) override;
    mouseReleaseEvent(event)
    {
        if (this.m_mouseDown && this.m_selectWidget.isVisible())
        {
            const sel = this.m_selectWidget.geometry();

            let first = true;
            const w = this.m_itemList[0].width();
            const h = this.m_itemList[0].height();
            for (let i = 0; i < this.m_itemList.length; i++)
            {
                if (!this.m_itemList[i].isVisible())
                    continue;

                const p1 = this.m_itemList[i].mapTo(this, new QPoint(0, 0));
                const p2 = this.m_itemList[i].mapTo(this, new QPoint(w, h));

                const rc = new QRect(p1, p2);
                if (sel.intersects(rc))
                {
                    if (first && QApplication.keyboardModifiers() !== Qt.ControlModifier)
                    {
                        for (let j = 0; j < this.m_itemList.length; j++)
                            this.m_itemList[j].setCheck(false);
                    }

                    this.m_itemList[i].setCheck(true);
                    first = false;
                }
            }
        }

        this.m_mouseDown = false;
        this.m_selectWidget.hide();
    }

    // void initShortCuts();
    initShortCuts()
    {
        const selectAll = new QShortcut(new QKeySequence(tr("Ctrl+A")), this);
        selectAll.sigActivated.push(() => this.onBTEItemSelectAll());
    }

    // void dispPack(const NetPack &pack, QTextEdit *textEdit);
    dispPack(pack, textEdit)
    {
        if (!this.ui.boxRecordPack.isChecked())
            return;

        if (this.ui.boxShowRT.isChecked() && pack.info.CmdID === BTS_CMD_SEND_RTDATA76)
            return;
        if (this.ui.boxShowDF.isChecked() && (pack.info.CmdID === BTS_CMD_SEND_DFDATA76
            || pack.info.CmdID === BTS_CMD_SEND_DFDATA_RESP76))
            return;

        const pack_data = this.m_manager.packData(pack);
        let disp = "";
        for (let i = 0; i < pack_data.length; i++)
        {
            disp += sprintf("%02X ", pack_data[i]);
            if ((i + 1) % 16 === 0 && i !== pack_data.length - 1)
                disp += "\n";
        }
        disp += "\n";

        const time = formatDateTime(new Date(), "[HH:mm:ss:zzz] ");
        let title = time + sprintf("idx = %d,cmd = %#04X", this.m_dispIndex, pack.info.CmdID);
        if (pack.info.IsRecvPack())
            title += sprintf(",ret = %#02X", pack.info.Result);

        textEdit.append(title + "\n" + disp);
        this.ui.textLog.append(title);

        this.m_dispIndex++;
    }

    // void send(int netId, ushort cmd, const QByteArray &data, uchar SubDevID = INVAILD_SUB_ID);
    // void send(int netId, StepRequestPtr pack);
    //   C++ 的两个重载合成一个: 第 2 个参数是 StepRequest 时走工步分支。
    send(netId, cmdOrPack, data, SubDevID)
    {
        if (cmdOrPack instanceof stru_BTS_CMD_STEP_REQUEST)
        {
            const pack = cmdOrPack;
            const body = new Uint8Array(0);
            pack.pack(body);

            this.send(netId, pack.type(), body, pack.SubDeviceId);
            return;
        }

        const cmd = cmdOrPack;
        console.assert(cmd && this.m_devices.has(netId));   // Q_ASSERT(cmd && m_devices.contains(netId));

        const device = this.m_devices.get(netId);
        if (SubDevID !== INVAILD_SUB_ID)
            SubDevID += 1;

        const pack = new NetPackVariant();
        pack.info.DeviceID = device.DeviceID;
        pack.info.SubDevID = SubDevID;
        pack.info.Length = BTS_HEAD_LENGTH + BTS_SEND_HEAD_LENGTH + data.length + BTS_TAIL_LENGTH;
        pack.info.CmdID = cmd;
        pack.data = data;

        console.log("device %s send %#04X", device.DeviceString, pack.info.CmdID);
        this.dispPack(pack, this.ui.textSend);
        this.m_server.sendPack(netId, pack);
    }

    // void sendParallel(int netId,QVector<int> channels);
    sendParallel(netId, itemIdx)
    {
        console.assert(this.m_devices.has(netId));   // Q_ASSERT(m_devices.contains(netId));

        const device = this.m_devices.get(netId);
        for (let i = 0; i < itemIdx.length; i++)
        {
            let pal_idx = -1;
            for (let pal = 0; pal < device.data.parallelList.length; pal++)
            {
                if (device.data.parallelList[pal].channels.indexOf(itemIdx[i]) >= 0)
                {
                    pal_idx = pal;
                    break;
                }
            }
            if (pal_idx < 0)
                continue;

            const p = device.data.parallelList[pal_idx];

            const pack = new stru_BTS_CMD_SEND_PARALLEL();
            pack.SubDeviceId = (p.channels[0] / 8) | 0;
            for (let j = 0; j < p.channels.length; j++)
                pack.channelInfo.push(p.channels[j] % 8);   // pack->channelInfo.push_back(...)

            device.stepPackList.push(pack);
        }

        this.sendStepPack(netId);
    }

    // void sendStepPack(int netId);
    sendStepPack(netId)
    {
        if (!this.m_devices.has(netId))
            return;

        // 发送
        const device = this.m_devices.get(netId);
        if (!device.stepSendWait && device.stepPackList.length > 0) //原: stepSendTime.msecsTo(QDateTime::currentDateTime()) >= 1000)
        {
            device.stepSendTime = new Date();   // 原 QDateTime::currentDateTime()
            device.stepSendWait = true;
            this.send(netId, device.stepPackList[0]);   // device.stepPackList.front()
        }
    }

    // void response(int netId, ushort cmd, uchar result, uchar SubDevID = INVAILD_SUB_ID, QByteArray body = QByteArray());
    // void response(int netId, const stru_BTS_CMD_SEND_DFDATA_RESP76 &pack);
    //   C++ 的两个重载合成一个: 第 2 个参数是 DFData 回应包时走那条分支。
    response(netId, cmdOrPack, result, SubDevID, data)
    {
        if (cmdOrPack instanceof stru_BTS_CMD_SEND_DFDATA_RESP76)
        {
            const body = new Uint8Array(0);
            cmdOrPack.pack(body);

            this.response(netId, BTS_CMD_SEND_DFDATA_RESP76, 0, 0, body);
            return;
        }

        const cmd = cmdOrPack;
        console.assert(this.m_devices.has(netId));   // Q_ASSERT(m_devices.contains(netId));
        const device = this.m_devices.get(netId);

        const pack = new NetPackVariant();
        pack.info.DeviceID = device.DeviceID;
        pack.info.SubDevID = SubDevID;
        pack.info.Length = BTS_HEAD_LENGTH + BTS_RECV_HEAD_LENGTH + data.length + BTS_TAIL_LENGTH;
        pack.info.CmdID = cmd;
        pack.info.Result = result;
        pack.data = data;

        console.log("device %s response %#04X", device.DeviceString, pack.info.CmdID);
        this.dispPack(pack, this.ui.textSend);
        this.m_server.sendPack(netId, pack);
    }

    // void onNewConnect(int netId);
    onNewConnect(netId)
    {
        const netinfo = this.m_server.netInfo(netId);
        console.log("connect ip = %s:%d", netinfo.ip, netinfo.port);

        const info = new DeviceInfo();
        info.data = null;
        info.netId = netId;
        this.m_devices.set(netId, info);
    }

    // void onDisConnect(int netId);
    onDisConnect(netId)
    {
        this.disconnect(netId);
    }

    // void disconnect(int netId);
    disconnect(netId)
    {
        if (!this.m_devices.has(netId))
            return;

        const count = this.ui.listWidget.count();
        for (let i = 0; i < count; i++)
        {
            const item = this.ui.listWidget.item(i);
            if (Number(item.data(Qt.UserRole)) === netId)
            {
                const deviceId = String(item.data(Qt.UserRole + 1));
                item.setData(Qt.UserRole, -1);
                item.setText(deviceId + tr("(离线)"));
                break;
            }
        }

        console.log("disconnect ip = %s", this.m_devices.get(netId).DeviceString);

        // 断开连接后清空该设备的报警/确认标志(设备不再在线,其报错不再计入报警)
        this.m_devices.get(netId).alarmFlag.fill(false);
        this.m_devices.get(netId).alarmClearFlag.fill(false);

        this.m_devices.delete(netId);

        // 设备离线后重算报警:已无任何在线设备的报错 item 则停止报警音
        this.refreshAlarmState();
    }

    // int deviceToNet(QString deviceId);
    deviceToNet(deviceId)
    {
        for (const [key, it] of this.m_devices)
        {
            if (it.DeviceString === deviceId)
                return key;

            continue;
        }

        return -1;
    }

    // int curNetId() const;
    curNetId()
    {
        const idx = this.ui.listWidget.currentRow();
        return idx < 0 ? -1 : Number(this.ui.listWidget.item(idx).data(Qt.UserRole));
    }

    // QString curDeviceString();
    curDeviceString()
    {
        const idx = this.ui.listWidget.currentRow();
        return idx < 0 ? "" : String(this.ui.listWidget.item(idx).data(Qt.UserRole + 1));
    }

    // void on_actionExit_triggered();
    on_actionExit_triggered()
    {
        this.close();
    }

    // ---- 分片 1 结束(对应 C++ BTEServer.cpp 1..559 行) ----

    // void on_actionAbout_triggered();
    on_actionAbout_triggered()
    {
        const dlg = new HelpDialog(this);
        dlg.exec();
    }

    // void switchLang(int lang);
    switchLang(lang)
    {
        switchAppLang(lang);
        this.ui.retranslateUi(this);
    }

    // void on_actionChn_triggered();
    on_actionChn_triggered()
    {
        const set = new QSettings(qApp.applicationDirPath() + "/option.ini", QSettings.IniFormat);
        set.setValue("lang", 0);
        this.switchLang(0);
    }

    // void on_actionEng_triggered();
    on_actionEng_triggered()
    {
        const set = new QSettings(qApp.applicationDirPath() + "/option.ini", QSettings.IniFormat);
        set.setValue("lang", 1);
        this.switchLang(1);
    }

    // void onDataRecv(int netId, NetPackPtr pack);
    onDataRecv(netId, pack)
    {
        const ptr = pack;    // NetPackVariant *ptr = (NetPackVariant*)pack.data();
        this.dispPack(ptr, this.ui.textRecv);

        if (!this.m_devices.has(netId))
        {
            console.error("invaild device");
            return;
        }

        const cmd = ptr.info.CmdID;
        const device = this.m_devices.get(netId);
        console.log("device %s recv pack: %#04X", device.DeviceString, cmd);

        if (cmd === BTS_CMD_CONNECT)
        {
            device.DeviceID = ptr.info.DeviceID;
            this.response(netId, BTS_CMD_CONNECT_RESP, 0);
            return;
        }
        else if (cmd === BTS_CMD_SEND_INFO)
        {
            const info = new stru_BTS_CMD_SEND_INFO();
            if (info.unpack(ptr.data))
            {
                const netInfo = this.m_server.netInfo(netId);
                const DeviceString = sprintf("%s-%dV%dA", netInfo.ip, info.VoltageMax, (info.ChargeMax / 1000) | 0);

                for (const [key, it] of this.m_devices)
                {
                    //重复,删除已有连接
                    if (it.DeviceString === DeviceString && key !== netId)
                    {
                        const old_id = key;
                        this.m_server.closeConnect(old_id);
                        this.disconnect(old_id);
                        break;
                    }
                }

                device.DeviceID = ptr.info.DeviceID;
                device.DeviceString = DeviceString;

                if (!device.data)
                {
                    device.data = this.m_project.deviceData(DeviceString);
                    this.initDeviceList();
                }

                device.data.deviceInfo = info;
                if (this.curNetId() === netId)
                    this.initDevice();

                //回复
                this.response(netId, BTS_CMD_SEND_INFO_RESP, 0);

                //set rtc
                const rtc = new stru_BTS_CMD_SET_RTC();
                rtc.time = new Date();   // 原 QDateTime::currentDateTime()
                device.stepPackList.push(rtc);

                //set interval
                const RTInterval = new stru_BGS66_CMD_SET_RT_INTERVAL();
                RTInterval.RTInterval = 1;
                device.stepPackList.push(RTInterval);

                // QTimer::singleShot(200, [this, netId] { this->sendStepPack(netId); });
                setTimeout(() => { this.sendStepPack(netId); }, 200);
            }
            return;
        }
        if (!device.data)
        {
            console.error("error pack seq type = %#x", cmd);
            return;
        }

        if (cmd === BTS_CMD_HEART_BEAT)
        {
            device.heartTime = new Date();   // 原 QDateTime::currentDateTime()
        }
        else if (cmd === BTS_CMD_RESUME_QUERY)
        {
            this.response(netId, BTS_CMD_RESUME_QUERY_RESP, 0);
            this.showWaitResume(device);
        }
        else if (this.m_respMap.has(cmd))
        {
            if (cmd === BTS_CMD_RESUME_CONFIRM_RESP)
            {
            }
            else if (cmd === BTS_CMD_SET_RTC_RESP)
            {
            }
            else if (cmd === BGS66_CMD_SET_RT_INTERVAL_RESP)
            {
            }
            else if (cmd === BTS_CMD_SET_STEP_EX_RESP76)
            {
            }
            else if (cmd === BTS_CMD_START_RESP70)
            {
                const start_res = new stru_BTS_CMD_START_RESP70();
                start_res.unpack(ptr.data);

                const sub_idx = ptr.info.SubDevID - 1;
                for (let i = 0; i < start_res.ChannelCount; i++)
                {
                    const item_idx = sub_idx * MAX_CHANNEL_NUM + (start_res.channelInfo[i].ChannelIDByNum - 1);
                    if (item_idx >= 0 && item_idx < this.m_itemList.length)
                    {
                        if (device.DeviceStatus[item_idx] === DeviceInfo.Status_WaitStart)
                            this.setItemStatus(netId, item_idx, DeviceInfo.Status_Run);
                    }
                    else
                    {
                        console.error("BTS_CMD_START_RESP70 sub_id = %d,chn = %d", ptr.info.SubDevID, start_res.channelInfo[i].ChannelIDByNum);
                        break;
                    }
                }
            }
            else if (cmd === BTS_CMD_STOP_RESP)
            {
                const stop_res = new stru_BTS_CMD_STOP_RESP();
                stop_res.unpack(ptr.data);

                const sub_idx = ptr.info.SubDevID - 1;
                const stopList = [];
                for (let i = 0; i < 8; i++)
                {
                    const item_idx = sub_idx * MAX_CHANNEL_NUM + i;
                    if (item_idx >= 0 && item_idx < this.m_itemList.length)
                    {
                        const chn = (1 << i);
                        if (chn & stop_res.ChannelID[0])
                            stopList.push(item_idx);
                    }
                    else
                    {
                        stopList.length = 0;   // stopList.clear();
                        console.error("BTS_CMD_STOP_RESP sub_id = %d", ptr.info.SubDevID);
                        break;
                    }
                }
                if (stopList.length > 0)
                    this.recordsStop(netId, stopList);
            }
            else if (cmd === BTS_CMD_RESUME_RESP)
            {
                const resume_res = new stru_BTS_CMD_RESUME_RESP();
                resume_res.unpack(ptr.data);

                const sub_idx = ptr.info.SubDevID - 1;
                for (let i = 0; i < 8; i++)
                {
                    const item_idx = sub_idx * MAX_CHANNEL_NUM + i;
                    if (item_idx >= 0 && item_idx < this.m_itemList.length)
                    {
                        if (device.DeviceStatus[item_idx] === DeviceInfo.Status_WaitStart)
                            this.setItemStatus(netId, item_idx, DeviceInfo.Status_Run);
                    }
                    else
                    {
                        console.error("BTS_CMD_RESUME_RESP sub_id = %d", ptr.info.SubDevID);
                        break;
                    }
                }
            }
            else if (cmd === BTS_CMD_STEP_JUMP_RESP)
            {
                const jump_res = new stru_BTS_CMD_STEP_JUMP_RESP();
                jump_res.unpack(ptr.data);

                const sub_idx = ptr.info.SubDevID - 1;
                for (let i = 0; i < 8; i++)
                {
                    const item_idx = sub_idx * MAX_CHANNEL_NUM + i;
                    if (item_idx >= 0 && item_idx < this.m_itemList.length)
                    {
                        if (device.DeviceStatus[item_idx] === DeviceInfo.Status_WaitJump)
                            this.setItemStatus(netId, item_idx, DeviceInfo.Status_Run);
                    }
                    else
                    {
                        console.error("BTS_CMD_STEP_JUMP_RESP sub_id = %d", ptr.info.SubDevID);
                        break;
                    }
                }
            }
            else if (cmd === BTS_CMD_SEND_PARALLEL_RESP)
            {
            }

            if (device.stepPackList.length > 0 &&
                this.m_respMap.get(cmd) === device.stepPackList[0].type())
            {
                device.stepSendWait = false;
                device.stepPackList.shift();   // stepPackList.pop_front();
            }
            this.sendStepPack(netId);
        }
        else if (cmd === BTS_CMD_SEND_RTDATA76)
        {
            const data76 = new stru_BTS_CMD_SEND_RTDATA76();
            if (data76.unpack(ptr.data))
            {
                const item_idx = (ptr.info.SubDevID - 1) * 8 + data76.ChannelIDByNum - 1;
                if (item_idx >= 0 && item_idx < this.m_itemList.length)
                {
                    device.onLineStatus[item_idx] = true;

                    //上位机异常中断，恢复状态
                    if (data76.StepType !== STEP_TYPE_NULL && data76.workStatus() === stru_BTS_CMD_SEND_RTDATA76.WORK_RUN
                        && (device.DeviceStatus[item_idx] === DeviceInfo.Status_Complete || device.DeviceStatus[item_idx] === DeviceInfo.Status_Stop))
                    {
                        this.m_project.resumeRecord(device.DeviceString, item_idx);
                        this.setItemStatus(netId, item_idx, DeviceInfo.Status_Run);

                        console.log("channel resume ip = %s,index = %d", device.DeviceString, item_idx);
                    }

                    //操作出错
                    if (data76.workStatus() === stru_BTS_CMD_SEND_RTDATA76.WORK_USER_STOP
                        || data76.workStatus() === stru_BTS_CMD_SEND_RTDATA76.WORK_ERROR_STOP)
                    {
                        this.setItemStatus(netId, item_idx, DeviceInfo.Status_Stop);
                        if (data76.workStatus() === stru_BTS_CMD_SEND_RTDATA76.WORK_ERROR_STOP)
                            console.log("channel error ip = %s,index = %d,code = %#0X", device.DeviceString, item_idx, data76.PrtCode);
                    }

                    this.m_project.setRTData(device.DeviceString, item_idx, data76);
                    if (netId === this.curNetId())
                    {
                        this.m_itemList[item_idx].setAlarmCleared(device.alarmClearFlag[item_idx]);
                        this.setItemData(item_idx, data76);
                    }

                    //通道故障(Bit7/错误停止/保护码,与卡片标红判据一致):
                    //报警标志对所有设备都记录,不受界面显示限制;该通道恢复正常后自动清除;
                    //是否响铃由 refreshAlarmState 按"在线+可见+置位且未确认"统一判定
                    const channelFault = (data76.WorkType & 0x80) !== 0
                        || data76.workStatus() === stru_BTS_CMD_SEND_RTDATA76.WORK_ERROR_STOP
                        || data76.PrtCode !== 0;
                    if (device.alarmFlag[item_idx] !== channelFault)
                    {
                        device.alarmFlag[item_idx] = channelFault;
                        if (channelFault)
                            console.log("alarm set ip = %s,index = %d,code = %#0X", device.DeviceString, item_idx, data76.PrtCode);
                        this.refreshAlarmState();
                    }

                    //结束
                    if (data76.StepType === STEP_TYPE_STOP)
                    {
                        const item_idxs = [];
                        item_idxs.push(item_idx);
                        this.recordsFinish(netId, item_idxs);
                    }
                }
                else
                {
                    console.error("BTS_CMD_SEND_RTDATA76 sub_id = %d,chn = %d", ptr.info.SubDevID, data76.ChannelIDByNum);
                }
            }
        }
        else if (cmd === BTS_CMD_SEND_DFDATA76)
        {
            const df76 = new stru_BTS_CMD_SEND_DFDATA76();
            if (df76.unpack(ptr.data))
            {
                const DeviceString = device.DeviceString;
                for (let i = 0; i < df76.DFDataNumber; i++)
                {
                    const data = df76.dataList[i];
                    const item_idx = (data.SubDevID - 1) * 8 + data.ChannelIDByNum - 1;
                    if (item_idx >= 0 && item_idx < this.m_itemList.length)
                    {
                        if (data.DataFlag === 0x55)
                        {
                            if (this.m_project.record(DeviceString, item_idx, data) === STATUS_OK)
                                this.m_stepDataDialog.addRealStep(DeviceString, item_idx, data);
                        }
                        if (netId === this.curNetId())
                        {
                            if (PackDefine.StepChargeType(data.StepType) === STEP_CHARGE)
                                this.setItemLastChargeCap(item_idx, DataTrans.fromCapUnit(data.ChargeCap));
                            if (PackDefine.StepChargeType(data.StepType) === STEP_DISCHARGE)
                                this.setItemLastDisChargeCap(item_idx, DataTrans.fromCapUnit(data.DisChargeCap));
                        }
                    }
                    else
                    {
                        console.error("BTS_CMD_SEND_DFDATA76 sub_id = %d,chn = %d", data.SubDevID, data.ChannelIDByNum);
                        break;
                    }
                }

                const df76_res = new stru_BTS_CMD_SEND_DFDATA_RESP76();
                df76_res.Index = df76.DFDataPointer;
                this.response(netId, df76_res);
            }
        }
        else if (cmd === BTS_CMD_LED_ON_RESP)
        {

        }
        else
        {
            console.error("unknow pack,type = %#x", cmd);
        }
    }

    // ---- 分片 2 结束(对应 C++ BTEServer.cpp 560..898 行) ----

    // void onListItemMenu(const QPoint &pt);
    onListItemMenu(pt)
    {
        const row = this.ui.listWidget.currentRow();
        if (row < 0)
            return;

        const getIp = (ip, ipType) =>
        {
            const dlg = new QDialog(this);
            const vlayout = new QVBoxLayout();
            const hlayout = new QHBoxLayout();

            const btnOK = new QPushButton(tr("确定"));
            const btnCancel = new QPushButton(tr("取消"));
            btnOK.setMinimumWidth(120);
            btnCancel.setMinimumWidth(120);
            hlayout.addStretch();
            hlayout.addWidget(btnOK);
            hlayout.addStretch();
            hlayout.addWidget(btnCancel);
            hlayout.addStretch();
            hlayout.setMargin(0);
            btnOK.sigClicked.push(() => dlg.accept());     // connect(btnOK, &QPushButton::clicked, &dlg, &QDialog::accept);
            btnCancel.sigClicked.push(() => dlg.reject()); // connect(btnCancel, &QPushButton::clicked, &dlg, &QDialog::reject);

            const layout = new QHBoxLayout();
            layout.setMargin(0);
            layout.addWidget(new QLabel("IP:"));

            const line = new QLineEdit();

            const validator = new MyValidator(line, ipType);
            line.setValidator(validator);
            layout.addWidget(line);
            line.setText(ip);

            vlayout.addWidget(new QLabel(tr("请输入IP")));
            vlayout.addLayout(layout);
            vlayout.addLayout(hlayout);
            dlg.setLayout(vlayout);
            if (dlg.exec() !== QDialog.Accepted)
                return "";

            return line.text();
        };

        const item = this.ui.listWidget.item(row);
        const deviceId = String(item.data(Qt.UserRole + 1));
        const netId = Number(item.data(Qt.UserRole));
        if (netId < 0)
            return;

        const menu = new QMenu();
        const act1 = menu.addAction(tr("更改中位机IP"));
        /*
        const act2 = menu.addAction(tr("写PC服务器IP"));
        const act3 = menu.addAction(tr("写中位机号与MAC号"));
        */

        const act = menu.exec(this.ui.listWidget.mapToGlobal(pt));
        if (act === act1)
        {
            const ip = getIp(deviceId.split("-")[0], 0);
            if (ip !== "")
            {
                const list = ip.split(".");
                if (list.length === 4)
                {
                    // QByteArray data; data.push_back(...);
                    const data = [];
                    data.push(Number(list[2]) & 0xFF);
                    data.push(Number(list[3]) & 0xFF);
                    this.send(netId, WRITE_DEVICE_IP, Uint8Array.from(data));
                }
                else
                {
                    QMessageBox.information(this, "", tr("IP 不合法，请检查"));
                }
            }
        }/*
        else if (act === act2)
        {
            const ip = getIp(deviceId.split("-")[0], 1);
            if (ip !== "")
            {
                const list = ip.split(".");
                if (list.length === 4)
                {
                    const data = [];
                    data.push(Number(list[2]) & 0xFF);
                    data.push(Number(list[3]) & 0xFF);
                    this.send(netId, WRITE_PC_IP, Uint8Array.from(data));
                }
                else
                {
                    QMessageBox.information(this, "", tr("IP 不合法，请检查"));
                }
            }
        }
        else if (act === act3)
        {
            const dlg = new DeviceMacDialog(this);
            dlg.mac = 0;
            dlg.dev = 0;
            if (dlg.exec() !== QDialog.Accepted)
                return;

            const data = [];
            data.push(dlg.dev);
            data.push(dlg.mac);
            this.send(netId, WRITE_DEVICE_MAC, Uint8Array.from(data));
        }
        */
    }

    // bool checkDevice(int netId);
    checkDevice(netId)
    {
        if (!this.m_devices.has(netId))
        {
            QMessageBox.information(this, "", tr("设备已离线"));
            return false;
        }
        return true;
    }

    // void onBTEItemMenu(const QPoint &pt);
    //   C++ 里 item 来自 sender(); JS 侧由信号回调作为第 2 个参数传入。
    onBTEItemMenu(pt, senderWidget)
    {
        const DeviceString = this.curDeviceString();
        if (DeviceString === "")
            return;

        const netId = this.curNetId();

        const item = senderWidget;                     // qobject_cast<BTEItemWidget*>(sender())
        const first_item = item.itemIndex();

        let startEnable = false, stopEnable = false, resumeEnable = false, finishEnable = false;
        let setCountEnable = true;
        let paraEnable = false;

        if (netId > 0)
        {
            //确认通道状态已经更新
            let onLineCheck = true;
            const device = this.m_devices.get(netId);
            for (let i = 0; i < device.onLineStatus.length; i++)
            {
                if (!device.onLineStatus[i])
                {
                    onLineCheck = false;
                    break;
                }
            }

            if (onLineCheck)
            {
                startEnable = (device.DeviceStatus[first_item] === DeviceInfo.Status_Complete
                    || device.DeviceStatus[first_item] === DeviceInfo.Status_Stop);
                stopEnable = (device.DeviceStatus[first_item] === DeviceInfo.Status_Run);
                resumeEnable = (device.DeviceStatus[first_item] === DeviceInfo.Status_Stop);
                finishEnable = (device.DeviceStatus[first_item] === DeviceInfo.Status_Stop);
                paraEnable = true;
            }
        }

        const deviceData = this.m_project.deviceData(DeviceString);
        for (let i = 0; i < MAX_DEVICE_ITEM; i++)
        {
            const stepType = deviceData.recordList[i].info.rtData.StepType;
            if (stepType !== STEP_TYPE_STOP && stepType !== STEP_TYPE_NULL)
            {
                setCountEnable = false;
                break;
            }
        }

        const menu = new QMenu();
        const actStart = menu.addAction(tr("单点启动"));
        const actStop = menu.addAction(tr("单点停止"));
        menu.addSeparator();
        const actResume = menu.addAction(tr("接续"));
        const actJump = menu.addAction(tr("工步跳转"));
        const actFinish = menu.addAction(tr("结束"));
        const actClearAlarm = menu.addAction(tr("清除报警"));
        menu.addSeparator();
        const actPara = menu.addAction(tr("通道并联"));
        const actInfo = menu.addAction(tr("通道信息"));
        const actData = menu.addAction(tr("通道数据"));
        const sub = menu.addMenu(tr("导出数据"));
        const actExport = sub.addAction(tr("导出excel"));
        menu.addSeparator();

        const actNum = menu.addAction(tr("显示数量"));
        const foucsNum = menu.addAction(tr("强制显示全部"));
        const per_menu = menu.addMenu(tr("显示比例"));
        const actPer75 = per_menu.addAction(tr("小"));
        const actPer100 = per_menu.addAction(tr("正常"));
        actNum.setEnabled(setCountEnable);

        const items = [];   //item 索引  (QVector<int> items)
        const channels = [];  //每个子设备通道索引 (QVector<int> channels[MAX_SUB_DEVICE])
        for (let i = 0; i < MAX_SUB_DEVICE; i++)
            channels.push([]);
        const stepCmd = [];
        for (let i = 0; i < this.m_itemList.length; i++)
        {
            if (this.m_itemList[i].isChecked())
            {
                const sub_id = (i / 8) | 0;
                channels[sub_id].push(i);
                items.push(i);
            }
        }

        //并联判断
        if (item.scale() > 1)
            actPara.setText(tr("解除并联"));
        else
        {
            let para = true;
            const first_sub = (first_item / 8) | 0;
            //判断在一行，且非并联
            for (let i = 0; i < items.length; i++)
            {
                if (((items[i] / 8) | 0) !== first_sub || this.m_itemList[items[i]].scale() !== 1)
                {
                    para = false;
                    break;
                }
            }
            //判断数量
            para = para && items.length >= 2;
            actPara.setEnabled(para);
        }
        for (let i = 0; i < items.length; i++)
        {
            if (this.m_itemList[items[i]].status() !== DeviceInfo.Status_Complete
                && this.m_itemList[items[i]].status() !== DeviceInfo.Status_Stop)
                actPara.setEnabled(false);

            /* 有错误时不能完成 */
            //if (this.m_itemList[items[i]].data().PrtCode != 0)
            //	finishEnable = false;
        }
        if (!paraEnable)
            actPara.setEnabled(false);

        stepCmd.push(actStart, actStop, actResume, actJump, actPara);
        actStart.setEnabled(startEnable);
        actStop.setEnabled(stopEnable);
        actResume.setEnabled(resumeEnable);
        actFinish.setEnabled(netId > 0 && finishEnable);
        actJump.setEnabled(stopEnable);

        //清除报警:选中通道中存在故障时才可用
        let clearAlarmEnable = false;
        for (let i = 0; i < items.length; i++)
        {
            if (this.isChannelFault(items[i]))
            {
                clearAlarmEnable = true;
                break;
            }
        }
        actClearAlarm.setEnabled(clearAlarmEnable);

        actFinish.setVisible(false);
        foucsNum.setVisible(false);

        //菜单
        const act = menu.exec(item.mapToGlobal(pt));
        if (stepCmd.indexOf(act) >= 0)
        {
            if (!this.checkDevice(netId))  //可能已经离线
                return;

            const device = this.m_devices.get(netId);
            if (act === actStart)
            {
                //启动采集
                const dlg = new StepSettingDialog();
                dlg.DeviceString = device.DeviceString;
                dlg.itemIndex = first_item;
                dlg.stepInfo = device.data.stepList[first_item];
                if (dlg.exec() === QDialog.Accepted)
                {
                    if (!this.checkDevice(netId))
                        return;

                    //下发工步(启动):解除这些通道此前"清除报警"的确认状态,故障仍在则重新报警
                    for (let i = 0; i < items.length; i++)
                        device.alarmClearFlag[items[i]] = false;
                    this.updateItemAlarmClearFlags();
                    this.refreshAlarmState();

                    if (finishEnable)
                    {
                        const finishList = [];
                        for (let i = 0; i < items.length; i++)
                        {
                            const item_idx = items[i];
                            if (device.DeviceStatus[item_idx] === DeviceInfo.Status_Stop)
                                finishList.push(item_idx);
                        }

                        //软件层面，结束采集
                        this.recordsFinish(netId, finishList);
                    }

                    //设置工步
                    const step = dlg.stepInfo.step;   // stru_BTS_CMD_SET_STEP76_Ex step = dlg.stepInfo.step;
                    step.clearChannel();
                    step.Guid.set(this.m_guid.subarray(0, 16));   // memcpy(step.Guid, m_guid, 16);

                    //启动
                    const start = new stru_BTS_CMD_START70();
                    start.Guid.set(this.m_guid.subarray(0, 16));
                    start.StepID = dlg.stepInfo.startStepIndex;

                    const itemList = [];
                    const infoList = [];
                    const stepList = createPackList(device, channels, [DeviceInfo.Status_Complete, DeviceInfo.Status_Stop], step,
                        (item_idx) =>
                        {
                            device.data.stepList[item_idx] = dlg.stepInfo;
                            itemList.push(item_idx);
                            infoList.push(dlg.stepInfo);
                        });

                    //开始采集
                    const ret = this.m_project.startRecords(DeviceString, itemList, infoList);
                    for (let i = 0; i < itemList.length; i++)
                        this.setItemStatus(netId, itemList[i], DeviceInfo.Status_WaitStart);

                    //添加采集包
                    const startList = [];
                    for (let i = 0; i < stepList.length; i++)
                    {
                        const start_pack = new stru_BTS_CMD_START70(start);
                        start_pack.SubDeviceId = stepList[i].SubDeviceId;
                        start_pack.ChannelID.set(stepList[i].ChannelID);   // memcpy(start_pack->ChannelID, stepList[i]->ChannelID, sizeof(...))

                        startList.push(start_pack);
                    }

                    this.sendParallel(netId, items);
                    device.stepPackList.push(...stepList, ...startList);   // device.stepPackList << stepList << startList;
                }
            }
            else if (act === actStop)
            {
                //手动终止时解除报警(覆盖"运行中报错"的场景;仍存在其它在线报错 item 则继续报警)
                this.refreshAlarmState();

                //通知中位机停止
                const pack = new stru_BTS_CMD_STOP();
                const stopList = createPackList(device, channels, [DeviceInfo.Status_Run], pack,
                    (item_idx) =>
                    {
                        this.setItemStatus(netId, item_idx, DeviceInfo.Status_WaitStop);
                    });

                device.stepPackList.push(...stopList);   // device.stepPackList << stopList;
            }
            else if (act === actResume)
            {
                //接续恢复运行:解除这些通道此前"清除报警"的确认状态,故障仍在则重新报警
                for (let i = 0; i < items.length; i++)
                    device.alarmClearFlag[items[i]] = false;
                this.updateItemAlarmClearFlags();
                //仍存在其它在线报错 item 则继续报警,全部恢复则停止
                this.refreshAlarmState();

                //通知中位机恢复
                const pack = new stru_BTS_CMD_RESUME();
                const resumeList = createPackList(device, channels, [DeviceInfo.Status_Stop], pack,
                    (item_idx) =>
                    {
                        this.setItemStatus(netId, item_idx, DeviceInfo.Status_WaitStart);
                    });

                device.stepPackList.push(...resumeList);   // device.stepPackList << resumeList;
                const resume_items = [];
                for (let i = 0; i < items.length; i++)
                {
                    const item_idx = items[i];
                    if (device.DeviceStatus[item_idx] === DeviceInfo.Status_WaitStart)
                        this.m_project.resumeRecord(device.DeviceString, item_idx);
                }
            }
            else if (act === actJump)
            {
                const dlg = new StepJumpDialog(this);
                dlg.stepInfo = device.data.stepList[first_item];
                if (dlg.exec() === QDialog.Accepted)
                {
                    if (!this.checkDevice(netId))
                        return;

                    const select = dlg.select;
                    //重置索引,只跳转工步相同的
                    for (let i = 0; i < MAX_SUB_DEVICE; i++)
                        channels[i].length = 0;   // channels[i].clear();
                    for (let i = 0; i < this.m_itemList.length; i++)
                    {
                        if (this.m_itemList[i].isChecked() && device.data.stepList[first_item].step
                            === device.data.stepList[first_item].step)
                        {
                            const sub_id = (i / 8) | 0;
                            channels[sub_id].push(i);
                            items.push(i);
                        }
                    }

                    const pack = new stru_BTS_CMD_STEP_JUMP();
                    pack.jumpStepId = select + 1;

                    const jumpList = createPackList(device, channels, [DeviceInfo.Status_Run], pack,
                        (item_idx) =>
                        {
                            this.setItemStatus(netId, item_idx, DeviceInfo.Status_WaitJump);
                        });

                    device.stepPackList.push(...jumpList);   // device.stepPackList << jumpList;
                }
            }
            else if (act === actPara)
            {
                const layout = this.ui.itemWidgets.layout();   // (QGridLayout *)
                if (item.scale() === 1) //并联
                {
                    //保存并联信息
                    const info = new ParallelInfo();
                    info.channels = items;
                    device.data.parallelList.push(info);   // device.data->parallelList << info;
                }
                else
                {
                    for (let i = 0; i < device.data.parallelList.length; i++)
                    {
                        if (device.data.parallelList[i].channels.indexOf(first_item) >= 0)
                        {
                            const pack = new stru_BTS_CMD_SEND_PARALLEL_CANCEL();
                            const p = device.data.parallelList[i];
                            pack.SubDeviceId = (p.channels[0] / 8) | 0;
                            for (let j = 0; j < p.channels.length; j++)
                                pack.channelInfo.push(p.channels[j] % 8);

                            this.send(netId, pack);   // send(netId, StepRequestPtr(pack));
                            device.data.parallelList.splice(i, 1);   // device.data->parallelList.remove(i);
                            break;
                        }
                    }
                }
                this.updateItemLayout();
            }

            //发送数据包
            this.sendStepPack(netId);
        }
        else if (act === actFinish)
        {
            if (!this.checkDevice(netId))
                return;

            const device = this.m_devices.get(netId);
            const finishList = [];
            for (let i = 0; i < items.length; i++)
            {
                const item_idx = items[i];
                if (device.DeviceStatus[item_idx] === DeviceInfo.Status_Stop)
                    finishList.push(item_idx);
            }

            //软件层面，结束采集
            this.recordsFinish(netId, finishList);
        }
        else if (act === actClearAlarm)
        {
            //右键"清除报警":确认选中通道的报警(置 alarmClearFlag,该通道不再报警;
            //alarmFlag 仍由实时帧维护;重新下发工步/接续后恢复报警判定)
            let clearCount = 0;
            if (this.m_devices.has(netId))
            {
                const alarmDevice = this.m_devices.get(netId);
                for (let i = 0; i < items.length; i++)
                {
                    if (items[i] >= 0 && items[i] < alarmDevice.alarmFlag.length && alarmDevice.alarmFlag[items[i]])
                    {
                        alarmDevice.alarmClearFlag[items[i]] = true;
                        clearCount++;
                    }
                }
            }
            if (clearCount > 0)
                console.log("clear alarm, clear count = %d", clearCount);
            this.updateItemAlarmClearFlags();
            this.refreshAlarmState();
        }
        else if (act === actInfo)
        {
            const dlg = new StepInfoDialog(this);
            dlg.record = this.m_project.currentRecord(DeviceString, first_item);
            dlg.exec();
        }
        else if (act === actData || act === actExport)
        {
            const record = this.m_project.currentRecord(DeviceString, first_item);
            this.m_project.loadRecordData(DeviceString, record);
            if (act === actData)
            {
                this.m_stepDataDialog.init(true, DeviceString, first_item, record);
                this.m_stepDataDialog.show();
            }
            else
            {
                this.recordExport(DeviceString, record);
            }
        }
        else if (act === actNum)
        {
            const device = this.m_project.deviceData(DeviceString);

            // QInputDialog::getInt(..., int *ok) -> JS 用 { value, ok } 返回
            const r = QInputDialog.getInt(this, tr("设置显示数量"), tr("显示数量"), device.enableItem, 1, 32, 1);
            const ok = r.ok;
            const enable_count = r.value;
            if (!ok)
                return;

            device.enableItem = enable_count;
            for (let i = 0; i < this.m_itemList.length; i++)
            {
                if (i >= enable_count)
                    this.m_itemList[i].setCheck(false);
                this.m_itemList[i].setEnabled(i < enable_count);
                this.m_itemList[i].setVisible(i < enable_count);
            }
            this.updateItemLayout();
        }
        else if (act === foucsNum)
        {
            for (let i = 0; i < this.m_itemList.length; i++)
                this.m_itemList[i].setVisible(true);
        }
        else if (act === actPer100 || act === actPer75)
        {
            const device = this.m_project.deviceData(DeviceString);
            const size = (act === actPer100) ? Item_Normal : Item_Small;
            device.itemSize = size;
            for (let i = 0; i < this.m_itemList.length; i++)
                this.m_itemList[i].setSize(size);
        }
    }

    // ---- 分片 3 结束(对应 C++ BTEServer.cpp 962..1506 行) ----

    // void recordsStop(int netId, QVector<int> itemList);
    recordsStop(netId, itemList)
    {
        console.assert(this.m_devices.has(netId));   // Q_ASSERT(m_devices.contains(netId));

        const deviceInfo = this.m_devices.get(netId);
        const DeviceString = deviceInfo.DeviceString;
        this.m_project.pauseRecords(DeviceString, itemList);
        for (let i = 0; i < itemList.length; i++)
            this.setItemStatus(netId, itemList[i], DeviceInfo.Status_Stop);
    }

    // void recordsFinish(int netId, QVector<int> itemList);
    recordsFinish(netId, itemList)
    {
        console.assert(this.m_devices.has(netId));   // Q_ASSERT(m_devices.contains(netId));

        const deviceInfo = this.m_devices.get(netId);
        const DeviceString = deviceInfo.DeviceString;
        this.m_project.stopRecords(DeviceString, itemList);
        for (let i = 0; i < itemList.length; i++)
            this.setItemStatus(netId, itemList[i], DeviceInfo.Status_Complete);
    }

    // void recordExport(QString DeviceId, BTERecordPtr record);
    recordExport(DeviceId, record)
    {
        if (record.info.recordNum === 0)
        {
            QMessageBox.information(this, "", tr("无有效数据"));
            return;
        }

        const time_str = formatDateTime(record.info.recordTime, "yyyy-MM-dd hh：mm：ss");

        const item_str = String((record.info.itemIndex / 8 | 0) + 1);   // QString::number(record->info.itemIndex / 8 + 1)
        const chn_str = sprintf("%02d", record.info.itemIndex % 8 + 1);

        let device_id_str = DeviceId;
        const device_ips = DeviceId.split(".");
        if (device_ips.length === 4)
            device_id_str = device_ips.slice(2).join(".");   // device_ips.mid(2).join(".")

        const cellBatch = record.info.stepInfo.cellBatch;
        let filepath = time_str + "_" + device_id_str + "-" + item_str + "-" + chn_str;
        if (cellBatch !== "")
            filepath += "_" + cellBatch;
        filepath += ".xlsx";

        const filename = QFileDialog.getSaveFileName(this, "", filepath, "*.xlsx");
        if (filename === "")
            return;

        this.recordExportToFile(DeviceId, record, filename);
    }

    // void recordExportToFile(QString DeviceId, BTERecordPtr record, QString filename);
    recordExportToFile(DeviceId, record, filename)
    {
        const timeStr = (time) => formatDateTime(time, "yyyy-MM-dd hh:mm:ss");
        const item_no = sprintf("%02d-%02d", (record.info.itemIndex / 8 | 0) + 1, record.info.itemIndex % 8 + 1);

        //lxw_workbook  *workbook = workbook_new(filename.toLocal8Bit().data());
        const workbook = workbook_new(filename);

        const format_normal = workbook_add_format(workbook);
        const format_blue = workbook_add_format(workbook);
        const iformat_blue = workbook_add_format(workbook);
        format_set_num_format(format_normal, "0.0000");

        format_set_bg_color(format_blue, qRgb(204, 255, 255));
        format_set_bg_color(format_blue, qRgb(204, 255, 255));
        format_set_num_format(format_blue, "0.0000");

        const fmt = (idx) => (idx % 2 === 0 ? format_blue : format_normal);
        const ifmt = (idx) => (idx % 2 === 0 ? iformat_blue : null);

        /*info*/
        const sheet_info = workbook_add_worksheet(workbook, "Info");
        let row = 0;
        worksheet_write_string(sheet_info, row++, 0, tr("测试信息"), null);
        worksheet_write_string(sheet_info, row++, 0, timeStr(record.info.recordTime), null);

        const infoHeader = [];
        infoHeader.push(tr("设备"), tr("单元"), tr("通道"), tr("批号/电池型号"), tr("电池编号"), tr("工步文件"), tr("开始时间"),
            tr("结束时间"), tr("分选文件"), tr("等级"), tr("备注"));
        for (let i = 0; i < infoHeader.length; i++)
            worksheet_write_string(sheet_info, row, i, infoHeader[i], format_blue);
        row++;

        //设备
        worksheet_write_string(sheet_info, row, 0, DeviceId, null);
        worksheet_write_number(sheet_info, row, 1, (record.info.itemIndex / 8 | 0) + 1, null);
        worksheet_write_number(sheet_info, row, 2, record.info.itemIndex % 8 + 1, null);

        worksheet_write_string(sheet_info, row, 3, record.info.stepInfo.cellBatch, null);
        worksheet_write_string(sheet_info, row, 4, record.info.stepInfo.cellNo, null);
        worksheet_write_string(sheet_info, row, 6, timeStr(record.info.recordTime), null);
        worksheet_write_string(sheet_info, row, 7, timeStr(record.info.recordTimeEnd), null);

        worksheet_write_string(sheet_info, row, 10, record.info.stepInfo.memo, null);

        /* statis */
        /*let sheet_statis = workbook_add_worksheet(workbook, "Statis");
        const infoStatis = [];
        infoStatis.push("通道", "循环", "步次", "状态", "起始电压(V)", "结束电压(V)", "起始电流(mA)",
            "结束电流(mA)", "容量(mAh)", "持续时间(h:min:s.ms)", "相对时间(h:min:s.ms)", "绝对时间");

        row = 0;
        for (let i = 0; i < infoStatis.length; i++)
            worksheet_write_string(sheet_statis, row, i, infoStatis[i], fmt(i));

        row++;
        //循环项
        let pre_stepId = record.dataList[0].StepID;
        let start_idx = 0;
        let end_idx = -1;

        for (let i = 0; i < record.dataList.length; i++)
        {
            const data = record.dataList[0];
            if (pre_stepId !== data.StepID)
                end_idx = i - 1;
            if (i === record.dataList.length - 1)
                end_idx = i;

            if (end_idx > 0)
            {
                const stepS = record.dataList[start_idx];
                const stepE = record.dataList[end_idx];
                const step_name = PackDefine.StepNameList[stepS.StepType];

                worksheet_write_string(sheet_statis, row, 0, item_no, fmt(0));
                worksheet_write_number(sheet_statis, row, 1, stepS.CycleStatus, fmt(1));

                worksheet_write_number(sheet_statis, row, 2, stepS.StepID, fmt(2));
                worksheet_write_string(sheet_statis, row, 3, step_name, fmt(3));
                worksheet_write_number(sheet_statis, row, 4, DataTrans.fromVoltageUnit(stepS.Voltage), fmt(4));
                worksheet_write_number(sheet_statis, row, 5, DataTrans.fromVoltageUnit(stepE.Voltage), fmt(5));
                worksheet_write_number(sheet_statis, row, 6, DataTrans.fromElectricUnit(stepS.Current), fmt(6));
                worksheet_write_number(sheet_statis, row, 7, DataTrans.fromElectricUnit(stepE.Current), fmt(7));

                const time_gap = stepE.ATime.toMSec() - stepS.ATime.toMSec();
                worksheet_write_string(sheet_statis, row, 8, DataTrans.fromTimeUnitSec(time_gap), fmt(8));
                worksheet_write_string(sheet_statis, row, 9, DataTrans.fromTimeUnitSec(time_gap), fmt(9));
                worksheet_write_string(sheet_statis, row, 10, stepE.ATime.toString(), fmt(10));

                start_idx = end_idx + 1;
            }
        }
        */

        /* detail */
        const infoDetail = [];
        infoDetail.push(tr("记录序号"), tr("状态"), tr("跳转"), tr("循环"), tr("步次"), tr("电流(A)"), tr("电压(V)"),
            tr("容量(Ah)"), tr("能量(Wh)"), tr("相对时间(h:min:s.ms)"), tr("绝对时间"), tr("温度(℃)"), tr("恒流比(%)"));

        //按工步分段,计算每个充电工步的恒流比,记录在该工步最后一条数据下
        const ccRatioMap = new Map();
        {
            const dataList = record.dataList;
            let s = 0;
            for (let i = 0; i < dataList.length; i++)
            {
                const step_end = (i === dataList.length - 1)
                    || dataList[i + 1].StepID !== dataList[s].StepID
                    || dataList[i + 1].CycleStatus !== dataList[s].CycleStatus;
                if (!step_end)
                    continue;

                if (PackDefine.StepChargeType(dataList[s].StepType) === STEP_CHARGE)
                    ccRatioMap.set(i, this.m_project.calcCCRatio(record.info.stepInfo, dataList, s, i));

                s = i + 1;
            }
        }

        const sheet_detail = workbook_add_worksheet(workbook, "Detail");
        row = 0;
        for (let i = 0; i < infoDetail.length; i++)
            worksheet_write_string(sheet_detail, row, i, infoDetail[i], fmt(i));
        row++;
        for (let i = 0; i < record.dataList.length; i++)
        {
            const step = record.dataList[i];

            const step_name = PackDefine.StepNameList[step.StepType];

            worksheet_write_number(sheet_detail, row, 0, i + 1, ifmt(0));
            worksheet_write_string(sheet_detail, row, 1, step_name, fmt(1));

            worksheet_write_number(sheet_detail, row, 2, step.StepChangeCount, ifmt(2));
            worksheet_write_number(sheet_detail, row, 3, step.CycleStatus, ifmt(3));
            worksheet_write_number(sheet_detail, row, 4, step.StepID, ifmt(4));
            worksheet_write_number(sheet_detail, row, 5, DataTrans.fromElectricUnit(step.Current), fmt(5));
            worksheet_write_number(sheet_detail, row, 6, DataTrans.fromVoltageUnit(step.Voltage), fmt(6));

            if (PackDefine.StepChargeType(step.StepType) === STEP_CHARGE)
            {
                worksheet_write_number(sheet_detail, row, 7, DataTrans.fromCapUnit(step.ChargeCap), fmt(7));
                worksheet_write_number(sheet_detail, row, 8, DataTrans.fromEngUnit(step.ChargeEng), fmt(8));
            }
            else if (PackDefine.StepChargeType(step.StepType) === STEP_DISCHARGE)
            {
                worksheet_write_number(sheet_detail, row, 7, DataTrans.fromCapUnit(step.DisChargeCap), fmt(7));
                worksheet_write_number(sheet_detail, row, 8, DataTrans.fromEngUnit(step.DisChargeEng), fmt(8));
            }

            worksheet_write_string(sheet_detail, row, 9, DataTrans.fromTimeUnit(step.TestTime), fmt(9));
            worksheet_write_string(sheet_detail, row, 10, step.ATime.toString(), fmt(10));

            worksheet_write_number(sheet_detail, row, 11, DataTrans.fromTempUnit(step.Tempature), fmt(11));

            if (ccRatioMap.has(i))
                worksheet_write_number(sheet_detail, row, 12, ccRatioMap.get(i), fmt(12));

            row++;
        }

        workbook_close(workbook);
    }

    // bool recordDeleteWarning();
    recordDeleteWarning()
    {
        if (QMessageBox.question(this, "", tr("是否删除数据"), QMessageBox.Yes | QMessageBox.No)
            === QMessageBox.Yes)
            return true;

        return false;
    }

    // void recordDelete(int beforeDay);
    recordDelete(beforeDay)
    {
        const currentDate = new Date();   // 原 QDateTime::currentDateTime()
        const deviceList = this.m_project.deviceList();

        for (let i = 0; i < deviceList.length; i++)
        {
            const deviceId = deviceList[i];
            const list = this.m_project.recordList(deviceId);   // const QMap<int, BTERecordInfo> &list
            const bte_idxs = [];

            for (const [key, info] of list)
            {
                if (daysBetween(info.recordTimeEnd, currentDate) > beforeDay)   // 原 recordTimeEnd.daysTo(currentDate)
                    bte_idxs.push(key);
            }

            this.m_project.deleteRecords(deviceId, bte_idxs);
        }
    }

    // ---- 分片 4 结束(对应 C++ BTEServer.cpp 1508..1759 行) ----

    // void onBTEItemChecked(bool check);
    //   C++ 里 item 来自 sender(); JS 侧由信号回调作为第 2 个参数传入。
    onBTEItemChecked(check, senderWidget)
    {
        if (!check)
            return;

        const item = senderWidget;                     // qobject_cast<BTEItemWidget*>(sender())
        const item_idx = this.m_itemList.indexOf(item);
        for (let i = 0; i < this.m_itemList.length; i++)
        {
            if (i !== item_idx)
                this.m_itemList[i].setCheck(false);
        }
    }

    // void onBTEItemSelectAll();
    onBTEItemSelectAll()
    {
        const data = this.m_project.deviceData(this.curDeviceString());
        for (let i = 0; i < this.m_itemList.length; i++)
        {
            if (i < data.enableItem)
                this.m_itemList[i].setCheck(true);
            else
                this.m_itemList[i].setCheck(false);
        }
    }

    // void initDeviceList();
    initDeviceList()
    {
        const pre_device = this.curDeviceString();

        this.ui.listWidget.blockSignals(true);
        this.ui.listWidget.clear();

        //在线
        for (const [key, it] of this.m_devices)
        {
            if (it.DeviceString !== "")
            {
                //add Item
                const item = new QListWidgetItem();
                item.setText(it.DeviceString + tr("(在线)"));
                item.setData(Qt.UserRole, key);
                item.setData(Qt.UserRole + 1, it.DeviceString);
                this.ui.listWidget.addItem(item);
            }
        }

        //离线
        const list = this.m_project.deviceList();
        for (let i = 0; i < list.length; i++)
        {
            if (this.deviceToNet(list[i]) < 0)
            {
                //add Item
                const item = new QListWidgetItem();
                item.setText(list[i] + tr("(离线)"));
                item.setData(Qt.UserRole, -1);
                item.setData(Qt.UserRole + 1, list[i]);
                this.ui.listWidget.addItem(item);
            }
        }

        //当前id
        if (pre_device !== "")
        {
            const item_num = this.ui.listWidget.count();
            for (let i = 0; i < item_num; i++)
            {
                const device_id = String(this.ui.listWidget.item(i).data(Qt.UserRole + 1));
                if (device_id === pre_device)
                    this.ui.listWidget.setCurrentRow(i);
            }
        }
        this.ui.listWidget.blockSignals(false);

        //查询页
        this.ui.boxDevice.clear();
        this.ui.boxDevice.addItems(this.m_project.deviceList());

        this.initDevice();
    }

    // ---------------------------------------------------------------------------
    // 原 ui_*.h 的 connectSlotsByName + on_xxx_clicked 自动连接 —— JS 侧要显式写。
    // 信号约定见 qtcompat.ts 的 installSignals: 真控件的 onClicked/onToggled/onCurrentIndexChanged
    // 等会转发进对应的 sig* 数组, 所以这里用 `w.sigClicked.push(cb)` 与原移植代码一致。
    // ---------------------------------------------------------------------------
    connectUi()
    {
        // 缺控件/缺信号的先跳过并报出来: 本移植的 UI XML 还没补齐(见 bteserverUi.ts)
        const wire = (name, sig, cb) =>
        {
            const w = this.ui[name];
            if (!w)
            {
                console.log("[connectUi] UI 里没有控件: " + name);
                return;
            }
            if (!w[sig])
            {
                console.log("[connectUi] " + name + " 上没有信号: " + sig);
                return;
            }
            w[sig].push(cb);
        };

        // 按钮(ui_*.h 靠命名约定自动连的)
        wire('btnShowNormal', 'sigClicked', () => this.on_btnShowNormal_clicked());
        wire('btnShowList', 'sigClicked', () => this.on_btnShowList_clicked());
        wire('btnShowSplitSetting', 'sigClicked', () => this.on_btnShowSplitSetting_clicked());
        wire('btnShowTest', 'sigClicked', () => this.on_btnShowTest_clicked());
        wire('btnSplitQuery', 'sigClicked', () => this.on_btnSplitQuery_clicked());
        wire('btnClearAlarm', 'sigClicked', () => this.on_btnClearAlarm_clicked());
        wire('btnQuery', 'sigClicked', () => this.on_btnQuery_clicked());
        wire('btnExportAll', 'sigClicked', () => this.on_btnExportAll_clicked());
        wire('btnDeleteSelect', 'sigClicked', () => this.on_btnDeleteSelect_clicked());
        wire('btnDeleteWeek', 'sigClicked', () => this.on_btnDeleteWeek_clicked());
        wire('btnDeleteMonth', 'sigClicked', () => this.on_btnDeleteMonth_clicked());
        wire('btnDeleteYear', 'sigClicked', () => this.on_btnDeleteYear_clicked());
        wire('btnDeleteDevice', 'sigClicked', () => this.on_btnDeleteDevice_clicked());
        wire('btnSaveSplit', 'sigClicked', () => this.on_btnSaveSplit_clicked());
        wire('btnCopySplitList', 'sigClicked', () => this.on_btnCopySplitList_clicked());
        wire('btnResetServer', 'sigClicked', () => this.on_btnResetServer_clicked());
        wire('btnClearSend', 'sigClicked', () => this.on_btnClearSend_clicked());
        wire('btnClearRecv', 'sigClicked', () => this.on_btnClearRecv_clicked());

        // 页签 / 列表
        wire('tabWidget', 'sigCurrentIndexChanged', () => this.on_tabWidget_currentChanged());
        wire('tabWidgetSplit', 'sigCurrentIndexChanged', () => this.on_tabWidgetSplit_currentChanged());
        // 列表的行变化: 框架侧信号名是 CurrentIndexChanged(Qt 的 currentRowChanged 在这套绑定里没有),
        // 槽名沿用移植过来的 on_listWidget_currentRowChanged
        wire('listWidget', 'sigCurrentIndexChanged', () => this.on_listWidget_currentRowChanged());

        // 菜单动作: 引擎还没把菜单/动作注册到 JS, actionChn/actionEng 只是替身(见 bteserverUi.ts)
        wire('actionChn', 'sigTriggered', () => this.on_actionChn_triggered());
        wire('actionEng', 'sigTriggered', () => this.on_actionEng_triggered());
    }

    // void initDevice();
    initDevice()
    {
        this.clearSplit();
        for (let i = 0; i < MAX_DEVICE_ITEM; i++)
        {
            this.m_itemList[i].clear();
            this.m_itemList[i].setCheck(false);
            this.m_itemList[i].hide();
        }

        //normal
        const cur_netId = this.curNetId();
        const deviceString = this.curDeviceString();
        if (deviceString === "")
        {
            //设备数据/列表变化后统一重算报警(可见按每台设备的显示配置判定,与当前选中无关)
            this.refreshAlarmState();
            return;
        }

        const deviceData = this.m_project.deviceData(deviceString);
        //clear
        const channelNum = deviceData.deviceInfo.channelNum();
        const list_colum = this.ui.tableList.columnCount();
        for (let i = 0; i < channelNum; i++)
        {
            this.m_itemList[i].clear();
            this.m_itemList[i].setVisible(true);
            this.m_itemList[i].setStatus(DeviceInfo.Status_OffLine);

            for (let j = 1; j < list_colum; j++)
                this.ui.tableList.item(i, j).setText("");

            //update status
            if (cur_netId > 0)
                this.setItemStatus(cur_netId, i, this.m_devices.get(cur_netId).DeviceStatus[i]);

            const ptr = this.m_project.currentRecord(deviceString, i);
            this.setItemData(i, ptr.info.rtData);
            this.ui.tableList.item(i, 1).setText(ptr.info.stepInfo.cellNo);
        }

        for (let i = 0; i < this.m_itemList.length; i++)
        {
            this.m_itemList[i].setSize(deviceData.itemSize);
            this.m_itemList[i].setEnabled(i < deviceData.enableItem);
            this.m_itemList[i].setVisible(i < deviceData.enableItem);
        }

        this.updateItemLayout();
        this.updateItemLastDisEng();
        this.updateItemAlarmClearFlags();
    }

    // void setItemData(int item_idx, const stru_BTS_CMD_SEND_RTDATA76 &pack);
    setItemData(item_idx, pack)
    {
        //
        this.m_itemList[item_idx].setData(pack);

        //list
        // getTitle(QString &title, QColor &title_color) -> JS 返回 { title, color }
        let title;
        let title_color;
        {
            const t = this.m_itemList[item_idx].getTitle();
            title = t.title;
            title_color = t.color;
        }

        const id = pack.StepID;
        const loopTime = pack.CycleStatus;
        const stepTime = DataTrans.fromTimeUnitSec(pack.TestTime);
        const current = DataTrans.fromElectricUnit(pack.Current);
        const voltage = DataTrans.fromVoltageUnit(pack.Voltage);
        const cap = DataTrans.fromCapUnit(pack.ChargeCap);
        const dis_cap = DataTrans.fromCapUnit(pack.DisChargeCap);
        const eng = DataTrans.fromEngUnit(pack.ChargeEng);
        const dis_eng = DataTrans.fromEngUnit(pack.DisChargeEng);
        const prec = 4;

        this.ui.tableList.item(item_idx, LST_STEP).setText(String(pack.StepID));
        this.ui.tableList.item(item_idx, LST_LOOP).setText(String(loopTime));
        this.ui.tableList.item(item_idx, LST_STATUS).setText(title);

        this.ui.tableList.item(item_idx, LST_TEMPATURE).setText(Number(DataTrans.fromTempUnit(pack.Tempature)).toFixed(1) + "℃");
        this.ui.tableList.item(item_idx, LST_VOLT).setText(voltage.toFixed(prec) + "V");
        this.ui.tableList.item(item_idx, LST_CURRENT).setText(current.toFixed(prec) + "A");

        this.ui.tableList.item(item_idx, LST_TIME).setText(stepTime);
        this.ui.tableList.item(item_idx, LST_CHARGE_CAP).setText(cap.toFixed(prec));
        this.ui.tableList.item(item_idx, LST_DISCHARGE_CAP).setText(dis_cap.toFixed(prec));

        this.ui.tableList.item(item_idx, LST_CHARGE_ENG).setText(eng.toFixed(prec));
        this.ui.tableList.item(item_idx, LST_DISCHARGE_ENG).setText(dis_eng.toFixed(prec));
    }

    // void setItemLastChargeCap(int item_idx, double ChargeCap);
    setItemLastChargeCap(item_idx, ChargeCap)
    {
        this.ui.tableList.item(item_idx, LST_END_CHARGE_CAP).setText(ChargeCap.toFixed(4) + "Ah");
    }

    // void setItemLastDisChargeCap(int item_idx, double disChargeCap);
    setItemLastDisChargeCap(item_idx, disChargeCap)
    {
        this.ui.tableList.item(item_idx, LST_END_DIS_CAP).setText(disChargeCap.toFixed(4) + "Ah");
    }

    // void setItemStatus(int netId, int item_idx, int status);
    setItemStatus(netId, item_idx, status)
    {
        console.assert(this.m_devices.has(netId));   // Q_ASSERT(m_devices.contains(netId));
        this.m_devices.get(netId).DeviceStatus[item_idx] = status;

        if (netId === this.curNetId())
        {
            const deviceId = this.m_devices.get(netId).DeviceString;
            this.m_itemList[item_idx].setStatus(status);
            if (status === DeviceInfo.Status_WaitStart)
            {
                this.m_itemList[item_idx].setData(new stru_BTS_CMD_SEND_RTDATA76());
            }
            else if (status === DeviceInfo.Status_Run)
            {
                const record = this.m_project.currentRecord(deviceId, item_idx);
                this.ui.tableList.item(item_idx, LST_CELL_NO).setText(record.info.stepInfo.cellNo);
            }

            // getTitle(QString &title, QColor &title_color) -> JS 返回 { title, color }
            const t = this.m_itemList[item_idx].getTitle();
            const title = t.title;
            const title_color = t.color;
            this.ui.tableList.item(item_idx, 4).setText(title);
        }
    }

    // void on_listWidget_currentRowChanged(int currentRow);
    on_listWidget_currentRowChanged(currentRow)
    {
        this.initDevice();
    }

    // void on_tabWidget_currentChanged(int index);
    on_tabWidget_currentChanged(index)
    {
        const isNormal = (index === 0);
        if (isNormal)
        {
            this.ui.stackedWidget.setCurrentWidget(this.ui.pageNormal);
        }
        else
        {
            this.ui.boxDevice.clear();
            this.ui.boxDevice.addItems(this.m_project.deviceList());

            this.ui.dateFrom.setDate(new Date());   // 原 QDate::currentDate()
            this.ui.dateTo.setDate(new Date());

            this.ui.stackedWidget.setCurrentWidget(this.ui.pageHistory);
        }

        this.ui.btnShowNormal.setVisible(isNormal);
        this.ui.btnShowList.setVisible(isNormal);
        this.ui.btnShowSplitSetting.setVisible(isNormal);
        this.ui.btnSplitQuery.setVisible(!isNormal);

        if (!HIDE_TEST_PAGE)
        {
            this.ui.btnShowTest.setVisible(isNormal);
        }
    }

    // void on_tabWidgetSplit_currentChanged(int current);
    on_tabWidgetSplit_currentChanged(current)
    {

    }

    // void on_btnShowNormal_clicked();
    on_btnShowNormal_clicked()
    {
        this.ui.stackedWidget.setCurrentWidget(this.ui.pageNormal);
    }

    // void on_btnShowList_clicked();
    on_btnShowList_clicked()
    {
        this.ui.stackedWidget.setCurrentWidget(this.ui.pageList);
    }

    // void on_btnShowSplitSetting_clicked();
    on_btnShowSplitSetting_clicked()
    {
        this.ui.stackedWidget.setCurrentWidget(this.ui.pageSplit);
        this.ui.tabWidgetSplit.setCurrentIndex(0);

        this.ui.splitSettingWidget.init();
    }

    // void on_btnShowTest_clicked();
    on_btnShowTest_clicked()
    {
        this.ui.stackedWidget.setCurrentWidget(this.ui.pageTest);
    }

    // ---- 分片 5 结束(对应 C++ BTEServer.cpp 1761..2031 行) ----

    // void onStepDataChannelChanged(int index);
    onStepDataChannelChanged(index)
    {
        if (this.m_stepDataDialog.isReal())
        {
            const DeviceString = this.m_stepDataDialog.deviceId();
            if (index >= 0 && index < 32)
            {
                const record = this.m_project.currentRecord(DeviceString, index);
                this.m_project.loadRecordData(DeviceString, record);
                this.m_stepDataDialog.init(true, DeviceString, index, record);
            }
        }
        else
        {
            if (index >= 0 && index < this.m_queryRecordList.length)
            {
                const query = this.m_queryRecordList[index];
                const record = this.m_project.historyRecord(query.deviceId, query.record_id);
                this.m_project.loadRecordData(query.deviceId, record);
                this.m_stepDataDialog.init(false, query.deviceId, index, record);
            }
        }
    }

    // void on_btnQuery_clicked();
    on_btnQuery_clicked()
    {
        this.ui.tableQuery.clearContents();

        const deviceId = this.ui.boxDevice.currentText();
        if (deviceId === "")
        {
            QMessageBox.information(this, "", tr("请先选择数据"));
            return;
        }

        //判断条件
        let query_unit = -1;
        let query_chn = -1;
        let unit_ok = false, chn_ok = false;

        const dayNear = this.ui.lineNearDay.value();
        // 日期选择控件(QDateTimeEdit)尚未移植: 其 dateTime() 现在返回空壳 -> asDate 归一成 null
        // (= 未设置, 不筛选); 控件就绪后返回 Date 即直接生效。原: to = to.addDays(1)
        const from = asDate(this.ui.dateFrom.dateTime());
        let to = asDate(this.ui.dateTo.dateTime());
        if (to)
            to = addDays(to, 1);
        const cur = new Date();   // 原 QDateTime::currentDateTime()

        // QLineEdit *edit, bool &ok -> JS 返回 { value, ok }
        const toInt = (edit) =>
        {
            const text = edit.text().trim();
            if (text === "")
            {
                return { value: 0, ok: true };
            }

            const n = parseInt(text, 10);
            let ok = !isNaN(n);          // text.toInt(&ok)
            ok = ok && n > 0;            // ok &= ret > 0
            return { value: isNaN(n) ? 0 : n, ok: ok };
        };

        let r = toInt(this.ui.lineQueryUnit);
        query_unit = r.value;
        unit_ok = r.ok;
        r = toInt(this.ui.lineQueryChannel);
        query_chn = r.value;
        chn_ok = r.ok;

        if (!unit_ok || !chn_ok)
        {
            QMessageBox.information(this, "", tr("查询条件不正确"));
            return;
        }
        query_unit -= 1;
        query_chn -= 1;

        let timeType = 0;
        if (this.ui.btnNone.isChecked())
            timeType = 0;
        else if (this.ui.btnNear.isChecked())
            timeType = 1;
        else
            timeType = 2;

        const creator = this.ui.lineCreator.text();
        const cellPatch = this.ui.lineCellPatch.text();
        const cellNum = this.ui.lineCellNum.text();

        //开始查询
        this.m_queryRecordList.length = 0;   // m_queryRecordList.clear();
        this.ui.tableQuery.clearContents();
        this.ui.tableQuery.setRowCount(0);

        const list = this.m_project.recordList(deviceId);   // const QMap<int, BTERecordInfo> &list
        const bte_idxs = [];

        for (const [i, info] of list)
        {
            // C++ 用 goto query_end 跳过本条; JS 用标签块 + break 等价实现
            query_end:
            {
                const item_idx = info.itemIndex;
                const unit = (item_idx / 8) | 0;
                const chn = item_idx % 8;

                //通道筛选
                const filter = false;
                if (query_unit !== -1 && unit !== query_unit)
                    break query_end;
                if (query_chn !== -1 && query_chn !== chn)
                    break query_end;

                //时间筛选
                //  原: recordTime.daysTo(cur) > dayNear  /  recordTime.lt(from) || recordTime.gt(to)
                //  isBefore/isAfter 对 null(未设置) 一律 false, 与 Qt 里无效 QDateTime 的比较一致
                if ((timeType === 1 && daysBetween(info.recordTime, cur) > dayNear)
                    || (timeType === 2 && (isBefore(info.recordTime, from) || isAfter(info.recordTime, to))))
                    break query_end;

                //其他信息筛选
                if (creator !== "" && info.stepInfo.creator.indexOf(creator) < 0)
                    break query_end;

                if (cellPatch !== "" && info.stepInfo.cellBatch.indexOf(cellPatch) < 0)
                    break query_end;

                if (cellNum !== "" && info.stepInfo.cellNo.indexOf(cellNum) < 0)
                    break query_end;

                bte_idxs.push(i);
            }
        }

        this.ui.tableQuery.setRowCount(bte_idxs.length);
        for (let i = 0; i < bte_idxs.length; i++)
        {
            const info = list.get(bte_idxs[i]);
            const item_idx = info.itemIndex;
            this.m_queryRecordList.push({ deviceId: deviceId, item_index: info.itemIndex, record_id: info.recordId });

            const row = i;
            const itemNO = new QTableWidgetItem(String(row + 1));
            const itemChan = new QTableWidgetItem();
            const itemTimeStrat = new QTableWidgetItem();
            const itemTimeEnd = new QTableWidgetItem();
            const itemRecordNum = new QTableWidgetItem();
            const itemCreator = new QTableWidgetItem();
            const itemCellNo = new QTableWidgetItem();
            const itemCellPatch = new QTableWidgetItem();
            const itemMemo = new QTableWidgetItem();

            itemChan.setText(sprintf("%d-%d", (item_idx / 8 | 0) + 1, item_idx % 8 + 1));
            itemTimeStrat.setText(formatDateTime(info.recordTime, "yyyy-MM-dd HH:mm:ss"));
            itemTimeEnd.setText(formatDateTime(info.recordTimeEnd, "yyyy-MM-dd HH:mm:ss"));
            itemRecordNum.setText(String(info.recordNum));
            itemCreator.setText(info.stepInfo.creator);
            itemCellNo.setText(info.stepInfo.cellNo);
            itemCellPatch.setText(info.stepInfo.cellBatch);
            itemMemo.setText(info.stepInfo.memo);

            const itemCheck = new QCheckBox(tr("选择"));
            itemCheck.setChecked(true);

            this.ui.tableQuery.setItem(row, 0, itemNO);
            this.ui.tableQuery.setItem(row, 1, itemChan);
            this.ui.tableQuery.setCellWidget(row, 2, itemCheck);
            this.ui.tableQuery.setItem(row, 3, itemTimeStrat);
            this.ui.tableQuery.setItem(row, 4, itemTimeEnd);
            this.ui.tableQuery.setItem(row, 5, itemRecordNum);

            this.ui.tableQuery.setItem(row, 6, itemCellNo);
            this.ui.tableQuery.setItem(row, 7, itemCellPatch);
            this.ui.tableQuery.setItem(row, 8, itemCreator);
            this.ui.tableQuery.setItem(row, 9, itemMemo);
        }

        if (this.ui.tableQuery.rowCount() === 0)
        {
            QMessageBox.information(this, "", tr("未找到相关数据"));
            return;
        }
    }

    // void on_btnExportAll_clicked();
    on_btnExportAll_clicked()
    {
        const path = QFileDialog.getExistingDirectory(this);
        if (path === "")
            return;

        for (let i = 0; i < this.ui.tableQuery.rowCount(); i++)
        {
            const query = this.m_queryRecordList[i];

            const record = this.m_project.historyRecord(query.deviceId, query.record_id);
            this.m_project.loadRecordData(query.deviceId, record);

            if (record.info.recordNum === 0)
                continue;

            const time_str = formatDateTime(record.info.recordTime, "yyyy-MM-dd hh：mm：ss");

            const item_str = String((record.info.itemIndex / 8 | 0) + 1);
            const chn_str = sprintf("%02d", record.info.itemIndex % 8 + 1);

            let device_id_str = query.deviceId;
            const device_ips = query.deviceId.split(".");
            if (device_ips.length === 4)
                device_id_str = device_ips.slice(2).join(".");   // device_ips.mid(2).join(".")

            const cellBatch = record.info.stepInfo.cellBatch;
            let filepath = path + "/" + time_str + "_" + device_id_str + "-" + item_str + "-" + chn_str;
            if (cellBatch !== "")
                filepath += "_" + cellBatch;
            filepath += ".xlsx";
            this.recordExportToFile(query.deviceId, record, filepath);
        }
    }

    // void onTableQueryMenu(const QPoint &pos);
    onTableQueryMenu(pos)
    {
        const item = this.ui.tableQuery.itemAt(pos);
        if (!item)
            return;
        const row = item.row();

        const menu = new QMenu();
        const actInfo = menu.addAction(tr("通道信息"));
        const actData = menu.addAction(tr("通道数据"));

        const sub = menu.addMenu(tr("导出数据"));
        const actExport = sub.addAction(tr("导出excel"));

        const act = menu.exec(this.ui.tableQuery.mapToGlobal(pos));
        if (act)
        {
            const query = this.m_queryRecordList[row];
            const record = this.m_project.historyRecord(query.deviceId, query.record_id);
            if (act === actInfo)
            {
                const dlg = new StepInfoDialog(this);
                dlg.record = record;
                dlg.exec();
            }
            else if (act === actData)
            {
                this.m_project.loadRecordData(query.deviceId, record);
                this.m_stepDataDialog.init(false, query.deviceId, row, record);
                this.m_stepDataDialog.show();
            }
            else if (act === actExport)
            {
                this.m_project.loadRecordData(query.deviceId, record);
                this.recordExport(query.deviceId, record);
            }
        }
    }

    // void on_btnDeleteSelect_clicked();
    on_btnDeleteSelect_clicked()
    {
        if (this.m_queryRecordList.length === 0)
            return;

        if (!this.recordDeleteWarning())
            return;

        const idxs = [];
        for (let i = 0; i < this.m_queryRecordList.length; i++)
            idxs.push(this.m_queryRecordList[i].record_id);

        this.m_project.deleteRecords(this.m_queryRecordList[0].deviceId, idxs);
        this.m_queryRecordList.length = 0;   // m_queryRecordList.clear();
        this.ui.tableQuery.clearContents();
        this.ui.tableQuery.setRowCount(0);
    }

    // void on_btnDeleteWeek_clicked();
    on_btnDeleteWeek_clicked()
    {
        if (!this.recordDeleteWarning())
            return;

        this.recordDelete(7);
        this.ui.tableQuery.clearContents();
    }

    // void on_btnDeleteMonth_clicked();
    on_btnDeleteMonth_clicked()
    {
        if (!this.recordDeleteWarning())
            return;

        this.recordDelete(30);
        this.ui.tableQuery.clearContents();
    }

    // void on_btnDeleteYear_clicked();
    on_btnDeleteYear_clicked()
    {
        if (!this.recordDeleteWarning())
            return;

        this.recordDelete(365);
        this.ui.tableQuery.clearContents();
    }

    // void on_btnDeleteDevice_clicked();
    on_btnDeleteDevice_clicked()
    {
        const deviceId = this.ui.boxDevice.currentText();
        if (deviceId === "")
            return;

        const netID = this.deviceToNet(deviceId);
        if (netID >= 0)
        {
            QMessageBox.information(this, "", tr("设备在线，无法删除"));
            return;
        }

        if (!this.recordDeleteWarning())
            return;

        this.ui.tableQuery.clearContents();
        this.m_project.deleteDevice(deviceId);
        this.initDeviceList();
    }

    // ---- 分片 6 结束(对应 C++ BTEServer.cpp 2033..2350 行) ----

    // void on_btnSplitQuery_clicked();
    on_btnSplitQuery_clicked()
    {
        //分选
        const count = this.ui.tableQuery.rowCount();
        const recordList = [];
        for (let i = 0; i < count; i++)
        {
            const box = this.ui.tableQuery.cellWidget(i, 2);   // (QCheckBox*)
            if (box.isChecked())
            {
                const query = this.m_queryRecordList[i];
                const ptr = this.m_project.historyRecord(query.deviceId, query.record_id);
                recordList.push(ptr);
            }
        }

        if (recordList.length === 0)
            return;

        const dlg = new SplitDialog(this);
        dlg.recordList = recordList;
        dlg.exec();
    }

    // void on_btnSaveSplit_clicked();
    on_btnSaveSplit_clicked()
    {
    }

    // void onSplitPrepareData();
    onSplitPrepareData()
    {
        //split
        const deviceId = this.curDeviceString();
        if (deviceId === "")
            return;

        const deviceData = this.m_project.deviceData(deviceId);
        this.ui.splitSettingWidget.recordList.length = 0;   // recordList.clear();
        for (let i = 0; i < MAX_DEVICE_ITEM; i++)
        {
            this.ui.splitSettingWidget.recordList.push(deviceData.recordList[i]);
        }
    }

    // void onSplitFinish();
    onSplitFinish()
    {
        const split_ret = this.ui.splitSettingWidget.result;
        if (split_ret.error)
        {
            const tips = sprintf(tr("分选失败 设备%02d-%02d 无有效放电数据"), (split_ret.erroNum / 8 | 0) + 1, split_ret.erroNum % 8 + 1);
            QMessageBox.information(this, "", tips);
            return;
        }

        this.ui.tabWidgetSplit.setCurrentIndex(1);
        this.ui.splitPie.setResult(split_ret);
        this.m_splitResult = split_ret;

        const range = this.ui.splitSettingWidget.range();
        this.ui.tableSplitLevel.clearContents();
        this.ui.tableSplitLevel.setRowCount(16);
        let row = 0;
        for (let i = 0; i < 16; i++)
        {
            if (i === 15 || range[i].vaild)
            {
                const name = (i < 15) ? String.fromCharCode('A'.charCodeAt(0) + i) : tr("落选");

                const itemNo = new QCheckBox(name);
                const itemNum = new QTableWidgetItem(String(split_ret.indexs[i].length));
                const itemLed = new QCheckBox(tr("点灯"));
                itemNo.setProperty("level", i);
                itemLed.setProperty("level", i);

                // C++ 里槽用 sender() 取控件, JS 侧由回调把控件传进去
                itemNo.sigClicked.push(() => this.onSplitItemNo(itemNo));
                itemLed.sigClicked.push(() => this.onSplitItemLed(itemLed));

                this.ui.tableSplitLevel.setCellWidget(row, 0, itemNo);
                this.ui.tableSplitLevel.setItem(row, 1, itemNum);
                this.ui.tableSplitLevel.setCellWidget(row, 2, itemLed);

                row++;
            }
        }
        this.ui.tableSplitLevel.setRowCount(row);
        this.ui.tableSplitLevel.cellWidget(0, 0).setChecked(true);

        this.onSplitItemNo();
    }

    // void onSplitItemNo();
    //   C++ 里 idx 来自 sender()->property("level"); JS 侧由信号回调传入控件。
    onSplitItemNo(senderWidget)
    {
        const idx = Number(senderWidget.property("level"));
        const items = [];
        const count = this.ui.tableSplitLevel.rowCount();
        for (let i = 0; i < count; i++)
        {
            const box = this.ui.tableSplitLevel.cellWidget(i, 0);   // (QCheckBox*)
            const level = Number(box.property("level"));
            if (box.isChecked())
                items.push(...this.m_splitResult.indexs[level]);   // items << m_splitResult.indexs[level];
        }

        //update table
        this.ui.tableSplitList.clearContents();
        this.ui.tableSplitList.setRowCount(items.length);
        for (let i = 0; i < items.length; i++)
        {
            const item_idx = items[i];
            const info = this.m_splitResult.recordList[item_idx];

            const itemNo = new QTableWidgetItem(String(i + 1));
            const itemPos = new QTableWidgetItem(String((item_idx / 8 | 0) + 1) + "-" + String(item_idx % 8 + 1));
            const itemCap = new QTableWidgetItem(String(info.DisChargeCap));
            const itemTime = new QTableWidgetItem(info.TimeText());
            const itemVolt = new QTableWidgetItem(String(info.MeanVoltage));
            const itemRatio = new QTableWidgetItem(Number(info.CcRatio).toFixed(2));
            const itemLevel = new QTableWidgetItem(info.LevelText());
            const itemCellNo = new QTableWidgetItem(info.CellNo);

            this.ui.tableSplitList.setItem(i, 0, itemNo);
            this.ui.tableSplitList.setItem(i, 1, itemPos);
            this.ui.tableSplitList.setItem(i, 2, itemCap);
            this.ui.tableSplitList.setItem(i, 3, itemTime);
            this.ui.tableSplitList.setItem(i, 4, itemVolt);
            this.ui.tableSplitList.setItem(i, 5, itemRatio);
            this.ui.tableSplitList.setItem(i, 6, itemLevel);
            this.ui.tableSplitList.setItem(i, 7, itemCellNo);
        }
        this.onSplitItemLed();
    }

    // void onSplitItemLed();
    //   C++ 里 idx 来自 sender()->property("level"); JS 侧由信号回调传入控件。
    onSplitItemLed(senderWidget)
    {
        const pack_led = new stru_BTS_CMD_LED_ON();
        pack_led.led.fill(0);   // memset(&pack_led.led, 0, sizeof(pack_led.led));

        const idx = Number(senderWidget.property("level"));
        const items = [];
        const count = this.ui.tableSplitLevel.rowCount();
        for (let i = 0; i < count; i++)
        {
            const boxNo = this.ui.tableSplitLevel.cellWidget(i, 0);   // (QCheckBox*)
            const box = this.ui.tableSplitLevel.cellWidget(i, 2);     // (QCheckBox*)
            const level = Number(box.property("level"));
            if (boxNo.isChecked() && box.isChecked())
                items.push(...this.m_splitResult.indexs[level]);
        }
        this.ui.splitResult.setChannels(items);

        for (let i = 0; i < items.length; i++)
        {
            const item_idx = items[i];
            const row = (item_idx / 8) | 0;
            const col = item_idx % 8;
            pack_led.led[row] |= 1 << col;
        }

        const netId = this.curNetId();
        if (netId > 0)
        {
            const data = new Uint8Array(0);   // QByteArray data;
            pack_led.pack(data);
            this.send(netId, BTS_CMD_LED_ON, data);
        }
    }

    // void on_btnCopySplitList_clicked();
    on_btnCopySplitList_clicked()
    {
        const count = this.ui.tableSplitList.rowCount();
        const text = [];   // QVector<QStringList> text
        const head = [];
        head.push(tr("序号"), tr("设备-单元-通道"), tr("放电容量(Ah)"), tr("放电时间（H:M:S:MS）"),
            tr("中值电压（V）"), tr("恒流比（%）"), tr("级别"), tr("电池批号"));

        text.push(head);
        for (let i = 0; i < count; i++)
        {
            const line = [];
            for (let j = 0; j < 8; j++)
            {
                line.push(this.ui.tableSplitList.item(i, j).text());
            }
            text.push(line);
        }
        ExportSplitResult(this, text);
    }

    // ---- 分片 7 结束(对应 C++ BTEServer.cpp 2352..2537 行) ----

    // bool ckeckConnect();
    ckeckConnect()
    {
        if (this.curNetId() === -1)
        {
            QMessageBox.information(this, "", tr("请先连接设备"));
            return false;
        }
        return true;
    }

    // void on_btnResetServer_clicked();
    on_btnResetServer_clicked()
    {
        this.m_devices.clear();
        this.ui.listWidget.clear();

        //所有设备已移除,无在线报错 item,停止报警音
        this.refreshAlarmState();
    }

    // void on_btnClearSend_clicked();
    on_btnClearSend_clicked()
    {
        this.ui.textSend.clear();
    }

    // void on_btnClearRecv_clicked();
    on_btnClearRecv_clicked()
    {
        this.ui.textRecv.clear();
    }

    // void clearSplit();
    clearSplit()
    {
        this.m_splitResult = new SplitResult();
        this.ui.tabWidgetSplit.setCurrentIndex(0);
        this.ui.splitSettingWidget.init();
        this.ui.splitResult.setChannels([]);   // setChannels(QVector<int>())
        this.ui.splitPie.clear();
        this.ui.tableSplitLevel.clearContents();
        this.ui.tableSplitLevel.setRowCount(0);
        this.ui.tableSplitList.clearContents();
        this.ui.tableSplitList.setRowCount(0);
    }

    // void updateItemLayout();
    updateItemLayout()
    {
        const size = new Array(MAX_DEVICE_ITEM);
        for (let i = 0; i < MAX_DEVICE_ITEM; i++)
        {
            this.m_itemList[i].show();
            size[i] = 1;
        }

        const deviceString = this.curDeviceString();
        const deviceData = this.m_project.deviceData(deviceString);
        for (const p of deviceData.parallelList)
        {
            size[p.channels[0]] = p.channels.length;
            for (let i = 1; i < p.channels.length; i++)
                size[p.channels[i]] = 0;
        }

        const layout = this.ui.itemWidgets.layout();   // (QGridLayout *)
        for (let row = 0; row < MAX_SUB_DEVICE; row++)
        {
            let col = 0;
            for (let i = 0; i < MAX_CHANNEL_NUM; i++)
            {
                const item_idx = row * 8 + i;
                if (size[item_idx] > 0 && this.m_itemList[item_idx].isEnabled())
                {
                    this.m_itemList[item_idx].setScale(size[item_idx]);
                    layout.addWidget(this.m_itemList[item_idx], row, col, 1, size[item_idx]);
                    col += size[item_idx];
                }
                else
                {
                    this.m_itemList[item_idx].hide();
                    this.m_itemList[item_idx].setCheck(false);
                }
            }
        }

        //显示范围变化(显示数量/并联合并/切换设备)后:故障通道已不在界面显示则停止报警
        this.refreshAlarmState();
    }

    // void updateItemLastDisEng();
    updateItemLastDisEng()
    {
        const deviceString = this.curDeviceString();
        const deviceData = this.m_project.deviceData(deviceString);

        const channelNum = deviceData.deviceInfo.channelNum();
        for (let item_idx = 0; item_idx < channelNum; item_idx++)
        {
            //最后一个放电工步
            const ptr = this.m_project.currentRecord(deviceString, item_idx);
            if (ptr.info.dataCache.endCap > 0)
                this.setItemLastChargeCap(item_idx, ptr.info.dataCache.endCap);
            if (ptr.info.dataCache.endDisCap > 0)
                this.setItemLastDisChargeCap(item_idx, ptr.info.dataCache.endDisCap);
        }
    }

    // void onHeartTime();
    onHeartTime()
    {
        const cur = new Date();   // 原 QDateTime::currentDateTime()

        //重发数据包
        for (const [key, it] of this.m_devices)
        {
            // 原 it.stepSendTime.secsTo(cur) >= 5 (未发过包时 Qt 返回 0, 不会触发重发)
            if (it.stepSendWait && secondsBetween(it.stepSendTime, cur) >= 5)
            {
                it.stepSendWait = false;
                this.sendStepPack(key);
            }

            //重新刷新状态
            if (it.stepPackList.length === 0)
            {
                for (let i = 0; i < it.DeviceStatus.length; i++)
                {
                    if (it.DeviceStatus[i] === DeviceInfo.Status_WaitStart
                        || it.DeviceStatus[i] === DeviceInfo.Status_WaitStop
                        || it.DeviceStatus[i] === DeviceInfo.Status_WaitJump)
                        this.setItemStatus(key, i, DeviceInfo.Status_Error);
                }
            }
        }

        //断线
        for (const [key, it] of this.m_devices)
        {
            // 原 it.heartTime.secsTo(cur) >= 60 (还没收到过心跳时 Qt 返回 0, 不会误判断线)
            if (secondsBetween(it.heartTime, cur) >= 60)
            {
                this.disconnect(key);
                break;
            }
        }
    }

    // void onAutoTime();
    onAutoTime()
    {
        this.m_project.autoSave();
    }

    // void onStepTime();
    onStepTime()
    {
        for (const [key] of this.m_devices)
        {
            this.sendStepPack(key);
        }
    }

    //该通道最近一帧是否处于故障状态(故障位/错误停止/保护码,与卡片标红判据一致)
    // bool isChannelFault(int item_idx) const;
    isChannelFault(item_idx)
    {
        const rt = this.m_itemList[item_idx].data();
        return (rt.WorkType & 0x80) !== 0
            || rt.workStatus() === stru_BTS_CMD_SEND_RTDATA76.WORK_ERROR_STOP
            || rt.PrtCode !== 0;
    }

    //是否存在报警:任意在线设备上存在"可见、报警标志置位且未被确认(清除)"的通道即报警。
    //在线:m_devices 中的设备(alarmFlag 只由实时帧置位,断线设备已整体移出 m_devices);
    //可见:该通道在设备自身的显示配置内——未超出 enableItem(该设备的显示数量),
    //    且未被并联合并隐藏。判据按每台设备的配置独立计算,不依赖界面当前选中哪台设备
    //    (切设备/无选中都不改变报警音);
    //确认:alarmClearFlag 置位(点击过"清除报警")的通道不报警,直到重新下发工步/接续复位。
    // bool hasAlarm() const;
    hasAlarm()
    {
        for (const [, device] of this.m_devices)
        {
            if (device.data)
            {
                for (let i = 0; i < device.alarmFlag.length; i++)
                {
                    if (!device.alarmFlag[i] || device.alarmClearFlag[i] || i >= device.data.enableItem)
                        continue;
                    //并联合并:主通道在界面上占位显示,其余成员被合并隐藏,不算可见
                    let hiddenByParallel = false;
                    for (const p of device.data.parallelList)
                    {
                        if (p.channels.indexOf(i) >= 0 && p.channels[0] !== i)
                        {
                            hiddenByParallel = true;
                            break;
                        }
                    }
                    if (!hiddenByParallel)
                        return true;
                }
            }
        }
        return false;
    }

    //报警状态重算:任一在线设备上有"在线+可见(该设备显示配置内)+报警标志置位且未确认"的通道就报警;
    //全部恢复正常/被确认(清除)/不可见后停止
    // void refreshAlarmState();
    refreshAlarmState()
    {
        if (this.hasAlarm())
            this.startAlarm();
        else
            this.stopAlarm();
    }

    //右下角"一键清除报警":清除全部设备全部 item 的报警标志并停止报警音
    // void on_btnClearAlarm_clicked();
    on_btnClearAlarm_clicked()
    {
        this.clearAlarmAll();
    }

    //一键清除:确认全部设备全部 item 的报警(置 alarmClearFlag,不再报警;重新下发工步/接续后恢复)
    // void clearAlarmAll();
    clearAlarmAll()
    {
        let clearCount = 0;
        for (const [, it] of this.m_devices)
        {
            for (let i = 0; i < it.alarmFlag.length; i++)
            {
                if (it.alarmFlag[i] && !it.alarmClearFlag[i])
                {
                    it.alarmClearFlag[i] = true;
                    clearCount++;
                }
            }
        }
        if (clearCount > 0)
            console.log("clear alarm all, clear count = %d", clearCount);
        this.updateItemAlarmClearFlags();
        this.stopAlarm();
    }

    //把当前显示设备的"报警确认(清除)"标志同步到卡片:故障卡片在确认(清除)后由红色改为橙色
    // void updateItemAlarmClearFlags();
    updateItemAlarmClearFlags()
    {
        const netId = this.curNetId();
        if (!this.m_devices.has(netId))
            return;

        const device = this.m_devices.get(netId);
        for (let i = 0; i < device.alarmClearFlag.length && i < this.m_itemList.length; i++)
            this.m_itemList[i].setAlarmCleared(device.alarmClearFlag[i]);
    }

    //开始报警:循环播放程序目录下的 alarm.wav
    // void startAlarm();
    startAlarm()
    {
        if (this.m_alarmOn)
            return;

        const path = qApp.applicationDirPath() + "/wav/alarm.wav";
        if (!QFile.exists(path))
        {
            console.error("alarm.wav not found: %s", path);
            return;
        }

        if (!this.m_alarm.start(path))
        {
            console.error("alarm.wav play failed: %s", path);
            return;
        }
        this.m_alarmOn = true;
        //报警期间右下角"清除报警"按钮高亮提示
        this.ui.btnClearAlarm.setStyleSheet("background-color: rgb(200, 0, 0); color: white;");


        console.log("channel fault reported, alarm started");
    }

    //停止报警:全部在线通道恢复正常或用户清除报警时调用
    // void stopAlarm();
    stopAlarm()
    {
        if (!this.m_alarmOn)
            return;

        this.m_alarm.stop();
        this.m_alarmOn = false;
        this.ui.btnClearAlarm.setStyleSheet("");

        console.log("alarm stopped by user");
    }

    // void showWaitResume(DeviceInfo &device);
    showWaitResume(device)
    {
        const deviceString = device.DeviceString;
        const netId = device.netId;
        if (this.m_waitResume.has(deviceString))
            return;

        this.m_waitResume.add(deviceString);
        // QTimer::singleShot(0, [this, deviceString]() { ... });
        setTimeout(() =>
        {
            const text = tr("中位机") + deviceString + tr("工步异常中断，是否接续");
            const ret = QMessageBox.question(this, "", text, QMessageBox.Yes | QMessageBox.No);
            const netId = this.deviceToNet(deviceString);
            if (netId === -1)	 //可能已经断线
            {
                this.m_waitResume.delete(deviceString);
                return;
            }

            if (ret === QMessageBox.No)
                g_project.saveRealRecords(deviceString);

            const confirm = new stru_BTS_CMD_RESUME_CONFIRM();
            confirm.result = (ret === QMessageBox.Yes) ? 1 : 2;
            this.m_devices.get(netId).stepPackList.push(confirm);   // m_devices[netId].stepPackList << StepRequestPtr(confirm);
            this.sendStepPack(netId);

            this.m_waitResume.delete(deviceString);
        }, 0);
    }

    // ---- 分片 8 结束(对应 C++ BTEServer.cpp 2539..2862 行, 即 BTEServer.cpp 全文) ----
}

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
// QString::asprintf / C 的 sprintf 在 JS 里的最小实现, 覆盖本文件用到的转换符。
export function sprintf(format, ...args)
{
    let i = 0;
    return String(format).replace(/%([#0 +\-]*)(\d+)?(\.\d+)?(hh|h|ll|l|z)?([diouxXeEfgGscp%])/g,
        (all, flags, width, prec, len, conv) =>
        {
            if (conv === '%')
                return '%';

            const v = args[i++];
            switch (conv)
            {
                case 'd': case 'i':
                    return pad(String(BigInt.asIntN(64, BigInt(Math.trunc(Number(v))))), flags, width);
                case 'u':
                    return pad(String(BigInt.asUintN(64, BigInt(Math.trunc(Number(v))))), flags, width);
                case 'o': return Number(v).toString(8);
                case 'x': case 'X':
                {
                    let s = Number(v).toString(16);
                    if (conv === 'X') s = s.toUpperCase();
                    if (flags.indexOf('#') >= 0) s = (conv === 'X' ? '0X' : '0x') + s;
                    return pad(s, flags, width);
                }
                case 'c': return String.fromCharCode(Number(v));
                case 's': return v === null || v === undefined ? '' : String(v);
                case 'e': case 'E': case 'f': case 'g': case 'G':
                    return Number(v).toFixed(prec !== undefined ? parseInt(prec.substr(1), 10) : 6);
                case 'p': return String(v);
                default: return all;
            }
        });
}

function pad(s, flags, width)
{
    if (!width)
        return s;
    const w = parseInt(width, 10);
    const c = (flags.indexOf('0') >= 0) ? '0' : ' ';
    return s.padStart(w, c);
}

// tr("...") 在 JS 侧由 Qt 的翻译机制接管; 先原样返回, 便于后续接入 .qm。
export function tr(text)
{
    return text;
}

// Qt 的 emit -> 遍历回调数组
export function emit(slots, ...args)
{
    for (let i = 0; i < slots.length; i++)
        slots[i](...args);
}

// ---------------------------------------------------------------------------
// C++ 里位于 BTEServer.cpp 901..960 行的两个"类外"定义, JS 侧集中放在类之后。
// ---------------------------------------------------------------------------

//根据状态，创建一个中位机通信队列
//template<class T>
//QVector<StepRequestPtr> createPackList(DeviceInfo &deviceInfo, QVector<int> channels[],
//    QVector<int> enableStatus, const T &base, std::function<void(int)> func = NULL)
//  JS 无模板/无拷贝构造: 用 base.constructor 新建 + Object.assign 代替 new T(base)。
export function createPackList(deviceInfo, channels, enableStatus, base, func)
{
    const list = [];
    //遍历列表
    for (let i = 0; i < MAX_SUB_DEVICE; i++)
    {
        if (channels[i].length === 0)
            continue;

        const pack = cloneStepRequest(base);   // StepRequestPtr pack = StepRequestPtr(new T(base));
        pack.SubDeviceId = i;

        let chn_num = 0;
        for (let chn = 0; chn < channels[i].length; chn++)
        {
            const item_idx = channels[i][chn];
            const chn_idx = item_idx % MAX_CHANNEL_NUM;
            if (enableStatus.indexOf(deviceInfo.DeviceStatus[item_idx]) >= 0)
            {
                pack.setChannel(chn_idx);
                if (func)
                    func(item_idx);

                chn_num++;
            }
        }

        if (chn_num > 0)
            list.push(pack);
    }
    return list;
}

// new T(base) 的等价: 同类型新对象 + 逐字段拷贝, 并把通道位图清空(与 C++ 紧接着 setChannel 的语义一致)。
function cloneStepRequest(base)
{
    const pack = new base.constructor();
    Object.assign(pack, base);
    if ('channelInfo' in pack)
        pack.channelInfo = [];
    if (pack.ChannelID !== undefined)
        pack.ChannelID = new Uint8Array(pack.ChannelID.length);
    return pack;
}

// class MyValidator : public QValidator
//   QRegExp -> JS RegExp(exactMatch 用 ^...$ 锚定 + test)
export class MyValidator
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor(parent, type)
    {
        const ipReg = "\\.((2(5[0-5]|[0-4]\\d))|[0-1]?\\d{1,2})";
        if (type === 0)
            this.reg = new RegExp("^192\\.168" + ipReg + ipReg + "$");
        else
            this.reg = new RegExp("^192\\.168.1" + ipReg + "$");

        // bool flag = reg.exactMatch("192.168.0.0");
    }

    // virtual State validate(QString &input, int &pos) const override;
    validate(input, pos)
    {
        if (!this.reg.test(input))
            return QValidator.State.Invalid;
        else
            return QValidator.State.Acceptable;
    }
}
