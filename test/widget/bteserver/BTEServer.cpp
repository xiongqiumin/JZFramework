// ---------------------------------------------------------------------------
// BTEServer.cpp —— 对应原版 bteserver/BTEServer/BTEServer.cpp。
//
// 界面由 BTEServer.ui 生成(见 ui_BTEServer.h), 这里只做两件事:
//   1) 把生成出来的控件归一下类(顺手接上骨架级行为);
//   2) 逻辑只做"能跑起来 + 证明各控件可用"这一层, 真逻辑(网络/协议/工程数据/分选)
//      后面按原版 BTEServer.cpp 一段段搬。
//
// 与 JZUiLoader 的关系: 生成代码 setupUi() 是 JZUiLoader::build() 的"编译期版本" ——
// 同一份 .ui、同一套方言表(JZUiParser), 只是把"运行期解析 XML 再 new 控件"换成了
// "构建期生成 C++ 代码"。运行期仍可按需用 JZUiLoader 动态加载其它 .ui。
// ---------------------------------------------------------------------------
#include "BTEServer.h"

#include "Common.h"      // MAX_* / UiHelper(原版 common/Common.h)
#include "DataTrans.h"   // 单位换算(原版 common/DataTrans.h)
#include "BTEProject.h"  // 工程 + 记录库(原版 BTEProject.h)
#include "HelpDialog.h"      // 菜单"关于"(原版同款; DeviceMacDialog 那条件在源码里是注释掉的, 没挂)
#include "JZMenu.h"
#include "JZAction.h"
#include "JZApplication.h"   // jzApp->applicationDirPath(): 工程目录默认放 exe 同级
#include "JZDialog.h"
#include "StepSettingDialog.h"   // 工步设置(原版同名对话框)
#include "JZCheckbox.h"
#include "JZComboBox.h"
#include "JZDateTime.h"
#include "JZFile.h"
#include "JZFileDialog.h"
#include "JZMessageBox.h"
#include "JZStd.h"
#include "JZTimer.h"
#include "JZTcpServer.h"
#include "JZTcpSocket.h"
#include "BTERubberBand.h"   // 框选选择层(原版 m_selectWidget 的等价物)
#include "JZKeyEvent.h"      // CtrlModifier: 框选按 Ctrl = 追加

#include <algorithm>
#include <cstdlib>
#include <stdio.h>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
// 原版 LogInfo(...) 的去处: **只打 stdout**(骨架常要跑无人值守的验证: 起窗口 + 抓输出)。
// ⚠ 原版主界面里没有日志框(那个 QTextEdit textLog 是 pageTest 里的 "Recv Log"), 所以这里不再
// 往某个界面控件里塞文本 —— 见 BTEServer.ui 开头的"与原版对齐"说明。
void logLine(const std::string& line)
{
    // ⚠ 不能裸 printf: 源码字面量是 UTF-8(CMake 设了 /source-charset:utf-8), 而中文 Windows 的
    // 控制台默认是 936(GBK) —— 直接打就是"缃戠粶"这种乱码。str_utf8_to_console() 按 stdout 的
    // 实际状态转(真控制台转 GBK / 重定向·管道·UTF-8 控制台原样), 与 JZLog 打印日志同一条规则。
    printf("%s\n", str_utf8_to_console(line).c_str());
    fflush(stdout);
}

// 取当前设备项的 netId(定义在下面, 这里先声明: 右键菜单要用)
int curNetId(JZListWidget* list);
// 按 netId 取设备(定义在网络段; 快捷键/清除报警等更早的地方也要用)
DeviceInfo* deviceOf(BTEServer& ui, int netId);
// 框选: 卡片报"被拖出自身"后由它铺开选择层 + 抓鼠标(定义在 initChannelGrid 之后)
void beginRubberSelect(BTEServer& ui, const JZPoint& originGlobal);
// 工步下发(卡片右键"工步设置..." -> StepSettingDialog -> 按子设备组包下发)
void openStepSetting(BTEServer& ui);
int  submitStepSetting(BTEServer& ui, DeviceInfo& dev, const std::vector<int>& channels);
// 设备列表重建(定义在下面; 网络回调要在建会话时就刷一次列表)
void rebuildDeviceList(BTEServer& ui);
// 下面三个在网络段(定义在右键菜单之后, 这里先声明: 菜单里的"改 IP"要发包)
DeviceInfo*  deviceOf(BTEServer& ui, int netId);

// ---------------------------------------------------------------------------
// 报警状态 / 卡片可见性 (定义放这里: rebuildDeviceList 与刷新报警都要用)
// ---------------------------------------------------------------------------

// 有没有"未确认的报警"(原版 hasAlarm): 任一**在线**设备上存在 alarmFlag 置位且未被确认的通道
bool anyAlarm(const BTEServer& ui)
{
    for (size_t i = 0; i < ui.devices.size(); ++i)
    {
        const DeviceInfo& d = ui.devices[i];
        if (!d.online)
            continue;

        for (int k = 0; k < MAX_DEVICE_ITEM; ++k)
        {
            if (d.alarmFlag[k] && !d.alarmClearFlag[k])
                return true;
        }
    }
    return false;
}

// "清除报警"按钮只在真有报警时露出来(没报警时它没意义, 底行按钮也够挤了)
void updateClearAlarmButton(BTEServer& ui)
{
    if (ui.btnClearAlarm)
        ui.btnClearAlarm->setVisible(anyAlarm(ui));
}

// 通道卡片的可见数量 —— 按**当前选中设备**的启用通道数(原版 DeviceData::enableItem)。
//   · 没设备 / 选中的是离线项 ⇒ 一张都不显示。原版构造里所有卡片就是 setVisible(false),
//     设备连上以后才按数量挨个显示 —— 骨架原先写死"先亮第一组", 于是没设备也满屏卡片。
//   · 设备在但没报通道数 ⇒ 按一组(MAX_CHANNEL_NUM)显示, 与 applyDeviceInfo 同一口径。
void updateChannelCardVisibility(BTEServer& ui)
{
    int show = 0;

    const DeviceInfo* d = deviceOf(ui, curNetId(ui.listWidget));
    if (d && d->online)
    {
        show = (d->channelNum > 0 && d->channelNum <= MAX_DEVICE_ITEM)
               ? d->channelNum : MAX_CHANNEL_NUM;
    }

    for (int i = 0; i < (int)ui.channelCards.size(); ++i)
        ui.channelCards[i]->setVisible(i < show);
}
JZTcpSocket* socketOf(BTEServer& ui, int netId);
void sendFrame(BTEServer& ui, JZTcpSocket* sock, const PackInfo& info,
               const JZByteArray& body);

// ---- 小工具 ----
int64_t nowMs()
{
    return JZDateTime::currentMSecsSinceEpoch();
}

// 把一条"行号 + 文本"追加到文本框尾部(骨架里到处在用: 日志/收发)
void appendLine(JZTextEdit* view, const std::string& line)
{
    if (!view)
        return;
    view->setText(view->text() + line + "\n");
}

// 当前选中的卡片(0 基通道下标); 没选返回空
std::vector<int> checkedChannels(BTEServer& ui)
{
    std::vector<int> out;
    for (size_t i = 0; i < ui.channelCards.size(); ++i)
    {
        if (ui.channelCards[i]->isVisible() && ui.channelCards[i]->isChecked())
            out.push_back((int)i);
    }
    return out;
}

// 通道下标 -> "单元-通道"(01-01 .. 04-08)
std::string channelTextOf(int itemIndex)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d-%02d",
             itemIndex / MAX_CHANNEL_NUM + 1, itemIndex % MAX_CHANNEL_NUM + 1);
    return buf;
}

