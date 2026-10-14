// BTERecordDataBase.cpp —— 对应原版 bteserver/BTEServer/BTERecordDataBase.cpp。
// 表结构与接口照搬原版; 序列化用 PackDefine.h 的 ByteWriter/ByteReader(替代 QDataStream),
// 压缩先不做(见头文件注释)。
#include "BTERecordDataBase.h"

#include "Common.h"       // MAX_DEVICE_ITEM(通道数)

#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// blob 序列化(原版是 QDataStream 的 operator<< / >>)
// ---------------------------------------------------------------------------
namespace {

void writeString(ByteWriter& w, const std::string& s)
{
    w.i32((int32_t)s.size());
    if (!s.empty())
        w.raw(s.data(), (int)s.size());
}

void readString(ByteReader& r, std::string& s)
{
    const int len = r.i32();
    if (len <= 0 || len > (1 << 20))
    {
        s.clear();
        return;
    }
    const JZByteArray bytes = r.raw(len);
    s.assign(bytes.data(), (size_t)len);
}

// ---- 工步内容(stru_BTS_CMD_SET_STEP76_Ex 的 blob 形式) ----
// 存放位置: **writeRecordInfo 的末尾**(不是 writeStepInfo 里) —— 原版顺序中 stepInfo 之后还有
// dataCache, 塞在中间会让旧数据读不出来。参数按原版思路只存"有效的", 用 64 位位图标记。
void writeStep76Ex(ByteWriter& w, const StruSetStep76Ex& step)
{
    w.raw(step.Guid, 16);
    w.raw(step.ChannelID, 8);
    w.i32((int32_t)step.stepList.size());

    for (size_t i = 0; i < step.stepList.size(); ++i)
    {
        const StruSetStepInfo& st = step.stepList[i];

        w.u16(st.StepLine);
        w.u8(st.StepID);
        w.u8(st.StepType);

        uint64_t map = 0;
        for (int k = 0; k < (int)st.paramList.size() && k < 64; ++k)
        {
            if (st.paramList[k].vaild)
                map |= (1ull << k);
        }
        w.i64((int64_t)map);

        for (int k = 0; k < (int)st.paramList.size() && k < 64; ++k)
        {
            if (!st.paramList[k].vaild)
                continue;
            w.u8(st.paramList[k].op);
            w.f64(st.paramList[k].num);
            w.i64(st.paramList[k].i64);
        }
    }
}

void readStep76Ex(ByteReader& r, StruSetStep76Ex& step)
{
    const JZByteArray guid = r.raw(16);
    const JZByteArray ch = r.raw(8);

    memset(step.Guid, 0, sizeof(step.Guid));
    memset(step.ChannelID, 0, sizeof(step.ChannelID));
    if (guid.size() == 16)
        memcpy(step.Guid, guid.data(), 16);
    if (ch.size() == 8)
        memcpy(step.ChannelID, ch.data(), 8);

    const int count = r.i32();
    step.stepList.clear();
    if (count < 0 || count > 4096)
        return;

    for (int i = 0; i < count && r.ok(); ++i)
    {
        StruSetStepInfo st;
        st.StepLine = r.u16();
        st.StepID   = r.u8();
        st.StepType = r.u8();

        const uint64_t map = (uint64_t)r.i64();
        st.clearParams();

        for (int k = 0; k < (int)st.paramList.size() && k < 64; ++k)
        {
            if ((map & (1ull << k)) == 0)
                continue;

            st.paramList[k].vaild = true;
            st.paramList[k].op  = r.u8();
            st.paramList[k].num = r.f64();
            st.paramList[k].i64 = r.i64();
        }

        step.stepList.push_back(st);
    }
}

// BTEStepInfo
void writeStepInfo(ByteWriter& w, const BTEStepInfo& p)
{
    w.i32(p.startStepIndex);
    writeString(w, p.creator);
    writeString(w, p.cellBatch);
    writeString(w, p.cellNo);
    writeString(w, p.memo);
    // 注: 工步内容(p.step)不在这里写 —— 它追加在 writeRecordInfo 的**整个末尾**(见那边),
    //     这样旧库文件(没有这段)照样能读。
}

void readStepInfo(ByteReader& r, BTEStepInfo& p)
{
    p.startStepIndex = r.i32();
    readString(r, p.creator);
    readString(r, p.cellBatch);
    readString(r, p.cellNo);
    readString(r, p.memo);
}

// 一个记录点(原版 stru_BTS_CMD_SEND_DFDATA76::Data 的 operator<< / >>)
void writeDFData(ByteWriter& w, const stru_BTS_CMD_SEND_DFDATA76::Data& d)
{
    w.u8(d.DataFlag);   w.u8(d.SubDevID);
    w.u8(d.ChannelIDByNum); w.u8(d.AuxChlIDByNum);
    w.u32(d.TestID);    w.i32(d.DataSerial);
    w.i32(d.CycleStatus);
    w.u8(d.StepID);     w.u8(d.StepType);
    w.u8(d.StepChangeCount); w.u8(d.WorkType);
    w.i64(d.TestTime);
    w.i32(d.Voltage);   w.i32(d.Current);
    w.u16((uint16_t)d.InternalResistance);
    w.u16((uint16_t)d.Tempature);
    w.i64(d.ChargeCap); w.i64(d.DisChargeCap);
    w.i64(d.ChargeEng); w.i64(d.DisChargeEng);
    w.u16((uint16_t)d.year);
    w.u8((uint8_t)d.month); w.u8((uint8_t)d.day);
    w.u8((uint8_t)d.hour);  w.u8((uint8_t)d.minute); w.u8((uint8_t)d.second);
    w.i32(d.CurStepRange);
}

void readDFData(ByteReader& r, stru_BTS_CMD_SEND_DFDATA76::Data& d)
{
    d.DataFlag = r.u8(); d.SubDevID = r.u8();
    d.ChannelIDByNum = r.u8(); d.AuxChlIDByNum = r.u8();
    d.TestID = r.u32(); d.DataSerial = r.i32();
    d.CycleStatus = r.i32();
    d.StepID = r.u8();  d.StepType = r.u8();
    d.StepChangeCount = r.u8(); d.WorkType = r.u8();
    d.TestTime = r.i64();
    d.Voltage = r.i32(); d.Current = r.i32();
    d.InternalResistance = (int16_t)r.u16();
    d.Tempature = (int16_t)r.u16();
    d.ChargeCap = r.i64(); d.DisChargeCap = r.i64();
    d.ChargeEng = r.i64(); d.DisChargeEng = r.i64();
    d.year = r.u16();
    d.month = r.u8(); d.day = r.u8();
    d.hour = r.u8();  d.minute = r.u8(); d.second = r.u8();
    d.CurStepRange = r.i32();
}

// 校验魔数(原版 recordMagic(): 两个结构体的 sizeof, 用来识别"旧版本数据")
std::vector<int> recordMagic()
{
    std::vector<int> list;
    list.push_back(1);   // BTEStepInfo 的版本
    list.push_back(1);   // BTERecordInfo 的版本
    return list;
}

void writeMagic(ByteWriter& w)
{
    const std::vector<int> magic = recordMagic();
    w.i32((int32_t)magic.size());
    for (size_t i = 0; i < magic.size(); ++i)
        w.i32(magic[i]);
}

bool checkMagic(ByteReader& r)
{
    const int n = r.i32();
    if (n != (int)recordMagic().size())
        return false;
    for (int i = 0; i < n; ++i)
    {
        if (r.i32() != recordMagic()[i])
            return false;
    }
    return true;
}

void writeRecordInfo(ByteWriter& w, const BTERecordInfo& info)
{
    w.i32(info.itemIndex);
    w.i32(info.recordId);
    writeString(w, info.deviceId);
    w.i64(info.recordTime);
    w.i64(info.recordTimeEnd);
    w.i32(info.recordNum);
    writeStepInfo(w, info.stepInfo);

    w.i32(info.dataCache.beginDis);
    w.i32(info.dataCache.endDis);
    w.f64(info.dataCache.midDisVoltage);
    w.f64(info.dataCache.endCap);
    w.f64(info.dataCache.endDisCap);
    w.f64(info.dataCache.endDisEng);
    w.i32(info.dataCache.endDisTestTime);
    w.i32(info.dataCache.beginChg);
    w.i32(info.dataCache.endChg);
    w.f64(info.dataCache.ccRatio);

    // 工步内容: 追加在**整个记录信息的末尾** —— 旧库文件没有这一段, 读的时候用 left() 判
    writeStep76Ex(w, info.stepInfo.step);
}

void readRecordInfo(ByteReader& r, BTERecordInfo& info)
{
    info.itemIndex = r.i32();
    info.recordId = r.i32();
    readString(r, info.deviceId);
    info.recordTime = r.i64();
    info.recordTimeEnd = r.i64();
    info.recordNum = r.i32();
    readStepInfo(r, info.stepInfo);

    info.dataCache.beginDis = r.i32();
    info.dataCache.endDis = r.i32();
    info.dataCache.midDisVoltage = r.f64();
    info.dataCache.endCap = r.f64();
    info.dataCache.endDisCap = r.f64();
    info.dataCache.endDisEng = r.f64();
    info.dataCache.endDisTestTime = r.i32();
    info.dataCache.beginChg = r.i32();
    info.dataCache.endChg = r.i32();
    info.dataCache.ccRatio = r.f64();

    // 工步内容(后加的字段): 旧数据读到这儿就没了 ⇒ left()==0 时跳过, 保持向后兼容
    if (r.left() > 0)
        readStep76Ex(r, info.stepInfo.step);
}

} // namespace

