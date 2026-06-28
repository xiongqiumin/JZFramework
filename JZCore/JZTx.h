#ifndef JZ_TX_H_
#define JZ_TX_H_

#include <inttypes.h>
#include <string>

class JZTxScopedRangeLog
{
public:
    JZTxScopedRangeLog(const std::string &name);
    ~JZTxScopedRangeLog();

protected:
    std::string m_tag;
    int64_t m_time;
};

#define JZTX_LOG(name) JZTxScopedRangeLog __func_scoped_range##__LINE__(name);
#define JZTX_FUNC_LOG  JZTX_LOG(__FUNCTION__)

#endif // !JZ_TX_H_