// 按菜单项文本找 action(原版是 uic 的 ui.actionExit 这类句柄; 方言里 <action> 带 text,
// 所以这里按文本找 —— 与 JS 侧 bteserverUi.ts 的 findAction 同一思路)
JZAction* findAction(JZMenuBar* bar, const std::string& text)
{
    if (!bar)
        return nullptr;
    for (int i = 0; i < bar->menuCount(); ++i)
    {
        JZMenu* m = bar->menuAt(i);
        if (!m)
            continue;
        for (int a = 0; a < m->actionCount(); ++a)
        {
            JZAction* act = m->actionAt(a);
            if (act && act->text() == text)
                return act;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// 表格列头(原版 initTable(); 列名与顺序照抄, 见 BTEServer.cpp):
//   tableQuery      —— 历史记录(10 列)
//   tableList       —— 通道实时表(15 列), 原版预建 MAX_DEVICE_ITEM 行, 首列填 "01-01" 形式的单元-通道号
//   tableSplitLevel —— 分选级别(3 列)
//   tableSplitList  —— 分选明细(8 列)
// 注: 原版 tableQuery 还挂了右键菜单(onTableQueryMenu)—— 骨架先不做, 见文件头的 TODO。
// ---------------------------------------------------------------------------
void initTable(BTEServer& ui)
{
    // 原版: UiHelper::SetTable(ui.tableQuery, strListHeader)
    if (ui.tableQuery)
    {
        std::vector<std::string> header;
        header.push_back("记录号");   header.push_back("通道号");
        header.push_back("分选");     header.push_back("启动时间");
        header.push_back("结束时间"); header.push_back("记录条数");
        header.push_back("电池编号"); header.push_back("批号/电池型号");
        header.push_back("创建者");   header.push_back("备注");
        UiHelper::SetTable(ui.tableQuery, header);
    }

    if (ui.tableList)
    {
        std::vector<std::string> header;
        header.push_back("通道号");   header.push_back("电池编号");
        header.push_back("工步");     header.push_back("循环");
        header.push_back("工作状态"); header.push_back("温度");
        header.push_back("电压");     header.push_back("电流");
        header.push_back("时间");     header.push_back("实充容量");
        header.push_back("实放容量"); header.push_back("实充能量");
        header.push_back("实放能量"); header.push_back("终止充电容量");
        header.push_back("终止容量");
        UiHelper::SetTable(ui.tableList, header);

        // 原版: setRowCount(MAX_DEVICE_ITEM) 后逐行把首列填成 "单元-通道"(01-01 .. 04-08)
        ui.tableList->setRowCount(MAX_DEVICE_ITEM);
        for (int i = 0; i < MAX_DEVICE_ITEM; ++i)
            ui.tableList->setItemText(i, 0, channelTextOf(i));
    }

    if (ui.tableSplitLevel)
    {
        std::vector<std::string> header;
        header.push_back("级别"); header.push_back("电池个数"); header.push_back("点灯");
        UiHelper::SetTable(ui.tableSplitLevel, header);
    }

    if (ui.tableSplitList)
    {
        std::vector<std::string> header;
        header.push_back("序号");           header.push_back("单元-通道");
        header.push_back("放电容量(Ah)");   header.push_back("放电时间（H:M:S:MS）");
        header.push_back("中值电压（V）");  header.push_back("恒流比（%）");
        header.push_back("级别");           header.push_back("电池编号");
        UiHelper::SetTable(ui.tableSplitList, header);
    }
}

// ---------------------------------------------------------------------------
// 设备列表的右键菜单(原版 onListItemMenu())
//
// 原版行为(逐条对齐):
//   * 没有当前项 -> 不弹; 当前项 netId < 0(离线) -> **不弹菜单**(这就是"没有在线设备时
//     右键什么都不出来"的由来 ⇒ 必须用 CustomContextMenu 自己判断后弹, 不能用
//     ActionsContextMenu —— 那种策略是框架无条件弹的);
//   * 菜单里只有一项"更改中位机IP"(原版的"写PC服务器IP / 写中位机号与MAC号"两条在
//     源码里就是被 /* */ 注掉的, 连带用到 DeviceMacDialog 的那段 —— 所以 DeviceMacDialog
//     这个文件在原版当前版本里其实**没被启用**, 我们保留文件但不挂菜单);
//   * 选中后弹一个"请输入IP"的临时输入框(原版是 lambda 里现搭的 QDialog + MyValidator),
//     校验必须 4 段, 取第 3、4 段填成 2 字节, 发 WRITE_DEVICE_IP。
// ---------------------------------------------------------------------------
JZDialog* makeIpDialog(BTEServer& ui, JZLineEdit** outLine, const std::string& initIp)
{
    JZDialog* dlg = new JZDialog(ui.main);
    dlg->setTitle("更改中位机IP");

    JZVBoxLayout* vbox = new JZVBoxLayout(8, 8);
    dlg->setLayout(vbox);

    JZLabel* tip = new JZLabel(dlg);
    tip->setText("请输入IP");
    vbox->addWidget(tip, 0);

    JZLineEdit* line = new JZLineEdit(dlg);
    line->setText(initIp);
    vbox->addWidget(line, 0);

    JZWidget* row = new JZWidget(dlg);
    JZHBoxLayout* hbox = new JZHBoxLayout(8, 8);
    JZPushButton* ok = new JZPushButton("确定", row);
    JZPushButton* cancel = new JZPushButton("取消", row);
    hbox->addWidget(ok, 0);
    hbox->addWidget(cancel, 0);
    row->setLayout(hbox);
    vbox->addWidget(row, 0);

    connect(&ok->sigClicked, [dlg]() { dlg->accept(); });
    connect(&cancel->sigClicked, [dlg]() { dlg->reject(); });

    *outLine = line;
    return dlg;
}

// 发一条"不属于某个子设备"的命令(原版 send(netId, cmd, data, SubDevID = INVAILD_SUB_ID))
void sendDevicePack(BTEServer& ui, int netId, uint16_t cmd, const JZByteArray& body)
{
    JZTcpSocket* sock = socketOf(ui, netId);
    DeviceInfo* dev = deviceOf(ui, netId);
    if (!sock || !dev)
        return;

    PackInfo info;
    info.DeviceID = dev->deviceId;
    info.SubDevID = (uint8_t)INVAILD_SUB_ID;   // 原版: 默认子设备号原样发(0xFE)
    info.CmdID = cmd;
    sendFrame(ui, sock, info, body);
}

void initDeviceListMenu(BTEServer& ui)
{
    if (!ui.listWidget)
        return;

    // 菜单挂到窗口下(所有权跟着窗口走): 不 setContextMenu(dlg 是借用语义), 由窗口持有即可
    JZMenu* menu = new JZMenu(ui.main);
    menu->addAction("更改中位机IP", [&ui]() {
        const int netId = curNetId(ui.listWidget);
        DeviceInfo* dev = deviceOf(ui, netId);
        if (!dev)
            return;

        // 设备名形如 "127.0.0.1-5000V10A" ⇒ 取 '-' 前面那段当现有 IP(原版: deviceId.split("-")[0])
        const std::string deviceString = dev->deviceString;
        const size_t dash = deviceString.find('-');
        const std::string ip = (dash == std::string::npos) ? deviceString
                                                          : deviceString.substr(0, dash);

        JZLineEdit* line = nullptr;
        JZDialog* dlg = makeIpDialog(ui, &line, ip);
        const int rc = dlg->exec();
        const std::string text = line ? line->text() : std::string();
        dlg->deleteLater();     // 模态循环里不立刻析构

        if (rc != JZDialog::Accepted || text.empty())
            return;

        // 4 段校验 + 取第 3、4 段(原版: list[2] / list[3], 每段一个字节)
        std::vector<std::string> parts;
        std::string part;
        for (size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '.') { parts.push_back(part); part.clear(); }
            else part += text[i];
        }
        parts.push_back(part);

        if (parts.size() != 4)
        {
            JZMessageBox* box = JZMessageBox::information(ui.main, "", "IP 不合法，请检查");
            box->exec();
            box->deleteLater();
            return;
        }

        ByteWriter w;
        w.u8((uint8_t)atoi(parts[2].c_str()));
        w.u8((uint8_t)atoi(parts[3].c_str()));
        sendDevicePack(ui, netId, WRITE_DEVICE_IP, w.data());
        logLine("[右键] 更改中位机IP -> " + text + "(已发 WRITE_DEVICE_IP)");
    });

    ui.listWidget->setContextMenuPolicy(JZWidget::CustomContextMenu);
    connect(&ui.listWidget->sigCustomContextMenuRequested, [&ui, menu](int x, int y) {
        // 原版: 没有当前项 / 当前项已离线(netId < 0) -> 不弹菜单
        if (curNetId(ui.listWidget) < 0)
            return;

        const JZPoint at = ui.listWidget->mapTo(ui.main, JZPoint(x, y));
        menu->exec(ui.main, JZRect(at.x(), at.y(), 1, 1));
    });
}

// ---------------------------------------------------------------------------
// 通道卡片的右键菜单(原版 onBTEItemMenu(): 目标通道 = 当前卡片; 若当前卡片是勾选态,
// 则对**所有勾选卡片**一起操作)。菜单由框架按右键位置自己弹(ActionsContextMenu),
// 所以每张卡片各挂一份(回调里认得是哪张卡)。
// ---------------------------------------------------------------------------
void initChannelCardMenu(BTEServer& ui, BTEItemWidget* card)
{
    JZMenu* menu = new JZMenu(card);

    // 目标通道: 卡片处于勾选态 -> 全部勾选的卡; 否则就这张
    auto targets = [&ui, card]() -> std::vector<int> {
        std::vector<int> all = checkedChannels(ui);
        if (card->isChecked() || all.empty())
        {
            if (all.empty() && card->itemIndex() >= 0)
                all.push_back(card->itemIndex());
            return all;
        }
        return all;
    };

    auto runCmd = [&ui, targets](uint16_t cmd, const char* what) {
        const int netId = curNetId(ui.listWidget);
        if (netId < 0)
        {
            logLine(std::string("[通道] ") + what + ": 没有选中设备");
            return;
        }
        const std::vector<int> chn = targets();
        if (chn.empty())
        {
            logLine("[通道] 没有目标通道");
            return;
        }
        const int n = sendChannelCommand(ui, netId, cmd, chn);
        logLine(std::string("[通道] ") + what + " -> " + std::to_string(n) + " 包(通道 "
                             + std::to_string(chn.size()) + " 个)");
    };

    menu->addAction("启动", [runCmd]() { runCmd(BTS_CMD_START70, "启动"); });
    menu->addAction("停止", [runCmd]() { runCmd(BTS_CMD_STOP, "停止"); });
    menu->addAction("接续", [runCmd]() { runCmd(BTS_CMD_RESUME, "接续"); });
    menu->addAction("工步跳转", [runCmd]() { runCmd(BTS_CMD_STEP_JUMP, "工步跳转"); });
    // 原版 onBTEItemMenu 里的"工步设置": 弹 StepSettingDialog, 确定后按选中通道下发
    menu->addAction("工步设置...", [&ui]() { openStepSetting(ui); });
    menu->addAction("清除报警", [&ui]() {
        const int n = clearAlarmAll(ui);
        logLine("[报警] 一键清除 " + std::to_string(n) + " 条");
    });
    menu->addAction("全选", [&ui]() {
        for (size_t i = 0; i < ui.channelCards.size(); ++i)
            if (ui.channelCards[i]->isVisible())
                ui.channelCards[i]->setCheck(true);
    });
    menu->addAction("取消全选", [&ui]() {
        for (size_t i = 0; i < ui.channelCards.size(); ++i)
            ui.channelCards[i]->setCheck(false);
    });

    card->setContextMenu(menu);
    card->setContextMenuPolicy(JZWidget::ActionsContextMenu);
}

// ---------------------------------------------------------------------------
// 通道卡片网格(原版 BTEServer.cpp :119-140: QGridLayout + MAX_SUB_DEVICE*MAX_CHANNEL_NUM 个
// BTEItemWidget, addWidget(item, row, col)、setVisible(false)、setChannelText、setItemIndex)。
//
// 骨架差异(明说): 原版一开始把所有卡片藏起来, 等设备连上按 enableItem 逐个显示; 这里先亮
// 第一组(一行 8 张)好让界面能看。卡片上的数值由实时帧(RTDATA76)填, 见 applyRtData()。
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 工步下发(原版 onBTEItemMenu 里"工步设置"那一段: StepSettingDialog -> createPackList ->
// sendStepPack 入队发送)
//
// 与原版的差异(明说):
//   · 原版工步存在**工程**的 DeviceData 里(stepList / startStepIndex), 骨架先挂在 DeviceInfo
//     上(见 BTEServer.h), 语义一样: 每台设备一份, 改过之后下次打开还是它;
//   · 原版按子设备组包后**入队 + 节流串行发**(stepPackList / sendStepPack, 靠回执出队);
//     骨架直接按子设备各发一帧 —— 工步包才几百字节, 不需要节流。
// 分组口径与原版 createPackList 一致: 每 MAX_CHANNEL_NUM(8) 路是一个子设备,
// 组内在包里的通道位图 = idx % 8(低位起)。
// ---------------------------------------------------------------------------
// 内部实现(匿名命名空间里: 只在本 TU 用); 对外的 submitStepSetting 是文件后半段的薄壳,
// 方便自检跨 TU 调 —— 见 BTEServer.h 的声明。
int submitStepSettingInternal(BTEServer& ui, DeviceInfo& dev, const std::vector<int>& channels)
{
    std::vector<int> bySub[MAX_SUB_DEVICE];
    for (size_t i = 0; i < channels.size(); ++i)
    {
        const int idx = channels[i];
        if (idx < 0 || idx >= MAX_DEVICE_ITEM)
            continue;
        bySub[idx / MAX_CHANNEL_NUM].push_back(idx);
    }

    JZTcpSocket* sock = socketOf(ui, dev.netId);
    int sent = 0;

    for (int sub = 0; sub < MAX_SUB_DEVICE; ++sub)
    {
        if (bySub[sub].empty())
            continue;

        StruSetStep76Ex one = dev.stepTemplate;      // 同一份工步, 只换通道位图
        one.clearChannel();
        for (size_t k = 0; k < bySub[sub].size(); ++k)
            one.setChannel(bySub[sub][k] % MAX_CHANNEL_NUM);

        PackInfo info;
        info.DeviceID = dev.deviceId;
        info.SubDevID = (uint8_t)sub;
        info.CmdID    = BTS_CMD_SET_STEP76_Ex;
        info.Result   = 0;

        sendFrame(ui, sock, info, one.pack());       // sendFrame 里自带记录 + 日志
        ++sent;
    }

    // 下发工步 = 新一轮测试: 报警确认标志复位(原版下发后同样要复位, 否则上一轮的"已确认"会带过来)
    for (int k = 0; k < MAX_DEVICE_ITEM; ++k)
        dev.alarmClearFlag[k] = 0;

    return sent;
}

void openStepSetting(BTEServer& ui)
{
    const std::vector<int> channels = checkedChannels(ui);
    if (channels.empty())
    {
        logLine("[工步] 没选中通道(先点选或框选卡片)");
        return;
    }

    DeviceInfo* dev = deviceOf(ui, curNetId(ui.listWidget));
    if (!dev || !dev->online)
    {
        logLine("[工步] 设备不在线, 不能下发");
        return;
    }

    if (dev->stepTemplate.stepList.empty())
        dev->stepTemplate.genDefaultStep();          // 还没有工步 ⇒ 默认模板(原版取工程里的 stepList)

    StepSettingDialog dlg(ui.main);
    dlg.startStepIndex = dev->startStepIndex;
    dlg.setStep(dev->stepTemplate);                  // 回填界面
    if (dlg.exec() != JZDialog::Accepted)
        return;                                      // 取消: 什么都不改

    // 界面值 -> 工步(uiToStep 在 accept 之前已经跑过), 记回设备
    dev->stepTemplate = dlg.step();
    dev->startStepIndex = dlg.startStepIndex;

    const int n = submitStepSettingInternal(ui, *dev, channels);

    logLine("[工步] 已下发 " + std::to_string(n) + " 个子设备包; 通道 "
            + std::to_string(channels.size()) + " 个, 起始工步 "
            + std::to_string(dlg.startStepIndex) + ", 创建者 " + dlg.creator
            + ", 批号 " + dlg.cellBatch);
}

// ---------------------------------------------------------------------------
// 快捷键(原版 BTEServer::initShortCuts, 构造函数 :142 调用):
//     QShortcut *selectAll = new QShortcut(QKeySequence("Ctrl+A"), this);
//     connect(selectAll, SIGNAL(activated()), this, SLOT(onBTEItemSelectAll()));
// 引擎的等价物 = **动作快捷键分发**: JZAction 设 shortcut + addAction 到窗口, 按键时由
// JZWindow::dispatchShortcut 匹配并 trigger()(见 JZWindow.cpp / JZKeySequence)。
// 它排在"焦点控件链上没人处理该按键"之后, 所以输入框里的 Ctrl+A(全选文本)照旧归输入框
// —— 与 Qt 用 ShortcutOverride 达到的效果一致。
// ---------------------------------------------------------------------------
void initShortCuts(BTEServer& ui)
{
    if (!ui.main)
        return;

    JZAction* selectAll = new JZAction(ui.main);     // 挂在窗口下: 随窗口一起析构
    selectAll->setObjectName("actSelectAllCards");   // 自动化(MCP ui_invoke 按 #viewId 找)用
    selectAll->setShortcut("Ctrl+A");

    connect(&selectAll->triggered, [&ui]() {
        // 原版 onBTEItemSelectAll(): 该设备**启用的通道**全选, 其余取消勾选
        const DeviceInfo* d = deviceOf(ui, curNetId(ui.listWidget));
        const int enable = (d && d->channelNum > 0 && d->channelNum <= MAX_DEVICE_ITEM)
            ? d->channelNum : MAX_CHANNEL_NUM;

        for (int i = 0; i < (int)ui.channelCards.size(); ++i)
            ui.channelCards[i]->setCheck(i < enable);

        logLine("[快捷键] Ctrl+A: 已全选 " + std::to_string(enable) + " 个通道");
    });

    ui.main->addAction(selectAll);
}

void initChannelGrid(BTEServer& ui)
{
    if (!ui.itemWidgets)
        return;

    JZGridLayout* grid = new JZGridLayout();
    grid->setSpacing(3);                        // 原版: layout->setSpacing(3)
    grid->setContentsMargins(3, 3, 3, 3);       // 原版: layout->setMargin(3)
    // 原版: setColumnStretch(8,1)/setRowStretch(8,1) —— 网格右下角的富余空间归第 8 行/列,
    // 卡片本身是固定尺寸, 不会被拉伸。
    grid->setColumnStretch(MAX_CHANNEL_NUM, 1);
    grid->setRowStretch(MAX_SUB_DEVICE, 1);

    ui.channelCards.reserve(MAX_DEVICE_ITEM);
    for (int i = 0; i < MAX_DEVICE_ITEM; ++i)
    {
        BTEItemWidget* card = new BTEItemWidget();

        const int row = i / MAX_CHANNEL_NUM;
        const int col = i % MAX_CHANNEL_NUM;

        card->setItemIndex(i);
        // 原版: setChannelText(QString::asprintf("%d - %d", row + 1, col + 1))
        char chbuf[16];
        snprintf(chbuf, sizeof(chbuf), "%d - %d", row + 1, col + 1);
        card->setChannelText(chbuf);
        // 原版构造: 先全部藏起来, 等设备连上按它的通道数逐个显示(见 updateChannelCardVisibility)。
        // 骨架原来写死"先亮第一组", 于是没设备也满屏卡片 —— 那是假的。
        card->setVisible(false);

        grid->addWidget(card, row, col);

        // 单击 = **单选**(原版 onBTEItemChecked: 只在"变选中"时清掉其它卡片)。
        // 带 Ctrl 时卡片自己取反、不发信号, 所以不会走到这里; 框选也不发信号(见
        // selectCardsInRect), 于是框选出来的多选不会被这里冲掉 —— 与原版同一套分工。
        connect(&card->sigToggled, [&ui, card](bool on) {
            if (on)
            {
                for (size_t i = 0; i < ui.channelCards.size(); ++i)
                {
                    if (ui.channelCards[i] != card)
                        ui.channelCards[i]->setCheck(false);
                }
            }

            logLine("[卡片] " + card->channelText() + (on ? " 选中(单选, 已清其它)" : " 取消选中"));
        });

        initChannelCardMenu(ui, card);      // 原版: onBTEItemMenu(启动/停止/接续/跳步...)

        // 框选: 卡片被拖出自身 40px 就交给选择层(原版是主窗口的 mouseMove 干的; 引擎里
        // 鼠标事件不冒泡, 所以改由卡片上报按下点 —— 见 BTEItemWidget::sigRubberStart)
        connect(&card->sigRubberStart, [&ui](JZPoint originGlobal) {
            beginRubberSelect(ui, originGlobal);
        });

        ui.channelCards.push_back(card);
    }

    ui.itemWidgets->setLayout(grid);

    updateChannelCardVisibility(ui);   // 建完先按当前设备刷一遍(没设备 ⇒ 全隐藏)
}

// ---------------------------------------------------------------------------
// 框选 (原版 BTEServer::mousePress/Move/ReleaseEvent 里的 m_selectWidget 那套)
//
// 引擎的鼠标事件**不沿父链冒泡**, 也没有"父窗口接管子控件拖拽"的机制, 所以链路改成:
//   卡片被拖出 40px → sigRubberStart(按下点屏幕坐标)
//     → beginRubberSelect(): 选择层铺满卡片区域 + 显示 + **窗口 grabMouse**
//       → BTERubberBand 自绘选择框(2px 浅黄边, 原版样式)
//         → 松开 → sigSelected(modifiers) → selectCardsInRect() 判命中 → 解除抓取
// 命中判定与原版一致(卡片 mapTo 主窗口后 intersects), Ctrl = 追加, 不按 = 先清空其它。
// ---------------------------------------------------------------------------

// 把选择框内的可见卡片勾上(原版 mouseReleaseEvent 里的循环)。append = 松开时按着 Ctrl。
void selectCardsInRect(BTEServer& ui, const JZRect& sel, bool append)
{
    if (!ui.main)
        return;

    bool first = true;
    int  hits = 0;

    for (size_t i = 0; i < ui.channelCards.size(); ++i)
    {
        BTEItemWidget* card = ui.channelCards[i];
        if (!card || !card->isVisible())
            continue;                          // 原版: 跳过隐藏的卡片

        const JZRect r = card->mapTo(ui.main, JZRect(0, 0, card->rect().width(), card->rect().height()));
        if (!sel.intersects(r))
            continue;

        if (first && !append)
        {
            // 不按 Ctrl = 替换语义: 先清掉其它卡片(原版把 m_itemList 里别的全 setCheck(false))
            for (size_t j = 0; j < ui.channelCards.size(); ++j)
            {
                if (ui.channelCards[j] != card)
                    ui.channelCards[j]->setCheck(false);
            }
        }

        card->setCheck(true);   // 与原版一样**不发 sigToggled** —— 框选只改多选状态, 不触发单选逻辑
        ++hits;
        first = false;
    }

    logLine("[框选] 命中 " + std::to_string(hits) + " 张卡片" + (append ? " (Ctrl 追加)" : " (替换)"));
}

// 开始一次框选(卡片报告"被拖出自身"时调用)
void beginRubberSelect(BTEServer& ui, const JZPoint& originGlobal)
{
    if (!ui.rubber || !ui.main || !ui.itemWidgets || ui.rubber->isVisible())
        return;                                // 已在框选中: 忽略重复触发

    // 选择层挂主窗口(保证画在所有子控件之上), 几何 = 卡片容器在主窗口里的矩形
    const JZRect box = ui.itemWidgets->mapTo(
        ui.main, JZRect(0, 0, ui.itemWidgets->rect().width(), ui.itemWidgets->rect().height()));

    ui.rubber->setGeometry(box);
    ui.rubber->begin(originGlobal);
    ui.rubber->setVisible(true);

    // 关键一步: 把后续鼠标事件(含移出卡片、移出窗口)直发选择层 —— 引擎里这是"事件冒泡"的替代
    ui.main->grabMouse(ui.rubber);
}

// 建选择层并接线(buildBTEServerUi 里、initChannelGrid 之后调用)
void initRubberBand(BTEServer& ui)
{
    if (!ui.main || !ui.itemWidgets)
        return;

    ui.rubber = new BTERubberBand(ui.main);   // 挂主窗口: 画在其它子控件之上
    ui.rubber->setObjectName("rubberBand");

    connect(&ui.rubber->sigSelected, [&ui](uint32_t modifiers) {
        const JZRect sel = ui.rubber->mapTo(ui.main, ui.rubber->selection());
        selectCardsInRect(ui, sel, (modifiers & JZKeyEvent::CtrlModifier) != 0);

        if (ui.main)
            ui.main->releaseMouse(ui.rubber);   // 松手才解除抓取(见 beginRubberSelect)
    });
}

// ---------------------------------------------------------------------------
// 连网 + 协议(原版: BTEServer 的 m_server + NetServer/NetDataManager):
//   收连接 -> 建会话 -> 收字节 -> 分帧(takePackets) -> 按 CmdID 分发 -> 回响应。
//
// 帧/校验/尾的口径见 PackDefine.h; 报文结构从原版 common/PackDefine.h 搬。
// 已接: CONNECT(回 CONNECT_RESP) / SEND_INFO(解设备名 + 回 RESP + 补发 SET_RTC/RT_INTERVAL)
//   / HEART_BEAT(记心跳时间) / SEND_RTDATA76(卡片 + 通道表 + 报警) / SEND_DFDATA76(记录入库 +
//   回 RESP) / SEND_LOG(日志文本) / 各控制包的回应(START/STOP/RESUME/JUMP);
// 其余命令到达时只记一条"还没搬", 不做假处理。
// ---------------------------------------------------------------------------
const int kBteServerPort = 8001;   // 原版端口(骨架默认不占它, 见 buildBTEServer 的参数)

// ---- 设备/socket 查找(按 netId) ----
DeviceInfo* deviceOf(BTEServer& ui, int netId)
{
    if (netId < 0)
        return nullptr;
    for (size_t i = 0; i < ui.devices.size(); ++i)
    {
        if (ui.devices[i].netId == netId)
            return &ui.devices[i];
    }
    return nullptr;
}

JZTcpSocket* socketOf(BTEServer& ui, int netId)
{
    std::map<int, JZTcpSocket*>::iterator it = ui.byNetId.find(netId);
    return (it == ui.byNetId.end()) ? nullptr : it->second;
}

// 测试页的收发记录(原版 dispPack): 开关 + RT/DF 过滤都在测试页那三个勾选框上
void recordPack(BTEServer& ui, bool send, const PackInfo& info, const JZByteArray& frame)
{
    if (!ui.boxRecordPack || !ui.boxRecordPack->isChecked())
        return;
    if (ui.boxShowRT && ui.boxShowRT->isChecked() && info.CmdID == BTS_CMD_SEND_RTDATA76)
        return;
    if (ui.boxShowDF && ui.boxShowDF->isChecked()
        && (info.CmdID == BTS_CMD_SEND_DFDATA76 || info.CmdID == BTS_CMD_SEND_DFDATA_RESP76))
        return;

    JZDateTime t = JZDateTime::currentDateTime();
    char buf[512];
    snprintf(buf, sizeof(buf), "idx = %d,cmd = %#06X [%s]",
             ui.dispIndex, info.CmdID, t.toString("HH:mm:ss").c_str());
    std::string title = std::string("[") + (send ? "发" : "收") + "] " + buf;
    if (info.IsRecvPack())
    {
        char rbuf[32];
        snprintf(rbuf, sizeof(rbuf), ",ret = %#02X", info.Result);
        title += rbuf;
    }

    appendLine(send ? ui.textSend : ui.textRecv, title + "\n" + hexDump(frame));
}

// 对端地址 —— 引擎的 JZTcpSocket 目前**没有**对端地址接口(只有 localIp/localPort),
// 原版是 NetServer 通过 QTcpServer 记的 peerAddress ⇒ 这里先用 netId 占位。
// TODO(引擎): 补一个 JZTcpSocket::peerIp()/peerPort() 之后换成真 IP(设备名与列表都用它)。
std::string peerIpOf(int netId)
{
    return "net" + std::to_string(netId);
}

void sendFrame(BTEServer& ui, JZTcpSocket* sock, const PackInfo& info,
               const JZByteArray& body)
{
    const JZByteArray frame = packFrame(info, body);
    if (sock)
        sock->write((const char*)frame.data(), frame.size());
    else
        logLine("[发] 没有可用的连接(netId 已断?), cmd = " + cmdName(info.CmdID));

    ++ui.dispIndex;
    recordPack(ui, true, info, frame);

    logLine("[发] cmd = " + cmdName(info.CmdID)
            + ", " + std::to_string(frame.size()) + " 字节\n" + hexDump(frame));
}

// 回一个"响应类"包(原版 BTEServer::response(netId, cmd, result))
void sendResponse(BTEServer& ui, JZTcpSocket* sock, const NetSession& s,
                  uint16_t cmd, uint8_t result)
{
    PackInfo info;
    info.DeviceID = s.deviceId;
    info.SubDevID = 0;
    info.CmdID = cmd;
    info.Result = result;
    sendFrame(ui, sock, info, JZByteArray());
}

// 一帧怎么处理(原版 BTEServer::onDataRecv 的分发)
void dispatchPacket(BTEServer& ui, JZTcpSocket* sock, NetSession& s,
                    const NetPack& pack)
{
    ++s.packCount;

    const uint16_t cmd = pack.info.CmdID;
    ++ui.dispIndex;
    recordPack(ui, false, pack.info, pack.data);

    logLine("[收] cmd = " + cmdName(cmd)
                         + ", dev = " + std::to_string(pack.info.DeviceID)
                         + ", body = " + std::to_string(pack.data.size()) + " 字节");

    if (cmd == BTS_CMD_CONNECT)
    {
        stru_BTS_CMD_CONNECT c;
        if (c.unpack(pack.data))
            s.deviceId = pack.info.DeviceID;
        sendResponse(ui, sock, s, BTS_CMD_CONNECT_RESP, 0);   // 原版: response(..., 0)
        return;
    }

    if (cmd == BTS_CMD_SEND_INFO)
    {
        stru_BTS_CMD_SEND_INFO info;
        if (!info.unpack(pack.data))
        {
            logLine("[协议] SEND_INFO 解包失败(body " + std::to_string(pack.data.size())
                                + " 字节, 期望 108)");
            sendResponse(ui, sock, s, BTS_CMD_SEND_INFO_RESP, 1);
            return;
        }

        s.deviceId = pack.info.DeviceID;
        s.gotInfo = true;
        s.deviceString = info.deviceString(peerIpOf(s.netId));
        applyDeviceInfo(ui, s.netId, s.deviceId, info, peerIpOf(s.netId));

        sendResponse(ui, sock, s, BTS_CMD_SEND_INFO_RESP, 0);  // 原版: 回 RESP, result 0

        // 原版紧接着下发两条: SET_RTC(对时) + SET_RT_INTERVAL(实时帧间隔, 1 秒)
        JZDateTime t = JZDateTime::currentDateTime();
        {
            PackInfo si;
            si.DeviceID = s.deviceId;
            si.CmdID = BTS_CMD_SET_RTC;
            sendFrame(ui, sock, si, packSetRtc(t.year(), t.month(), t.day(),
                                                  t.hour(), t.minute(), t.second()));
        }
        {
            PackInfo si;
            si.DeviceID = s.deviceId;
            si.CmdID = BGS66_CMD_SET_RT_INTERVAL;
            sendFrame(ui, sock, si, packRtInterval(1));
        }
        return;
    }

    if (cmd == BTS_CMD_HEART_BEAT)
    {
        if (DeviceInfo* dev = deviceOf(ui, s.netId))
            dev->heartMs = nowMs();
        return;
    }

    if (cmd == BTS_CMD_SEND_RTDATA76)
    {
        stru_BTS_CMD_SEND_RTDATA76 rt;
        if (!rt.unpack(pack.data))
        {
            logLine("[协议] RTDATA76 解包失败(body " + std::to_string(pack.data.size())
                                + " 字节, 期望 75)");
            return;
        }

        // 原版: item_idx = (SubDevID - 1) * 8 + ChannelIDByNum - 1
        const int subIdx = (int)pack.info.SubDevID - 1;
        const int itemIndex = subIdx * MAX_CHANNEL_NUM + (int)rt.ChannelIDByNum - 1;
        if (itemIndex < 0 || itemIndex >= MAX_DEVICE_ITEM)
        {
            logLine("[协议] RTDATA76 通道越界(sub=" + std::to_string(pack.info.SubDevID)
                                + ", chn=" + std::to_string(rt.ChannelIDByNum) + ")");
            return;
        }
        applyRtData(ui, s.netId, itemIndex, rt);
        return;
    }

    if (cmd == BTS_CMD_SEND_DFDATA76)
    {
        stru_BTS_CMD_SEND_DFDATA76 df;
        if (!df.unpack(pack.data))
        {
            logLine("[协议] DFDATA76 解包失败(body " + std::to_string(pack.data.size())
                                + " 字节)");
            return;
        }

        DeviceInfo* dev = deviceOf(ui, s.netId);
        const std::string devId = dev ? dev->deviceString : s.deviceString;
        const int n = applyDfData(ui, devId, df);

        // 原版: 回一个 RESP, Index = DFDataPointer
        stru_BTS_CMD_SEND_DFDATA_RESP76 resp;
        resp.Index = df.DFDataPointer;
        PackInfo ri;
        ri.DeviceID = s.deviceId;
        ri.SubDevID = (uint8_t)pack.info.SubDevID;
        ri.CmdID = BTS_CMD_SEND_DFDATA_RESP76;
        ri.Result = 0;
        sendFrame(ui, sock, ri, resp.packBody());
        if (n > 0)
            logLine("[记录] " + devId + " 入库 " + std::to_string(n) + " 点");
        return;
    }

    if (cmd == BTS_CMD_SEND_LOG)
    {
        stru_BTS_CMD_SEND_LOG log;
        if (log.unpack(pack.data))
        {
            char buf[256];
            snprintf(buf, sizeof(buf), "[日志] 单元%d 通道%d 时间=%s 类型=%u 信息=%d",
                     (int)log.UnitID, (int)log.ChannelIDByNum, log.timeText().c_str(),
                     (unsigned)log.LogID, (int)log.LogInfo);
            logLine(buf);
        }
        sendResponse(ui, sock, s, BTS_CMD_SEND_LOG_RESP, 0);
        return;
    }

    if (cmd == BTS_CMD_START_RESP70)
    {
        // 原版: 逐个通道把 Status_WaitStart 置成 Status_Run
        stru_BTS_CMD_START_RESP70 resp;
        if (resp.unpack(pack.data))
        {
            DeviceInfo* dev = deviceOf(ui, s.netId);
            const int subIdx = (int)pack.info.SubDevID - 1;
            for (size_t i = 0; dev && i < resp.channelInfo.size(); ++i)
            {
                const int itemIndex = subIdx * MAX_CHANNEL_NUM
                                      + (int)resp.channelInfo[i].ChannelIDByNum - 1;
                if (itemIndex >= 0 && itemIndex < MAX_DEVICE_ITEM
                    && dev->status[itemIndex] == Status_WaitStart)
                    dev->status[itemIndex] = Status_Run;
            }
        }
        return;
    }

    if (cmd == BTS_CMD_STOP_RESP || cmd == BTS_CMD_RESUME_RESP || cmd == BTS_CMD_STEP_JUMP_RESP)
    {
        // 这三个回应都是 8 字节通道位图: 中位机确认了哪些通道
        DeviceInfo* dev = deviceOf(ui, s.netId);
        const int subIdx = (int)pack.info.SubDevID - 1;
        const std::vector<int> chns = unpackChannelMask(pack.data);
        for (size_t i = 0; dev && i < chns.size(); ++i)
        {
            const int itemIndex = subIdx * MAX_CHANNEL_NUM + chns[i];
            if (itemIndex < 0 || itemIndex >= MAX_DEVICE_ITEM)
                continue;
            if (cmd == BTS_CMD_STOP_RESP)
                dev->status[itemIndex] = Status_Stop;
            else if (dev->status[itemIndex] == Status_WaitStart
                     || dev->status[itemIndex] == Status_WaitJump)
                dev->status[itemIndex] = Status_Run;
        }
        return;
    }

    if (cmd == BTS_CMD_SET_STEP_EX_RESP76)
    {
        // 工步下发回执。原版: 收到一个回执就从 stepPackList 出队继续发下一个;
        // 骨架是"按子设备直接各发一帧"(见 submitStepSetting), 所以这里只记一条。
        // body = Guid[16] + ChannelID[8] + DataEndFlag(1) + DataIndex(2) —— 与原版结构同名同序。
        int chn = 0;
        if (pack.data.size() >= 16 + 8)
        {
            const uint8_t* p = (const uint8_t*)pack.data.data() + 16;   // 跳过 Guid
            for (int i = 0; i < 8; ++i)
            {
                uint8_t b = p[i];
                while (b) { chn += (b & 1); b >>= 1; }                  // 数一下置了几路
            }
        }

        logLine("[工步] 下发回执: " + std::to_string(chn) + " 路通道 (SubDevID="
                + std::to_string((int)pack.info.SubDevID) + ")");
        return;
    }

    logLine("[协议] " + cmdName(cmd) + " 的业务还没搬(TODO: 报文结构 + 处理)");
}

// ---------------------------------------------------------------------------
// 心跳定时(原版 m_heartTimer.start(1000) -> onHeartTime()): 超时没收到心跳就把设备标离线。
// 骨架只做"离线判定"那一半 —— 重发工步包要有完整下发队列(stepPackList), 还没搬。
// ---------------------------------------------------------------------------
void initHeartTimer(BTEServer& ui)
{
    
    ui.heartTimer = new JZTimer(ui.main);
    connect(&ui.heartTimer->sigTimeout, [&ui]() {
        const int64_t now = nowMs();
        for (size_t i = 0; i < ui.devices.size(); ++i)
        {
            DeviceInfo& d = ui.devices[i];
            if (!d.online || d.heartMs == 0)
                continue;
            if (now - d.heartMs > (int64_t)HEART_TIMEOUT_SEC * 1000)
            {
                logLine("[网络] netId=" + std::to_string(d.netId)
                                     + " 心跳超时(" + std::to_string((int)((now - d.heartMs) / 1000))
                                     + "s; now=" + std::to_string((long long)now)
                                     + " heart=" + std::to_string((long long)d.heartMs)
                                     + "), 按离线处理");
                d.online = false;
                std::fill(d.alarmFlag.begin(), d.alarmFlag.end(), 0);
                std::fill(d.alarmClearFlag.begin(), d.alarmClearFlag.end(), 0);
                rebuildDeviceList(ui);
            }
        }
    });
    ui.heartTimer->start(1000);
}

void initServer(BTEServer& ui, int port)
{

    ui.server = new JZTcpServer();

    JZTcpServer* srv = ui.server;                 // 回调只捕获指针(活得比信号长)

    // ui 由调用方持有(见头文件说明) ⇒ 回调捕获它的地址是安全的
    connect(&srv->sigAccept, [srv, &ui]() {
        JZTcpSocket* sock = srv->nextPendingConnection();
        if (!sock)
            return;

        NetSession& s = ui.sessions[sock];
        s.netId = (int)ui.sessions.size();       // 原版 netId 由 NetServer 分配
        ui.byNetId[s.netId] = sock;              // 下发命令要按 netId 找 socket
        logLine("[网络] 客户端接入(netId=" + std::to_string(s.netId) + ")");

        // 原版 onNewConnect: 先插一个空设备条目(等 SEND_INFO 才知道它是谁)
        {
            DeviceInfo dev;
            dev.netId = s.netId;
            dev.heartMs = nowMs();
            ui.devices.push_back(dev);
            rebuildDeviceList(ui);
        }

        connect(&sock->sigReadyRead, [sock, &ui]() {
            NetSession& session = ui.sessions[sock];
            session.buffer.append(sock->readAll());

            std::vector<NetPack> packs;
            std::string err;
            takePackets(session.buffer, packs, &err);
            if (!err.empty())
                logLine("[协议] 分帧: " + err);

            for (size_t i = 0; i < packs.size(); ++i)
                dispatchPacket(ui, sock, session, packs[i]);   // 里面记 "[收] ..." 并计数
        });

        connect(&sock->sigDisconnected, [sock, &ui]() {
            std::map<JZTcpSocket*, NetSession>::iterator it = ui.sessions.find(sock);
            if (it == ui.sessions.end())
                return;

            logLine("[网络] 连接断开(netId=" + std::to_string(it->second.netId)
                         + ", 收过 " + std::to_string(it->second.packCount) + " 帧)");
            const int netId = it->second.netId;
            ui.sessions.erase(it);
            ui.byNetId.erase(netId);

            // 原版 onDisConnect -> disconnect(netId): 列表项标"(离线)"、data[0] 置 -1,
            // 报警标志清空(设备不在线, 它的报错不再计入报警)。
            DeviceInfo* dev = deviceOf(ui, netId);
            if (dev)
            {
                dev->online = false;
                std::fill(dev->alarmFlag.begin(), dev->alarmFlag.end(), 0);
                std::fill(dev->alarmClearFlag.begin(), dev->alarmClearFlag.end(), 0);
            }
            rebuildDeviceList(ui);
        });
    });

    if (srv->listen(port))
    {
        ui.serverPort = srv->port();
        logLine("[网络] 监听中, 端口 = " + std::to_string(ui.serverPort)
                     + (port == kBteServerPort ? "(与原版一致)" : ""));
    }
    else
    {
        // 注: 传 0 一定会走到这里 —— 引擎的 listen() 不接受 0(没有"让系统挑端口"这条)
        logLine("[网络] 监听端口 " + std::to_string(port)
                     + " 失败(被占用? 或传了 0, 引擎不支持 0)");
    }
}

// ---------------------------------------------------------------------------
// 设备列表(原版 initDeviceList(): 一行一个设备, 文本 "设备名(在线)"/"(离线)";
// data 存 [netId, DeviceString]; 掉线那行 netId 记 -1)。
// 数据源是 ui.devices —— 由协议层填(连上就有条目, 报 SEND_INFO 之后才有名字)。
// 顺带把"设备"下拉框(查询页)刷成同一份名单。
// ---------------------------------------------------------------------------
void rebuildDeviceList(BTEServer& ui)
{
    
    if (ui.listWidget)
    {
        const int keep = ui.listWidget->currentIndex();
        ui.listWidget->clear();

        for (size_t i = 0; i < ui.devices.size(); ++i)
        {
            const DeviceInfo& d = ui.devices[i];
            // 还没报 SEND_INFO 的条目没有名字, 先按 netId 显示
            const std::string name = d.deviceString.empty()
                                     ? ("net" + std::to_string(d.netId)) : d.deviceString;
            ui.listWidget->appendItem(name + (d.online ? "(在线)" : "(离线)"));

            JZListWidgetItem* item = ui.listWidget->item(ui.listWidget->count() - 1);
            if (!item)
                continue;
            std::vector<JZVariant> payload;
            payload.push_back(JZVariant(d.online ? d.netId : -1));
            payload.push_back(JZVariant(name));
            item->setData(JZVariant(payload));
        }

        if (keep >= 0 && keep < ui.listWidget->count())
            ui.listWidget->setCurrentIndex(keep);
        else if (ui.listWidget->count() > 0)
            ui.listWidget->setCurrentIndex(0);
    }

    if (ui.boxDevice)
    {
        const std::string cur = ui.boxDevice->currentText();
        ui.boxDevice->clear();
        for (size_t i = 0; i < ui.devices.size(); ++i)
        {
            const DeviceInfo& d = ui.devices[i];
            if (!d.deviceString.empty())
                ui.boxDevice->addItem(d.deviceString);
        }
        if (!cur.empty())
            ui.boxDevice->setCurrentText(cur);
        if (ui.boxDevice->currentIndex() < 0 && ui.boxDevice->count() > 0)
            ui.boxDevice->setCurrentIndex(0);
    }

    // 设备列表变了(连上/断开/增删) ⇒ 卡片的显示数量跟着刷(原版 rebuildDeviceList 后同样要刷)
    updateChannelCardVisibility(ui);
}

// 原版本的 curNetId(): 取当前项的 data[0]
int curNetId(JZListWidget* list)
{
    if (!list)
        return -1;
    const int idx = list->currentIndex();
    if (idx < 0)
        return -1;
    const std::vector<JZVariant> payload = list->itemData(idx).toList();
    return payload.empty() ? -1 : payload[0].toInt();
}

} // namespace

// ===========================================================================
// 对外那半(头文件里声明的: 与 socket 解耦, 自检直接调) —— 放在匿名命名空间**外面**,
// 否则会和这里的声明撞成"重载不明确"(ADL 会同时找到两份)。
// ===========================================================================
// ---------------------------------------------------------------------------
// 报警音(原版 BTEServer 的 startAlarm/stopAlarm/refreshAlarmState):
//   只要还有"在线 + 报警置位 + 未确认"的通道就循环播放 wav/alarm.wav; 全清/全恢复就停。
//   wav 文件不存在时只报一次(原版 LogError), 不反复刷。
// ---------------------------------------------------------------------------
const char* kAlarmWav = "wav/alarm.wav";

void startAlarm(BTEServer& ui)
{
    if (!ui.alarm || ui.alarm->isPlaying())
        return;

    if (!JZFile::exists(kAlarmWav))
    {
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            logLine(std::string("[报警] 找不到 ") + kAlarmWav + "(原版: 程序目录下 wav/alarm.wav)");
        }
        return;
    }
    ui.alarm->start(kAlarmWav);
}

void stopAlarm(BTEServer& ui)
{
    if (ui.alarm)
        ui.alarm->stop();
}

// 扫一遍全部在线设备的报警标志: 还有没确认的就响, 否则停(原版 refreshAlarmState)
void refreshAlarmState(BTEServer& ui)
{
    if (anyAlarm(ui))
        startAlarm(ui);
    else
        stopAlarm(ui);

    updateClearAlarmButton(ui);   // 按钮的可见性跟着报警状态走
}

// ---------------------------------------------------------------------------
// 数值格式化(卡片/通道表用): 换算见 DataTrans.h 的 from*Unit
// ---------------------------------------------------------------------------
static std::string fmtV(double v)   { char b[32]; snprintf(b, sizeof(b), "%.4f", v); return b; }
static std::string fmtA(double v)   { char b[32]; snprintf(b, sizeof(b), "%.4f", v); return b; }
static std::string fmtAh(double v)  { char b[32]; snprintf(b, sizeof(b), "%.4f", v); return b; }
static std::string fmtTemp(double v){ char b[32]; snprintf(b, sizeof(b), "%.1f", v); return b; }

// ---------------------------------------------------------------------------
// 实时帧 -> 界面(原版 setItemData + 报警那一段)
//   * 卡片: 4 行数值(电压/电流/温度/容量) + 报警配色;
//   * 通道表 tableList: 工步/循环/状态/温度/电压/电流/时间/容量/能量;
//   * 设备里的 alarmFlag 由实时帧维护(故障置位, 恢复自动复位)。
// 只刷"当前显示设备"的那张卡(原版: if (netId == curNetId())) —— 别的设备的数据也记在
// devices 里, 切过去时能立刻看到。
// ---------------------------------------------------------------------------
void applyRtData(BTEServer& ui, int netId, int itemIndex, const stru_BTS_CMD_SEND_RTDATA76& rt)
{
    DeviceInfo* dev = deviceOf(ui, netId);
    if (!dev)
        return;

    dev->heartMs = nowMs();

    // 状态: 运行/完成/停止(原版 WORK_* -> Status_*)
    const int st = rt.workStatus();
    dev->status[itemIndex] = (st == stru_BTS_CMD_SEND_RTDATA76::WORK_RUN)
                             ? Status_Run
                             : (st == stru_BTS_CMD_SEND_RTDATA76::WORK_COMPLETE
                                ? Status_Complete
                                : Status_Stop);

    // 报警标志: 故障置位 / 恢复复位(原版 alarmFlag 由实时帧维护, 与"清除确认"是两回事)
    const bool fault = rt.isFault();
    if ((dev->alarmFlag[itemIndex] != 0) != fault)
    {
        dev->alarmFlag[itemIndex] = fault ? 1 : 0;
        if (fault)
        {
            logLine("[报警] " + dev->deviceString + " "
                                + channelTextOf(itemIndex) + " 故障, 保护码 = "
                                + std::to_string(rt.PrtCode));
            // 新报警 = 未确认 ⇒ 重新判定要不要响铃(原版 refreshAlarmState)
            dev->alarmClearFlag[itemIndex] = 0;
            refreshAlarmState(ui);
        }
        else
        {
            // 通道恢复正常: 复位并重新判定(可能因此停铃)
            dev->alarmClearFlag[itemIndex] = 0;
            refreshAlarmState(ui);
        }
    }

    // 只刷当前设备的卡片/表
    if (netId != curNetId(ui.listWidget))
        return;

    if (itemIndex >= 0 && itemIndex < (int)ui.channelCards.size())
    {
        BTEItemWidget* card = ui.channelCards[itemIndex];
        // 原版 setItemData(): 整帧交给卡片, 由卡片自己按 status/PrtCode/StepType 决定
        // 底色与 9 行文案(见 BTEItemWidget::bgColor/getDispInfo)。
        card->setData(rt);
        card->setStatus(dev->status[itemIndex]);
        card->setAlarmCleared(dev->alarmClearFlag[itemIndex] != 0);
        card->setVisible(true);      // 有实时数据的通道一定是亮着的
    }

    if (ui.tableList && itemIndex >= 0 && itemIndex < ui.tableList->rowCount())
    {
        const int row = itemIndex;
        ui.tableList->setItemText(row, 2, std::to_string((int)rt.StepID));            // 工步
        ui.tableList->setItemText(row, 3, std::to_string((int)rt.CycleStatus));       // 循环
        ui.tableList->setItemText(row, 4, rt.statusText());                          // 工作状态
        ui.tableList->setItemText(row, 5, fmtTemp(DataTrans::fromTempUnit(rt.Tempature)) + "℃");
        ui.tableList->setItemText(row, 6, fmtV(DataTrans::fromVoltageUnit(rt.Voltage)) + "V");
        ui.tableList->setItemText(row, 7, fmtA(DataTrans::fromElectricUnit(rt.Current)) + "A");
        ui.tableList->setItemText(row, 8, DataTrans::fromTimeUnitSec(rt.TestTime));
        ui.tableList->setItemText(row, 9, fmtAh(DataTrans::fromCapUnit(rt.ChargeCap)));
        ui.tableList->setItemText(row, 10, fmtAh(DataTrans::fromCapUnit(rt.DisChargeCap)));
        ui.tableList->setItemText(row, 11, fmtAh(DataTrans::fromEngUnit(rt.ChargeEng)));
        ui.tableList->setItemText(row, 12, fmtAh(DataTrans::fromEngUnit(rt.DisChargeEng)));
    }
}

// ---------------------------------------------------------------------------
// 记录点(DF)帧 -> 内存记录库(原版 m_project.record())
//   0x55(平台点容量)才入库; 同一设备同一通道的连续点归到一条记录里
//   (记录号/起止时间/条数/中值电压/放电容量 —— 后两项给分选用)。
// ---------------------------------------------------------------------------
int applyDfData(BTEServer& ui, const std::string& deviceId, const stru_BTS_CMD_SEND_DFDATA76& df)
{
    if (deviceId.empty())
        return 0;

    int done = 0;
    for (size_t i = 0; i < df.dataList.size(); ++i)
    {
        const stru_BTS_CMD_SEND_DFDATA76::Data& d = df.dataList[i];
        if (d.DataFlag != 0x55)          // 原版只记 0x55 平台点容量
            continue;

        const int itemIndex = ((int)d.SubDevID - 1) * MAX_CHANNEL_NUM
                              + (int)d.ChannelIDByNum - 1;
        if (itemIndex < 0 || itemIndex >= MAX_DEVICE_ITEM)
            continue;

        // 原版: m_project.record(deviceId, item_idx, data) —— 进"当前记录", 归档由
        // stop/finish 触发(见 BTEProject::archiveRecord), 落盘走设备自己的 sqlite 库
        if (ui.project.record(deviceId, itemIndex, d) == STATUS_OK)
            ++done;
    }
    return done;
}

// ---------------------------------------------------------------------------
// 下发一条通道控制命令(原版 send() + createPackList())
//   按 item_idx 拆成"每子设备一包"(原版就是这么组的: 一个包只带同一子设备的通道),
//   包头 SubDevID = 子设备号 + 1(原版 send() 里的 +1)。
// ---------------------------------------------------------------------------
int sendChannelCommand(BTEServer& ui, int netId, uint16_t cmd,
                       const std::vector<int>& channels, uint8_t stepId)
{
    JZTcpSocket* sock = socketOf(ui, netId);
    DeviceInfo* dev = deviceOf(ui, netId);
    if (!sock || !dev)
        return 0;

    // 按子设备分组
    std::map<int, std::vector<int> > bySub;
    for (size_t i = 0; i < channels.size(); ++i)
    {
        const int idx = channels[i];
        if (idx < 0 || idx >= MAX_DEVICE_ITEM)
            continue;
        bySub[idx / MAX_CHANNEL_NUM].push_back(idx % MAX_CHANNEL_NUM);
    }

    int sent = 0;
    std::map<int, std::vector<int> >::iterator it = bySub.begin();
    for (; it != bySub.end(); ++it)
    {
        const int sub = it->first;
        const std::vector<int>& chns = it->second;

        JZByteArray body;
        if (cmd == BTS_CMD_START70)
        {
            uint8_t guid[16];
            for (int i = 0; i < 16; ++i)
                guid[i] = (uint8_t)(0x40 + i);                 // 原版 m_guid
            body = packStart70(chns, stepId, guid);
        }
        else if (cmd == BTS_CMD_STEP_JUMP)
        {
            body = packStepJump(chns, stepId);
        }
        else if (cmd == BTS_CMD_STOP || cmd == BTS_CMD_RESUME)
        {
            body = packChannelMaskBody(chns);
        }
        else
        {
            continue;   // 只支持这四种通道命令
        }

        PackInfo info;
        info.DeviceID = dev->deviceId;
        info.SubDevID = (uint8_t)(sub + 1);        // 原版 send(): SubDevID + 1
        info.CmdID = cmd;
        sendFrame(ui, sock, info, body);

        // 置"等待回应"状态(原版 Status_WaitStart/WaitStop/WaitJump)
        const int wait = (cmd == BTS_CMD_START70) ? Status_WaitStart
                       : (cmd == BTS_CMD_STOP)    ? Status_WaitStop
                       : (cmd == BTS_CMD_STEP_JUMP) ? Status_WaitJump
                       : Status_Run;
        for (size_t k = 0; k < chns.size(); ++k)
        {
            const int itemIndex = sub * MAX_CHANNEL_NUM + chns[k];
            if (itemIndex >= 0 && itemIndex < MAX_DEVICE_ITEM)
                dev->status[itemIndex] = wait;
        }
        ++sent;
    }
    return sent;
}

// ---------------------------------------------------------------------------
// 一键清除报警(原版 clearAlarmAll): 未确认的报警统统置成"已确认"(卡片转橙), 刷新当前设备
// ---------------------------------------------------------------------------
int clearAlarmAll(BTEServer& ui)
{
    int n = 0;
    for (size_t i = 0; i < ui.devices.size(); ++i)
    {
        DeviceInfo& d = ui.devices[i];
        for (int k = 0; k < MAX_DEVICE_ITEM; ++k)
        {
            if (d.alarmFlag[k] && !d.alarmClearFlag[k])
            {
                d.alarmClearFlag[k] = 1;
                ++n;
            }
        }
    }

    // 同步到卡片(原版 updateItemAlarmClearFlags)
    for (int k = 0; k < (int)ui.channelCards.size(); ++k)
    {
        BTEItemWidget* card = ui.channelCards[k];
        const DeviceInfo* d = deviceOf(ui, curNetId(ui.listWidget));
        if (!d)
            break;
        // 原版 updateItemAlarmClearFlags 只写"已确认"标志 —— 卡片的红/橙由它 + PrtCode 决定,
        // 不需要(也没有)单独的"报警"开关。
        card->setAlarmCleared(d->alarmClearFlag[k] != 0);
    }

    // 原版 clearAlarmAll 最后: stopAlarm() —— 全都确认过了, 不用再响
    stopAlarm(ui);
    return n;
}

// ---------------------------------------------------------------------------
// 查询(原版 on_btnQuery_clicked): 左栏条件 -> pageHistory 里的 tableQuery(原版同名)
//   条件: 设备(下拉) / 单元 / 通道 / 电池编号(包含) / 批号(包含) / 创建者(包含)
//         + 时间(不限 / 最近 N 天 / 指定 从..至..)
//   ⚠ 原版第 3 列"分选"是 setCellWidget(QCheckBox) —— 骨架没有 setCellWidget, 用 "√/空" 文本
//     顶替(用户明确: setCellWidget 不做)。
// ---------------------------------------------------------------------------
int queryRecords(BTEServer& ui)
{
    if (!ui.tableQuery)
        return -1;

    const std::string deviceId = ui.boxDevice ? ui.boxDevice->currentText() : std::string();
    if (deviceId.empty())
        return -1;

    // 单元/通道: 空 = 不限; 填了必须是正整数(原版 toInt 的口径: > 0 才算合法)
    int unit = -1, chn = -1;
    auto toInt = [](JZLineEdit* edit, bool& ok) -> int {
        const std::string text = edit ? edit->text() : std::string();
        if (text.empty()) { ok = true; return 0; }
        ok = true;
        int v = 0;
        for (size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] < '0' || text[i] > '9') { ok = false; return 0; }
            v = v * 10 + (text[i] - '0');
        }
        ok = ok && (v > 0);
        return v;
    };
    bool unitOk = true, chnOk = true;
    unit = toInt(ui.lineQueryUnit, unitOk);
    chn = toInt(ui.lineQueryChannel, chnOk);
    if (!unitOk || !chnOk)
        return -1;
    unit -= 1;
    chn -= 1;

    const int timeType = (ui.btnNone && ui.btnNone->isChecked()) ? 0
                       : (ui.btnNear && ui.btnNear->isChecked()) ? 1 : 2;
    const int dayNear = ui.lineNearDay ? ui.lineNearDay->value() : 7;
    const int64_t now = nowMs();
    const int64_t fromMs = ui.dateFrom ? ui.dateFrom->date().toMSecsSinceEpoch() : 0;
    // 原版: to = to.addDays(1) —— "至"那一天要含进来
    const int64_t toMs = ui.dateTo ? ui.dateTo->date().addDays(1).toMSecsSinceEpoch() : now;

    const std::string cellNum = ui.lineCellNum ? ui.lineCellNum->text() : std::string();
    const std::string cellPatch = ui.lineCellPatch ? ui.lineCellPatch->text() : std::string();
    const std::string creator = ui.lineCreator ? ui.lineCreator->text() : std::string();

    auto contains = [](const std::string& all, const std::string& sub) {
        return sub.empty() || all.find(sub) != std::string::npos;
    };

    // 原版: const QMap<int, BTERecordInfo> &list = m_project.recordList(deviceId);
    const std::map<int, BTERecordInfo>& list = ui.project.recordList(deviceId);

    ui.queryHits.clear();
    std::map<int, BTERecordInfo>::const_iterator it = list.begin();
    for (; it != list.end(); ++it)
    {
        const BTERecordInfo& r = it->second;

        const int u = r.itemIndex / MAX_CHANNEL_NUM;
        const int c = r.itemIndex % MAX_CHANNEL_NUM;
        if (unit >= 0 && u != unit)
            continue;
        if (chn >= 0 && c != chn)
            continue;

        if (timeType == 1)
        {
            const int days = (int)((now - r.recordTimeEnd) / (24 * 3600 * 1000));
            if (days > dayNear)
                continue;
        }
        else if (timeType == 2 && (r.recordTime < fromMs || r.recordTime > toMs))
            continue;

        if (!contains(r.stepInfo.cellNo, cellNum) || !contains(r.stepInfo.cellBatch, cellPatch)
            || !contains(r.stepInfo.creator, creator))
            continue;

        ui.queryHits.push_back(r.recordId);
    }

    // ---- 结果进表(pageHistory 里那张 tableQuery; 原版查询结果就填这一张) ----
    const int rows = (int)ui.queryHits.size();
    ui.tableQuery->setRowCount(rows);

    for (int i = 0; i < rows; ++i)
    {
        std::map<int, BTERecordInfo>::const_iterator hit = list.find(ui.queryHits[i]);
        if (hit == list.end())
            continue;
        const BTERecordInfo& r = hit->second;

        const std::string start = JZDateTime::fromMSecsSinceEpoch(r.recordTime)
                                  .toString("yyyy-MM-dd HH:mm:ss");
        const std::string end = JZDateTime::fromMSecsSinceEpoch(r.recordTimeEnd)
                                .toString("yyyy-MM-dd HH:mm:ss");
        const std::string level = (ui.splitLevels.count(r.recordId) > 0)
                                  ? std::to_string(ui.splitLevels[r.recordId]) : std::string();

        // 列: 记录号 / 通道号 / 分选 / 启动时间 / 结束时间 / 记录条数 / 电池编号 / 批号 / 创建者 / 备注
        const std::string cells[10] = {
            std::to_string(i + 1), channelTextOf(r.itemIndex), level,
            start, end, std::to_string(r.recordNum),
            r.stepInfo.cellNo, r.stepInfo.cellBatch, r.stepInfo.creator, r.stepInfo.memo
        };
        for (int c = 0; c < 10; ++c)
            ui.tableQuery->setItemText(i, c, cells[c]);
    }

    if (ui.stackedWidget && rows > 0)
        ui.stackedWidget->setCurrentIndex(BTEServer::PageHistory);

    return rows;
}

