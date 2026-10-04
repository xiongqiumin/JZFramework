#include "DataTrans.h"

#include <cstdio>

namespace DataTrans {

double fromVoltageUnit(int32_t var)
{
    return var / 10000.0;
}

double fromElectricUnit(int32_t var)
{
    return var / 10000.0;
}

double fromTempUnit(int16_t var)
{
    // 注意: 必须是 10.0 —— 整数除法会丢掉 0.1℃ 精度(如 55.5 会变成 55.0)
    return var / 10.0;
}

double fromCapUnit(int64_t var)
{
    return var / 3600.0 / 10000.0;
}

double fromEngUnit(int64_t var)
{
    return var / 3600.0 / 10000.0;
}

std::string fromTimeUnitSec(int64_t var)
{
    if (var < 0)
        var = 0;
    const int64_t totalSec = var / 1000;
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
             (int)(totalSec / 3600), (int)((totalSec / 60) % 60), (int)(totalSec % 60));
    return buf;
}

} // namespace DataTrans
