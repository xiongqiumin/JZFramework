#ifndef BTE_PACK_DEFINE_H_
#define BTE_PACK_DEFINE_H_

#include "JZByteArray.h"

#include <cstdint>
#include <vector>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// PackDefine.h —— 对应原版 bteserver/common/PackDefine.h(命令字 + 报文结构)。
//
// ⚠ 一处**故意**的合并: 原版把"帧格式/组帧拆帧"放在 common/net/netPack.h + netPack.cpp
//   (PackInfo / NetPack / NetPackVariant) 与 netDataManager 里, 报文结构才在 PackDefine.h;
//   骨架合成一个文件(帧在上半, 报文在下半), 等 net 那套(NetServer/NetClient)一起搬时再拆。
//
// 帧结构(照抄 netDataManager.cpp 的 packData()/parsePack()):
//
//   ┌────────────┬──────────────┬──────────┬────────┬──────┐
//   │ 8 字节 0xFF │ PackInfo 6/7 │  body    │ 校验 1 │ 0x5A │
//   └────────────┴──────────────┴──────────┴────────┴──────┘
//
//   * PackInfo = DeviceID(1) SubDevID(1) Length(2, 小端) CmdID(2, 小端) [Result(1)];
//     Result 只在**响应类**包里有(见 isRecvCmd) ⇒ 6 或 7 字节;
//   * Length = 整帧长度, **含** 8 字节头与"校验+尾"2 字节 ⇒ 6/7 之外的部分就是 body;
//   * 校验 = body 之前那一段(PackInfo+body)的**逐字节无符号相加**(原版 CaculateSum);
//   * 尾 = 固定 0x5A(原版 packTail)。
//
// Qt -> 框架的对应物(逐条):
//   QByteArray  -> JZByteArray(JZCore; data()/size()/mid()/append()/push_back());
//   QDataStream(LittleEndian) -> ByteWriter / ByteReader(下面两个小游标; 原版没有这两个类,
//     它们是 QDataStream 的替身 —— 名字也就沿用"字节游标"这个说法, 不对齐 Qt);
//   QMap/QVector -> std::map / std::vector;
//   QTcpSocket  -> JZNet 的 JZTcpSocket(收发在 BTEServer.cpp 里接)。
//
// 已搬: 帧的组/拆 + 命令字表 + CONNECT(握手) + SEND_INFO(设备信息) + 通用回包
//   + SEND_RTDATA76(实时数据) + SEND_DFDATA76(记录点) + SEND_LOG(日志) + 通道位图
//   + 上位机下发的那几个控制包(START70 / STOP / RESUME / STEP_JUMP / SET_RTC /
//     SET_RT_INTERVAL / RESUME_CONFIRM / LED_ON) + 各自的回应解包。
// 未搬(结构大, 字段逐个对): SET_STEP76_Ex(工步下发, 变长) / PARALLEL / GET_INFO_RESP ——
//   加的时候照 stru_BTS_CMD_SEND_INFO 的样子写(unpack + packBody + 字段顺序照 PackDefine.h 的结构声明)。
// ---------------------------------------------------------------------------

// ---- 帧常量(PackDefine.h) ----
enum
{
    BTS_HEAD_LENGTH      = 8,       // BTS_HEAD_LENGTH
    BTS_SEND_HEAD_LENGTH = 6,       // BTS_SEND_HEAD_LENGTH
    BTS_RECV_HEAD_LENGTH = 7,       // BTS_RECV_HEAD_LENGTH
    BTS_TAIL_LENGTH      = 2,       // BTS_TAIL_LENGTH(校验 1 + 尾 1)
    BTS_TAIL_BYTE        = 0x5A,
};

// ---- 命令字(PackDefine.h 的 #define, 用到哪条搬哪条) ----
enum
{
    BTS_CMD_CONNECT             = 0x0000,
    BTS_CMD_CONNECT_RESP        = 0x8000,

    BTS_CMD_SEND_INFO           = 0x0072,
    BTS_CMD_SEND_INFO_RESP      = 0x00F2,

