#ifndef JZ_RESOURCES_H_
#define JZ_RESOURCES_H_
#include <string>
#include <map>
#include <cstdint>
#include "JZStd.h"

struct JZFRAMEWORK_API JZResourceData
{
    const unsigned char *data;
    int size;
    int64_t mtime = 0;   // 源文件修改时间(ms, 构建时由 jz_resources.py 冻结, 运行期不变)
};

class JZFRAMEWORK_API JZResources
{
public:
    static JZResources *instance();    

    void add(const std::string &name, const unsigned char *data, int size, int64_t mtime = 0);
    const JZResourceData *find(const std::string &name) const;
    bool contains(const std::string &name) const;
    std::vector<std::string> resourcesList() const;

private:
    JZResources() = default;
    JZ_DISABLE_COPY(JZResources)

    std::map<std::string, JZResourceData> m_resources;
};

#endif // JZ_RESOURCES_H_
