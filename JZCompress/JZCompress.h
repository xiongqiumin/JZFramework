#ifndef JZ_COMPRESS_H_
#define JZ_COMPRESS_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "JZCore/JZByteArray.h"
#include "JZCore/JZResult.h"

/*
 * JZCompress: 压缩 / 解压工具集（接口先行，具体算法后续实现）
 *
 * 设计目标：
 *   1. 流式压缩 / 解压：适合网络分块传输，边收边压 / 边收边解。
 *   2. 按块（一次性）压缩 / 解压：整段内存输入 -> 整段输出。
 *   3. 标准 zip 归档：压缩整个文件夹 -> .zip，并支持读取 / 按需解压归档内条目。
 *
 * 仅采用 HTTP 内容编码（Content-Encoding）支持的算法：
 *   - Deflate / Zlib / GZip / Brotli：HTTP 原生支持（gzip/deflate/br）。
 *   - Zstd：IANA 已注册为 "zstd" 内容编码，支持面较新，作为可选。
 *
 * 关于 ZIP：ZIP 是归档容器（非 HTTP 内容编码），其条目内部使用的正是
 * Deflate 内核。因此 zip 功能复用上面的 Deflate 压缩器，二者共享同一内核，
 * 但帧封装不同（HTTP 直接传 deflate/gzip 流；ZIP 把 deflate 块包进归档结构）。
 *
 * 当前本文件仅定义接口，所有方法返回 JZResult::Error("not implemented")，
 * 待选定底层实现（如 zlib / miniz 移植）后填充。
 */

// ==================== 算法枚举（均为 HTTP 内容编码支持） ====================
enum class JZCompressAlgorithm
{
    Deflate,    // raw deflate 流（HTTP "deflate" 用 zlib 封装的 raw deflate）
    Zlib,       // zlib 封装 (rfc1950)，裸流易嵌入自定义协议
    GZip,       // gzip 封装 (rfc1952)，带文件头 / CRC，HTTP 最常用
    Brotli,     // Google Brotli，HTTP "br" 内容编码
    Zstd,       // Zstandard，HTTP "zstd" 内容编码（支持面较新，可选）
};

// 算法 -> 可读名称（用于日志 / 错误描述）
const char *jzCompressAlgorithmName(JZCompressAlgorithm algo);

// ==================== 一次性（按块）压缩 / 解压 ====================
// 整段输入 -> 整段输出。level 含义与具体算法相关：[-1=默认, 0=不压缩, 1..9=等级]。
JZResult<JZByteArray> jzCompressBlock(const JZByteArray &in, JZCompressAlgorithm algo, int level = -1);
JZResult<JZByteArray> jzDecompressBlock(const JZByteArray &in, JZCompressAlgorithm algo);

// 说明：对内存边界敏感的流式场景（如 raw deflate 的窗口/帧头处理）统一使用
// JZCompressStream / JZDecompressStream（见下），不再提供带 windowBits 的重载。

// ==================== 流式压缩 / 解压 ====================
/*
 * 适用场景：从网络 / 文件按任意大小分块到达，调用方每收到一块就 write 一次，
 * 返回已生成的压缩 / 解压输出（可能为空，也可能一次吐出多字节）。
 *
 * 关于「输出节奏」与「结束」的契约：
 *   - write() 贪婪输出：内部以 Z_SYNC_FLUSH 在合理间隔自动把当前可产出的压缩
 *     字节全部推出并返回。调用方**无需在每次 write() 之间手动 flush**；deflate
 *     不会为追求更高压缩比而无限缓冲、卡住数据。空返回仅表示此刻无新输出。
 *   - 仅「末尾」需要一个终结调用：压缩端因流式帧无隐式边界、且帧尾
 *     CRC/ISIZE 必须在确认无更多输入后才能生成，故由调用方在喂完所有块后显式
 *     调用 JZCompressStream::finish() 封帧尾（同 zlib 的 Z_FINISH）。这是用户层
 *     约定，不是周期性动作。
 *   - 解压端则相反：deflate 最终块位 / gzip 帧尾 / zstd 帧结束标记本身是数据自带的
 *     「结束信号」，write() 内部可自动判定流结束；但调用方仍须调用
 *     JZDecompressStream::finish() 触发尾部校验（CRC/Adler），损坏数据才会在
 *     finish() 以 JZResult 错误暴露，而非静默通过。
 *
 * 注意：write() 之间数据是连续的，不要跨对象 / 跨算法复用缓冲。
 */