    BTS_CMD_GET_RTC             = 0x0002,
    BTS_CMD_GET_RTC_RESP        = 0x0082,
    BTS_CMD_SET_RTC             = 0x0003,
    BTS_CMD_SET_RTC_RESP        = 0x0083,

    BGS66_CMD_SET_RT_INTERVAL       = 0x005E,
    BGS66_CMD_SET_RT_INTERVAL_RESP  = 0x00DE,

    BTS_CMD_SET_STEP76_Ex       = 0x0200,
    BTS_CMD_SET_STEP_EX_RESP76  = 0x8200,

    BTS_CMD_START70             = 0x006E,
    BTS_CMD_START_RESP70        = 0x00EE,
    BTS_CMD_STOP                = 0x0011,
    BTS_CMD_STOP_RESP           = 0x0091,
    BTS_CMD_RESUME              = 0x0014,
    BTS_CMD_RESUME_RESP         = 0x0094,
    BTS_CMD_RESUME_QUERY        = 0x0035,
    BTS_CMD_RESUME_QUERY_RESP   = 0x00B5,
    BTS_CMD_RESUME_CONFIRM      = 0x0036,
    BTS_CMD_RESUME_CONFIRM_RESP = 0x00B6,
    BTS_CMD_STEP_JUMP           = 0x006F,
    BTS_CMD_STEP_JUMP_RESP      = 0x00EF,
    BTS_CMD_HEART_BEAT          = 0x0030,
    BTS_CMD_SEND_LOG            = 0x000F,
    BTS_CMD_SEND_LOG_RESP       = 0x008F,

    BTS_CMD_SEND_PARALLEL           = 0x0300,
    BTS_CMD_SEND_PARALLEL_RESP      = 0x8300,
    BTS_CMD_SEND_PARALLEL_CANCEL    = 0x0400,

    BTS_CMD_SEND_RTDEVSTATUS    = 0x0272,
    BTS_CMD_SEND_RTDATA76       = 0x07C0,
    BTS_CMD_SEND_DFDATA76       = 0x07D0,
    BTS_CMD_SEND_DFDATA_RESP76  = 0x87D0,

    BTS_CMD_LED_ON              = 0x6000,
    BTS_CMD_LED_ON_RESP         = 0x6080,

    // 写 IP / MAC(上位机 -> 中位机; 原版 #define 在 PackDefine.h 尾部)
    WRITE_DEVICE_IP             = 0x5000,
    WRITE_PC_IP                 = 0x5100,
    WRITE_DEVICE_MAC            = 0x5200,
};

// 响应类命令(包头多一个 Result 字节; 照抄 PackInfo::IsRecvPack() 的那张表)
bool isRecvCmd(uint16_t cmd);
// 命令名(日志用)
std::string cmdName(uint16_t cmd);

// ---- 协议里的特殊值 ----
enum
{
    // 无效子设备号(原版 #define INVAILD_SUB_ID 0xFe): 包头 SubDevID 原样发它,
    // 表示"这条命令不属于某个子设备"(如写 IP/MAC); 通道类命令才填 子设备号+1。
    INVAILD_SUB_ID = 0xFE,
};

// ---- 小端字节游标(替代 QDataStream) ----
class ByteWriter
{
public:
    void u8(uint8_t v);
    void u16(uint16_t v);
    void u32(uint32_t v);
    void i32(int32_t v);
    void i64(int64_t v);
    void f64(double v);       // 小端 8 字节(记录库存档里的电压/容量是 double)
    void raw(const void* data, int len);
    void bytes(const JZByteArray& b);

    const JZByteArray& data() const { return m_data; }
    JZByteArray& data() { return m_data; }

private:
    JZByteArray m_data;
};

class ByteReader
{
public:
    explicit ByteReader(const JZByteArray& data) : m_data(data) {}

    uint8_t  u8();
    uint16_t u16();
    uint32_t u32();
    int32_t  i32();
    int64_t  i64();
    double   f64();
    JZByteArray raw(int len);
    void     skip(int len);

