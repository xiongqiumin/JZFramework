// PackDefine.cpp —— 对应原版 bteserver/common/PackDefine.cpp(+ common/net/netPack.cpp
// 的 PackInfo 那半个, 见 PackDefine.h 文件头的说明)。
//
// 实现是从原版 C++ 直接搬的, 只把 Qt 类型换成框架对应物
// (QByteArray -> JZByteArray, QDataStream -> ByteWriter/Reader)。
#include "PackDefine.h"

#include "JZDateTime.h"
#include "DataTrans.h"     // 工步下发: toVoltageUnit/ElectricUnit/toTempUnit/toCapUnit

#include <algorithm>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// 命令表
// ---------------------------------------------------------------------------
bool isRecvCmd(uint16_t cmd)
{
    // 照抄 PackInfo::IsRecvPack() 那张表(响应类包才带 Result 字节)
    switch (cmd)
    {
    case BTS_CMD_CONNECT_RESP:
    case BTS_CMD_SEND_INFO_RESP:
    case BTS_CMD_GET_RTC_RESP:
    case BTS_CMD_SET_RTC_RESP:
    case BGS66_CMD_SET_RT_INTERVAL_RESP:
    case BTS_CMD_SEND_PARALLEL_RESP:
    case BTS_CMD_SET_STEP_EX_RESP76:
    case BTS_CMD_START_RESP70:
    case BTS_CMD_STOP_RESP:
    case BTS_CMD_SEND_LOG_RESP:
    case BTS_CMD_SEND_DFDATA_RESP76:
    case BTS_CMD_LED_ON_RESP:
        return true;
    default:
        return false;
    }
}

std::string cmdName(uint16_t cmd)
{
    switch (cmd)
    {
    case BTS_CMD_CONNECT:               return "CONNECT";
    case BTS_CMD_CONNECT_RESP:          return "CONNECT_RESP";
    case BTS_CMD_SEND_INFO:             return "SEND_INFO";
    case BTS_CMD_SEND_INFO_RESP:        return "SEND_INFO_RESP";
    case BTS_CMD_GET_RTC:               return "GET_RTC";
    case BTS_CMD_GET_RTC_RESP:          return "GET_RTC_RESP";
    case BTS_CMD_SET_RTC:               return "SET_RTC";
    case BTS_CMD_SET_RTC_RESP:          return "SET_RTC_RESP";
    case BGS66_CMD_SET_RT_INTERVAL:     return "SET_RT_INTERVAL";
    case BGS66_CMD_SET_RT_INTERVAL_RESP:return "SET_RT_INTERVAL_RESP";
    case BTS_CMD_SET_STEP76_Ex:         return "SET_STEP76_Ex";
    case BTS_CMD_SET_STEP_EX_RESP76:    return "SET_STEP_EX_RESP76";
    case BTS_CMD_START70:               return "START70";
    case BTS_CMD_START_RESP70:          return "START_RESP70";
    case BTS_CMD_STOP:                  return "STOP";
    case BTS_CMD_STOP_RESP:             return "STOP_RESP";
    case BTS_CMD_RESUME:                return "RESUME";
    case BTS_CMD_RESUME_RESP:           return "RESUME_RESP";
    case BTS_CMD_RESUME_QUERY:          return "RESUME_QUERY";
    case BTS_CMD_RESUME_QUERY_RESP:     return "RESUME_QUERY_RESP";
    case BTS_CMD_RESUME_CONFIRM:        return "RESUME_CONFIRM";
    case BTS_CMD_RESUME_CONFIRM_RESP:   return "RESUME_CONFIRM_RESP";
    case BTS_CMD_STEP_JUMP:             return "STEP_JUMP";
    case BTS_CMD_STEP_JUMP_RESP:        return "STEP_JUMP_RESP";
    case BTS_CMD_HEART_BEAT:            return "HEART_BEAT";
    case BTS_CMD_SEND_LOG:              return "SEND_LOG";
    case BTS_CMD_SEND_LOG_RESP:         return "SEND_LOG_RESP";
    case BTS_CMD_SEND_PARALLEL:         return "SEND_PARALLEL";
    case BTS_CMD_SEND_PARALLEL_RESP:    return "SEND_PARALLEL_RESP";
    case BTS_CMD_SEND_PARALLEL_CANCEL:  return "SEND_PARALLEL_CANCEL";
    case BTS_CMD_SEND_RTDEVSTATUS:      return "SEND_RTDEVSTATUS";
    case BTS_CMD_SEND_RTDATA76:         return "SEND_RTDATA76";
    case BTS_CMD_SEND_DFDATA76:         return "SEND_DFDATA76";
    case BTS_CMD_SEND_DFDATA_RESP76:    return "SEND_DFDATA_RESP76";
    case BTS_CMD_LED_ON:                return "LED_ON";
    case BTS_CMD_LED_ON_RESP:           return "LED_ON_RESP";
    case WRITE_DEVICE_IP:               return "WRITE_DEVICE_IP";
    case WRITE_PC_IP:                   return "WRITE_PC_IP";
    case WRITE_DEVICE_MAC:              return "WRITE_DEVICE_MAC";
    default:                            break;
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "UNKNOWN(%#06X)", cmd);
    return buf;
}

// ---------------------------------------------------------------------------
// 字节游标(小端; 替代 QDataStream LittleEndian)
// ---------------------------------------------------------------------------
void ByteWriter::u8(uint8_t v) { m_data.push_back((char)v); }

