// ---------------------------------------------------------------------------
// PackDefine.js —— 对应 C++ common/PackDefine.h + PackDefine.cpp
//
// 说明: 本文件当前收 PackDefine.h 的"常量/枚举/错误码 + PackDefine 类";
//       各 stru_* 的 pack/unpack(对应 PackDefine.cpp)仍在 ../../structs.js, 下一批并入本文件,
//       并入后 structs.js 删除。
//
// Qt -> JS: 宏(#define) -> export const; 枚举 -> 具名常量对象;
//           QMap/QStringList/QVector -> Map/数组; Q_ASSERT -> console.assert。
// ---------------------------------------------------------------------------

// ---- 帧长度 ----
export const BTS_HEAD_LENGTH = 8;
export const BTS_SEND_HEAD_LENGTH = 6;
export const BTS_RECV_HEAD_LENGTH = 7;
export const BTS_TAIL_LENGTH = 2;

// ---- 命令字 (PackDefine.h 的 #define 逐条搬运) ----
export const BTS_CMD_CONNECT = 0x0000;
export const BTS_CMD_CONNECT_RESP = 0x8000;

export const BTS_CMD_SEND_INFO = 0x72;
export const BTS_CMD_SEND_INFO_RESP = (0x72 + 0x80);

export const BTS_CMD_GET_RTC = 0x02;
export const BTS_CMD_GET_RTC_RESP = 0x82;

export const BTS_CMD_SET_RTC = 0x03;
export const BTS_CMD_SET_RTC_RESP = 0x83;

// BGS6.6设置实时数据发送间隔
export const BGS66_CMD_SET_RT_INTERVAL = 0x5E;
export const BGS66_CMD_SET_RT_INTERVAL_RESP = (0x5E + 0x80);

// BTS70 设置中位机测试编号
export const BTS70_CMD_SET_TESTID = 0x74;
export const BTS70_CMD_SET_TESTID_RESP = (0x74 + 0x80);

// 设置中位机通道映射
export const BTS_CMD_SET_BGSAUX_MAP70 = 0x73;
export const BTS_CMD_SET_BGSAUX_MAP_RESP70 = (0x73 + 0x80);

// 设置中位机辅助通道
export const BTS_CMD_SET_BGSAUX = 0x39;
export const BTS_CMD_SET_BGSAUX_RESP = (0x39 + 0x80);

// BTS76_Ex设置中位机工步信息
export const BTS_CMD_SET_STEP76_Ex = 0x0200;
export const BTS_CMD_SET_STEP_EX_RESP76 = (0x0200 + 0x8000);

// BTS70 启动测试通道
export const BTS_CMD_START70 = 0x6E;
export const BTS_CMD_START_RESP70 = (0x6E + 0x80);

// 停止中位机测试通道
export const BTS_CMD_STOP = 0x11;
export const BTS_CMD_STOP_RESP = 0x91;

// 接续中位机测速通道
export const BTS_CMD_RESUME = 0x14;
export const BTS_CMD_RESUME_RESP = 0x94;

// 中位机接续提示
export const BTS_CMD_RESUME_QUERY = 0x35;
export const BTS_CMD_RESUME_QUERY_RESP = 0xB5;

// 中位机接续反馈
export const BTS_CMD_RESUME_CONFIRM = 0x36;
export const BTS_CMD_RESUME_CONFIRM_RESP = 0xB6;

// 接续中位机测速通道
export const BTS_CMD_STEP_JUMP = 0x6f;
export const BTS_CMD_STEP_JUMP_RESP = (0x6f + 0x80);

// 中位机发送心跳数据
export const BTS_CMD_HEART_BEAT = 0x30;

// 中位机发送日志信息
export const BTS_CMD_SEND_LOG = 0x0F;
export const BTS_CMD_SEND_LOG_RESP = 0x8F;

// 中位机并联
export const BTS_CMD_SEND_PARALLEL = 0x0300;
export const BTS_CMD_SEND_PARALLEL_RESP = 0x8300;

