#ifndef JS_ENGINE_CORE_MODULE_H_
#define JS_ENGINE_CORE_MODULE_H_
#include <string>
#include <vector>
#include <map>
#include <memory>

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
class JZFRAMEWORK_API JZJSCoreModule : public JZObject
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
    JSValue fileCopy(const std::string& from, const std::string& to, bool force);   // 失败抛 JS 异常
    JSValue fileRename(const std::string& from, const std::string& to);            // 失败抛 JS 异常
    JSValue dirCopy(const std::string& src, const std::string& dst, bool force);   // 递归复制目录, 失败抛 JS 异常
    Json::Value stat(const std::string& path);                 // 不存在返回 Json null
    Json::Value dirEntryList(const std::string& path);         // 不存在返回 Json null
    bool dirMkdir(const std::string& path, bool recursive);
    bool dirRmdir(const std::string& path);
    bool dirRemove(const std::string& path, bool recursive);
    // ---- 句柄(fd)级 IO(node 风格: 对应 fs.openSync/readSync/writeSync/closeSync/fstatSync) ----
    // fd 由本模块分配, 从 3 起(与 Node 保留 0/1/2 一致); 打开失败返回 -1。
    // 这一组是 JZFile 句柄接口(open/close/read/write/seek/tell/size/atEnd)的直接映射,
    // 底层 FILE* 的缓冲/游标语义原样暴露 —— 与 JZCore 同步实现一一对应。
    int fdOpen(const std::string& path, const std::string& mode);   // mode 为 C stdio 模式串("rb"/"wb"/"a+b"...)
    bool fdClose(int fd);
    JSValue fdRead(int fd, int maxSize);                           // Uint8Array(最多 maxSize 字节, 空 = EOF)
    JSValue fdWrite(int fd, JSValue data);                         // 返回写入字节数; 数据类型不合法/fd 无效抛异常
    bool fdSeek(int fd, int pos, int whence);                      // whence: 0=SET 1=CUR 2=END
    int fdTell(int fd);                                            // 无效 fd 返回 -1
    int fdSize(int fd);                                            // 无效 fd 返回 -1
    bool fdAtEnd(int fd);

    std::string fileName(const std::string& path);
    std::string dirName(const std::string& path);
    std::string extName(const std::string& path);
    bool isAbsolute(const std::string& path);
    std::string platform();     // 映射 windows -> win32
    std::string tmpdir();
    std::string cwd();          // 进程当前工作目录(与 Qt 的 QDir::currentPath() 同义)
    std::string workDir();      // 引擎运行根(setWorkDir 设的; 一个 app 可起多个引擎, 各有各的运行根)
    std::string appFile();      // 入口脚本(第一个加载的 js, 一般 main.js)绝对路径; appDir 由它推导
    std::string execPath();

    // ---- crypto / 随机 / UUID 原子 API(由内嵌 JS 适配层调用, 挂在 __jz.core 下) ----
    std::string uuid();                              // 返回 RFC4122 v4 uuid 字符串(无大括号)
    JSValue randomBytes(int size);                   // 返回 Uint8Array, size<0 抛异常
    JSValue randomInt(int min, int max);             // 返回 [min, max) 内整数, 越界抛 RangeError
    JSValue md5(JSValue data);                        // 接受 string/Uint8Array/ArrayBuffer, 返回 16 字节 Uint8Array

    static bool jsValueToBytes(JSContext* ctx, JSValue v, JZByteArray& out);

protected:
    JSValue makeUint8Array(const JZByteArray& data);
    JSValue throwError(const std::string& msg);

    JZFile* fileByFd(int fd);   // 无效 fd 返回 nullptr

    JZJSEngine* m_engine = nullptr;
    std::map<int, std::unique_ptr<JZFile>> m_files;   // fd -> 打开的文件(deinitEngine 时全部关闭)
    int m_nextFd = 3;
};

#endif  // !JS_ENGINE_CORE_MODULE_H_