// ---------------------------------------------------------------------------
// 删除记录(原版 recordDelete(beforeDay) + deleteRecords): beforeDay > 0 按"结束时间早于
// now - beforeDay 天"删; deviceId 非空则只删该设备(原版 btnDeleteDevice)。
// ---------------------------------------------------------------------------
int deleteRecords(BTEServer& ui, int beforeDay, const std::string& deviceId)
{
    const int64_t limit = (beforeDay > 0) ? (nowMs() - (int64_t)beforeDay * 24 * 3600 * 1000) : 0;
    int removed = 0;

    // 只删某台设备(原版 btnDeleteDevice)
    if (!deviceId.empty())
    {
        const std::map<int, BTERecordInfo>& list = ui.project.recordList(deviceId);
        removed = (int)list.size();
        ui.project.deleteRecords(deviceId);
        ui.queryHits.clear();
        ui.splitLevels.clear();
        return removed;
    }

    // 按时间整批删(原版 recordDelete(beforeDay): 遍历**所有**设备)
    const std::vector<std::string> devices = ui.project.deviceList();
    for (size_t i = 0; i < devices.size(); ++i)
    {
        const std::map<int, BTERecordInfo>& list = ui.project.recordList(devices[i]);

        std::vector<int> ids;
        std::map<int, BTERecordInfo>::const_iterator it = list.begin();
        for (; it != list.end(); ++it)
        {
            if (it->second.recordTimeEnd < limit)
                ids.push_back(it->first);
        }
        if (!ids.empty())
        {
            removed += (int)ids.size();
            ui.project.deleteRecords(devices[i], ids);
        }
    }
    ui.queryHits.clear();
    ui.splitLevels.clear();
    return removed;
}

