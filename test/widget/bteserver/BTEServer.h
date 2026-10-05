#ifndef BTESERVER_H_
#define BTESERVER_H_

// ---------------------------------------------------------------------------
// BTEServer.h —— 对应原版 bteserver/BTEServer/BTEServer.h(窗口类 + 设备条目结构)。
//
// 与原版的形状差异(明说, 不是漏搬):
//   * 原版 `class BTEServer : public QMainWindow`, 自己就是窗口、ui.setupUi(this);
//     这里引擎的 .ui 生成类(Ui::BTEServerUi)自己 new 出 JZMainWindow, 所以宿主是**一个
//     持有生成类的聚合结构**(名字沿用 BTEServer), 由调用方持有 —— 网络/协议回调(lambda)
//     长期引用它的地址, 不能按值返回(见下面 buildBTEServer 的注释)。
//   * 生成类名: 原版是 Ui::BTEServerClass(来自 .ui 里的 <class>), 引擎的 jz_uic 固定按
//     "<.ui 文件名>Ui" 生成 ⇒ 这里是 Ui::BTEServerUi(uic 不支持 <class>, 只能这样)。
//   * 原版 DeviceInfo 就定义在 BTEServer.h(文件作用域), 这里照搬; BTERecordInfo 原版在
//     BTERecordDataBase.h(跟 sqlite 库在一起), 骨架没有那个库, 先放在这里。
// ---------------------------------------------------------------------------

#include <string>
#include <vector>

#include "ui_BTEServer.h"    // jz_uic 从 BTEServer.ui 生成(见 CMake 的 jz_add_ui)
#include "BTEItemWidget.h"   // 通道卡片(原版 BTEItemWidget: 自绘 QWidget)
#include "PackDefine.h"      // 报文结构(原版 common/PackDefine.h)
#include "BTEProject.h"      // 工程/记录库(原版 BTEProject + BTERecordDataBase)
#include "AlarmPlayer.h"     // 报警音(原版 AlarmPlayer)
#include "Common.h"          // MAX_* + UiHelper(原版 common/Common.h)
#include "JZByteArray.h"
#include "JZDateTime.h"

#include <map>

class JZTcpServer;
class JZTcpSocket;
class JZTimer;

class JZMainWindow;
class JZMenuBar;
class JZListWidget;
class JZTabWidget;
class JZStackedWidget;
class JZLabel;
class JZTextEdit;
class JZTableWidget;
class JZPushButton;
class JZComboBox;
class JZLineEdit;
class JZDateEdit;
class JZRadioButton;
class JZSpinBox;
class JZDoubleSpinBox;
class JZCheckbox;

// ---------------------------------------------------------------------------
// DeviceInfo(照搬原版 BTEServer.h 的同名结构: 一台中位机在服务器侧的全部运行时状态)
// ---------------------------------------------------------------------------
struct DeviceInfo
{
    // 通道状态值(Status_*)在 Common.h 的 DeviceStatus 里 —— 卡片(BTEItemWidget)也要按它配色,
    // 所以只留一份, 取值与原版 BTEServer.h 的 Status_* 完全一致。

    int         netId = 0;
    uint8_t     deviceId = 0;          // 原版 DeviceID
    std::string deviceString;          // 原版 DeviceString("ip-电压V电流A")
    // 离线后**保留条目**(列表项改"(离线)"、data[0] 置 -1), 这样设备下拉框里还能查它的历史记录
    // —— 原版是从 m_devices 里摘掉、只留列表项; 骨架少了 sqlite, 拿这份列表当设备清单用。
    bool        online = true;
    int64_t     heartMs = 0;           // 最近一次心跳(原版 heartTime: QDateTime)
    stru_BTS_CMD_SEND_INFO info;       // SEND_INFO 带过来的量程/通道数
    int         channelNum = 0;        // 由 info.channelNum() 算(骨架按它决定亮几张卡)

    std::vector<int> status;           // 原版 DeviceStatus: 每通道的工作状态(Status_*), 长 MAX_DEVICE_ITEM
    std::vector<int> alarmFlag;        // 实时帧维护: 故障置位, 恢复复位(原版同名同义)
    std::vector<int> alarmClearFlag;   // "清除报警"确认: 确认后置位, 下发工步后复位(原版同名同义)

