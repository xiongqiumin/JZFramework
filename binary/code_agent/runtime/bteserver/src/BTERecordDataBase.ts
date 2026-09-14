// ---------------------------------------------------------------------------
// BTERecordDataBase.js —— 对应 C++ BTEServer/BTERecordDataBase.h + BTERecordDataBase.cpp
//
// 工步信息/记录信息/BTERecord 的序列化, 以及 sqlite 记录库(实时表 + 历史表)。
//
// **QDataStream 约定(本文件及后续文件统一)**:
//   C++ 的 `s << x` 翻译成 `s.write(x)`; `s >> x` 翻译成 `x = s.read()`。
//   这里保持的是**序列化顺序**(即真正的逻辑), stream 对象本身先不管是否存在。
//
// 依赖说明(按"先不管类是否存在"处理): 下面 import 的 stru_* 结构体类目前**尚未**
// 从 ../structs.js 导出(收口批次补齐), 现在只保证代码结构与 C++ 一致。
//
// Qt -> JS: QDataStream -> 上述 write/read; QVector<...> << 逐项 push;
//           QByteArray -> Uint8Array; QDateTime -> Date;
//           sqlite3 C API(sqlite3_open/exec/prepare/bind_blob/step/column_*)
//           按原样保留 —— 对应实现待定; zip_mem/unzip_mem 来自被排除的 gzip/,
//           同样保留调用。
// ---------------------------------------------------------------------------

import { MAX_DEVICE_ITEM } from './Common.js';
import { stru_BTS_CMD_SEND_DFDATA76, stru_BTS_CMD_SET_STEP76_Ex, stru_BTS_CMD_SEND_RTDATA76 } from './structs.js';
// 存档(记录库 blob)的 JSON 编解码 —— 取代原来的 QDataStream + recordMagic 魔数
import {
    recordInfoToJson, recordInfoSetJson,
    dfDataToJson, dfDataSetJson,
    textToBytes, bytesToText, jsonTextOf, parseJsonText,
} from './storage.js';

// extern "C" int zip_mem(char *in, int in_size, char *out, int out_size);
// extern "C" int unzip_mem(char *in, int in_size, char *out, int out_size);
// (来自 gzip/, 按用户要求未移植)

export const ZIP_BUFFER_SIZE = (20 * 1024 * 1024);
const zipBuffer = new Uint8Array(ZIP_BUFFER_SIZE);   // static char zipBuffer[ZIP_BUFFER_SIZE];

// 注: recordMagic()/sizeof_* 已删除 —— 它们是 QDataStream 二进制 blob 的"魔数+长度"版本校验,
//     现在 blob 里是 JSON, 校验退化为"能否解析"(见 storage.parseJsonText)。

// ---------------------------------------------------------------------------
// BTEStepInfo
// ---------------------------------------------------------------------------
export class BTEStepInfo
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // BTEStepInfo();
    constructor()
    {
        this.startStepIndex = 0;              // 起始工步,1开始
        this.creator = "";                    // 创建人
        this.cellBatch = "";                  // 电池批号
        this.cellNo = "";                     // 电池编号
        this.memo = "";                       // 备注
        this.step = new stru_BTS_CMD_SET_STEP76_Ex();  // 下发工步

        // 存档(JSON)读回工步包时, stepList 里的 StepInfo 得现建 ——
        // storage.js 不依赖报文层的类, 由这里提供工厂。
        this.makeStepInfo = () => new stru_BTS_CMD_SET_STEP76_Ex.StepInfo();
    }
}

// 注: 原来的 `operator<<(QDataStream&, BTEStepInfo&)` / `operator>>` 已删除 ——
//     存档改用 JSON, 编解码在 storage.js: btStepInfoToJson / btStepInfoSetJson。