// ---------------------------------------------------------------------------
// 分选(原版 SplitDialog 的简化版): 拿最近一次查询的结果, 按**放电容量**升序排,
// 相邻差值 > 阈值(spinSplitThreshold)就另起一级 —— 填级别表 + 明细表。
// ---------------------------------------------------------------------------
int splitRecords(BTEServer& ui)
{
    const std::string deviceId = ui.boxDevice ? ui.boxDevice->currentText() : std::string();
    if (deviceId.empty())
        return 0;

    // 原版: 拿查询勾选到的记录(historyRecord 载入), 按放电容量/中值电压分档
    std::vector<BTERecordInfo> list;
    for (size_t i = 0; i < ui.queryHits.size(); ++i)
    {
        BTERecordPtr rec = ui.project.historyRecord(deviceId, ui.queryHits[i]);
        if (rec)
            list.push_back(rec->info);
    }
    if (list.empty())
        return 0;

    std::sort(list.begin(), list.end(),
              [](const BTERecordInfo& a, const BTERecordInfo& b) {
                  return a.dataCache.endDisCap < b.dataCache.endDisCap;
              });

    const double threshold = ui.spinSplitThreshold ? ui.spinSplitThreshold->value() : 1.5;

    int level = 1;
    double lastCap = list[0].dataCache.endDisCap;
    std::vector<int> levelCount;      // 每级的电池个数(下标 = 级别-1)
    levelCount.push_back(0);

    ui.splitLevels.clear();
    for (size_t i = 0; i < list.size(); ++i)
    {
        if (i > 0 && list[i].dataCache.endDisCap - lastCap > threshold)
        {
            ++level;
            levelCount.push_back(0);
        }
        ui.splitLevels[list[i].recordId] = level;
        levelCount[level - 1] += 1;
        lastCap = list[i].dataCache.endDisCap;
    }

    // ---- 级别表(级别 / 电池个数 / 点灯) ----
    if (ui.tableSplitLevel)
    {
        ui.tableSplitLevel->setRowCount((int)levelCount.size());
        for (size_t i = 0; i < levelCount.size(); ++i)
        {
            ui.tableSplitLevel->setItemText((int)i, 0, std::to_string(i + 1));
            ui.tableSplitLevel->setItemText((int)i, 1, std::to_string(levelCount[i]));
            ui.tableSplitLevel->setItemText((int)i, 2, std::to_string(i + 1));   // 点灯 = 级别号
        }
    }

    // ---- 明细表(序号 / 单元-通道 / 放电容量 / 放电时间 / 中值电压 / 恒流比 / 级别 / 电池编号) ----
    if (ui.tableSplitList)
    {
        ui.tableSplitList->setRowCount((int)list.size());
        for (size_t i = 0; i < list.size(); ++i)
        {
            const BTERecordInfo& r = list[i];
            char cap[32], volt[32];
            snprintf(cap, sizeof(cap), "%.4f", r.dataCache.endDisCap);
            snprintf(volt, sizeof(volt), "%.4f", r.dataCache.midDisVoltage);

            ui.tableSplitList->setItemText((int)i, 0, std::to_string(i + 1));
            ui.tableSplitList->setItemText((int)i, 1, channelTextOf(r.itemIndex));
            ui.tableSplitList->setItemText((int)i, 2, cap);
            ui.tableSplitList->setItemText((int)i, 3,
                                           DataTrans::fromTimeUnitSec((int64_t)r.dataCache.endDisTestTime * 1000));
            ui.tableSplitList->setItemText((int)i, 4, volt);
            // 恒流比: 原版 calcCCRatio 要整段充电工步的数据 + 工步类型表 ⇒ 还没搬, 先填 0
            ui.tableSplitList->setItemText((int)i, 5, "0.00");
            ui.tableSplitList->setItemText((int)i, 6, std::to_string(ui.splitLevels[r.recordId]));
            ui.tableSplitList->setItemText((int)i, 7, r.stepInfo.cellNo);
        }
    }
    return level;
}