    bool ok() const { return m_ok; }
    int  pos() const { return m_pos; }
    int  size() const { return m_data.size(); }
    int  left() const { return m_data.size() - m_pos; }

private:
    const JZByteArray& m_data;
    int  m_pos = 0;
    bool m_ok = true;
};

// 和校验(原版 CaculateSum: 逐字节无符号相加, 溢出即截断到 8 位)
uint8_t checkSum(const uint8_t* data, int len);

// ---- 包头(照抄原版 net/netPack.h 的 PackInfo; 方法名也照原版: Pack/UnPack/IsRecvPack) ----
struct PackInfo
{
    uint8_t  DeviceID = 0;
    uint8_t  SubDevID = 0;
    uint16_t Length = 0;      // 整帧长度(含 8 字节头 + 校验 + 尾)
    uint16_t CmdID = 0xFFFF;  // 原版初值 -1
    uint8_t  Result = 0;

    bool IsRecvPack() const { return isRecvCmd(CmdID); }
    int  headLength() const { return IsRecvPack() ? BTS_RECV_HEAD_LENGTH : BTS_SEND_HEAD_LENGTH; }
    int  bodyLength() const;      // Length - 8 - 2 - headLength()

    // 把 PackInfo 自身写进 buffer(按 IsRecvPack 决定要不要 Result)
    void Pack(JZByteArray& buffer) const;
    // 从 buffer 的 offset 处读 PackInfo; 数据不全返回 false(调用方保留缓冲等下一批字节)
    bool UnPack(const JZByteArray& buffer, int offset);
};

// ---- 收到的包(照抄 NetPackVariant: info + 原始 body) ----
struct NetPack
{
    PackInfo info;
    JZByteArray data;       // body 原始字节, 由具体报文结构自己解
};

// 组一帧(Length 由这里算, 与原版 send()/response() 同一口径)
JZByteArray packFrame(const PackInfo& info, const JZByteArray& body);

// 增量分帧: 从 buffer 里尽量取帧, 取到的追加到 out; 没取够的残留**留在 buffer 里**。
// 出错策略: 校验/尾/长度不合格时不整段清空, 而是跳过当前候选头继续找
// (原版是清空整段; 移植版 protocol.ts 的 FrameParser 改成"跳 1 字节重扫", 这里跟它一致,
//  更能扛脏数据)。返回取到的帧数; error 非空时写入最近一次失败原因。
int takePackets(JZByteArray& buffer, std::vector<NetPack>& out, std::string* error);

// 组一帧并返回可读的十六进制(每 16 字节一行), 日志/抓包用(原版 dispPack)
std::string hexDump(const JZByteArray& bytes);

// ---------------------------------------------------------------------------
// 报文: 连接(stru_BTS_CMD_CONNECT)
// ---------------------------------------------------------------------------
struct stru_BTS_CMD_CONNECT
{
    uint8_t ClientType = 0;      // 协议版本号

    bool unpack(const JZByteArray& body);
};

// ---------------------------------------------------------------------------
// 报文: 设备信息(stru_BTS_CMD_SEND_INFO; 字段顺序照 PackDefine.h 的结构声明)
// ---------------------------------------------------------------------------
struct stru_BTS_CMD_SEND_INFO
{
    uint16_t Version = 0;            // 版本号(0x0075 = Ver7.5)
    char     fixed[6] = { 0 };       // 6 字节固定字符
    uint8_t  Type = 0;               // 量程结构方式
    uint8_t  DataEndFlag = 0;        // 0x01 后面还有包 / 0x02 传输完毕
    uint8_t  DataIndex = 0;          // 分包索引(从 1 开始)
    uint8_t  DeviceType = 0;         // 设备类型
    uint16_t TypeNum = 0;            // 量程类型个数
    uint8_t  Channel[32] = { 0 };    // 下位机数量
    uint8_t  ChannelType = 0;        // 通道类型(01 主通道)
    uint32_t ChargeMax = 0;          // 充电电流最大量程
    uint32_t DischargeMax = 0;       // 放电电流最大量程
    uint32_t VoltageMax = 0;         // 电压量程
    uint32_t TempatureMax = 0;       // 温度量程
    uint32_t InternalResistanceMax = 0;  // 内阻量程
    uint8_t  currentNum = 0;         // 可变电流个数
    uint32_t currentRange[10] = { 0 };