// ---------------------------------------------------------------------------
// BTEStepInfo / BTERecordInfo
// ---------------------------------------------------------------------------
BTEStepInfo::BTEStepInfo()
{
    startStepIndex = 0;
}

BTERecordInfo::BTERecordInfo()
{
    itemIndex = -1;
    recordId = -1;
    recordNum = 0;
    dataCache.beginDis = dataCache.endDis = -1;
    dataCache.beginChg = dataCache.endChg = -1;
}

// ---------------------------------------------------------------------------
// BTERecordDataBase
// ---------------------------------------------------------------------------
BTERecordDataBase::BTERecordDataBase()
{
    recordId = USER_START_ID;
    m_transFlag = 0;
    // m_db(JZSqlDatabase) 默认就是"未打开"状态, 不必显式清空
}

BTERecordDataBase::~BTERecordDataBase()
{
    close();
}

bool BTERecordDataBase::open(const std::string& filepath)
{
    // 建表沿用旧口径(建表语句都带 if not exists); 失败也不致命, 由 isOpen()/后续读写表现。
    bool ok = m_db.open(filepath);
    if (!ok)
        return false;

    ok = m_db.exec("create table if not exists record_table(id INTEGER PRIMARY KEY,data blob)");
    ok = m_db.exec("create table if not exists recordinfo_table(id INTEGER PRIMARY KEY,data blob)") && ok;

    for (int i = 0; i < MAX_DEVICE_ITEM; ++i)
    {
        char sql[128];
        snprintf(sql, sizeof(sql),
                 "create table if not exists record_real_table_%d(id INTEGER PRIMARY KEY,data blob)", i);
        ok = m_db.exec(sql) && ok;
    }

    loadHistory();
    return ok;
}

