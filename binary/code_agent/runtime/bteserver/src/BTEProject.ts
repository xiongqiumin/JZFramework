// ---------------------------------------------------------------------------
// BTEProject.js —— 对应 C++ BTEServer/BTEProject.h + BTEProject.cpp
//
// 工程数据: 设备列表(DeviceData) + 分选模板(SplitInfo) + 记录(BTERecord)的读写与备份。
//
// QDataStream 约定同 BTERecordDataBase.js: `s << x` -> `s.write(x)`, `s >> x` -> `x = s.read()`。
//
// Qt -> JS: QMap<QString, DeviceDataPtr> -> Map; QSharedPointer -> 直接引用;
//           QDataStream -> write/read; QFile/QDir -> fs(core/dist/index.js);
//           QApplication::applicationDirPath() -> 保留调用; QVector<int> -> 数组;
//           std::array<Range, MAX_SPLIT_LEVEL> rangeList[2] -> 二维数组。
// ---------------------------------------------------------------------------

import { fs, path } from 'core/dist/index.js';
import { MAX_DEVICE_ITEM, MAX_SPLIT_LEVEL } from './Common.js';
import { formatDateTime } from './datetime.js';
import { DataTrans } from './DataTrans.js';
import { PackDefine, STEP_CHARGE, STEP_DISCHARGE } from './PackDefine.js';
import {
    BTERecordDataBase, BTERecord, BTERecordInfo, BTEStepInfo, USER_START_ID,
} from './BTERecordDataBase.js';
// 存档的 JSON 编解码(取代原来的 QDataStream operator<</>> 系列)
import {
    STORAGE_VERSION, saveJsonFile, loadJsonFile,
    splitInfoToJson, splitInfoSetJson,
    deviceDataToJson, deviceDataSetJson,
} from './storage.js';
// stru_* 报文包类: 网络报文的字节布局由它们负责(与存档无关)
import { stru_BTS_CMD_SEND_INFO } from './structs.js';
import { QApplication, QDir, QFile } from './qtcompat.js';

// ---- BTEProject.h 里的状态码 ----
export const STATUS_OK = 0;            // 正常
export const STATUS_DEVICE_ERROR = 1;  // 错误的设备
export const STATUS_LOGIC_ERROR = 2;   // 逻辑错误
export const STATUS_PARAM_ERROR = 3;   // 参数错误

// ---------------------------------------------------------------------------
// struct SplitInfo
// ---------------------------------------------------------------------------
export class SplitInfo
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // struct Range { Range(); bool vaild; double lower; double upper; };
    static get Range() { return SplitInfo_Range; }

    constructor()
    {
        this.name = "";
        // std::array<Range, MAX_SPLIT_LEVEL> rangeList[2];  // 放电容量, 放电中值电压
        this.rangeList = [new Array(MAX_SPLIT_LEVEL), new Array(MAX_SPLIT_LEVEL)];
        for (let i = 0; i < 2; i++)
            for (let j = 0; j < MAX_SPLIT_LEVEL; j++)
                this.rangeList[i][j] = new SplitInfo_Range();
    }
}

class SplitInfo_Range
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // SplitInfo::Range::Range()
    constructor()
    {
        this.vaild = false;
        this.lower = 0;
        this.upper = 0;

        this.vaild = false;
        this.upper = this.lower = 0;
    }
}

// 注: 原来的 `operator<<(QDataStream&, SplitInfo&)` / `operator>>` 已删除 ——
//     存档改用 JSON, 编解码在 storage.js: splitInfoToJson / splitInfoSetJson。

// ---------------------------------------------------------------------------
// struct ParallelInfo
// ---------------------------------------------------------------------------
export class ParallelInfo
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor()
    {
        this.channels = [];   // QVector<int> channels;
    }
}

// 注: 原来的 writeParallelInfo/readParallelInfo 已删除 —— 见 storage.parallelInfoToJson。