    bool unpack(const JZByteArray& body);
    JZByteArray packBody() const;

    // 设备名(原版: sprintf("%s-%dV%dA", ip, VoltageMax, ChargeMax / 1000))
    std::string deviceString(const std::string& ip) const;

    // 通道数(原版 channelNum(): 数 Channel[] 前导的 0xFF, 每字节算 8 路)
    int channelNum() const;
};

// ---------------------------------------------------------------------------
// 单位换算搬到 DataTrans.h/.cpp(原版 common/DataTrans.h), 用 DataTrans::fromXxxUnit()。
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 通道位图(原版 stru_BTS_CMD_STEP_REQUEST::ChannelID[8])
//   8 字节共 64 位, 从**低位**起每 bit 一路: 第 n 路 = byte[n/8] 的 bit(n%8)。
//   对外一律用 **0 基通道下标**(0..63), 与 setChannel(idx)/item_idx%8 同一口径。
// ---------------------------------------------------------------------------
JZByteArray packChannelMask(const std::vector<int>& channels);
std::vector<int> unpackChannelMask(const JZByteArray& mask);

// ---------------------------------------------------------------------------
// 报文: 实时数据(stru_BTS_CMD_SEND_RTDATA76; 75 字节)
// ---------------------------------------------------------------------------
struct stru_BTS_CMD_SEND_RTDATA76
{
    // WorkType 的 Bit5..4(原版 workStatus())
    enum { WORK_COMPLETE = 0, WORK_RUN = 1, WORK_USER_STOP = 2, WORK_ERROR_STOP = 3 };

    int32_t  DFDataPointer = 0;   // 记录点数据指针
    uint8_t  RTDataCount = 0;     // 实时数据记录个数
    uint8_t  DataFlag = 0;        // 0x55 主通道 / 0x61.. 辅助通道
    uint8_t  ChannelIDByNum = 1;  // 通道序号, 从 1 开始
    uint8_t  AuxChlIDByNum = 0;   // 辅助通道序号, 主通道为 0
    int32_t  CycleStatus = 0;     // 循环状态
    uint8_t  StepID = 0;          // 当前工步号
    uint8_t  StepType = 0;        // 工步类型
    uint8_t  WorkType = 0;        // 工作状态(见下)
    int64_t  TestTime = 0;        // 当前工步已执行时间(ms)
    int32_t  Voltage = 0;         // 0.1mV
    int32_t  Current = 0;
    int16_t  InternalResistance = 0;
    int16_t  Tempature = 0;       // 0.1℃
    int64_t  ChargeCap = 0;
    int64_t  DisChargeCap = 0;
    int64_t  ChargeEng = 0;
    int64_t  DisChargeEng = 0;
    int32_t  CurStepRange = 0;    // 当前工步电流量程(mA)
    int32_t  PrtCode = 0;         // 保护码, 非保护为 0

    bool unpack(const JZByteArray& body);
    JZByteArray packBody() const;

    int  workStatus() const { return (WorkType >> 4) & 0x03; }
    // 通道故障判据(与 BTEServer 报警判据一致): bit7 / 安全停止 / 有保护码
    bool isFault() const;
    std::string statusText() const;
};

// ---------------------------------------------------------------------------
// 报文: 中位机日志(stru_BTS_CMD_SEND_LOG)
// ---------------------------------------------------------------------------
struct stru_BTS_CMD_SEND_LOG
{
    uint8_t  LogNumber = 0;
    uint16_t LogIndex = 0;
    uint16_t LogSerial = 0;
    uint8_t  UnitID = 0;
    uint8_t  ChannelIDByNum = 0;
    int32_t  TestID = 0;
    int      year = 2000, month = 1, day = 1, hour = 0, minute = 0, second = 0;
    uint8_t  LogID = 0;
    int32_t  LogInfo = 0;