void ByteWriter::u16(uint16_t v)
{
    m_data.push_back((char)(v & 0xFF));
    m_data.push_back((char)((v >> 8) & 0xFF));
}

void ByteWriter::u32(uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        m_data.push_back((char)((v >> (8 * i)) & 0xFF));
}

void ByteWriter::i32(int32_t v) { u32((uint32_t)v); }

void ByteWriter::i64(int64_t v)
{
    for (int i = 0; i < 8; ++i)
        m_data.push_back((char)(((uint64_t)v >> (8 * i)) & 0xFF));
}

void ByteWriter::f64(double v)
{
    uint64_t bits = 0;
    memcpy(&bits, &v, sizeof(bits));       // IEEE754 位模式, 按整数写(小端)
    i64((int64_t)bits);
}

void ByteWriter::raw(const void* data, int len)
{
    if (data && len > 0)
        m_data.append((const char*)data, len);
}

void ByteWriter::bytes(const JZByteArray& b)
{
    if (!b.isEmpty())
        m_data.append(b);
}

uint8_t ByteReader::u8()
{
    if (m_pos + 1 > m_data.size()) { m_ok = false; return 0; }
    return (uint8_t)m_data[m_pos++];
}

uint16_t ByteReader::u16()
{
    if (m_pos + 2 > m_data.size()) { m_ok = false; m_pos = m_data.size(); return 0; }
    const uint16_t v = (uint16_t)((uint8_t)m_data[m_pos] | ((uint16_t)(uint8_t)m_data[m_pos + 1] << 8));
    m_pos += 2;
    return v;
}

uint32_t ByteReader::u32()
{
    if (m_pos + 4 > m_data.size()) { m_ok = false; m_pos = m_data.size(); return 0; }
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= (uint32_t)(uint8_t)m_data[m_pos + i] << (8 * i);
    m_pos += 4;
    return v;
}

int32_t ByteReader::i32() { return (int32_t)u32(); }

int64_t ByteReader::i64()
{
    if (m_pos + 8 > m_data.size()) { m_ok = false; m_pos = m_data.size(); return 0; }
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= (uint64_t)(uint8_t)m_data[m_pos + i] << (8 * i);
    m_pos += 8;
    return (int64_t)v;
}

