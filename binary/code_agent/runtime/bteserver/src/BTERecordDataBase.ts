// ---------------------------------------------------------------------------
// BTERecordDataBase.js —— 对应 C++ BTEServer/BTERecordDataBase.h + BTERecordDataBase.cpp
//
// 工步信息/记录信息/BTERecord 的序列化, 以及 sqlite 记录库(实时表 + 历史表)。
//
// **QDataStream 约定(本文件及后续文件统一)**:
//   C++ 的 `s << x` 翻译成 `s.write(x)`; `s >> x` 翻译成 `x = s.read()`。
//   这里保持的是**序列化顺序**(即真正的逻辑), stream 对象本身先不管是否存在。
//
// 2026-09-22 变更 —— 记录库真正可用(原来整库是"不可用"的退化状态):
//   C++ 直接调 sqlite3 C API(sqlite3_open/exec/prepare/bind_blob/step/column_*),
//   引擎里没有这些全局(见 globals.d.ts 的说明), 于是 open() 只会打印
//   "sqlite3 未移植, 记录库不可用" 并返回 false —— 历史记录彻底没存。
//   现在改用**引擎内置的 `sqlite` 模块**(node:sqlite 风格的同步封装, 见
//   jz_types/sqlite/types/index.d.ts, 由 JZJSqliteModule 提供):
//
//     const db = new DatabaseSync(path);      // 打开(不存在则新建)
//     db.exec(sql);                           // 建表/事务/无结果语句
//     db.prepare(sql).run(a, b)               // 绑定 + 执行
//     db.prepare(sql).get()  /  .all()        // 取一行 / 取全部
//
//   位置参数 `?` 与具名参数都支持; JS 侧 Uint8Array <-> sqlite BLOB 双向可用
//   (引擎绑定层 sqlite3_bind_blob / SQLITE_BLOB -> Uint8Array)。
//   ⚠ BLOB 列读回来是 Uint8Array, 但历史上手工写入的可能是 TEXT ⇒ 统一过 asBytes()。
//
// 与 C++ 的差异(存档格式, 只影响"能不能读 C++ 写的老库"):
//   C++ 的 blob 是 QDataStream 二进制 + recordMagic 魔数, saveRecord 还先过
//   zip_mem 压缩(gzip/ 未移植, 引擎没有 zip/unzip); 本移植的 blob 是 **JSON 文本**
//   (见 storage.js)且**不压缩**。所以 JS 写的 .db 与 C++ 生成的 .db 互不通用 ——
//   JS 侧读写自洽(同一版本内 save/load 往返一致)。
// ---------------------------------------------------------------------------

import { MAX_DEVICE_ITEM } from './Common.js';
import { stru_BTS_CMD_SEND_DFDATA76, stru_BTS_CMD_SET_STEP76_Ex, stru_BTS_CMD_SEND_RTDATA76 } from './structs.js';
// 记录库后端: 引擎内置 sqlite 模块(node:sqlite 风格; 由用户入口被动加载同包 dist)。
// ⚠ 用**默认导入**取成员, 不写 `import { DatabaseSync } from 'sqlite'`: 随工程分发的
//   TS 7.0.2(tsgo) 不解析 ambient 里的相对通配再导出(仓库源码这份), 具名导入会 TS2614;
//   部署后的 jz_types 由 package.py 补了显式具名再导出, 两种写法都行 —— 默认导入最稳。
import sqlite from 'sqlite';
const DatabaseSync = sqlite.DatabaseSync;
// 存档(记录库 blob)的 JSON 编解码 —— 取代原来的 QDataStream + recordMagic 魔数
import {
    recordInfoToJson, recordInfoSetJson,
    dfDataToJson, dfDataSetJson,
    textToBytes, bytesToText, jsonTextOf, parseJsonText,
} from './storage.js';

// 注: zip_mem/unzip_mem(来自未移植的 gzip/)与 20MB 的 static zipBuffer 已删除 ——
//     存档改为不压缩的 JSON 文本(见文件头"与 C++ 的差异")。

// 注: recordMagic()/sizeof_* 已删除 —— 它们是 QDataStream 二进制 blob 的"魔数+长度"版本校验,
//     现在 blob 里是 JSON, 校验退化为"能否解析"(见 storage.parseJsonText)。