class JZCompressStream
{
public:
    JZCompressStream(JZCompressAlgorithm algo, int level = -1);
    ~JZCompressStream();

    // 设置内部自动 flush 间隔（输入字节数）：攒够这么多输入才强制 Z_SYNC_FLUSH
    // 推出已产出字节。0=每次 write 都立即 flush（延迟最低，适合流式 LLM）。
    // 默认 0。调大可提升压缩比但增加首字节延迟。必须在首个 write() 前设置。
    void setBlockSize(int blockSize);

    // 喂入一段原始数据，内部自动（间隔）flush 并贪婪返回已产出的压缩字节
    // （可能为空）。finish() 之后再次调用 write() 视为错误（需先 reset() 复用）。
    JZByteArray write(const JZByteArray &input);

    // 终结调用：调用方确认再无输入后调用，返回帧尾字节
    // （gzip CRC+ISIZE / zstd 帧尾等）并标记流结束。仅在末尾调用一次。
    JZByteArray finish();

    // 重置内部状态，可复用对象开始新一轮压缩。
    void reset();

    JZCompressAlgorithm algorithm() const;

private:
    JZ_DISABLE_COPY(JZCompressStream)
    struct Impl;
    Impl *m_impl;
};

class JZDecompressStream
{
public:
    // expectSizeHint: 可选的原始大小提示（部分格式需要，如 raw deflate）。0=未知。
    JZDecompressStream(JZCompressAlgorithm algo, int64_t expectSizeHint = 0);
    ~JZDecompressStream();

    // 喂入一段压缩数据，返回解压出的原始字节（可能为空）。
    // 流结束由数据自带的结束标记判定，无需调用方额外通知。
    JZByteArray write(const JZByteArray &input);

    // 收尾：校验帧尾（CRC/Adler 等）并返回尾部剩余字节（正常应为空）。
    // 必须在全部 write() 之后调用；若校验失败应返回 JZResult 错误。
    JZByteArray finish();

    void reset();

    JZCompressAlgorithm algorithm() const;

private:
    JZ_DISABLE_COPY(JZDecompressStream)
    struct Impl;
    Impl *m_impl;
};

// ==================== 标准 zip 归档 ====================
struct JZZipEntry
{
    std::string name;          // 归档内路径（使用 '/'，相对根）
    int64_t size = 0;          // 原始大小（字节）
    int64_t compressedSize = 0;// 压缩后大小（字节）
    bool isDir = false;        // 是否为目录项
    uint32_t crc32 = 0;        // CRC32 校验
    int64_t mtime = 0;         // 修改时间（epoch 秒），0=未知
    int method = 0;            // 压缩方法（0=store, 8=deflate）
};

// 进度回调：(已处理字节, 总字节) -> 返回 false 可取消当前操作。
using JZZipProgress = std::function<bool(int64_t current, int64_t total)>;

/*
 * JZZip: 标准 zip 归档 / 解档（接口先行，具体实现后续接入 miniz 等）。
 *
 * 特性：
 *   - 压缩支持「虚拟条目」：内存数据 (JZByteArray) 或磁盘文件均可 addFile 进归档，
 *     无需先把内存数据落盘。
 *   - zip 全程通过进度回调上报 (current, total)，便于 UI 显示进度 / 取消。
 *
 * 用法（虚拟压缩）：
 *   JZZip z;
 *   z.addFile("config.json", memData);     // 内存数据
 *   z.addFile("bin/app.bin", "D:/x/app.bin");// 磁盘文件
 *   z.addFolder("assets");                 // 整目录
 *   z.zip("out.zip", 6);
 */
