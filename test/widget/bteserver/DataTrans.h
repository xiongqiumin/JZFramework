#ifndef BTE_DATA_TRANS_H_
#define BTE_DATA_TRANS_H_

// ---------------------------------------------------------------------------
// DataTrans.h —— 对应原版 bteserver/common/DataTrans.h: 协议里那些"扩了倍数的整数"
// 与工程值之间的换算(换算系数见原版文件头那段注释)。
//
//   * 电压/电流: 0.1mV / 10000  -> V / A
//   * 温度:      0.1℃  / 10     -> ℃
//   * 容量/能量: /3600/10000    -> Ah / Wh
//
// 只搬了"上位机显示要用"的 from*; to*(下发工步时把工程值变回协议值)等工步下发一起搬。
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string>

namespace DataTrans {

double fromVoltageUnit(int32_t var);    // 0.1mV -> V
double fromElectricUnit(int32_t var);   // -> A
double fromTempUnit(int16_t var);       // 0.1℃ -> ℃
double fromCapUnit(int64_t var);        // -> Ah
double fromEngUnit(int64_t var);        // -> Wh

// 原版 fromTimeUnitSec(uint32_t var): ms -> "HH:MM:SS"(小时可以超过 24, 不拆成天数)
std::string fromTimeUnitSec(int64_t var);

} // namespace DataTrans

#endif // BTE_DATA_TRANS_H_