// 中位机并联解除
export const BTS_CMD_SEND_PARALLEL_CANCEL = 0x0400;

// 中位机发送实时设备状态给服务器
export const BTS_CMD_SEND_RTDEVSTATUS = 0x44;

// BTS76 中位机发送实时数据给服务器
export const BTS_CMD_SEND_RTDATA76 = 0x07C;

// BTS76 中位机发送DF数据给服务器
export const BTS_CMD_SEND_DFDATA76 = 0x7D;
export const BTS_CMD_SEND_DFDATA_RESP76 = (0x7D + 0x80);

// LED on
export const BTS_CMD_LED_ON = 0x6000;
export const BTS_CMD_LED_ON_RESP = (0x6000 + 0x80);

export const INVAILD_SUB_ID = 0xFe;
export const MS_PER_DAY = (24 * 3600 * 1000);

export const WRITE_DEVICE_IP = 0x5000;
export const WRITE_PC_IP = 0x5100;
export const WRITE_DEVICE_MAC = 0x5200;

// ---- 结果码 ----
export const SUCCEED = 0x00; // 操作成功
export const FAIL = 0x01; // 操作失败
export const BUSY = 0x02; // 忙
export const PARAMETERTABLEERROR = 0x03; // 系统参数Flash存储故障，请重试。
export const DEVICEIDERROR = 0x04; // 设备ID出错。
export const UNITTESTTING = 0x05; // 单元测试中。
export const UNITCOMMUNICATIONERROR = 0x06; // 内部通讯错误。
export const ReceiveDataCheckError = 0x10; // 接收到的数据校验和错
export const ReceiveDataEndFlagError = 0x11; // 接收到的结束字符0x5A错
export const ReceiveBTSCommandError = 0x12; // 接收到的BTS命令字非法
export const RequestDataLengthError = 0x13; // 协议中的项目数量溢出
export const RequestSubDeviceIDError = 0x14; // 子设备ID号错误
export const RequestUnitChannelIDError = 0x15; // 通道ID非法
export const RequestStepIDNotFind = 0x16; // 没有发现所请求的工步号
export const SetStepIDSerialError = 0x17; // 工步序号不连续错
export const StartFail = 0x20; // 启动失败。

// ---- 运算符 ----
export const OP_EQU = 0x01; // ==
export const OP_NEQ = 0x02; // !=
export const OP_BIG = 0x03; // >
export const OP_BIG_EQU = 0x04; // >=
export const OP_SMALL = 0x05; // <
export const OP_SMALL_EQU = 0x06; // <=

// ---- 工步类型 ----
export const STEP_TYPE_NULL = 0; // 未定义
export const STEP_TYPE_1 = 1;    // 恒流充电
export const STEP_TYPE_2 = 2;    // 恒流放电
export const STEP_TYPE_3 = 3;    // 恒压充电
export const STEP_TYPE_IDLE = 4; // 静    置
export const STEP_TYPE_LOOP = 5; // 循    环
export const STEP_TYPE_STOP = 6; // 停    止
export const STEP_TYPE_7 = 7;    // 恒流恒压充电
export const STEP_TYPE_8 = 8;    // 恒功率放电
export const STEP_TYPE_9 = 9;    // 恒功率充电	9
export const STEP_TYPE_10 = 10;  // 恒阻放电	10
export const STEP_TYPE_11 = 11;  // 恒阻充电	11
export const STEP_TYPE_12 = 12;  // 测量内阻	12
export const STEP_TYPE_13 = 13;  // 挂起工步	13
export const STEP_TYPE_14 = 14;  // N闭环恒压充电	14
export const STEP_TYPE_15 = 15;  // N闭环恒流恒压充电	15
export const STEP_TYPE_16 = 16;  // 脉冲工步	16
export const STEP_TYPE_17 = 17;  // 模拟工步	17
export const STEP_TYPE_18 = 18;  // 电池组恒流恒压	18
export const STEP_TYPE_19 = 19;  // 恒压放电	19
export const STEP_TYPE_20 = 20;  // 恒流恒压放电	20
export const STEP_TYPE_21 = 21;  // 控制工步	21