    bool unpack(const JZByteArray& body);
    std::string timeText() const;
};

// ---------------------------------------------------------------------------
// 报文: 记录点数据(stru_BTS_CMD_SEND_DFDATA76)
//   DFDataNumber(1) + DFDataPointer(4) + n × Data(83 字节)
// ---------------------------------------------------------------------------
struct stru_BTS_CMD_SEND_DFDATA76
{
    // 原版是内嵌的 struct Data(83 字节), 名字也照原版
    struct Data
    {
        uint8_t  DataFlag = 0;        // 0x55 平台点容量 / 0x58 工步终止 / 0x59 工步起始 / 0xAA LOG / 0x6A 上传完成
        uint8_t  SubDevID = 0;
        uint8_t  ChannelIDByNum = 0;  // 从 1 开始
        uint8_t  AuxChlIDByNum = 0;
        uint32_t TestID = 0;
        int32_t  DataSerial = 0;
        int32_t  CycleStatus = 0;
        uint8_t  StepID = 0;
        uint8_t  StepType = 0;
        uint8_t  StepChangeCount = 0;
        uint8_t  WorkType = 0;
        int64_t  TestTime = 0;
        int32_t  Voltage = 0;
        int32_t  Current = 0;
        int16_t  InternalResistance = 0;
        int16_t  Tempature = 0;
        int64_t  ChargeCap = 0;
        int64_t  DisChargeCap = 0;
        int64_t  ChargeEng = 0;
        int64_t  DisChargeEng = 0;
        int      year = 2000, month = 1, day = 1, hour = 0, minute = 0, second = 0;
        int32_t  CurStepRange = 0;

        // 原版 ATime_t::toMSec(): btHour 可能 >= 24, 要按天进位
        int64_t timeMSec() const;
        std::string timeText() const;
    };

    uint8_t         DFDataNumber = 0;
    int32_t         DFDataPointer = 0;
    std::vector<Data> dataList;

    bool unpack(const JZByteArray& body);
};

// 回应(stru_BTS_CMD_SEND_DFDATA_RESP76): 原样回显数据指针
struct stru_BTS_CMD_SEND_DFDATA_RESP76
{
    int32_t Index = 0;
    JZByteArray packBody() const;
};

// ---------------------------------------------------------------------------
// 上位机 -> 中位机 的控制包(body 组包; 包头由 packFrame 填)
// 注: 通道类包的 body **不含** SubDeviceId —— 子设备号走包头的 SubDevID 字段
//     (原版 send(): SubDevID != INVAILD_SUB_ID 时 +1, 否则取 0xFF 表示"所有子设备")。
// ---------------------------------------------------------------------------
// START70: Guid(16) + ChannelID(8) + StepID(1)
JZByteArray packStart70(const std::vector<int>& channels, uint8_t stepId, const uint8_t* guid);
// STOP / RESUME: ChannelID(8)
JZByteArray packChannelMaskBody(const std::vector<int>& channels);
// STEP_JUMP: ChannelID(8) + jumpStepId(1)
JZByteArray packStepJump(const std::vector<int>& channels, uint8_t jumpStepId);
// SET_RTC: 年(从 2000)月日时分秒, 各 1 字节
JZByteArray packSetRtc(int year, int month, int day, int hour, int minute, int second);
// BGS66 SET_RT_INTERVAL: 1 字节(秒)
JZByteArray packRtInterval(uint8_t sec);
// RESUME_CONFIRM: 1 字节 result
JZByteArray packResumeConfirm(uint8_t result);
// LED_ON: 4 字节点灯
JZByteArray packLedOn(const uint8_t led[4]);

// ---------------------------------------------------------------------------
// 回应解包(上位机收)
// ---------------------------------------------------------------------------
// START_RESP70: Guid(16) + ChannelCount(1) + n × (ChannelIDByNum 1 + TestID 4)
struct stru_BTS_CMD_START_RESP70
{
    struct ChannelInfo { uint8_t ChannelIDByNum = 0; int32_t TestID = 0; };