void BTERecordDataBase::close()
{
    m_db.close();
}

void BTERecordDataBase::beginTransaction()
{
    if (!m_db.isOpen())
        return;
    if (m_transFlag == 0)
        m_db.exec("begin;");
    m_transFlag++;
}

void BTERecordDataBase::endTransaction()
{
    if (!m_db.isOpen())
        return;
    m_transFlag--;
    if (m_transFlag == 0)
        m_db.exec("commit;");
}

void BTERecordDataBase::loadHistory()
{
    if (!m_db.isOpen())
        return;

    recordHistory.clear();

    JZSqlQuery q;
    if (q.prepare(m_db, "select * from recordinfo_table"))
    {
        while (q.step())
        {
            int len = 0;
            const char* data = (const char*)q.columnBlob(1, &len);
            JZByteArray buffer(data, len);

            ByteReader r(buffer);
            if (!checkMagic(r))
                continue;

            BTERecordInfo info;
            readRecordInfo(r, info);
            if (r.ok())
                recordHistory[info.recordId] = info;
        }
    }

    recordId = recordHistory.empty() ? USER_START_ID : (recordHistory.rbegin()->first + 1);
}

void BTERecordDataBase::saveHistory(const BTERecordInfo& info)
{
    if (!m_db.isOpen())
        return;

    char sql[128];
    snprintf(sql, sizeof(sql), "insert or replace into recordinfo_table values(%d,?)", info.recordId);

    JZSqlQuery q;
    if (!q.prepare(m_db, sql))
        return;

    ByteWriter w;
    writeMagic(w);
    writeRecordInfo(w, info);
    const JZByteArray buffer = w.data();

    // 封装的 bind(blob) 内部是 SQLITE_TRANSIENT(拷一份) ⇒ 比原来传 SQLITE_STATIC 更稳
    q.bind(1, buffer.data(), (int)buffer.size());
    q.step();
}

void BTERecordDataBase::newRealTimeRecord(int id)
{
    if (!m_db.isOpen())
        return;

    char sql[128];
    snprintf(sql, sizeof(sql), "delete from record_real_table_%d", id);
    m_db.exec(sql);
}

int BTERecordDataBase::getRowCount(int id)
{
    if (!m_db.isOpen())
        return -1;

    char sql[128];
    snprintf(sql, sizeof(sql), "select count(*) from record_real_table_%d", id);

    JZSqlQuery q;
    if (!q.prepare(m_db, sql))
        return -1;
    if (!q.step())
        return -1;
    return q.columnInt(0);
}