double ByteReader::f64()
{
    const uint64_t bits = (uint64_t)i64();
    double v = 0;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

JZByteArray ByteReader::raw(int len)
{
    if (len < 0 || m_pos + len > m_data.size())
    {
        m_ok = false;
        m_pos = m_data.size();
        return JZByteArray();
    }
    JZByteArray out = m_data.mid(m_pos, len);
    m_pos += len;
    return out;
}

void ByteReader::skip(int len)
{
    if (len < 0 || m_pos + len > m_data.size())
    {
        m_ok = false;
        m_pos = m_data.size();
        return;
    }
    m_pos += len;
}

uint8_t checkSum(const uint8_t* data, int len)
{
    // 原版 CaculateSum: 逐字节无符号相加(结果天然截到 8 位)
    uint8_t sum = 0;
    for (int i = 0; i < len; ++i)
        sum = (uint8_t)(sum + data[i]);
    return sum;
}

// ---------------------------------------------------------------------------
// PackInfo
// ---------------------------------------------------------------------------
int PackInfo::bodyLength() const
{
    return (int)Length - BTS_HEAD_LENGTH - BTS_TAIL_LENGTH - headLength();
}

void PackInfo::Pack(JZByteArray& buffer) const
{
    ByteWriter w;
    w.u8(DeviceID);
    w.u8(SubDevID);
    w.u16(Length);
    w.u16(CmdID);
    if (IsRecvPack())
        w.u8(Result);

    buffer.append(w.data());
}

bool PackInfo::UnPack(const JZByteArray& buffer, int offset)
{
    if (offset < 0 || offset + BTS_SEND_HEAD_LENGTH + BTS_TAIL_LENGTH > buffer.size())
        return false;

    DeviceID = (uint8_t)buffer[offset];
    SubDevID = (uint8_t)buffer[offset + 1];
    Length = (uint16_t)((uint8_t)buffer[offset + 2] | ((uint16_t)(uint8_t)buffer[offset + 3] << 8));
    CmdID = (uint16_t)((uint8_t)buffer[offset + 4] | ((uint16_t)(uint8_t)buffer[offset + 5] << 8));

    // CmdID 定了才知道包头是 6 还是 7 字节 —— 不够就等下一批字节
    const int infoLen = headLength();
    if (offset + infoLen + BTS_TAIL_LENGTH > buffer.size())
        return false;

    Result = (infoLen == BTS_RECV_HEAD_LENGTH) ? (uint8_t)buffer[offset + 6] : 0;

    if (bodyLength() < 0)
        return false;
    if (offset + infoLen + bodyLength() + BTS_TAIL_LENGTH > buffer.size())
        return false;

    return true;
}

// ---------------------------------------------------------------------------
// 组帧 / 拆帧
// ---------------------------------------------------------------------------
JZByteArray packFrame(const PackInfo& info, const JZByteArray& body)
{
    PackInfo head = info;
    head.Length = (uint16_t)(BTS_HEAD_LENGTH + head.headLength() + body.size() + BTS_TAIL_LENGTH);

    JZByteArray out;
    char fixHead[BTS_HEAD_LENGTH];
    memset(fixHead, 0xFF, sizeof(fixHead));      // 原版 packHead = 8 字节 0xFF
    out.append(fixHead, sizeof(fixHead));

    head.Pack(out);
    if (!body.isEmpty())
        out.append(body);

    const uint8_t chk = checkSum((const uint8_t*)out.data() + BTS_HEAD_LENGTH,
                                    out.size() - BTS_HEAD_LENGTH);
    out.push_back((char)chk);
    out.push_back((char)BTS_TAIL_BYTE);
    return out;
}

// 下一个候选帧头(连续 8 个 0xFF); 找不到返回 -1
static int findHead(const JZByteArray& buffer, int from)
{
    const int last = buffer.size() - BTS_HEAD_LENGTH;
    for (int i = from; i <= last; ++i)
    {
        bool ok = true;
        for (int j = 0; j < BTS_HEAD_LENGTH; ++j)
        {
            if ((uint8_t)buffer[i + j] != 0xFF) { ok = false; break; }
        }
        if (ok)
            return i;
    }
    return -1;
}

int takePackets(JZByteArray& buffer, std::vector<NetPack>& out, std::string* error)
{
    int taken = 0;
    int from = 0;
    const int minLen = BTS_HEAD_LENGTH + BTS_SEND_HEAD_LENGTH + BTS_TAIL_LENGTH;

    while (buffer.size() - from >= minLen)
    {
        const int head = findHead(buffer, from);
        if (head < 0)
        {
            // 没有候选头: 只留末尾"可能是被截断的头"那几个字节
            const int keep = std::min((int)buffer.size(), BTS_HEAD_LENGTH - 1);
            buffer = (keep > 0) ? buffer.mid(buffer.size() - keep, keep) : JZByteArray();
            if (error)
                *error = "没有帧头(丢弃前置脏数据)";
            return taken;
        }

        const int packStart = head + BTS_HEAD_LENGTH;
        PackInfo info;
        if (!info.UnPack(buffer, packStart))
            break;      // 数据不全: 留在缓冲里等下一批字节

        const int infoLen = info.headLength();
        const int bodyLen = info.bodyLength();

        const uint8_t chk = (uint8_t)buffer[packStart + infoLen + bodyLen];
        const uint8_t calc = checkSum((const uint8_t*)buffer.data() + packStart, infoLen + bodyLen);
        if (chk != calc)
        {
            if (error)
                *error = "和校验不对";
            buffer = buffer.mid(head + 1);      // 跳过这个候选头, 继续找
            from = 0;
            continue;
        }
        if ((uint8_t)buffer[packStart + infoLen + bodyLen + 1] != BTS_TAIL_BYTE)
        {
            if (error)
                *error = "包尾不是 0x5A";
            buffer = buffer.mid(head + 1);
            from = 0;
            continue;
        }

        NetPack p;
        p.info = info;
        p.data = buffer.mid(packStart + infoLen, bodyLen);
        out.push_back(p);
        ++taken;

        from = head + BTS_HEAD_LENGTH + infoLen + bodyLen + BTS_TAIL_LENGTH;
    }

    if (from > 0)
        buffer = buffer.mid(from);

    return taken;
}

std::string hexDump(const JZByteArray& bytes)
{
    std::string out;
    char buf[8];
    for (int i = 0; i < bytes.size(); ++i)
    {
        snprintf(buf, sizeof(buf), "%02X ", (uint8_t)bytes[i]);
        out += buf;
        if ((i + 1) % 16 == 0 && i + 1 != bytes.size())
            out += "\n";
    }
    return out;
}

// ---------------------------------------------------------------------------
// CONNECT
// ---------------------------------------------------------------------------
bool stru_BTS_CMD_CONNECT::unpack(const JZByteArray& body)
{
    if (body.size() < 1)
        return false;
    ClientType = (uint8_t)body[0];
    return true;
}

// ---------------------------------------------------------------------------
// SEND_INFO(字段顺序照 PackDefine.h 的结构声明)
// body 长度 = 2+6+1+1+1+1+2+32+1+4+4+4+4+4+1+40 = 108 字节
// ---------------------------------------------------------------------------
bool stru_BTS_CMD_SEND_INFO::unpack(const JZByteArray& body)
{
    ByteReader r(body);

    Version = r.u16();

    const JZByteArray fix = r.raw(6);
    if (!r.ok())
        return false;
    memcpy(fixed, fix.data(), 6);

    Type = r.u8();
    DataEndFlag = r.u8();
    DataIndex = r.u8();
    DeviceType = r.u8();
    TypeNum = r.u16();

    const JZByteArray chn = r.raw(32);
    if (!r.ok())
        return false;
    memcpy(Channel, chn.data(), 32);

    ChannelType = r.u8();
    ChargeMax = r.u32();
    DischargeMax = r.u32();
    VoltageMax = r.u32();
    TempatureMax = r.u32();
    InternalResistanceMax = r.u32();
    currentNum = r.u8();

    for (int i = 0; i < 10; ++i)
        currentRange[i] = r.u32();

    return r.ok();
}

JZByteArray stru_BTS_CMD_SEND_INFO::packBody() const
{
    ByteWriter w;
    w.u16(Version);
    w.raw(fixed, 6);
    w.u8(Type);
    w.u8(DataEndFlag);
    w.u8(DataIndex);
    w.u8(DeviceType);
    w.u16(TypeNum);
    w.raw(Channel, 32);
    w.u8(ChannelType);
    w.u32(ChargeMax);
    w.u32(DischargeMax);
    w.u32(VoltageMax);
    w.u32(TempatureMax);
    w.u32(InternalResistanceMax);
    w.u8(currentNum);
    for (int i = 0; i < 10; ++i)
        w.u32(currentRange[i]);
    return w.data();
}

std::string stru_BTS_CMD_SEND_INFO::deviceString(const std::string& ip) const
{
    // 原版: sprintf("%s-%dV%dA", ip, info.VoltageMax, info.ChargeMax / 1000)
    char buf[128];
    snprintf(buf, sizeof(buf), "%s-%uV%uA", ip.c_str(),
             (unsigned)VoltageMax, (unsigned)(ChargeMax / 1000));
    return buf;
}

int stru_BTS_CMD_SEND_INFO::channelNum() const
{
    // 原版 channelNum(): 数 Channel[] 前导的 0xFF, 每个字节算 8 路
    int n = 0;
    for (int i = 0; i < 32; ++i)
    {
        if (Channel[i] != 0xFF)
            break;
        n += 8;
    }
    return n;
}

// 单位换算在 DataTrans.cpp(原版 common/DataTrans.cpp), 这里不再重复。

// ---------------------------------------------------------------------------
// 通道位图
// ---------------------------------------------------------------------------
JZByteArray packChannelMask(const std::vector<int>& channels)
{
    uint8_t mask[8];
    memset(mask, 0, sizeof(mask));
    for (size_t i = 0; i < channels.size(); ++i)
    {
        const int idx = channels[i];
        if (idx < 0 || idx >= 64)
            continue;
        mask[idx / 8] |= (uint8_t)(1 << (idx % 8));     // 原版 setChannel()
    }

    JZByteArray out;
    out.append((const char*)mask, sizeof(mask));
    return out;
}

std::vector<int> unpackChannelMask(const JZByteArray& mask)
{
    std::vector<int> out;
    for (int i = 0; i < mask.size() * 8; ++i)
    {
        if ((uint8_t)mask[i / 8] & (1 << (i % 8)))
            out.push_back(i);
    }
    return out;
}

// ---------------------------------------------------------------------------
// SEND_RTDATA76(75 字节)
// ---------------------------------------------------------------------------
bool stru_BTS_CMD_SEND_RTDATA76::unpack(const JZByteArray& body)
{
    ByteReader r(body);
    DFDataPointer = r.i32();
    RTDataCount = r.u8();
    DataFlag = r.u8();
    ChannelIDByNum = r.u8();
    AuxChlIDByNum = r.u8();
    CycleStatus = r.i32();
    StepID = r.u8();
    StepType = r.u8();
    WorkType = r.u8();
    TestTime = r.i64();
    Voltage = r.i32();
    Current = r.i32();
    InternalResistance = (int16_t)r.u16();
    Tempature = (int16_t)r.u16();
    ChargeCap = r.i64();
    DisChargeCap = r.i64();
    ChargeEng = r.i64();
    DisChargeEng = r.i64();
    CurStepRange = r.i32();
    PrtCode = r.i32();
    return r.ok();
}

JZByteArray stru_BTS_CMD_SEND_RTDATA76::packBody() const
{
    ByteWriter w;
    w.i32(DFDataPointer);
    w.u8(RTDataCount);
    w.u8(DataFlag);
    w.u8(ChannelIDByNum);
    w.u8(AuxChlIDByNum);
    w.i32(CycleStatus);
    w.u8(StepID);
    w.u8(StepType);
    w.u8(WorkType);
    w.i64(TestTime);
    w.i32(Voltage);
    w.i32(Current);
    w.u16((uint16_t)InternalResistance);
    w.u16((uint16_t)Tempature);
    w.i64(ChargeCap);
    w.i64(DisChargeCap);
    w.i64(ChargeEng);
    w.i64(DisChargeEng);
    w.i32(CurStepRange);
    w.i32(PrtCode);
    return w.data();
}

bool stru_BTS_CMD_SEND_RTDATA76::isFault() const
{
    // 与 BTEServer 的报警判据一致: bit7(通道故障) / 安全停止 / 有保护码
    return (WorkType & 0x80) != 0 || workStatus() == WORK_ERROR_STOP || PrtCode != 0;
}

std::string stru_BTS_CMD_SEND_RTDATA76::statusText() const
{
    switch (workStatus())
    {
    case WORK_COMPLETE:   return "完成";
    case WORK_RUN:        return "运行";
    case WORK_USER_STOP:  return "用户停止";
    default:              return "安全停止";
    }
}

// ---------------------------------------------------------------------------
// SEND_LOG
// ---------------------------------------------------------------------------
bool stru_BTS_CMD_SEND_LOG::unpack(const JZByteArray& body)
{
    ByteReader r(body);
    LogNumber = r.u8();
    LogIndex = r.u16();
    LogSerial = r.u16();
    UnitID = r.u8();
    ChannelIDByNum = r.u8();
    TestID = r.i32();
    year = (int)r.u8() + 2000;
    month = r.u8();
    day = r.u8();
    hour = r.u8();
    minute = r.u8();
    second = r.u8();
    LogID = r.u8();
    LogInfo = r.i32();
    return r.ok();
}

std::string stru_BTS_CMD_SEND_LOG::timeText() const
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             year, month, day, hour, minute, second);
    return buf;
}

