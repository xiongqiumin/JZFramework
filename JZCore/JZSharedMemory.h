#ifndef JZ_SHARED_MEMORY_H_
#define JZ_SHARED_MEMORY_H_

#include <string>

#include "JZGlobal.h"

// ---------------------------------------------------------------------------
// JZSharedMemory —— 跨进程共享内存(接口对齐 QSharedMemory)
//
//   // 进程 A(创建方)
//   JZSharedMemory shm("jz_shell_req");
//   if (!shm.create(64 * 1024)) { ... shm.errorString() ... }
//   memcpy(shm.data(), buf, n);
//
//   // 进程 B(使用方)
//   JZSharedMemory shm2("jz_shell_req");
//   if (!shm2.attach()) { ... }
//   const char *p = (const char *)shm2.constData();
//
// 生命周期:
//   · create/attach 成功之后才有映射(data()/size() 有效);
//   · detach() **只解除本进程的映射**, 不删除共享段;
//   · remove(key) 才是删除 —— 之后所有 attach 的进程访问该段都会失效
//     (Windows 上由"最后一个句柄关闭"实现, 所以 remove 只是提前释放名字)。
//
// key 是**逻辑名**(不要带路径分隔符), 平台前缀由本类自己加:
//   Windows -> "Local\\JZSharedMemory_<key>"(Local 命名空间: 不需要管理员权限, 不跨会话)
//   POSIX   -> "/jz_<key>"                  (shm_open 的命名规则)
//
// ⚠ 这不是"多进程同步": 多个进程同时读写同一块内存需要调用方自己加锁
//   (跨进程互斥用命名信号量, 还没做; 目前建议一个方向一个段 + 回合制)。
// ---------------------------------------------------------------------------
class JZFRAMEWORK_API JZSharedMemory
{
public:
    enum AccessMode
    {
        ReadOnly = 0,   // 只读映射
        ReadWrite = 1,  // 读写映射(创建时必须是这个)
    };

    explicit JZSharedMemory(const std::string &key);
    ~JZSharedMemory();

    // 不可拷贝: 底层是一个句柄/映射, 拷贝会导致重复解映射
    JZSharedMemory(const JZSharedMemory &) = delete;
    JZSharedMemory &operator=(const JZSharedMemory &) = delete;

    // 创建并映射。已存在同名段时返回 false(用 attach 打开已有的那个)。
    bool create(int size, AccessMode mode = ReadWrite);
    // 打开已存在的段并映射(段不存在返回 false)。
    bool attach(AccessMode mode = ReadWrite);
    // 解除映射。可在析构前主动调用; 重复调用安全。
    bool detach();

    bool isAttached() const { return m_attached; }
    void *data() { return m_data; }
    const void *constData() const { return m_data; }
    // 段大小(attach 时从系统查回来, 不是发起方传的那个数)
    int size() const { return m_size; }
    std::string key() const { return m_key; }
    // 最近一次失败的**可读原因**(成功时为空)
    std::string errorString() const { return m_error; }

    // 删除共享段(所有 attach 者随后都拿不到数据)。一般由创建者在最后调用。
    static bool remove(const std::string &key);

private:
    // 逻辑名 -> 平台名(静态: remove() 是静态入口, 也要用)
    static std::string platformKey(const std::string &key);

    std::string m_key;      // 逻辑名(不含平台前缀)
    std::string m_error;
    void *m_data = nullptr;
    int m_size = 0;
    bool m_attached = false;
#ifdef _WIN32
    void *m_map = nullptr;  // HANDLE(CreateFileMapping / OpenFileMapping 的结果)
#else
    int m_fd = -1;          // shm_open 得到的 fd(映射后就可以关, 这里保留便于错误处理)
#endif
};

#endif // !JZ_SHARED_MEMORY_H_
