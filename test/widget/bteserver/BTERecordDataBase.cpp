// BTERecordDataBase.cpp —— 对应原版 bteserver/BTEServer/BTERecordDataBase.cpp。
// 表结构与接口照搬原版; 序列化用 PackDefine.h 的 ByteWriter/ByteReader(替代 QDataStream),
// 压缩先不做(见头文件注释)。
#include "BTERecordDataBase.h"

#include "Common.h"       // MAX_DEVICE_ITEM(通道数)

#include "sqlite3.h"

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
    m_db = nullptr;
}

BTERecordDataBase::~BTERecordDataBase()
{
    close();
}

bool BTERecordDataBase::open(const std::string& filepath)
{
    int nRes = sqlite3_open(filepath.c_str(), &m_db);
    if (nRes == SQLITE_OK)
    {
        nRes = sqlite3_exec(m_db,
            "create table if not exists record_table(id INTEGER PRIMARY KEY,data blob)", 0, 0, nullptr);
        nRes = sqlite3_exec(m_db,
            "create table if not exists recordinfo_table(id INTEGER PRIMARY KEY,data blob)", 0, 0, nullptr);

        for (int i = 0; i < MAX_DEVICE_ITEM; ++i)
        {
            char sql[128];
            snprintf(sql, sizeof(sql),
                     "create table if not exists record_real_table_%d(id INTEGER PRIMARY KEY,data blob)", i);
            nRes = sqlite3_exec(m_db, sql, 0, 0, nullptr);
        }
    }
    loadHistory();
    return nRes == SQLITE_OK;
}