// ---------------------------------------------------------------------------
// SEND_DFDATA76
// ---------------------------------------------------------------------------
int64_t stru_BTS_CMD_SEND_DFDATA76::Data::timeMSec() const
{
    // 原版 ATime_t::toMSec(): 小时可能 >= 24, 先按天进位
    const int extraDays = hour / 24;
    const int h = hour % 24;
    JZDateTime dt(year, month, day + extraDays, h, minute, second);
    return dt.toMSecsSinceEpoch();
}

std::string stru_BTS_CMD_SEND_DFDATA76::Data::timeText() const
{
    const int extraDays = hour / 24;
    JZDateTime dt(year, month, day + extraDays, hour % 24, minute, second);
    return dt.toString("yyyy-MM-dd HH:mm:ss");
}

bool stru_BTS_CMD_SEND_DFDATA76::unpack(const JZByteArray& body)
{
    ByteReader r(body);
    DFDataNumber = r.u8();
    DFDataPointer = r.i32();

    dataList.clear();
    for (int i = 0; i < (int)DFDataNumber; ++i)
    {
        stru_BTS_CMD_SEND_DFDATA76::Data d;
        d.DataFlag = r.u8();
        d.SubDevID = r.u8();
        d.ChannelIDByNum = r.u8();
        d.AuxChlIDByNum = r.u8();
        d.TestID = r.u32();
        d.DataSerial = r.i32();
        d.CycleStatus = r.i32();
        d.StepID = r.u8();
        d.StepType = r.u8();
        d.StepChangeCount = r.u8();
        d.WorkType = r.u8();
        d.TestTime = r.i64();
        d.Voltage = r.i32();
        d.Current = r.i32();
        d.InternalResistance = (int16_t)r.u16();
        d.Tempature = (int16_t)r.u16();
        d.ChargeCap = r.i64();
        d.DisChargeCap = r.i64();
        d.ChargeEng = r.i64();
        d.DisChargeEng = r.i64();
        d.year = (int)r.u16();
        d.month = r.u8();
        d.day = r.u8();
        d.hour = r.u8();
        d.minute = r.u8();
        d.second = r.u8();
        d.CurStepRange = r.i32();

        if (!r.ok())
            return false;
        dataList.push_back(d);
    }
    return r.ok();
}