export const STEP_TYPE_PARAM = 0xFE; // 参数设置

// ---- 充放电归类 ----
export const STEP_CHARGE = 0;
export const STEP_DISCHARGE = 1;
export const STEP_CHARGE_OTHER = 2;

// ---- 工步参数下标(bitmap1 / bitmap2) ----
export const PARAM_Current = 0;  // 电流运算符(A)
export const PARAM_Voltage = 1;  // 电压运算符(V)
export const PARAM_Time = 2;     // 时间运算符(ms)
export const PARAM_Capacity = 3; // 容量运算符
export const PARAM_Tempature = 4; // 温度运算符
export const PARAM_Power = 5;    // 功率运算符
export const PARAM_Resistance = 6; // 电阻运算符
export const PARAM_StopCurrent = 7; // 截止电流运算符
export const PARAM_StopVoltage = 8; // 截止电压运算符
export const PARAM_VoltageChange = 9; // - ΔV运算符
export const PARAM_BeginStepID = 10; // 起始工步号
export const PARAM_CountLoop = 11; // 循环次数
export const PARAM_JumpLine = 12; // 跳转行号
export const PARAM_ReservedParam1 = 13; // 保留参数1
export const PARAM_ReservedParam2 = 14; // 保留参数2
export const PARAM_ProtectVoltageUpper = 15; // 电压安全保护上限
export const PARAM_ProtectVoltageLower = 16; // 电压安全保护下限
export const PARAM_ProtectCurrentUpper = 17; // 电流安全保护上限
export const PARAM_ProtectCurrentLower = 18; // 电流安全保护下限
export const PARAM_ProtectTempatureUpper = 19; // 温度安全保护上限
export const PARAM_ProtectTempatureLower = 20; // 温度安全保护下限
export const PARAM_ProtectDelayTime = 21; // 延迟保护时间
export const PARAM_ProtectDetalVolTime = 22; // 化成变化率检测时间
export const PARAM_ProtectDetalVol = 23; // 化成变化率电压
export const PARAM_RecordTimeInterval = 24; // 时间间隔记录条件
export const PARAM_RecordCurrentChange = 25; // 电流突变记录条件
export const PARAM_RecordVoltageChange = 26; // 电压突变记录条件
export const PARAM_RecordTempatureChange = 27; // 温度突变记录条件
export const PARAM_PlatVoltage1 = 28; // 平台点监测电压1
export const PARAM_PlatVoltage2 = 29; // 平台点监测电压2
export const PARAM_PlatVoltage3 = 30; // 平台点监测电压3