    uint8_t                ChannelCount = 0;
    std::vector<ChannelInfo> channelInfo;

    bool unpack(const JZByteArray& body);
};

// ---------------------------------------------------------------------------
// 工步类型 / 充放电归类 / 工步名表(原版 common/PackDefine.h + net/netPackDefine.cpp)
//   卡片(BTEItemWidget)要靠它: 标题文字(StepNameList)、底色(充电绿/放电黄/其它淡青)
// ---------------------------------------------------------------------------
enum
{
    STEP_TYPE_NULL = 0,     // 未定义
    STEP_TYPE_1 = 1,        // 恒流充电
    STEP_TYPE_2 = 2,        // 恒流放电
    STEP_TYPE_3 = 3,        // 恒压充电
    STEP_TYPE_IDLE = 4,     // 静    置
    STEP_TYPE_LOOP = 5,     // 循    环
    STEP_TYPE_STOP = 6,     // 停    止
    STEP_TYPE_7 = 7,        // 恒流恒压充电
    STEP_TYPE_8 = 8,        // 恒功率放电
    STEP_TYPE_9 = 9,        // 恒功率充电
    STEP_TYPE_10 = 10,      // 恒阻放电
    STEP_TYPE_11 = 11,      // 恒阻充电
    STEP_TYPE_12 = 12,      // 测量内阻
    STEP_TYPE_13 = 13,      // 挂起工步
    STEP_TYPE_14 = 14,      // N 闭环恒压充电
    STEP_TYPE_15 = 15,      // N 闭环恒流恒压充电
    STEP_TYPE_16 = 16,      // 脉冲工步
    STEP_TYPE_17 = 17,      // 模拟工步
    STEP_TYPE_18 = 18,      // 电池组恒流恒压
    STEP_TYPE_19 = 19,      // 恒压放电
    STEP_TYPE_20 = 20,      // 恒流恒压放电
    STEP_TYPE_21 = 21,      // 控制工步
    STEP_TYPE_PARAM = 0xFE, // 参数设置
};

enum
{
    STEP_CHARGE = 0,
    STEP_DISCHARGE = 1,
    STEP_CHARGE_OTHER = 2,
};

class PackDefine
{
public:
    static std::vector<std::string>   StepNameList;   // 工步名; 下标 = STEP_TYPE_*
    static std::map<int, std::string> ErrorList;      // 保护码 -> 文案

    // 填 StepNameList(原版在 net/netPackDefine.cpp 的 NetPackRegist() 里做, 程序启动时调一次)
    static void init();

    static std::string ErrorText(int type);           // 查不到 -> "未知错误"(与原版一致)
    static int         StepChargeType(int type);
};

// ---------------------------------------------------------------------------
// 工步下发 SET_STEP76_Ex
//   对应原版 common/PackDefine.h 的 stru_BTS_CMD_SET_STEP76_Ex + common/PackDefine.cpp
//   里的 pack/unpack/genDefaultStep。前端就是"工步设置对话框"填出来的东西。
//
// 移植口径(与原版的差异, 都写在明处):
//   · 原版是"结构体 + 虚 pack() + QDataStream/QUuid + PARAM_FUNC 宏"; 这里换成同一份二进制
//     布局 + JZByteArray/ByteWriter 直写(骨架协议层一贯的写法, 见 packFrame/packStart70)。
//   · **参数不用 60 个 PARAM_FUNC 宏**, 改成通用 setParam(PARAM_xxx, op, value) —— 语义等价,
//     调用处照样能一眼看出是哪个参数。
//   · 参数值用"double + int64 两个槽"代替 QVariant: 数值类(电压/电流/温度/容量/…/整型以外的)
//     走 num, 时间(64 位)与整型走 i64 —— 打包时按参数类型取对应槽。这是唯一的表示差异。
// ---------------------------------------------------------------------------