// ---------------------------------------------------------------------------
// 导出查询结果(原版导出的是 xls; 骨架写 CSV —— 少一个 libxlsx 依赖)。
// ---------------------------------------------------------------------------
int exportRecords(BTEServer& ui, const std::string& dir)
{
    if (dir.empty() || ui.queryHits.empty())
        return 0;

    std::string path = dir;
    if (path[path.size() - 1] != '\\' && path[path.size() - 1] != '/')
        path += "\\";
    path += "bte_records.csv";

    const std::string deviceId = ui.boxDevice ? ui.boxDevice->currentText() : std::string();

    std::string csv = "记录号,通道号,级别,启动时间,结束时间,记录条数,电池编号,批号,创建者,备注\n";
    for (size_t i = 0; i < ui.queryHits.size(); ++i)
    {
        BTERecordPtr rec = ui.project.historyRecord(deviceId, ui.queryHits[i]);
        if (!rec)
            continue;
        const BTERecordInfo& r = rec->info;

        const std::string start = JZDateTime::fromMSecsSinceEpoch(r.recordTime)
                                  .toString("yyyy-MM-dd HH:mm:ss");
        const std::string end = JZDateTime::fromMSecsSinceEpoch(r.recordTimeEnd)
                                .toString("yyyy-MM-dd HH:mm:ss");
        const std::string level = (ui.splitLevels.count(r.recordId) > 0)
                                  ? std::to_string(ui.splitLevels[r.recordId]) : std::string();

        csv += std::to_string(i + 1) + "," + channelTextOf(r.itemIndex) + "," + level + ","
               + start + "," + end + "," + std::to_string(r.recordNum) + ","
               + r.stepInfo.cellNo + "," + r.stepInfo.cellBatch + ","
               + r.stepInfo.creator + "," + r.stepInfo.memo + "\n";
    }

    JZFile f;
    if (!f.open(path, "w"))
        return 0;
    f.write(csv.data(), (int)csv.size());
    f.close();
    return 1;
}