    // 工步(原版存在工程的 DeviceData 里: stepList + startStepIndex; 骨架先挂在设备上,
    // 等工程层搬完再挪 —— 语义一样: 每台设备一份, 改过一次下次打开就是它)
    StruSetStep76Ex stepTemplate;
    int             startStepIndex = 1;

    DeviceInfo() : status(MAX_DEVICE_ITEM, Status_Complete),
                   alarmFlag(MAX_DEVICE_ITEM, 0), alarmClearFlag(MAX_DEVICE_ITEM, 0) {}
};

// 每个连接一份会话(原版 NetDataManager::SessionInfo: socket + 待拆包的缓冲)
struct NetSession
{
    int         netId = 0;      // 原版 netId(NetServer 分配)
    JZByteArray buffer;         // 收包缓冲(拆帧未完成的残留)
    uint8_t     deviceId = 0;   // 原版 device.DeviceID
    std::string deviceString;   // 设备名(原版 sprintf("%s-%dV%dA"))
    bool        gotInfo = false;
    int         packCount = 0;  // 收过的帧数(骨架统计/自检用)
};

// BTERecordInfo 在 BTERecordDataBase.h 里(与原版同一个位置), 这里不再重复定义。

// ---------------------------------------------------------------------------
// BTEServer —— 界面宿主(对应原版 class BTEServer; 差异见文件头)。
// 生成类里已经有**类型化成员** + byId 表(与 JZUiLoader::Result::byId 同义), 所以这里
// 只是给常用句柄起个别名(少写一层 gen.), 方便后面照原版 BTEServer.cpp 一段段搬逻辑。
//
// 骨架里明确留 TODO 的地方: 语言切换的完整实现(多语言表)、HelpDialog、工步下发与工程
// (BTEProject / StepSettingDialog)、分选模板界面、报警音。
// (框选已搬: 见 BTERubberBand.h + BTEServer.cpp 的 initRubberBand / selectCardsInRect)
// ---------------------------------------------------------------------------
class BTERubberBand;   // 框选选择层(BTERubberBand.h)

struct BTEServer
{
    Ui::BTEServerUi gen;                 // 生成代码: root + 类型化成员 + byId

    JZMainWindow*    main = nullptr;     // = gen.main
    JZMenuBar*       menuBar = nullptr;  // = gen.main->menuBar()
    JZListWidget*    listWidget = nullptr;
    JZTabWidget*     tabWidget = nullptr;
    JZStackedWidget* stackedWidget = nullptr;
    JZLabel*         labelChannel = nullptr;

    JZComboBox*      boxDevice = nullptr;
    JZLineEdit*      lineQueryUnit = nullptr;
    JZDateEdit*      dateFrom = nullptr;
    JZDateEdit*      dateTo = nullptr;
    JZRadioButton*   btnNone = nullptr;
    JZRadioButton*   btnNear = nullptr;
    JZSpinBox*       lineNearDay = nullptr;
    JZTableWidget*   tableQuery = nullptr;      // pageHistory 里那张(原版同名)
    JZTableWidget*   tableList = nullptr;
    JZTableWidget*   tableSplitLevel = nullptr;
    JZTableWidget*   tableSplitList = nullptr;
    JZPushButton*    btnQuery = nullptr;
    JZPushButton*    btnExportAll = nullptr;
    JZPushButton*    btnDeleteSelect = nullptr;
    JZPushButton*    btnDeleteWeek = nullptr;
    JZPushButton*    btnDeleteMonth = nullptr;
    JZPushButton*    btnDeleteYear = nullptr;
    JZPushButton*    btnDeleteDevice = nullptr;
    JZRadioButton*   btnFrom = nullptr;
    JZLineEdit*      lineQueryChannel = nullptr;
    JZLineEdit*      lineCellNum = nullptr;
    JZLineEdit*      lineCellPatch = nullptr;
    JZLineEdit*      lineCreator = nullptr;