// 参数运算符(原版同名同值)
enum {
    OP_EQU       = 0x01,   // ==
    OP_NEQ       = 0x02,   // !=
    OP_BIG       = 0x03,   // >
    OP_BIG_EQU   = 0x04,   // >=
    OP_SMALL     = 0x05,   // <
    OP_SMALL_EQU = 0x06,   // <=
};

// 工步参数下标(原版同名同值): bitmap1 = 0..31, bitmap2 = 32..63
enum {
    // ---- bitmap1 ----
    PARAM_Current = 0,          // 电流运算符(A)
    PARAM_Voltage,              // 电压运算符(V)
    PARAM_Time,                 // 时间运算符(ms)
    PARAM_Capacity,             // 容量运算符
    PARAM_Tempature,            // 温度运算符
    PARAM_Power,                // 功率运算符
    PARAM_Resistance,           // 电阻运算符
    PARAM_StopCurrent,          // 截止电流运算符
    PARAM_StopVoltage,          // 截止电压运算符
    PARAM_VoltageChange,        // -ΔV 运算符
    PARAM_BeginStepID,          // 起始工步号
    PARAM_CountLoop,            // 循环次数
    PARAM_JumpLine,             // 跳转行号
    PARAM_ReservedParam1,       // 保留参数1
    PARAM_ReservedParam2,       // 保留参数2
    PARAM_ProtectVoltageUpper = 15, // 电压安全保护上限
    PARAM_ProtectVoltageLower,      // 电压安全保护下限
    PARAM_ProtectCurrentUpper,      // 电流安全保护上限
    PARAM_ProtectCurrentLower,      // 电流安全保护下限
    PARAM_ProtectTempatureUpper,    // 温度安全保护上限
    PARAM_ProtectTempatureLower,    // 温度安全保护下限
    PARAM_ProtectDelayTime,         // 延迟保护时间
    PARAM_ProtectDetalVolTime,      // 化成变化率检测时间
    PARAM_ProtectDetalVol,          // 化成变化率电压
    PARAM_RecordTimeInterval,       // 时间间隔记录条件
    PARAM_RecordCurrentChange,      // 电流突变记录条件
    PARAM_RecordVoltageChange,      // 电压突变记录条件
    PARAM_RecordTempatureChange,    // 温度突变记录条件
    PARAM_PlatVoltage1,             // 平台点监测电压1
    PARAM_PlatVoltage2,             // 平台点监测电压2
    PARAM_PlatVoltage3,             // 平台点监测电压3

    // ---- bitmap2 ----
    PARAM_AuxChannelMap = 32,       // 映射辅助通道号
    PARAM_AuxChannel,               // 实际辅助通道号
    PARAM_AuxCndTempatureUpper,     // 辅助通道温度限制上限值
    PARAM_AuxCndTempatureLower,     // 辅助通道温度限制下限值
    PARAM_AuxCndVolUpper,           // 辅助通道电压限制上限值
    PARAM_AuxCndVolLower,           // 辅助通道电压限制下限值
    PARAM_AuxCndCurUpper,           // 辅助通道电流限制上限值
    PARAM_AuxCndCurLower,           // 辅助通道电流限制下限值
    PARAM_AuxPrtTempatureUpper,     // 辅助通道温度保护上限值
    PARAM_AuxPrtTempatureLower,     // 辅助通道温度保护下限值
    PARAM_AuxPrtVolUpper,           // 辅助通道电压保护上限值
    PARAM_AuxPrtVolLower,           // 辅助通道电压保护下限值
    PARAM_AuxPrtCurUpper,           // 辅助通道电流保护上限值
    PARAM_AuxPrtCurLower,           // 辅助通道电流保护下限值
    PARAM_AuxRecordTimeInterval,    // 辅助通道时间间隔记录条件
    PARAM_AuxRecordCurrentChange,   // 辅助通道电流记录条件
    PARAM_AuxRecordVoltageChange,   // 辅助通道电压记录条件
    PARAM_AuxRecordTempatureChange, // 辅助通道温度记录条件
    PARAM_AuxConditionTime,         // 辅助通道时间限制条件
    PARAM_AuxConditionBeginStepID,  // 起始工步号
    PARAM_AuxConditionCountLoop,    // 循环次数
    PARAM_IRDCScanTime,             // 红外扫描工步拍照时间
    PARAM_IRDCTempUpper,            // 红外扫描上限温度
    PARAM_IRDCTempLower,            // 红外扫描下限温度
    PARAM_AuxPrtDelayTime,          // 辅助通道延时时间保护条件
    PARAM_ProtectCapacityUpper,     // 容量安全保护上限
    PARAM_ConditionEng,             // 能量限制条件
    PARAM_ConditionSingleVol,       // 单体电压限制条件
    PARAM_AuxPtrMaxDiffVlot,        // 辅助通道电压差保护条件