// sqlite 的 blob 列读回来是 Uint8Array; 兼容"曾经写成 TEXT"的库(字符串/ArrayBuffer)。
function asBytes(v): Uint8Array
{
    if (v instanceof Uint8Array)
        return v;
    if (typeof v === 'string')
        return textToBytes(v);
    if (v instanceof ArrayBuffer)
        return new Uint8Array(v);
    return new Uint8Array(0);
}

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
    // static const int USER_START_ID = 1000;
    // 声明成静态字段(原来只在类外挂了一次) —— 否则 `BTERecordDataBase.USER_START_ID`
    // 在类型层是"属性不存在", 且赋值那句会被判 TS2339。
    static USER_START_ID = USER_START_ID;

    // BTERecordDataBase();
    constructor()
    {
        this.recordHistory = new Map();   // QMap<int,BTERecordInfo> recordHistory;
        this.m_db = null;                 // sqlite3 *m_db;  ->  引擎 sqlite 模块的 DatabaseSync
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
        // 原 C++: sqlite3_open + 三条 create table(重复执行安全) + loadHistory()。
        // 引擎的 DatabaseSync 打开失败/建表失败都抛异常 ⇒ 这里编译期就不需要"未移植"守卫了。
        try
        {
            const db = new DatabaseSync(filepath);
            db.exec("create table if not exists record_table(id INTEGER PRIMARY KEY,data blob)");
            db.exec("create table if not exists recordinfo_table(id INTEGER PRIMARY KEY,data blob)");
            for (let i = 0; i < 32; i++)
            {
                db.exec(sprintf(
                    "create table if not exists record_real_table_%d(id INTEGER PRIMARY KEY,data blob)", i));
            }
            this.m_db = db;
        }
        catch (e)
        {
            console.error("[BTERecordDataBase] 打开记录库失败: " + filepath + " (" + e + ")");
            this.m_db = null;
            return false;
        }

        this.loadHistory();
        return true;
    }

    // void close();
    close()
    {
        if (this.m_db)
        {
            try
            {
                this.m_db.close();
            }
            catch (e)
            {
                console.error("[BTERecordDataBase] 关闭记录库出错: " + e);
            }
            this.m_db = null;
        }
    }

    // void beginTransaction();
    beginTransaction()
    {
        if (!this.m_db)
            return;

        if (this.m_transFlag === 0)
            this.m_db.exec("begin;"); // 开启事务

        this.m_transFlag++;
    }

    // void endTransaction();
    endTransaction()
    {
        if (!this.m_db)
            return;

        this.m_transFlag--;
        if (this.m_transFlag === 0)
            this.m_db.exec("commit;");
    }

    // void loadHistory();
    loadHistory()
    {
        if (!this.m_db)
            return;

        this.recordHistory.clear();

        // 原: sqlite3_prepare("select * from recordinfo_table") + 逐行 step + column_blob(1)
        const rows = this.m_db.prepare("select * from recordinfo_table").all();

        for (let i = 0; i < rows.length; i++)
        {
            // 原: QDataStream 解 + recordMagic 魔数校验; 现在 blob 就是 JSON 文本,
            //     解析失败(parseJsonText 返回 null)即视为该行无效
            const j = parseJsonText(bytesToText(asBytes(rows[i].data)));
            if (j)
            {
                const info = new BTERecordInfo();
                recordInfoSetJson(info, j);

                this.recordHistory.set(info.recordId, info);
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

        // 原: insert or replace into recordinfo_table values(%d,?) —— id 拼进 SQL、data 绑 ?;
        //     这里 id 也走 bind(等价, 少一处字符串拼接)。
        const bytes = textToBytes(jsonTextOf(recordInfoToJson(info)));
        try
        {
            this.m_db.prepare("insert or replace into recordinfo_table values(?,?)")
                .run(info.recordId, bytes);
        }
        catch (e)
        {
            console.error("[BTERecordDataBase] saveHistory 失败(id=" + info.recordId + "): " + e);
        }
    }

    // void newRealTimeRecord(int id);
    newRealTimeRecord(id)
    {
        if (!this.m_db)
            return;

        // 表名带通道号 ⇒ 只能拼进 SQL(值都走 bind)
        this.m_db.exec(sprintf("delete from record_real_table_%d", id));
    }

    // int getRowCount(int id);
    getRowCount(id)
    {
        if (!this.m_db)
            return false;   // 与 C++ 一致(无库时返回 false/0; 调用点都先判了 m_db)

        try
        {
            // 原 C++ 是 prepare_v2 + step + column_int; 这里 get() 取第一行(count(*) 恒有 1 行)
            const row = this.m_db.prepare(
                sprintf("select count(*) as c from record_real_table_%d", id)).get();
            return row ? row.c : -1;
        }
        catch (e)
        {
            console.error("[BTERecordDataBase] getRowCount 失败(id=" + id + "): " + e);
            return -1;
        }
    }

    // bool saveRealTimeRecord(int id, const BTERecord &info);
    saveRealTimeRecord(id, record)
    {
        if (!this.m_db)
            return false;

        console.assert(id < MAX_DEVICE_ITEM);   // Q_ASSERT(id < MAX_DEVICE_ITEM);

        const start = this.getRowCount(id);
        if (start < 0)          // 原 C++ 没有这条(prepare 失败返回 -1, 循环会从 -1 开始插)
            return false;
        if (start === record.dataList.length)
            return true;

        // 原 C++: insert or replace into record_real_table_%d values(?,?) + bind_int(i) + bind_blob
        try
        {
            const stmt = this.m_db.prepare(
                sprintf("insert or replace into record_real_table_%d values(?,?)", id));
            for (let i = start; i < record.dataList.length; i++)
            {
                const bytes = textToBytes(jsonTextOf(dfDataToJson(record.dataList[i])));
                stmt.run(i, bytes);          // StatementSync 每次 run 前自动 reset(= C++ 的 sqlite3_reset)
            }
        }
        catch (e)
        {
            console.error("[BTERecordDataBase] saveRealTimeRecord 失败(id=" + id + "): " + e);
            return false;
        }

        return true;
    }

    // bool loadRealTimeRecord(int id, BTERecord &info);
    loadRealTimeRecord(id, record)
    {
        if (!this.m_db)
            return false;

        record.dataList.length = 0;   // record.dataList.clear();

        try
        {
            const rows = this.m_db.prepare(sprintf("select * from record_real_table_%d", id)).all();
            for (let i = 0; i < rows.length; i++)
            {
                const j = parseJsonText(bytesToText(asBytes(rows[i].data)));
                if (!j)
                    break;

                const dfData = new stru_BTS_CMD_SEND_DFDATA76.Data();
                dfDataSetJson(dfData, j);                         // 原 s >> dfData

                record.dataList.push(dfData);                     // record.dataList << dfData;
            }
        }
        catch (e)
        {
            console.error("[BTERecordDataBase] loadRealTimeRecord 失败(id=" + id + "): " + e);
        }

        if (record.info.recordNum !== record.dataList.length)
        {
            this.newRealTimeRecord(id);

            record.info.recordNum = 0;
            console.error("Load RealTime Record Failed,index = " + id);
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

        try
        {
            this.m_db.exec(sprintf("delete from record_table where id = %d", id));
            this.m_db.exec(sprintf("delete from recordinfo_table where id = %d", id));
        }
        catch (e)
        {
            console.error("[BTERecordDataBase] deleteRecord 失败(id=" + id + "): " + e);
        }

        this.recordHistory.delete(id);
    }

    // bool saveRecord(int id,const BTERecord &info);
    saveRecord(id, record)
    {
        if (!this.m_db)
            return false;

        console.assert(id >= USER_START_ID);   // Q_ASSERT(id >= USER_START_ID);

        // 原: QDataStream(魔数 + dataList) 再 zip_mem 压缩进 blob;
        // 现在: dataList 的 JSON 文本直接当 blob(**不压缩** —— 引擎没有 zip; 见文件头差异说明)。
        const list = [];
        for (let i = 0; i < record.dataList.length; i++)
            list.push(dfDataToJson(record.dataList[i]));
        const bytes = textToBytes(jsonTextOf(list));

        try
        {
            this.m_db.prepare("insert or replace into record_table values(?,?)").run(id, bytes);
        }
        catch (e)
        {
            console.error("[BTERecordDataBase] saveRecord 失败(id=" + id + "): " + e);
            return false;
        }

        return true;
    }

    // bool loadRecord(int id, BTERecord &info);
    loadRecord(id, record)
    {
        if (!this.m_db)
            return false;

        console.assert(id >= USER_START_ID);   // Q_ASSERT(id >= USER_START_ID);

        let row = null;
        try
        {
            row = this.m_db.prepare(sprintf("select * from record_table where id = %d", id)).get();
        }
        catch (e)
        {
            console.error("[BTERecordDataBase] loadRecord 失败(id=" + id + "): " + e);
            return false;
        }
        if (!row)
            return false;

        // 原: 先 unzip_mem 解压再按 JSON 读; 现在 blob 就是未压缩的 JSON
        const j = parseJsonText(bytesToText(asBytes(row.data)));
        if (!j)
            return false;

        record.dataList = [];                              // 原: record.dataList = s.read();
        for (let i = 0; i < j.length; i++)
        {
            const d = new stru_BTS_CMD_SEND_DFDATA76.Data();
            dfDataSetJson(d, j[i]);
            record.dataList.push(d);
        }
        return true;
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
