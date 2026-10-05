// BTEProject.cpp —— 对应原版 bteserver/BTEServer/BTEProject.cpp。
// 形状照搬; 工步相关的部分(SET_STEP76_Ex / 断电续跑的 backup-restore)等工步一起补,
// 现在只做"设备清单 + 记录库 + 分选模板 + 工程落盘"。
#include "BTEProject.h"

#include "DataTrans.h"
#include "JZDir.h"
#include "JZFile.h"

#include <cstdio>

BTEProject* g_project = nullptr;

// ---------------------------------------------------------------------------
// SplitInfo / DeviceData
// ---------------------------------------------------------------------------
SplitInfo::Range::Range()
{
    vaild = false;
    lower = upper = 0;
}

DeviceData::DeviceData()
{
    enableItem = MAX_DEVICE_ITEM;
    itemSize = 0;
    for (int i = 0; i < MAX_DEVICE_ITEM; ++i)
        recordActiveFlag[i] = 0;
}

// ---------------------------------------------------------------------------
// BTEProject
// ---------------------------------------------------------------------------
BTEProject::BTEProject()
{
}

BTEProject::~BTEProject()
{
    release();
}

void BTEProject::init(const std::string& projectDir)
{
    m_projectDir = projectDir;

    // 注: JZDir 的成员 mkpath/remove 把**相对路径**解析为"相对 m_path"; 这里要相对**当前目录**
    //     建 project 目录, 所以用无参 JZDir(path 为空 = 当前目录)。
    JZDir dir;
    dir.mkpath(projectDir);
    dir.mkpath(projectDir + "/record");

    load();
}

void BTEProject::release()
{
    save();
    m_devices.clear();
}

std::string BTEProject::projectPath() const
{
    return m_projectDir + "/project.dat";
}

std::string BTEProject::recordDBPath(const std::string& deviceId) const
{
    // 设备名形如 "127.0.0.1-5V10A": 点换成下划线, 免得和扩展名混
    std::string name = deviceId;
    for (size_t i = 0; i < name.size(); ++i)
    {
        if (name[i] == '.' || name[i] == '/' || name[i] == '\\')
            name[i] = '_';
    }
    return m_projectDir + "/record/" + name + ".db";
}

void BTEProject::save()
{
    if (m_projectDir.empty())
        return;

    ByteWriter w;
    w.i32((int32_t)m_devices.size());
    std::map<std::string, DeviceDataPtr>::iterator it = m_devices.begin();
    for (; it != m_devices.end(); ++it)
    {
        const int len = (int)it->first.size();
        w.i32(len);
        w.raw(it->first.data(), len);
        w.i32(it->second->enableItem);
        w.i32(it->second->itemSize);
    }

    JZFile f;
    if (f.open(projectPath(), "w"))
    {
        const JZByteArray data = w.data();
        f.write(data.data(), data.size());
        f.close();
    }
}

void BTEProject::load()
{
    if (m_projectDir.empty())
        return;

    JZFile f;
    if (!f.open(projectPath(), "r"))
        return;

    const JZByteArray data = f.readAll();
    f.close();
    if (data.isEmpty())
        return;

    ByteReader r(data);
    const int count = r.i32();
    for (int i = 0; i < count && r.ok(); ++i)
    {
        const int len = r.i32();
        const JZByteArray nameBytes = r.raw(len);
        if (!r.ok())
            break;

        std::string deviceId(nameBytes.data(), (size_t)len);
        DeviceData* dev = deviceData(deviceId);          // 顺手把记录库打开
        if (dev && r.ok())
        {
            dev->enableItem = r.i32();
            dev->itemSize = r.i32();
        }
    }
}

// 原版 autoSave(): 定时把正在跑的记录写进"实时备份表"(掉电/退出后能续上)
void BTEProject::autoSave()
{
    std::map<std::string, DeviceDataPtr>::iterator it = m_devices.begin();
    for (; it != m_devices.end(); ++it)
    {
        DeviceData* dev = it->second.get();
        for (int i = 0; i < MAX_DEVICE_ITEM; ++i)
        {
            if (dev->recordActiveFlag[i] && dev->recordList[i])
            {
                BTERecordPtr rec = dev->recordList[i];
                rec->info.recordNum = (int)rec->dataList.size();
                dev->recordDataBase.saveRealTimeRecord(i, *rec);
            }
        }
    }
}

DeviceData* BTEProject::deviceData(const std::string& deviceId)
{
    std::map<std::string, DeviceDataPtr>::iterator it = m_devices.find(deviceId);
    if (it != m_devices.end())
        return it->second.get();

    DeviceDataPtr dev(new DeviceData());
    dev->deviceId = deviceId;
    dev->recordDataBase.open(recordDBPath(deviceId));
    m_devices[deviceId] = dev;
    return dev.get();
}