    PARAM_COUNT = 64,               // 骨架便利: paramList 固定 64 项(与原版 resize(64) 一致)
};

// 一个参数: 是否有效 + 运算符 + 值
struct StruSetStepParam
{
    bool    vaild = false;
    uint8_t op = OP_EQU;
    double  num = 0.0;      // 数值类参数(电压/电流/温度/容量/功率/…)
    int64_t i64 = 0;        // 时间(ms, 64 位) / 整型(32 位) 参数
};

// 一个工步
struct StruSetStepInfo
{
    StruSetStepInfo();

    void clearParams();

    // 设参数(通用入口; 原版是 60 个 PARAM_FUNC 宏生成的 setXxx)
    void setParam(int idx, uint8_t op, double value);        // 数值类
    void setParamI64(int idx, uint8_t op, int64_t value);    // 时间/整型类

    bool isVaild(int idx) const { return idx >= 0 && idx < (int)paramList.size() && paramList[idx].vaild; }

    // 参数分类(照原版; 决定打包时写几个字节、怎么换算)
    bool isReserved(int type) const;
    bool isVolt(int type) const;
    bool isCurrent(int type) const;
    bool isTemp(int type) const;
    bool isCapacity(int type) const;
    bool isTime64(int type) const;
    bool isInt32(int type) const;

    uint32_t getBitmap(int index) const;            // 32 位一组: 哪些参数有效
    void     setBitmap(int index, uint32_t bitmap);

    JZByteArray pack() const;                       // 单工步包(长度 + 头 + body + crc32)
    bool unpack(const JZByteArray& stepPack);       // 与 pack 对称

    uint16_t StepLine = 0;                          // 工步行号
    uint8_t  StepID   = 0;                          // 工步号
    uint8_t  StepType = STEP_TYPE_NULL;             // 工步类型
    std::vector<StruSetStepParam> paramList;        // 固定 PARAM_COUNT 项(下标 = PARAM_xxx)
};

// 整包(工步文件)
struct StruSetStep76Ex
{
    StruSetStep76Ex();

    void setChannel(int idx);        // 置位第 idx 路通道(0 基; 与原版同口径: 低位起)
    void clearChannel();

    void genDefaultStep();           // 默认工步模板(照原版 PackDefine::genDefaultStep)
    void updateStepLine();           // 重排 StepLine / StepID(从 1 开始)

    JZByteArray pack() const;        // 包体(Guid + ChannelID + 长度 + 工步数 + 标志 + 各工步包)
    bool unpack(const JZByteArray& body);

    uint8_t Guid[16];                // 客户端标识
    uint8_t ChannelID[8];            // 通道位图(64 位, 低位起)
    std::vector<StruSetStepInfo> stepList;
};

// 直接组出可发送的整帧(命令码 BTS_CMD_SET_STEP76_Ex; deviceId/subDevId 由调用方按通道给)
JZByteArray packSetStep76Ex(const StruSetStep76Ex& step, uint8_t deviceId = 0, uint8_t subDevId = 0);

// 标准 CRC-32(与设备端一致: 初值 0 / 反射多项式 0xEDB88320 / 末取反; 长度<1 返回 0xFFFFFFFF)
uint32_t crc32(const uint8_t* data, int len);

#endif // BTE_PACK_DEFINE_H_
