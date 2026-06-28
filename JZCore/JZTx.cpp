#include "JZTx.h"
#include "JZLog.h"
#include "JZPlatform.h"

JZTxScopedRangeLog::JZTxScopedRangeLog(const std::string &name)
{
    LOG_D(name + " Enter");
    m_tag = name;
    m_time = JZPlatform::getTickCountMs();
}

JZTxScopedRangeLog::~JZTxScopedRangeLog()
{
    int64_t end = JZPlatform::getTickCountMs();

    LOG_D(m_tag +  " Exit, cost = " + std::to_string(end - m_time));
}