    JZDoubleSpinBox* spinSplitThreshold = nullptr;
    JZPushButton*    btnSplitQuery = nullptr;
    JZPushButton*    btnCopySplitList = nullptr;

    // 右栏底部的页切换按钮 + 一键清除报警(原版 widget_3 那一行)
    JZPushButton*    btnShowNormal = nullptr;
    JZPushButton*    btnShowList = nullptr;
    JZPushButton*    btnShowSplitSetting = nullptr;
    JZPushButton*    btnShowTest = nullptr;
    JZPushButton*    btnClearAlarm = nullptr;

    // 测试页(原版 pageTest): 收发双栏 + 记录开关 + 端口
    JZLineEdit*      linePort = nullptr;
    JZPushButton*    btnResetServer = nullptr;
    JZCheckbox*      boxRecordPack = nullptr;
    JZCheckbox*      boxShowRT = nullptr;
    JZCheckbox*      boxShowDF = nullptr;
    JZTextEdit*      textSend = nullptr;
    JZTextEdit*      textRecv = nullptr;
    JZPushButton*    btnClearSend = nullptr;
    JZPushButton*    btnClearRecv = nullptr;

    // 通道卡片网格(原版: ui.itemWidgets 容器 + QVector<BTEItemWidget*> m_itemList)
    JZWidget*                   itemWidgets = nullptr;
    std::vector<BTEItemWidget*> channelCards;   // 4x8 = MAX_DEVICE_ITEM 张

    // 框选选择层(原版: QWidget* m_selectWidget)。挂在哪、什么时候抓鼠标见 initRubberBand
    BTERubberBand*              rubber = nullptr;

    // 网络服务(原版: BTEServer 里的 m_server(ServerThread) + init(8001); 这里用引擎的 JZTcpServer)
    // 注: 堆上持有 —— 本结构会被返回/搬移, 而 JZObject 不可拷贝。
    JZTcpServer*     server = nullptr;
    int              serverPort = 0;      // 实际监听端口(传 0 时由系统挑)

    // ---- 下面这些在原版里是 BTEServer 的直接成员 / m_project(sqlite) ----
    std::vector<DeviceInfo>             devices;      // 原版 m_devices(QMap<netId, DeviceInfo>)
    std::map<JZTcpSocket*, NetSession>  sessions;     // 原版 NetDataManager 的会话表
    std::map<int, JZTcpSocket*>         byNetId;      // netId -> socket(下发命令要用; 原版 NetServer 内部)
    BTEProject                          project;      // 原版 m_project(设备清单 + 记录库)
    std::vector<int>                    queryHits;    // 原版 m_queryRecordList: 最近一次查询命中的**记录 id**
    std::map<int, int>                  splitLevels;  // 记录 id -> 分选级别(分选后填; 原版存在记录里)
    int                                 dispIndex = 0;   // 测试页发包序号(原版 m_dispIndex)

    // 报警音(原版 m_alarm: 收到通道故障后循环播放 alarm.wav, 直到手动清除)
    AlarmPlayer*                        alarm = nullptr;

    // 心跳定时(原版 m_heartTimer.start(1000) -> onHeartTime()): 超时未收到心跳就把设备标离线
    JZTimer*         heartTimer = nullptr;

    std::string      error;              // 建界面失败的原因(main 为 nullptr 时看它)

    // 右栏堆叠页下标(顺序与原版一致)
    enum Page { PageNormal = 0, PageList, PageHistory, PageSplit, PageTest };
};