JZByteArray stru_BTS_CMD_SEND_DFDATA_RESP76::packBody() const
{
    ByteWriter w;
    w.i32(Index);
    return w.data();
}

// ---------------------------------------------------------------------------
// 上位机下发的控制包
// ---------------------------------------------------------------------------
JZByteArray packStart70(const std::vector<int>& channels, uint8_t stepId, const uint8_t* guid)
{
    ByteWriter w;
    uint8_t zero[16];
    memset(zero, 0, sizeof(zero));
    w.raw(guid ? guid : zero, 16);
    w.bytes(packChannelMask(channels));
    w.u8(stepId);
    return w.data();
}

JZByteArray packChannelMaskBody(const std::vector<int>& channels)
{
    return packChannelMask(channels);
}

JZByteArray packStepJump(const std::vector<int>& channels, uint8_t jumpStepId)
{
    ByteWriter w;
    w.bytes(packChannelMask(channels));
    w.u8(jumpStepId);
    return w.data();
}

JZByteArray packSetRtc(int year, int month, int day, int hour, int minute, int second)
{
    ByteWriter w;
    w.u8((uint8_t)(year - 2000));
    w.u8((uint8_t)month);
    w.u8((uint8_t)day);
    w.u8((uint8_t)hour);
    w.u8((uint8_t)minute);
    w.u8((uint8_t)second);
    return w.data();
}

JZByteArray packRtInterval(uint8_t sec)
{
    ByteWriter w;
    w.u8(sec);
    return w.data();
}

JZByteArray packResumeConfirm(uint8_t result)
{
    ByteWriter w;
    w.u8(result);
    return w.data();
}

JZByteArray packLedOn(const uint8_t led[4])
{
    ByteWriter w;
    for (int i = 0; i < 4; ++i)
        w.u8(led[i]);
    return w.data();
}

// ---------------------------------------------------------------------------
// START_RESP70
// ---------------------------------------------------------------------------
bool stru_BTS_CMD_START_RESP70::unpack(const JZByteArray& body)
{
    ByteReader r(body);
    r.skip(16);                       // Guid
    ChannelCount = r.u8();
    channelInfo.clear();
    for (int i = 0; i < (int)ChannelCount; ++i)
    {
        ChannelInfo info;
        info.ChannelIDByNum = r.u8();
        info.TestID = r.i32();
        if (!r.ok())
            return false;
        channelInfo.push_back(info);
    }
    return r.ok();
}

// ---------------------------------------------------------------------------
// PackDefine(工步名表 / 保护码文案) —— 原版在 net/netPackDefine.cpp 的 NetPackRegist() 里初始化
// ---------------------------------------------------------------------------
std::vector<std::string>   PackDefine::StepNameList;
std::map<int, std::string> PackDefine::ErrorList;

void PackDefine::init()
{
    static bool done = false;
    if (done)
        return;
    done = true;

    // 工步名(原版 NetPackRegist 里内嵌的那份中文表, 顺序与 STEP_TYPE_* 一一对应)
    StepNameList = {
        "未定义", "恒流充电", "恒流放电", "恒压充电",
        "静    置", "循    环", "结    束", "恒流恒压充电",
        "恒功率放电", "恒功率充电", "恒阻放电", "恒阻充电",
        "测量内阻", "挂起工步", "N闭环恒压充电", "N闭环恒流恒压充电",
        "脉冲工步", "模拟工步", "电池组恒流恒压", "恒压放电",
        "恒流恒压放电", "控制工步"
    };

    // 保护码文案: 原版从 exe 目录的 Modules/BTELogCode.txt 逐行读("码-十六进制-描述")。
    // 骨架不带这个文件 —— ErrorList 留空, ErrorText() 就返回 "未知错误"(与原版缺该文件时一致)。
    // 真要恢复: 在这里按同样格式读文件填 ErrorList 即可。
}