namespace {

// ---------------------------------------------------------------------------
// 行为接线(菜单动作 / 页签联动 / 设备列表 / 查询·删除·分选 / 测试页)
// 说明: 需要改 net 状态的地方一律捕获 **&ui**(由调用方持有, 见头文件说明);
//       只碰控件的地方捕获控件指针(它们挂在窗口树上, 活得比这些信号长)。
// ---------------------------------------------------------------------------

// 删除前的确认(原版 recordDeleteWarning)。⚠ 模态框只能在真事件循环里用 ——
// 自检走的是 queryRecords/deleteRecords 这些不带弹窗的函数, 不经过这里。
bool deleteWarning(BTEServer& ui, const std::string& what)
{
    JZMessageBox* box = JZMessageBox::question(ui.main, "删除", "确定要删除" + what + "吗?");
    const int r = box->exec();
    box->deleteLater();
    return r == JZDialog::Accepted;
}

void wire(BTEServer& ui)
{
    JZMainWindow*    main = ui.main;
    JZStackedWidget* stack = ui.stackedWidget;
    JZListWidget*    list = ui.listWidget;
    JZSpinBox*       nearDay = ui.lineNearDay;

    // ---- 菜单 ----
    // 原版: on_actionExit_triggered -> close(); on_actionAbout_triggered -> HelpDialog;
    //       on_actionChn/Eng_triggered -> 写 option.ini + switchLang
    if (JZAction* act = findAction(ui.menuBar, "退出"))
        connect(&act->triggered, [&ui]() {
            // 原版 closeEvent: 还有通道在跑(非"完成/停止")就不让退
            bool running = false;
            for (size_t i = 0; i < ui.devices.size() && !running; ++i)
            {
                const DeviceInfo& d = ui.devices[i];
                if (!d.online)
                    continue;
                for (int k = 0; k < MAX_DEVICE_ITEM; ++k)
                {
                    if (d.status[k] != Status_Complete
                        && d.status[k] != Status_Stop)
                    {
                        running = true;
                        break;
                    }
                }
            }
            if (running)
            {
                JZMessageBox* box = JZMessageBox::warning(ui.main, "",
                                    "正在运行, 请先停止所有设备通道再退出");
                box->exec();
                box->deleteLater();
                return;
            }
            if (ui.main)
                ui.main->close();
        });
    if (JZAction* act = findAction(ui.menuBar, "关于"))
        connect(&act->triggered, [&ui]() {
            // 原版 on_actionAbout_triggered: HelpDialog dlg(this); dlg.exec();
            // ⚠ 模态框只能在真事件循环里用 —— 自检不点这个菜单项。
            HelpDialog dlg;
            dlg.exec();
        });

    // 语言切换(原版 switchLang): 现在只改标题与"通道列表"标签 + 打日志, 多语言表待补
    JZLabel* channelLabel = ui.labelChannel;
    if (JZAction* act = findAction(ui.menuBar, "中文"))
        connect(&act->triggered, [main, channelLabel]() {
            if (main) main->setTitle("BTEServer");
            if (channelLabel) channelLabel->setText("通道列表");
            logLine("[语言] 切换为中文");
        });
    if (JZAction* act = findAction(ui.menuBar, "英语"))
        connect(&act->triggered, [main, channelLabel]() {
            if (main) main->setTitle("BTEServer (EN)");
            if (channelLabel) channelLabel->setText("Channel List");
            logLine("[语言] switch to English");
        });

    // ---- 左页签联动(原版 on_tabWidget_currentChanged: 切"历史数据"页把右栏切到历史页) ----
    if (ui.tabWidget)
        connect(&ui.tabWidget->currentChanged, [stack](int idx) {
            logLine("[页签] 左栏 = " + std::to_string(idx));
            if (stack)
                stack->setCurrentIndex(idx == 1 ? BTEServer::PageHistory : BTEServer::PageNormal);
        });

    // ---- 设备列表(原版 initDeviceList + on_listWidget_currentRowChanged) ----
    if (list)
        connect(&list->currentIndexChanged, [&ui, list, stack](int idx) {
            logLine("[设备] 选中 idx=" + std::to_string(idx)
                         + " netId=" + std::to_string(curNetId(list)));
            if (stack)
                stack->setCurrentIndex(BTEServer::PageNormal);

            // 换设备 ⇒ 卡片显示数量跟着换(原版 on_listWidget_currentRowChanged 里做的)
            updateChannelCardVisibility(ui);
        });

    // ---- 页切换按钮(原版工具栏: btnShowNormal/List/SplitSetting/Test) ----
    auto gotoPage = [&ui](BTEServer::Page page, const char* what) {
        if (ui.stackedWidget)
            ui.stackedWidget->setCurrentIndex(page);
        logLine(std::string("[页面] ") + what);
    };
    if (ui.btnShowNormal)
        connect(&ui.btnShowNormal->sigClicked, [gotoPage]() { gotoPage(BTEServer::PageNormal, "通道"); });
    if (ui.btnShowList)
        connect(&ui.btnShowList->sigClicked, [gotoPage]() { gotoPage(BTEServer::PageList, "列表"); });
    if (ui.btnShowSplitSetting)
        connect(&ui.btnShowSplitSetting->sigClicked, [gotoPage]() { gotoPage(BTEServer::PageSplit, "分选"); });
    if (ui.btnShowTest)
        connect(&ui.btnShowTest->sigClicked, [gotoPage]() { gotoPage(BTEServer::PageTest, "测试"); });

    // ---- 一键清除报警(原版 btnClearAlarm -> clearAlarmAll) ----
    if (ui.btnClearAlarm)
        connect(&ui.btnClearAlarm->sigClicked, [&ui]() {
            const int n = clearAlarmAll(ui);
            logLine("[报警] 一键清除: 确认了 " + std::to_string(n) + " 条报警");
        });

    // ---- 查询(原版 on_btnQuery_clicked: 条件查记录库 -> tableQuery) ----
    if (ui.btnQuery)
        connect(&ui.btnQuery->sigClicked, [&ui]() {
            const int n = queryRecords(ui);
            if (n < 0)
            {
                JZMessageBox* box = JZMessageBox::information(ui.main, "", "请先选择设备(或查询条件不正确)");
                box->exec();
                box->deleteLater();
                return;
            }
            logLine("[查询] 命中 " + std::to_string(n) + " 条");
            if (n == 0)
            {
                JZMessageBox* box = JZMessageBox::information(ui.main, "", "未找到相关数据");
                box->exec();
                box->deleteLater();
            }
        });

    // ---- 导出全部(原版: 选目录后逐条导出; 骨架写成一个 CSV) ----
    if (ui.btnExportAll)
        connect(&ui.btnExportAll->sigClicked, [&ui]() {
            const std::string dir = JZFileDialog::getExistingDirectory(ui.main, "选择导出目录");
            if (dir.empty())
                return;
            const int n = exportRecords(ui, dir);
            logLine("[导出] " + dir + (n > 0 ? " 已写出 bte_records.csv"
                                                        : " 没有可导出的记录(先查询)"));
        });

    // ---- 删除选中(删最近一次查询命中的那些; 原版按表格里的勾选框) ----
    if (ui.btnDeleteSelect)
        connect(&ui.btnDeleteSelect->sigClicked, [&ui]() {
            if (ui.queryHits.empty())
                return;
            if (!deleteWarning(ui, "选中的 " + std::to_string(ui.queryHits.size()) + " 条记录"))
                return;

            // 原版: 把勾选到的记录 id 收集起来交给 m_project.deleteRecords()
            const std::string deviceId = ui.boxDevice ? ui.boxDevice->currentText() : std::string();
            const int removed = (int)ui.queryHits.size();
            ui.project.deleteRecords(deviceId, ui.queryHits);
            ui.queryHits.clear();
            ui.splitLevels.clear();
            if (ui.tableQuery)
                ui.tableQuery->setRowCount(0);
            logLine("[删除] 选中记录 " + std::to_string(removed) + " 条");
        });

    // ---- 按时间整批删(原版 btnDeleteWeek/Month/Year) ----
    auto deleteByDay = [&ui](int days, const char* what) {
        if (!deleteWarning(ui, std::string(what)))
            return;
        const int n = deleteRecords(ui, days, std::string());
        if (ui.tableQuery)
            ui.tableQuery->setRowCount(0);
        logLine("[删除] " + std::string(what) + " -> " + std::to_string(n) + " 条");
    };
    if (ui.btnDeleteWeek)
        connect(&ui.btnDeleteWeek->sigClicked, [deleteByDay]() { deleteByDay(7, "一周前的记录"); });
    if (ui.btnDeleteMonth)
        connect(&ui.btnDeleteMonth->sigClicked, [deleteByDay]() { deleteByDay(30, "一月前记录"); });
    if (ui.btnDeleteYear)
        connect(&ui.btnDeleteYear->sigClicked, [deleteByDay]() { deleteByDay(365, "一年前记录"); });

    // ---- 删设备(原版 btnDeleteDevice: 在线设备不让删) ----
    if (ui.btnDeleteDevice)
        connect(&ui.btnDeleteDevice->sigClicked, [&ui]() {
            const std::string deviceId = ui.boxDevice ? ui.boxDevice->currentText() : std::string();
            if (deviceId.empty())
                return;

            DeviceInfo* dev = nullptr;
            for (size_t i = 0; i < ui.devices.size(); ++i)
            {
                if (ui.devices[i].deviceString == deviceId)
                    dev = &ui.devices[i];
            }
            if (dev && dev->online)
            {
                JZMessageBox* box = JZMessageBox::information(ui.main, "", "设备在线, 无法删除");
                box->exec();
                box->deleteLater();
                return;
            }
            if (!deleteWarning(ui, "设备 " + deviceId + " 的全部记录"))
                return;
            const int n = deleteRecords(ui, 0, deviceId);
            logLine("[删除] 设备 " + deviceId + " -> " + std::to_string(n) + " 条记录");
        });

    // ---- 时间范围单选(原版: 三个 radio 决定查询的时间条件; "不限"时天数框置灰) ----
    if (ui.btnNone)
        connect(&ui.btnNone->sigToggled, [nearDay]() {
            if (nearDay) nearDay->setEnabled(false);
            logLine("[时间] 不限");
        });
    if (ui.btnNear)
        connect(&ui.btnNear->sigToggled, [nearDay]() {
            if (nearDay) nearDay->setEnabled(true);
            logLine("[时间] 最近 N 天");
        });
    if (ui.btnFrom)
        connect(&ui.btnFrom->sigToggled, [&ui]() {
            if (ui.dateFrom) ui.dateFrom->setEnabled(true);
            if (ui.dateTo)   ui.dateTo->setEnabled(true);
            logLine("[时间] 指定 从..至..");
        });

    // ---- 分选(原版 btnSplitQuery / btnCopySplitList) ----
    if (ui.btnSplitQuery)
        connect(&ui.btnSplitQuery->sigClicked, [&ui]() {
            const int levels = splitRecords(ui);
            logLine(levels > 0
                                ? "[分选] 分成 " + std::to_string(levels) + " 级"
                                : "[分选] 没有可分选的记录(先查询)");
        });
    if (ui.btnCopySplitList)
        connect(&ui.btnCopySplitList->sigClicked, [&ui]() {
            // 原版是复制到剪贴板; 引擎还没有剪贴板接口 ⇒ 降级成"导出明细 CSV"
            std::string dir = JZFileDialog::getExistingDirectory(ui.main, "导出行明细");
            if (dir.empty())
                return;
            if (dir[dir.size() - 1] != '\\' && dir[dir.size() - 1] != '/')
                dir += "\\";

            std::string csv = "序号,单元-通道,放电容量(Ah),放电时间,中值电压(V),级别,电池编号\n";
            if (ui.tableSplitList)
            {
                for (int r = 0; r < ui.tableSplitList->rowCount(); ++r)
                {
                    for (int c = 0; c < ui.tableSplitList->columnCount(); ++c)
                    {
                        if (c == 5)      // 恒流比列跳过
                            continue;
                        csv += ui.tableSplitList->itemText(r, c) + ",";
                    }
                    csv += "\n";
                }
            }
            JZFile f;
            if (f.open(dir + "split_list.csv", "w"))
            {
                f.write(csv.data(), (int)csv.size());
                f.close();
                logLine("[分选] 明细已导出 " + dir + "split_list.csv");
            }
        });

    // ---- 分选阈值(原版 QDoubleSpinBox -> 引擎的 JZDoubleSpinBox): 证明小数信号通 ----
    if (ui.spinSplitThreshold)
        connect(&ui.spinSplitThreshold->sigValueChanged, [](double v) {
            char buf[64];
            snprintf(buf, sizeof(buf), "[分选] 阈值 = %.1f", v);
            logLine(buf);
        });

    // ---- 测试页: 端口重开 + 收发清空(原版 btnResetServer / btnClearSend / btnClearRecv) ----
    if (ui.btnResetServer)
        connect(&ui.btnResetServer->sigClicked, [&ui]() {
            const int port = ui.linePort ? atoi(ui.linePort->text().c_str()) : 0;
            if (port <= 0)
            {
                logLine("[网络] 端口不合法: " + (ui.linePort ? ui.linePort->text() : std::string()));
                return;
            }
            if (ui.server)
                ui.server->close();
            if (ui.server->listen(port))
            {
                ui.serverPort = ui.server->port();
                logLine("[网络] 已重新监听, 端口 = " + std::to_string(ui.serverPort));
            }
            else
            {
                logLine("[网络] 监听 " + std::to_string(port) + " 失败");
            }
        });
    if (ui.btnClearSend)
        connect(&ui.btnClearSend->sigClicked, [&ui]() { if (ui.textSend) ui.textSend->setText(""); });
    if (ui.btnClearRecv)
        connect(&ui.btnClearRecv->sigClicked, [&ui]() { if (ui.textRecv) ui.textRecv->setText(""); });
}

} // namespace