// ---------------------------------------------------------------------------
// struct DeviceData
// ---------------------------------------------------------------------------
export class DeviceData
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // DeviceData();
    constructor()
    {
        this.deviceId = "";                                  // QString deviceId;
        this.deviceInfo = new stru_BTS_CMD_SEND_INFO();      // stru_BTS_CMD_SEND_INFO deviceInfo; 设备信息列表
        this.recordDataBase = new BTERecordDataBase();       // BTERecordDataBase recordDataBase; 数据库
        this.parallelList = [];                              // QVector<ParallelInfo> parallelList; 并联信息

        this.stepList = new Array(MAX_DEVICE_ITEM);          // BTEStepInfo stepList[MAX_DEVICE_ITEM]; 通道数,工步信息列表
        this.recordList = new Array(MAX_DEVICE_ITEM);        // BTERecordPtr recordList[MAX_DEVICE_ITEM]; 当前测试数据
        this.recordLoadFlag = new Array(MAX_DEVICE_ITEM);    // int recordLoadFlag[MAX_DEVICE_ITEM]; 当前测试数据是否加载
        this.recordActiveFlag = new Array(MAX_DEVICE_ITEM);  // int recordActiveFlag[MAX_DEVICE_ITEM]; 当前测试数据是否活动

        this.enableItem = 0;   // 当前显示的电池数量
        this.itemSize = 0;

        for (let i = 0; i < MAX_DEVICE_ITEM; i++)
        {
            this.recordList[i] = new BTERecord();
            this.recordList[i].info.itemIndex = i;
            this.recordList[i].info.recordId = -1;

            this.recordLoadFlag[i] = false;
            this.recordActiveFlag[i] = false;
        }
        this.enableItem = MAX_DEVICE_ITEM;
        this.itemSize = 0;
    }

    // double maxCurrent();   最大电流
    maxCurrent()
    {
        return (this.deviceInfo.ChargeMax / 1000.0);
    }

    // double maxVoltage();   最大电压
    maxVoltage()
    {
        return this.deviceInfo.VoltageMax;
    }

    // double maxCapacity();  最大容量
    maxCapacity()
    {
        return 100000;
    }

    // double maxTempature();
    maxTempature()
    {
        if (this.deviceInfo.TempatureMax === 0)
            return 100000;
        else
            return this.deviceInfo.TempatureMax;
    }

    // int scale(int itemIndex);
    scale(itemIndex)
    {
        for (let i = 0; i < this.parallelList.length; i++)
        {
            if (this.parallelList[i].channels.indexOf(itemIndex) >= 0)
                return this.parallelList[i].channels.length;
        }
        return 1;
    }
}

// 注: 原来的 `operator<<(QDataStream&, DeviceData&)` / `operator>>` 已删除 ——
//     存档改用 JSON: storage.js 的 deviceDataToJson / deviceDataSetJson
//     (deviceInfo 不再 pack 成字节流塞进去, 直接按字段存)。