// 建界面(Ui::BTEServerUi::setupUi)+ 接骨架级行为 + 起网络监听。失败时 main 为 nullptr(看 error)。
//
// ⚠ **由调用方持有 ui**: 网络/协议回调(lambda)会长期引用它的地址(devices / 列表 / 日志口),
//   所以这里不返回结构体(返回值语义会搬移/析构一份, 回调就悬垂了) —— 调用方给一个活到程序
//   结束的对象。这也是"骨架一开始用返回值 + 回调只捕获控件指针"的由来。
//
// serverPort: 监听端口, 默认就是原版的 8001。
// ⚠ 引擎的 JZTcpServer::listen() **不接受 0**(想"让系统挑端口"没有实现), 传 0 会监听失败。
// projectDir: 工程目录(里面放 project.dat + record/<设备>.db)。
//   默认**空** = exe 同目录下的 Data —— 对齐原版 BTEProject::init 的
//   `QApplication::applicationDirPath() + "/Data"`; 传值就按传的来。
//   ⚠ 默认值别写成相对路径: 相对路径是**当前工作目录**, 从别处敲命令行会把工程目录撒在
//     源码树里(踩过: 从仓库根启动时生成 <repo>/bte_project)。
void buildBTEServer(BTEServer& ui, int serverPort = 8001,
                    const std::string& projectDir = std::string());

// 工步下发: 按子设备分组组包下发(原版 createPackList 的骨架版), 返回发出的包数。
// 导出这个是给自检用的 —— 不弹对话框就能验证"分组 + 组包"这条链路。
// (弹对话框的入口 openStepSetting 只在 BTEServer.cpp 内部用, 不导出。)
int  submitStepSetting(BTEServer& ui, DeviceInfo& dev, const std::vector<int>& channels);

// 设备信息(SEND_INFO)到达: 建/更新设备条目 + 刷新设备列表 + 打日志。
// 与 socket 无关的那半单独拿出来, 自检可以直接调它验证"报文 -> 设备名 -> 列表"这条链路。
// ip: 对端地址(骨架暂时拿不到真 IP, 见 .cpp 里的 TODO(引擎))。
void applyDeviceInfo(BTEServer& ui, int netId, uint8_t deviceId, const stru_BTS_CMD_SEND_INFO& info,
                     const std::string& ip);

// ---------------------------------------------------------------------------
// 下面几个也是"与 socket 解耦的那半", 自检(main.cpp --selftest)直接调:
// ---------------------------------------------------------------------------
// 实时帧到达: 卡片数值 + tableList 那一行 + 报警标志(原版 setItemData + 报警判据)。
// itemIndex: 已经由 (SubDevID-1)*8 + (ChannelIDByNum-1) 算好的 0 基通道下标。
void applyRtData(BTEServer& ui, int netId, int itemIndex, const stru_BTS_CMD_SEND_RTDATA76& rt);

// 记录点(DF)帧到达: 攒进内存记录库(原版 m_project.record())。
// 返回本次入/更新的记录条数。
int applyDfData(BTEServer& ui, const std::string& deviceId, const stru_BTS_CMD_SEND_DFDATA76& df);

// 下发一条通道控制命令(原版 send()): 按 item_idx 拆成"每子设备一包"。
// channels: 0 基通道下标; cmd 只支持 START70 / STOP / RESUME / STEP_JUMP 四种。
// 返回实际发出的包数(找不到设备/没有勾选通道 = 0)。
int sendChannelCommand(BTEServer& ui, int netId, uint16_t cmd,
                       const std::vector<int>& channels, uint8_t stepId = 0);

// 一键清除报警(原版 clearAlarmAll): 全部设备未确认的报警都置为"已确认"(卡片转橙)。
int clearAlarmAll(BTEServer& ui);

// 查询(原版 on_btnQuery_clicked): 按左栏条件查内存记录库, 结果进 pageHistory 里的 tableQuery。
// 返回命中条数; 条件不合法返回 -1。
int queryRecords(BTEServer& ui);

// 删除: beforeDay > 0 时删"结束时间早于 now-beforeDay 天"的记录; deviceId 非空则只删该设备。
// 返回删掉的条数。
int deleteRecords(BTEServer& ui, int beforeDay, const std::string& deviceId);

// 分选(原版 SplitDialog 的简化版): 按放电容量给最近一次查询的结果分档, 填两表。
// 分档规则: 按容量升序, 相邻两个差值 > 阈值就另起一级。返回级别数。
int splitRecords(BTEServer& ui);

// 把查询结果导出成 CSV(原版 recordExport 导出的是 xls; 骨架先用 CSV, 少一个依赖)。
// 返回写出的文件数。
int exportRecords(BTEServer& ui, const std::string& dir);

#endif // BTESERVER_H_
