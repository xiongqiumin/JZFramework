#include "JZResources.h"
#include "JZStd.h"
#include "JZDebug.h"
#include <string.h>
#include <utility>
#include <vector>

// ==================== 打包资源格式 (tools/jz_resources.py --xor-key) ====================
// 头部 20 字节:
//   0..3   magic 'J''Z''R''E'
//   4      version = 1
//   5      algo    = 1 (xorshift32 流异或)
//   6..7   reserved
//   8..11  plainSize (uint32 LE, 明文长度)
//   12..15 seed      (uint32 LE, 该文件的异或密钥流初始状态)
//   16..19 check     (uint32 LE, 明文 FNV-1a, 用于识别 key 错误/数据损坏)
// 之后是等长的密文。algo/seed 都由生成器写死, 运行期不需要额外密钥。
static const int kPackedHeaderSize = 20;
static const unsigned char kPackedMagic[4] = { 'J', 'Z', 'R', 'E' };
static const unsigned char kPackedVersion = 1;
static const unsigned char kPackedAlgoXor = 1;

static inline uint32_t jzResReadU32LE(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t jzResFnv1a32(const unsigned char *data, int size)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < size; ++i)
    {
        h ^= (uint32_t)data[i];
        h *= 16777619u;
    }
    return h;
}

static inline bool jzResIsPacked(const unsigned char *data, int size)
{
    return data != nullptr && size >= kPackedHeaderSize &&
           memcmp(data, kPackedMagic, 4) == 0 &&
           data[4] == kPackedVersion && data[5] == kPackedAlgoXor;
}

// 与 tools/jz_resources.py::xor_stream / JZXorShift::stream 完全同一算法:
// 每字节异或 state 低 8 位, 然后 xorshift32 迭代一次。加密/解密是同一操作。
static void jzResXorStream(unsigned char *data, int size, uint32_t state)
{
    for (int i = 0; i < size; ++i)
    {
        data[i] ^= (unsigned char)(state & 0xFFu);
        uint32_t x = state;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state = x;
    }
}

// 从资源名 ":/xxx/yyy" 中解析出资源组名 xxx (对应注册函数 jzres_xxx_init())
static std::string jzResourceGroup(const std::string &name)
{
    size_t start = 0;
    if (start < name.size() && name[start] == ':') start++;
    if (start < name.size() && name[start] == '/') start++;
    size_t end = name.find('/', start);
    if (end == std::string::npos) end = name.size();
    return name.substr(start, end - start);
}

JZResources *JZResources::instance()
{    
    static JZResources inst;
    return &inst;
}

void JZResources::add(const std::string &name, const unsigned char *data, int size, int64_t mtime)
{
    if (jzResIsPacked(data, size))
    {
        uint32_t plainSize = jzResReadU32LE(data + 8);
        uint32_t seed = jzResReadU32LE(data + 12);
        uint32_t check = jzResReadU32LE(data + 16);

        if ((int64_t)plainSize != (int64_t)size - kPackedHeaderSize)
        {
            jzWarning() << "JZResources::add: packed resource" << name
                        << "size mismatch, skip";
            return;
        }

        // plainSize 为 0 时也留 1 字节, 保证 data 非空(上层有 !res->data 的判空)
        std::vector<unsigned char> plain(plainSize > 0 ? plainSize : 1);
        const unsigned char *cipher = data + kPackedHeaderSize;
        for (uint32_t i = 0; i < plainSize; ++i)
            plain[i] = cipher[i];
        jzResXorStream(plain.data(), (int)plainSize, seed);

        if (jzResFnv1a32(plain.data(), (int)plainSize) != check)
        {
            jzWarning() << "JZResources::add: packed resource" << name
                        << "unpack failed (corrupted data), skip";
            return;
        }

        m_owned[name] = std::move(plain);

        JZResourceData res_data;
        res_data.data = m_owned[name].data();
        res_data.size = (int)plainSize;
        res_data.mtime = mtime;
        m_resources[name] = res_data;
        return;
    }

    JZResourceData res_data;
    res_data.data = data;
    res_data.size = size;
    res_data.mtime = mtime;
    m_resources[name] = res_data;
}

const JZResourceData *JZResources::find(const std::string &name) const
{
    auto it = m_resources.find(name);
    if(it == m_resources.end())
    {
        // 资源名由 jz_resources.py 生成, 格式固定为 ":/xxx/yyy" (以 ':' 开头),
        // 其中 xxx 为资源组名, 对应注册函数 jzres_xxx_init()。
        std::string group = jzResourceGroup(name);
        if(group.empty())
        {
            jzWarning() << "JZResources::find: invalid res" << name;
        }
        else
        {
            std::string init_func = "jzres_" + group + "_init()";
            jzWarning() << "JZResources::find: resource \"" << name << "\" faild, "
                        << "please check is" << init_func.c_str() << "invoke";
        }

        return nullptr;
    } 
    return &it->second;
}

bool JZResources::contains(const std::string &name) const
{
    return m_resources.find(name) != m_resources.end();
}

std::vector<std::string> JZResources::resourcesList() const
{
    return map_keys(m_resources);
}