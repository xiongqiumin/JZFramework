#ifndef BTE_RECORD_DATA_BASE_H_
#define BTE_RECORD_DATA_BASE_H_

// ---------------------------------------------------------------------------
// BTERecordDataBase.h —— 对应原版 bteserver/BTEServer/BTERecordDataBase.h
// (BTEStepInfo / BTERecordInfo / BTERecord / BTERecordDataBase)。
//
// 原版的形状: 每台设备一个 sqlite 文件, 记录信息(recordinfo_table)与记录点数据
// (record_table)都按 blob 存, 存之前用 zip_mem 压缩; 另有 32 张"实时备份表"
// (record_real_table_%d) 给正在跑的通道用。这里照搬**这套表结构与接口**, 差异:
//   * QDataStream -> ByteWriter/ByteReader(PackDefine.h 里那对小游标);
//   * 压缩(zip_mem/unzip_mem, 原版来自 zlib)先不做 —— blob 直接存, 注释里记着;
//   * BTEStepInfo 的 step(下发的工步内容, stru_BTS_CMD_SET_STEP76_Ex)已经搬过来了,
//     序列化方式见 .cpp 的 writeStep76Ex/readStep76Ex(只存有效参数 + 64 位位图,
//     与原版"节省空间只保存有效数据"同一思路; 追加在 writeRecordInfo 末尾以兼容旧数据)。
//   * sqlite3 用的是引擎自带的 amalgamation(JZDatabase/sqlite3.c, 已编进 JZFramework)。
// ---------------------------------------------------------------------------

#include "PackDefine.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

// SQLite 走框架自己的 C++ 封装(见 JZDatabase/JZSqlDatabase.h): 本文件(及其使用方)不再
// include sqlite3.h, 也不直接调 sqlite3 的 C API(动态库下那些符号不导出)。
#include "JZSqlDatabase.h"

// BTEStepInfo(工步信息)
struct BTEStepInfo
{
    BTEStepInfo();

    int         startStepIndex = 0;   // 起始工步, 1 开始
    std::string creator;              // 创建人
    std::string cellBatch;            // 电池批号
    std::string cellNo;               // 电池编号
    std::string memo;                 // 备注

    // 真正下发的工步内容(原版同名字段)。
    // 序列化: **追加在 writeRecordInfo 的末尾**(它后面还有 dataCache, 放这里会插到中间);
    // 读的时候用 ByteReader::left() 判断有没有这一段 —— 旧库文件没有, 照样能读。
    StruSetStep76Ex step;
};

// BTERecordInfo(一条测试记录的信息; 不含数据点)
struct BTERecordInfo
{
    // 原版的内嵌 DataCache: 分选/导出要用的几个派生量
    struct DataCache
    {
        int    beginDis = -1;         // 最后一次放电起始
        int    endDis = -1;           // 最后一次放电结束
        double midDisVoltage = 0;     // 中值电压
        double endCap = 0;            // 终止充电容量
        double endDisCap = 0;         // 终止容量
        double endDisEng = 0;         // 终止能量
        int    endDisTestTime = 0;    // 终止测试时间
        int    beginChg = -1;         // 最后一次充电起始
        int    endChg = -1;           // 最后一次充电结束
        double ccRatio = 0;           // 恒流比 Qcc/(Qcc+Qcv)*100%
    };

    BTERecordInfo();

    int         recordId = -1;        // 记录 id(>= USER_START_ID 才是正式记录)
    int         itemIndex = -1;       // 通道下标
    std::string deviceId;             // ⚠ 骨架加的: 原版用"哪个设备库文件"隐式区分设备,
                                      //   查询页要按设备过滤 ⇒ 显式存一份
    int64_t     recordTime = 0;       // 记录开始时间(ms)
    int64_t     recordTimeEnd = 0;    // 记录结束时间(ms)
    int         recordNum = 0;        // 记录点数量
    BTEStepInfo stepInfo;
    DataCache   dataCache;
};

// BTERecord = 记录信息 + 记录点数据
struct BTERecord
{
    BTERecordInfo info;
    std::vector<stru_BTS_CMD_SEND_DFDATA76::Data> dataList;
};
typedef std::shared_ptr<BTERecord> BTERecordPtr;

// BTERecordDataBase
class BTERecordDataBase
{
public:
    static const int USER_START_ID = 1000;   // 0-1000 留作实时备份, 正式 id 从 1000 开始

    BTERecordDataBase();
    ~BTERecordDataBase();

    bool open(const std::string& filepath);
    void close();
    bool isOpen() const { return m_db.isOpen(); }

    void beginTransaction();
    void endTransaction();

    // 实时数据表(正在跑的通道, 掉电/退出后能续上)
    void newRealTimeRecord(int id);
    bool saveRealTimeRecord(int id, const BTERecord& info);
    bool loadRealTimeRecord(int id, BTERecord& info);

    // 历史数据表
    void newRecord(const BTERecord& info);
    void deleteRecord(int id);
    bool saveRecord(int id, const BTERecord& info);
    bool loadRecord(int id, BTERecord& info);

    std::map<int, BTERecordInfo> recordHistory;   // 公开成员, 与原版一致

private:
    void loadHistory();
    void saveHistory(const BTERecordInfo& info);
    int  getRowCount(int id);

    JZSqlDatabase m_db;                  // 记录库连接(打不开时 isOpen()==false)
    int      recordId = USER_START_ID;
    int      m_transFlag = 0;
};

#endif // BTE_RECORD_DATA_BASE_H_