class JZZip
{
public:
    JZZip();
    ~JZZip();

    // 进度回调（压缩 / 解压均会周期调用）。返回 false 取消当前操作。
    void setProgressCallback(JZZipProgress cb);

    // ---------------- 压缩（支持虚拟条目） ----------------
    // 添加内存数据作为 zip 内路径 zipEntryPath 的条目（虚拟压缩，无需磁盘文件）。
    void addFile(const std::string &zipEntryPath, const JZByteArray &data);
    // 添加磁盘文件 orgPath 作为 zip 内路径 zipEntryPath 的条目。
    void addFile(const std::string &zipEntryPath, const std::string &orgPath);
    // 添加空目录项。
    void addDirectory(const std::string &zipEntryPath);
    // 添加整个文件夹（递归），zip 内路径以 folder 为根生成相对路径。
    void addFolder(const std::string &folder,
                   bool recursive = true,
                   const std::vector<std::string> &filter = {});

    // 已登记的待压缩条目个数。
    int entryCount() const;

    // 执行打包到 zipPath。level: -1=默认, 0=store, 1..9=deflate 等级。
    // 内部按全部条目原始总大小推进进度回调。
    JZResult<void> zip(const std::string &zipPath, int level = -1);

    // ---------------- 归档读取 ----------------
    // 枚举 zip 内容（不解压）。
    JZResult<std::vector<JZZipEntry>> list(const std::string &zipPath);

private:
    JZ_DISABLE_COPY(JZZip)
    struct Impl;
    Impl *m_impl;
};

/*
 * JZUnZip: 只读归档读取器（基于 miniz mz_zip 实现）。
 *
 * JZUnZip 用于「打开归档、枚举内容、按需抽取单个条目」——典型场景：预览压缩包、
 * 只取其中一个文件到内存（如配置文件 / 资源），无需解压整个包。
 *
 * 用法：
 *   JZUnZip u;
 *   u.open("data.zip");
 *   auto entries = u.list();
 *   auto buf = u.extractToMemory("config.json");   // 单文件进内存
 *   u.extractToFile("bin/app.bin", "D:/out/app.bin");// 单文件落盘
 *   u.unzip("D:/out");                               // 整包解压到目录
 */
class JZUnZip
{
public:
    JZUnZip();
    ~JZUnZip();

    // 进度回调（单文件抽取时按该文件解压进度上报）。返回 false 取消。
    void setProgressCallback(JZZipProgress cb);

    // 打开 zip（只读，不解压内容）。
    JZResult<void> open(const std::string &zipPath);
    void close();
    bool isOpen() const;

    // 枚举全部条目（不解压）。需先 open。
    JZResult<std::vector<JZZipEntry>> list();

    // 条目是否存在（按归档内路径匹配）。
    bool contains(const std::string &entryName) const;

    // 按需解压单个条目到内存。entryName 为归档内路径。
    JZResult<JZByteArray> extractToMemory(const std::string &entryName);

    // 按需解压单个条目到磁盘文件（destPath 为目标文件完整路径，非目录）。
    JZResult<void> extractToFile(const std::string &entryName, const std::string &destPath);

    // 解压整个归档到目录 dir（dir 不存在自动创建）。
    // 目录项建对应空目录；文件项按归档内相对路径落盘到 dir 之下。
    // 内部按全部文件原始总大小推进进度回调（current, total），回调返回 false 即取消。
    JZResult<void> unzip(const std::string &dir);

private:
    JZ_DISABLE_COPY(JZUnZip)
    struct Impl;
    Impl *m_impl;
};

#endif // !JZ_COMPRESS_H_