// ---------------------------------------------------------------------------
// class BTEProject
// ---------------------------------------------------------------------------
export class BTEProject
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // BTEProject();
    constructor()
    {
        this.m_projectDir = "";
        this.m_devices = new Map();      // QMap<QString,DeviceDataPtr> m_devices;
        this.m_splitInfoList = [];       // QVector<SplitInfo> m_splitInfoList; 分选模版
        this.m_backData = "";            // 原 QByteArray: autoSave 用来比对"存档内容是否变化"
    }

    // ~BTEProject();
    dispose()
    {
        this.release();
    }

    // void init();
    init()
    {
        this.m_projectDir = QApplication.applicationDirPath() + "/Data";
        if (!new QDir(this.m_projectDir).exists())
            new QDir().mkdir(this.m_projectDir);

        const recordPath = this.recordDBPath();
        if (!new QDir(recordPath).exists())
            new QDir().mkdir(recordPath);

        const backup = this.recordBackupPath();
        if (!new QDir(backup).exists())
            new QDir().mkdir(backup);
    }

    // void release();
    release()
    {
        for (const ptr of this.m_devices.values())
            ptr.recordDataBase.close();
    }

    // QString projectPath();
    projectPath()
    {
        return this.m_projectDir + "/BET.prj";
    }

    // QString recordDBPath();
    recordDBPath()
    {
        return this.m_projectDir + "/DataBase";
    }

    // QString recordBackupPath();
    recordBackupPath()
    {
        return this.m_projectDir + "/BackUp";
    }

    // 工程存档整体 <-> 普通 JS 对象(取代原来的 sizeList() 魔数表 + saveToStream(QDataStream&))
    //
    // 原实现往 .prj 里写的是 QDataStream 二进制流, 开头还塞了一串"各类型 sizeof/BYTE_OF"魔数
    // 用来版本校验。存储格式不要求兼容, 所以这里直接 JSON:
    //   { v: 版本号, devices: [DeviceData...], splitInfoList: [SplitInfo...] }
    // 魔数表随之取消 —— 版本兼容交给 v(见 storage.js)。
    toJson()
    {
        const devices = [];
        for (const value of this.m_devices.values())
            devices.push(deviceDataToJson(value));

        const splits = [];
        for (let i = 0; i < this.m_splitInfoList.length; i++)
            splits.push(splitInfoToJson(this.m_splitInfoList[i]));

        return { v: STORAGE_VERSION, devices: devices, splitInfoList: splits };
    }

    fromJson(j)
    {
        if (!j)
            return false;

        const devices = j.devices || [];
        for (let i = 0; i < devices.length; i++)
        {
            const data = new DeviceData();
            deviceDataSetJson(data, devices[i], () => new BTEStepInfo());
            this.m_devices.set(data.deviceId, data);
        }

        this.m_splitInfoList = [];
        const splits = j.splitInfoList || [];
        for (let i = 0; i < splits.length; i++)
        {
            const info = new SplitInfo();
            splitInfoSetJson(info, splits[i]);
            this.m_splitInfoList.push(info);
        }
        return true;
    }

    // void save();
    save()
    {
        saveJsonFile(fs, this.projectPath(), this.toJson());

        /* 备份实时数据 */
        for (const key of this.m_devices.keys())
        {
            const db = this.m_devices.get(key).recordDataBase;
            this.backupRecords(key);
        }
    }

    // void load();
    load()
    {
        const j = loadJsonFile(fs, this.projectPath());
        if (j && j.v !== STORAGE_VERSION)
            console.error("[BTEProject] 存档版本不匹配(v=" + j.v + "), 按当前版本尽力读");
        this.fromJson(j);

        // #ifdef _DEBUG
        if (isDebug())
        {
            if (this.m_devices.size === 0)
            {
                const data = new DeviceData();
                data.deviceId = "127.0.0.1";
                this.m_devices.set(data.deviceId, data);
            }
        }

        // 打开数据库
        for (const ptr of this.m_devices.values())
        {
            ptr.recordDataBase.open(this.recordDBPath() + "/" + ptr.deviceId + ".db");
        }

        this.m_backData = JSON.stringify(this.toJson());   // autoSave 的比对基线
    }

    // void autoSave();
    autoSave()
    {
        const cur = JSON.stringify(this.toJson());
        if (cur !== this.m_backData)               // 原 tmpData != m_backData
        {
            console.log("Project auto save");
            this.save();

            this.m_backData = cur;
        }
    }

    // DeviceData *deviceData(QString deviceId);
    deviceData(deviceId)
    {
        if (this.m_devices.has(deviceId))
            return this.m_devices.get(deviceId);

        const setting = new DeviceData();
        setting.deviceId = deviceId;

        // 初始化工步
        const info = new BTEStepInfo();
        info.step.genDefaultStep();

        for (let i = 0; i < MAX_DEVICE_ITEM; i++)
            setting.stepList[i] = info;

        // 打开数据库
        const dbpath = this.recordDBPath() + "/" + deviceId + ".db";
        if (QFile.exists(dbpath))   // 删除原有数据库
            QFile.remove(dbpath);
        setting.recordDataBase.open(dbpath);

        this.m_devices.set(deviceId, setting);
        return setting;
    }

    // void deleteDevice(QString deviceId);
    deleteDevice(deviceId)
    {
        if (!this.m_devices.has(deviceId))
            return;

        this.m_devices.get(deviceId).recordDataBase.close();
        this.m_devices.get(deviceId).clear();     // m_devices[deviceId].clear();
        this.m_devices.delete(deviceId);
        QFile.remove(this.recordDBPath() + "/" + deviceId + ".db");
    }

    // QStringList deviceList();
    deviceList()
    {
        const list = [];

        for (const key of this.m_devices.keys())
        {
            list.push(key);                       // list.push_back(it.key());
        }

        return list;
    }

    // QVector<SplitInfo> splitInfoList();
    splitInfoList()
    {
        return this.m_splitInfoList;
    }

    // void setSplitInfoList(QVector<SplitInfo> list);
    setSplitInfoList(list)
    {
        this.m_splitInfoList = list;
    }

    // void backupRecord(QString id,const BTERecord &record);
    backupRecord(id, record)
    {
        const db = this.m_devices.get(id).recordDataBase;
        db.beginTransaction();
        db.saveRealTimeRecord(record.info.itemIndex, record);
        db.endTransaction();
    }

    // void backupRecords(QString id);
    backupRecords(id)
    {
        const db = this.m_devices.get(id).recordDataBase;
        db.beginTransaction();
        for (let i = 0; i < MAX_DEVICE_ITEM; i++)
            this.backupRecord(id, this.m_devices.get(id).recordList[i]);

        db.endTransaction();
    }

    // void restoreRecord(QString id, BTERecord &record);
    restoreRecord(id, record)
    {
        const db = this.m_devices.get(id).recordDataBase;
        const item_idx = record.info.itemIndex;

        db.loadRealTimeRecord(item_idx, record);
        this.m_devices.get(id).recordLoadFlag[item_idx] = true;
    }

    // int startRecords(QString deviceId,QVector<int> itemList,const QVector<BTEStepInfo> &infoList);
    startRecords(deviceId, itemList, infoList)
    {
        // 如果正在记录当,会放弃当前记录
        const db = this.m_devices.get(deviceId).recordDataBase;
        db.beginTransaction();

        for (let i = 0; i < itemList.length; i++)
        {
            const item_idx = itemList[i];
            const record = this.m_devices.get(deviceId).recordList[item_idx];
            record.info = new BTERecordInfo();
            record.info.itemIndex = item_idx;
            record.info.recordTime = new Date();   // 原 QDateTime::currentDateTime()
            record.info.stepInfo = infoList[i];
            record.info.stepInfo.cellNo = formatDateTime(record.info.recordTime, "yyMMddhhmmss") + sprintf("%04d", item_idx + 1);
            record.dataList.length = 0;   // record->dataList.clear();

            // 备份
            this.m_devices.get(deviceId).recordActiveFlag[item_idx] = true;
            this.m_devices.get(deviceId).recordLoadFlag[item_idx] = true;

            db.newRealTimeRecord(item_idx);
        }

        db.endTransaction();
        return STATUS_OK;
    }

    // int stopRecord(QString deviceId, int item_idx);
    stopRecord(deviceId, item_idx)
    {
        const db = this.m_devices.get(deviceId).recordDataBase;
        const record = this.m_devices.get(deviceId).recordList[item_idx];
        if (record.info.recordNum === 0                 // 无数据
            || record.info.recordTimeEnd !== null)      // 说明已经保存过了(原 !isNull())
            return STATUS_LOGIC_ERROR;

        // 转换为正式路径
        record.info.recordTimeEnd = new Date();          // 原 QDateTime::currentDateTime()

        // 保存历史列表
        db.newRecord(record);

        // 备份
        this.m_devices.get(deviceId).recordActiveFlag[item_idx] = false;
        this.backupRecord(deviceId, record);

        return STATUS_OK;
    }

    // int stopRecords(QString deviceId, QVector<int> itemList);
    stopRecords(deviceId, itemList)
    {
        // 保存文件
        const db = this.m_devices.get(deviceId).recordDataBase;
        db.beginTransaction();

        for (let i = 0; i < itemList.length; i++)
        {
            // 没有开始记录
            const item_idx = itemList[i];
            this.stopRecord(deviceId, item_idx);
        }

        db.endTransaction();
        return STATUS_OK;
    }

    // int pauseRecords(QString deviceId, QVector<int> itemList);
    pauseRecords(deviceId, itemList)
    {
        const db = this.m_devices.get(deviceId).recordDataBase;
        db.beginTransaction();

        for (let i = 0; i < itemList.length; i++)
        {
            // 没有开始记录
            const item_idx = itemList[i];
            const ptr = this.m_devices.get(deviceId).recordList[item_idx];

            this.backupRecord(deviceId, ptr);
            this.m_devices.get(deviceId).recordActiveFlag[item_idx] = false;
        }

        db.endTransaction();
        return STATUS_OK;
    }

    // int resumeRecord(QString deviceId, int item);
    resumeRecord(deviceId, item_idx)
    {
        if (!this.m_devices.get(deviceId).recordLoadFlag[item_idx])
            this.restoreRecord(deviceId, this.m_devices.get(deviceId).recordList[item_idx]);

        this.m_devices.get(deviceId).recordActiveFlag[item_idx] = true;
        return STATUS_OK;
    }

    // void saveRealRecords(QString deviceId);
    saveRealRecords(deviceId)
    {
        const itemList = [];
        for (let i = 0; i < 32; i++)
        {
            // 没有开始记录
            const item_idx = i;
            this.resumeRecord(deviceId, item_idx);

            if (this.m_devices.get(deviceId).recordList[item_idx].dataList.length > 0)
                itemList.push(i);     // itemList << i;
        }

        if (itemList.length > 0)
            this.stopRecords(deviceId, itemList);
    }

    /*
        恒流比 = Qcc / (Qcc + Qcv) * 100%
        Qcc:恒流阶段容量,即电压达到工步设定电压时已充入的容量
        Qcv:恒压阶段容量,即从设定电压恒压充电至截止电流的容量
        恒流充电(无恒压阶段)恒流比为100%
    */
    // static double calcCCRatio(const BTEStepInfo &stepInfo, const QVector<Data> &dataList, int s, int e);
    static calcCCRatio(stepInfo, dataList, s, e)
    {
        if (s < 0 || e < s || e >= dataList.length)
            return 0;

        const data = dataList[e];
        const step_idx = data.StepID - 1;
        if (step_idx < 0 || step_idx >= stepInfo.step.stepList.length)
            return 0;

        const step = stepInfo.step.stepList[step_idx];
        if (!step.isVoltageVaild())
            return 100.0;

        const setVolt = DataTrans.toVoltageUnit(step.Voltage());
        const base = DataTrans.fromCapUnit(dataList[s].ChargeCap);
        const total = DataTrans.fromCapUnit(data.ChargeCap) - base;
        if (total <= 0)
            return 0;

        let qcc = total;
        for (let i = s; i <= e; i++)
        {
            if (dataList[i].Voltage >= setVolt)   // 注: C++ 里是 (double)dataList[i].Voltage
            {
                qcc = DataTrans.fromCapUnit(dataList[i].ChargeCap) - base;
                break;
            }
        }

        return qcc / total * 100.0;
    }

    // int record(QString deviceId,int item_idx,const stru_BTS_CMD_SEND_DFDATA76::Data &data);
    record(deviceId, item_idx, data)
    {
        // 没有开始记录
        if (!this.m_devices.get(deviceId).recordActiveFlag[item_idx])
            return STATUS_LOGIC_ERROR;

        const ptr = this.m_devices.get(deviceId).recordList[item_idx];
        if (data.StepID <= 0 || data.StepID > ptr.info.stepInfo.step.stepList.length)
        {
            console.error("StepID out range,StepID = %d", data.StepID);
            return STATUS_LOGIC_ERROR;
        }

        ptr.dataList.push(data);   // ptr->dataList.push_back(data);
        if (PackDefine.StepChargeType(data.StepType) === STEP_CHARGE)
        {
            ptr.info.dataCache.endCap = DataTrans.fromCapUnit(data.ChargeCap);

            // 记录位置
            if (ptr.info.dataCache.endChg >= 0 && (ptr.info.dataCache.endChg + 1 === ptr.info.recordNum)) // 连续
                ptr.info.dataCache.endChg = ptr.info.recordNum;
            else
            {
                ptr.info.dataCache.beginChg = ptr.info.recordNum;
                ptr.info.dataCache.endChg = ptr.info.recordNum;
            }

            // 计算恒流比
            if (ptr.info.dataCache.beginChg >= 0 && ptr.info.dataCache.endChg >= 0)
            {
                const s = ptr.info.dataCache.beginChg;
                const e = ptr.info.dataCache.endChg;
                ptr.info.dataCache.ccRatio = BTEProject.calcCCRatio(ptr.info.stepInfo, ptr.dataList, s, e);
            }
        }
        else if (PackDefine.StepChargeType(data.StepType) === STEP_DISCHARGE)
        {
            // 记录位置
            if (ptr.info.dataCache.endDis >= 0 && (ptr.info.dataCache.endDis + 1 === ptr.info.recordNum)) // 连续
            {
                ptr.info.dataCache.endDis = ptr.info.recordNum;
            }
            else
            {
                ptr.info.dataCache.beginDis = ptr.info.recordNum;
                ptr.info.dataCache.endDis = ptr.info.recordNum;
            }

            // 计算
            if (ptr.info.dataCache.beginDis >= 0 && ptr.info.dataCache.endDis >= 0)
            {
                // auto cmp = [](const Data &d1, const Data &d2)->bool { return d1.DisChargeCap < d2.DisChargeCap; };
                const cmp = (d1, d2) => (d1.DisChargeCap < d2.DisChargeCap);

                const s = ptr.info.dataCache.beginDis;
                const e = ptr.info.dataCache.endDis;

                // 计算放电中值电压
                const midCap = ptr.dataList[((s + e) / 2) | 0];
                // auto rangeRet = std::equal_range(dataList.begin()+s, dataList.begin()+e+1, midCap, cmp);
                const rangeFirst = lowerBound(ptr.dataList, s, e + 1, midCap, cmp);
                let mid_idx = rangeFirst;
                mid_idx = qMax(mid_idx, s);
                mid_idx = qMin(mid_idx, e);

                ptr.info.dataCache.midDisVoltage = DataTrans.fromVoltageUnit(ptr.dataList[mid_idx].Voltage);
                ptr.info.dataCache.endDisCap = DataTrans.fromCapUnit(ptr.dataList[e].DisChargeCap);
                ptr.info.dataCache.endDisEng = DataTrans.fromEngUnit(ptr.dataList[e].DisChargeEng);
                ptr.info.dataCache.endDisTestTime = ptr.dataList[e].TestTime;
            }
        }
        ptr.info.recordNum++;

        return STATUS_OK;
    }

    // void setRTData(QString deviceId, int item_idx, const stru_BTS_CMD_SEND_RTDATA76 &data);
    setRTData(deviceId, item_idx, data)
    {
        const ptr = this.m_devices.get(deviceId).recordList[item_idx];
        ptr.info.rtData = data;
    }

    // const QMap<int,BTERecordInfo> &recordList(QString deviceId);  记录数量，不含实时
    recordList(deviceId)
    {
        const db = this.m_devices.get(deviceId).recordDataBase;
        return db.recordHistory;
    }

    // BTERecordPtr currentRecord(QString deviceId, int item_idx);
    currentRecord(deviceId, item_idx)
    {
        return this.m_devices.get(deviceId).recordList[item_idx];
    }

    // BTERecordPtr historyRecord(QString deviceId, int record_id);
    historyRecord(deviceId, record_id)
    {
        const db = this.m_devices.get(deviceId).recordDataBase;
        if (db.recordHistory.has(record_id))
        {
            const ptr = new BTERecord();
            ptr.info = db.recordHistory.get(record_id);
            return ptr;
        }

        console.assert(false);   // Q_ASSERT(0);
        return null;             // BTERecordPtr()
    }

    // void deleteRecords(QString deviceId);
    // void deleteRecords(QString deviceId, QVector<int> record_ids);
    //   C++ 的两个重载在 JS 里合成一个函数: 不传 record_ids 时先收集全部记录号。
    deleteRecords(deviceId, record_ids)
    {
        if (record_ids === undefined)
        {
            record_ids = [];
            const list = this.m_devices.get(deviceId).recordDataBase.recordHistory;
            for (const key of list.keys())
            {
                record_ids.push(key);   // record_ids << it.key();
            }
        }

        const db = this.m_devices.get(deviceId).recordDataBase;
        db.beginTransaction();
        for (let i = 0; i < record_ids.length; i++)
            this.deleteRecord(deviceId, record_ids[i]);
        db.endTransaction();
    }

    // void deleteRecord(QString deviceId, int record_id);
    deleteRecord(deviceId, record_id)
    {
        const db = this.m_devices.get(deviceId).recordDataBase;
        db.deleteRecord(record_id);
    }

    // bool loadRecordData(QString deviceId, BTERecordPtr record);
    loadRecordData(deviceId, record)
    {
        if (!this.m_devices.has(deviceId))
            return false;

        if (record.info.recordId >= USER_START_ID) // 非实时
        {
            const db = this.m_devices.get(deviceId).recordDataBase;
            db.loadRecord(record.info.recordId, record);
        }
        else
        {
            const item_idx = record.info.itemIndex;
            if (!this.m_devices.get(deviceId).recordLoadFlag[item_idx])
                this.restoreRecord(deviceId, record);
        }

        return true;
    }
}