bool BTERecordDataBase::saveRealTimeRecord(int id, const BTERecord& record)
{
    if (!m_db.isOpen() || id < 0 || id >= MAX_DEVICE_ITEM)
        return false;

    const int start = getRowCount(id);
    if (start == (int)record.dataList.size())
        return true;
    if (start < 0)
        return false;

    char sql[128];
    snprintf(sql, sizeof(sql), "insert or replace into record_real_table_%d values(?,?)", id);

    JZSqlQuery q;
    if (!q.prepare(m_db, sql))
        return false;

    for (int i = start; i < (int)record.dataList.size(); ++i)
    {
        ByteWriter w;
        writeDFData(w, record.dataList[i]);
        const JZByteArray buffer = w.data();

        q.bind(1, i);
        q.bind(2, buffer.data(), (int)buffer.size());
        q.step();
        if (!q.isDone())          // 原语义: step 不是 DONE 即失败
            return false;
        q.reset();                // 复用同一条 SQL: 复位游标(绑定会被下面重新覆盖)
    }
    return true;
}

bool BTERecordDataBase::loadRealTimeRecord(int id, BTERecord& record)
{
    if (!m_db.isOpen())
        return false;

    record.dataList.clear();

    char sql[128];
    snprintf(sql, sizeof(sql), "select * from record_real_table_%d", id);

    JZSqlQuery q;
    if (q.prepare(m_db, sql))
    {
        while (q.step())
        {
            int len = 0;
            const char* data = (const char*)q.columnBlob(1, &len);
            JZByteArray buffer(data, len);

            ByteReader r(buffer);
            stru_BTS_CMD_SEND_DFDATA76::Data dfData;
            readDFData(r, dfData);
            if (r.ok())
                record.dataList.push_back(dfData);
        }
    }

    // 原版: 数量对不上就认为这份实时备份坏了, 清掉重来
    if (record.info.recordNum != (int)record.dataList.size())
    {
        newRealTimeRecord(id);
        record.info.recordNum = 0;
    }
    return true;
}

void BTERecordDataBase::newRecord(const BTERecord& rec)
{
    if (!m_db.isOpen())
        return;

    BTERecord newInfo;
    newInfo.info = rec.info;
    newInfo.info.recordId = recordId++;

    recordHistory[newInfo.info.recordId] = newInfo.info;
    saveHistory(newInfo.info);
    saveRecord(newInfo.info.recordId, rec);
}

void BTERecordDataBase::deleteRecord(int id)
{
    if (!m_db.isOpen())
        return;

    char sql[256];
    snprintf(sql, sizeof(sql), "delete from record_table where id = %d", id);
    m_db.exec(sql);

    snprintf(sql, sizeof(sql), "delete from recordinfo_table where id = %d", id);
    m_db.exec(sql);

    recordHistory.erase(id);
}

bool BTERecordDataBase::saveRecord(int id, const BTERecord& record)
{
    if (!m_db.isOpen() || id < USER_START_ID)
        return false;

    char sql[128];
    snprintf(sql, sizeof(sql), "insert or replace into record_table values(%d,?)", id);

    JZSqlQuery q;
    if (!q.prepare(m_db, sql))
        return false;

    ByteWriter w;
    writeMagic(w);
    w.i32((int32_t)record.dataList.size());
    for (size_t i = 0; i < record.dataList.size(); ++i)
        writeDFData(w, record.dataList[i]);
    const JZByteArray buffer = w.data();

    q.bind(1, buffer.data(), (int)buffer.size());
    q.step();
    // 原语义: 只要不是"出错"就算成功 ⇒ 封装的 isDone()(DONE) 是它的严格子集
    return q.isDone();
}

bool BTERecordDataBase::loadRecord(int id, BTERecord& record)
{
    if (!m_db.isOpen() || id < USER_START_ID)
        return false;

    char sql[128];
    snprintf(sql, sizeof(sql), "select * from record_table where id = %d", id);

    JZSqlQuery q;
    if (!q.prepare(m_db, sql))
        return false;

    bool find = false;
    if (q.step())
    {
        int len = 0;
        const char* data = (const char*)q.columnBlob(1, &len);
        JZByteArray buffer(data, len);

        ByteReader r(buffer);
        if (checkMagic(r))
        {
            const int count = r.i32();
            record.dataList.clear();
            for (int i = 0; i < count && r.ok(); ++i)
            {
                stru_BTS_CMD_SEND_DFDATA76::Data d;
                readDFData(r, d);
                record.dataList.push_back(d);
            }
            find = r.ok();
        }
    }
    return find;
}