void BTEProject::deleteDevice(const std::string& deviceId)
{
    std::map<std::string, DeviceDataPtr>::iterator it = m_devices.find(deviceId);
    if (it == m_devices.end())
        return;

    it->second->recordDataBase.close();

    const std::string path = recordDBPath(deviceId);
    JZFile::remove(path);
    m_devices.erase(it);
}

std::vector<std::string> BTEProject::deviceList()
{
    std::vector<std::string> list;
    std::map<std::string, DeviceDataPtr>::iterator it = m_devices.begin();
    for (; it != m_devices.end(); ++it)
        list.push_back(it->first);
    return list;
}

std::vector<SplitInfo> BTEProject::splitInfoList()
{
    return m_splitInfoList;
}

void BTEProject::setSplitInfoList(const std::vector<SplitInfo>& list)
{
    m_splitInfoList = list;
}

// ---------------------------------------------------------------------------
// 记录的生命周期
// ---------------------------------------------------------------------------
int BTEProject::startRecords(const std::string& deviceId, const std::vector<int>& itemList)
{
    DeviceData* dev = deviceData(deviceId);
    if (!dev)
        return STATUS_DEVICE_ERROR;

    for (size_t i = 0; i < itemList.size(); ++i)
    {
        const int itemIndex = itemList[i];
        if (itemIndex < 0 || itemIndex >= MAX_DEVICE_ITEM)
            continue;

        BTERecordPtr rec(new BTERecord());
        rec->info.itemIndex = itemIndex;
        rec->info.deviceId = deviceId;
        rec->info.stepInfo = dev->stepList[itemIndex];
        rec->info.recordTime = 0;
        dev->recordList[itemIndex] = rec;
        dev->recordActiveFlag[itemIndex] = 1;
        dev->recordDataBase.newRealTimeRecord(itemIndex);
    }
    return STATUS_OK;
}

// 归档一条: 写进历史表(record_table + recordinfo_table)并清掉实时备份
void BTEProject::archiveRecord(DeviceData* dev, int itemIndex)
{
    if (!dev || itemIndex < 0 || itemIndex >= MAX_DEVICE_ITEM)
        return;
    if (!dev->recordList[itemIndex] || dev->recordList[itemIndex]->dataList.empty())
        return;

    BTERecordPtr rec = dev->recordList[itemIndex];
    rec->info.recordNum = (int)rec->dataList.size();

    dev->recordDataBase.newRecord(*rec);
    dev->recordDataBase.newRealTimeRecord(itemIndex);

    dev->recordList[itemIndex].reset();
    dev->recordActiveFlag[itemIndex] = 0;
}

int BTEProject::stopRecords(const std::string& deviceId, const std::vector<int>& itemList)
{
    DeviceData* dev = deviceData(deviceId);
    if (!dev)
        return STATUS_DEVICE_ERROR;

    for (size_t i = 0; i < itemList.size(); ++i)
        archiveRecord(dev, itemList[i]);
    return STATUS_OK;
}

int BTEProject::stopRecord(const std::string& deviceId, int itemIndex)
{
    DeviceData* dev = deviceData(deviceId);
    if (!dev)
        return STATUS_DEVICE_ERROR;
    archiveRecord(dev, itemIndex);
    return STATUS_OK;
}

// 原版 resumeRecord: 上位机异常中断后, 通道还在跑 ⇒ 把上次那份实时备份继续用
int BTEProject::resumeRecord(const std::string& deviceId, int itemIndex)
{
    DeviceData* dev = deviceData(deviceId);
    if (!dev)
        return STATUS_DEVICE_ERROR;
    if (!dev->recordActiveFlag[itemIndex])
        dev->recordDataBase.loadRealTimeRecord(itemIndex, *(dev->recordList[itemIndex]));
    return STATUS_OK;
}