// bitmap2
export const PARAM_AuxChannelMap = 32; // 映射辅助通道号
export const PARAM_AuxChannel = 33; // 实际辅助通道号
export const PARAM_AuxCndTempatureUpper = 34; // 辅助通道温度限制上限值
export const PARAM_AuxCndTempatureLower = 35; // 辅助通道温度限制下限值
export const PARAM_AuxCndVolUpper = 36; // 辅助通道电压限制上限值
export const PARAM_AuxCndVolLower = 37; // 辅助通道电压限制下限值
export const PARAM_AuxCndCurUpper = 38; // 辅助通道电流限制上限值
export const PARAM_AuxCndCurLower = 39; // 辅助通道电流限制下限值
export const PARAM_AuxPrtTempatureUpper = 40; // 辅助通道温度保护上限值
export const PARAM_AuxPrtTempatureLower = 41; // 辅助通道温度保护下限值
export const PARAM_AuxPrtVolUpper = 42; // 辅助通道电压保护上限值
export const PARAM_AuxPrtVolLower = 43; // 辅助通道电压保护下限值
export const PARAM_AuxPrtCurUpper = 44; // 辅助通道电流保护上限值
export const PARAM_AuxPrtCurLower = 45; // 辅助通道电流保护下限值
export const PARAM_AuxRecordTimeInterval = 46; // 辅助通道时间间隔记录条件
export const PARAM_AuxRecordCurrentChange = 47; // 辅助通道电流记录条件
export const PARAM_AuxRecordVoltageChange = 48; // 辅助通道电压记录条件
export const PARAM_AuxRecordTempatureChange = 49; // 辅助通道温度记录条件
export const PARAM_AuxConditionTime = 50; // 辅助通道时间限制条件
export const PARAM_AuxConditionBeginStepID = 51; // 起始工步号
export const PARAM_AuxConditionCountLoop = 52; // 循环次数
export const PARAM_IRDCScanTime = 53; // 红外扫描工步拍照时间
export const PARAM_IRDCTempUpper = 54; // 红外扫描上限温度
export const PARAM_IRDCTempLower = 55; // 红外扫描下限限温度
export const PARAM_AuxPrtDelayTime = 56; // 辅助通道延时时间保护条件
export const PARAM_ProtectCapacityUpper = 57; // 容量安全保护上限
export const PARAM_ConditionEng = 58; // 能量限制条件
export const PARAM_ConditionSingleVol = 59; // 单体电压限制条件
export const PARAM_AuxPtrMaxDiffVlot = 60; // 辅助通道电压差保护条件

// ---- 日志/错误码 ----
export const LOG_AUTO_STOP = 0x0001;  // 测试完成 Finish
export const LOG_FORCE_STOP = 0x0002;  // 用户停止 Stop By User
export const LOG_FORCE_EXIT = 0x0003;  // 关闭程序 Program Exit
export const LOG_FORCE_JUMP = 0x0004;  // 跳转 Jump
export const LOG_SAFE_STOP = 0x0005;  // 安全停止 SafeGuard
export const LOG_CONTINUE = 0x0006;  // 接续 Resume
export const LOG_RESET_SETP = 0x0007;  // 重置工步 Change Step
export const LOG_START = 0x0008;  // 用户启动 Start
export const LOG_QUERY_UNIT = 0x0010;  // 搜索单元 Search Uint
export const LOG_COMM_ERR = 0x0020;  // 串口错误 Communication Error
export const LOG_POWEROFF = 0x10000000;  // 断电 Power Off
export const LOG_POWERON = 0x10000001;  // 上电 Power On

export const ERR_STEP_STORE = 0x80000000;  // 工步存储错误;
export const ERR_STEP_RETRIEVE = 0x80000001;  // 工步读取错误;
export const ERR_STEP_CHECKSUM = 0x80000002;  // 工步校验错误;
export const ERR_STEP_SEQUENCE = 0x80000003;  // 工步不连续;
export const ERR_STEP_NUMBER = 0x80000004;  // 工步号超出范围;
export const ERR_STACK_OVERFLOW = 0x80000100;  // 内部循环堆栈溢出;
export const ERR_CONFIG_RETRIVE = 0x80000200;  // 上电取配置信息失败;

// ---------------------------------------------------------------------------
// class PackDefine
// ---------------------------------------------------------------------------
export class PackDefine
{
    // QString PackDefine::ErrorText(int type)
    static ErrorText(type)
    {
        if (PackDefine.ErrorList.has(type))
            return PackDefine.ErrorList.get(type);
        else
            return "未知错误";
    }

    // int PackDefine::StepChargeType(int type)
    static StepChargeType(type)
    {
        if (type === STEP_TYPE_7 || type === STEP_TYPE_3 || type === STEP_TYPE_1)
            return STEP_CHARGE;
        if (type === STEP_TYPE_2)
            return STEP_DISCHARGE;

        return STEP_CHARGE_OTHER;
    }
}

// static QStringList  StepNameList;
// static QVector<QVector<int>> StepTypeMustHave;
// static QMap<int,QString>  ErrorList;
PackDefine.StepNameList = [];
PackDefine.StepTypeMustHave = [];
PackDefine.ErrorList = new Map();