// ===========================================================================
// 对外: 建界面(生成代码) + 归类 + 接线
// ===========================================================================
// 对外的工步下发(薄壳): 实现在匿名命名空间里(submitStepSettingInternal) —— 导出这一层
// 是为了让自检能跨 TU 直接调它, 不必弹对话框。见 BTEServer.h 的声明。
int submitStepSetting(BTEServer& ui, DeviceInfo& dev, const std::vector<int>& channels)
{
    return submitStepSettingInternal(ui, dev, channels);
}

void buildBTEServer(BTEServer& ui, int serverPort, const std::string& projectDir)
{
    ui.gen.setupUi();          // ← BTEServer.ui 生成的代码: 建整棵树(含菜单/属性)
    if (!ui.gen.root)
    {
        ui.error = "生成的 setupUi() 没有建出根控件";
        return;
    }

    // ---- 归一下类(生成的成员本身就是类型化的, 这里只是省掉 gen. 那一层) ----
    ui.main = ui.gen.main;
    ui.menuBar = ui.main ? ui.main->menuBar() : nullptr;
    ui.listWidget = ui.gen.listWidget;
    ui.tabWidget = ui.gen.tabWidget;
    ui.stackedWidget = ui.gen.stackedWidget;
    ui.labelChannel = ui.gen.labelChannel;

    ui.boxDevice = ui.gen.boxDevice;
    ui.lineQueryUnit = ui.gen.lineQueryUnit;
    ui.dateFrom = ui.gen.dateFrom;
    ui.dateTo = ui.gen.dateTo;
    ui.btnNone = ui.gen.btnNone;
    ui.btnNear = ui.gen.btnNear;
    ui.lineNearDay = ui.gen.lineNearDay;
    ui.tableQuery = ui.gen.tableQuery;
    ui.tableList = ui.gen.tableList;
    ui.tableSplitLevel = ui.gen.tableSplitLevel;
    ui.tableSplitList = ui.gen.tableSplitList;
    ui.btnQuery = ui.gen.btnQuery;
    ui.btnExportAll = ui.gen.btnExportAll;
    ui.btnDeleteSelect = ui.gen.btnDeleteSelect;
    ui.btnDeleteWeek = ui.gen.btnDeleteWeek;
    ui.btnDeleteMonth = ui.gen.btnDeleteMonth;
    ui.btnDeleteYear = ui.gen.btnDeleteYear;
    ui.btnDeleteDevice = ui.gen.btnDeleteDevice;
    ui.btnFrom = ui.gen.btnFrom;
    ui.lineQueryChannel = ui.gen.lineQueryChannel;
    ui.lineCellNum = ui.gen.lineCellNum;
    ui.lineCellPatch = ui.gen.lineCellPatch;
    ui.lineCreator = ui.gen.lineCreator;
    ui.spinSplitThreshold = ui.gen.spinSplitThreshold;
    ui.btnSplitQuery = ui.gen.btnSplitQuery;
    ui.btnCopySplitList = ui.gen.btnCopySplitList;
    ui.btnShowNormal = ui.gen.btnShowNormal;
    ui.btnShowList = ui.gen.btnShowList;
    ui.btnShowSplitSetting = ui.gen.btnShowSplitSetting;
    ui.btnShowTest = ui.gen.btnShowTest;
    ui.btnClearAlarm = ui.gen.btnClearAlarm;
    ui.linePort = ui.gen.linePort;
    ui.btnResetServer = ui.gen.btnResetServer;
    ui.boxRecordPack = ui.gen.boxRecordPack;
    ui.boxShowRT = ui.gen.boxShowRT;
    ui.boxShowDF = ui.gen.boxShowDF;
    ui.textSend = ui.gen.textSend;
    ui.textRecv = ui.gen.textRecv;
    ui.btnClearSend = ui.gen.btnClearSend;
    ui.btnClearRecv = ui.gen.btnClearRecv;
    ui.itemWidgets = ui.gen.itemWidgets;

    // 原版左栏是 maximumSize.width = 240(原版 .ui 里那个匿名 widget 的 maximumSize);
    // 方言还没有 maximumSize 属性, 在代码里设(等价)。
    if (ui.gen.widgetLeft)
        ui.gen.widgetLeft->setMaximumWidth(240);

    // 工步名表/保护码文案(原版在 net/netPackDefine.cpp 的 NetPackRegist() 里初始化, 启动时一次)
    PackDefine::init();

    // ---- 原版构造函数的骨干序列(能做的部分; 行号对应 BTEServer.cpp) ----
    initTable(ui);                                         // 原版: :88 initTable()
    initDeviceListMenu(ui);                                // 原版: :115 listWidget 右键菜单
    initChannelGrid(ui);                                   // 原版: :119 通道卡片网格
    initShortCuts(ui);                                     // 原版: :142 initShortCuts()
    initRubberBand(ui);                                    // 框选选择层(原版: m_selectWidget)
    rebuildDeviceList(ui);                                 // 原版: :166 initDeviceList()(此时还没设备)
    if (ui.tabWidget)
        ui.tabWidget->setCurrentIndex(0);                  // 原版: tabWidget->setCurrentIndex(0)("当前"页)
    if (ui.stackedWidget)
        ui.stackedWidget->setCurrentIndex(BTEServer::PageNormal);

    // 查询页的日期默认今天(原版 on_tabWidget_currentChanged 里设的)
    if (ui.dateFrom)
        ui.dateFrom->setDate(JZDateTime::currentDateTime());
    if (ui.dateTo)
        ui.dateTo->setDate(JZDateTime::currentDateTime());
    if (ui.linePort)
        ui.linePort->setText(std::to_string(serverPort));

    wire(ui);
    initServer(ui, serverPort);                            // 原版: :168-169 m_server.start()/init(8001)
    initHeartTimer(ui);                                    // 原版: :171 m_heartTimer.start(1000)

    // 原版构造: :82-84 g_project = &m_project; m_project.init(); loadProject();
    // 目录口径按原版 BTEProject::init(): **exe 同目录**下的 Data/(applicationDirPath),
    // 不是工作目录 —— 否则从别处启动会把工程数据撒在源码树里。
    // 里面放 project.dat + record/<设备>.db。显式传了 projectDir 就按传的来。
    std::string dir = projectDir;
    if (dir.empty())
        dir = jzApp->applicationDirPath() + "/Data";
    ui.project.init(dir);
    g_project = &ui.project;

    // 原版 :159 m_alarm = new AlarmPlayer();
    ui.alarm = new AlarmPlayer();

    logLine("[骨架] BTEServer 界面已建(由 BTEServer.ui 生成: 设备列表/页签/堆叠页/菜单/表格/通道卡片)");
    logLine("[骨架] 已接: 实时帧/记录点/日志 + 通道启停接续跳步 + 查询·删除·分选 + 测试页收发");
    logLine("[骨架] 已接: 记录库(sqlite)/工程(BTEProject)、报警音(AlarmPlayer)、帮助/改IP 对话框");
    logLine("[骨架] 已接: 快捷键 Ctrl+A 全选通道(原版 initShortCuts; 用 JZAction::setShortcut + 窗口分发)");
    logLine("[骨架] 已接: 框选(拖拽卡片画框多选; 原版 m_selectWidget, 引擎里靠 grabMouse 实现)");
    logLine("[骨架] TODO: 工步下发(SET_STEP76_Ex)与工步设置界面、分选模板界面、多语言");

    // 原版 `#define HIDE_TEST_PAGE 1` ⇒ ui.btnShowTest->hide(): 测试页按钮默认不露(开发调试用)
    if (ui.btnShowTest)
        ui.btnShowTest->setVisible(false);

    // 初始没设备/没报警 ⇒ "清除报警"先收起来(真有报警时 refreshAlarmState 会放出来)
    updateClearAlarmButton(ui);
}