void BTERecordDataBase::close()
{
    if (m_db)
    {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

void BTERecordDataBase::beginTransaction()
{
    if (!m_db)
        return;
    if (m_transFlag == 0)
        sqlite3_exec(m_db, "begin;", 0, 0, nullptr);
    m_transFlag++;
}

void BTERecordDataBase::endTransaction()
{
    if (!m_db)
        return;
    m_transFlag--;
    if (m_transFlag == 0)
        sqlite3_exec(m_db, "commit;", 0, 0, nullptr);
}

void BTERecordDataBase::loadHistory()
{
    if (!m_db)
        return;

    recordHistory.clear();

    sqlite3_stmt* stmt = nullptr;
    const int result = sqlite3_prepare(m_db, "select * from recordinfo_table", -1, &stmt, 0);
    if (result == SQLITE_OK)
    {
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            const char* data = (const char*)sqlite3_column_blob(stmt, 1);
            const int len = sqlite3_column_bytes(stmt, 1);
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
    if (stmt)
        sqlite3_finalize(stmt);

    recordId = recordHistory.empty() ? USER_START_ID : (recordHistory.rbegin()->first + 1);
}

void BTERecordDataBase::saveHistory(const BTERecordInfo& info)
{
    if (!m_db)
        return;

    char sql[128];
    snprintf(sql, sizeof(sql), "insert or replace into recordinfo_table values(%d,?)", info.recordId);

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare(m_db, sql, -1, &stmt, 0);

    ByteWriter w;
    writeMagic(w);
    writeRecordInfo(w, info);
    const JZByteArray buffer = w.data();

    sqlite3_bind_blob(stmt, 1, buffer.data(), buffer.size(), SQLITE_STATIC);
    sqlite3_step(stmt);
    if (stmt)
        sqlite3_finalize(stmt);
}

void BTERecordDataBase::newRealTimeRecord(int id)
{
    if (!m_db)
        return;

    char sql[128];
    snprintf(sql, sizeof(sql), "delete from record_real_table_%d", id);
    sqlite3_exec(m_db, sql, nullptr, nullptr, nullptr);
}

int BTERecordDataBase::getRowCount(int id)
{
    if (!m_db)
        return -1;

    char sql[128];
    snprintf(sql, sizeof(sql), "select count(*) from record_real_table_%d", id);

    sqlite3_stmt* stmt = nullptr;
    const int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
        return -1;
    if (sqlite3_step(stmt) != SQLITE_ROW)
    {
        sqlite3_finalize(stmt);
        return -1;
    }
    const int count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

bool BTERecordDataBase::saveRealTimeRecord(int id, const BTERecord& record)
{
    if (!m_db || id < 0 || id >= MAX_DEVICE_ITEM)
        return false;

    const int start = getRowCount(id);
    if (start == (int)record.dataList.size())
        return true;
    if (start < 0)
        return false;

    char sql[128];
    snprintf(sql, sizeof(sql), "insert or replace into record_real_table_%d values(?,?)", id);

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare(m_db, sql, -1, &stmt, 0);

    for (int i = start; i < (int)record.dataList.size(); ++i)
    {
        ByteWriter w;
        writeDFData(w, record.dataList[i]);
        const JZByteArray buffer = w.data();

        sqlite3_bind_int(stmt, 1, i);
        sqlite3_bind_blob(stmt, 2, buffer.data(), buffer.size(), SQLITE_STATIC);
        const int nRes = sqlite3_step(stmt);
        sqlite3_reset(stmt);
        if (nRes != SQLITE_DONE)
        {
            sqlite3_finalize(stmt);
            return false;
        }
    }
    if (stmt)
        sqlite3_finalize(stmt);
    return true;
}

bool BTERecordDataBase::loadRealTimeRecord(int id, BTERecord& record)
{
    if (!m_db)
        return false;

    record.dataList.clear();

    char sql[128];
    snprintf(sql, sizeof(sql), "select * from record_real_table_%d", id);

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare(m_db, sql, -1, &stmt, 0);
    if (stmt)
    {
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            const char* data = (const char*)sqlite3_column_blob(stmt, 1);
            const int len = sqlite3_column_bytes(stmt, 1);
            JZByteArray buffer(data, len);

            ByteReader r(buffer);
            stru_BTS_CMD_SEND_DFDATA76::Data dfData;
            readDFData(r, dfData);
            if (r.ok())
                record.dataList.push_back(dfData);
        }
        sqlite3_finalize(stmt);
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
    if (!m_db)
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
    if (!m_db)
        return;

    char sql[256];
    snprintf(sql, sizeof(sql), "delete from record_table where id = %d", id);
    sqlite3_exec(m_db, sql, nullptr, nullptr, nullptr);

    snprintf(sql, sizeof(sql), "delete from recordinfo_table where id = %d", id);
    sqlite3_exec(m_db, sql, nullptr, nullptr, nullptr);

    recordHistory.erase(id);
}

bool BTERecordDataBase::saveRecord(int id, const BTERecord& record)
{
    if (!m_db || id < USER_START_ID)
        return false;

    char sql[128];
    snprintf(sql, sizeof(sql), "insert or replace into record_table values(%d,?)", id);

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare(m_db, sql, -1, &stmt, 0);

    ByteWriter w;
    writeMagic(w);
    w.i32((int32_t)record.dataList.size());
    for (size_t i = 0; i < record.dataList.size(); ++i)
        writeDFData(w, record.dataList[i]);
    const JZByteArray buffer = w.data();

    sqlite3_bind_blob(stmt, 1, buffer.data(), buffer.size(), SQLITE_STATIC);
    const int nRes = sqlite3_step(stmt);
    if (stmt)
        sqlite3_finalize(stmt);

    return nRes == SQLITE_DONE || nRes == SQLITE_OK || nRes == SQLITE_ROW;
}

bool BTERecordDataBase::loadRecord(int id, BTERecord& record)
{
    if (!m_db || id < USER_START_ID)
        return false;

    char sql[128];
    snprintf(sql, sizeof(sql), "select * from record_table where id = %d", id);

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare(m_db, sql, -1, &stmt, 0);

    bool find = false;
    if (stmt && sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char* data = (const char*)sqlite3_column_blob(stmt, 1);
        const int len = sqlite3_column_bytes(stmt, 1);
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
    if (stmt)
        sqlite3_finalize(stmt);
    return find;
}