std::string PackDefine::ErrorText(int type)
{
    std::map<int, std::string>::const_iterator it = ErrorList.find(type);
    return (it == ErrorList.end()) ? std::string("未知错误") : it->second;
}

int PackDefine::StepChargeType(int type)
{
    if (type == STEP_TYPE_7 || type == STEP_TYPE_3 || type == STEP_TYPE_1)
        return STEP_CHARGE;
    if (type == STEP_TYPE_2)
        return STEP_DISCHARGE;
    return STEP_CHARGE_OTHER;
}

// ===========================================================================
// 工步下发 SET_STEP76_Ex (结构/口径见 PackDefine.h 的说明; 与设备端逐字节对齐)
// ===========================================================================

uint32_t crc32(const uint8_t* data, int len)
{
    // 标准 CRC-32: 初值 0 / 反射多项式 0xEDB88320 / 结尾取反。
    // 与原版 PackDefine.cpp 的 getCRC32() 查表法逐位等价(那张 256 项表就是本多项式生成的),
    // 这里用逐位算法省掉常量表 —— 工步包不大, 速度无所谓。
    if (len < 1)
        return 0xFFFFFFFFu;

    uint32_t crc = 0;
    for (int i = 0; i < len; ++i)
    {
        crc ^= (uint32_t)data[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
    }
    return crc ^ 0xFFFFFFFFu;
}

// ---- StruSetStepInfo ----

StruSetStepInfo::StruSetStepInfo()
{
    paramList.resize(PARAM_COUNT);
    clearParams();
    StepLine = 0;
    StepID = 0;
    StepType = STEP_TYPE_NULL;
}

void StruSetStepInfo::clearParams()
{
    StruSetStepParam empty;                 // vaild=false / op=OP_EQU / 值清零
    for (size_t i = 0; i < paramList.size(); ++i)
        paramList[i] = empty;
}

void StruSetStepInfo::setParam(int idx, uint8_t op, double value)
{
    if (idx < 0 || idx >= (int)paramList.size())
        return;

    StruSetStepParam& p = paramList[idx];
    p.vaild = true;
    p.op = op;
    p.num = value;
}

void StruSetStepInfo::setParamI64(int idx, uint8_t op, int64_t value)
{
    if (idx < 0 || idx >= (int)paramList.size())
        return;

    StruSetStepParam& p = paramList[idx];
    p.vaild = true;
    p.op = op;
    p.i64 = value;
}

bool StruSetStepInfo::isReserved(int type) const
{
    return type == PARAM_ReservedParam1 || type == PARAM_ReservedParam2;
}

bool StruSetStepInfo::isVolt(int type) const
{
    return type == PARAM_Voltage
        || type == PARAM_StopVoltage
        || type == PARAM_VoltageChange
        || type == PARAM_ProtectVoltageLower
        || type == PARAM_ProtectVoltageUpper
        || type == PARAM_RecordVoltageChange
        || type == PARAM_ProtectDetalVol;
}

bool StruSetStepInfo::isCurrent(int type) const
{
    return type == PARAM_Current
        || type == PARAM_StopCurrent
        || type == PARAM_ProtectCurrentUpper
        || type == PARAM_ProtectCurrentLower
        || type == PARAM_RecordCurrentChange;
}

bool StruSetStepInfo::isTemp(int type) const
{
    return type == PARAM_Tempature
        || type == PARAM_ProtectTempatureUpper
        || type == PARAM_ProtectTempatureLower
        || type == PARAM_RecordTempatureChange;
}

bool StruSetStepInfo::isCapacity(int type) const
{
    return type == PARAM_Capacity;          // 协议里容量是 8 字节
}

bool StruSetStepInfo::isTime64(int type) const
{
    return type == PARAM_Time || type == PARAM_RecordTimeInterval;
}

bool StruSetStepInfo::isInt32(int type) const
{
    return type == PARAM_BeginStepID
        || type == PARAM_CountLoop
        || type == PARAM_JumpLine
        || type == PARAM_ProtectDelayTime
        || type == PARAM_ProtectDetalVolTime;
}

uint32_t StruSetStepInfo::getBitmap(int index) const
{
    const int s = index * 32;
    if (s < 0 || s >= (int)paramList.size())
        return 0;

    uint32_t var = 0;
    for (int i = s; i < s + 32 && i < (int)paramList.size(); ++i)   // 原版这一步没夹边界, 这里补上
    {
        if (paramList[i].vaild)
            var |= (1u << (i - s));
    }

    if (var != 0)
        var |= (1u << 31);                  // bit31 = "后面还有 bitMap"
    return var;
}

void StruSetStepInfo::setBitmap(int index, uint32_t bitmap)
{
    const int s = index * 32;
    for (int i = s; i < s + 31 && i < (int)paramList.size(); ++i)   // 只取低 31 位(bit31 是标志)
        paramList[i].vaild = ((bitmap & (1u << (i - s))) != 0);
}

JZByteArray StruSetStepInfo::pack() const
{
    // ---- body: bitMap[..] + 按位序排的有效参数 ----
    ByteWriter body;

    uint32_t bit_map[12] = { 0 };
    int max_idx = 0;
    for (int i = 0; i < 12; ++i)
    {
        bit_map[i] = getBitmap(i);
        ++max_idx;
        if (bit_map[i] == 0)
            break;                          // 遇到 0 结束(与 unpack 对称)
    }
    for (int i = 0; i < max_idx; ++i)
        body.u32(bit_map[i]);

    const int param_count = ((int)paramList.size() < max_idx * 32) ? (int)paramList.size() : max_idx * 32;
    for (int i = 0; i < param_count; ++i)
    {
        if (i % 32 == 31)
            continue;                       // bit31 是标志位, 不携带数据
        if (!paramList[i].vaild)
            continue;

        const StruSetStepParam& p = paramList[i];
        if (isReserved(i))
        {
            body.i32((int32_t)p.i64);       // 预留参数: 4 字节整数, **没有**运算符
            continue;
        }

        body.u8(p.op);                      // 其它参数: 运算符 + 值
        if (isVolt(i))
            body.u32(DataTrans::toVoltageUnit(p.num));
        else if (isCurrent(i))
            body.u32(DataTrans::ElectricUnit(p.num));
        else if (isTemp(i))
            body.u32(DataTrans::toTempUnit(p.num));
        else if (isCapacity(i))
            body.i64(DataTrans::toCapUnit(p.num));
        else if (isTime64(i))
            body.i64(p.i64);
        else if (isInt32(i))
            body.i32((int32_t)p.i64);
        else
            body.u32(0);                    // 未分类参数: 占位(原版这里 Q_ASSERT(0); 骨架还没用到)
    }

    // ---- 包头 + body + crc32 ----
    const JZByteArray b = body.data();

    ByteWriter out;
    out.u16((uint16_t)(b.size() + 10));     // 长度: 6 字节包头 + 4 字节 crc
    out.u16(StepLine);
    out.u8(StepID);
    out.u8(StepType);
    out.raw(b.data(), b.size());

    out.u32(crc32((const uint8_t*)out.data().data(), out.data().size()));   // crc 覆盖包头 + body
    return out.data();
}

bool StruSetStepInfo::unpack(const JZByteArray& stepPack)
{
    const int head_size = 6;                // 长度 + 工步行号 + 工步号 + 工步类型
    const int crc_size = 4;
    if (stepPack.size() < head_size + crc_size)
        return false;

    uint32_t crc = 0;
    memcpy(&crc, stepPack.data() + stepPack.size() - crc_size, crc_size);
    if (crc != crc32((const uint8_t*)stepPack.data(), stepPack.size() - crc_size))
        return false;

    ByteReader in(stepPack);
    const uint16_t packLen = in.u16();
    if (packLen != (uint16_t)stepPack.size())
        return false;

    StepLine = in.u16();
    StepID   = in.u8();
    StepType = in.u8();

    clearParams();

    uint32_t bit_map[12] = { 0 };
    int max_idx = 0;
    for (int i = 0; i < 12; ++i)
    {
        bit_map[i] = in.u32();
        ++max_idx;
        if (bit_map[i] == 0)
            break;
    }
    for (int i = 0; i < max_idx; ++i)
        setBitmap(i, bit_map[i]);

    const int param_count = ((int)paramList.size() < max_idx * 32) ? (int)paramList.size() : max_idx * 32;
    for (int i = 0; i < param_count; ++i)
    {
        if (i % 32 == 31)
            continue;
        if (!paramList[i].vaild)
            continue;

        StruSetStepParam& p = paramList[i];
        if (isReserved(i))
        {
            p.i64 = in.i32();
            continue;
        }

        p.op = in.u8();
        if (isVolt(i))
            p.num = DataTrans::fromVoltageUnit(in.i32());
        else if (isCurrent(i))
            p.num = DataTrans::fromElectricUnit(in.i32());
        else if (isTemp(i))
            p.num = DataTrans::fromTempUnit((int16_t)in.i32());
        else if (isCapacity(i))
            p.num = DataTrans::fromCapUnit(in.i64());
        else if (isTime64(i))
            p.i64 = in.i64();
        else if (isInt32(i))
            p.i64 = in.i32();
        else
            in.u32();
    }

    return in.ok();
}

// ---- StruSetStep76Ex ----

StruSetStep76Ex::StruSetStep76Ex()
{
    memset(Guid, 0, sizeof(Guid));
    memset(ChannelID, 0, sizeof(ChannelID));
}

void StruSetStep76Ex::setChannel(int idx)
{
    if (idx < 0 || idx >= 64)
        return;
    ChannelID[idx / 8] |= (uint8_t)(1u << (idx % 8));    // 低位起(与 packChannelMask 同口径)
}

void StruSetStep76Ex::clearChannel()
{
    memset(ChannelID, 0, sizeof(ChannelID));
}

void StruSetStep76Ex::updateStepLine()
{
    for (size_t i = 0; i < stepList.size(); ++i)
    {
        stepList[i].StepLine = (uint16_t)(i + 1);
        stepList[i].StepID   = (uint8_t)(i + 1);
    }
}

void StruSetStep76Ex::genDefaultStep()
{
    // 照原版 PackDefine::genDefaultStep(): 6 个工步
    // 恒流恒压充电 -> 静置 -> 恒流放电 -> 静置 -> 补电(恒流恒压) -> 停止
    stepList.clear();

    {
        StruSetStepInfo step;
        step.StepType = STEP_TYPE_7;                            // 恒流恒压充电
        step.setParam(PARAM_Current, OP_SMALL_EQU, 3);
        step.setParam(PARAM_StopCurrent, OP_BIG_EQU, 0.5);
        step.setParam(PARAM_Voltage, OP_SMALL_EQU, 4.2);
        step.setParamI64(PARAM_ReservedParam1, OP_EQU, 0x41);
        step.setParam(PARAM_ProtectVoltageLower, OP_BIG_EQU, 2.4);
        step.setParam(PARAM_ProtectVoltageUpper, OP_SMALL_EQU, 4.3);
        stepList.push_back(step);
    }
    {
        StruSetStepInfo step;
        step.StepType = STEP_TYPE_IDLE;                         // 静置
        step.setParamI64(PARAM_Time, OP_BIG_EQU, 3 * 60 * 1000);
        step.setParamI64(PARAM_ReservedParam1, OP_EQU, 0x41);
        stepList.push_back(step);
    }
    {
        StruSetStepInfo step;
        step.StepType = STEP_TYPE_2;                            // 恒流放电
        step.setParam(PARAM_Current, OP_SMALL_EQU, 10);
        step.setParam(PARAM_StopVoltage, OP_BIG_EQU, 3);
        step.setParamI64(PARAM_ReservedParam1, OP_EQU, 0x41);
        stepList.push_back(step);
    }
    {
        StruSetStepInfo step;
        step.StepType = STEP_TYPE_IDLE;                         // 静置
        step.setParamI64(PARAM_Time, OP_BIG_EQU, 3 * 60 * 1000);
        step.setParamI64(PARAM_ReservedParam1, OP_EQU, 0x41);
        stepList.push_back(step);
    }
    {
        StruSetStepInfo step;
        step.StepType = STEP_TYPE_7;                            // 补电(恒流恒压)
        step.setParam(PARAM_Current, OP_SMALL_EQU, 3);
        step.setParam(PARAM_StopCurrent, OP_BIG_EQU, 0.5);
        step.setParam(PARAM_Voltage, OP_SMALL_EQU, 3.7);
        step.setParamI64(PARAM_ReservedParam1, OP_EQU, 0x41);
        stepList.push_back(step);
    }
    {
        StruSetStepInfo step;
        step.StepType = STEP_TYPE_STOP;                         // 停止
        stepList.push_back(step);
    }

    updateStepLine();
}

JZByteArray StruSetStep76Ex::pack() const
{
    ByteWriter out;
    out.raw(Guid, 16);
    out.raw(ChannelID, 8);

    // 先各工步单独打包, 顺便累计"工步文件总长"(原版 StepFileLen)
    int stepFileLen = 0;
    std::vector<JZByteArray> packs;
    for (size_t i = 0; i < stepList.size(); ++i)
    {
        const JZByteArray one = stepList[i].pack();
        stepFileLen += one.size();
        packs.push_back(one);
    }

    out.i32(stepFileLen);
    out.u16((uint16_t)stepList.size());
    out.u8(0x02);        // DataEndFlag: 0x02 = 后面没有了(传输完毕)
    out.u16(0x1);        // DataIndex: 分包索引号, 第一包 = 1

    for (size_t i = 0; i < packs.size(); ++i)
        out.raw(packs[i].data(), packs[i].size());

    return out.data();
}

bool StruSetStep76Ex::unpack(const JZByteArray& body)
{
    const int head_size = 16 + 8 + 4 + 2 + 1 + 2;
    if (body.size() < head_size)
        return false;

    ByteReader in(body);

    const JZByteArray guid = in.raw(16);
    const JZByteArray ch = in.raw(8);
    if (guid.size() < 16 || ch.size() < 8)
        return false;
    memcpy(Guid, guid.data(), 16);
    memcpy(ChannelID, ch.data(), 8);

    in.i32();               // StepFileLen: 不是解析必需(每个工步包自己带长度), 跳过
    const uint16_t stepCount = in.u16();
    in.u8();                // DataEndFlag
    in.u16();               // DataIndex

    stepList.clear();
    for (int i = 0; i < stepCount && in.ok(); ++i)
    {
        // 单工步包自带长度(前 2 字节, 小端), 先读它再取整包 —— ByteReader 没有 peek, 只能这样
        const JZByteArray head = in.raw(2);
        if (head.size() < 2)
            break;

        const uint16_t len = (uint16_t)(uint8_t(head.data()[0])
                                        | ((uint16_t)uint8_t(head.data()[1]) << 8));
        if (len < 10 || len > (uint16_t)in.left() + 2)
            break;

        const JZByteArray rest = in.raw(len - 2);
        if (rest.size() != len - 2)
            break;

        JZByteArray one = head;
        one.append(rest.data(), rest.size());

        StruSetStepInfo st;
        if (!st.unpack(one))
            break;
        stepList.push_back(st);
    }

    return in.ok();
}

JZByteArray packSetStep76Ex(const StruSetStep76Ex& step, uint8_t deviceId, uint8_t subDevId)
{
    PackInfo info;
    info.DeviceID = deviceId;
    info.SubDevID = subDevId;
    info.CmdID    = BTS_CMD_SET_STEP76_Ex;
    info.Result   = 0;

    return packFrame(info, step.pack());
}