// ---------------------------------------------------------------------------
// 设备信息到达(与 socket 无关的那半; 头文件里有声明, 自检直接调它)
// ---------------------------------------------------------------------------
void applyDeviceInfo(BTEServer& ui, int netId, uint8_t deviceId, const stru_BTS_CMD_SEND_INFO& info,
                     const std::string& ip)
{
    
    const std::string name = info.deviceString(ip);

    // 重名(同一台设备重连): 把旧那条按离线处理(原版: closeConnect(old_id) + disconnect(old_id))
    for (size_t i = 0; i < ui.devices.size(); ++i)
    {
        DeviceInfo& old = ui.devices[i];
        if (old.netId != netId && old.deviceString == name && old.online)
        {
            old.online = false;
            logLine("[网络] 设备重连, 旧连接 netId=" + std::to_string(old.netId) + " 置离线");
        }
    }

    DeviceInfo* dev = nullptr;
    for (size_t i = 0; i < ui.devices.size(); ++i)
    {
        if (ui.devices[i].netId == netId)
            dev = &ui.devices[i];
    }
    if (!dev)
    {
        DeviceInfo d;
        d.netId = netId;
        ui.devices.push_back(d);
        dev = &ui.devices.back();
    }

    dev->deviceId = deviceId;
    dev->deviceString = name;
    dev->online = true;
    dev->heartMs = nowMs();
    dev->info = info;
    dev->channelNum = info.channelNum();

    // 原版 initDevice(): 按设备的通道数决定亮几张卡片(上面已置 online, 口径与它一致)
    updateChannelCardVisibility(ui);

    rebuildDeviceList(ui);

    char buf[200];
    snprintf(buf, sizeof(buf), "[协议] 设备信息: %s (版本 %u, 充电量程 %u mA, 电压量程 %u, 通道 %d)",
             name.c_str(), (unsigned)info.Version, (unsigned)info.ChargeMax,
             (unsigned)info.VoltageMax, (int)dev->channelNum);
    logLine(buf);
}
