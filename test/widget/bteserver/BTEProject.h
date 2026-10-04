#ifndef BTE_PROJECT_H_
#define BTE_PROJECT_H_

// ---------------------------------------------------------------------------
// BTEProject.h —— 对应原版 bteserver/BTEServer/BTEProject.h(工程: 设备清单 + 记录管理)。
//
// 原版 BTEProject 管四样东西: ① 每台设备的配置(DeviceData: 量程/并联/工步/显示设置)
// ② 每台设备一个记录库(BTERecordDataBase) ③ 分选模板(SplitInfo) ④ 工程落盘(save/load)。
// 这里照搬这个形状, 差异:
//   * BTEStepInfo 里的工步报文(stru_BTS_CMD_SET_STEP76_Ex)还没搬 ⇒ stepList 只留"谁建的/
//     电池编号/批号/备注"这些元信息;
//   * 工程落盘用 ByteWriter 写一份简单的 project.dat(原版用 QDataStream + 备份/恢复机制);
//   * startRecords/pauseRecords/resumeRecord 这些"记录生命周期"接口保留, 但内部只做
//     "归档 + 清实时表"两步(原版还负责断电续跑的 restore/backup, 那部分等工步一起补)。
// ---------------------------------------------------------------------------

#include "BTERecordDataBase.h"
#include "PackDefine.h"
#include "Common.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

enum
{
    STATUS_OK,             // 正常
    STATUS_DEVICE_ERROR,   // 错误的设备
    STATUS_LOGIC_ERROR,    // 逻辑错误
    STATUS_PARAM_ERROR,    // 参数错误
};

// 分选模板(原版 SplitInfo): 一个模板 = 一组"容量区间 / 中值电压区间"
struct SplitInfo
{
    struct Range
    {
        Range();

        bool   vaild = false;
        double lower = 0;
        double upper = 0;
    };

    std::string name;
    std::vector<Range> rangeList[2];     // [0] 放电容量  [1] 放电中值电压
};

// 并联信息(原版 ParallelInfo)
struct ParallelInfo
{
    std::vector<int> channels;
};

// 一台设备的全部数据(原版 DeviceData)
struct DeviceData
{
    DeviceData();

    std::string          deviceId;
    stru_BTS_CMD_SEND_INFO deviceInfo;                 // 设备信息(SEND_INFO)
    BTERecordDataBase    recordDataBase;               // 数据库
    std::vector<ParallelInfo> parallelList;            // 并联信息

    BTEStepInfo   stepList[MAX_DEVICE_ITEM];           // 每通道的工步信息
    BTERecordPtr  recordList[MAX_DEVICE_ITEM];         // 每通道**当前**测试记录
    int           recordActiveFlag[MAX_DEVICE_ITEM];   // 该通道当前记录是否活跃

    int  enableItem = MAX_DEVICE_ITEM;   // 当前显示的电池数量(卡片亮几张)
    int  itemSize = 0;                   // 卡片尺寸(0 = Item_Normal, 1 = Item_Small)
};
typedef std::shared_ptr<DeviceData> DeviceDataPtr;

class BTEProject
{
public:
    BTEProject();
    ~BTEProject();

    void init(const std::string& projectDir);
    void release();
    void save();
    void load();
    void autoSave();                       // 把正在跑的记录写进"实时备份表"

    std::string projectDir() const { return m_projectDir; }

    DeviceData* deviceData(const std::string& deviceId);
    void        deleteDevice(const std::string& deviceId);
    std::vector<std::string> deviceList();

    /* 分选模板 */
    std::vector<SplitInfo> splitInfoList();
    void setSplitInfoList(const std::vector<SplitInfo>& list);

    /* 记录 */
    int  startRecords(const std::string& deviceId, const std::vector<int>& itemList);
    int  stopRecords(const std::string& deviceId, const std::vector<int>& itemList);
    int  stopRecord(const std::string& deviceId, int itemIndex);
    int  resumeRecord(const std::string& deviceId, int itemIndex);

    // 一个 DF 记录点入库(原版 record())
    int  record(const std::string& deviceId, int itemIndex,
                const stru_BTS_CMD_SEND_DFDATA76::Data& data);

    const std::map<int, BTERecordInfo>& recordList(const std::string& deviceId);   // 历史记录(不含数据点)
    BTERecordPtr historyRecord(const std::string& deviceId, int recordId);
    BTERecordPtr currentRecord(const std::string& deviceId, int itemIndex);

    void deleteRecord(const std::string& deviceId, int recordId);
    void deleteRecords(const std::string& deviceId);
    void deleteRecords(const std::string& deviceId, const std::vector<int>& recordIds);

    // 载入记录的数据点(默认只有 info, 数据是懒加载的)
    bool loadRecordData(const std::string& deviceId, BTERecordPtr record);

    // 恒流比 Qcc/(Qcc+Qcv)*100%(原版 calcCCRatio): 要整段充电工步的数据, 工步还没搬 ⇒ 返回 0
    static double calcCCRatio(const BTERecord& record);

private:
    std::string projectPath() const;
    std::string recordDBPath(const std::string& deviceId) const;
    void        archiveRecord(DeviceData* dev, int itemIndex);   // 归档: 历史表 + 清实时表

    std::string m_projectDir;
    std::map<std::string, DeviceDataPtr> m_devices;
    std::vector<SplitInfo> m_splitInfoList;
};

extern BTEProject* g_project;

#endif // BTE_PROJECT_H_
