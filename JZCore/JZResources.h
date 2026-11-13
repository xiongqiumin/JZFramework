#ifndef JZ_RESOURCES_H_
#define JZ_RESOURCES_H_
#include <string>
#include <map>
#include <vector>
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

    // 登记资源。data 可能来自 jz_resources.py 的两种产物:
    //   1. 明文数组(默认) —— 直接登记指针, 零拷贝;
    //   2. 异或打包数组(--xor-key) —— 检测到打包头后在此解包, 登记到解出的明文缓冲上。
    // 两种形态对 find()/JZFile/JZFileInfo 完全透明(data 始终是明文)。
    void add(const std::string &name, const unsigned char *data, int size, int64_t mtime = 0);
    const JZResourceData *find(const std::string &name) const;
    bool contains(const std::string &name) const;
    std::vector<std::string> resourcesList() const;

private:
    JZResources() = default;
    JZ_DISABLE_COPY(JZResources)

    std::map<std::string, JZResourceData> m_resources;
    // 打包资源解出来的明文缓冲(进程级常驻)。缓冲区 move 进 map 后不再变动,
    // 所以 m_resources 里保存的 data 指针长期有效。
    std::map<std::string, std::vector<unsigned char>> m_owned;
};

#endif // JZ_RESOURCES_H_