// ---------------------------------------------------------------------------
// 一个记录点入库(原版 record())
// ---------------------------------------------------------------------------
int BTEProject::record(const std::string& deviceId, int itemIndex,
                       const stru_BTS_CMD_SEND_DFDATA76::Data& data)
{
    DeviceData* dev = deviceData(deviceId);
    if (!dev)
        return STATUS_DEVICE_ERROR;
    if (itemIndex < 0 || itemIndex >= MAX_DEVICE_ITEM)
        return STATUS_PARAM_ERROR;

    BTERecordPtr rec = dev->recordList[itemIndex];
    if (!rec)
    {
        rec.reset(new BTERecord());
        rec->info.itemIndex = itemIndex;
        rec->info.deviceId = deviceId;
        rec->info.stepInfo = dev->stepList[itemIndex];
        rec->info.recordTime = data.timeMSec();
        rec->info.dataCache.beginDis = 0;
        dev->recordList[itemIndex] = rec;
        dev->recordActiveFlag[itemIndex] = 1;
    }

    rec->dataList.push_back(data);
    rec->info.recordNum = (int)rec->dataList.size();
    rec->info.recordTimeEnd = data.timeMSec();

    // 派生量(原版在 dataCache 里维护, 分选/导出要用):
    //   ⚠ 原版按"充/放电工步"区分(PackDefine::StepChargeType —— 这张表骨架早就有);
    //     这里一律按放电记, 缺的是"通道当前处于哪个工步"这条数据(要等记录级工步接入,
    //     见 calcCCRatio 的说明)。
    rec->info.dataCache.endDisCap = DataTrans::fromCapUnit(data.DisChargeCap);
    rec->info.dataCache.endDisEng = DataTrans::fromEngUnit(data.DisChargeEng);
    rec->info.dataCache.midDisVoltage = DataTrans::fromVoltageUnit(data.Voltage);
    rec->info.dataCache.endDisTestTime = (int)(data.TestTime / 1000);
    rec->info.dataCache.endDis = (int)rec->dataList.size() - 1;

    return STATUS_OK;
}

const std::map<int, BTERecordInfo>& BTEProject::recordList(const std::string& deviceId)
{
    static std::map<int, BTERecordInfo> empty;
    DeviceData* dev = deviceData(deviceId);
    return dev ? dev->recordDataBase.recordHistory : empty;
}

BTERecordPtr BTEProject::historyRecord(const std::string& deviceId, int recordId)
{
    DeviceData* dev = deviceData(deviceId);
    if (!dev)
        return BTERecordPtr();

    std::map<int, BTERecordInfo>::iterator it = dev->recordDataBase.recordHistory.find(recordId);
    if (it == dev->recordDataBase.recordHistory.end())
        return BTERecordPtr();

    BTERecordPtr rec(new BTERecord());
    rec->info = it->second;
    return rec;
}

BTERecordPtr BTEProject::currentRecord(const std::string& deviceId, int itemIndex)
{
    DeviceData* dev = deviceData(deviceId);
    if (!dev || itemIndex < 0 || itemIndex >= MAX_DEVICE_ITEM)
        return BTERecordPtr();

    if (!dev->recordList[itemIndex])
    {
        dev->recordList[itemIndex].reset(new BTERecord());
        dev->recordList[itemIndex]->info.itemIndex = itemIndex;
        dev->recordList[itemIndex]->info.deviceId = deviceId;
    }
    return dev->recordList[itemIndex];
}

bool BTEProject::loadRecordData(const std::string& deviceId, BTERecordPtr record)
{
    if (!record)
        return false;
    DeviceData* dev = deviceData(deviceId);
    if (!dev)
        return false;
    return dev->recordDataBase.loadRecord(record->info.recordId, *record);
}

void BTEProject::deleteRecord(const std::string& deviceId, int recordId)
{
    DeviceData* dev = deviceData(deviceId);
    if (dev)
        dev->recordDataBase.deleteRecord(recordId);
}

void BTEProject::deleteRecords(const std::string& deviceId)
{
    DeviceData* dev = deviceData(deviceId);
    if (!dev)
        return;

    std::vector<int> ids;
    std::map<int, BTERecordInfo>::iterator it = dev->recordDataBase.recordHistory.begin();
    for (; it != dev->recordDataBase.recordHistory.end(); ++it)
        ids.push_back(it->first);
    deleteRecords(deviceId, ids);
}

void BTEProject::deleteRecords(const std::string& deviceId, const std::vector<int>& recordIds)
{
    DeviceData* dev = deviceData(deviceId);
    if (!dev)
        return;

    dev->recordDataBase.beginTransaction();
    for (size_t i = 0; i < recordIds.size(); ++i)
        dev->recordDataBase.deleteRecord(recordIds[i]);
    dev->recordDataBase.endTransaction();
}

double BTEProject::calcCCRatio(const BTERecord& record)
{
    // 原版: 取最后一段充电工步的数据, 恒流段容量 / (恒流+恒压)容量 * 100。
    // 工步报文(记录里已有 stepInfo.step)与类型表(PackDefine::StepChargeType, 骨架早就搬了)
    // 都齐了; 还没接的是"按充电段切 dataCache/dataList"这一步 ⇒ 先返回 0(见头文件注释)。
    (void)record;
    return 0.0;
}
