#ifndef JS_ENGINE_CORE_MODULE_H_
#define JS_ENGINE_CORE_MODULE_H_

#include <string>
#include <vector>

#include "JZJSEngine.h"
#include "JZByteArray.h"
#include "JZFile.h"
#include "JZDir.h"
#include "JZFileInfo.h"
#include "JZPlatform.h"
#include "json/json.h"

// JS core 模块: 把 JZCore 已有的 file/dir/os 能力暴露成 Node.js 风格 API
// (fs / path / os / process)。全部为 Node 里的同步接口, 直接映射到 JZCore 的同步实现,
// 不做任何异步包装(异步 Promise 版本后续再加)。
class JZJSCoreModule : public JZObject
{
public:
    JZJSCoreModule();
    ~JZJSCoreModule();

    void initEngine(JZJSEngine* engine);
    void deinitEngine();

    // ---- 原子 API(由内嵌 JS 适配层调用, 挂在 __jz.core 下) ----
    bool fileExists(const std::string& path);
    bool fileRemove(const std::string& path);
    JSValue readFile(const std::string& path);                 // 返回 Uint8Array, 失败抛 JS 异常
    JSValue writeFile(const std::string& path, JSValue data, bool append); // 失败抛 JS 异常
    Json::Value stat(const std::string& path);                 // 不存在返回 Json null
    Json::Value dirEntryList(const std::string& path);         // 不存在返回 Json null
    bool dirMkdir(const std::string& path, bool recursive);
    bool dirRmdir(const std::string& path);
    bool dirRemove(const std::string& path, bool recursive);
    std::string fileName(const std::string& path);
    std::string dirName(const std::string& path);
    std::string extName(const std::string& path);
    bool isAbsolute(const std::string& path);
    std::string platform();     // 映射 windows -> win32
    std::string tmpdir();
    std::string cwd();
    std::string execPath();

    static bool jsValueToBytes(JSContext* ctx, JSValue v, JZByteArray& out);

protected:
    JSValue makeUint8Array(const JZByteArray& data);
    JSValue throwError(const std::string& msg);

    JZJSEngine* m_engine = nullptr;
};

#endif  // !JS_ENGINE_CORE_MODULE_H_