// ---------------------------------------------------------------------------
// BTERecordInfo
// ---------------------------------------------------------------------------
export class BTERecordInfo
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // struct DataCache
    // {
    //     int beginDis;         //最后一次放电起始
    //     int endDis;           //最后一次放电结束
    //     double midDisVoltage; //中值电压
    //     double endCap;        //终止充电容量
    //     double endDisCap;     //终止容量
    //     double endDisEng;     //终止能量
    //     int endDisTestTime;   //终止测试时间
    //     /* 以下为新增字段,位于序列化末尾,兼容旧版本数据 */
    //     int beginChg;         //最后一次充电起始
    //     int endChg;           //最后一次充电结束
    //     double ccRatio;       //恒流比 Qcc/(Qcc+Qcv)*100%
    // };
    static get DataCache()
    {
        return BTERecordInfo_DataCache;
    }

    // BTERecordInfo();
    constructor()
    {
        this.DeviceID = 0;            // 设备id
        this.itemIndex = -1;          // 通道信息
        this.recordId = -1;           // 记录id
        this.stepInfo = null;         // 工步信息 (BTEStepInfo)
        this.recordTime = null;       // 记录开始时间 (Date; null = 未设置)
        this.recordTimeEnd = null;    // 记录结束时间 (Date; null = 未设置)
        this.recordNum = 0;           // 记录数量

        this.dataCache = new BTERecordInfo_DataCache();   // DataCache dataCache;
        this.rtData = null;           // stru_BTS_CMD_SEND_RTDATA76 rtData; 实时信息

        this.itemIndex = -1;
        this.recordId = -1;
        this.recordNum = 0;

        // memset(&rtData, 0, sizeof(rtData));
        // memset(&dataCache, 0, sizeof(dataCache));
        this.rtData = new stru_BTS_CMD_SEND_RTDATA76();
        this.dataCache.beginDis = this.dataCache.endDis = -1;
        this.dataCache.beginChg = this.dataCache.endChg = -1;

        this.stepInfo = new BTEStepInfo();
        // 原 C++: recordTime = QDateTime(); 默认构造出来是**无效时间**, 对应 JS 的 null
        // (注意不能写成 new Date() —— 那是"当前时间", 语义相反)
        this.recordTime = null;
        this.recordTimeEnd = null;
    }
}

class BTERecordInfo_DataCache
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor()
    {
        this.beginDis = 0;
        this.endDis = 0;
        this.midDisVoltage = 0;
        this.endCap = 0;
        this.endDisCap = 0;
        this.endDisEng = 0;
        this.endDisTestTime = 0;

        this.beginChg = 0;
        this.endChg = 0;
        this.ccRatio = 0;
    }
}

// 注: 原来的 `operator<<(QDataStream&, BTERecordInfo&)` / `operator>>` 已删除 ——
//     存档改用 JSON, 编解码在 storage.js: recordInfoToJson / recordInfoSetJson。
//     (旧格式"缺恒流比字段"的兼容分支随之取消: JSON 缺字段时按默认值填, 见 recordInfoSetJson)

// struct BTERecord { BTERecordInfo info; QVector<Data> dataList; };
export class BTERecord
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor()
    {
        this.info = new BTERecordInfo();
        this.dataList = [];   // QVector<stru_BTS_CMD_SEND_DFDATA76::Data> dataList;
    }
}
// typedef QSharedPointer<BTERecord> BTERecordPtr;  -> JS 直接传对象引用

// ---------------------------------------------------------------------------
// class BTERecordDataBase
// ---------------------------------------------------------------------------
export const USER_START_ID = 1000;   // static const int USER_START_ID = 1000;