// extern class BTEProject *g_project;
export let g_project = null;

// C++ 里 BTEServer 构造中执行 "g_project = &m_project;"。
// ESM 不允许给 import 进来的绑定赋值, 因此改用这个 setter。
export function setGProject(project)
{
    g_project = project;
}

// ---------------------------------------------------------------------------
// 小工具
//
// 原有一批"存档魔数"辅助函数(sizeof_* / BYTE_OF / equalIntVector / byteArrayEqual)已删除:
// 它们在 C++ 里是给 QDataStream 二进制存档做"按类型算长度 + 比对版本"用的,
// 现在存档是 JSON, 版本兼容交给 storage.STORAGE_VERSION。
// ---------------------------------------------------------------------------
function isDebug() { return true; }   // 对应 C++ 的 #ifdef _DEBUG

function sprintf(format, ...args)
{
    let i = 0;
    return format.replace(/%0?(\d+)?d/g, (all, width) =>
    {
        const v = String(args[i++]);
        return width ? v.padStart(parseInt(width, 10), '0') : v;
    });
}

function qMax(a, b) { return a > b ? a : b; }
export function qMin(a, b) { return a < b ? a : b; }

// std::equal_range 的起点等价物(first): 在 [begin, end) 内找第一个不小于 midCap 的位置
function lowerBound(list, begin, end, midCap, cmp)
{
    let count = end - begin;
    while (count > 0)
    {
        const step = (count / 2) | 0;
        let it = begin + step;
        if (cmp(list[it], midCap))
        {
            begin = ++it;
            count -= step + 1;
        }
        else
            count = step;
    }
    return begin;
}