export class BTERecordDataBase
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // BTERecordDataBase();
    constructor()
    {
        this.recordHistory = new Map();   // QMap<int,BTERecordInfo> recordHistory;
        this.m_db = null;                 // sqlite3 *m_db;
        this.recordId = BTERecordDataBase.USER_START_ID;  // 记录id 0-1000 留作实时备份, 正式id 从 1000 开始
        this.m_transFlag = 0;

        this.recordId = USER_START_ID;
        this.m_transFlag = 0;
        this.m_db = null;
    }

    // ~BTERecordDataBase();
    dispose()
    {
        this.close();
    }

    // bool open(QString filepath);
    open(filepath)
    {
        // sqlite3 后端尚未移植(BTERecordDataBase 里 sqlite3_* 是 C API 直调)。
        // 这里显式退化: 记录库不可用, 但**不影响**工程/存档的加载(只是没有历史记录)。
        // (测试与无界面运行都会走到这条分支)
        if (typeof sqlite3_open !== "function")
        {
            console.error("[BTERecordDataBase] sqlite3 未移植, 记录库不可用: " + filepath);
            this.m_db = null;
            return false;
        }

        let nRes = sqlite3_open(filepath, this.m_db);   // sqlite3_open(filepath.toUtf8().data(), &m_db)
        if (nRes === SQLITE_OK)
        {
            const sSQL1 = "create table if not exists record_table(id INTEGER PRIMARY KEY,data blob)";
            nRes = sqlite3_exec(this.m_db, sSQL1, 0, 0, null);

            const sSQL2 = "create table if not exists recordinfo_table(id INTEGER PRIMARY KEY,data blob)";
            nRes = sqlite3_exec(this.m_db, sSQL2, 0, 0, null);

            for (let i = 0; i < 32; i++)
            {
                const sSQL3 = sprintf("create table if not exists record_real_table_%d(id INTEGER PRIMARY KEY,data blob)", i);
                nRes = sqlite3_exec(this.m_db, sSQL3, 0, 0, null);
            }
        }
        this.loadHistory();
        return nRes === SQLITE_OK;
    }

    // void close();
    close()
    {
        if (this.m_db)
        {
            sqlite3_close(this.m_db);
            this.m_db = null;
        }
    }

    // void beginTransaction();
    beginTransaction()
    {
        if (!this.m_db)
            return;

        if (this.m_transFlag === 0)
            sqlite3_exec(this.m_db, "begin;", 0, 0, null); // 开启事务

        this.m_transFlag++;
    }

    // void endTransaction();
    endTransaction()
    {
        if (!this.m_db)
            return;

        this.m_transFlag--;
        if (this.m_transFlag === 0)
            sqlite3_exec(this.m_db, "commit;", 0, 0, null);
    }

    // void loadHistory();
    loadHistory()
    {
        if (!this.m_db)
            return;

        this.recordHistory.clear();

        const sql = sprintf("select * from recordinfo_table");

        let stmt = null;
        const result = sqlite3_prepare(this.m_db, sql, -1, stmt, 0);   // sqlite3_prepare(..., &stmt, 0)

        if (result === SQLITE_OK)                                     // 查询成功返回的是SQLITE_ROW
        {
            while (sqlite3_step(stmt) === SQLITE_ROW)
            {
                const data = sqlite3_column_blob(stmt, 1); // 读取数据，返回一个指针
                const data_len = sqlite3_column_bytes(stmt, 1);    // 返回数据大小

                // 原: QDataStream 解 + recordMagic 魔数校验; 现在 blob 就是 JSON 文本
                const j = parseJsonText(bytesToText(new Uint8Array(data, 0, data_len)));
                if (j)
                {
                    const info = new BTERecordInfo();
                    recordInfoSetJson(info, j);

                    this.recordHistory.set(info.recordId, info);
                }
            }
        }

        if (this.recordHistory.size > 0)
            this.recordId = lastKey(this.recordHistory) + 1;   // recordHistory.lastKey() + 1
        else
            this.recordId = USER_START_ID;
    }

    // void saveHistory(const BTERecordInfo &info);
    saveHistory(info)
    {
        if (!this.m_db)
            return;

        const sql = sprintf("insert or replace into recordinfo_table values(%d,?)", info.recordId);

        let stmt = null;
        sqlite3_prepare(this.m_db, sql, sql.length, stmt, 0);            // 完成对sql语句的解析

        // 原: QDataStream 写进 QByteArray; 现在直接把 JSON 文本当 blob 存
        const bytes = textToBytes(jsonTextOf(recordInfoToJson(info)));

        sqlite3_bind_blob(stmt, 1, bytes, bytes.length, null);
        sqlite3_step(stmt);
    }

    // void newRealTimeRecord(int id);
    newRealTimeRecord(id)
    {
        if (!this.m_db)
            return;

        const sql = sprintf("delete from record_real_table_%d", id);
        sqlite3_exec(this.m_db, sql, null, null, null);
    }

    // int getRowCount(int id);
    getRowCount(id)
    {
        if (!this.m_db)
            return false;

        let stmt = null;

        const sql = sprintf("select count(*) from record_real_table_%d", id);
        let rc = sqlite3_prepare_v2(this.m_db, sql, -1, stmt, null);
        if (rc !== SQLITE_OK)
            return -1;

        rc = sqlite3_step(stmt);
        if (rc !== SQLITE_ROW)
            return -1;

        return sqlite3_column_int(stmt, 0);
    }

    // bool saveRealTimeRecord(int id, const BTERecord &info);
    saveRealTimeRecord(id, record)
    {
        if (!this.m_db)
            return false;

        console.assert(id < MAX_DEVICE_ITEM);   // Q_ASSERT(id < MAX_DEVICE_ITEM);

        const start = this.getRowCount(id);
        if (start === record.dataList.length)
            return true;

        const sql = sprintf("insert or replace into record_real_table_%d values(?,?)", id);

        let stmt = null;
        sqlite3_prepare(this.m_db, sql, sql.length, stmt, 0);            // 完成对sql语句的解析
        for (let i = start; i < record.dataList.length; i++)
        {
            const bytes = textToBytes(jsonTextOf(dfDataToJson(record.dataList[i])));

            let nRes = sqlite3_bind_int(stmt, 1, i);
            nRes = sqlite3_bind_blob(stmt, 2, bytes, bytes.length, null);
            nRes = sqlite3_step(stmt);
            if (nRes !== SQLITE_DONE)
                return false;

            sqlite3_reset(stmt);
        }

        return true;
    }

    // bool loadRealTimeRecord(int id, BTERecord &info);
    loadRealTimeRecord(id, record)
    {
        if (!this.m_db)
            return false;

        record.dataList.length = 0;   // record.dataList.clear();

        const sql = sprintf("select * from record_real_table_%d", id);

        let stmt = null;
        const result = sqlite3_prepare(this.m_db, sql, -1, stmt, 0);

        if (result === SQLITE_OK)                                     // 查询成功返回的是SQLITE_ROW
        {
            while (sqlite3_step(stmt) === SQLITE_ROW)
            {
                const data = sqlite3_column_blob(stmt, 1); // 读取数据，返回一个指针
                const data_len = sqlite3_column_bytes(stmt, 1);    // 返回数据大小

                const j = parseJsonText(bytesToText(new Uint8Array(data, 0, data_len)));
                if (!j)
                    break;

                const dfData = new stru_BTS_CMD_SEND_DFDATA76.Data();
                dfDataSetJson(dfData, j);                         // 原 s >> dfData

                record.dataList.push(dfData);                     // record.dataList << dfData;
            }
        }

        if (record.info.recordNum !== record.dataList.length)
        {
            this.newRealTimeRecord(id);

            record.info.recordNum = 0;
            console.error("Load RealTime Record Failed,index = %d", id);
        }

        return true;
    }

    // void newRecord(const BTERecord &info);
    newRecord(rec)
    {
        if (!this.m_db)
            return;

        const new_info = new BTERecord();
        new_info.info = rec.info;
        new_info.info.recordId = this.recordId++;

        // 保存信息
        this.recordHistory.set(new_info.info.recordId, new_info.info);
        this.saveHistory(new_info.info);

        // 保存数据
        this.saveRecord(new_info.info.recordId, rec);
    }

    // void deleteRecord(int id);
    deleteRecord(id)
    {
        if (!this.m_db)
            return;

        let sql = sprintf("delete from record_table where id = %d", id);
        let ret = sqlite3_exec(this.m_db, sql, null, null, null);

        sql = sprintf("delete from recordinfo_table where id = %d", id);
        ret = sqlite3_exec(this.m_db, sql, null, null, null);

        this.recordHistory.delete(id);
    }

    // bool saveRecord(int id,const BTERecord &info);
    saveRecord(id, record)
    {
        if (!this.m_db)
            return false;

        console.assert(id >= USER_START_ID);   // Q_ASSERT(id >= USER_START_ID);

        const sql = sprintf("insert or replace into record_table values(%d,?)", id);

        let stmt = null;
        sqlite3_prepare(this.m_db, sql, sql.length, stmt, 0);            // 完成对sql语句的解析

        // 原: QDataStream 写 QByteArray(魔数 + dataList) 再 zip; 现在是 JSON 文本再 zip
        const list = [];
        for (let i = 0; i < record.dataList.length; i++)
            list.push(dfDataToJson(record.dataList[i]));
        const buffer = textToBytes(jsonTextOf(list));

        const zip_len = zip_mem(buffer, buffer.length, zipBuffer, ZIP_BUFFER_SIZE);
        sqlite3_bind_blob(stmt, 1, zipBuffer, zip_len, null);
        const nRes = sqlite3_step(stmt);

        return nRes === SQLITE_OK;
    }

    // bool loadRecord(int id, BTERecord &info);
    loadRecord(id, record)
    {
        if (!this.m_db)
            return false;

        console.assert(id >= USER_START_ID);   // Q_ASSERT(id >= USER_START_ID);

        const sql = sprintf("select * from record_table where id = %d", id);

        let stmt = null;
        sqlite3_prepare(this.m_db, sql, -1, stmt, 0);

        const result = sqlite3_step(stmt);
        let find = false;
        if (result === SQLITE_ROW)                                     // 查询成功返回的是SQLITE_ROW
        {
            const data = sqlite3_column_blob(stmt, 1); // 读取数据，返回一个指针
            const data_len = sqlite3_column_bytes(stmt, 1);    // 返回数据大小

            const zip_len = unzip_mem(data, data_len, zipBuffer, ZIP_BUFFER_SIZE);
            const j = parseJsonText(bytesToText(new Uint8Array(zipBuffer, 0, zip_len)));
            if (j)
            {
                record.dataList = [];                              // 原: record.dataList = s.read();
                for (let i = 0; i < j.length; i++)
                {
                    const d = new stru_BTS_CMD_SEND_DFDATA76.Data();
                    dfDataSetJson(d, j[i]);
                    record.dataList.push(d);
                }
                find = true;
            }
        }
        return find;
    }
}
BTERecordDataBase.USER_START_ID = USER_START_ID;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
// C 的 sprintf 在 JS 里没有内建对应物, 这里只覆盖本文件用到的 %d。
function sprintf(format, ...args)
{
    let i = 0;
    return format.replace(/%d/g, () => String(args[i++]));
}

// 注: equalIntVector 已删除 —— 它是给 QDataStream 存档的"魔数数组比对"用的,
//     现在 blob 是 JSON, 解析失败即视为无效(见 storage.parseJsonText)。

// QMap::lastKey()
function lastKey(map)
{
    let k = null;
    for (const key of map.keys())
        k = key;
    return k;
}
